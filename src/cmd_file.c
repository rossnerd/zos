#define _POSIX_C_SOURCE 200809L
/**
 * @file cmd_file.c
 * @brief Příkazy pracující se soubory ve виртуálním FS (INCP/OUTCP/CAT/RM/CP/MV).
 *
 * Refaktoring (konzervativní):
 *  - zachované signatury funkcí (API) i texty hlášek
 *  - menší zanoření, dřívější návraty při chybách
 *  - společné pomocné funkce pro opakované úkony (otevření FS, načtení SB, práce s cestou)
 *  - doplněné dokumentační komentáře a základní kontroly I/O
 *
 * Poznámka: V případě chyb alokace (NO SPACE apod.) provádí funkce best-effort úklid
 * (rollback bitmap), aby se minimalizovalo „rozbití“ obrazu FS. Hlášky zůstávají stejné.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../include/fs_core.h"
#include "../include/fs_utils.h"

/* ========================================================================== */
/* Interní helpery                                                            */
/* ========================================================================== */

static FILE *open_fs_rw(const char *filename)
{
    return filename ? fopen(filename, "rb+") : NULL;
}

static FILE *open_fs_ro(const char *filename)
{
    return filename ? fopen(filename, "rb") : NULL;
}

static bool load_sb_or_fail(FILE *f, struct superblock *sb)
{
    return f && sb && load_superblock(f, sb);
}


/**
 * @brief Uvolní alokované datové clustery (best-effort).
 */
static void rollback_data_clusters(FILE *f, struct superblock *sb, const int32_t blocks[5])
{
    if (!f || !sb || !blocks) {
        return;
    }

    for (int i = 0; i < 5; i++) {
        if (blocks[i] != CLUSTER_UNUSED) {
            set_bit(f, sb, false, blocks[i], false);
        }
    }
}

/* ========================================================================== */
/* INCP / OUTCP                                                               */
/* ========================================================================== */

/**
 * @brief Importuje soubor z host OS do VFS.
 *
 * @param filename  Cesta k souboru s obrazem VFS.
 * @param host_path Cesta k souboru na hostiteli.
 * @param vfs_path  Cílová cesta ve VFS.
 * @return 1 při úspěchu, 0 při chybě.
 */
int fs_incp(const char *filename, const char *host_path, const char *vfs_path)
{
    FILE *host_f = fopen(host_path, "rb");
    if (!host_f) {
        printf("FILE NOT FOUND (host)\n");
        return 0;
    }

    /* zjištění velikosti */
    if (fseek(host_f, 0, SEEK_END) != 0) {
        fclose(host_f);
        return 0;
    }
    const long file_size = ftell(host_f);
    if (file_size < 0) {
        fclose(host_f);
        return 0;
    }
    (void)fseek(host_f, 0, SEEK_SET);

    FILE *f = open_fs_rw(filename);
    if (!f) {
        fclose(host_f);
        return 0;
    }

    struct superblock sb;
    if (!load_sb_or_fail(f, &sb)) {
        fclose(host_f);
        fclose(f);
        return 0;
    }

    /* omezení: max 5 přímých clusterů */
    if (file_size > (long)5 * (long)sb.cluster_size) {
        printf("TOO BIG\n");
        fclose(host_f);
        fclose(f);
        return 0;
    }

    /* rozpad cesty na rodiče a jméno položky */
    char parent_path[256];
    char new_name[128];
    parse_path(vfs_path, parent_path, new_name);

    const int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) {
        printf("PATH NOT FOUND\n");
        fclose(host_f);
        fclose(f);
        return 0;
    }

    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) {
        printf("EXIST\n");
        fclose(host_f);
        fclose(f);
        return 0;
    }

    const int free_inode = find_free_bit(f, &sb, true);
    if (free_inode == -1) {
        printf("NO SPACE\n");
        fclose(host_f);
        fclose(f);
        return 0;
    }
    set_bit(f, &sb, true, free_inode, true);

    int32_t blocks[5] = { CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED };

    /* zapisujeme clustery po sb.cluster_size */
    uint8_t *cluster_buf = (uint8_t *)malloc((size_t)sb.cluster_size);
    if (!cluster_buf) {
        /* rollback inode bitmap */
        set_bit(f, &sb, true, free_inode, false);
        fclose(host_f);
        fclose(f);
        return 0;
    }

    long bytes_remaining = file_size;
    int b_idx = 0;

    while (bytes_remaining > 0 && b_idx < 5) {
        const int free_block = find_free_bit(f, &sb, false);
        if (free_block == -1) {
            printf("NO SPACE\n");
            rollback_data_clusters(f, &sb, blocks);
            set_bit(f, &sb, true, free_inode, false);
            free(cluster_buf);
            fclose(host_f);
            fclose(f);
            return 0;
        }

        set_bit(f, &sb, false, free_block, true);
        blocks[b_idx++] = free_block;

        memset(cluster_buf, 0, (size_t)sb.cluster_size);

        const size_t to_read = (bytes_remaining > sb.cluster_size)
                             ? (size_t)sb.cluster_size
                             : (size_t)bytes_remaining;

        (void)fread(cluster_buf, 1, to_read, host_f);

        /* přímý zápis do clusteru (stejně jako původní implementace) */
        (void)fseek(f, sb.data_start_address + (long)free_block * (long)sb.cluster_size, SEEK_SET);
        (void)fwrite(cluster_buf, 1, (size_t)sb.cluster_size, f);

        bytes_remaining -= (long)to_read;
    }

    free(cluster_buf);

    struct pseudo_inode new_inode = (struct pseudo_inode){0};
    new_inode.nodeid = free_inode;
    new_inode.file_size = (int)file_size;
    new_inode.references = 1;
    new_inode.isDirectory = false;
    new_inode.direct1 = blocks[0];
    new_inode.direct2 = blocks[1];
    new_inode.direct3 = blocks[2];
    new_inode.direct4 = blocks[3];
    new_inode.direct5 = blocks[4];
    new_inode.indirect1 = CLUSTER_UNUSED;
    new_inode.indirect2 = CLUSTER_UNUSED;

    write_inode(f, &sb, free_inode, &new_inode);

    struct directory_item new_entry = {0};
    new_entry.inode = free_inode;
    strcpy(new_entry.item_name, new_name);
    (void)add_directory_item(f, &sb, parent_id, &new_entry);

    fclose(host_f);
    fclose(f);
    return 1;
}

/**
 * @brief Exportuje soubor z VFS do host OS.
 *
 * Pozn.: Funkce není v hlavičce zadání, ale je součástí projektu (příkaz OUTCP).
 */
int fs_outcp(const char *filename, const char *vfs_path, const char *host_path)
{
    FILE *f = open_fs_ro(filename);
    if (!f) {
        printf("FILE NOT FOUND\n");
        return 0;
    }

    struct superblock sb;
    if (!load_sb_or_fail(f, &sb)) {
        fclose(f);
        printf("FILE NOT FOUND\n");
        return 0;
    }

    const int inode_id = fs_path_to_inode(filename, vfs_path);
    if (inode_id == -1) {
        printf("FILE NOT FOUND\n");
        fclose(f);
        return 0;
    }

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);
    if (inode.isDirectory) {
        printf("FILE NOT FOUND\n");
        fclose(f);
        return 0;
    }

    uint8_t *buffer = (uint8_t *)malloc((size_t)inode.file_size);
    if (!buffer) {
        fclose(f);
        return 0;
    }

    (void)load_file_content(f, &sb, inode_id, buffer);
    fclose(f);

    FILE *out = fopen(host_path, "wb");
    if (!out) {
        printf("CANNOT CREATE FILE\n");
        free(buffer);
        return 0;
    }

    (void)fwrite(buffer, 1, (size_t)inode.file_size, out);
    fclose(out);
    free(buffer);
    return 1;
}

/* ========================================================================== */
/* CAT / RM / CP / MV                                                         */
/* ========================================================================== */

int fs_cat(const char *filename, const char *path)
{
    FILE *f = open_fs_ro(filename);
    if (!f) {
        return 0;
    }

    struct superblock sb;
    (void)load_superblock(f, &sb);

    const int inode_id = fs_path_to_inode(filename, path);
    if (inode_id == -1) {
        printf("FILE NOT FOUND\n");
        fclose(f);
        return 0;
    }

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);

    if (inode.isDirectory) {
        printf("FILE NOT FOUND (It is a directory)\n");
        fclose(f);
        return 0;
    }

    /* Načteme obsah (+1 pro nulový znak kvůli printf). */
    uint8_t *buffer = (uint8_t *)malloc((size_t)inode.file_size + 1);
    if (!buffer) {
        fclose(f);
        return 0;
    }

    (void)load_file_content(f, &sb, inode_id, buffer);
    buffer[inode.file_size] = '\0';

    printf("%s\n", (char *)buffer);

    free(buffer);
    fclose(f);
    return 1;
}

int fs_rm(const char *filename, const char *path)
{
    FILE *f = open_fs_rw(filename);
    if (!f) {
        return 0;
    }

    struct superblock sb;
    (void)load_superblock(f, &sb);

    /* Musíme najít rodiče a jméno, abychom mohli smazat odkaz */
    char parent_path[256];
    char name[128];
    parse_path(path, parent_path, name);

    const int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) {
        printf("FILE NOT FOUND (Parent not found)\n");
        fclose(f);
        return 0;
    }

    const int inode_id = find_inode_in_dir(f, &sb, parent_id, name);
    if (inode_id == -1) {
        printf("FILE NOT FOUND\n");
        fclose(f);
        return 0;
    }

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);

    if (inode.isDirectory) {
        /* rm nesmí mazat adresáře (jen rmdir) */
        printf("FILE NOT FOUND (It is a directory)\n");
        fclose(f);
        return 0;
    }

    (void)remove_directory_item(f, &sb, parent_id, name);
    free_inode_resources(f, &sb, inode_id);

    fclose(f);
    return 1;
}

int fs_cp(const char *filename, const char *s1, const char *s2)
{
    FILE *f = open_fs_rw(filename);
    if (!f) {
        return 0;
    }

    struct superblock sb;
    (void)load_superblock(f, &sb);

    /* 1) zdroj */
    const int src_id = fs_path_to_inode(filename, s1);
    if (src_id == -1) {
        printf("FILE NOT FOUND\n");
        fclose(f);
        return 0;
    }

    struct pseudo_inode src_inode;
    read_inode(f, &sb, src_id, &src_inode);
    if (src_inode.isDirectory) {
        printf("FILE NOT FOUND (Source is dir)\n");
        fclose(f);
        return 0;
    }

    /* 2) cíl */
    char parent_path[256];
    char name[128];
    parse_path(s2, parent_path, name);

    const int dest_parent_id = fs_path_to_inode(filename, parent_path);
    if (dest_parent_id == -1) {
        printf("PATH NOT FOUND\n");
        fclose(f);
        return 0;
    }

    if (find_inode_in_dir(f, &sb, dest_parent_id, name) != -1) {
        printf("EXIST\n");
        fclose(f);
        return 0;
    }

    /* 3) načíst data zdroje */
    uint8_t *buffer = (uint8_t *)malloc((size_t)src_inode.file_size);
    if (!buffer) {
        fclose(f);
        return 0;
    }
    (void)load_file_content(f, &sb, src_id, buffer);

    /* 4) nový inode */
    const int free_inode = find_free_bit(f, &sb, true);
    if (free_inode == -1) {
        printf("NO SPACE\n");
        free(buffer);
        fclose(f);
        return 0;
    }
    set_bit(f, &sb, true, free_inode, true);

    /* 5) zápis obsahu do nového inodu */
    if (!write_buffer_to_new_inode(f, &sb, free_inode, buffer, src_inode.file_size)) {
        printf("NO SPACE\n");
        set_bit(f, &sb, true, free_inode, false); /* rollback inode bitmap */
        free(buffer);
        fclose(f);
        return 0;
    }

    /* 6) vložení do adresáře */
    struct directory_item item = {0};
    item.inode = free_inode;
    strcpy(item.item_name, name);
    (void)add_directory_item(f, &sb, dest_parent_id, &item);

    free(buffer);
    fclose(f);
    return 1;
}

int fs_mv(const char *filename, const char *s1, const char *s2)
{
    FILE *f = open_fs_rw(filename);
    if (!f) {
        return 0;
    }

    struct superblock sb;
    (void)load_superblock(f, &sb);

    /* Zdroj */
    char src_parent_path[256];
    char src_name[128];
    parse_path(s1, src_parent_path, src_name);

    const int src_parent_id = fs_path_to_inode(filename, src_parent_path);
    const int src_inode_id = (src_parent_id == -1)
                           ? -1
                           : find_inode_in_dir(f, &sb, src_parent_id, src_name);

    if (src_inode_id == -1) {
        printf("FILE NOT FOUND\n");
        fclose(f);
        return 0;
    }

    /* Cíl */
    char dest_parent_path[256];
    char dest_name[128];
    parse_path(s2, dest_parent_path, dest_name);

    const int dest_parent_id = fs_path_to_inode(filename, dest_parent_path);
    if (dest_parent_id == -1) {
        printf("PATH NOT FOUND\n");
        fclose(f);
        return 0;
    }

    if (find_inode_in_dir(f, &sb, dest_parent_id, dest_name) != -1) {
        printf("EXIST (Target file exists)\n");
        fclose(f);
        return 0;
    }

    /* 1) odebrat ze zdroje */
    (void)remove_directory_item(f, &sb, src_parent_id, src_name);

    /* 2) přidat do cíle (stejný inode) */
    struct directory_item item = {0};
    item.inode = src_inode_id;
    strcpy(item.item_name, dest_name);

    if (!add_directory_item(f, &sb, dest_parent_id, &item)) {
        printf("ERROR MOVING (Target dir full?)\n");
        /* původní kód zde také neprovádí rollback */
    }

    fclose(f);
    return 1;
}
