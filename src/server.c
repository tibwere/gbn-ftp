#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>

#include "gbnftp.h" 
#include "common.h"

#define FOLDER_PATH "/home/tibwere/.gbn-ftp-public/"

int sockfd;
unsigned short int port;
struct sockaddr_in addr;
struct gbn_config config;

extern const struct gbn_config DEFAULT_GBN_CONFIG;
extern char *optarg;
extern int opterr;

void exit_server(int status) 
{
        /* TODO: implementare chiusura pulita */
        exit(status);
}

enum app_usages parse_cmd(int argc, char **argv, struct gbn_config *conf, long *tpsize_ptr)
{
        int opt;

        struct option long_options[] = {
                {"port",        required_argument,      0, 'p'},
                {"windowsize",  required_argument,      0, 'N'},
                {"rto",         required_argument,      0, 't'},
                {"adaptive",    no_argument,            0, 'A'},
                {"probability", required_argument,      0, 'P'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {"tpsize",      required_argument,      0, 's'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "p:N:t:P:s:hv", long_options, NULL)) != -1) {
                switch (opt) {
                        case 'p':
                                port = strtol(optarg, NULL, 10);
                                break;
                        case 'N':
                                if (strtol(optarg, NULL, 10) < MAX_SEQ_NUMBER / 2)
                                        conf->N = strtol(optarg, NULL, 10);
                                break;
                        case 't':
                                conf->rto_msec = strtol(optarg, NULL, 10);
                                break;
                        case 'A':
                                conf->is_adaptive = true;
                                break;
                        case 'P':
                                conf->probability = strtol(optarg, NULL, 10) / 100;
                                break;
                        case 'h':
                                return (argc != 2) ? ERROR : HELP;
                        case 's':
                                *tpsize_ptr = strtol(optarg, NULL, 10);
                                break;
                        case 'v':
                                return (argc != 2) ? ERROR : VERSION;
                        default:
                                fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                                abort();
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

void main_loop()
{
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        
        // variabili da rimuovere non appena si implementa la reale logica
        char buff[CHUNK_SIZE + sizeof(gbn_ftp_header_t)];
        gbn_ftp_header_t header;
        char payload[CHUNK_SIZE];
        ssize_t size;

        while(true) {

                memset(&client_addr, 0x0, sizeof(client_addr));
                memset(buff, 0x0, CHUNK_SIZE);
                size = recvfrom(sockfd, buff, 1024, 0, (struct sockaddr *)&client_addr, &len);

                if (size < 0) {
                        error_handler("\"recvfrom()\" failed.");
                        break;
                }

                get_segment(buff, &header, payload, size);
                printf("Type received: %d\n", get_message_type(header));
        }
}

int main(int argc, char **argv)
{
        enum app_usages modality;
        long POOL_SIZE = sysconf(_SC_NPROCESSORS_ONLN) << 2;

        memcpy(&config, &DEFAULT_GBN_CONFIG, sizeof(config));
        port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, &config, &POOL_SIZE);

        switch(modality) {
                case STANDARD: 
                        printf("Configs:\n\tN: %u\n\trcvtimeout: %lu\n\tprobability: %.1f\n\tport: %u\n\tadapitve: %s\n\n", 
                                config.N, config.rto_msec, config.probability, port, (config.is_adaptive) ? "true" : "false");
                        break;
                case HELP:
                case VERSION: 
                        printf("Not yet implemented\n");    
                        exit_server(EXIT_FAILURE);
                        break;
                case ERROR:
                        printf("Unable to parse command line.\n");
                        exit_server(EXIT_FAILURE);
                        break;                    
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();        
        }   

        sockfd = setup_server();
        printf("Socket created: %d\n", sockfd);

        main_loop();

        close(sockfd);        
        exit_server(EXIT_SUCCESS);
}

