#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include "gbn.h" 
#include "common.h"

#define ADDRESS_STRING_LENGTH 1024

int sockfd;
unsigned short int server_port;
struct sockaddr_in server_addr;

extern const struct gbn_config DEFAULT_GBN_CONFIG;
extern char *optarg;
extern int opterr;

enum app_usages parse_cmd(int argc, char **argv, struct gbn_config *conf, char *address)
{
        int opt;
        bool valid_cmd = false;

        struct option long_options[] = {
                {"address",     required_argument,      0, 'a'},
                {"port",        required_argument,      0, 'p'},
                {"windowsize",  required_argument,      0, 'N'},
                {"rto",         required_argument,      0, 't'},
                {"probability", required_argument,      0, 'P'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "a:p:N:t:P:hv", long_options, NULL)) != -1) {
                switch(opt) {
                        case 'a':
                                strncpy(address, optarg, ADDRESS_STRING_LENGTH);
                                valid_cmd = true;
                                break;
                        case 'p':
                                server_port = strtol(optarg, NULL, 10);
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

        return (valid_cmd) ? STANDARD : ERROR;
}

int connect_to_server(const char *address_string)
{
        int fd;

        if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                error_handler("\"socket()\" failed."); 
                return -1;
        }

        memset(&server_addr, 0x0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, address_string, &server_addr.sin_addr) <= 0) {
                error_handler("\"inet_pton()\" failed."); 
                return -1;
        }        
	
	if (connect(fd, (struct sockaddr *) &server_addr, sizeof(struct sockaddr_in)) == -1) {
                error_handler("\"connect()\" failed.");
                return -1;
        }
	
	return fd;
}

int main(int argc, char **argv)
{
        struct gbn_config config;
        enum app_usages modality;
        char address_string[ADDRESS_STRING_LENGTH];
        char buff[CHUNK_SIZE];

        memset(address_string, 0x0, ADDRESS_STRING_LENGTH);

        memcpy(&config, &DEFAULT_GBN_CONFIG, sizeof(config));
        server_port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, &config, address_string);

        switch(modality) {
                case STANDARD: 
                        printf("Configs:\n\tN: %u\n\trcvtimeout: %lu\n\tprobability: %.1f\n\taddress: %s\n\tport: %u\n\tadapitve: %s\n\n", 
                                config.N, config.rto_msec, config.probability, address_string, server_port, (config.is_adaptive) ? "true" : "false");
                        break;
                default:
                        printf("Not yet implemented\n");             
        }   

        sockfd = connect_to_server(address_string);
                
        printf("Socket created: %d\n", sockfd);

        while(true) {
                memset(buff, 0x0, CHUNK_SIZE);

                /*TODO: aggiungere funzione di lettura migliore (da BD)*/
                scanf("%s", buff);
                send(sockfd, buff, 1024, 0);
                printf("message sent\n");
        }

        close(sockfd);  
        return 0;
}

