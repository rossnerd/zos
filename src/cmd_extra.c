#define _POSIX_C_SOURCE 200809L // Kvůli strdup
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/fs_core.h"
#include "../include/fs_utils.h"


// --- XCP (Spojení souborů) ---
int fs_xcp(const char *filename, const char *s1, const char *s2, const char *s3) {
    FILE *f = fopen(filename, "rb+");
    if (!f) return 0;
    struct superblock sb;
    load_superblock(f, &sb);

    // 1. Získání Inodů zdrojů
    int id1 = fs_path_to_inode(filename, s1);
    int id2 = fs_path_to_inode(filename, s2);

    if (id1 == -1 || id2 == -1) {
        printf("FILE NOT FOUND (Source)\n");
        fclose(f); return 0;
    }

    // 2. Ověření, že to jsou soubory
    struct pseudo_inode i1, i2;
    read_inode(f, &sb, id1, &i1);
    read_inode(f, &sb, id2, &i2);

    if (i1.isDirectory || i2.isDirectory) {
        printf("SOURCE IS DIRECTORY\n");
        fclose(f); return 0;
    }

    // 3. Kontrola velikosti
    if (i1.file_size + i2.file_size > 5 * CLUSTER_SIZE) {
        printf("RESULT TOO BIG\n");
        fclose(f); return 0;
    }

    // 4. Načtení dat do RAM
    uint8_t *big_buffer = calloc(1, 5 * CLUSTER_SIZE);
    load_file_content(f, &sb, id1, big_buffer);
    load_file_content(f, &sb, id2, big_buffer + i1.file_size);
    int total_size = i1.file_size + i2.file_size;

    // 5. Vytvoření cílového souboru s3 (Logika podobná incp/mkdir)
    // Parsování cesty s3
    char *path_copy = strdup(s3);
    char *last_slash = strrchr(path_copy, '/');
    char parent_path[256] = ""; char new_name[128] = "";

    if (last_slash == NULL) { strcpy(parent_path, "/"); strncpy(new_name, s3, 11); }
    else if (last_slash == path_copy) { strcpy(parent_path, "/"); strncpy(new_name, last_slash + 1, 11); }
    else { *last_slash = '\0'; strcpy(parent_path, path_copy); strncpy(new_name, last_slash + 1, 11); }
    free(path_copy);

    int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) { printf("PATH NOT FOUND (Target)\n"); free(big_buffer); fclose(f); return 0; }
    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) { printf("EXIST\n"); free(big_buffer); fclose(f); return 0; }

    int free_inode = find_free_bit(f, &sb, true);
    if (free_inode == -1) { printf("NO SPACE (Inodes)\n"); free(big_buffer); fclose(f); return 0; }
    set_bit(f, &sb, true, free_inode, true); // Rezervujeme inode

    // 6. Zápis spojených dat
    if (!write_buffer_to_new_inode(f, &sb, free_inode, big_buffer, total_size)) {
        printf("NO SPACE (Blocks)\n");
        // Rollback inodu by byl vhodný, ale pro SP stačí
        free(big_buffer); fclose(f); return 0;
    }

    // 7. Přidání do adresáře
    struct directory_item new_entry = { .inode = free_inode };
    strcpy(new_entry.item_name, new_name);
    add_directory_item(f, &sb, parent_id, &new_entry);

    free(big_buffer);
    fclose(f);
    return 1;
}

// --- ADD (Append) ---
int fs_add(const char *filename, const char *s1, const char *s2) {
    FILE *f = fopen(filename, "rb+");
    if (!f) return 0;
    struct superblock sb;
    load_superblock(f, &sb);

    // 1. Získání Inodů
    int id1 = fs_path_to_inode(filename, s1);
    int id2 = fs_path_to_inode(filename, s2);

    if (id1 == -1 || id2 == -1) { printf("FILE NOT FOUND\n"); fclose(f); return 0; }

    struct pseudo_inode i1, i2;
    read_inode(f, &sb, id1, &i1);
    read_inode(f, &sb, id2, &i2);

    if (i1.isDirectory || i2.isDirectory) { printf("IS DIRECTORY\n"); fclose(f); return 0; }
    if (i1.file_size + i2.file_size > 5 * CLUSTER_SIZE) { printf("TOO BIG\n"); fclose(f); return 0; }

    // 2. Načtení obou souborů do RAM
    uint8_t *big_buffer = calloc(1, 5 * CLUSTER_SIZE);
    
    // Načteme s1
    load_file_content(f, &sb, id1, big_buffer);
    // Načteme s2 a rovnou ho přilepíme za s1
    load_file_content(f, &sb, id2, big_buffer + i1.file_size);
    
    int new_total_size = i1.file_size + i2.file_size;

    // 3. Přepis souboru s1
    // Poznámka: Správně bychom měli uvolnit staré bloky s1 v bitmapě, 
    // pokud by nový obsah zabíral méně místa (což u appendu nehrozí),
    // nebo pokud bychom chtěli alokovat úplně nové bloky.
    // Zde uděláme "chytrý hack": 
    // Uvolníme staré bloky s1 (v paměti i bitmapě) a zapíšeme soubor znovu jako nový.
    
    int32_t old_blocks[] = {i1.direct1, i1.direct2, i1.direct3, i1.direct4, i1.direct5};
    for(int i=0; i<5; i++) {
        if(old_blocks[i] != CLUSTER_UNUSED) {
            set_bit(f, &sb, false, old_blocks[i], false); // Uvolnit bit (nastavit na 0)
        }
    }

    // Teď je s1 "logicky smazaný" (má sice staré metadata, ale bloky jsou volné).
    // Použijeme write_buffer_to_new_inode, který nám vyplní inode znovu a alokuje nové bloky.
    // Protože ID inodu (id1) už máme a je v bitmapě stále označený jako 'used', 
    // jen přepíšeme jeho obsah a pointery.
    
    if (!write_buffer_to_new_inode(f, &sb, id1, big_buffer, new_total_size)) {
        printf("NO SPACE (Blocks)\n");
        // Tady je riziko ztráty dat s1, pokud dojde místo v půlce.
        free(big_buffer); fclose(f); return 0;
    }

    free(big_buffer);
    fclose(f);
    return 1;
}