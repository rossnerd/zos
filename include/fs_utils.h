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

// Odstraní položku (podle jména) z adresáře
// Vrací 1 (úspěch), 0 (chyba/nenalezeno)
int remove_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, char *name);

// Zkontroluje, zda je adresář prázdný (obsahuje jen "." a "..")
// Vrací 1 (je prázdný), 0 (není prázdný)
int is_dir_empty(FILE *f, struct superblock *sb, int inode_id);

// Uvolní datové bloky a samotný inode (používá se pro rm/rmdir)
void free_inode_resources(FILE *f, struct superblock *sb, int inode_id);

// Odstraní položku (podle jména) z adresáře
// Vrací 1 (úspěch), 0 (chyba/nenalezeno)
int remove_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, char *name);

// Zkontroluje, zda je adresář prázdný (obsahuje jen "." a "..")
// Vrací 1 (je prázdný), 0 (není prázdný)
int is_dir_empty(FILE *f, struct superblock *sb, int inode_id);

// Uvolní datové bloky a samotný inode (používá se pro rm/rmdir)
void free_inode_resources(FILE *f, struct superblock *sb, int inode_id);

#endif