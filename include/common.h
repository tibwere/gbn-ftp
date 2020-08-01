#ifndef COMMON_H
#define COMMON_H

#include "gbn.h"

enum app_usages {
        STANDARD,
        HELP,
        VERSION,
        ERROR
};

void error_handler(const char *message);

#endif