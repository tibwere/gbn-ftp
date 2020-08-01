#ifndef GBN_H
#define GBN_H

#include <stdbool.h>

struct gbn_config {
        unsigned int N;
        unsigned long rto_msec;
        bool is_adaptive;
        float probability;
};

#endif