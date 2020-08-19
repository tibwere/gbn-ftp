#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/select.h>

#include "gbnftp.h" 
#include "common.h"


#define CMD_SIZE 16
#define FOLDER_PATH "/home/tibwere/.gbn-ftp-public/"
#define START_WORKER_PORT 29290


struct worker_info {
        int socket;
        unsigned short port;
        bool is_available;
        struct sockaddr_in client_sockaddr;
        unsigned int base;
        unsigned int next_seq_num;
        unsigned int expected_seq_num;
        pthread_mutex_t mutex;
        pthread_cond_t cond_var;
        int read_pipe_fd;
        int write_pipe_fd;
};


extern char *optarg;
extern int opterr;


struct worker_info *winfo;
struct gbn_config *config;
pthread_mutex_t tpool_mutex;
pthread_cond_t tpool_cond_var;
unsigned int active_threads;


void *recv_worker(void *args);
void *send_worker(void *args);
void exit_server(int status);
int finalize_connection(long index);
enum app_usages parse_cmd(int argc, char **argv, long *tpsize_ptr, unsigned short int *port);
int init_socket(unsigned short int port);
long get_available_worker(long nmemb);
void start_workers(long index, struct sockaddr_in *client_sockaddr);
void main_loop(int acc_socket, long size);
struct worker_info *init_worker_info(long nmemb);


void *recv_worker(void * args) 
{
        long id = (long) args;        
        fd_set read_fds;
        // struct timeval tv;
        int retval;
        char buff[CMD_SIZE];
        gbn_ftp_header_t recv_header;
        char message[sizeof(gbn_ftp_header_t)];
        ssize_t recv_size;

        while (true) {
                
                FD_ZERO(&read_fds);
                FD_SET(winfo[id].socket, &read_fds);
                // tv.tv_sec = 0;
                // tv.tv_usec = conn_info->configs->rto_usec;

                retval = select(winfo[id].socket + 1, &read_fds, NULL, NULL, NULL/*&tv*/);
                if (retval == -1) {
                        error_handler("\"select()\" failed.");
                        pthread_exit(NULL);
                } else if (retval) {
                        memset(buff, 0x0, CMD_SIZE);
                        memset(message, 0x0, sizeof(gbn_ftp_header_t));

                        recv_size = gbn_receive(winfo[id].socket, &recv_header, NULL, &winfo[id].client_sockaddr);
                        if (recv_size != sizeof(gbn_ftp_header_t)) {
                                error_handler("\"gbn_receive()\" failed.");
                                break;
                        }

                        switch(get_message_type(recv_header)) {
                                case LIST: snprintf(buff, CMD_SIZE, "LIST"); break;
                                case PUT: snprintf(buff, CMD_SIZE, "PUT"); break;
                                case GET: snprintf(buff, CMD_SIZE, "GET"); break;
                                case ACK_OR_RESP: snprintf(buff, CMD_SIZE, "ACK_OR_RESP"); break;
                                default: break;
                        }

                        write(winfo[id].write_pipe_fd, buff, CMD_SIZE);

                        if (pthread_mutex_lock(&winfo[id].mutex)) {
                                error_handler("\"mutex_lock()\" failed.");
                                pthread_exit(NULL);
                        }
                        
                        winfo[id].base++;

                        if (pthread_mutex_unlock(&winfo[id].mutex)) {
                                error_handler("\"mutex_unlock()\" failed.");
                                pthread_exit(NULL);
                        }

                        if (pthread_cond_signal(&winfo[id].cond_var)) {
                                error_handler("\"cond_signal()\" failed.");
                                pthread_exit(NULL);
                        }

                }  else {
                        snprintf(buff, CMD_SIZE, "TIMER");
                        write(winfo[id].write_pipe_fd, buff, CMD_SIZE);
                }
        }

        pthread_exit(NULL);
}

void *send_worker(void *args)
{
        long id = (long) args;
        pthread_t recv_tid;
        fd_set read_fds;
        int retval;
        char buff[CMD_SIZE];

        printf("Worker no. %ld [Client connected: %s:%d]\n", 
                id, 
                inet_ntoa((winfo[id].client_sockaddr).sin_addr), 
                ntohs((winfo[id].client_sockaddr).sin_port));

        winfo[id].socket = finalize_connection(id);

        pthread_create(&recv_tid, NULL, recv_worker, args);

        while(true) {
                FD_ZERO(&read_fds);
                FD_SET(winfo[id].read_pipe_fd, &read_fds);
                memset(buff, 0x0, CMD_SIZE);

                retval = select(winfo[id].read_pipe_fd + 1, &read_fds, NULL, NULL, NULL);

                if (retval == -1) {
                        error_handler("\"select()\" failed");
                        pthread_exit(NULL);
                } else {
                        read(winfo[id].read_pipe_fd, buff, CMD_SIZE);
                }

                if (pthread_mutex_lock(&winfo[id].mutex)) {
                        error_handler("\"mutex_lock()\" failed.");
                        pthread_exit(NULL);
                }

                while(winfo[id].next_seq_num >= winfo[id].base + config->N) {
                        if (pthread_cond_wait(&winfo[id].cond_var, &winfo[id].mutex)) {
                                error_handler("\"cond_wait()\" failed.");
                                pthread_exit(NULL);
                        }
                }

                winfo[id].next_seq_num ++;

                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        error_handler("\"mutex_lock()\" failed.");
                        pthread_exit(NULL);
                }

                printf("Mess: %s (base = %d; next_seq_num = %d)\n", buff, winfo[id].base, winfo[id].next_seq_num);
        }

        pthread_exit(NULL);
}

void exit_server(int status) 
{
        /* TODO: implementare chiusura pulita */
        exit(status);
}

int finalize_connection(long index)
{
        int fd;
        gbn_ftp_header_t header;
        set_conn(&header, true);

        if ((fd = init_socket(winfo[index].port)) == -1)
                return -1;
        
        if (gbn_send(fd, header, NULL, 0, &winfo[index].client_sockaddr, config) == -1) {
                error_handler("\"gbn_send()\" failed.");
                pause();
        }

        return fd;
}

enum app_usages parse_cmd(int argc, char **argv, long *tpsize_ptr, unsigned short int *port)
{
        int opt;

        struct option long_options[] = {
                {"port",        required_argument,      0, 'p'},
                {"wndsize",     required_argument,      0, 'N'},
                {"rto",         required_argument,      0, 't'},
                {"adaptive",    no_argument,            0, 'A'},
                {"prob",        required_argument,      0, 'P'},
                {"tpsize",      required_argument,      0, 's'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "p:N:t:P:s:hv", long_options, NULL)) != -1) {
                switch (opt) {
                        case 'p':
                                *port = strtol(optarg, NULL, 10);
                                break;
                        case 'N':
                                if (strtol(optarg, NULL, 10) < MAX_SEQ_NUMBER / 2)
                                        config->N = strtol(optarg, NULL, 10);
                                break;
                        case 't':
                                config->rto_usec = strtol(optarg, NULL, 10);
                                break;
                        case 'A':
                                config->is_adaptive = true;
                                break;
                        case 'P':
                                config->probability = (double) strtol(optarg, NULL, 10) / (double) 100;
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

long get_available_worker(long nmemb)
{
        long i = 0;

        while (i < nmemb) {
                if (winfo[i].is_available) {
                        winfo[i].is_available = false;
                        return i;
                }

                ++i;
        }

        return -1;
}

void start_workers(long index, struct sockaddr_in *client_sockaddr)
{
        pthread_t tid;
        int pipe_fds[2];

        pthread_mutex_init(&winfo[index].mutex, NULL);
        
        pthread_cond_init(&winfo[index].cond_var, NULL);

        memcpy(&winfo[index].client_sockaddr, client_sockaddr, sizeof(struct sockaddr_in));

        pipe(pipe_fds);
        winfo[index].read_pipe_fd = pipe_fds[0];
        winfo[index].write_pipe_fd = pipe_fds[1];

        pthread_create(&tid, NULL, send_worker, (void *) index);
}

void main_loop(int acc_socket, long tpsize)
{
        struct sockaddr_in addr;
        ssize_t received_size;
        gbn_ftp_header_t header;
        int worker_info_index;

	if (pthread_mutex_lock(&tpool_mutex)) {
		error_handler("\"mutex_lock()\" failed.");
		return;
	}

        while(true) {

                while(active_threads >= tpsize) {
			if (pthread_cond_wait(&tpool_cond_var, &tpool_mutex)) {
				error_handler("\"cond_wait()\" failed");
				return;	
			}
		}

                if (pthread_mutex_unlock(&tpool_mutex)) {
                        error_handler("\"mutex_unlock()\" failed.");
                        return;
                }

                memset(&addr, 0x0, sizeof(struct sockaddr_in));
                
                received_size = gbn_receive(acc_socket, &header, NULL, &addr);

                if (received_size != sizeof(gbn_ftp_header_t)) {
                        error_handler("\"recvfrom()\" failed.");
                        return;
                }

                if (is_conn(header)) {
                        if ((worker_info_index = get_available_worker(tpsize)) == -1) {
                                //non ci sono piu' porte disponibili
                                break;
                        }
                
                        start_workers(worker_info_index, &addr);
                }  

                if (pthread_mutex_lock(&tpool_mutex)) {
                        error_handler("\"mutex_lock()\" failed.");
                        return;
                }

                active_threads ++;
        }

}

struct worker_info *init_worker_info(long nmemb) 
{
        struct worker_info *wi;

        if((wi = calloc(nmemb, sizeof(struct worker_info))) == NULL)
                return NULL;

        for (int i = 0; i < nmemb; ++i) {
                memset(&wi[i], 0x0, sizeof(struct worker_info));
                wi[i].socket = -1;
                wi[i].base = 0;
                wi[i].next_seq_num = 0;
                wi[i].expected_seq_num = 0;
                wi[i].port = START_WORKER_PORT + i;
                wi[i].is_available = true;
        }

        return wi;
}

int main(int argc, char **argv)
{
        int acceptance_sockfd;
        unsigned short int acceptance_port;
        enum app_usages modality;
        long concurrenty_connections;

        srand(time(0));
        concurrenty_connections = sysconf(_SC_NPROCESSORS_ONLN) << 2;
        active_threads = 0;

        if((config = init_configurations()) == NULL) {
                error_handler("\"init_configurations()\" failed.");
                exit_server(EXIT_FAILURE); 
        }

        if ((winfo = init_worker_info(concurrenty_connections)) == NULL) {
                error_handler("\"init_worker_info()\" failed.");
                exit_server(EXIT_FAILURE);
        }        

        acceptance_port = DEFAULT_PORT;
        modality = parse_cmd(argc, argv, &concurrenty_connections, &acceptance_port);

        switch(modality) {
                case STANDARD: 
                        printf("Configs:\n\tN: %u\n\trcvtimeout: %lu\n\tprobability: %.1f\n\tport: %u\n\tadapitve: %s\n\n", 
                                config->N, config->rto_usec, config->probability, acceptance_port, (config->is_adaptive) ? "true" : "false");
                        break;
                case HELP:
                        printf("\n\tusage: gbn-ftp-server [options]\n");
                        printf("\n\tList of available options:\n");
                        printf("\t\t-p [--port]\t<port>\t\tserver port\n");
                        printf("\t\t-N [--wndsize]\t<size>\t\tWindow size (for GBN)\n");
                        printf("\t\t-t [--rto]\t<timeout>\tRetransmition timeout [usec] (for GBN)\n");
                        printf("\t\t-A [--adaptive]\t\t\tTimer adaptative\n");
                        printf("\t\t-P [--prob]\t<percentage>\tLoss probability\n");
                        printf("\t\t-s [--tpsize]\t<size>\t\tMax number of concurrenty connections\n");
                        printf("\t\t-h [--version]\t\t\tVersion of gbn-ftp-server\n");
                        exit_server(EXIT_SUCCESS);
                        break;
                case VERSION: 
                        printf("\n\tgbn-ftp-server version 1.0 (developed by tibwere)\n\n");    
                        exit_server(EXIT_SUCCESS);
                        break;
                case ERROR:
                        printf("Unable to parse command line.\n");
                        exit_server(EXIT_FAILURE);
                        break;                    
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();        
        } 

        pthread_mutex_init(&tpool_mutex, NULL);
        pthread_cond_init(&tpool_cond_var, NULL);

        
        if ((acceptance_sockfd = init_socket(acceptance_port)) == -1) {
                error_handler("\"init_socket()\" failed");
        }

        printf("Server listening on fd %d\n", acceptance_sockfd);

        main_loop(acceptance_sockfd, concurrenty_connections);

        close(acceptance_sockfd);        
        exit_server(EXIT_SUCCESS);
}

