#include <stdio.h>
#include <string.h>
#include "../include/fs_core.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Použití: %s <název_fs> [příkaz] [parametry]\n", argv[0]);
        return 1;
    }

    const char *fs_name = argv[1];

    // Pokud nejsou další argumenty, program skončí (zatím)
    // V další fázi zde uděláme interaktivní smyčku, pokud argumenty chybí
    if (argc == 2) {
        printf("Zatím není implementován interaktivní režim. Zadejte příkaz jako argument.\n");
        return 0;
    }

    const char *command = argv[2];

    if (strcmp(command, "format") == 0) {
        if (argc < 4) {
            printf("Chyba: Musíte zadat velikost (např. 600MB)\n");
            return 1;
        }
        if (fs_format(fs_name, argv[3])) {
            printf("OK\n"); // Dle zadání [cite: 118]
        } else {
            printf("CANNOT CREATE FILE\n"); // Dle zadání [cite: 119]
        }
    } else {
        printf("Neznámý příkaz: %s\n", command);
    }

    return 0;
}