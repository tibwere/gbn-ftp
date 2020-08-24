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
#include <fcntl.h>

#include "gbnftp.h" 
#include "common.h"


#define CMD_SIZE 128
#define START_WORKER_PORT 29290


struct worker_info {
        int socket;
        unsigned short port;
        bool is_available;
        struct sockaddr_in client_sockaddr;
        pthread_mutex_t mutex;
        pthread_cond_t cond_var;
        enum message_type modality;
        char filename[CHUNK_SIZE];
        unsigned int base;
        unsigned int next_seq_num;
        unsigned int expected_seq_num;
        unsigned int last_acked_seq_num;
};


extern bool verbose;
extern char *optarg;
extern int opterr;


struct worker_info *winfo;
struct gbn_config *config;


void *send_worker(void *args);
void exit_server(int status);
enum app_usages parse_cmd(int argc, char **argv, long *tpsize_ptr, unsigned short int *port);
int init_socket(unsigned short int port, char *error_message);
long get_available_worker(long nmemb);
bool start_workers(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, char *error_message);
bool acceptance_loop(int acc_socket, long size, char *error_message);
struct worker_info *init_worker_info(long nmemb);
bool handle_recv(int id, char *error_message);


void *send_worker(void *args)
{
        long id = (long) args;
        char err_mess[ERR_SIZE];
        bool is_first = true;
        bool quit = false;
        gbn_ftp_header_t header;
        char chunk_read[CHUNK_SIZE];
        ssize_t size_read;
        int fd;
        char filename[2 * CHUNK_SIZE];

        memset(err_mess, 0x0, ERR_SIZE);

        do {
                if (pthread_mutex_lock(&winfo[id].mutex)) {
                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }

                while(winfo[id].next_seq_num >= winfo[id].base + config->N) {
                        if (pthread_cond_wait(&winfo[id].cond_var, &winfo[id].mutex)) {
                                snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_condvar_%ld)", id);
                                perr(err_mess);
                                goto exit_from_sender_thread;
                        }
                }

                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }

                if (is_first) {
                        set_ack(&header, true);
                        set_sequence_number(&header, 1);
                        set_message_type(&header, winfo[id].modality);
                        set_last(&header, false);

                        if (gbn_send(winfo[id].socket, header, NULL, 0, &winfo[id].client_sockaddr, config) == -1) {
                                snprintf(err_mess, ERR_SIZE, "Unable to send initial ACK for %ld-th connection (ACK)", id);
                                perr(err_mess);
                                goto exit_from_sender_thread;
                        }

                        memset(filename, 0x0, 2 * CHUNK_SIZE);

                        if (winfo[id].modality == LIST) {
                                snprintf(filename, 2 * CHUNK_SIZE, "/home/%s/.gbn-ftp-public/.tmp-ls", getenv("USER"));
                                if ((fd = open(filename, O_RDONLY)) == -1) {
                                        snprintf(err_mess, ERR_SIZE, "Unable to open LS tmp file for %ld-th connection", id);
                                        perr(err_mess);
                                        goto exit_from_sender_thread;    
                                }
                        }

                        if (winfo[id].modality == GET) {

                                snprintf(filename, 2 * CHUNK_SIZE, "/home/%s/.gbn-ftp-public/%s", getenv("USER"), winfo[id].filename);
                                if ((fd = open(filename, O_RDONLY)) == -1) {
                                        snprintf(err_mess, ERR_SIZE, "Unable to retrive requested file for %ld-th connection", id);
                                        perr(err_mess);
                                        goto exit_from_sender_thread;
                                }
                        }


                        is_first = false;
                        printf("Worker no. %ld [Client connected: %s:%d (OP: %d)]\n", 
                                id, 
                                inet_ntoa((winfo[id].client_sockaddr).sin_addr), 
                                ntohs((winfo[id].client_sockaddr).sin_port),
                                winfo[id].modality);

                } else {
                        memset(chunk_read, 0x0, CHUNK_SIZE);

                        set_message_type(&header, winfo[id].modality);
                        set_sequence_number(&header, winfo[id].next_seq_num++);
                        set_ack(&header, false);
                        set_last(&header, false);

                        size_read = read(fd, chunk_read, CHUNK_SIZE);

                        if (size_read == -1) {
                                snprintf(err_mess, ERR_SIZE, "Unable to read from LS tmp file for %ld-th connection", id);
                                perr(err_mess);
                                goto exit_from_sender_thread;     
                        } 
                        
                        if (size_read < CHUNK_SIZE) {
                                set_last(&header, true);
                                quit = true;
                        }
                        
                        if (gbn_send(winfo[id].socket, header, chunk_read, size_read, &winfo[id].client_sockaddr, config) == -1) {
                                snprintf(err_mess, ERR_SIZE, "Unable to send chunk to client for %ld-th connection", id);
                                perr(err_mess);
                                goto exit_from_sender_thread;
                        }
                }                    
                
        } while(!quit);

exit_from_sender_thread:
        pthread_exit(NULL);
}

void exit_server(int status) 
{
        /* TODO: implementare chiusura pulita */
        exit(status);
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
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor"); 
                return -1;
        }

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable_option, sizeof(int))) {
                snprintf(error_message, ERR_SIZE, "Unable to change options on socket just opened");
                return -1;
        }

        addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
        
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to bind address to socket");
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

bool start_workers(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, char *error_message)
{
        pthread_t tid;

        if (pthread_mutex_init(&winfo[index].mutex, NULL)) {
                snprintf(error_message, ERR_SIZE, "Unable to initialize syncronization protocol for worker threads (worker_mutex_%ld)", index);
                return false;
        }
        
        if (pthread_cond_init(&winfo[index].cond_var, NULL)) {
                snprintf(error_message, ERR_SIZE, "Unable to initialize syncronization protocol for worker threads (worker_condvar_%ld)", index);
                return false;
        }

        memcpy(&winfo[index].client_sockaddr, client_sockaddr, sizeof(struct sockaddr_in));

        winfo[index].modality = modality;
        strncpy(winfo[index].filename, payload, CHUNK_SIZE);

        if ((winfo[index].socket = init_socket(winfo[index].port, error_message)) == -1) 
                return false;

        if (pthread_create(&tid, NULL, send_worker, (void *) index)) {
                snprintf(error_message, ERR_SIZE, "Unable to spawn worker threads for %ld-th client", index);
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
                wi[i].base = 1;
                wi[i].next_seq_num = 1;
                wi[i].expected_seq_num = 1;
                wi[i].last_acked_seq_num = 0;
                wi[i].port = START_WORKER_PORT + i;
                wi[i].is_available = true;
                wi[i].modality = ZERO;
        }

        return wi;
}


bool handle_recv(int id, char *error_message) 
{
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];

        memset(payload, 0x0, CHUNK_SIZE);

        if (gbn_receive(winfo[id].socket, &recv_header, payload, &winfo[id].client_sockaddr) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to get message from %d-th client (gbn_receive)", id);
                return false;
        }

        if(is_ack(recv_header)) {

                if (get_sequence_number(recv_header) >= winfo[id].base) {

                        if (pthread_mutex_lock(&winfo[id].mutex)) {
                                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%d)", id);
                                return false;
                        }

                        winfo[id].base = get_sequence_number(recv_header) + 1;

                        if (pthread_mutex_unlock(&winfo[id].mutex)) {
                                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%d)", id);
                                return false;
                        }

                        if (pthread_cond_signal(&winfo[id].cond_var)) {
                                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_condvar_%d)", id);
                                return false;
                        }
                }
        }

        return true;
}


bool acceptance_loop(int acc_socket, long tpsize, char *error_message)
{
        int winfo_index, ready_fds, maxfd;
        struct sockaddr_in addr;
        gbn_ftp_header_t header;
        char payload[CHUNK_SIZE];
        fd_set all_fds, read_fds;

        FD_ZERO(&all_fds);
        FD_SET(acc_socket, &all_fds);
        maxfd = acc_socket;

        while(true) {

                read_fds = all_fds;
                memset(&addr, 0x0, sizeof(struct sockaddr_in));
                memset(payload, 0x0, CHUNK_SIZE);

                if ((ready_fds = select(maxfd + 1, &read_fds, NULL, NULL, NULL)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive command from client (select)");
                        return false;
                }

                if (FD_ISSET(acc_socket, &read_fds)) {
                        
                        if (gbn_receive(acc_socket, &header, payload, &addr) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to receive command from client");
                                return false;
                        }

                        if ((winfo_index = get_available_worker(tpsize)) == -1) {
                                snprintf(error_message, ERR_SIZE, "All workers are busy. Can't handle request");
                                return false;
                        }

                        if (!start_workers(winfo_index, &addr, get_message_type(header), payload, error_message))
                                return false;

                        FD_SET(winfo[winfo_index].socket, &all_fds);

                        if (winfo[winfo_index].socket > maxfd) 
                                maxfd = winfo[winfo_index].socket;

                        if (--ready_fds <= 0)
                                continue;
                }  

                for (int i = 0; (i < tpsize) && (ready_fds > 0); ++i) {
                        if (winfo[i].socket != -1) {
                                if (FD_ISSET(winfo[i].socket, &read_fds)) {
                                        if (!handle_recv(i, error_message))
                                                return false;
                                }

                                --ready_fds;
                        }
                }
        }

        return true;
}

int main(int argc, char **argv)
{
        int acceptance_sockfd;
        unsigned short int acceptance_port;
        enum app_usages modality;
        long concurrenty_connections;
        char err_mess[ERR_SIZE];

        memset(err_mess, 0x0, ERR_SIZE);
        srand(time(0));
        concurrenty_connections = sysconf(_SC_NPROCESSORS_ONLN) << 2;

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
                        abort();        
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

