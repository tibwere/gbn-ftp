#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

extern int opterr;

struct gbn_config {
        unsigned int N;
        unsigned long rto_msec;
        float probability;
};

int parse_cmd(int argc, char **argv, struct gbn_config *conf, int *port)
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
                }
        }

        return 0;
}


void default_gbn_configuration(struct gbn_config* cfg)
{
        memset(cfg, 0x0, sizeof(struct gbn_config));
        cfg->N = 16;
        cfg->rto_msec = 1000;
        cfg->probability = 0.2; 
}


int main(int argc, char **argv)
{
        struct gbn_config config;
        int port;

        default_gbn_configuration(&config);

        parse_cmd(argc, argv, &config, &port);
        
        printf("Configs:\n\tN: %u\n\tt: %lu\n\tP: %f\n\tp: %u\n",
                        config.N, config.rto_msec, config.probability, port);

        return 0;
}

