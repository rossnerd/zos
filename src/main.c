#define _POSIX_C_SOURCE 200809L

/**
 * @file main.c
 * @brief CLI "shell" nad zjednodušeným souborovým systémem (ZOS semestrální práce).
 *
 * Program načítá příkazy ze stdin (nebo z textového souboru přes `load`) a deleguje
 * vlastní FS operace do modulů `cmd_*.c` / `fs_utils.c`.
 *
 * Důležité:
 * - Formát výstupů (OK/FILE NOT FOUND/PATH NOT FOUND/...) je závazný podle zadání.
 * - V tomto souboru řešíme pouze parsování a práci s cestami (relativní/absolutní).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Project headers (Makefile adds -I./include) */
#include "fs_core.h"
#include "fs_utils.h"

/** Maximální délka cesty, kterou drží "shell" (musí být konzistentní napříč main.c). */
enum { MAX_PATH_LEN = 1024 };

/** Maximální počet tokenů příkazu ("argv") zpracovaných v jednom řádku. */
enum { MAX_ARGS = 16 };

/**
 * @brief Kontext jednoduché interaktivní práce (aktuální adresář + název FS souboru).
 */
typedef struct ShellContext {
    const char *fs_name;
    char cwd[MAX_PATH_LEN];
} ShellContext;

/**
 * @brief Rozdělí řádek na tokeny podle whitespace.
 *
 * Funkce modifikuje vstupní buffer (vkládá '\0'). Tokeny vrací jako ukazatele
 * do původního řádku.
 *
 * @param line     Modifikovatelný řetězec.
 * @param argv     Pole pro tokeny.
 * @param argv_cap Kapacita pole argv.
 * @return Počet nalezených tokenů.
 */
static int tokenize(char *line, char **argv, int argv_cap)
{
    int argc = 0;
    char *saveptr = NULL;

    for (char *tok = strtok_r(line, " \t\r\n", &saveptr);
         tok != NULL && argc < argv_cap;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        argv[argc++] = tok;
    }
    return argc;
}

/**
 * @brief Sestaví absolutní cestu z (cwd + in) a normalizuje segmenty "." a "..".
 *
 * - Pokud je @p in prázdné, vrací @p cwd.
 * - Pokud @p in začíná '/', jde o absolutní cestu.
 * - Normalizace:
 *   - "." se zahodí
 *   - ".." odstraní poslední segment (pokud existuje)
 *
 * @param cwd    Aktuální adresář (absolutní cesta).
 * @param in     Vstupní cesta (relativní nebo absolutní).
 * @param out    Výstupní buffer.
 * @param out_sz Velikost výstupního bufferu.
 */
static void make_abs_path(const char *cwd, const char *in, char *out, size_t out_sz)
{
    /* vždy vrátit nějakou rozumnou cestu. */
    if (out_sz == 0) {
        return;
    }
    out[0] = '\0';

    /* 1) Sestav dočasnou cestu (absolutní). */
    char tmp[MAX_PATH_LEN];
    tmp[0] = '\0';

    if (in == NULL || in[0] == '\0') {
        (void)snprintf(out, out_sz, "%s", cwd);
        return;
    }

    if (in[0] == '/') {
        (void)snprintf(tmp, sizeof(tmp), "%s", in);
    } else {
        if (strcmp(cwd, "/") == 0) {
            (void)snprintf(tmp, sizeof(tmp), "/%s", in);
        } else {
            (void)snprintf(tmp, sizeof(tmp), "%s/%s", cwd, in);
        }
    }

    /* 2) Normalizace: použijeme tokenizaci tmp pomocí '/', bez alokací. */
    char *segments[256];
    int seg_count = 0;

    for (char *p = tmp; *p != '\0';) {
        while (*p == '/') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }

        char *seg = p;
        while (*p != '\0' && *p != '/') {
            ++p;
        }
        if (*p == '/') {
            *p = '\0';
            ++p;
        }

        if (strcmp(seg, ".") == 0) {
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            if (seg_count > 0) {
                --seg_count;
            }
            continue;
        }
        if (seg_count < (int)(sizeof(segments) / sizeof(segments[0]))) {
            segments[seg_count++] = seg;
        }
    }

    /* 3) Sestav normalizovanou cestu do out. */
    size_t pos = 0;
    out[pos++] = '/';
    out[pos] = '\0';

    for (int i = 0; i < seg_count; ++i) {
        const size_t len = strlen(segments[i]);
        /* +1 za '/' (pokud není první), +1 za '\0' */
        if (pos + len + 2 > out_sz) {
            break; /* zkrátíme (lepší než přetečení) */
        }
        if (pos > 1) {
            out[pos++] = '/';
        }
        memcpy(out + pos, segments[i], len);
        pos += len;
        out[pos] = '\0';
    }

    /* Pokud nic nezbylo, výsledkem je root. */
    if (out[0] == '\0') {
        (void)snprintf(out, out_sz, "/");
    }
}

/**
 * @brief Zjistí, zda inode odpovídá adresáři.
 * @note Tisk chybových hlášek musí zůstat kompatibilní se zadáním.
 */
static bool is_inode_directory(const char *fs_name, int inode_id)
{
    FILE *f = fopen(fs_name, "rb");
    if (f == NULL) {
        printf("FILE NOT FOUND\n");
        return false;
    }

    struct superblock sb;
    load_superblock(f, &sb);

    struct pseudo_inode inode;
    read_inode(f, &sb, inode_id, &inode);
    fclose(f);

    return inode.isDirectory;
}

/**
 * @brief Provede jeden příkaz.
 *
 * @return true pokud se má pokračovat, false pokud se má ukončit ("exit").
 */
static bool exec_command(ShellContext *ctx, int argc, char **argv)
{
    if (argc == 0) {
        return true;
    }

    const char *cmd = argv[0];

    /* --- systémové příkazy --- */
    if (strcmp(cmd, "exit") == 0) {
        return false;
    }
    if (strcmp(cmd, "pwd") == 0) {
        printf("%s\n", ctx->cwd);
        return true;
    }
    if (strcmp(cmd, "cd") == 0) {
        const char *target = (argc >= 2) ? argv[1] : "/";
        char abs_path[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, target, abs_path, sizeof(abs_path));

        const int inode_id = fs_path_to_inode(ctx->fs_name, abs_path);
        if (inode_id == -1) {
            printf("PATH NOT FOUND\n");
            return true;
        }
        if (!is_inode_directory(ctx->fs_name, inode_id)) {
            /* Zadání chce pro cd chybu "PATH NOT FOUND". */
            printf("PATH NOT FOUND\n");
            return true;
        }

        (void)snprintf(ctx->cwd, sizeof(ctx->cwd), "%s", abs_path);
        printf("OK\n");
        return true;
    }

    if (strcmp(cmd, "load") == 0) {
        if (argc < 2) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        FILE *script = fopen(argv[1], "r");
        if (script == NULL) {
            printf("FILE NOT FOUND\n");
            return true;
        }

        char *line = NULL;
        size_t n = 0;
        while (getline(&line, &n, script) != -1) {
            /* přeskoč komentáře a prázdné řádky */
            char *p = line;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (*p == '#' || *p == '\n' || *p == '\0') {
                continue;
            }

            char *argv2[MAX_ARGS];
            const int argc2 = tokenize(line, argv2, MAX_ARGS);
            if (argc2 == 0) {
                continue;
            }
            if (!exec_command(ctx, argc2, argv2)) {
                break;
            }
        }
        free(line);
        fclose(script);
        return true;
    }

    /* --- příkazy pracující s cestami ve FS ---
     * Pozn.: implementace ve fs_* funkcích očekává absolutní cestu.
     */

    if (strcmp(cmd, "format") == 0) {
        if (argc < 2) {
            printf("CANNOT CREATE FILE\n");
            return true;
        }
        if (fs_format(ctx->fs_name, argv[1])) {
            printf("OK\n");
        } else {
            printf("CANNOT CREATE FILE\n");
        }
        (void)snprintf(ctx->cwd, sizeof(ctx->cwd), "/");
        return true;
    }

    if (strcmp(cmd, "statfs") == 0) {
        fs_statfs(ctx->fs_name);
        return true;
    }

    if (strcmp(cmd, "ls") == 0) {
        const char *target = (argc >= 2) ? argv[1] : ".";
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, target, abs, sizeof(abs));

        const int inode_id = fs_path_to_inode(ctx->fs_name, abs);
        if (inode_id == -1) {
            printf("PATH NOT FOUND\n");
            return true;
        }
        fs_ls(ctx->fs_name, inode_id);
        return true;
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc < 2) {
            printf("PATH NOT FOUND\n");
            return true;
        }
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs, sizeof(abs));
        fs_info_path(ctx->fs_name, abs);
        return true;
    }

    if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 2) {
            printf("PATH NOT FOUND\n");
            return true;
        }
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs, sizeof(abs));
        if (fs_mkdir(ctx->fs_name, abs)) {
            printf("OK\n");
        }
        return true;
    }

    if (strcmp(cmd, "rmdir") == 0) {
        if (argc < 2) {
            printf("PATH NOT FOUND\n");
            return true;
        }
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs, sizeof(abs));
        if (strcmp(abs, ctx->cwd) == 0 || strcmp(abs, "/") == 0) {
            /* Zakázat mazání aktuálního adresáře (a rootu) – udržení konzistence PWD. */
            printf("NOT EMPTY\n");
            return true;
        }
        if (fs_rmdir(ctx->fs_name, abs)) {
            printf("OK\n");
        }
        return true;
    }

    if (strcmp(cmd, "incp") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[2], abs, sizeof(abs));
        if (fs_incp(ctx->fs_name, argv[1], abs)) {
            printf("OK\n");
        }
        return true;
    }

    if (strcmp(cmd, "outcp") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs, sizeof(abs));
        if (fs_outcp(ctx->fs_name, abs, argv[2])) {
            printf("OK\n");
        }
        return true;
    }

    if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs, sizeof(abs));
        fs_cat(ctx->fs_name, abs);
        return true;
    }

    if (strcmp(cmd, "rm") == 0) {
        if (argc < 2) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        char abs[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs, sizeof(abs));
        if (fs_rm(ctx->fs_name, abs)) {
            printf("OK\n");
        }
        return true;
    }

    if (strcmp(cmd, "cp") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        char abs1[MAX_PATH_LEN];
        char abs2[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs1, sizeof(abs1));
        make_abs_path(ctx->cwd, argv[2], abs2, sizeof(abs2));
        if (fs_cp(ctx->fs_name, abs1, abs2)) {
            printf("OK\n");
        }
        return true;
    }

    if (strcmp(cmd, "mv") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        char abs1[MAX_PATH_LEN];
        char abs2[MAX_PATH_LEN];
        make_abs_path(ctx->cwd, argv[1], abs1, sizeof(abs1));
        make_abs_path(ctx->cwd, argv[2], abs2, sizeof(abs2));
        if (fs_mv(ctx->fs_name, abs1, abs2)) {
            printf("OK\n");
        }
        return true;
    }

    /* --- rozšíření pro login na "r": xcp + add --- */
    if (strcmp(cmd, "xcp") == 0) {
        if (argc < 4) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        /* Záměrně předáváme původní argumenty - implementace je v cmd_extra.c. */
        if (fs_xcp(ctx->fs_name, argv[1], argv[2], argv[3])) {
            printf("OK\n");
        }
        return true;
    }
    if (strcmp(cmd, "add") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return true;
        }
        if (fs_add(ctx->fs_name, argv[1], argv[2])) {
            printf("OK\n");
        }
        return true;
    }

    printf("UNKNOWN COMMAND\n");
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        /* původní chování: špatný počet parametrů -> CANNOT OPEN FILE */
        printf("CANNOT OPEN FILE\n");
        return 1;
    }

    ShellContext ctx = {
        .fs_name = argv[1],
        .cwd = "/",
    };

    char *line = NULL;
    size_t n = 0;

    while (getline(&line, &n, stdin) != -1) {
        char *argv2[MAX_ARGS];
        const int argc2 = tokenize(line, argv2, MAX_ARGS);
        if (!exec_command(&ctx, argc2, argv2)) {
            break;
        }
    }

    free(line);
    return 0;
}
