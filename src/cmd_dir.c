#define _POSIX_C_SOURCE 200809L /* strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/fs_core.h"
#include "../include/fs_utils.h"

/**
 * @file cmd_dir.c
 * @brief Implementace příkazů pracujících s adresáři: ls, mkdir, rmdir.
 *
 * Pozn.: Řetězce výstupů jsou záměrně konzervativní, protože bývají součástí
 * automatických testů (např. "FILE NOT FOUND", "PATH NOT FOUND", "EXIST").
 */

/** Maximální počet přímých datových bloků v pseudo-inodu (direct1..direct5). */
enum { DIRECT_BLOCK_COUNT = 5 };

/**
 * @brief Bezpečně zkopíruje jméno položky (MAX_NAME_LEN) a zajistí NUL terminátor.
 */
/**
 * @brief Bezpečně zkopíruje jméno položky do cílového pole a zajistí NUL terminátor.
 *
 * @param dst      Cílové pole (např. directory_item.item_name).
 * @param dst_size Velikost cílového pole v bajtech.
 * @param src      Zdrojový řetězec (jméno).
 */
static void copy_item_name(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    /* Zkopíruj maximálně dst_size-1 znaků a vždy ukonči NULem. */
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/**
 * @brief Vrátí přímé bloky adresáře (direct1..direct5) do pole.
 */
static void get_direct_blocks(const struct pseudo_inode *inode, int32_t out_blocks[DIRECT_BLOCK_COUNT])
{
    out_blocks[0] = inode->direct1;
    out_blocks[1] = inode->direct2;
    out_blocks[2] = inode->direct3;
    out_blocks[3] = inode->direct4;
    out_blocks[4] = inode->direct5;
}

/**
 * @brief Projde položky adresáře a vypíše je (bez "." a "..").
 *
 * @param f Otevřený FS soubor.
 * @param sb Načtený superblock.
 * @param dir_inode Inode adresáře.
 */
static void list_directory_items(FILE *f, const struct superblock *sb, const struct pseudo_inode *dir_inode)
{
    int32_t blocks[DIRECT_BLOCK_COUNT];
    get_direct_blocks(dir_inode, blocks);

    const int items_per_cluster = (int)(sb->cluster_size / sizeof(struct directory_item));
    struct directory_item item;

    for (int i = 0; i < DIRECT_BLOCK_COUNT; i++) {
        if (blocks[i] == CLUSTER_UNUSED) {
            continue;
        }

        const long cluster_addr = sb->data_start_address + (long)blocks[i] * sb->cluster_size;
        if (fseek(f, cluster_addr, SEEK_SET) != 0) {
            /* Pokud se nepovede seeknout, raději pokračuj dál než spadnout. */
            continue;
        }

        for (int j = 0; j < items_per_cluster; j++) {
            if (fread(&item, sizeof(item), 1, f) != 1) {
                break;
            }

            if (item.item_name[0] == '\0') {
                continue;
            }

            /* Nechceme vypisovat "." a ".." (většinou se netestují). */
            if (strcmp(item.item_name, ".") == 0 || strcmp(item.item_name, "..") == 0) {
                continue;
            }

            struct pseudo_inode item_inode;
            const long return_pos = ftell(f);
            read_inode(f, (struct superblock *)sb, item.inode, &item_inode);
            printf("%s: %s\n", item_inode.isDirectory ? "DIR" : "FILE", item.item_name);

            /* Vrať se na místo za položkou adresáře. */
            if (return_pos >= 0) {
                fseek(f, return_pos, SEEK_SET);
            }
        }
    }
}

/**
 * @brief Výpis obsahu adresáře.
 *
 * @param filename Cesta k souboru FS.
 * @param inode_id Inode id adresáře, který se má vypsat.
 */
void fs_ls(const char *filename, int inode_id)
{
    FILE *f = fopen(filename, "rb");
    if (!f) {
        printf("FILE NOT FOUND\n");
        return;
    }

    struct superblock sb;
    load_superblock(f, &sb);

    struct pseudo_inode dir_inode;
    read_inode(f, &sb, inode_id, &dir_inode);

    if (!dir_inode.isDirectory) {
        printf("PATH NOT FOUND\n");
        fclose(f);
        return;
    }

    list_directory_items(f, &sb, &dir_inode);
    fclose(f);
}

/**
 * @brief Vytvoří nový adresář na zadané cestě.
 *
 * Vypisuje:
 * - "PATH NOT FOUND" pokud neexistuje rodičovská cesta
 * - "EXIST" pokud už v rodiči existuje položka stejného jména
 * - "NO SPACE" pokud není volný inode nebo cluster
 *
 * @param filename Cesta k souboru FS.
 * @param path Absolutní cesta nového adresáře (např. "/a/b").
 * @return 1 úspěch, 0 chyba
 */
int fs_mkdir(const char *filename, const char *path)
{
    FILE *f = fopen(filename, "rb+");
    if (!f) {
        return 0;
    }

    struct superblock sb;
    load_superblock(f, &sb);

    /* Rozparsuj cestu na parent a jméno. */
    char parent_path[256] = "";
    char new_name[MAX_NAME_LEN + 1] = "";

    char *path_copy = strdup(path);
    if (!path_copy) {
        fclose(f);
        return 0;
    }

    char *last_slash = strrchr(path_copy, '/');
    if (last_slash == NULL) {
        /* Nečekané – ale pro konzistenci bereme jako root parent. */
        strcpy(parent_path, "/");
        copy_item_name(new_name, sizeof(new_name), path);
    } else if (last_slash == path_copy) {
        /* "/name" => parent "/" */
        strcpy(parent_path, "/");
        copy_item_name(new_name, sizeof(new_name), last_slash + 1);
    } else {
        *last_slash = '\0';
        strncpy(parent_path, path_copy, sizeof(parent_path) - 1);
        parent_path[sizeof(parent_path) - 1] = '\0';
        copy_item_name(new_name, sizeof(new_name), last_slash + 1);
    }
    free(path_copy);

    if (strlen(new_name) == 0 || strlen(new_name) > MAX_NAME_LEN) {
        fclose(f);
        return 0;
    }

    const int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) {
        printf("PATH NOT FOUND\n");
        fclose(f);
        return 0;
    }

    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) {
        printf("EXIST\n");
        fclose(f);
        return 0;
    }

    const int free_inode = find_free_bit(f, &sb, true);
    const int free_block = find_free_bit(f, &sb, false);
    if (free_inode == -1 || free_block == -1) {
        printf("NO SPACE\n");
        fclose(f);
        return 0;
    }

    set_bit(f, &sb, true, free_inode, true);
    set_bit(f, &sb, false, free_block, true);

    struct pseudo_inode new_inode = {0};
    new_inode.nodeid = free_inode;
    new_inode.isDirectory = true;
    new_inode.references = 1;
    new_inode.file_size = sb.cluster_size;
    new_inode.direct1 = free_block;
    new_inode.direct2 = CLUSTER_UNUSED;
    new_inode.direct3 = CLUSTER_UNUSED;
    new_inode.direct4 = CLUSTER_UNUSED;
    new_inode.direct5 = CLUSTER_UNUSED;
    new_inode.indirect1 = CLUSTER_UNUSED;
    new_inode.indirect2 = CLUSTER_UNUSED;

    write_inode(f, &sb, free_inode, &new_inode);

    /* Inicializace dat adresáře: vyplň cluster nulami a vlož "." + "..". */
    const long data_addr = sb.data_start_address + (long)free_block * sb.cluster_size;
    if (fseek(f, data_addr, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    uint8_t *zeros = (uint8_t *)calloc(1, sb.cluster_size);
    if (!zeros) {
        fclose(f);
        return 0;
    }
    fwrite(zeros, 1, sb.cluster_size, f);
    free(zeros);

    struct directory_item dot = {.inode = free_inode};
    strcpy(dot.item_name, ".");
    struct directory_item dotdot = {.inode = parent_id};
    strcpy(dotdot.item_name, "..");

    fseek(f, data_addr, SEEK_SET);
    fwrite(&dot, sizeof(dot), 1, f);
    fwrite(&dotdot, sizeof(dotdot), 1, f);

    /* Přidej položku do rodičovského adresáře. */
    struct directory_item new_entry = {.inode = free_inode};
    copy_item_name(new_entry.item_name, sizeof(new_entry.item_name), new_name);
    add_directory_item(f, &sb, parent_id, &new_entry);

    fclose(f);
    return 1;
}

/**
 * @brief Odstraní prázdný adresář.
 *
 * Vypisuje:
 * - "FILE NOT FOUND" pokud cesta neexistuje nebo není adresář
 * - "NOT EMPTY" pokud adresář obsahuje něco jiného než "." a ".."
 *
 * @param filename Cesta k souboru FS.
 * @param path Absolutní cesta adresáře k odstranění.
 * @return 1 úspěch, 0 chyba
 */
int fs_rmdir(const char *filename, const char *path)
{
    FILE *f = fopen(filename, "rb+");
    if (!f) {
        return 0;
    }

    struct superblock sb;
    load_superblock(f, &sb);

    char parent_path[256];
    char name[MAX_NAME_LEN + 1];
    parse_path(path, parent_path, name);

    const int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) {
        printf("FILE NOT FOUND\n");
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

    if (!inode.isDirectory) {
        printf("FILE NOT FOUND\n");
        fclose(f);
        return 0;
    }

    if (!is_dir_empty(f, &sb, inode_id)) {
        printf("NOT EMPTY\n");
        fclose(f);
        return 0;
    }

    remove_directory_item(f, &sb, parent_id, name);
    free_inode_resources(f, &sb, inode_id);

    fclose(f);
    return 1;
}
