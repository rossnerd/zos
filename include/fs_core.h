#ifndef FS_CORE_H
#define FS_CORE_H

#include <stdio.h>
#include "structs.h"

// Funkce pro formátování disku
// size_str může být "100KB", "10MB" atd.
int fs_format(const char *filename, const char *size_str);

// Vypíše statistiky FS (příkaz statfs)
void fs_statfs(const char *filename);

// Vypíše obsah adresáře (příkaz ls)
// Prozatím vypíše jen kořenový adresář, pokud path == NULL nebo "/"
void fs_ls(const char *filename, int inode_id);

// Vypíše informace o inodu/souboru (příkaz info)
void fs_info(const char *filename, int inode_id);

// info nad cestou (kvůli interaktivnímu režimu)
void fs_info_path(const char *filename, const char *path);

// Najde volný bit v bitmapě (pro inody nebo clustery)
// Vrací index (0..N) nebo -1, pokud je plno.
int find_free_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap);

// Označí bit jako obsazený (1) nebo volný (0)
void set_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap, int index, bool status);

// Hlavní funkce pro překlad cesty na ID inodu.
// Vrací ID inodu nebo -1, pokud cesta neexistuje.
int fs_path_to_inode(const char *filename, const char *path);

// Vytvoří nový adresář
// Vrací 1 při úspěchu, 0 při chybě (např. plný disk, existuje, nenalezen rodič)
int fs_mkdir(const char *filename, const char *path);

// Importuje soubor z Host OS do VFS
// incp <host_path> <vfs_path>
int fs_incp(const char *filename, const char *host_path, const char *vfs_path);

// Export souboru z VFS do Host OS
// outcp <vfs_path> <host_path>
int fs_outcp(const char *filename, const char *vfs_path, const char *host_path);

// Spojí soubory s1 a s2 do nového souboru s3 (xcp s1 s2 s3)
int fs_xcp(const char *filename, const char *s1, const char *s2, const char *s3);

// Přidá obsah s2 na konec s1 (add s1 s2)
int fs_add(const char *filename, const char *s1, const char *s2);

int fs_cat(const char *filename, const char *path);

int fs_rm(const char *filename, const char *path);

int fs_rmdir(const char *filename, const char *path);

int fs_cp(const char *filename, const char *s1, const char *s2);

int fs_mv(const char *filename, const char *s1, const char *s2);

#endif // FS_CORE_H