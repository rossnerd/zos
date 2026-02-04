#define _POSIX_C_SOURCE 200809L /* kvůli strdup (v jiných modulech) */
/**
 * @file cmd_system.c
 * @brief Systémové příkazy: format, statfs, info.
 *
 * Konzervativní refaktoring:
 *  - zachované veřejné funkce a jejich signatury (fs_format, fs_statfs, fs_info, fs_info_path)
 *  - zachované texty výstupů (kvůli automatickým testům)
 *  - doplněné dokumentační komentáře a základní kontroly I/O
 *  - snížené zanoření (early-return), sjednocené pomocné funkce
 *
 * Pozn.: Funkce parse_size není v hlavičce, ale ponecháváme ji ne-`static` pro jistotu
 * (může být použita i jinde v projektu).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../include/fs_core.h"
#include "../include/fs_utils.h"

/* ========================================================================== */
/* Interní helpery                                                            */
/* ========================================================================== */

/**
 * @brief Case-insensitive test, zda řetězec končí suffixem (např. "KB", "MB").
 */
static int ends_with_ci(const char *s, const char *suffix)
{
    if (!s || !suffix) {
        return 0;
    }

    const size_t sl = strlen(s);
    const size_t su = strlen(suffix);
    if (su == 0 || su > sl) {
        return 0;
    }

    const char *tail = s + (sl - su);
    for (size_t i = 0; i < su; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Načte celé N bajtů ze souboru (best-effort).
 */
static int read_exact(FILE *f, void *buf, size_t n)
{
    return f && buf && fread(buf, 1, n, f) == n;
}

/**
 * @brief Zapíše celé N bajtů do souboru (best-effort).
 */
static int write_exact(FILE *f, const void *buf, size_t n)
{
    return f && buf && fwrite(buf, 1, n, f) == n;
}

/* ========================================================================== */
/* FORMAT                                                                     */
/* ========================================================================== */

/**
 * @brief Převod velikosti z textu na bajty.
 *
 * Podporuje formáty jako "100KB", "10MB" (případně i "kB"). Pokud suffix není uveden,
 * bere se hodnota jako bajty.
 *
 * @param size_str Řetězec velikosti.
 * @return Velikost v bajtech (long). Při neplatném vstupu vrací 0 (stejně jako atol()).
 */
long parse_size(const char *size_str)
{
    if (!size_str) {
        return 0;
    }

    char *end = NULL;
    long size = strtol(size_str, &end, 10); /* kompatibilní chování: při chybě 0 */
    if (size <= 0) {
        return size;
    }

    /* Zachováme původní kompatibilitu: KB/kB a MB. */
    if (strstr(size_str, "KB") || strstr(size_str, "kB") || ends_with_ci(size_str, "kb")) {
        size *= 1024L;
    } else if (strstr(size_str, "MB") || ends_with_ci(size_str, "mb")) {
        size *= 1024L * 1024L;
    }

    return size;
}

int fs_format(const char *filename, const char *size_str)
{
    const long disk_size = parse_size(size_str);

    FILE *f = filename ? fopen(filename, "wb+") : NULL;
    if (!f) {
        return 0;
    }

    struct superblock sb;
    memset(&sb, 0, sizeof(sb));

    /* signature má v zadání vypsanou hodnotu "r-login" */
    strncpy(sb.signature, "r-login", sizeof(sb.signature) - 1);
    strncpy(sb.volume_descriptor, "Semestralni prace ZOS 2025", sizeof(sb.volume_descriptor) - 1);

    sb.disk_size = (int32_t)disk_size;
    sb.cluster_size = CLUSTER_SIZE;
    sb.cluster_count = (sb.cluster_size > 0) ? (sb.disk_size / sb.cluster_size) : 0;

    sb.bitmapi_start_address = (int32_t)sizeof(struct superblock);

    const int inode_bitmap_size = (sb.cluster_count + 7) / 8;
    sb.bitmap_start_address = sb.bitmapi_start_address + inode_bitmap_size;

    const int data_bitmap_size = (sb.cluster_count + 7) / 8;
    sb.inode_start_address = sb.bitmap_start_address + data_bitmap_size;

    const long inodes_area_size = (long)sb.cluster_count * (long)sizeof(struct pseudo_inode);
    sb.data_start_address = sb.inode_start_address + (int32_t)inodes_area_size;

    if (sb.data_start_address >= sb.disk_size) {
        fclose(f);
        return 0;
    }

    /* Superblock */
    (void)write_exact(f, &sb, sizeof(sb));

    /* Bitmapy */
    uint8_t *ibitmap = (uint8_t *)calloc(1, (size_t)inode_bitmap_size);
    if (!ibitmap) {
        fclose(f);
        return 0;
    }
    ibitmap[0] |= 1; /* root inode obsazený */
    (void)fseek(f, sb.bitmapi_start_address, SEEK_SET);
    (void)write_exact(f, ibitmap, (size_t)inode_bitmap_size);
    free(ibitmap);

    uint8_t *dbitmap = (uint8_t *)calloc(1, (size_t)data_bitmap_size);
    if (!dbitmap) {
        fclose(f);
        return 0;
    }
    dbitmap[0] |= 1; /* root data cluster obsazený */
    (void)fseek(f, sb.bitmap_start_address, SEEK_SET);
    (void)write_exact(f, dbitmap, (size_t)data_bitmap_size);
    free(dbitmap);

    /* Inody */
    struct pseudo_inode root_inode;
    memset(&root_inode, 0, sizeof(root_inode));
    root_inode.nodeid = 0;
    root_inode.isDirectory = true;
    root_inode.references = 1;
    root_inode.file_size = sb.cluster_size;

    root_inode.direct1 = 0;
    root_inode.direct2 = CLUSTER_UNUSED;
    root_inode.direct3 = CLUSTER_UNUSED;
    root_inode.direct4 = CLUSTER_UNUSED;
    root_inode.direct5 = CLUSTER_UNUSED;
    root_inode.indirect1 = CLUSTER_UNUSED;
    root_inode.indirect2 = CLUSTER_UNUSED;

    (void)fseek(f, sb.inode_start_address, SEEK_SET);
    (void)write_exact(f, &root_inode, sizeof(root_inode));

    struct pseudo_inode empty_inode;
    memset(&empty_inode, 0, sizeof(empty_inode));
    empty_inode.direct1 = CLUSTER_UNUSED;
    empty_inode.direct2 = CLUSTER_UNUSED;
    empty_inode.direct3 = CLUSTER_UNUSED;
    empty_inode.direct4 = CLUSTER_UNUSED;
    empty_inode.direct5 = CLUSTER_UNUSED;
    empty_inode.indirect1 = CLUSTER_UNUSED;
    empty_inode.indirect2 = CLUSTER_UNUSED;

    for (int i = 1; i < sb.cluster_count; i++) {
        (void)write_exact(f, &empty_inode, sizeof(empty_inode));
    }

    /* Root data (., ..) */
    struct directory_item self;
    memset(&self, 0, sizeof(self));
    self.inode = 0;
    strcpy(self.item_name, ".");

    struct directory_item parent;
    memset(&parent, 0, sizeof(parent));
    parent.inode = 0;
    strcpy(parent.item_name, "..");

    (void)fseek(f, sb.data_start_address, SEEK_SET);
    (void)write_exact(f, &self, sizeof(self));
    (void)write_exact(f, &parent, sizeof(parent));

    /* Vyplnění zbytku clusteru nulami */
    const long used = 2L * (long)sizeof(struct directory_item);
    const long pad = (long)sb.cluster_size - used;
    if (pad > 0) {
        uint8_t *zeros = (uint8_t *)calloc(1, (size_t)pad);
        if (zeros) {
            (void)write_exact(f, zeros, (size_t)pad);
            free(zeros);
        }
    }

    /* Dotáhneme soubor na požadovanou velikost disku */
    (void)fseek(f, disk_size - 1, SEEK_SET);
    (void)fputc(0, f);

    fclose(f);
    return 1;
}

/* ========================================================================== */
/* STATFS + INFO                                                              */
/* ========================================================================== */

static int bit_is_set(const uint8_t *bm, int idx)
{
    return (bm[idx / 8] >> (idx % 8)) & 1;
}

static long count_set_bits_upto(const uint8_t *bm, long n_bits)
{
    long c = 0;
    for (long i = 0; i < n_bits; i++) {
        c += bit_is_set(bm, (int)i);
    }
    return c;
}

void fs_statfs(const char *filename)
{
    FILE *f = filename ? fopen(filename, "rb") : NULL;
    if (!f) {
        printf("FILE NOT FOUND\n");
        return;
    }

    struct superblock sb;
    if (!load_superblock(f, &sb)) {
        fclose(f);
        printf("FILE NOT FOUND\n");
        return;
    }

    /* Skutečné počty dle rozložení ve VFS */
    const long inode_count =
        (sb.data_start_address - sb.inode_start_address) / (long)sizeof(struct pseudo_inode);
    const long data_cluster_count =
        (sb.disk_size - sb.data_start_address) / (long)sb.cluster_size;

    const long inode_bm_bytes = sb.bitmap_start_address - sb.bitmapi_start_address;
    const long data_bm_bytes  = sb.inode_start_address - sb.bitmap_start_address;

    if (inode_bm_bytes <= 0 || data_bm_bytes <= 0 || inode_count < 0 || data_cluster_count < 0) {
        fclose(f);
        printf("FILE NOT FOUND\n");
        return;
    }

    uint8_t *ibm = (uint8_t *)malloc((size_t)inode_bm_bytes);
    uint8_t *dbm = (uint8_t *)malloc((size_t)data_bm_bytes);
    if (!ibm || !dbm) {
        free(ibm);
        free(dbm);
        fclose(f);
        printf("FILE NOT FOUND\n");
        return;
    }

    (void)fseek(f, sb.bitmapi_start_address, SEEK_SET);
    (void)read_exact(f, ibm, (size_t)inode_bm_bytes);

    (void)fseek(f, sb.bitmap_start_address, SEEK_SET);
    (void)read_exact(f, dbm, (size_t)data_bm_bytes);

    const long used_inodes = count_set_bits_upto(ibm, inode_count);
    const long used_blocks = count_set_bits_upto(dbm, data_cluster_count);

    const long free_inodes = inode_count - used_inodes;
    const long free_blocks = data_cluster_count - used_blocks;

    /* Počet adresářů: projdeme pouze obsazené inody */
    long dir_count = 0;
    for (long i = 0; i < inode_count; i++) {
        if (!bit_is_set(ibm, (int)i)) {
            continue;
        }
        struct pseudo_inode ino;
        read_inode(f, &sb, (int)i, &ino);
        if (ino.isDirectory) {
            dir_count++;
        }
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

static void fs_info_print(const char *name, const struct pseudo_inode *inode)
{
    /* Název – velikost – i-uzel – odkazy (přímé + nepřímé) */
    printf("%s - %d B - i-node %d\n", name, inode->file_size, inode->nodeid);

    /* přímé odkazy */
    printf("direct: ");
    int first = 1;
    const int32_t d[5] = { inode->direct1, inode->direct2, inode->direct3, inode->direct4, inode->direct5 };
    for (int i = 0; i < 5; i++) {
        if (d[i] == CLUSTER_UNUSED) {
            continue;
        }
        if (!first) {
            printf(", ");
        }
        printf("%d", d[i]);
        first = 0;
    }
    if (first) {
        printf("-");
    }
    printf("\n");

    /* nepřímé odkazy */
    printf("indirect1: %d\n", inode->indirect1 == CLUSTER_UNUSED ? -1 : inode->indirect1);
    printf("indirect2: %d\n", inode->indirect2 == CLUSTER_UNUSED ? -1 : inode->indirect2);
}

void fs_info(const char *filename, int inode_id)
{
    FILE *f = filename ? fopen(filename, "rb") : NULL;
    if (!f) {
        printf("FILE NOT FOUND\n");
        return;
    }

    struct superblock sb;
    (void)load_superblock(f, &sb);

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);

    /* Bez jména – fallback (používej spíš fs_info_path) */
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "inode%d", inode.nodeid);
    fs_info_print(tmp, &inode);

    fclose(f);
}

void fs_info_path(const char *filename, const char *path)
{
    const int inode_id = fs_path_to_inode(filename, path);
    if (inode_id == -1) {
        printf("PATH NOT FOUND\n");
        return;
    }

    /* basename pro NAME */
    const char *name = path;
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (slash) {
        name = slash + 1;
    }
    if (!name || name[0] == '\0') {
        name = "/";
    }

    FILE *f = filename ? fopen(filename, "rb") : NULL;
    if (!f) {
        printf("FILE NOT FOUND\n");
        return;
    }

    struct superblock sb;
    (void)load_superblock(f, &sb);

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);

    fclose(f);
    fs_info_print(name, &inode);
}
