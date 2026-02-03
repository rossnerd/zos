#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>
#include <stdbool.h>

// ID pro volnou položku
#define ID_ITEM_FREE 0
#define CLUSTER_UNUSED -1
// Konstanty pro velikosti
#define CLUSTER_SIZE 1024   // Pevná velikost clusteru (zjednoduší výpočty)
#define MAX_NAME_LEN 12     // 8+3 + \0

// Superblock [cite: 16-20]
struct superblock {
    char signature[9];              // login autora FS
    char volume_descriptor[251];    // popis vygenerovaného FS
    int32_t disk_size;              // celkova velikost VFS
    int32_t cluster_size;           // velikost clusteru
    int32_t cluster_count;          // pocet clusteru
    int32_t bitmapi_start_address;  // adresa pocatku bitmapy i-uzlů
    int32_t bitmap_start_address;   // adresa pocatku bitmapy datových bloků
    int32_t inode_start_address;    // adresa pocatku i-uzlů
    int32_t data_start_address;     // adresa pocatku datovych bloku
};

// I-uzel [cite: 21-29]
struct pseudo_inode {
    int32_t nodeid;                 // ID i-uzlu
    bool isDirectory;               // soubor nebo adresar
    int8_t references;              // počet odkazů na i-uzel
    int32_t file_size;              // velikost souboru v bytech
    int32_t direct1;                // 1. přímý odkaz
    int32_t direct2;                // 2. přímý odkaz
    int32_t direct3;                // 3. přímý odkaz
    int32_t direct4;                // 4. přímý odkaz
    int32_t direct5;                // 5. přímý odkaz
    int32_t indirect1;              // 1. nepřímý odkaz
    int32_t indirect2;              // 2. nepřímý odkaz
};

// Položka adresáře [cite: 29-30]
struct directory_item {
    int32_t inode;                  // inode odpovídající souboru
    char item_name[12];             // 8+3 + \0
};

#endif // STRUCTS_H