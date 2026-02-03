#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../include/fs_core.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Použití: %s <název_fs> [příkaz] [parametry]\n", argv[0]);
        return 1;
    }

    const char *fs_name = argv[1];

    if (argc == 2) {
         // Zde později bude while(1) smyčka
         printf("Zadejte příkaz.\n");
         return 0;
    }

    const char *command = argv[2];

    if (strcmp(command, "format") == 0) {
        if (argc < 4) {
            printf("Chyba: format <velikost>\n");
            return 1;
        }
        if (fs_format(fs_name, argv[3])) printf("OK\n");
        else printf("CANNOT CREATE FILE\n");
        
    } else if (strcmp(command, "statfs") == 0) {
        fs_statfs(fs_name);
        
    } else if (strcmp(command, "ls") == 0) {
        const char *target_path = "/";
        if (argc >= 4) target_path = argv[3];

        int inode_id = fs_path_to_inode(fs_name, target_path);
        if (inode_id == -1) {
            printf("PATH NOT FOUND\n");
        } else {
            fs_ls(fs_name, inode_id);
        }
        
    } else if (strcmp(command, "info") == 0) {
        if (argc < 4) {
            printf("Chyba: info <cislo_inodu> (zatím jen ID)\n");
            return 1;
        }
        // Zatím bereme jako argument přímo ID inodu, v další fázi to bude cesta
        fs_info(fs_name, atoi(argv[3]));
        
    } else if (strcmp(command, "mkdir") == 0) {
        if (argc < 4) {
            printf("EXIST\n"); // Nebo syntax error, ale zadání říká EXIST/PATH NOT FOUND
            return 1;
        }
        if (fs_mkdir(fs_name, argv[3])) {
            printf("OK\n");
        }
        // Pokud fs_mkdir vrátí 0, chybová hláška už byla vypsána uvnitř funkce (PATH NOT FOUND, EXIST...)
    } else if (strcmp(command, "incp") == 0) {
        if (argc < 5) {
            printf("Chyba: incp <host_file> <vfs_path>\n");
            return 1;
        }
        // argv[3] je zdroj na hostu, argv[4] je cíl ve VFS
        if (fs_incp(fs_name, argv[3], argv[4])) {
            printf("OK\n");
        }
    } else if (strcmp(command, "xcp") == 0) {
        if (argc < 6) {
            printf("Chyba: xcp <s1> <s2> <s3>\n");
            return 1;
        }
        if (fs_xcp(fs_name, argv[3], argv[4], argv[5])) {
            printf("OK\n");
        }

    } else if (strcmp(command, "add") == 0) {
        if (argc < 5) {
            printf("Chyba: add <s1> <s2>\n");
            return 1;
        }
        if (fs_add(fs_name, argv[3], argv[4])) {
            printf("OK\n");
        }
    } else {
        printf("Neznámý příkaz: %s\n", command);
    }

    return 0;
}