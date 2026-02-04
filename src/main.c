#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "../include/fs_core.h"
#include "../include/fs_utils.h"

// --- jednoduché zpracování cest (absolutní/relativní + normalizace . a ..) ---

static void path_join_and_normalize(const char *cwd, const char *in, char *out, size_t out_sz) {
    char tmp[1024];
    tmp[0] = '\0';

    if (!in || in[0] == '\0') {
        snprintf(out, out_sz, "%s", cwd);
        return;
    }

    if (in[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", in);
    } else {
        if (strcmp(cwd, "/") == 0) snprintf(tmp, sizeof(tmp), "/%s", in);
        else snprintf(tmp, sizeof(tmp), "%s/%s", cwd, in);
    }

    // normalizace pomocí stacku segmentů
    const char *p = tmp;
    char seg[256];
    const char *segments[256];
    int seg_count = 0;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t i = 0;
        while (*p && *p != '/' && i < sizeof(seg) - 1) seg[i++] = *p++;
        seg[i] = '\0';

        if (strcmp(seg, ".") == 0) {
            continue;
        }
        if (strcmp(seg, "..") == 0) {
            if (seg_count > 0) seg_count--;
            continue;
        }
        segments[seg_count++] = strdup(seg);
    }

    // sestavení
    size_t pos = 0;
    if (out_sz > 0) out[0] = '\0';
    if (pos < out_sz) {
        out[pos++] = '/';
        out[pos] = '\0';
    }
    for (int i = 0; i < seg_count; i++) {
        size_t len = strlen(segments[i]);
        if (pos + len + 2 > out_sz) break;
        if (pos > 1) out[pos++] = '/';
        memcpy(out + pos, segments[i], len);
        pos += len;
        out[pos] = '\0';
    }
    if (pos == 0) snprintf(out, out_sz, "/");

    for (int i = 0; i < seg_count; i++) free((void*)segments[i]);
}

static int tokenize(char *line, char **argv, int argv_cap) {
    int argc = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(line, " \t\r\n", &saveptr);
         tok && argc < argv_cap;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        argv[argc++] = tok;
    }
    return argc;
}

static int exec_one(const char *fs_name, const char *cwd, char *cwd_mut, size_t cwd_sz, int argc, char **argv) {
    if (argc == 0) return 1;

    const char *cmd = argv[0];

    // --- systém ---
    if (strcmp(cmd, "exit") == 0) {
        return 0;
    }

    if (strcmp(cmd, "pwd") == 0) {
        printf("%s\n", cwd);
        return 1;
    }

    if (strcmp(cmd, "cd") == 0) {
        const char *target = (argc >= 2) ? argv[1] : "/";
        char abs_path[1024];
        path_join_and_normalize(cwd, target, abs_path, sizeof(abs_path));
        int inode_id = fs_path_to_inode(fs_name, abs_path);
        if (inode_id == -1) {
            printf("PATH NOT FOUND\n");
            return 1;
        }
        // ověř, že je to adresář
        FILE *f = fopen(fs_name, "rb");
        if (!f) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        struct superblock sb;
        load_superblock(f, &sb);
        struct pseudo_inode inode;
        read_inode(f, &sb, inode_id, &inode);
        fclose(f);
        if (!inode.isDirectory) {
            printf("PATH NOT FOUND\n");
            return 1;
        }
        snprintf(cwd_mut, cwd_sz, "%s", abs_path);
        printf("OK\n");
        return 1;
    }

    if (strcmp(cmd, "load") == 0) {
        if (argc < 2) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        FILE *script = fopen(argv[1], "r");
        if (!script) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        char *line = NULL;
        size_t n = 0;
        while (getline(&line, &n, script) != -1) {
            // komentáře a prázdné řádky
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '#' || *p == '\n' || *p == '\0') continue;

            char *argv2[16];
            int argc2 = tokenize(line, argv2, 16);
            if (argc2 == 0) continue;
            int cont = exec_one(fs_name, cwd_mut, cwd_mut, cwd_sz, argc2, argv2);
            if (cont == 0) break;
        }
        free(line);
        fclose(script);
        return 1;
    }

    // --- příkazy s cestami ---
    // pozn.: implementace ve tvých funkcích očekává absolutní cestu, proto převádíme.

    if (strcmp(cmd, "format") == 0) {
        if (argc < 2) {
            printf("CANNOT CREATE FILE\n");
            return 1;
        }
        if (fs_format(fs_name, argv[1])) printf("OK\n");
        else printf("CANNOT CREATE FILE\n");
        snprintf(cwd_mut, cwd_sz, "/");
        return 1;
    }

    if (strcmp(cmd, "statfs") == 0) {
        fs_statfs(fs_name);
        return 1;
    }

    if (strcmp(cmd, "ls") == 0) {
        const char *target = (argc >= 2) ? argv[1] : ".";
        char abs[1024];
        path_join_and_normalize(cwd, target, abs, sizeof(abs));
        int inode_id = fs_path_to_inode(fs_name, abs);
        if (inode_id == -1) {
            printf("PATH NOT FOUND\n");
            return 1;
        }
        fs_ls(fs_name, inode_id);
        return 1;
    }

    if (strcmp(cmd, "info") == 0) {
        if (argc < 2) {
            printf("PATH NOT FOUND\n");
            return 1;
        }
        char abs[1024];
        path_join_and_normalize(cwd, argv[1], abs, sizeof(abs));
        fs_info_path(fs_name, abs);
        return 1;
    }

    if (strcmp(cmd, "mkdir") == 0) {
        if (argc < 2) {
            printf("PATH NOT FOUND\n");
            return 1;
        }
        char abs[1024];
        path_join_and_normalize(cwd, argv[1], abs, sizeof(abs));
        if (fs_mkdir(fs_name, abs)) printf("OK\n");
        return 1;
    }

    if (strcmp(cmd, "rmdir") == 0) {
        if (argc < 2) {
            printf("PATH NOT FOUND\n");
            return 1;
        }
        char abs[1024];
        path_join_and_normalize(cwd, argv[1], abs, sizeof(abs));
        if (fs_rmdir(fs_name, abs)) printf("OK\n");
        return 1;
    }

    if (strcmp(cmd, "incp") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        char abs[1024];
        path_join_and_normalize(cwd, argv[2], abs, sizeof(abs));
        if (fs_incp(fs_name, argv[1], abs)) printf("OK\n");
        return 1;
    }

    if (strcmp(cmd, "outcp") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        char abs[1024];
        path_join_and_normalize(cwd, argv[1], abs, sizeof(abs));
        if (fs_outcp(fs_name, abs, argv[2])) printf("OK\n");
        return 1;
    }

    if (strcmp(cmd, "cat") == 0) {
        if (argc < 2) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        char abs[1024];
        path_join_and_normalize(cwd, argv[1], abs, sizeof(abs));
        fs_cat(fs_name, abs);
        return 1;
    }

    if (strcmp(cmd, "rm") == 0) {
        if (argc < 2) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        char abs[1024];
        path_join_and_normalize(cwd, argv[1], abs, sizeof(abs));
        if (fs_rm(fs_name, abs)) printf("OK\n");
        return 1;
    }

    if (strcmp(cmd, "cp") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        char abs1[1024], abs2[1024];
        path_join_and_normalize(cwd, argv[1], abs1, sizeof(abs1));
        path_join_and_normalize(cwd, argv[2], abs2, sizeof(abs2));
        if (fs_cp(fs_name, abs1, abs2)) printf("OK\n");
        return 1;
    }

    if (strcmp(cmd, "mv") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        char abs1[1024], abs2[1024];
        path_join_and_normalize(cwd, argv[1], abs1, sizeof(abs1));
        path_join_and_normalize(cwd, argv[2], abs2, sizeof(abs2));
        if (fs_mv(fs_name, abs1, abs2)) printf("OK\n");
        return 1;
    }

    // --- rozšíření pro login na "r": xcp + add ---
    if (strcmp(cmd, "xcp") == 0) {
        if (argc < 4) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        // xcp s1 s2 s3 -> přímo předáváme (tvá implementace je v cmd_extra.c)
        if (fs_xcp(fs_name, argv[1], argv[2], argv[3])) printf("OK\n");
        return 1;
    }
    if (strcmp(cmd, "add") == 0) {
        if (argc < 3) {
            printf("FILE NOT FOUND\n");
            return 1;
        }
        if (fs_add(fs_name, argv[1], argv[2])) printf("OK\n");
        return 1;
    }

    printf("UNKNOWN COMMAND\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("CANNOT OPEN FILE\n");
        return 1;
    }
    const char *fs_name = argv[1];

    char cwd[1024];
    snprintf(cwd, sizeof(cwd), "/");

    char *line = NULL;
    size_t n = 0;
    while (getline(&line, &n, stdin) != -1) {
        char *argv2[16];
        int argc2 = tokenize(line, argv2, 16);
        int cont = exec_one(fs_name, cwd, cwd, sizeof(cwd), argc2, argv2);
        if (cont == 0) break;
    }
    free(line);
    return 0;
}
