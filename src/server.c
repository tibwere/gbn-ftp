#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include "gbn.h" 
#include "common.h"

int sockfd;
unsigned short int port;
socklen_t len;
struct sockaddr_in addr;

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

        default_gbn_configuration(&config);

        modality = parse_cmd(argc, argv, &config, &port);

        switch(modality) {
                case STANDARD: 
                        printf("Configs:\n\tN: %u\n\tt: %lu\n\tP: %f\n\tp: %u\n", 
                                config.N, config.rto_msec, config.probability, port);
                        break;
                default:
                        printf("Not yet implemented\n");             
        }   

        sockfd = setup_server();

        printf("Socket created: %d\n", sockfd);

        close(sockfd);        
        return 0;
}

