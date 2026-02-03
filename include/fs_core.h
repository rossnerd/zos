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

#endif // FS_CORE_H