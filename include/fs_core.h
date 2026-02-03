#ifndef FS_CORE_H
#define FS_CORE_H

#include <stdio.h>
#include "structs.h"

// Funkce pro formátování disku
// size_str může být "100KB", "10MB" atd.
int fs_format(const char *filename, const char *size_str);

#endif // FS_CORE_H