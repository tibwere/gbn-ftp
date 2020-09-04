#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <sys/select.h>
#include <fcntl.h>
#include <math.h>
#include <libgen.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>

#include "gbnftp.h" 
#include "common.h"


#define ADDRESS_STRING_LENGTH 1024
#define HEADER_FILE_LENGTH 241

#ifndef DEBUG
        #define cls() printf("\033[2J\033[H")
#else   
        #define cls() 
#endif

#define l_request_loop(status_ptr) request_loop(-1, LIST, NULL, status_ptr, NULL)
#define g_request_loop(writefd, filename, status_ptr, delete_file) request_loop(writefd, GET, filename, status_ptr, delete_file)
#define p_request_loop(writefd, filename, status_ptr, already_exists) request_loop(writefd, PUT, filename, status_ptr, already_exists)


struct put_args {
        int fd;                                 /* descrittore del file su cui si deve operare */
        unsigned int base;                      /* variabile base del protocollo gbn associata alla connessione */
        unsigned int next_seq_num;              /* variabile next_seq_num del protocollo gbn associata alla connessione */
        unsigned int expected_seq_num;          /* variabile expected_seq_num del protocollo gbn associata alla connessione */
        unsigned int last_acked_seq_num;        /* variabile last_acked_seq_num del protocollo gbn associata alla connessione */
        pthread_mutex_t mutex;                  /* mutex utilizzato per la sincronizzazione fra main thread e servente */
        enum connection_status status;          /* stato della connessione {FREE, REQUEST, CONNECTED, TIMEOUT, QUIT} */
        struct timeval start_timer;             /* struttura utilizzata per la gestione del timer */
        pthread_t tid;                          /* ID del thread servente */
};


extern bool verbose;
extern char *optarg;
extern int opterr;


int sockfd;
struct gbn_config *config;
struct sockaddr_in request_sockaddr;
unsigned short int server_port;
struct put_args *args;
struct gbn_adaptive_timeout *adapt;
sigset_t t_set;
char header[HEADER_FILE_LENGTH];
enum app_usages modality;


void sig_handler(int signo); 
ssize_t send_file_chunk(void);
bool handle_retransmit(void);
void *put_sender_routine(void *dummy);
void exit_client(int status); 
enum app_usages parse_cmd(int argc, char **argv, char *address);
bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port);
ssize_t send_request(enum message_type type, const char *filename, size_t filename_length);
ssize_t send_ack(enum message_type type, unsigned int seq_num, bool is_last);
bool request_loop(int writefd, enum message_type type, const char *filename, enum connection_status *status_ptr, bool *error_ret);
bool lg_connect_loop(int writefd, enum message_type type, enum connection_status *status_ptr);
bool p_connect_loop(void);
bool list(void);
bool get_file(void);
bool init_put_args(void);
bool dispose_put_args(void);
bool put_file(void);
bool check_installation(void);
bool load_header();

#ifdef DEBUG
void sig_handler(int signo) 
#else
void sig_handler(__attribute__((unused)) int signo)
#endif
{
        #ifdef DEBUG
        printf("\n\n{DEBUG} [Main Thread] Captured %d signal\n", signo);
        #endif

        exit_client(EXIT_SUCCESS);
}

ssize_t send_file_chunk(void)
{
        gbn_ftp_header_t header;
        char buff[CHUNK_SIZE];
        ssize_t rsize;
        ssize_t wsize;

        memset(buff, 0x0, CHUNK_SIZE);

        if ((rsize = read(args->fd, buff, CHUNK_SIZE)) == -1) {
                perr("{ERROR} [Sender Thread] Unable to read from selected file");
                return -1;                
        }

        if (rsize > 0) {

                set_message_type(&header, PUT);
                set_sequence_number(&header, args->next_seq_num);
                set_ack(&header, false);      
                set_err(&header, false);
                set_last(&header, (rsize < CHUNK_SIZE));   

                if ((wsize = gbn_send(sockfd, header, buff, rsize, NULL)) == -1) {
                        perr("{ERROR} [Sender Thread] Unable to send chunk to server");
                        return -1;
                }

                #ifdef DEBUG
                printf("{DEBUG} [Sender Thread] Segment no. %d %ssent\n", get_sequence_number(header), (wsize == 0) ? "not " : "");
                #endif

                if (pthread_mutex_lock(&args->mutex)) {
                        perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                        return -1;
                }

                if (config->is_adaptive) {
                        if (adapt->restart) {
                                gettimeofday(&adapt->saved_tv, NULL);
                                adapt->seq_num = args->next_seq_num;
                                adapt->restart = false;
                        }
                }

                if (args->base == args->next_seq_num) 
                        gettimeofday(&args->start_timer, NULL);

                if (pthread_mutex_unlock(&args->mutex)) {
                        perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                        return -1;
                }

                args->next_seq_num++;

                return wsize; 
        }  

        return 0;    
}

bool handle_retransmit(void) 
{
        unsigned int base; 
        unsigned int next_seq_num;

        if (pthread_mutex_lock(&args->mutex)) {
                perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                return -1;
        }

        base = args->base - 1;
        next_seq_num = args->next_seq_num - 1;
        args->next_seq_num = args->base;
        lseek(args->fd, base * CHUNK_SIZE, SEEK_SET);
        gettimeofday(&args->start_timer, NULL);

        if (pthread_mutex_unlock(&args->mutex)) {
                perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                return -1;
        }

        for (unsigned int i = base; i < next_seq_num; ++i)
                if (send_file_chunk() == -1)
                        return false;

        set_status_safe(&args->status, CONNECTED, &args->mutex);

        return true;
}

void *put_sender_routine(__attribute__((unused)) void *dummy) 
{
        struct timeval tv;
        unsigned short timeout_counter = 0;
        unsigned int last_base_for_timeout = get_gbn_param_safe(&args->base, &args->mutex);

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                perr("{ERROR} [Sender Thread] Unable to set sigmask for worker thread");
                pthread_exit(NULL); 
        }

        while (get_status_safe(&args->status, &args->mutex) != QUIT) {
                
                switch (get_status_safe(&args->status, &args->mutex)) {

                        case CONNECTED:

                                if (can_send_more_segment_safe(&args->base, &args->next_seq_num, config->N, &args->mutex)) {
                                        if (send_file_chunk() == -1)
                                                set_status_safe(&args->status, QUIT, &args->mutex);
                                }

                                if (get_status_safe(&args->status, &args->mutex) != QUIT) {
                                        gettimeofday(&tv, NULL);

                                        if (config->is_adaptive) {
                                                if (elapsed_usec(&args->start_timer, &tv) >= get_adaptive_rto_safe(adapt, &args->mutex))
                                                        set_status_safe(&args->status, TIMEOUT, &args->mutex);
                                        } else {
                                                if (elapsed_usec(&args->start_timer, &tv) >= config->rto_usec)
                                                        set_status_safe(&args->status, TIMEOUT, &args->mutex);
                                        }
                                }

                                break;

                        case TIMEOUT:

                                #ifdef DEBUG
                                printf("{DEBUG} [Sender Thread] Timeout event (%d)\n", args->base);
                                #endif       

                                if (get_gbn_param_safe(&args->base, &args->mutex) == last_base_for_timeout) {

                                        if (timeout_counter < MAX_TO) {
                                                if (!handle_retransmit())
                                                        set_status_safe(&args->status, QUIT, &args->mutex);
                                        } else  {
                                                #ifdef DEBUG
                                                printf("{DEBUG} [Sender Thread] Maximum number of timeout reached for %d, abort\n", args->base);
                                                #endif

                                                set_status_safe(&args->status, QUIT, &args->mutex);      
                                        }

                                        ++timeout_counter;
                                } else {

                                        timeout_counter = 1;
                                        
                                        if (!handle_retransmit())
                                                set_status_safe(&args->status, QUIT, &args->mutex);

                                        last_base_for_timeout = get_gbn_param_safe(&args->base, &args->mutex);
                                }

                                break;
                        
                        default: 
                                break;
                }

        }

        #ifdef DEBUG
        printf("{DEBUG} [Sender Thread] is quitting right now\n");
        #endif

        pthread_exit(NULL);
}

void exit_client(int status) 
{
        dispose_put_args();

        if (config) {
                if (config->is_adaptive) {
                        if (adapt)
                                free(adapt);
                }
                free(config);
        }

        if (args) 
                free(args);

        close(sockfd);

        if (modality == STANDARD)
                printf("\nBye bye\n\n");
        exit(status);
}

enum app_usages parse_cmd(int argc, char **argv, char *address)
{
        int opt;
        bool valid_cmd = false;
        verbose = false;

        struct option long_options[] = {
                {"address",     required_argument,      0, 'a'},
                {"port",        required_argument,      0, 'p'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {"verbose",     no_argument,            0, 'V'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "a:p:hvV", long_options, NULL)) != -1) {
                switch (opt) {
                        case 'a':
                                strncpy(address, optarg, ADDRESS_STRING_LENGTH);
                                valid_cmd = true;
                                break;
                        case 'p':
                                server_port = strtol(optarg, NULL, 10);
                                break;
                        case 'V':
                                verbose = true;
                                break;
                        case 'h':
                                return (argc != 2) ? ERROR : HELP;
                        case 'v':
                                return (argc != 2) ? ERROR : VERSION;
                        default:
                                fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                                abort();
                }
        }

        return (valid_cmd) ? STANDARD : ERROR;
}

bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port)
{
        memset(server_sockaddr, 0x0, sizeof(struct sockaddr_in));
	server_sockaddr->sin_family = AF_INET;
	server_sockaddr->sin_port = htons(port);
        
        if (inet_pton(AF_INET, address_string, &server_sockaddr->sin_addr) <= 0) {
                perr("{ERROR} [Main Thread] Unable to convert address from string to internal logical representation"); 
                return false;
        } 

        return true;
}

ssize_t send_request(enum message_type type, const char *filename, size_t filename_length)
{
        gbn_ftp_header_t header;
        ssize_t wsize;
        char error_message[ERR_SIZE];

        memset(error_message, 0x0, ERR_SIZE);

        set_sequence_number(&header, 0);
        set_last(&header, true);
        set_err(&header, false);
        set_ack(&header, false);
        set_message_type(&header, type);

        if ((wsize = gbn_send(sockfd, header, filename, filename_length, &request_sockaddr)) == -1) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to send request command to server (OP %d)", type);
                perr(error_message);
                return -1;
        }

        #ifdef DEBUG
        printf("{DEBUG} [Main Thread] Request segment %ssent\n", (wsize == 0) ? "not " : "");
        #endif
        
        return wsize;
}

ssize_t send_ack(enum message_type type, unsigned int seq_num, bool is_last)
{
        gbn_ftp_header_t header;
        ssize_t wsize;
        char error_message[ERR_SIZE];

        memset(error_message, 0x0, ERR_SIZE);

        set_sequence_number(&header, seq_num);
        set_last(&header, is_last);
        set_ack(&header, true);
        set_message_type(&header, type);

        if ((wsize = gbn_send(sockfd, header, NULL, 0, NULL)) == -1) {
                if (errno != ECONNREFUSED) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to send ack to server (OP %d)", type);
                        perr(error_message);
                        return -1;
                }                
        }

        #ifdef DEBUG
        printf("{DEBUG} [Main Thread] ACK no. %d %ssent\n", seq_num, (wsize == 0) ? "not " : "");
        #endif
        
        return wsize;
}

bool request_loop(int writefd, enum message_type type, const char *filename, enum connection_status *status_ptr, bool *error_ret)
{
        struct timeval tv;
        int retval;
        ssize_t recv_size;
        gbn_ftp_header_t recv_header;
        struct sockaddr_in addr;
        size_t header_size = sizeof(gbn_ftp_header_t);
        fd_set read_fds, std_fds;
        char config_ser[CHUNK_SIZE];

        FD_ZERO(&std_fds);
        FD_SET(sockfd, &std_fds);

        memset(&addr, 0x0, sizeof(struct sockaddr_in));
        memset(config_ser, 0x0, CHUNK_SIZE);

        while(*status_ptr == REQUEST) {

                if (send_request(type, filename, (filename) ? strlen(filename) : 0) == -1) 
                        return false; 

                read_fds = std_fds;
                tv.tv_sec = floor((double) config->rto_usec / (double) 1000000);
                tv.tv_usec = config->rto_usec % 1000000;

                if ((retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv)) == -1) {
                        perr("{ERROR} [Main Thread] Unable to get NEW_PORT message from server (select)");
                        return false;
                }

                if (retval) {

                        if ((recv_size = gbn_receive(sockfd, &recv_header, config_ser, &addr)) == -1) {
                                perr("{ERROR} [Main Thread] Unable to get NEW_PORT message from server (gbn_receive)");
                                return false;
                        }

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == CHUNK_SIZE) && !is_err(recv_header)) {

                                *status_ptr = CONNECTED;
                                deserialize_configuration(config, config_ser);
                                printf("%d - %ld - %d\n", config->N, config->rto_usec, config->is_adaptive);
                                #ifdef DEBUG
                                printf("{DEBUG} [Main Thread] Received NEW PORT message [Config: N %d; rto %ld usec (%s)]\n", 
                                        config->N, config->rto_usec, (config->is_adaptive) ? "adaptive" : "fixed");
                                #endif

                                if (connect(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
                                        perr("{ERROR} [Main Thread] Connection to server failed");
                                        return false;
                                }
                                if (send_ack(type, 0, false) == -1)
                                        return false;
                        }

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == 0) && is_err(recv_header)) {
                                
                                #ifdef DEBUG
                                printf("{DEBUG} [Main Thread] Received error message\n");
                                #endif

                                if (writefd != -1)
                                        close(writefd);
                                
                                if (type != LIST)
                                        *error_ret = true;

                                return false;
                        }
                }
        }

        return true;
}

bool lg_connect_loop(int writefd, enum message_type type, enum connection_status *status_ptr)
{
        unsigned int expected_seq_num, last_acked_seq_num;
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        ssize_t recv_size;
        size_t header_size = sizeof(gbn_ftp_header_t);

        expected_seq_num = 1;
        last_acked_seq_num = 0;

        while (*status_ptr == CONNECTED) {
                memset(payload, 0x0, CHUNK_SIZE);

                if((recv_size = gbn_receive(sockfd, &recv_header, payload, NULL)) == -1) {
                        perr("{ERROR} [Main Thread] Unable to receive pkt from server (gbn_receive)");
                        return false;
                }

                #ifdef DEBUG
                printf("{DEBUG} [Main Thread] Received chunk no. %d (DIM. %ld)\n", get_sequence_number(recv_header), recv_size - header_size);
                #endif

                if(get_sequence_number(recv_header) == expected_seq_num) {

                        last_acked_seq_num = expected_seq_num;

                        if (write(writefd, payload, recv_size - header_size) == -1) {
                                perr("{ERROR} [Main Thread] Unable to print out info received from server");
                                return false;  
                        }

                        if (send_ack(type, expected_seq_num++, is_last(recv_header)) == -1)
                                return false;

                        if (is_last(recv_header)) {
                                *status_ptr = QUIT;
                        
                                // Al termine invio 4 ACK per aumentare la probabilità di ricezione da parte del server
                                for (int i = 0; i < LAST_MESSAGE_LOOP - 1; ++i) {
                                        if (send_ack(type, last_acked_seq_num, true) == -1) {
                                                if (errno == ECONNREFUSED)
                                                        break;
                                                else
                                                        return false;
                                        }
                                }
                        }

                } else {
                        if (send_ack(type, last_acked_seq_num, false) == -1)
                                return false;
                } 
        }

        return true;
}

bool p_connect_loop(void)
{
        gbn_ftp_header_t recv_header;
        struct timeval tv;

        while (args->status == CONNECTED) {

                if (gbn_receive(sockfd, &recv_header, NULL, NULL) == -1) {
                        perr("{ERROR} [Main Thread] Unable to get ACK message from server");
                        return false;
                }

                if(!is_ack(recv_header)) {
                        perr("{ERROR} [Main Thread] Comunication protocol broken");
                        return false; 
                }

                #ifdef DEBUG
                printf("{DEBUG} [Main Thread] ACK no. %d received\n", get_sequence_number(recv_header));
                #endif

                if (is_last(recv_header)) {

                        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                                perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                                return false;                                
                        }

                        set_status_safe(&args->status, QUIT, &args->mutex);

                        if (pthread_sigmask(SIG_UNBLOCK, &t_set, NULL)) {
                                perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                                return false;                                
                        }
                        
                        #ifdef DEBUG
                        printf("{DEBUG} [Main Thread] Comunication with server has expired\n");
                        #endif

                        break;
                }

                if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                        perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                        return false;                                
                }

                if (pthread_mutex_lock(&args->mutex)) {
                        perr("{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                        return false;
                }

                if (config->is_adaptive) {
                        if (adapt->seq_num <= get_sequence_number(recv_header)) {
                                gettimeofday(&tv, NULL);
                                adapt->sampleRTT = elapsed_usec(&adapt->saved_tv, &tv);
                                adapt->estimatedRTT = ((1 - ALPHA) * adapt->estimatedRTT) + (ALPHA * adapt->sampleRTT);
                                adapt->devRTT = ((1 - BETA) * adapt->devRTT) + (BETA * abs_val(adapt->sampleRTT - adapt->estimatedRTT));
                                adapt->seq_num = get_sequence_number(recv_header);
                                adapt->restart = true;

                                #ifdef DEBUG
                                printf("{DEBUG} [Main Thread] Updated value of rto: %ld usec\n", (adapt->estimatedRTT + 4 * adapt->devRTT));
                                #endif
                        }
                }

                args->base = get_sequence_number(recv_header) + 1;
                
                if (args->base == args->next_seq_num)
                        memset(&args->start_timer, 0x0, sizeof(struct timeval));
                else    
                        gettimeofday(&args->start_timer, NULL);

                if (pthread_mutex_unlock(&args->mutex)) {
                        perr("{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                        return false;
                }

                if (pthread_sigmask(SIG_UNBLOCK, &t_set, NULL)) {
                        perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                        return false;                                
                }
        }

        return true;
}

bool list(void) 
{
        enum connection_status status = REQUEST;

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perr("{ERROR} [Main Thread] Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        if (!l_request_loop(&status))
                return false;

        cls();
        printf("AVAILABLE FILE ON SERVER\n\n");

        if (!lg_connect_loop(STDIN_FILENO, LIST, &status))
                return false;

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL))
                return false;

        printf("\n\nPress any key to get back to menu ");
        fflush(stdout);
        getchar();

        close(sockfd);

        if (pthread_sigmask(SIG_UNBLOCK, &t_set, NULL))
                return false;

        return true;
}

bool get_file(void) 
{
        enum connection_status status;
        int fd;
        char filename[CHUNK_SIZE];
        char path[PATH_SIZE];
        bool delete_file = false;

        status = REQUEST;

        memset(path, 0x0, PATH_SIZE);
        memset(filename, 0x0, CHUNK_SIZE);

        cls();
        printf("Which file do you want to download? ");
        fflush(stdout);
        get_input(CHUNK_SIZE, filename, true);

        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-download/%s", getenv("USER"), filename);

        if((fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
                perr("{ERROR} [Main Thread] Unable to create a local copy of remote file");
                return false;
        } 

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perr("{ERROR} [Main Thread] Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        if (!g_request_loop(fd, filename, &status, &delete_file)) {
                if (delete_file) {
                        printf("Selected file does not exists on server\nPress enter to get back to menu ");
                        getchar();
                        remove(path);
                        return true;
                } else {
                        return false;
                }
        }

        printf("Wait for download ...\n");
        if (!lg_connect_loop(fd, GET, &status))
                return false;

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL))
                return false;

        printf("\n\nFile succesfully downloaded\nPress enter to get back to menu");
        fflush(stdout);
        getchar();

        close(fd);
        close(sockfd);

        if (pthread_sigmask(SIG_UNBLOCK, &t_set, NULL))
                return false;

        return true;
}

bool init_put_args(void)
{
        if ((args = malloc(sizeof(struct put_args))) == NULL) {
                perr("{ERROR} [Main Thread] Unable to allocate metadata for PUT operation (malloc)");
                return false;
        }

        memset(args, 0x0, sizeof(struct put_args));

        if (pthread_mutex_init(&args->mutex, NULL)) {
                perr("{ERROR} [Main Thread] Unable to allocate metadata for PUT operation (pthread_mutex_init)");
                return false;
        }
        
        args->fd = -1;
        args->base = 1;
        args->next_seq_num = 1;
        args->expected_seq_num = 1;
        args->last_acked_seq_num = 0;
        args->status = REQUEST;

        if (config->is_adaptive) {
                if ((adapt = malloc(sizeof(struct gbn_adaptive_timeout))) == NULL) {
                        perr("{ERROR} [Main Thread] Unable to allocate metadata for PUT operation (malloc)");    
                        return false;
                }

                memset(adapt, 0x0, sizeof(struct gbn_adaptive_timeout));

                adapt->seq_num = 1;
                adapt->restart = true;
                adapt->sampleRTT = config->rto_usec;
                adapt->estimatedRTT = config->rto_usec;
                adapt->devRTT = 0;
        }

        return true;
        
}

bool dispose_put_args(void)
{
        if (args != NULL) {
                if (pthread_mutex_destroy(&args->mutex)) {
                        perr("{ERROR} [Main Thread] unable to free metadata used for PUT operation");
                        return false;
                }

                close(args->fd);
                free(args);
                args = NULL;

                if (config->is_adaptive) {
                        free(adapt);
                        adapt = NULL;
                }
        }

        return true;
}

bool put_file(void) 
{
        size_t filename_size;
        char filename[CHUNK_SIZE];
        char path[PATH_SIZE];
        char choice;
        bool already_exists = false;

        if (!init_put_args())
                return false;

        while(true) {
        
                memset(path, 0x0, PATH_SIZE);
                memset(filename, 0x0, CHUNK_SIZE);

                cls();
                printf("Which file do you want to upload to server (full path)? ");
                fflush(stdout);
                get_input(PATH_SIZE, path, true);

                printf("Choose the name for the upload (default: %s)? ", basename(path));
                fflush(stdout);
                filename_size = get_input(CHUNK_SIZE, filename, false);

                if((args->fd = open(path, O_RDONLY)) == -1) {
                        if (errno == ENOENT) {
                                printf("Selected file does not exists\n");
                                choice = multi_choice("Do you want to retry?", "yn", 2);

                                if (choice == 'Y')
                                        continue;
                                else
                                        return false;

                        }
                        perr("{ERROR} [Main Thread] Unable to open chosen file");
                        return false;
                } 

                break;
        }

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perr("{ERROR [Main Thread] Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        if (!p_request_loop(args->fd, (filename_size) ? filename : basename(path), &args->status, &already_exists)) {
                if (already_exists) {
                        printf("Selected file already exists on server\nPress enter to get back to menu ");
                        getchar();
                        return true;
                } else {
                        return false;
                }                
        }

        if (pthread_create(&args->tid, NULL, put_sender_routine, args)) {
                perr("{ERROR} [Main Thread] Unable to spawn sender thread");
                return false;
        }

        if (!p_connect_loop())
                return false;

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL))
                return false;

        printf("File succesfully uploaded\nPress enter to get back to menu\n");
        getchar();

        pthread_join(args->tid, NULL);

        #ifdef DEBUG
        printf("{DEBUG} [Main Thread] Joined sender thread\n");
        #endif

        dispose_put_args();
        close(sockfd);

        if (pthread_sigmask(SIG_UNBLOCK, &t_set, NULL))
                return false;

        return true;
}

bool check_installation(void) 
{
        char path[PATH_SIZE];

        memset(path, 0x0, PATH_SIZE);
        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-download", getenv("USER"));
        
        if (access(path, F_OK) != -1)
                return true;
        else   
                return false;
}

bool load_header(void)
{
        int fd;
        char path[PATH_SIZE];
        ssize_t rsize;

        memset(path, 0x0, PATH_SIZE);

        if ((rsize = readlink("/proc/self/exe", path, PATH_SIZE)) == -1) {
                perr("{ERROR} [Main Thread] Unable to locate exec path");
                return false;
        }

        
        if ((fd = open(strncat(dirname(path), "/../header.txt", 15), O_RDONLY)) == -1) {
                perr("{ERROR} [Main Thread] Unable to load figlet header");
                return false;
        }

        if (read(fd, header, HEADER_FILE_LENGTH) == -1) {
                perr("{ERROR} [Main Thread] Unable to read from figlet file");
                return false;
        }

        return true;
}

int main(int argc, char **argv)
{
        char address_string[ADDRESS_STRING_LENGTH];
        char choice;
        bool figlet_header;
        bool is_first = true;

        #ifdef DEBUG
        printf("*** DEBUG MODE ***\n\n\n");
        #endif

        if (!check_installation()) {
                fprintf(stderr, "Client not installed yet!\n\nPlease run the following command:\n\tsh /path/to/script/install-client.sh\n");
                exit_client(EXIT_FAILURE);
        }

        if (!setup_signals(&t_set, sig_handler))
                exit_client(EXIT_FAILURE);        

        srand(time(0));

        memset(address_string, 0x0, ADDRESS_STRING_LENGTH);
        
        if((config = init_configurations()) == NULL) {
                perr("Unable to load default configurations for server");
                exit_client(EXIT_FAILURE); 
        }
        
        server_port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, address_string);

        switch (modality) {
                case STANDARD: 
                        break;
                case HELP:
                        printf("\n\tusage: gbn-ftp-client -a [--address] <address> [options]\n");
                        printf("\n\tList of available options:\n");
                        printf("\t\t-p [--port]\t<port>\t\tserver port\n");
                        printf("\t\t-v [--version]\t\t\tVersion of gbn-ftp-client\n");
                        printf("\t\t-V [--verbose]\t\t\tPrint verbose version of error\n");
                        exit_client(EXIT_SUCCESS);
                        break;
                case VERSION: 
                        printf("\n\tgbn-ftp-client version 1.0 (developed by tibwere)\n\n");    
                        exit_client(EXIT_SUCCESS);
                        break;
                case ERROR:
                        fprintf(stderr, "Wrong argument inserted.\nPlease re-run gbn-ftp-client with -h [--help] option.\n");
                        exit_client(EXIT_FAILURE);   
                        break;                   
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();        
        }

        figlet_header = load_header();

        if (!set_sockadrr_in(&request_sockaddr, address_string, server_port))
                exit_client(EXIT_FAILURE);

        cls();
        do {
                if (figlet_header)
                        printf("%s\n\n", header);

                if (is_first) {
                        printf("Welcome to GBN-FTP service\n\n");
                        is_first = false;
                }

                printf("*** What do you wanna do? ***\n\n");
                printf("[L]IST all available files\n");
                printf("[P]UT a file on the server\n");
                printf("[G]ET a file from server\n");
                printf("[Q]uit\n");

                choice = multi_choice("\nPick an option", "LPGQ", 4);

                switch (choice) {
                        case 'L': 
                                if (!list())
                                        choice = 'Q';
                                
                                break;
                        case 'P': 
                                if (!put_file())
                                        choice = 'Q';
                                     
                                break;
                        case 'G':
                                if (!get_file())
                                        choice = 'Q';
                                
                                break;                                
                        case 'Q': 
                                break;
                        default:
                                fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                                abort();
                }

                #ifndef DEBUG
                if (choice != 'Q')
                        cls();
                #endif
                
        } while (choice != 'Q');   
  
        exit_client(EXIT_SUCCESS);
}

