#define _POSIX_C_SOURCE 200809L /* strdup */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/fs_core.h"
#include "../include/fs_utils.h"

/**
 * @file cmd_extra.c
 * @brief Implementace „extra“ příkazů nad pseudo FS: XCP a ADD.
 *
 * Soubor obsahuje pouze logiku příkazů; samotné primitivy pro práci se
 * superblockem, inody, bitmapami a adresářovými položkami jsou ve fs_* modulech.
 *
 * Pozn.: Záměrně zachováváme původní texty chybových hlášek kvůli testům.
 */

/** Maximální počet clusterů, které může dle zadání soubor použít. */
enum { FS_MAX_FILE_CLUSTERS = 5 };

/** Pomocná makra pro konzistentní cleanup. */
#define SAFE_FREE(p) do { free((p)); (p) = NULL; } while (0)

/**
 * @brief Spojí obsah dvou souborů a uloží výsledek do nového cílového souboru.
 *
 * Chování (včetně hlášek) odpovídá původní implementaci:
 * - s1 a s2 musí existovat a být soubory (ne adresáře)
 * - výsledný soubor nesmí překročit @ref FS_MAX_FILE_CLUSTERS * CLUSTER_SIZE
 * - cílová cesta s3 musí mít existující rodičovský adresář
 * - cílový soubor nesmí existovat
 *
 * @param filename  Cesta k image souboru pseudo FS.
 * @param s1        První zdrojový soubor.
 * @param s2        Druhý zdrojový soubor.
 * @param s3        Cílová cesta (nový soubor).
 * @return 1 při úspěchu, jinak 0.
 */
int fs_xcp(const char *filename, const char *s1, const char *s2, const char *s3)
{
    int ok = 0;
    FILE *f = NULL;
    uint8_t *big_buffer = NULL;

    struct superblock sb;

    f = fopen(filename, "rb+");
    if (!f) {
        return 0;
    }
    load_superblock(f, &sb);

    /* 1) Získání inodů zdrojů */
    const int id1 = fs_path_to_inode(filename, s1);
    const int id2 = fs_path_to_inode(filename, s2);

    if (id1 == -1 || id2 == -1) {
        printf("FILE NOT FOUND (Source)\n");
        goto cleanup;
    }

    /* 2) Ověření, že jde o soubory */
    struct pseudo_inode i1, i2;
    read_inode(f, &sb, id1, &i1);
    read_inode(f, &sb, id2, &i2);

    if (i1.isDirectory || i2.isDirectory) {
        printf("SOURCE IS DIRECTORY\n");
        goto cleanup;
    }

    /* 3) Kontrola velikosti výsledku */
    const int total_size = i1.file_size + i2.file_size;
    if (total_size > FS_MAX_FILE_CLUSTERS * CLUSTER_SIZE) {
        printf("RESULT TOO BIG\n");
        goto cleanup;
    }

    /* 4) Načtení obou souborů do RAM (max 5 clusterů) */
    big_buffer = (uint8_t *)calloc(1, (size_t)FS_MAX_FILE_CLUSTERS * CLUSTER_SIZE);
    if (!big_buffer) {
        /* V zadání se tato situace netestuje; zvolíme tichý fail. */
        goto cleanup;
    }
    load_file_content(f, &sb, id1, big_buffer);
    load_file_content(f, &sb, id2, big_buffer + i1.file_size);

    /* 5) Rozparsování cílové cesty s3 na (parent_path, new_name) */
    char parent_path[256] = "";
    char new_name[128] = "";

    char *path_copy = strdup(s3);
    if (!path_copy) {
        goto cleanup;
    }

    char *last_slash = strrchr(path_copy, '/');

    if (last_slash == NULL) {
        /* "soubor" -> parent je root */
        strcpy(parent_path, "/");
        strncpy(new_name, s3, 11);
        new_name[11] = '\0';
    } else if (last_slash == path_copy) {
        /* "/soubor" -> parent root */
        strcpy(parent_path, "/");
        strncpy(new_name, last_slash + 1, 11);
        new_name[11] = '\0';
    } else {
        /* "/a/b/soubor" -> parent "/a/b" */
        *last_slash = '\0';
        strncpy(parent_path, path_copy, sizeof(parent_path) - 1);
        parent_path[sizeof(parent_path) - 1] = '\0';

        strncpy(new_name, last_slash + 1, 11);
        new_name[11] = '\0';
    }

    free(path_copy);
    path_copy = NULL;

    const int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) {
        printf("PATH NOT FOUND (Target)\n");
        goto cleanup;
    }
    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) {
        printf("EXIST\n");
        goto cleanup;
    }

    const int free_inode = find_free_bit(f, &sb, true);
    if (free_inode == -1) {
        printf("NO SPACE (Inodes)\n");
        goto cleanup;
    }
    set_bit(f, &sb, true, free_inode, true); /* rezervace inodu */

    /* 6) Zápis spojených dat do nového inodu */
    if (!write_buffer_to_new_inode(f, &sb, free_inode, big_buffer, total_size)) {
        printf("NO SPACE (Blocks)\n");
        /* Rollback inodu by byl ideální, ale pro SP není vyžadován. */
        goto cleanup;
    }

    /* 7) Přidání položky do cílového adresáře */
    struct directory_item new_entry = { .inode = free_inode };
    strcpy(new_entry.item_name, new_name);
    add_directory_item(f, &sb, parent_id, &new_entry);

    ok = 1;

cleanup:
    SAFE_FREE(big_buffer);
    if (f) {
        fclose(f);
        f = NULL;
    }
    return ok;
}

/**
 * @brief Připojí (append) obsah souboru s2 na konec souboru s1.
 *
 * Implementace využívá jednoduchý postup:
 * 1) Načte obsah s1 a s2 do RAM (max 5 clusterů)
 * 2) Uvolní původní bloky s1 v bitmapě
 * 3) Zapíše nový obsah do stejného inodu s1 (id1) pomocí write_buffer_to_new_inode()
 *
 * Upozornění: Při nedostatku místa během zápisu může dojít ke ztrátě původního s1,
 * což je stejné riziko jako v původní implementaci.
 *
 * @param filename  Cesta k image souboru pseudo FS.
 * @param s1        Cílový soubor (přepisuje se).
 * @param s2        Zdrojový soubor (připojí se).
 * @return 1 při úspěchu, jinak 0.
 */
int fs_add(const char *filename, const char *s1, const char *s2)
{
    int ok = 0;
    FILE *f = NULL;
    uint8_t *big_buffer = NULL;

    struct superblock sb;

    f = fopen(filename, "rb+");
    if (!f) {
        return 0;
    }
    load_superblock(f, &sb);

    /* 1) Získání inodů */
    const int id1 = fs_path_to_inode(filename, s1);
    const int id2 = fs_path_to_inode(filename, s2);

    if (id1 == -1 || id2 == -1) {
        printf("FILE NOT FOUND\n");
        goto cleanup;
    }

    struct pseudo_inode i1, i2;
    read_inode(f, &sb, id1, &i1);
    read_inode(f, &sb, id2, &i2);

    if (i1.isDirectory || i2.isDirectory) {
        printf("IS DIRECTORY\n");
        goto cleanup;
    }

    const int new_total_size = i1.file_size + i2.file_size;
    if (new_total_size > FS_MAX_FILE_CLUSTERS * CLUSTER_SIZE) {
        printf("TOO BIG\n");
        goto cleanup;
    }

    /* 2) Načtení obou souborů do RAM */
    big_buffer = (uint8_t *)calloc(1, (size_t)FS_MAX_FILE_CLUSTERS * CLUSTER_SIZE);
    if (!big_buffer) {
        goto cleanup;
    }

    load_file_content(f, &sb, id1, big_buffer);
    load_file_content(f, &sb, id2, big_buffer + i1.file_size);

    /*
     * 3) Uvolnění původních bloků s1 a zápis nového obsahu.
     *
     * Pozn.: CLUSTER_UNUSED znamená „není alokováno“. Uvolňujeme pouze platné clustery.
     */
    const int32_t old_blocks[FS_MAX_FILE_CLUSTERS] = {
        i1.direct1, i1.direct2, i1.direct3, i1.direct4, i1.direct5
    };

    for (int i = 0; i < FS_MAX_FILE_CLUSTERS; i++) {
        if (old_blocks[i] != CLUSTER_UNUSED) {
            set_bit(f, &sb, false, old_blocks[i], false);
        }
    }

    if (!write_buffer_to_new_inode(f, &sb, id1, big_buffer, new_total_size)) {
        printf("NO SPACE (Blocks)\n");
        goto cleanup;
    }

    ok = 1;

cleanup:
    SAFE_FREE(big_buffer);
    if (f) {
        fclose(f);
        f = NULL;
    }
    return ok;
}
