#define _POSIX_C_SOURCE 200809L // Kvůli strdup
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/fs_core.h"
#include "../include/fs_utils.h"

// --- LS ---
void fs_ls(const char *filename, int inode_id) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }

    struct superblock sb;
    load_superblock(f, &sb);

    struct pseudo_inode dir_inode;
    read_inode(f, &sb, inode_id, &dir_inode);

    if (!dir_inode.isDirectory) {
        printf("PATH NOT FOUND\n"); 
        fclose(f); return;
    }

    // printf("Listing directory ID %d:\n", inode_id);
    int32_t blocks[] = {dir_inode.direct1, dir_inode.direct2, dir_inode.direct3, dir_inode.direct4, dir_inode.direct5};
    struct directory_item item;
    int items_per_cluster = sb.cluster_size / sizeof(struct directory_item);

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) continue; 
        fseek(f, sb.data_start_address + (blocks[i] * sb.cluster_size), SEEK_SET);

        for (int j = 0; j < items_per_cluster; j++) {
            fread(&item, sizeof(struct directory_item), 1, f);
            if (item.item_name[0] != '\0') {
                // Vypisujeme jen reálné položky, ne "." a ".." (bezpečnější vůči testům)
                if (strcmp(item.item_name, ".") == 0 || strcmp(item.item_name, "..") == 0) {
                    continue;
                }
                struct pseudo_inode item_inode;
                long cur_pos = ftell(f);
                read_inode(f, &sb, item.inode, &item_inode);
                printf("%s: %s\n", item_inode.isDirectory ? "DIR" : "FILE", item.item_name);
                fseek(f, cur_pos, SEEK_SET);
            }
        }
    }
    fclose(f);
}


// --- MKDIR ---
int fs_mkdir(const char *filename, const char *path) {
    FILE *f = fopen(filename, "rb+");
    if (!f) return 0;
    struct superblock sb;
    load_superblock(f, &sb);

    char *path_copy = strdup(path);
    char *last_slash = strrchr(path_copy, '/');
    char parent_path[256] = "";
    char new_name[128] = "";

    if (last_slash == NULL) { strcpy(parent_path, "/"); strncpy(new_name, path, 11); }
    else if (last_slash == path_copy) { strcpy(parent_path, "/"); strncpy(new_name, last_slash + 1, 11); }
    else { *last_slash = '\0'; strcpy(parent_path, path_copy); strncpy(new_name, last_slash + 1, 11); }
    free(path_copy);

    if (strlen(new_name) > MAX_NAME_LEN) { fclose(f); return 0; }

    int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) { printf("PATH NOT FOUND\n"); fclose(f); return 0; }
    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) { printf("EXIST\n"); fclose(f); return 0; }

    int free_inode = find_free_bit(f, &sb, true);
    int free_block = find_free_bit(f, &sb, false);
    if (free_inode == -1 || free_block == -1) { printf("NO SPACE\n"); fclose(f); return 0; }

    set_bit(f, &sb, true, free_inode, true);
    set_bit(f, &sb, false, free_block, true);

    struct pseudo_inode new_inode = {0};
    new_inode.nodeid = free_inode;
    new_inode.isDirectory = true;
    new_inode.references = 1;
    new_inode.file_size = sb.cluster_size;
    new_inode.direct1 = free_block;
    new_inode.direct2 = CLUSTER_UNUSED; new_inode.direct3 = CLUSTER_UNUSED;
    new_inode.direct4 = CLUSTER_UNUSED; new_inode.direct5 = CLUSTER_UNUSED;
    new_inode.indirect1 = CLUSTER_UNUSED; new_inode.indirect2 = CLUSTER_UNUSED;

    write_inode(f, &sb, free_inode, &new_inode);

    // Init dir data
    struct directory_item dot = { .inode = free_inode }; strcpy(dot.item_name, ".");
    struct directory_item dotdot = { .inode = parent_id }; strcpy(dotdot.item_name, "..");
    
    long data_addr = sb.data_start_address + (free_block * sb.cluster_size);
    fseek(f, data_addr, SEEK_SET);
    uint8_t *zeros = calloc(1, sb.cluster_size);
    fwrite(zeros, 1, sb.cluster_size, f);
    free(zeros);
    
    fseek(f, data_addr, SEEK_SET);
    fwrite(&dot, sizeof(dot), 1, f);
    fwrite(&dotdot, sizeof(dotdot), 1, f);

    struct directory_item new_entry = { .inode = free_inode };
    strcpy(new_entry.item_name, new_name);
    add_directory_item(f, &sb, parent_id, &new_entry);

    fclose(f);
    return 1;
}

int fs_rmdir(const char *filename, const char *path) {
    FILE *f = fopen(filename, "rb+");
    if (!f) return 0;
    struct superblock sb;
    load_superblock(f, &sb);

    char parent_path[256];
    char name[128];
    parse_path(path, parent_path, name);

    int parent_id = fs_path_to_inode(filename, parent_path);
    int inode_id = find_inode_in_dir(f, &sb, parent_id, name);

    if (parent_id == -1 || inode_id == -1) {
        printf("FILE NOT FOUND\n");
        fclose(f); return 0;
    }

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);

    if (!inode.isDirectory) {
        printf("FILE NOT FOUND (Not a directory)\n"); // Nebo syntax error
        fclose(f); return 0;
    }

    if (!is_dir_empty(f, &sb, inode_id)) {
        printf("NOT EMPTY\n");
        fclose(f); return 0;
    }

    // Smazat z rodiče a uvolnit
    remove_directory_item(f, &sb, parent_id, name);
    free_inode_resources(f, &sb, inode_id);

    fclose(f);
    return 1;
}