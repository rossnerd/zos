#ifndef FS_UTILS_H
#define FS_UTILS_H

#include <stdio.h>
#include "structs.h"
#include <stdbool.h>

// --- I/O Superblock & Inode ---
int load_superblock(FILE *f, struct superblock *sb);
void read_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode);
void write_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode);

// --- Bitmapy ---
int find_free_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap);
void set_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap, int index, bool status);

// --- Práce s adresáři a cestami ---
int find_inode_in_dir(FILE *f, struct superblock *sb, int parent_inode_id, char *name);
int add_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, struct directory_item *new_item);
int fs_path_to_inode(const char *filename, const char *path);

#endif