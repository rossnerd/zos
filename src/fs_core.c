#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../include/fs_core.h"

// --- PROTOTYPY INTERNÍCH FUNKCÍ (aby nezáleželo na pořadí v souboru) ---
int load_superblock(FILE *f, struct superblock *sb);
void read_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode);
int find_inode_in_dir(FILE *f, struct superblock *sb, int parent_inode_id, char *name);
int find_free_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap);
void set_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap, int index, bool status);
int add_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, struct directory_item *new_item);

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
    struct pseudo_inode root_inode;
    memset(&root_inode, 0, sizeof(struct pseudo_inode));
    root_inode.nodeid = 0;
    root_inode.isDirectory = true;
    root_inode.references = 1;
    root_inode.file_size = sb.cluster_size; 
    
    // NASTAVENÍ ODKAZŮ
    root_inode.direct1 = 0; // První cluster (0) je náš
    root_inode.direct2 = CLUSTER_UNUSED;
    root_inode.direct3 = CLUSTER_UNUSED;
    root_inode.direct4 = CLUSTER_UNUSED;
    root_inode.direct5 = CLUSTER_UNUSED;
    root_inode.indirect1 = CLUSTER_UNUSED;
    root_inode.indirect2 = CLUSTER_UNUSED;
    
    fseek(f, sb.inode_start_address, SEEK_SET);
    fwrite(&root_inode, sizeof(root_inode), 1, f);

    // Zbytek tabulky inodů - musíme nastavit pointery na -1
    struct pseudo_inode empty_inode;
    memset(&empty_inode, 0, sizeof(empty_inode));
    // Inicializace "prázdného" inodu, aby neměl pointery na cluster 0
    empty_inode.direct1 = CLUSTER_UNUSED;
    empty_inode.direct2 = CLUSTER_UNUSED;
    empty_inode.direct3 = CLUSTER_UNUSED;
    empty_inode.direct4 = CLUSTER_UNUSED;
    empty_inode.direct5 = CLUSTER_UNUSED;
    empty_inode.indirect1 = CLUSTER_UNUSED;
    empty_inode.indirect2 = CLUSTER_UNUSED;

    for(int i=1; i < sb.cluster_count; i++) { // Pozor: i < pocet inodu, ne clusteru (zjednodušení)
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

// Načte superblock ze souboru. Vrací 1 při úspěchu, 0 při chybě.
int load_superblock(FILE *f, struct superblock *sb) {
    fseek(f, 0, SEEK_SET);
    if (fread(sb, sizeof(struct superblock), 1, f) != 1) {
        return 0;
    }
    return 1;
}

// Spočítá počet jedniček v poli bajtů (pro bitmapy)
int count_set_bits(const uint8_t *bitmap, int size_in_bytes) {
    int count = 0;
    for (int i = 0; i < size_in_bytes; i++) {
        uint8_t b = bitmap[i];
        for (int j = 0; j < 8; j++) {
            if ((b >> j) & 1) count++;
        }
    }
    return count;
}

// --- POMOCNÉ FUNKCE PRO ZÁPIS ---

// Přidá položku (název + inode) do adresáře (parent_inode_id).
// Vrací 1 pokud OK, 0 pokud je adresář plný nebo nastala chyba.
int add_directory_item(FILE *f, struct superblock *sb, int parent_inode_id, struct directory_item *new_item) {
    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);

    if (!parent.isDirectory) return 0;

    int32_t blocks[] = {parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5};
    struct directory_item item;
    int items_per_cluster = sb->cluster_size / sizeof(struct directory_item);

    // Hledáme volné místo v existujících clusterech
    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) {
            // Tady by měla být logika pro alokaci nového clusteru pro RODIČE,
            // pokud jsou stávající plné. Pro zjednodušení semestrální práce
            // to zatím vynecháme a předpokládáme, že se vejdeme do existujících.
            // (Root má 1 cluster = 64 položek, to stačí).
            continue; 
        }

        long cluster_offset = sb->data_start_address + (blocks[i] * sb->cluster_size);
        
        for (int j = 0; j < items_per_cluster; j++) {
            long item_pos = cluster_offset + (j * sizeof(struct directory_item));
            fseek(f, item_pos, SEEK_SET);
            fread(&item, sizeof(struct directory_item), 1, f);

            // Našli jsme volné místo? (prázdný název)
            if (item.item_name[0] == '\0') {
                // Zapíšeme novou položku ZDE
                fseek(f, item_pos, SEEK_SET);
                fwrite(new_item, sizeof(struct directory_item), 1, f);
                return 1; // Úspěch
            }
        }
    }
    return 0; // Adresář je plný (všech 5 clusterů nebo alokované clustery)
}

// --- HLAVNÍ FUNKCE MKDIR ---

int fs_mkdir(const char *filename, const char *path) {
    FILE *f = fopen(filename, "rb+"); // Otevíráme pro čtení I zápis
    if (!f) return 0;

    struct superblock sb;
    load_superblock(f, &sb);

    // 1. Rozparsování cesty na rodiče a název nového adresáře
    // Příklad: "/home/user" -> parent="/home", name="user"
    // Příklad: "/test" -> parent="/", name="test"
    
    char *path_copy = strdup(path);
    char *last_slash = strrchr(path_copy, '/');
    char parent_path[256] = "";
    char new_name[128] = "";

    if (last_slash == NULL) {
        // Cesta bez lomítka (např. "sloz1") -> rodič je root (nebo aktuální, my bereme root)
        strcpy(parent_path, "/");
        strncpy(new_name, path, 11);
    } else if (last_slash == path_copy) {
        // Cesta "/sloz1" -> rodič je "/"
        strcpy(parent_path, "/");
        strncpy(new_name, last_slash + 1, 11);
    } else {
        // Cesta "/a/b" -> rodič "/a"
        *last_slash = '\0'; // Useknete string na pozici lomítka
        strcpy(parent_path, path_copy);
        strncpy(new_name, last_slash + 1, 11);
    }
    free(path_copy); // Uvolníme pomocný buffer

    // Ověření délky jména
    if (strlen(new_name) > MAX_NAME_LEN) {
        printf("FILENAME TOO LONG\n");
        fclose(f);
        return 0;
    }

    // 2. Získání ID rodiče
    int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) {
        printf("PATH NOT FOUND\n");
        fclose(f);
        return 0;
    }

    // 3. Ověření, zda složka už neexistuje
    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) {
        printf("EXIST\n");
        fclose(f);
        return 0;
    }

    // 4. Nalezení volných zdrojů (Inode + Data Cluster)
    int free_inode_id = find_free_bit(f, &sb, true); // true = inode map
    int free_block_id = find_free_bit(f, &sb, false); // false = data map

    if (free_inode_id == -1 || free_block_id == -1) {
        printf("NO SPACE (Inodes or Blocks full)\n");
        fclose(f);
        return 0;
    }

    // 5. Zápis do FS
    // A) Označíme bity jako obsazené
    set_bit(f, &sb, true, free_inode_id, true);
    set_bit(f, &sb, false, free_block_id, true);

    // B) Inicializace nového Inodu
    struct pseudo_inode new_inode = {0};
    new_inode.nodeid = free_inode_id;
    new_inode.isDirectory = true;
    new_inode.references = 1;
    new_inode.file_size = sb.cluster_size;
    new_inode.direct1 = free_block_id;
    new_inode.direct2 = CLUSTER_UNUSED;
    new_inode.direct3 = CLUSTER_UNUSED;
    new_inode.direct4 = CLUSTER_UNUSED;
    new_inode.direct5 = CLUSTER_UNUSED;
    new_inode.indirect1 = CLUSTER_UNUSED;
    new_inode.indirect2 = CLUSTER_UNUSED;

    long inode_addr = sb.inode_start_address + (free_inode_id * sizeof(struct pseudo_inode));
    fseek(f, inode_addr, SEEK_SET);
    fwrite(&new_inode, sizeof(new_inode), 1, f);

    // C) Inicializace obsahu adresáře (. a ..)
    struct directory_item dot = { .inode = free_inode_id };
    strcpy(dot.item_name, ".");
    
    struct directory_item dotdot = { .inode = parent_id };
    strcpy(dotdot.item_name, "..");

    long data_addr = sb.data_start_address + (free_block_id * sb.cluster_size);
    fseek(f, data_addr, SEEK_SET);
    // Vynulujeme celý cluster (důležité!)
    uint8_t *zeros = calloc(1, sb.cluster_size);
    fwrite(zeros, 1, sb.cluster_size, f);
    free(zeros);
    
    // Zapíšeme . a ..
    fseek(f, data_addr, SEEK_SET);
    fwrite(&dot, sizeof(dot), 1, f);
    fwrite(&dotdot, sizeof(dotdot), 1, f);

    // D) Přidání záznamu do rodičovského adresáře
    struct directory_item new_entry;
    new_entry.inode = free_inode_id;
    strcpy(new_entry.item_name, new_name);

    if (!add_directory_item(f, &sb, parent_id, &new_entry)) {
        printf("CANNOT WRITE TO PARENT DIR (Full?)\n");
        // Tady by měl být rollback (uvolnit bity), ale pro semestrální práci stačí error.
        fclose(f);
        return 0;
    }

    fclose(f);
    return 1;
}

// --- IMPLEMENTACE PŘÍKAZŮ ---

void fs_statfs(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }

    struct superblock sb;
    if (!load_superblock(f, &sb)) { fclose(f); return; }

    // Načtení bitmap
    int inode_bitmap_len = sb.bitmap_start_address - sb.bitmapi_start_address;
    int data_bitmap_len = sb.inode_start_address - sb.bitmap_start_address;

    uint8_t *ibitmap = malloc(inode_bitmap_len);
    uint8_t *dbitmap = malloc(data_bitmap_len);

    fseek(f, sb.bitmapi_start_address, SEEK_SET);
    fread(ibitmap, 1, inode_bitmap_len, f);

    fseek(f, sb.bitmap_start_address, SEEK_SET);
    fread(dbitmap, 1, data_bitmap_len, f);

    int used_inodes = count_set_bits(ibitmap, inode_bitmap_len);
    int used_blocks = count_set_bits(dbitmap, data_bitmap_len);

    // Výpis statistik 
    printf("--- STATFS ---\n");
    printf("Disk Size: %d B\n", sb.disk_size);
    printf("Cluster Size: %d B\n", sb.cluster_size);
    printf("Cluster Count: %d\n", sb.cluster_count);
    printf("Used Inodes: %d\n", used_inodes);
    printf("Used Blocks: %d\n", used_blocks);
    printf("Free Blocks: %d\n", sb.cluster_count - used_blocks);
    
    free(ibitmap);
    free(dbitmap);
    fclose(f);
}

// Pomocná funkce pro čtení inodu
void read_inode(FILE *f, struct superblock *sb, int inode_id, struct pseudo_inode *inode) {
    long offset = sb->inode_start_address + (inode_id * sizeof(struct pseudo_inode));
    fseek(f, offset, SEEK_SET);
    fread(inode, sizeof(struct pseudo_inode), 1, f);
}

// Pomocná funkce: Nastaví/Vynuluje bit v souboru na konkrétní adrese
void set_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap, int index, bool status) {
    long start_addr = is_inode_bitmap ? sb->bitmapi_start_address : sb->bitmap_start_address;
    long byte_offset = index / 8;
    int bit_offset = index % 8;

    fseek(f, start_addr + byte_offset, SEEK_SET);
    
    uint8_t byte;
    fread(&byte, 1, 1, f);

    if (status) {
        byte |= (1 << bit_offset); // Nastaví na 1
    } else {
        byte &= ~(1 << bit_offset); // Nastaví na 0
    }

    fseek(f, -1, SEEK_CUR); // Vrátíme se o byte zpět
    fwrite(&byte, 1, 1, f);
}

// Pomocná funkce: Najde první volný bit (hodnota 0)
int find_free_bit(FILE *f, struct superblock *sb, bool is_inode_bitmap) {
    long start_addr = is_inode_bitmap ? sb->bitmapi_start_address : sb->bitmap_start_address;
    int total_items = sb->cluster_count; // Zjednodušení: počet inodů = počet clusterů
    int size_in_bytes = (total_items + 7) / 8;

    uint8_t *buffer = malloc(size_in_bytes);
    fseek(f, start_addr, SEEK_SET);
    fread(buffer, 1, size_in_bytes, f);

    for (int i = 0; i < total_items; i++) {
        int byte_index = i / 8;
        int bit_index = i % 8;

        if (!((buffer[byte_index] >> bit_index) & 1)) {
            free(buffer);
            return i; // Našli jsme volný index
        }
    }

    free(buffer);
    return -1; // Plno
}

// Hledá název "name" v adresáři "parent_inode_id".
// Vrací ID nalezeného inodu nebo -1.
int find_inode_in_dir(FILE *f, struct superblock *sb, int parent_inode_id, char *name) {
    struct pseudo_inode parent;
    read_inode(f, sb, parent_inode_id, &parent);

    if (!parent.isDirectory) return -1;

    int32_t blocks[] = {parent.direct1, parent.direct2, parent.direct3, parent.direct4, parent.direct5};
    struct directory_item item;
    int items_per_cluster = sb->cluster_size / sizeof(struct directory_item);

    // Projdeme všechny datové bloky rodičovského adresáře
    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) continue;

        fseek(f, sb->data_start_address + (blocks[i] * sb->cluster_size), SEEK_SET);
        
        for (int j = 0; j < items_per_cluster; j++) {
            fread(&item, sizeof(struct directory_item), 1, f);
            
            // Kontrola prázdného místa (název začíná \0)
            if (item.item_name[0] == '\0') continue;

            if (strcmp(item.item_name, name) == 0) {
                return item.inode; // NALEZENO
            }
        }
    }
    return -1; // Nenalezeno
}

int fs_path_to_inode(const char *filename, const char *path) {
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;

    struct superblock sb;
    load_superblock(f, &sb);

    // Začínáme v ROOTu
    // Poznámka: Pokud bychom řešili 'cd', zde by byla proměnná current_inode
    int current_inode = 0; 
    
    // Uděláme si kopii cesty, protože strtok ji ničí
    char *path_copy = strdup(path);
    char *token = strtok(path_copy, "/");

    while (token != NULL) {
        // Pokud je token "." (aktuální) nebo prázdný, ignorujeme
        if (strcmp(token, ".") == 0 || strlen(token) == 0) {
            token = strtok(NULL, "/");
            continue;
        }

        // Najdi token v aktuálním adresáři
        int next_inode = find_inode_in_dir(f, &sb, current_inode, token);
        
        if (next_inode == -1) {
            // Nenalezeno
            free(path_copy);
            fclose(f);
            return -1;
        }

        current_inode = next_inode;
        token = strtok(NULL, "/");
    }

    free(path_copy);
    fclose(f);
    return current_inode;
}

void fs_ls(const char *filename, int inode_id) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }

    struct superblock sb;
    load_superblock(f, &sb);

    struct pseudo_inode dir_inode;
    read_inode(f, &sb, inode_id, &dir_inode); // Čteme do dir_inode

    if (!dir_inode.isDirectory) {
        printf("PATH NOT FOUND (Not a directory)\n"); 
        fclose(f);
        return;
    }

    printf("Listing directory (Inode %d):\n", inode_id);

    // ZDE BYLA CHYBA: místo root.direct1 musí být dir_inode.direct1
    int32_t blocks[] = {
        dir_inode.direct1, dir_inode.direct2, dir_inode.direct3, 
        dir_inode.direct4, dir_inode.direct5
    };
    
    struct directory_item item;
    int items_per_cluster = sb.cluster_size / sizeof(struct directory_item);

    for (int i = 0; i < 5; i++) {
        if (blocks[i] == CLUSTER_UNUSED) continue; 

        long data_offset = sb.data_start_address + (blocks[i] * sb.cluster_size);
        fseek(f, data_offset, SEEK_SET);

        for (int j = 0; j < items_per_cluster; j++) {
            fread(&item, sizeof(struct directory_item), 1, f);
            
            if (item.item_name[0] != '\0') {
                struct pseudo_inode item_inode;
                long cur_pos = ftell(f);
                read_inode(f, &sb, item.inode, &item_inode);
                
                if (item_inode.isDirectory) {
                    printf("DIR: %s\n", item.item_name);
                } else {
                    printf("FILE: %s\n", item.item_name);
                }
                fseek(f, cur_pos, SEEK_SET);
            }
        }
    }
    fclose(f);
}

void fs_info(const char *filename, int inode_id) {
    FILE *f = fopen(filename, "rb");
    if (!f) { printf("FILE NOT FOUND\n"); return; }
    
    struct superblock sb;
    load_superblock(f, &sb);
    
    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);
    
    // Výpis informací [cite: 90-97]
    printf("--- INODE INFO ---\n");
    printf("ID: %d\n", inode.nodeid);
    printf("Type: %s\n", inode.isDirectory ? "Directory" : "File");
    printf("Size: %d B\n", inode.file_size);
    printf("Links: %d\n", inode.references);
    printf("Direct Blocks: %d %d %d %d %d\n", 
           inode.direct1, inode.direct2, inode.direct3, inode.direct4, inode.direct5);
    
    fclose(f);
}

int fs_incp(const char *filename, const char *host_path, const char *vfs_path) {
    // 1. Otevření zdrojového souboru (Host)
    FILE *host_f = fopen(host_path, "rb");
    if (!host_f) {
        printf("FILE NOT FOUND (on host)\n");
        return 0;
    }

    // Zjištění velikosti
    fseek(host_f, 0, SEEK_END);
    long file_size = ftell(host_f);
    fseek(host_f, 0, SEEK_SET);

    // Limitace na přímé odkazy (5 * Cluster Size)
    // CLUSTER_SIZE máme v define, ale pro jistotu si ho načteme ze SB,
    // zde pro kontrolu natvrdo 5 * 1024 (nebo podle vaší definice).
    if (file_size > 5 * CLUSTER_SIZE) {
        printf("FILE TOO BIG (Implementation limit: 5KB)\n");
        fclose(host_f);
        return 0;
    }

    // 2. Příprava VFS
    FILE *f = fopen(filename, "rb+");
    if (!f) {
        fclose(host_f);
        return 0;
    }

    struct superblock sb;
    load_superblock(f, &sb);

    // Parsování cesty (kam to uložit)
    char *path_copy = strdup(vfs_path);
    char *last_slash = strrchr(path_copy, '/');
    char parent_path[256] = "";
    char new_name[128] = "";

    if (last_slash == NULL) { // "soubor.txt" -> root
        strcpy(parent_path, "/");
        strncpy(new_name, vfs_path, 11);
    } else if (last_slash == path_copy) { // "/soubor.txt" -> root
        strcpy(parent_path, "/");
        strncpy(new_name, last_slash + 1, 11);
    } else { // "/data/soubor.txt" -> "/data"
        *last_slash = '\0';
        strcpy(parent_path, path_copy);
        strncpy(new_name, last_slash + 1, 11);
    }
    free(path_copy);

    // Kontrola délky jména
    if (strlen(new_name) > MAX_NAME_LEN) {
        printf("FILENAME TOO LONG\n");
        fclose(host_f); fclose(f); return 0;
    }

    // Najdi rodiče
    int parent_id = fs_path_to_inode(filename, parent_path);
    if (parent_id == -1) {
        printf("PATH NOT FOUND\n");
        fclose(host_f); fclose(f); return 0;
    }

    // Zkontroluj duplicitu jména
    if (find_inode_in_dir(f, &sb, parent_id, new_name) != -1) {
        printf("EXIST\n"); // Soubor už existuje
        fclose(host_f); fclose(f); return 0;
    }

    // 3. Alokace Inodu
    int free_inode_id = find_free_bit(f, &sb, true);
    if (free_inode_id == -1) {
        printf("NO SPACE (Inodes)\n");
        fclose(host_f); fclose(f); return 0;
    }
    set_bit(f, &sb, true, free_inode_id, true);

    // 4. Alokace dat a zápis
    struct pseudo_inode new_inode = {0};
    new_inode.nodeid = free_inode_id;
    new_inode.isDirectory = false;
    new_inode.references = 1;
    new_inode.file_size = file_size;
    
    // Nastavíme vše na unused
    new_inode.direct1 = CLUSTER_UNUSED; new_inode.direct2 = CLUSTER_UNUSED;
    new_inode.direct3 = CLUSTER_UNUSED; new_inode.direct4 = CLUSTER_UNUSED;
    new_inode.direct5 = CLUSTER_UNUSED;
    new_inode.indirect1 = CLUSTER_UNUSED; new_inode.indirect2 = CLUSTER_UNUSED;

    // Pole pointerů na direct položky ve struktuře (hack pro cyklus)
    // Pozor: toto funguje jen pro zápis, ne pro čtení pointerů, protože int je hodnota.
    // Musíme si pamatovat ID bloků do pole.
    int32_t allocated_blocks[5] = {CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED, CLUSTER_UNUSED};

    int bytes_remaining = file_size;
    int block_idx = 0;
    uint8_t buffer[CLUSTER_SIZE];

    while (bytes_remaining > 0 && block_idx < 5) {
        int free_block = find_free_bit(f, &sb, false);
        if (free_block == -1) {
            printf("NO SPACE (Blocks)\n");
            // Zde by měl být cleanup (smazat inode), ale pro SP stačí return 0
            fclose(host_f); fclose(f); return 0;
        }
        set_bit(f, &sb, false, free_block, true);
        allocated_blocks[block_idx] = free_block;

        // Čtení dat z host souboru
        int to_read = (bytes_remaining > CLUSTER_SIZE) ? CLUSTER_SIZE : bytes_remaining;
        memset(buffer, 0, CLUSTER_SIZE); // Vyčistit buffer
        fread(buffer, 1, to_read, host_f);
        
        // Zápis do VFS
        long data_addr = sb.data_start_address + (free_block * sb.cluster_size);
        fseek(f, data_addr, SEEK_SET);
        fwrite(buffer, 1, CLUSTER_SIZE, f); // Zapíšeme celý cluster (padding nuly)

        bytes_remaining -= to_read;
        block_idx++;
    }

    // Přiřazení bloků do inodu
    new_inode.direct1 = allocated_blocks[0];
    new_inode.direct2 = allocated_blocks[1];
    new_inode.direct3 = allocated_blocks[2];
    new_inode.direct4 = allocated_blocks[3];
    new_inode.direct5 = allocated_blocks[4];

    // Zápis inodu
    long inode_addr = sb.inode_start_address + (free_inode_id * sizeof(struct pseudo_inode));
    fseek(f, inode_addr, SEEK_SET);
    fwrite(&new_inode, sizeof(new_inode), 1, f);

    // Přidání do adresáře
    struct directory_item new_item;
    new_item.inode = free_inode_id;
    strcpy(new_item.item_name, new_name);
    
    add_directory_item(f, &sb, parent_id, &new_item);

    fclose(host_f);
    fclose(f);
    return 1;
}
