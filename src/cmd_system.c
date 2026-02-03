#define _POSIX_C_SOURCE 200809L // Kvůli strdup
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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