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
#define ERRSIZE 256
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


extern bool verbose;
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
int init_socket(unsigned short int port, char *error_message);
long get_available_worker(long nmemb);
bool start_workers(long index, struct sockaddr_in *client_sockaddr, char *error_message);
bool acceptance_loop(int acc_socket, long size, char *error_message);
struct worker_info *init_worker_info(long nmemb);


void *recv_worker(void * args) 
{
        long id = (long) args;        
        fd_set read_fds;
        // struct timeval tv;
        int retval;
        char cmd_to_send[CMD_SIZE];
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        char err_mess[ERRSIZE];

        memset(err_mess, 0x0, ERRSIZE);

        while (true) {
                
                FD_ZERO(&read_fds);
                FD_SET(winfo[id].socket, &read_fds);
                // tv.tv_sec = 0;
                // tv.tv_usec = conn_info->configs->rto_usec;

                retval = select(winfo[id].socket + 1, &read_fds, NULL, NULL, NULL/*&tv*/);
                if (retval == -1) {
                        snprintf(err_mess, ERRSIZE, "Unable to get message from %ld-th client (select)", id);
                        perr(err_mess);
                        goto exit_from_receiver_thread;
                } else if (retval) {
                        memset(cmd_to_send, 0x0, CMD_SIZE);
                        memset(payload, 0x0, CHUNK_SIZE);

                        if (gbn_receive(winfo[id].socket, &recv_header, payload, &winfo[id].client_sockaddr) == -1) {
                                snprintf(err_mess, ERRSIZE, "Unable to get message from %ld-th client (gbn_receive)", id);
                                perr(err_mess);
                                goto exit_from_receiver_thread;
                        }

                        snprintf(cmd_to_send, CMD_SIZE, "TEST");
                        
                        if (write(winfo[id].write_pipe_fd, cmd_to_send, CMD_SIZE) == -1) {
                                snprintf(err_mess, ERRSIZE, "Unable to communicate via pipe to %ld-th sender", id);
                                perr(err_mess);
                                goto exit_from_receiver_thread;
                        }

                        if (pthread_mutex_lock(&winfo[id].mutex)) {
                                snprintf(err_mess, ERRSIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                                perr(err_mess);
                                goto exit_from_receiver_thread;
                        }
                        
                        winfo[id].base++;

                        if (pthread_mutex_unlock(&winfo[id].mutex)) {
                                snprintf(err_mess, ERRSIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                                perr(err_mess);
                                goto exit_from_receiver_thread;
                        }

                        if (pthread_cond_signal(&winfo[id].cond_var)) {
                                snprintf(err_mess, ERRSIZE, "Syncronization protocol for worker threads broken (worker_condvar_%ld)", id);
                                perr(err_mess);
                                goto exit_from_receiver_thread;
                        }

                }  else {
                        snprintf(cmd_to_send, CMD_SIZE, "TIMER");

                        if (write(winfo[id].write_pipe_fd, cmd_to_send, CMD_SIZE) == -1) {
                                snprintf(err_mess, ERRSIZE, "Unable to communicate via pipe to %ld-th sender", id);
                                perr(err_mess);
                                goto exit_from_receiver_thread;
                        }
                }
        }
        
exit_from_receiver_thread:
        pthread_exit(NULL);
}

void *send_worker(void *args)
{
        long id = (long) args;
        pthread_t recv_tid;
        fd_set read_fds;
        int retval;
        char buff[CMD_SIZE];
        char err_mess[ERRSIZE];

        memset(err_mess, 0x0, ERRSIZE);

        winfo[id].socket = finalize_connection(id);

        if (pthread_create(&recv_tid, NULL, recv_worker, args) == -1) {
                snprintf(err_mess, ERRSIZE, "Unable to spawn %ld-th receiver thread", id);
                perr(err_mess);
                goto exit_from_sender_thread;
        }

        while(true) {
                FD_ZERO(&read_fds);
                FD_SET(winfo[id].read_pipe_fd, &read_fds);
                memset(buff, 0x0, CMD_SIZE);

                retval = select(winfo[id].read_pipe_fd + 1, &read_fds, NULL, NULL, NULL);

                if (retval == -1) {
                        snprintf(err_mess, ERRSIZE, "Unable to communicate via pipe to %ld-th receiver (select)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                } 
                        
                if (read(winfo[id].read_pipe_fd, buff, CMD_SIZE) == -1) {
                        snprintf(err_mess, ERRSIZE, "Unable to communicate via pipe to %ld-th receiver (read)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }

                if (pthread_mutex_lock(&winfo[id].mutex)) {
                        snprintf(err_mess, ERRSIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }

                while(winfo[id].next_seq_num >= winfo[id].base + config->N) {
                        if (pthread_cond_wait(&winfo[id].cond_var, &winfo[id].mutex)) {
                                snprintf(err_mess, ERRSIZE, "Syncronization protocol for worker threads broken (worker_condvar_%ld)", id);
                                perr(err_mess);
                                goto exit_from_sender_thread;
                        }
                }

                winfo[id].next_seq_num ++;

                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        snprintf(err_mess, ERRSIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }

                printf("Mess: %s (base = %d; next_seq_num = %d)\n", buff, winfo[id].base, winfo[id].next_seq_num);
        }

exit_from_sender_thread:
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
        unsigned int ack_no;
        gbn_ftp_header_t send_header;
        gbn_ftp_header_t recv_header;
        fd_set read_fds;
        int retval;
        char ack_no_str[CHUNK_SIZE];
        bool is_ack;

        set_conn(&send_header, true);
        set_sequence_number(&send_header, winfo[index].next_seq_num++);
        set_message_type(&send_header, ACK_OR_RESP);

        memset(ack_no_str, 0x0, CHUNK_SIZE);

        // da cambiare il null
        if ((fd = init_socket(winfo[index].port, NULL)) == -1)
                return -1;


        while(true) {
                FD_ZERO(&read_fds);
                FD_SET(fd, &read_fds);

                if (gbn_send(fd, send_header, "0", 1, &winfo[index].client_sockaddr, config) == -1) {
                        error_handler("\"gbn_send()\" failed.");
                }

                retval = select(fd + 1, &read_fds, NULL, NULL, NULL);

                if (retval == -1) {
                        error_handler("\"select()\" failed.");
                        return -1;  
                } else {
                        if (gbn_receive(fd, &recv_header, ack_no_str, &winfo[index].client_sockaddr) == -1) {
                                error_handler("\"gbn_receive()\" failed.");
                                return -1;
                        }

                        is_ack = is_ack_pkt(recv_header, ack_no_str, &ack_no);

                        if (!(is_ack && (ack_no == winfo[index].base))) {
                                error_handler("Connection protocol broken.");
                                return -1;
                        }

                        winfo[index].base++;
                        break;

                }
        }


        printf("Worker no. %ld [Client connected: %s:%d]\n", 
                index, 
                inet_ntoa((winfo[index].client_sockaddr).sin_addr), 
                ntohs((winfo[index].client_sockaddr).sin_port));

        return fd;
}

enum app_usages parse_cmd(int argc, char **argv, long *tpsize_ptr, unsigned short int *port)
{

        verbose = true;
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
                {"verbose",     no_argument,            0, 'V'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "p:N:t:P:s:hvV", long_options, NULL)) != -1) {
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
                        case 'V':
                                verbose = true;
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

int init_socket(unsigned short int port, char *error_message)
{
        int fd;
        struct sockaddr_in addr;
        int enable_option = 1;

        memset(&addr, 0x0, sizeof(addr));

        if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERRSIZE, "Unable to get socket file descriptor"); 
                return -1;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable_option, sizeof(int))) {
                snprintf(error_message, ERRSIZE, "Unable to change options on socket just opened");
                return -1;
        }

        addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
        
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                snprintf(error_message, ERRSIZE, "Unable to bind address to socket");
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

bool start_workers(long index, struct sockaddr_in *client_sockaddr, char *error_message)
{
        pthread_t tid;
        int pipe_fds[2];

        if (pthread_mutex_init(&winfo[index].mutex, NULL)) {
                snprintf(error_message, ERRSIZE, "Unable to initialize syncronization protocol for worker threads (worker_mutex_%ld)", index);
                return false;
        }
        
        if (pthread_cond_init(&winfo[index].cond_var, NULL)) {
                snprintf(error_message, ERRSIZE, "Unable to initialize syncronization protocol for worker threads (worker_condvar_%ld)", index);
                return false;
        }

        memcpy(&winfo[index].client_sockaddr, client_sockaddr, sizeof(struct sockaddr_in));

        if (pipe(pipe_fds) == -1) {
                snprintf(error_message, ERRSIZE, "Unable to open a pipe between %ld-th receiver and sender threads", index);
                return false;

        }

        winfo[index].read_pipe_fd = pipe_fds[0];
        winfo[index].write_pipe_fd = pipe_fds[1];

        if (pthread_create(&tid, NULL, send_worker, (void *) index)) {
                snprintf(error_message, ERRSIZE, "Unable to spawn worker threads for %ld-th client", index);
                return false;  
        }

        return true;
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

bool acceptance_loop(int acc_socket, long tpsize, char *error_message)
{
        struct sockaddr_in addr;
        ssize_t received_size;
        gbn_ftp_header_t header;
        int worker_info_index;

	if (pthread_mutex_lock(&tpool_mutex)) {
		snprintf(error_message, ERRSIZE, "Syncronization protocol for worker threads broken (tpool_mutex)");
		return false;
	}

        while(true) {

                while(active_threads >= tpsize) {
			if (pthread_cond_wait(&tpool_cond_var, &tpool_mutex)) {
				snprintf(error_message, ERRSIZE, "Syncronization protocol for worker threads broken (tpool_condvar)");
				return false;	
			}
		}

                if (pthread_mutex_unlock(&tpool_mutex)) {
                        snprintf(error_message, ERRSIZE, "Syncronization protocol for worker threads broken (tpool_mutex)");
                        return false;
                }

                memset(&addr, 0x0, sizeof(struct sockaddr_in));
                
                received_size = gbn_receive(acc_socket, &header, NULL, &addr);

                if (received_size != sizeof(gbn_ftp_header_t)) {
                        snprintf(error_message, ERRSIZE, "3-way handshake protocol for connection broken (SYN)");
                        return false;
                }

                if (is_syn_pkt(header)) {
                        if ((worker_info_index = get_available_worker(tpsize)) == -1) {
                                snprintf(error_message, ERRSIZE, "Unexpected missing available port");
                                return false;
                        }
                
                        if (!start_workers(worker_info_index, &addr, error_message))
                                return false;

                }  

                if (pthread_mutex_lock(&tpool_mutex)) {
                        snprintf(error_message, ERRSIZE, "Syncronization protocol for worker threads broken (tpool_mutex)");
                        return false;
                }

                active_threads ++;
        }

        return true;
}

int main(int argc, char **argv)
{
        int acceptance_sockfd;
        unsigned short int acceptance_port;
        enum app_usages modality;
        long concurrenty_connections;
        char err_mess[ERRSIZE];

        memset(err_mess, 0x0, ERRSIZE);
        srand(time(0));
        concurrenty_connections = sysconf(_SC_NPROCESSORS_ONLN) << 2;
        active_threads = 0;

        if((config = init_configurations()) == NULL) {
                perr("Unable to load default configurations for server");
                exit_server(EXIT_FAILURE); 
        }

        if ((winfo = init_worker_info(concurrenty_connections)) == NULL) {
                perr("Unable to setup metadata for worker threads");
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
                        printf("\t\t-V [--verbose]\t\t\tPrint verbose version of error\n");
                        exit_server(EXIT_SUCCESS);
                        break;
                case VERSION: 
                        printf("\n\tgbn-ftp-server version 1.0 (developed by tibwere)\n\n");    
                        exit_server(EXIT_SUCCESS);
                        break;
                case ERROR:
                        fprintf(stderr, "Wrong argument inserted.\nPlease re-run gbn-ftp-server with -h [--help] option.\n");
                        exit_server(EXIT_FAILURE);
                        break;                    
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        exit_server(EXIT_FAILURE);        
        } 

        if (pthread_mutex_init(&tpool_mutex, NULL) == 0) {
                perr("Unable to initialize syncronization protocol for worker threads (tpool_mutex)");
                exit_server(EXIT_FAILURE);
        }
        
        if (pthread_cond_init(&tpool_cond_var, NULL)) {
                perr("Unable to initialize syncronization protocol for worker threads (tpool_condvar)");
                exit_server(EXIT_FAILURE);
        }

        if ((acceptance_sockfd = init_socket(acceptance_port, err_mess)) == -1) {
                perr(err_mess);
                exit_server(EXIT_FAILURE);
        }

        printf("Server is now listening ...\n");

        if (!acceptance_loop(acceptance_sockfd, concurrenty_connections, err_mess)) {
                perr(err_mess);
                exit_server(EXIT_FAILURE);
        }

        close(acceptance_sockfd);        
        exit_server(EXIT_SUCCESS);
}

