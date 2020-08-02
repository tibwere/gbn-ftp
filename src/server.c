#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include "gbn.h" 
#include "common.h"

int sockfd;
unsigned short int port;
struct sockaddr_in addr;

extern const struct gbn_config DEFAULT_GBN_CONFIG;
extern char *optarg;
extern int opterr;

enum app_usages parse_cmd(int argc, char **argv, struct gbn_config *conf)
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
                                port = strtol(optarg, NULL, 10);
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
        enum app_usages modality;
        char buff[CHUNK_SIZE];
        struct sockaddr_in client_addr;
        socklen_t len;

        len = sizeof(client_addr);

        memcpy(&config, &DEFAULT_GBN_CONFIG, sizeof(config));
        port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, &config);

        switch(modality) {
                case STANDARD: 
                        printf("Configs:\n\tN: %u\n\trcvtimeout: %lu\n\tprobability: %.1f\n\tport: %u\n\tadapitve: %s\n\n", 
                                config.N, config.rto_msec, config.probability, port, (config.is_adaptive) ? "true" : "false");
                        break;
                default:
                        printf("Not yet implemented\n");             
        }   

        sockfd = setup_server();
        printf("Socket created: %d\n", sockfd);

        while(true) {

                memset(&client_addr, 0x0, sizeof(client_addr));
                memset(buff, 0x0, 1024);

                if ((recvfrom(sockfd, buff, 1024, 0, (struct sockaddr *)&client_addr, &len)) < 0) {
                        error_handler("\"recvfrom()\" failed.");
                        break;
                }

                printf("Received: %s\n", buff);
        }

        close(sockfd);        
        return 0;
}

