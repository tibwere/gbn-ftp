#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <pthread.h>

#include "gbnftp.h" 
#include "common.h"


#define FOLDER_PATH "/home/tibwere/.gbn-ftp-public/"
#define START_WORKER_PORT 29290
#define PORT_NOT_AVAILABLE 29289

struct worker_port {
        unsigned short int port;
        bool is_avalaible;
};

struct thread_arguments {
        unsigned short int port;
        struct sockaddr_in client_sockaddr;
};


extern const struct gbn_config DEFAULT_GBN_CONFIG;
extern char *optarg;
extern int opterr;


void exit_server(int status);
int finalize_connection(struct thread_arguments *targs);
void * worker(void *args);
enum app_usages parse_cmd(int argc, char **argv, struct gbn_config *conf, long *tpsize_ptr, unsigned short int *port);
int init_socket(unsigned short int port);
struct worker_port *init_ports(long size);
unsigned short int get_available_port(struct worker_port *ports, long size);
void main_loop(int sockfd, struct worker_port *ports, long size);


void exit_server(int status) 
{
        /* TODO: implementare chiusura pulita */
        exit(status);
}

int finalize_connection(struct thread_arguments *targs)
{
        int fd;
        gbn_ftp_header_t header;
        set_conn(&header, true);

        if ((fd = init_socket(targs->port)) == -1)
                return -1;

        char *conn_message = make_segment(header, NULL, 0);
        sendto(fd, conn_message, sizeof(gbn_ftp_header_t), MSG_NOSIGNAL, (struct sockaddr *) &targs->client_sockaddr, sizeof(struct sockaddr_in));   
        return fd;
}

void * worker(void *args) 
{
        struct thread_arguments *conn_info = (struct thread_arguments *)args;
        int sockfd = finalize_connection(conn_info);

        printf("Successfully connected (fd = %d)\n", sockfd);

        pause();

        return NULL;
}

enum app_usages parse_cmd(int argc, char **argv, struct gbn_config *conf, long *tpsize_ptr, unsigned short int *port)
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
                                *port = strtol(optarg, NULL, 10);
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

int init_socket(unsigned short int port)
{
        int fd;
        struct sockaddr_in addr;
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

struct worker_port *init_ports(long size)
{
        struct worker_port *p;
        
        if ((p = calloc(size, sizeof(struct worker_port))) == NULL)
                return NULL;
        
        for (int i = 0; i < size; ++i) {
                p[i].port = START_WORKER_PORT + i;
                p[i].is_avalaible = true;
        }       

        return p;
}

unsigned short int get_available_port(struct worker_port *ports, long size)
{
        long i = 0;

        while (i < size) {
                if (ports[i].is_avalaible) {
                        ports[i].is_avalaible = false;
                        return ports[i].port;
                }

                ++i;
        }

        return PORT_NOT_AVAILABLE;
}

void main_loop(int sockfd, struct worker_port *ports, long tpsize)
{
        ssize_t recv_size;
        char buff[sizeof(gbn_ftp_header_t)];
        struct sockaddr_in client_sockaddr;
        socklen_t len = sizeof(client_sockaddr);
        gbn_ftp_header_t header;
        pthread_t dummy;
        struct thread_arguments *args;
        unsigned short int worker_port;

        while(true) {
                memset(&client_sockaddr, 0x0, sizeof(client_sockaddr));
                memset(buff, 0x0, sizeof(gbn_ftp_header_t));
                memset(&args, 0x0, sizeof(struct thread_arguments));

                recv_size = recvfrom(sockfd, buff, sizeof(gbn_ftp_header_t), 0, (struct sockaddr *)&client_sockaddr, &len);
                if (recv_size != sizeof(gbn_ftp_header_t)) {
                        error_handler("\"recvfrom()\" failed.");
                        break;
                }
                
                get_segment(buff, &header, NULL, recv_size);

                if (is_conn(header)) {
                        if ((worker_port = get_available_port(ports, tpsize)) == PORT_NOT_AVAILABLE) {
                                //non ci sono piu' porte disponibili
                                break;
                        }
                
                        if ((args = malloc(sizeof(struct thread_arguments))) == NULL) {
                                error_handler("\"malloc()\" failed.");
                                break; 
                        }
                        args->port = worker_port;
                        args->client_sockaddr = client_sockaddr;
                        if (pthread_create(&dummy, NULL, worker, args)) {
                                error_handler("\"pthread_create()\" failed.");
                                break;   
                        }
                }   
        }
}

int main(int argc, char **argv)
{
        int acceptance_sockfd;
        unsigned short int acceptance_port;
        struct worker_port *worker_ports;
        struct gbn_config config;
        enum app_usages modality;

        long concurrenty_connections = sysconf(_SC_NPROCESSORS_ONLN) << 2;

        memcpy(&config, &DEFAULT_GBN_CONFIG, sizeof(config));
        acceptance_port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, &config, &concurrenty_connections, &acceptance_port);

        switch(modality) {
                case STANDARD: 
                        printf("Configs:\n\tN: %u\n\trcvtimeout: %lu\n\tprobability: %.1f\n\tport: %u\n\tadapitve: %s\n\n", 
                                config.N, config.rto_msec, config.probability, acceptance_port, (config.is_adaptive) ? "true" : "false");
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

        worker_ports = init_ports(concurrenty_connections);
        acceptance_sockfd = init_socket(acceptance_port);

        main_loop(acceptance_sockfd, worker_ports, concurrenty_connections);

        close(acceptance_sockfd);        
        exit_server(EXIT_SUCCESS);
}

