#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include "gbn.h" 
#include "common.h"

#define DEFAULT_PORT 2929

int sockfd;
unsigned short int port;
socklen_t len;
struct sockaddr_in addr;

extern const struct gbn_config default_config;

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

int setup_server()
{
        int fd;
        int enable_option = 1;

        memset(&addr, 0x0, sizeof(addr));

        if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                error_handler("\"socket()\" failed."); 
                return -1;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable_option, sizeof(int))) {
                error_handler("\"setsockopt(REUSEADDR)\" failed.");
                return -1;
        }

        addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
        
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                error_handler("\"bind()\" failed.");
                return -1;
        }

        return fd;
}

int main(int argc, char **argv)
{
        struct gbn_config config;
        int port;
        enum app_usages modality;

        memcpy(&config, &default_config, sizeof(config));
        port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, &config, &port);

        switch(modality) {
                case STANDARD: 
                        printf("Configs:\n\tN: %u\n\trcvtimeout: %lu\n\tprobability: %f\n\tport: %u\n\tadapitve: %s\n\n", 
                                config.N, config.rto_msec, config.probability, port, (config.is_adaptive) ? "true" : "false");
                        break;
                default:
                        printf("Not yet implemented\n");             
        }   

        sockfd = setup_server();

        printf("Socket created: %d\n", sockfd);

        close(sockfd);        
        return 0;
}

