#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../include/fs_core.h"
#include "../include/fs_utils.h"

// --- FORMAT ---
long parse_size(const char *size_str) {
    long size = atol(size_str);
    if (strstr(size_str, "KB") || strstr(size_str, "kB")) size *= 1024;
    else if (strstr(size_str, "MB")) size *= 1024 * 1024;
    return size;
}

int fs_format(const char *filename, const char *size_str) {
    long disk_size = parse_size(size_str);
    FILE *f = fopen(filename, "wb+");
    if (!f) return 0;

    struct superblock sb = {0};
    strncpy(sb.signature, "r-login", 9); 
    strncpy(sb.volume_descriptor, "Semestralni prace ZOS 2025", 250);
    sb.disk_size = (int32_t)disk_size;
    sb.cluster_size = CLUSTER_SIZE;
    sb.cluster_count = sb.disk_size / sb.cluster_size;
    
    sb.bitmapi_start_address = sizeof(struct superblock);
    int inode_bitmap_size = (sb.cluster_count + 7) / 8;
    sb.bitmap_start_address = sb.bitmapi_start_address + inode_bitmap_size;
    int data_bitmap_size = (sb.cluster_count + 7) / 8;
    sb.inode_start_address = sb.bitmap_start_address + data_bitmap_size;
    long inodes_area_size = sb.cluster_count * sizeof(struct pseudo_inode);
    sb.data_start_address = sb.inode_start_address + inodes_area_size;

    if (sb.data_start_address >= sb.disk_size) { fclose(f); return 0; }

    fwrite(&sb, sizeof(sb), 1, f);

    // Bitmapy
    uint8_t *ibitmap = calloc(1, inode_bitmap_size);
    ibitmap[0] |= 1; 
    fseek(f, sb.bitmapi_start_address, SEEK_SET);
    fwrite(ibitmap, 1, inode_bitmap_size, f);
    free(ibitmap);

    uint8_t *dbitmap = calloc(1, data_bitmap_size);
    dbitmap[0] |= 1;
    fseek(f, sb.bitmap_start_address, SEEK_SET);
    fwrite(dbitmap, 1, data_bitmap_size, f);
    free(dbitmap);

    // Inody
    struct pseudo_inode root_inode = {0};
    root_inode.nodeid = 0;
    root_inode.isDirectory = true;
    root_inode.references = 1;
    root_inode.file_size = sb.cluster_size;
    root_inode.direct1 = 0;
    root_inode.direct2 = CLUSTER_UNUSED; root_inode.direct3 = CLUSTER_UNUSED;
    root_inode.direct4 = CLUSTER_UNUSED; root_inode.direct5 = CLUSTER_UNUSED;
    root_inode.indirect1 = CLUSTER_UNUSED; root_inode.indirect2 = CLUSTER_UNUSED;

    fseek(f, sb.inode_start_address, SEEK_SET);
    fwrite(&root_inode, sizeof(root_inode), 1, f);

    struct pseudo_inode empty_inode = {0};
    empty_inode.direct1 = CLUSTER_UNUSED; empty_inode.direct2 = CLUSTER_UNUSED;
    empty_inode.direct3 = CLUSTER_UNUSED; empty_inode.direct4 = CLUSTER_UNUSED;
    empty_inode.direct5 = CLUSTER_UNUSED; empty_inode.indirect1 = CLUSTER_UNUSED;
    empty_inode.indirect2 = CLUSTER_UNUSED;

    for(int i=1; i < sb.cluster_count; i++) fwrite(&empty_inode, sizeof(empty_inode), 1, f);

    // Root data
    struct directory_item self = { .inode = 0 }; strcpy(self.item_name, ".");
    struct directory_item parent = { .inode = 0 }; strcpy(parent.item_name, "..");

    fseek(f, sb.data_start_address, SEEK_SET);
    fwrite(&self, sizeof(struct directory_item), 1, f);
    fwrite(&parent, sizeof(struct directory_item), 1, f);

    // Vyplnění zbytku clusteru nulami
    char zeros[CLUSTER_SIZE - 2*sizeof(struct directory_item)];
    memset(zeros, 0, sizeof(zeros));
    fwrite(zeros, 1, sizeof(zeros), f);

    fseek(f, disk_size - 1, SEEK_SET);
    fputc(0, f);
    fclose(f);
    return 1;
}

// --- LS ---
void fs_ls(const char *filename, int inode_id) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }

    struct superblock sb;
    load_superblock(f, &sb);

    struct pseudo_inode dir_inode;
    read_inode(f, &sb, inode_id, &dir_inode);

    if (!dir_inode.isDirectory) {
        printf("PATH NOT FOUND (Not a directory)\n"); 
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

// --- STATFS, INFO ---
void fs_statfs(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }
    struct superblock sb;
    load_superblock(f, &sb);
    
    // Zjednodušený výpočet pro ukázku - ve finále by se měla projít celá bitmapa
    // Zde jen vypíšeme metadata
    printf("--- STATFS ---\nDisk: %d B\nCluster: %d B\nCount: %d\n", sb.disk_size, sb.cluster_size, sb.cluster_count);
    fclose(f);
}

void fs_info(const char *filename, int inode_id) {
    FILE *f = fopen(filename, "rb");
    if (!f) return;
    struct superblock sb;
    load_superblock(f, &sb);
    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);
    printf("ID: %d, Size: %d, Type: %s\n", inode.nodeid, inode.file_size, inode.isDirectory ? "DIR" : "FILE");
    printf("Blocks: %d %d %d %d %d\n", inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5);
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

// --- INCP ---
int fs_incp(const char *filename, const char *host_path, const char *vfs_path) {
    FILE *host_f = fopen(host_path, "rb");
    if (!host_f) { printf("FILE NOT FOUND (host)\n"); return 0; }
    fseek(host_f, 0, SEEK_END);
    long file_size = ftell(host_f);
    fseek(host_f, 0, SEEK_SET);

    if (file_size > 5 * CLUSTER_SIZE) { printf("TOO BIG\n"); fclose(host_f); return 0; }

    FILE *f = fopen(filename, "rb+");
    if (!f) { fclose(host_f); return 0; }
    struct superblock sb;
    load_superblock(f, &sb);

    // Parse path (reuse logic or extract to helper)
    char *path_copy = strdup(vfs_path);
    char *last_slash = strrchr(path_copy, '/');
    char parent_path[256] = "";
    char new_name[128] = "";
    if (last_slash == NULL) { strcpy(parent_path, "/"); strncpy(new_name, vfs_path, 11); }
    else if (last_slash == path_copy) { strcpy(parent_path, "/"); strncpy(new_name, last_slash + 1, 11); }
    else { *last_slash = '\0'; strcpy(parent_path, path_copy); strncpy(new_name, last_slash + 1, 11); }
    free(path_copy);

    int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) { printf("PATH NOT FOUND\n"); fclose(host_f); fclose(f); return 0; }
    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) { printf("EXIST\n"); fclose(host_f); fclose(f); return 0; }

    int free_inode = find_free_bit(f, &sb, true);
    if (free_inode == -1) { printf("NO SPACE\n"); fclose(host_f); fclose(f); return 0; }
    set_bit(f, &sb, true, free_inode, true);

    int32_t blocks[5] = {CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED};
    int bytes_rem = file_size;
    int b_idx = 0;
    uint8_t buffer[CLUSTER_SIZE];

    while (bytes_rem > 0 && b_idx < 5) {
        int free_block = find_free_bit(f, &sb, false);
        if (free_block == -1) { printf("NO SPACE\n"); fclose(host_f); fclose(f); return 0; }
        set_bit(f, &sb, false, free_block, true);
        blocks[b_idx++] = free_block;
        
        memset(buffer, 0, CLUSTER_SIZE);
        int to_read = (bytes_rem > CLUSTER_SIZE) ? CLUSTER_SIZE : bytes_rem;
        fread(buffer, 1, to_read, host_f);
        
        fseek(f, sb.data_start_address + (free_block * sb.cluster_size), SEEK_SET);
        fwrite(buffer, 1, CLUSTER_SIZE, f);
        bytes_rem -= to_read;
    }

    struct pseudo_inode new_inode = {0};
    new_inode.nodeid = free_inode;
    new_inode.file_size = file_size;
    new_inode.references = 1;
    new_inode.direct1 = blocks[0]; new_inode.direct2 = blocks[1];
    new_inode.direct3 = blocks[2]; new_inode.direct4 = blocks[3]; new_inode.direct5 = blocks[4];
    new_inode.indirect1 = CLUSTER_UNUSED; new_inode.indirect2 = CLUSTER_UNUSED;
    
    write_inode(f, &sb, free_inode, &new_inode);

    struct directory_item new_entry = { .inode = free_inode };
    strcpy(new_entry.item_name, new_name);
    add_directory_item(f, &sb, parent_id, &new_entry);

    fclose(host_f); fclose(f);
    return 1;
}