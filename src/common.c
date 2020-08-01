#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "gbn.h"
#include "common.h"

extern char *optarg;
extern int opterr;

enum app_usages parse_cmd(int argc, char **argv, struct gbn_config *conf, int *port)
{
        int opt;

        struct option long_options[] = {
                {"port",        required_argument,      0, 'p'},
                {"windowsize",  required_argument,      0, 'N'},
                {"rto",         required_argument,      0, 't'},
                {"probability", required_argument,      0, 'P'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "p:N:t:P:hv", long_options, NULL)) != -1) {
                switch(opt) {
                        case 'p':
                                *port = strtol(optarg, NULL, 10);
                                break;
                        case 'N':
                                conf->N = strtol(optarg, NULL, 10);
                                break;
                        case 't':
                                conf->rto_msec = strtol(optarg, NULL, 10);
                                break;
                        case 'P':
                                conf->probability = strtol(optarg, NULL, 10) / 100;
                                break;
                        case 'h':
                                return (argc != 2) ? ERROR : HELP;
                        case 'v':
                                return (argc != 2) ? ERROR : VERSION;
                }
        }

        return STANDARD;
}

const struct gbn_config default_config = {
        16, 1000, false, 0.2
};


void default_gbn_configuration(struct gbn_config* cfg)
{
        memset(cfg, 0x0, sizeof(struct gbn_config));
        cfg->N = 16;
        cfg->rto_msec = 1000;
        cfg->probability = 0.2; 
}

void error_handler(const char *message)
{
        fprintf(stderr, "%s.\nError %d: %s\n", message, errno, strerror(errno));
}