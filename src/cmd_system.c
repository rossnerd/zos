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

static int bit_is_set(const uint8_t *bm, int idx) {
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

static long count_set_bits_upto(const uint8_t *bm, long n_bits) {
    long c = 0;
    for (long i = 0; i < n_bits; i++) {
        c += bit_is_set(bm, (int)i);
    }
    return c;
}

void fs_statfs(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }

    struct superblock sb;
    if (!load_superblock(f, &sb)) { fclose(f); printf("FILE NOT FOUND\n"); return; }

    // skutečné počty dle rozložení ve VFS
    long inode_count = (sb.data_start_address - sb.inode_start_address) / (long)sizeof(struct pseudo_inode);
    long data_cluster_count = (sb.disk_size - sb.data_start_address) / (long)sb.cluster_size;

    long inode_bm_bytes = sb.bitmap_start_address - sb.bitmapi_start_address;
    long data_bm_bytes  = sb.inode_start_address - sb.bitmap_start_address;

    if (inode_bm_bytes <= 0 || data_bm_bytes <= 0 || inode_count < 0 || data_cluster_count < 0) {
        fclose(f);
        printf("FILE NOT FOUND\n");
        return;
    }

    uint8_t *ibm = (uint8_t*)malloc((size_t)inode_bm_bytes);
    uint8_t *dbm = (uint8_t*)malloc((size_t)data_bm_bytes);
    if (!ibm || !dbm) {
        free(ibm); free(dbm);
        fclose(f);
        printf("FILE NOT FOUND\n");
        return;
    }

    fseek(f, sb.bitmapi_start_address, SEEK_SET);
    fread(ibm, 1, (size_t)inode_bm_bytes, f);

    fseek(f, sb.bitmap_start_address, SEEK_SET);
    fread(dbm, 1, (size_t)data_bm_bytes, f);

    long used_inodes = count_set_bits_upto(ibm, inode_count);
    long used_blocks = count_set_bits_upto(dbm, data_cluster_count);

    long free_inodes = inode_count - used_inodes;
    long free_blocks = data_cluster_count - used_blocks;

    // počet adresářů (projdeme pouze obsazené i-uzly)
    long dir_count = 0;
    for (long i = 0; i < inode_count; i++) {
        if (!bit_is_set(ibm, (int)i)) continue;
        struct pseudo_inode ino;
        read_inode(f, &sb, (int)i, &ino);
        if (ino.isDirectory) dir_count++;
    }

    printf("--- STATFS ---\n");
    printf("Disk: %d B\n", sb.disk_size);
    printf("Cluster: %d B\n", sb.cluster_size);
    printf("Inodes: %ld used, %ld free\n", used_inodes, free_inodes);
    printf("Blocks: %ld used, %ld free\n", used_blocks, free_blocks);
    printf("Directories: %ld\n", dir_count);

    free(ibm);
    free(dbm);
    fclose(f);
}

static void fs_info_print(const char *name, const struct pseudo_inode *inode) {
    // Název – velikost – i-uzel – odkazy (přímé + nepřímé)
    // (Formát je zvolen tak, aby odpovídal zadání: jméno + size + inode + seznam odkazů)
    printf("%s - %d B - i-node %d\n", name, inode->file_size, inode->nodeid);

    // přímé odkazy
    printf("direct: ");
    int first = 1;
    int32_t d[5] = {inode->direct1, inode->direct2, inode->direct3, inode->direct4, inode->direct5};
    for (int i = 0; i < 5; i++) {
        if (d[i] == CLUSTER_UNUSED) continue;
        if (!first) printf(", ");
        printf("%d", d[i]);
        first = 0;
    }
    if (first) printf("-");
    printf("\n");

    // nepřímé odkazy (u nás jsou to ukazatele na nepřímé bloky; jejich obsah lze případně rozšířit)
    printf("indirect1: %d\n", inode->indirect1 == CLUSTER_UNUSED ? -1 : inode->indirect1);
    printf("indirect2: %d\n", inode->indirect2 == CLUSTER_UNUSED ? -1 : inode->indirect2);
}

void fs_info(const char *filename, int inode_id) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }

    struct superblock sb;
    load_superblock(f, &sb);

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);

    // bez jména – fallback (používej spíš fs_info_path)
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "inode%d", inode.nodeid);
    fs_info_print(tmp, &inode);

    fclose(f);
}

void fs_info_path(const char *filename, const char *path) {
    int inode_id = fs_path_to_inode(filename, path);
    if (inode_id == -1) {
        printf("PATH NOT FOUND\n");
        return;
    }

    // basename pro NAME
    const char *name = path;
    const char *slash = strrchr(path, '/');
    if (slash) name = slash + 1;
    if (!name || name[0] == '\0') name = "/";

    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }
    struct superblock sb;
    load_superblock(f, &sb);
    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);
    fclose(f);

    fs_info_print(name, &inode);
}

