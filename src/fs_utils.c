#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/fs_utils.h"

// Načtení SB
int load_superblock(FILE *f, struct superblock *sb) {
    fseek(f, 0, SEEK_SET);
    return (fread(sb, sizeof(struct superblock), 1, f) == 1);
}

// Čtení Inodu
void read_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode) {
    long offset = sb->inode_start_address + (inode_id * sizeof(struct pseudo_inode));
    fseek(f, offset, SEEK_SET);
    fread(inode, sizeof(struct pseudo_inode), 1, f);
}

// Zápis Inodu
void write_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode) {
    long offset = sb->inode_start_address + (inode_id * sizeof(struct pseudo_inode));
    fseek(f, offset, SEEK_SET);
    fwrite(inode, sizeof(struct pseudo_inode), 1, f);
}

// Hledání volného bitu
int find_free_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap) {
    long start_addr = is_inode_bitmap ? sb->bitmapi_start_address : sb->bitmap_start_address;
    int total_items = sb->cluster_count;
    int size_in_bytes = (total_items + 7) / 8;

    uint8_t *buffer = malloc(size_in_bytes);
    fseek(f, start_addr, SEEK_SET);
    fread(buffer, 1, size_in_bytes, f);

    for (int i = 0; i < total_items; i++) {
        if (!((buffer[i / 8] >> (i % 8)) & 1)) {
            free(buffer);
            return i;
        }
    }
    free(buffer);
    return -1;
}

// Nastavení bitu
void set_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap, int index, bool status) {
    long start_addr = is_inode_bitmap ? sb->bitmapi_start_address : sb->bitmap_start_address;
    long byte_offset = index / 8;
    int bit_offset = index % 8;

    fseek(f, start_addr + byte_offset, SEEK_SET);
    uint8_t byte;
    fread(&byte, 1, 1, f);

    if (status) byte |= (1 << bit_offset);
    else byte &= ~(1 << bit_offset);

    fseek(f, -1, SEEK_CUR);
    fwrite(&byte, 1, 1, f);
}

// Hledání v adresáři
int find_inode_in_dir(FILE *f, struct superblock *sb, int parent_inode_id, char *name) {
    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);
    if (!parent.isDirectory) return -1;

    int32_t blocks[] = {parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5};
    struct directory_item item;
    int items_per_cluster = sb->cluster_size / sizeof(struct directory_item);

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) continue;
        fseek(f, sb->data_start_address + (blocks[i] * sb->cluster_size), SEEK_SET);
        for (int j = 0; j < items_per_cluster; j++) {
            fread(&item, sizeof(struct directory_item), 1, f);
            if (item.item_name[0] != '\0' && strcmp(item.item_name, name) == 0) {
                return item.inode;
            }
        }
    }
    return -1;
}

// Přidání položky do adresáře
int add_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, struct directory_item *new_item) {
    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);
    if (!parent.isDirectory) return 0;

    int32_t blocks[] = {parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5};
    struct directory_item item;
    int items_per_cluster = sb->cluster_size / sizeof(struct directory_item);

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) continue;
        long cluster_offset = sb->data_start_address + (blocks[i] * sb->cluster_size);
        for (int j = 0; j < items_per_cluster; j++) {
            long item_pos = cluster_offset + (j * sizeof(struct directory_item));
            fseek(f, item_pos, SEEK_SET);
            fread(&item, sizeof(struct directory_item), 1, f);
            if (item.item_name[0] == '\0') {
                fseek(f, item_pos, SEEK_SET);
                fwrite(new_item, sizeof(struct directory_item), 1, f);
                return 1;
            }
        }
    }
    return 0; 
}

// Path to Inode
int fs_path_to_inode(const char *filename, const char *path) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    struct superblock sb;
    load_superblock(f, &sb);

    int current_inode = 0; 
    char *path_copy = strdup(path);
    char *token = strtok(path_copy, "/");

    while (token != NULL) {
        if (strcmp(token, ".") == 0 || strlen(token) == 0) {
            token = strtok(NULL, "/");
            continue;
        }
        int next_inode = find_inode_in_dir(f, &sb, current_inode, token);
        if (next_inode == -1) {
            free(path_copy); fclose(f); return -1;
        }
        current_inode = next_inode;
        token = strtok(NULL, "/");
    }
    free(path_copy); fclose(f);
    return current_inode;
}

// --- MAZÁNÍ A UVOLŇOVÁNÍ ---

int remove_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, char *name) {
    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);
    
    int32_t blocks[] = {parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5};
    struct directory_item item;
    int items_per_cluster = sb->cluster_size / sizeof(struct directory_item);

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) continue;
        
        long cluster_offset = sb->data_start_address + (blocks[i] * sb->cluster_size);
        
        for (int j = 0; j < items_per_cluster; j++) {
            long item_pos = cluster_offset + (j * sizeof(struct directory_item));
            fseek(f, item_pos, SEEK_SET);
            fread(&item, sizeof(struct directory_item), 1, f);
            
            if (item.item_name[0] != '\0' && strcmp(item.item_name, name) == 0) {
                // Položku "smažeme" tak, že ji celou vynulujeme
                // Tím se místo uvolní pro add_directory_item
                struct directory_item empty_item = {0};
                fseek(f, item_pos, SEEK_SET);
                fwrite(&empty_item, sizeof(struct directory_item), 1, f);
                return 1;
            }
        }
    }
    return 0; // Nenalezeno
}

int is_dir_empty(FILE *f, struct superblock *sb, int inode_id) {
    struct pseudo_inode inode;
    read_inode(f, sb, inode_id, &inode);
    
    int32_t blocks[] = {inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5};
    struct directory_item item;
    int items_per_cluster = sb->cluster_size / sizeof(struct directory_item);

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) continue;
        long cluster_offset = sb->data_start_address + (blocks[i] * sb->cluster_size);

        for (int j = 0; j < items_per_cluster; j++) {
            fseek(f, cluster_offset + (j * sizeof(struct directory_item)), SEEK_SET);
            fread(&item, sizeof(struct directory_item), 1, f);
            
            if (item.item_name[0] != '\0') {
                // Pokud najdeme cokoliv jiného než . a .., adresář není prázdný
                if (strcmp(item.item_name, ".") != 0 && strcmp(item.item_name, "..") != 0) {
                    return 0; // Není prázdný
                }
            }
        }
    }
    return 1; // Je prázdný
}

void free_inode_resources(FILE *f, struct superblock *sb, int inode_id) {
    struct pseudo_inode inode;
    read_inode(f, sb, inode_id, &inode);

    // 1. Uvolnění datových bloků v bitmapě
    int32_t blocks[] = {inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5};
    for (int i = 0; i < 5; i++) {
        if (blocks[i] != CLUSTER_UNUSED) {
            set_bit(f, sb, false, blocks[i], false); // false = data bitmap, false = nastavit na 0
        }
    }

    // 2. Uvolnění inodu v bitmapě
    set_bit(f, sb, true, inode_id, false); // true = inode bitmap
}

// Pomocná funkce pro rozdělení cesty na "Rodičovská cesta" a "Jméno souboru"
// Vstup: path (např. "/data/soubor.txt")
// Výstup: parent_path ("/data"), filename ("soubor.txt")
void parse_path(const char *path, char *parent_path, char *filename) {
    char *path_copy = strdup(path);
    char *last_slash = strrchr(path_copy, '/');

    if (last_slash == NULL) { 
        // "soubor.txt" -> root
        strcpy(parent_path, "/");
        strncpy(filename, path, 11);
    } else if (last_slash == path_copy) { 
        // "/soubor.txt" -> root
        strcpy(parent_path, "/");
        strncpy(filename, last_slash + 1, 11);
    } else { 
        // "/data/soubor.txt" -> "/data"
        *last_slash = '\0';
        strcpy(parent_path, path_copy);
        strncpy(filename, last_slash + 1, 11);
    }
    free(path_copy);
    
    // Oříznutí názvu na 12 znaků (bezpečnost)
    filename[11] = '\0';
}

// --- POMOCNÉ FUNKCE PRO PRÁCI S OBSAHEM ---

// Načte obsah souboru (inode_id) do bufferu. Buffer musí být alokován volajícím.
// Vrací počet přečtených bytů.
int load_file_content(FILE *f, struct superblock *sb, int inode_id, uint8_t *buffer) {
    struct pseudo_inode inode;
    read_inode(f, sb, inode_id, &inode);

    int32_t blocks[] = {inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5};
    int bytes_remaining = inode.file_size;
    int bytes_read = 0;

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED || bytes_remaining <= 0) break;

        long data_addr = sb->data_start_address + (blocks[i] * sb->cluster_size);
        fseek(f, data_addr, SEEK_SET);

        int to_read = (bytes_remaining > sb->cluster_size) ? sb->cluster_size : bytes_remaining;
        fread(buffer + bytes_read, 1, to_read, f);

        bytes_read += to_read;
        bytes_remaining -= to_read;
    }
    return bytes_read;
}

// Zjednodušená funkce pro zápis dat z bufferu do NOVÉHO inodu
// (Tato logika je vytažena z fs_incp pro znovupoužití)
int write_buffer_to_new_inode(FILE *f, struct superblock *sb, int inode_id, uint8_t *buffer, int size) {
    int32_t blocks[5] = {CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED};
    int bytes_rem = size;
    int b_idx = 0;

    while (bytes_rem > 0 && b_idx < 5) {
        int free_block = find_free_bit(f, sb, false); // false = data bitmap
        if (free_block == -1) return 0; // Došlo místo
        
        set_bit(f, sb, false, free_block, true);
        blocks[b_idx++] = free_block;

        long data_addr = sb->data_start_address + (free_block * sb->cluster_size);
        fseek(f, data_addr, SEEK_SET);
        
        // Vždy zapíšeme celý cluster (vyčištěný nulami), i když data končí dřív
        uint8_t cluster_buf[CLUSTER_SIZE] = {0};
        int to_copy = (bytes_rem > CLUSTER_SIZE) ? CLUSTER_SIZE : bytes_rem;
        memcpy(cluster_buf, buffer + (size - bytes_rem), to_copy);
        
        fwrite(cluster_buf, 1, CLUSTER_SIZE, f);
        bytes_rem -= to_copy;
    }

    struct pseudo_inode inode = {0};
    inode.nodeid = inode_id;
    inode.file_size = size;
    inode.references = 1;
    inode.isDirectory = false;
    inode.direct1 = blocks[0]; inode.direct2 = blocks[1];
    inode.direct3 = blocks[2]; inode.direct4 = blocks[3];
    inode.direct5 = blocks[4];
    inode.indirect1 = CLUSTER_UNUSED; inode.indirect2 = CLUSTER_UNUSED;

    write_inode(f, sb, inode_id, &inode);
    return 1;
}