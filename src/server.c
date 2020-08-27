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
#include <errno.h>

#include "gbnftp.h" 
#include "common.h"


#define ID_STR_LENGTH 1024
#define CMD_SIZE 128
#define START_WORKER_PORT 29290

struct worker_info {
        int socket;
        char id_string[ID_STR_LENGTH];
        unsigned short port;
        volatile enum connection_status status;
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
        int fd;
};


extern bool verbose;
extern char *optarg;
extern int opterr;


unsigned short int acceptance_port;
struct worker_info *winfo;
struct gbn_config *config;
fd_set all_fds;

bool handle_retransmit(long id, char * error_message); 
ssize_t send_file_chunk(long id, char *error_message);
ssize_t send_new_port_mess(long id, char *error_message);
void *send_worker(void *args);
void exit_server(int status);
enum app_usages parse_cmd(int argc, char **argv, long *tpsize_ptr);
int init_socket(unsigned short int port, char *error_message);
long get_available_worker(long nmemb, const struct sockaddr_in *addr, bool *already_handled_ptr);
bool start_sender(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, char *error_message);
bool reset_worker_info(int id, bool need_destroy, char *error_message);
bool init_worker_info(long nmemb, char *error_message);
bool handle_recv(int id, char *error_message);
bool acceptance_loop(int acc_socket, long size, char *error_message);
bool check_installation(void); 


bool handle_retransmit(long id, char * error_message) 
{
        unsigned int base = winfo[id].base - 1;
        unsigned int next_seq_num = winfo[id].next_seq_num;

        printf("Riparto da %ld\n", lseek(winfo[id].fd, base * CHUNK_SIZE, SEEK_SET));
        winfo[id].next_seq_num = winfo[id].base;
        winfo[id].status = CONNECTED;
        gettimeofday(&winfo[id].start_timer, NULL);

        for (unsigned int i = base; i < next_seq_num; ++i)
                if (send_file_chunk(id, error_message) == -1)
                        return false;

        return true;
}

ssize_t send_file_chunk(long id, char *error_message)
{
        gbn_ftp_header_t header;
        char buff[CHUNK_SIZE];
        ssize_t rsize;
        ssize_t wsize;

        memset(buff, 0x0, CHUNK_SIZE);

        if ((rsize = read(winfo[id].fd, buff, CHUNK_SIZE)) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to read from selected file for %ld-th connection", id);
                return -1;                
        }

        if (rsize > 0) {

                set_message_type(&header, winfo[id].modality);
                set_sequence_number(&header, winfo[id].next_seq_num++);
                set_ack(&header, false);        
                
                if (rsize < CHUNK_SIZE) {
                        set_last(&header, true);
                } else {
                        set_last(&header, false);
                }   

                if ((wsize = gbn_send(winfo[id].socket, header, buff, rsize, &winfo[id].client_sockaddr, &winfo[id].cfg)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send chunk to client for %ld-th connection", id);
                        return -1;
                }

                #ifdef DEBUG
                printf("%s Segment no. %d %ssent\n", winfo[id].id_string, get_sequence_number(header), (wsize == 0) ? "not " : "");
                #endif

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

ssize_t send_new_port_mess(long id, char *error_message)
{
        gbn_ftp_header_t header;
        struct timespec ts;
        struct timeval tv;
        int ret;
        ssize_t wsize;

        set_sequence_number(&header, 0);
        set_message_type(&header, winfo[id].modality);
        set_last(&header, false);
        set_ack(&header, false);

        do {
                if ((wsize = gbn_send(winfo[id].socket, header, NULL, 0, &winfo[id].client_sockaddr, &winfo[id].cfg)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send NEW_PORT message to %ld-th client", id);
                        return false;
                }

                #ifdef DEBUG
                printf("%s NEW PORT segment %ssent\n", winfo[id].id_string, (wsize == 0) ? "not " : "");
                #endif

                if (pthread_mutex_lock(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        return false;
                }

                while (winfo[id].status != CONNECTED) {
                        gettimeofday(&tv, NULL);
                        ts.tv_sec = tv.tv_sec + floor((double) winfo[id].cfg.rto_usec / (double) 1000000);
                        ts.tv_nsec = (tv.tv_usec + (winfo[id].cfg.rto_usec % 1000000)) * 1000;
                        
                        ret = pthread_cond_timedwait(&winfo[id].cond_var, &winfo[id].mutex, &ts);
                        
                        if (ret > 0 && ret != ETIMEDOUT) {
                                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_condvar_%ld)", id);
                                return false;
                        } else {
                                break;
                        }
                }

                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex_%ld)", id);
                        return false;
                }  

        } while (winfo[id].status == REQUEST);
        
        return true;
}

void *send_worker(void *args) 
{
        long id = (long) args;
        char err_mess[ERR_SIZE];
        struct timeval tv;
        bool fail = false;
        unsigned short timeout_counter = 0;
        unsigned int last_base_for_timeout = winfo[id].base;

        snprintf(winfo[id].id_string, ID_STR_LENGTH, "[Worker no. %ld - Client connected: %s:%d (OP: %d)]", 
                id, 
                inet_ntoa((winfo[id].client_sockaddr).sin_addr), 
                ntohs((winfo[id].client_sockaddr).sin_port),
                winfo[id].modality);

        do  {
                switch (winfo[id].status) {

                        case REQUEST:
                                if (send_new_port_mess(id, err_mess) == -1) {
                                        fail = true;
                                        winfo[id].status = QUIT;
                                }      
                                break;

                        case CONNECTED:
                                if (winfo[id].next_seq_num < winfo[id].base + winfo[id].cfg.N) {
                                        if (send_file_chunk(id, err_mess) == -1) {
                                                fail = true;
                                                winfo[id].status = QUIT;
                                        }
                                }

                                if (winfo[id].status != QUIT) {
                                        gettimeofday(&tv, NULL);
                                        if (elapsed_usec(&winfo[id].start_timer, &tv) >= winfo[id].cfg.rto_usec)
                                                winfo[id].status = TIMEOUT;
                                }

                                break;

                        case TIMEOUT:

                                #ifdef DEBUG
                                printf("%s Timeout event (%d)\n", winfo[id].id_string, winfo[id].base);
                                #endif       

                                if (winfo[id].base == last_base_for_timeout) {
                                        if (timeout_counter < MAX_TO) {
                                                if (!handle_retransmit(id, err_mess)) {
                                                        fail = true;
                                                        winfo[id].status = QUIT;
                                                }
                                        } else  
                                                winfo[id].status = QUIT;


                                        ++timeout_counter;
                                } else {
                                        timeout_counter = 1;
                                        
                                        if (!handle_retransmit(id, err_mess)) {
                                                fail = true;
                                                winfo[id].status = QUIT;
                                        }

                                }

                                break;
                        
                        default: 
                                break;
                }

        } while (winfo[id].status != QUIT);

        if (fail)
                perr(err_mess);

        #ifdef DEBUG
        printf("%s is quitting right now\n", winfo[id].id_string);
        #endif

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

long get_available_worker(long nmemb, const struct sockaddr_in *addr, bool *already_handled_ptr)
{
        long ret, i;
        
        ret = -1;
        i = 0;

        while (i < nmemb) {
                if (winfo[i].status == FREE) {
                        winfo[i].status = REQUEST;
                        ret = i;
                        break;
                }

                ++i;
        }

        i = 0;
        *already_handled_ptr = false;

        while (i < nmemb) {
                if (memcmp(&winfo[i].client_sockaddr, addr, sizeof(struct sockaddr_in)) == 0)
                        *already_handled_ptr = true;

                ++i;
        }

        return ret;
}

bool start_sender(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, char *error_message)
{
        char path[PATH_SIZE];

        memset(path, 0x0, PATH_SIZE);

        memcpy(&winfo[index].client_sockaddr, client_sockaddr, sizeof(struct sockaddr_in));

        winfo[index].modality = modality;
        strncpy(winfo[index].filename, payload, CHUNK_SIZE);

        if (winfo[index].modality == LIST) {
                snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/.tmp-ls", getenv("USER"));
                if ((winfo[index].fd = open(path, O_RDONLY)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to open LS tmp file for %ld-th connection", index);
                        return false;    
                }
        }

        if (winfo[index].modality == GET) {

                snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/%s", getenv("USER"), winfo[index].filename);
                if ((winfo[index].fd = open(path, O_RDONLY)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to retrive requested file for %ld-th connection", index);
                        return false;
                }
        }

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
        winfo[id].modality = ZERO;
        winfo[id].status = FREE;

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
                winfo[i].port = START_WORKER_PORT + i;
                
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

                #ifdef DEBUG
                printf("%s ACK no. %d received (base: %d)\n", winfo[id].id_string, get_sequence_number(recv_header), winfo[id].base);
                #endif

                if (get_sequence_number(recv_header) == 0) {
                        winfo[id].status = CONNECTED;
                }

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

                if (winfo[id].base == winfo[id].next_seq_num)
                        memset(&winfo[id].start_timer, 0x0, sizeof(struct timeval));
                else    
                        gettimeofday(&winfo[id].start_timer, NULL);

                if (is_last(recv_header)) {
                        winfo[id].status = QUIT;
                        FD_CLR(winfo[id].socket, &all_fds);

                        pthread_join(winfo[id].tid, NULL);

                        if (!reset_worker_info(id, true, error_message))
                                return false;

                        #ifdef DEBUG
                        printf("Comunication with %d-th client has expired\n", id);
                        #endif
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
        bool already_handled;

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

                        #ifdef DEBUG
                        printf("[Main Thread] Received request from %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                        #endif

                        if (!(is_last(header) && !is_ack(header) && (get_sequence_number(header) == 0))) {
                                snprintf(error_message, ERR_SIZE, "Connection protocol broken");
                                return false;
                        }
                        
                        if ((winfo_index = get_available_worker(tpsize, &addr, &already_handled)) == -1) {
                                snprintf(error_message, ERR_SIZE, "All workers are busy. Can't handle request");
                                return false;
                        }

                        if (!already_handled) {
                                #ifdef DEBUG
                                printf("[Main Thread] Accept request from %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                                #endif

                                if (!start_sender(winfo_index, &addr, get_message_type(header), payload, error_message))
                                        return false;

                                FD_SET(winfo[winfo_index].socket, &all_fds);

                                if (winfo[winfo_index].socket > maxfd) 
                                        maxfd = winfo[winfo_index].socket;
                        }

                        #ifdef DEBUG
                        else
                                printf("[Main Thread] Reject request from %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                        #endif

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
        #ifdef DEBUG
        printf("*** DEBUG MODE ***\n\n\n");
        #endif

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

        if (!init_worker_info(concurrenty_connections, err_mess)) {
                perr(err_mess);
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

