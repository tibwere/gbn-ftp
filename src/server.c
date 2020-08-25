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
#include <sys/time.h>
#include <fcntl.h>
#include <math.h>

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
        struct timeval start_timer;
        struct gbn_config cfg;
        pthread_t tid;
};


extern bool verbose;
extern char *optarg;
extern int opterr;


volatile int *quit_conditions;
unsigned short int acceptance_port;
struct worker_info *winfo;
struct gbn_config *config;
fd_set all_fds;


ssize_t send_file_chunk(long id, int fd, char *error_message);
int handle_first_message(long id, char *error_message);
void set_timeout(long index, struct timespec *ts);
void handle_retransmition(long index, int fd, char *error_message);
void *send_worker(void *args);
void exit_server(int status);
enum app_usages parse_cmd(int argc, char **argv, long *tpsize_ptr);
int init_socket(unsigned short int port, char *error_message);
long get_available_worker(long nmemb);
bool start_sender(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, char *error_message);
bool reset_worker_info(int id, bool need_destroy, char *error_message);
bool init_worker_info(long nmemb, char *error_message);
bool handle_recv(int id, char *error_message);
bool acceptance_loop(int acc_socket, long size, char *error_message);
bool check_installation(void); 


ssize_t send_file_chunk(long id, int filedesc, char *error_message)
{
        gbn_ftp_header_t header;
        char buff[CHUNK_SIZE];
        ssize_t rsize;
        ssize_t wsize;

        memset(buff, 0x0, CHUNK_SIZE);

        if ((rsize = read(filedesc, buff, CHUNK_SIZE)) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to read from selected file for %ld-th connection", id);
                return -1;                
        }

        if (rsize > 0) {

                set_message_type(&header, winfo[id].modality);
                set_sequence_number(&header, winfo[id].next_seq_num++);
                set_ack(&header, false);        
                
                if (rsize < CHUNK_SIZE) 
                        set_last(&header, true);
                else
                        set_last(&header, false);

                if ((wsize = gbn_send(winfo[id].socket, header, buff, rsize, &winfo[id].client_sockaddr, &winfo[id].cfg)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send chunk to client for %ld-th connection", id);
                        return -1;
                }

                printf("Inviato un segmento [base = %d - nextseqnum = %d dim = %ld]\n", winfo[id].base, winfo[id].next_seq_num, wsize);

                if (pthread_mutex_lock(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        return -1;
                }

                if (winfo[id].base == winfo[id].next_seq_num) 
                        gettimeofday(&winfo[id].start_timer, NULL);
                
                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        return -1;
                }

                return wsize; 
        }  

        return 0;    
}

int handle_first_message(long id, char *error_message) 
{
        int fd;
        char filename[PATH_SIZE];
        gbn_ftp_header_t header;

        set_ack(&header, true);
        set_sequence_number(&header, 1);
        set_message_type(&header, winfo[id].modality);
        set_last(&header, false);

        if (gbn_send(winfo[id].socket, header, NULL, 0, &winfo[id].client_sockaddr, &winfo[id].cfg) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to send initial ACK for %ld-th connection (ACK)", id);
                return -1;
        }

        memset(filename, 0x0, PATH_SIZE);

        if (winfo[id].modality == LIST) {
                snprintf(filename, PATH_SIZE, "/home/%s/.gbn-ftp-public/.tmp-ls", getenv("USER"));
                if ((fd = open(filename, O_RDONLY)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to open LS tmp file for %ld-th connection", id);
                        return -1;    
                }
        }

        if (winfo[id].modality == GET) {

                snprintf(filename, PATH_SIZE, "/home/%s/.gbn-ftp-public/%s", getenv("USER"), winfo[id].filename);
                if ((fd = open(filename, O_RDONLY)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to retrive requested file for %ld-th connection", id);
                        return -1;
                }
        }

        return fd;
}

void set_timeout(long index, struct timespec *ts)
{
        ts->tv_sec = floor((double) winfo[index].cfg.rto_usec / (double) 1000000);
        ts->tv_nsec = (winfo[index].cfg.rto_usec % 1000000) * 1000;
}

void handle_retransmition(long index, int fd, char *error_message)
{
        //printf("Timer scaduto\n");
        lseek(fd, (winfo[index].base - 1) * CHUNK_SIZE, SEEK_SET);
        winfo[index].next_seq_num = winfo[index].base;

        for (unsigned int i = winfo[index].base; i < winfo[index].next_seq_num; ++i) {
                printf("base: %d - next_seq_num: %d\n", winfo[index].base, winfo[index].next_seq_num);
                send_file_chunk(index, fd, error_message);
        }
}

void *send_worker(void *args)
{
        long id = (long) args;
        char err_mess[ERR_SIZE];
        int fd;
        struct timeval now;
        ssize_t last_write_size;

        struct timespec ts;

        memset(err_mess, 0x0, ERR_SIZE);

        if ((fd = handle_first_message(id, err_mess)) == -1) {
                perr(err_mess);
                goto exit_from_sender_thread;
        }

        gettimeofday(&winfo[id].start_timer, NULL);

        printf("Worker no. %ld [Client connected: %s:%d (OP: %d)]\n", 
                id, 
                inet_ntoa((winfo[id].client_sockaddr).sin_addr), 
                ntohs((winfo[id].client_sockaddr).sin_port),
                winfo[id].modality);

        while (quit_conditions[id] == 0) {

                gettimeofday(&now, NULL);
                if (elapsed_usec(&winfo[id].start_timer, &now) >= winfo[id].cfg.rto_usec) 
                        handle_retransmition(id, fd, err_mess);

                if (pthread_mutex_lock(&winfo[id].mutex)) {
                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }

                while(winfo[id].next_seq_num >= winfo[id].base + winfo[id].cfg.N) {

                        set_timeout(id, &ts); 
                        
                        if (pthread_cond_timedwait(&winfo[id].cond_var, &winfo[id].mutex, &ts)) {
                                snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_condvar_%ld)", id);
                                perr(err_mess);
                                goto exit_from_sender_thread;
                        }

                        if (winfo[id].next_seq_num >= winfo[id].base + winfo[id].cfg.N)
                                handle_retransmition(id, fd, err_mess);
                }

                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }

                if ((last_write_size = send_file_chunk(id, fd, err_mess)) == -1) {
                        perr(err_mess);
                        goto exit_from_sender_thread;
                }                 
                
        }

exit_from_sender_thread:
        printf("Sender %ld is quitting right now\n", id);
        pthread_exit(NULL);
}

void exit_server(int status) 
{
        /* TODO: implementare chiusura pulita */
        exit(status);
}

enum app_usages parse_cmd(int argc, char **argv, long *tpsize_ptr)
{
        verbose = true;
        int opt;

        struct option long_options[] = {
                {"port",        required_argument,      0, 'p'},
                {"wndsize",     required_argument,      0, 'N'},
                {"rtousec",     required_argument,      0, 't'},
                {"adaptive",    no_argument,            0, 'A'},
                {"prob",        required_argument,      0, 'P'},
                {"tpsize",      required_argument,      0, 's'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {"verbose",     no_argument,            0, 'V'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "p:N:t:A:P:s:hvV", long_options, NULL)) != -1) {
                switch (opt) {
                        case 'p':
                                acceptance_port = strtol(optarg, NULL, 10);
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
        int enable = 1;

        memset(&addr, 0x0, sizeof(addr));

        if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor"); 
                return -1;
        }

        if (port != acceptance_port) {
                if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to set option on socket (REUSEADDR)"); 
                        return -1;
                }
        }

        addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
        
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
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

bool start_sender(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, char *error_message)
{
        memcpy(&winfo[index].client_sockaddr, client_sockaddr, sizeof(struct sockaddr_in));

        winfo[index].modality = modality;
        strncpy(winfo[index].filename, payload, CHUNK_SIZE);

        if ((winfo[index].socket = init_socket(winfo[index].port, error_message)) == -1) 
                return false;

        if (pthread_create(&winfo[index].tid, NULL, send_worker, (void *) index)) {
                snprintf(error_message, ERR_SIZE, "Unable to spawn worker threads for %ld-th client", index);
                return false;  
        }

        return true;
}

bool reset_worker_info(int id, bool need_destroy, char *error_message)
{
        if (need_destroy) {
                if (pthread_mutex_destroy(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "Unable to free metadata used by syncronization protocol for %d-th connection", id);
                        return false;
                }
                
                if (pthread_cond_destroy(&winfo[id].cond_var)) {
                        snprintf(error_message, ERR_SIZE, "Unable to free metadata used by syncronization protocol for %d-th connection", id);
                        return false;
                }
        }

        if (pthread_mutex_init(&winfo[id].mutex, NULL)) {
                snprintf(error_message, ERR_SIZE, "Unable to init metadata used by syncronization protocol for %d-th connection", id);
                return false;
        }

        if (pthread_cond_init(&winfo[id].cond_var, NULL)) {
                snprintf(error_message, ERR_SIZE, "Unable to init metadata used by syncronization protocol for %d-th connection", id);
                return false;
        }

        memcpy(&winfo[id].cfg, config, sizeof(struct gbn_config));

        winfo[id].socket = -1;
        winfo[id].base = 1;
        winfo[id].next_seq_num = 1;
        winfo[id].expected_seq_num = 1;
        winfo[id].last_acked_seq_num = 0;
        winfo[id].is_available = true;
        winfo[id].modality = ZERO;

        return true;
}

bool init_worker_info(long nmemb, char *error_message) 
{
        if((winfo = calloc(nmemb, sizeof(struct worker_info))) == NULL) {
                snprintf(error_message, ERR_SIZE, "Unable to allocate metadata for worker threads");
                return false;
        }
                

        for (int i = 0; i < nmemb; ++i) {
                memset(&winfo[i], 0x0, sizeof(struct worker_info));
                winfo[i].port = acceptance_port + i;
                
                if (!reset_worker_info(i, false, error_message)) {
                        free(winfo);
                        return false;
                }
                        
        }

        return true;
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

                printf("Ricevuto ACK %d\n", get_sequence_number(recv_header));

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

                if (is_last(recv_header)) {
                        FD_CLR(winfo[id].socket, &all_fds);
                        quit_conditions[id] = 1;
                        
                        pthread_join(winfo[id].tid, NULL);
                        if (!reset_worker_info(id, true, error_message))
                                return false;

                        
                        printf("Comunication with %d-th client has expired\n", id);
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
        fd_set read_fds;

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

                        if (!start_sender(winfo_index, &addr, get_message_type(header), payload, error_message))
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

                                        --ready_fds;
                                }
                        }
                }

        }

        return true;
}

bool check_installation(void) 
{
        char path[PATH_SIZE];

        memset(path, 0x0, PATH_SIZE);
        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/.tmp-ls", getenv("USER"));
        
        if (access(path, F_OK) != -1)
                return true;
        else   
                return false;
}

int main(int argc, char **argv)
{
        int acceptance_sockfd;
        enum app_usages modality;
        long concurrenty_connections;
        char err_mess[ERR_SIZE];

        if (!check_installation()) {
                fprintf(stderr, "Server not installed yet!\n\nPlease run the following command:\n\tsh /path/to/script/install-server.sh\n");
                exit_server(EXIT_FAILURE);
        }

        memset(err_mess, 0x0, ERR_SIZE);
        srand(time(0));
        concurrenty_connections = sysconf(_SC_NPROCESSORS_ONLN) << 2;

        if((config = init_configurations()) == NULL) {
                perr("Unable to load default configurations for server");
                exit_server(EXIT_FAILURE); 
        }

        if (!init_worker_info(concurrenty_connections, err_mess)) {
                perr(err_mess);
                exit_server(EXIT_FAILURE);
        }  

        if ((quit_conditions = calloc(concurrenty_connections, sizeof(int))) == NULL) {
                perr("Unable to init metadata for syncronization protocol");
                exit_server(EXIT_FAILURE);
        }

        for (int i = 0; i < concurrenty_connections; ++i)
                quit_conditions[i] = 0;

        acceptance_port = DEFAULT_PORT;
        modality = parse_cmd(argc, argv, &concurrenty_connections);

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
                        printf("\t\t-t [--rtousec]\t<timeout>\tRetransmition timeout [usec] (for GBN)\n");
                        printf("\t\t-A [--adaptive]\t\t\tTimer adaptative\n");
                        printf("\t\t-P [--prob]\t<percentage>\tLoss probability\n");
                        printf("\t\t-s [--tpsize]\t<size>\t\tMax number of concurrenty connections\n");
                        printf("\t\t-v [--version]\t\t\tVersion of gbn-ftp-server\n");
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

