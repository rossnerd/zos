#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../include/fs_core.h"

// Pomocná funkce pro parsování velikosti (např. "10MB" -> 10485760)
long parse_size(const char *size_str) {
    long size = atol(size_str);
    char unit = toupper(size_str[strlen(size_str) - 2]); // Předposlední znak (M v MB)
    
    if (strstr(size_str, "KB") || strstr(size_str, "kB")) {
        size *= 1024;
    } else if (strstr(size_str, "MB")) {
        size *= 1024 * 1024;
    }
    // Pokud je jen číslo, bereme jako Byty
    return size;
}

int fs_format(const char *filename, const char *size_str) {
    long disk_size = parse_size(size_str);
    
    FILE *f = fopen(filename, "wb+");
    if (!f) {
        perror("Nelze otevřít soubor pro formátování");
        return 0;
    }

    // 1. Nastavení Superblocku
    struct superblock sb;
    memset(&sb, 0, sizeof(sb));
    strncpy(sb.signature, "r-login", 9); // Změňte na svůj login!
    strncpy(sb.volume_descriptor, "Semestralni prace ZOS 2025", 250);
    
    sb.disk_size = (int32_t)disk_size;
    sb.cluster_size = CLUSTER_SIZE;
    
    // Spočítáme maximální počet clusterů, které se vejdou
    // Toto je zjednodušený výpočet. V reálu bychom odečetli velikost metadat.
    // Pro školní účely často stačí: počet clusterů = velikost disku / velikost clusteru
    sb.cluster_count = sb.disk_size / sb.cluster_size;
    
    // Výpočet adres
    sb.bitmapi_start_address = sizeof(struct superblock);
    
    // Velikost bitmapy inodů (v bytech) = počet inodů / 8
    // Předpokládáme max 1 inode na cluster
    int inode_bitmap_size = (sb.cluster_count + 7) / 8;
    
    sb.bitmap_start_address = sb.bitmapi_start_address + inode_bitmap_size;
    
    // Velikost bitmapy dat (v bytech)
    int data_bitmap_size = (sb.cluster_count + 7) / 8;
    
    sb.inode_start_address = sb.bitmap_start_address + data_bitmap_size;
    
    // Tabulka inodů
    long inodes_area_size = sb.cluster_count * sizeof(struct pseudo_inode);
    
    sb.data_start_address = sb.inode_start_address + inodes_area_size;

    // Kontrola, zda se metadata vejdou před data_start_address
    if (sb.data_start_address >= sb.disk_size) {
        printf("Chyba: Disk je příliš malý pro formátování.\n");
        fclose(f);
        return 0;
    }

    printf("Formátování FS:\nVelikost: %d B\nClusterů: %d\nData start: %d\n", 
           sb.disk_size, sb.cluster_count, sb.data_start_address);

    // 2. Vynulování souboru (nebo alespoň oblasti metadat)
    // Pro rychlost stačí zapsat metadata a zbytek nechat být, 
    // ale čistší je udělat ftruncate nebo zapsat nuly.
    // Zde zapíšeme Superblock
    fwrite(&sb, sizeof(sb), 1, f);

    // 3. Vynulování bitmap
    void *empty_bitmap = calloc(1, inode_bitmap_size > data_bitmap_size ? inode_bitmap_size : data_bitmap_size);
    
    // Zápis bitmapy inodů (vše 0 - volné)
    // POZOR: Root adresář bude inode 0. Musíme ho označit jako obsazený.
    uint8_t *ibitmap = (uint8_t*)calloc(1, inode_bitmap_size);
    ibitmap[0] |= 1; // Nastavíme 1. bit (inode 0 je obsazený)
    fseek(f, sb.bitmapi_start_address, SEEK_SET);
    fwrite(ibitmap, 1, inode_bitmap_size, f);
    free(ibitmap);

    // Zápis bitmapy dat (vše 0 - volné)
    // Root adresář bude potřebovat 1 datový cluster (cluster 0). Označíme ho.
    uint8_t *dbitmap = (uint8_t*)calloc(1, data_bitmap_size);
    dbitmap[0] |= 1; // Nastavíme 1. bit (cluster 0 je obsazený)
    fseek(f, sb.bitmap_start_address, SEEK_SET);
    fwrite(dbitmap, 1, data_bitmap_size, f);
    free(dbitmap);

    // 4. Inicializace Inodů
    // Zapíšeme Root Inode na index 0
    struct pseudo_inode root_inode;
    memset(&root_inode, 0, sizeof(struct pseudo_inode));
    root_inode.nodeid = 0;
    root_inode.isDirectory = true;
    root_inode.references = 1;
    root_inode.file_size = sb.cluster_size; // Alokujeme mu celý cluster
    root_inode.direct1 = 0; // Ukazuje na datový cluster 0
    // Ostatní direct/indirect jsou 0 (ID_ITEM_FREE)
    
    fseek(f, sb.inode_start_address, SEEK_SET);
    fwrite(&root_inode, sizeof(root_inode), 1, f);

    // Zbytek tabulky inodů vynulujeme (abychom tam neměli smetí)
    struct pseudo_inode empty_inode = {0}; // Všechny položky 0
    // Optimalizace: zapisovat po blocích, ne po jednom, ale pro přehlednost:
    for(int i=1; i < sb.cluster_count; i++) {
         fwrite(&empty_inode, sizeof(empty_inode), 1, f);
    }

    // 5. Inicializace Dat Root adresáře
    // Root musí obsahovat "." a ".."
    struct directory_item self = { .inode = 0 };
    strcpy(self.item_name, ".");
    
    struct directory_item parent = { .inode = 0 }; // Root nemá rodiče, odkazuje na sebe
    strcpy(parent.item_name, "..");

    fseek(f, sb.data_start_address, SEEK_SET); // Jdeme na začátek datové oblasti (cluster 0)
    fwrite(&self, sizeof(struct directory_item), 1, f);
    fwrite(&parent, sizeof(struct directory_item), 1, f);

    // Zbytek clusteru vyplníme nulami (důležité pro detekci konce adresáře)
    char zeros[CLUSTER_SIZE - 2*sizeof(struct directory_item)];
    memset(zeros, 0, sizeof(zeros));
    fwrite(zeros, 1, sizeof(zeros), f);

    // Nakonec nastavíme velikost souboru na disku
    fseek(f, disk_size - 1, SEEK_SET);
    fputc(0, f);

    fclose(f);
    return 1;
}