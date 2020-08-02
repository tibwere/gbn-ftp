#ifndef COMMON_H
#define COMMON_H

#include "gbn.h"

#define DEFAULT_PORT 2929
#define CHUNK_SIZE 1024

enum app_usages {
        STANDARD,
        HELP,
        VERSION,
        ERROR
};

void error_handler(const char *message);

#endif