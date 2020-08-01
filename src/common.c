#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "gbn.h"
#include "common.h"

const struct gbn_config default_config = {
        16, 1000, false, 0.2
};

void error_handler(const char *message)
{
        fprintf(stderr, "%s.\nError %d: %s\n", message, errno, strerror(errno));
}