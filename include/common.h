#ifndef COMMON_H
#define COMMON_H

#include "gbn.h"

enum app_usages {
        STANDARD,
        HELP,
        VERSION,
        ERROR
};

enum app_usages parse_cmd(int argc, char **argv, struct gbn_config *conf, int *port);
void default_gbn_configuration(struct gbn_config* cfg);
void error_handler(const char *message);

#endif