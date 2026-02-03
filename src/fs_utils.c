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