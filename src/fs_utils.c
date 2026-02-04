#define _POSIX_C_SOURCE 200809L
/**
 * @file fs_utils.c
 * @brief Pomocné funkce pro práci s pseudo-souborovým systémem (SB, inody, bitmapy, adresáře, obsah).
 *
 * DŮLEŽITÉ:
 *  - Zachovává původní API definované v include/fs_utils.h.
 *  - Zachovává logiku/rozvržení dat na disku (offsety, velikosti struktur, práce s cluster_size).
 *  - Refaktoring je konzervativní: zlepšuje čitelnost, doplňuje kontroly a komentáře,
 *    ale nemění výstupy ani chování očekávané testy.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../include/fs_utils.h"

/* ========================================================================== */
/* Interní helpery                                                            */
/* ========================================================================== */

/**
 * @brief Bezpečný přesun kurzoru na absolutní offset.
 * @return true při úspěchu, jinak false.
 */
static bool seek_abs(FILE *f, long offset)
{
    return f && fseek(f, offset, SEEK_SET) == 0;
}

/**
 * @brief Absolutní offset inodu v souboru FS.
 */
static long inode_offset(const struct superblock *sb, int inode_id)
{
    return sb->inode_start_address
         + (long)inode_id * (long)sizeof(struct pseudo_inode);
}

/**
 * @brief Absolutní offset clusteru (datového bloku) v souboru FS.
 */
static long cluster_offset(const struct superblock *sb, int32_t cluster_id)
{
    return sb->data_start_address + (long)cluster_id * (long)sb->cluster_size;
}

/**
 * @brief Start adresa správné bitmapy (inode/data).
 */
static long bitmap_start(const struct superblock *sb, bool is_inode_bitmap)
{
    return is_inode_bitmap ? sb->bitmapi_start_address : sb->bitmap_start_address;
}

/* ========================================================================== */
/* Superblock + inode I/O                                                     */
/* ========================================================================== */

int load_superblock(FILE *f, struct superblock *sb)
{
    if (!f || !sb) {
        return 0;
    }

    if (!seek_abs(f, 0)) {
        return 0;
    }

    return fread(sb, sizeof(*sb), 1, f) == 1;
}

void read_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode)
{
    if (!f || !sb || !inode || inode_id < 0) {
        return;
    }

    const long off = inode_offset(sb, inode_id);
    if (!seek_abs(f, off)) {
        return;
    }

    (void)fread(inode, sizeof(*inode), 1, f);
}

void write_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode)
{
    if (!f || !sb || !inode || inode_id < 0) {
        return;
    }

    const long off = inode_offset(sb, inode_id);
    if (!seek_abs(f, off)) {
        return;
    }

    (void)fwrite(inode, sizeof(*inode), 1, f);
}

/* ========================================================================== */
/* Bitmapy                                                                    */
/* ========================================================================== */

int find_free_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap)
{
    if (!f || !sb) {
        return -1;
    }

    const long start_addr = bitmap_start(sb, is_inode_bitmap);

    /* Pozn.: původní kód používá cluster_count jako "počet položek" bitmapy
       pro inode i datové bloky – zachováváme to kvůli kompatibilitě. */
    const int total_items = sb->cluster_count;
    const int size_in_bytes = (total_items + 7) / 8;

    uint8_t *buffer = (uint8_t *)malloc((size_t)size_in_bytes);
    if (!buffer) {
        return -1;
    }

    if (!seek_abs(f, start_addr)) {
        free(buffer);
        return -1;
    }
    (void)fread(buffer, 1, (size_t)size_in_bytes, f);

    for (int i = 0; i < total_items; i++) {
        const uint8_t byte = buffer[i / 8];
        const uint8_t mask = (uint8_t)(1u << (i % 8));
        if ((byte & mask) == 0) {
            free(buffer);
            return i;
        }
    }

    free(buffer);
    return -1;
}

void set_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap, int index, bool status)
{
    if (!f || !sb || index < 0) {
        return;
    }

    const long start_addr = bitmap_start(sb, is_inode_bitmap);
    const long byte_offset = index / 8;
    const int bit_offset = index % 8;

    if (!seek_abs(f, start_addr + byte_offset)) {
        return;
    }

    uint8_t byte = 0;
    (void)fread(&byte, 1, 1, f);

    if (status) {
        byte |= (uint8_t)(1u << bit_offset);
    } else {
        byte &= (uint8_t)~(1u << bit_offset);
    }

    /* Přepíšeme právě čtený bajt (o 1 zpět). */
    (void)fseek(f, -1, SEEK_CUR);
    (void)fwrite(&byte, 1, 1, f);
}

/* ========================================================================== */
/* Adresáře                                                                   */
/* ========================================================================== */

int find_inode_in_dir(FILE *f, struct superblock *sb, int parent_inode_id, char *name)
{
    if (!f || !sb || !name || parent_inode_id < 0) {
        return -1;
    }

    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);
    if (!parent.isDirectory) {
        return -1;
    }

    const int32_t blocks[5] = { parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5 };
    const int items_per_cluster = (int)(sb->cluster_size / sizeof(struct directory_item));

    struct directory_item item;
    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) {
            continue;
        }

        if (!seek_abs(f, cluster_offset(sb, blocks[i]))) {
            continue;
        }

        for (int j = 0; j < items_per_cluster; j++) {
            (void)fread(&item, sizeof(item), 1, f);
            if (item.item_name[0] != '\0' && strcmp(item.item_name, name) == 0) {
                return item.inode;
            }
        }
    }

    return -1;
}

int add_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, struct directory_item *new_item)
{
    if (!f || !sb || !new_item || parent_inode_id < 0) {
        return 0;
    }

    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);
    if (!parent.isDirectory) {
        return 0;
    }

    const int32_t blocks[5] = { parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5 };
    const int items_per_cluster = (int)(sb->cluster_size / sizeof(struct directory_item));

    struct directory_item item;
    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) {
            continue;
        }

        const long base = cluster_offset(sb, blocks[i]);

        for (int j = 0; j < items_per_cluster; j++) {
            const long item_pos = base + (long)j * (long)sizeof(struct directory_item);

            if (!seek_abs(f, item_pos)) {
                continue;
            }

            (void)fread(&item, sizeof(item), 1, f);
            if (item.item_name[0] == '\0') {
                if (!seek_abs(f, item_pos)) {
                    return 0;
                }
                (void)fwrite(new_item, sizeof(*new_item), 1, f);
                return 1;
            }
        }
    }

    return 0;
}

/**
 * @brief Převod cesty na inode (pro potřeby příkazů).
 *
 * @param filename Cesta k souboru s FS (disk image).
 * @param path Absolutní/relativní cesta v pseudo-FS (např. "/data/soubor.txt").
 * @return inode ID při úspěchu, jinak -1.
 */
int fs_path_to_inode(const char *filename, const char *path)
{
    if (!filename || !path) {
        return -1;
    }

    FILE *f = fopen(filename, "rb");
    if (!f) {
        return -1;
    }

    struct superblock sb;
    (void)load_superblock(f, &sb);

    int current_inode = 0; /* root */

    char *path_copy = strdup(path);
    if (!path_copy) {
        fclose(f);
        return -1;
    }

    char *token = strtok(path_copy, "/");
    while (token != NULL) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok(NULL, "/");
            continue;
        }

        const int next_inode = find_inode_in_dir(f, &sb, current_inode, token);
        if (next_inode == -1) {
            free(path_copy);
            fclose(f);
            return -1;
        }

        current_inode = next_inode;
        token = strtok(NULL, "/");
    }

    free(path_copy);
    fclose(f);
    return current_inode;
}

/* ========================================================================== */
/* Mazání a uvolňování                                                        */
/* ========================================================================== */

int remove_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, char *name)
{
    if (!f || !sb || !name || parent_inode_id < 0) {
        return 0;
    }

    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);

    const int32_t blocks[5] = { parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5 };
    const int items_per_cluster = (int)(sb->cluster_size / sizeof(struct directory_item));

    struct directory_item item;
    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) {
            continue;
        }

        const long base = cluster_offset(sb, blocks[i]);

        for (int j = 0; j < items_per_cluster; j++) {
            const long item_pos = base + (long)j * (long)sizeof(struct directory_item);

            if (!seek_abs(f, item_pos)) {
                continue;
            }
            (void)fread(&item, sizeof(item), 1, f);

            if (item.item_name[0] != '\0' && strcmp(item.item_name, name) == 0) {
                /* "Smažeme" položku nulováním – zachováme původní chování. */
                const struct directory_item empty_item = (struct directory_item){0};

                if (!seek_abs(f, item_pos)) {
                    return 0;
                }
                (void)fwrite(&empty_item, sizeof(empty_item), 1, f);
                return 1;
            }
        }
    }

    return 0;
}

int is_dir_empty(FILE *f, struct superblock *sb, int inode_id)
{
    if (!f || !sb || inode_id < 0) {
        return 0;
    }

    struct pseudo_inode inode;
    read_inode(f, sb, inode_id, &inode);

    const int32_t blocks[5] = { inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5 };
    const int items_per_cluster = (int)(sb->cluster_size / sizeof(struct directory_item));

    struct directory_item item;
    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) {
            continue;
        }

        const long base = cluster_offset(sb, blocks[i]);

        for (int j = 0; j < items_per_cluster; j++) {
            if (!seek_abs(f, base + (long)j * (long)sizeof(struct directory_item))) {
                continue;
            }
            (void)fread(&item, sizeof(item), 1, f);

            if (item.item_name[0] != '\0') {
                if (strcmp(item.item_name, ".") != 0 && strcmp(item.item_name, "..") != 0) {
                    return 0; /* není prázdný */
                }
            }
        }
    }

    return 1; /* je prázdný */
}

void free_inode_resources(FILE *f, struct superblock *sb, int inode_id)
{
    if (!f || !sb || inode_id < 0) {
        return;
    }

    struct pseudo_inode inode;
    read_inode(f, sb, inode_id, &inode);

    /* 1) Uvolnění datových bloků v bitmapě */
    const int32_t blocks[5] = { inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5 };
    for (int i = 0; i < 5; i++) {
        if (blocks[i] != CLUSTER_UNUSED) {
            set_bit(f, sb, false, blocks[i], false); /* data bitmap: 0 */
        }
    }

    /* 2) Uvolnění inodu v inode bitmapě */
    set_bit(f, sb, true, inode_id, false);
}

/* ========================================================================== */
/* Práce s cestou                                                             */
/* ========================================================================== */

void parse_path(const char *path, char *parent_path, char *filename)
{
    if (!path || !parent_path || !filename) {
        return;
    }

    char *path_copy = strdup(path);
    if (!path_copy) {
        /* Konzervativně: pokud alokace selže, nastavíme bezpečné defaulty. */
        strcpy(parent_path, "/");
        strncpy(filename, path, 11);
        filename[11] = '\0';
        return;
    }

    char *last_slash = strrchr(path_copy, '/');

    if (last_slash == NULL) {
        /* "soubor.txt" -> root */
        strcpy(parent_path, "/");
        strncpy(filename, path, 11);
    } else if (last_slash == path_copy) {
        /* "/soubor.txt" -> root */
        strcpy(parent_path, "/");
        strncpy(filename, last_slash + 1, 11);
    } else {
        /* "/data/soubor.txt" -> "/data" */
        *last_slash = '\0';
        strcpy(parent_path, path_copy);
        strncpy(filename, last_slash + 1, 11);
    }

    free(path_copy);

    /* Oříznutí názvu na 12 znaků (bezpečnost) */
    filename[11] = '\0';
}

/* ========================================================================== */
/* Práce s obsahem souborů                                                    */
/* ========================================================================== */

int load_file_content(FILE *f, struct superblock *sb, int inode_id, uint8_t *buffer)
{
    if (!f || !sb || !buffer || inode_id < 0) {
        return 0;
    }

    struct pseudo_inode inode;
    read_inode(f, sb, inode_id, &inode);

    const int32_t blocks[5] = { inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5 };

    int bytes_remaining = inode.file_size;
    int bytes_read = 0;

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED || bytes_remaining <= 0) {
            break;
        }

        if (!seek_abs(f, cluster_offset(sb, blocks[i]))) {
            break;
        }

        const int to_read = (bytes_remaining > sb->cluster_size) ? sb->cluster_size : bytes_remaining;
        (void)fread(buffer + bytes_read, 1, (size_t)to_read, f);

        bytes_read += to_read;
        bytes_remaining -= to_read;
    }

    return bytes_read;
}

int write_buffer_to_new_inode(FILE *f, struct superblock *sb, int inode_id, uint8_t *buffer, int size)
{
    if (!f || !sb || !buffer || inode_id < 0 || size < 0) {
        return 0;
    }

    int32_t blocks[5] = { CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED };
    int bytes_rem = size;
    int b_idx = 0;

    while (bytes_rem > 0 && b_idx < 5) {
        const int free_block = find_free_bit(f, sb, false); /* data bitmap */
        if (free_block == -1) {
            return 0; /* došlo místo */
        }

        set_bit(f, sb, false, free_block, true);
        blocks[b_idx++] = free_block;

        if (!seek_abs(f, cluster_offset(sb, free_block))) {
            return 0;
        }

        /* Původní chování: vždy zapíšeme celý cluster_size (vyčištěný nulami). */
        uint8_t *cluster_buf = (uint8_t *)calloc(1, (size_t)sb->cluster_size);
        if (!cluster_buf) {
            return 0;
        }

        const int to_copy = (bytes_rem > sb->cluster_size) ? sb->cluster_size : bytes_rem;
        memcpy(cluster_buf, buffer + (size - bytes_rem), (size_t)to_copy);

        (void)fwrite(cluster_buf, 1, (size_t)sb->cluster_size, f);
        free(cluster_buf);

        bytes_rem -= to_copy;
    }

    struct pseudo_inode inode = (struct pseudo_inode){0};
    inode.nodeid = inode_id;
    inode.file_size = size;
    inode.references = 1;
    inode.isDirectory = false;
    inode.direct1 = blocks[0];
    inode.direct2 = blocks[1];
    inode.direct3 = blocks[2];
    inode.direct4 = blocks[3];
    inode.direct5 = blocks[4];
    inode.indirect1 = CLUSTER_UNUSED;
    inode.indirect2 = CLUSTER_UNUSED;

    write_inode(f, sb, inode_id, &inode);
    return 1;
}
