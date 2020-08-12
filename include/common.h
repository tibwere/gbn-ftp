#ifndef COMMON_H
#define COMMON_H

#include "gbnftp.h"

#define DEFAULT_PORT 2929
#define CHUNK_SIZE 1024

enum app_usages {
        STANDARD,
        HELP,
        VERSION,
        ERROR
};

void error_handler(const char *message);
size_t get_input(unsigned int length, char *string, bool not_null);
char multi_choice(const char *question, const char *choices, int no_choices);

#endif