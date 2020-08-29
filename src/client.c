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


#include "gbnftp.h" 
#include "common.h"


#define ADDRESS_STRING_LENGTH 1024

#ifndef DEBUG
        #define cls() printf("\033[2J\033[H")
#else   
        #define cls() 
#endif


struct put_args {
        int fd;
        unsigned int base;
        unsigned int next_seq_num;
        unsigned int expected_seq_num;
        unsigned int last_acked_seq_num;
        pthread_mutex_t mutex;
        enum connection_status status;
        struct timeval start_timer; 
        pthread_t tid;
};


extern bool verbose;
extern char *optarg;
extern int opterr;


int sockfd;
struct gbn_config *config;
struct sockaddr_in request_sockaddr;
unsigned short int server_port;


void exit_client(int status);
enum app_usages parse_cmd(int argc, char **argv, char *address);
bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port, char *error_message);
ssize_t send_request(enum message_type type, const char *filename, size_t filename_length, char *error_message);
ssize_t send_ack(enum message_type type, unsigned int seq_num, bool is_last, char *error_message);
bool list(char *error_message);
bool get_file(char *error_message);
bool check_installation(void);


ssize_t send_file_chunk(struct put_args *args, char *error_message)
{
        gbn_ftp_header_t header;
        char buff[CHUNK_SIZE];
        ssize_t rsize;
        ssize_t wsize;

        memset(buff, 0x0, CHUNK_SIZE);

        if ((rsize = read(args->fd, buff, CHUNK_SIZE)) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to read from selected file");
                return -1;                
        }

        if (rsize > 0) {

                set_message_type(&header, PUT);
                set_sequence_number(&header, args->next_seq_num++);
                set_ack(&header, false);      
                set_err(&header, false);  
                
                if (rsize < CHUNK_SIZE) {
                        set_last(&header, true);
                } else {
                        set_last(&header, false);
                }   

                if ((wsize = gbn_send(sockfd, header, buff, rsize, NULL, config)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send chunk to server");
                        return -1;
                }

                #ifdef DEBUG
                printf("[DEBUG] Segment no. %d %ssent\n", get_sequence_number(header), (wsize == 0) ? "not " : "");
                #endif

                if (pthread_mutex_lock(&args->mutex)) {
                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                        return -1;
                }

                if (args->base == args->next_seq_num) 
                        gettimeofday(&args->start_timer, NULL);

                if (pthread_mutex_unlock(&args->mutex)) {
                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                        return -1;
                }

                return wsize; 
        }  

        return 0;    
}

bool handle_retransmit(struct put_args *args, char * error_message) 
{
        unsigned int base; 
        unsigned int next_seq_num;

        if (pthread_mutex_lock(&args->mutex)) {
                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                return -1;
        }

        base = args->base - 1;
        args->next_seq_num = args->base;
        args->status = CONNECTED;
        lseek(args->fd, base * CHUNK_SIZE, SEEK_SET);
        gettimeofday(&args->start_timer, NULL);

        if (pthread_mutex_unlock(&args->mutex)) {
                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                return -1;
        }

        next_seq_num = args->next_seq_num;

        for (unsigned int i = base; i < next_seq_num; ++i)
                if (send_file_chunk(args, error_message) == -1)
                        return false;

        return true;
}

void *put_sender_routine(void *args) 
{
        struct put_args *arguments = (struct put_args *)args;
        char err_mess[ERR_SIZE];
        struct timeval tv;
        bool fail = false;
        bool has_unlocked;
        unsigned short timeout_counter = 0;
        unsigned int last_base_for_timeout = arguments->base;

        do  {
                fail = false;

                switch (arguments->status) {

                        case CONNECTED:

                                has_unlocked = false;
                                if (pthread_mutex_lock(&arguments->mutex)) {
                                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        fail = true;
                                }

                                if (arguments->next_seq_num < arguments->base + config->N) {
                                        
                                        if (pthread_mutex_unlock(&arguments->mutex)) {
                                                snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                                fail = true;
                                        }
                                        has_unlocked = true;

                                        if (send_file_chunk(arguments, err_mess) == -1)
                                                fail = true;
                                }

                                if (!has_unlocked) {
                                        if (pthread_mutex_unlock(&arguments->mutex)) {
                                                snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                                fail = true;
                                        } 
                                        has_unlocked = true;                                      
                                }

                                has_unlocked = false;
                                if (pthread_mutex_lock(&arguments->mutex)) {
                                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        fail = true;
                                } 

                                if (arguments->status != QUIT && !fail) {
                                        gettimeofday(&tv, NULL);

                                        if (elapsed_usec(&arguments->start_timer, &tv) >= config->rto_usec)
                                                arguments->status = TIMEOUT;

                                }

                                if (pthread_mutex_unlock(&arguments->mutex)) {
                                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        fail = true;
                                }
                                has_unlocked = true;

                                break;

                        case TIMEOUT:

                                #ifdef DEBUG
                                printf("[DEBUG] Timeout event (%d)\n", arguments->base);
                                #endif       

                                has_unlocked = false;
                                if (pthread_mutex_lock(&arguments->mutex)) {
                                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        fail = true;
                                }

                                if (arguments->base == last_base_for_timeout) {
                                        
                                        if (pthread_mutex_unlock(&arguments->mutex)) {
                                                snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                                fail = true;
                                        }
                                        has_unlocked = true;

                                        if (timeout_counter < MAX_TO) {
                                                if (!handle_retransmit(arguments, err_mess))
                                                        fail = true;
                                        
                                        } else  {

                                                has_unlocked = false;
                                                if (pthread_mutex_lock(&arguments->mutex)) {
                                                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                                        fail = true;
                                                }

                                                arguments->status = QUIT;

                                                if (pthread_mutex_unlock(&arguments->mutex)) {
                                                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                                        fail = true;
                                                }
                                                has_unlocked = true;        
                                        }
                                                

                                        ++timeout_counter;
                                } else {

                                        if (pthread_mutex_unlock(&arguments->mutex)) {
                                                snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                                fail = true;
                                        }
                                        has_unlocked = true;

                                        timeout_counter = 1;
                                        
                                        if (!handle_retransmit(arguments, err_mess)) {
                                                fail = true;
                                        }
                                }

                                break;
                        
                        default: 
                                break;
                }

        } while (arguments->status != QUIT || fail);

        if (!has_unlocked) {
                if (pthread_mutex_unlock(&arguments->mutex)) {
                        snprintf(err_mess, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                        fail = true;
                }                
        }

        if (fail)
                perr(err_mess);

        #ifdef DEBUG
        printf("[DEBUG] Sender worker is quitting right now\n");
        #endif

        pthread_exit(NULL);
}

void exit_client(int status) 
{
        /* TODO: implementare chiusura pulita */
        close(sockfd);
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
                {"wndsize",     required_argument,      0, 'N'},
                {"rtosec",      required_argument,      0, 's'},
                {"rtousec",     required_argument,      0, 'u'},
                {"adaptive",    no_argument,            0, 'A'},
                {"prob",        required_argument,      0, 'P'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {"verbose",     no_argument,            0, 'V'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "a:p:N:t:A:P:hvV", long_options, NULL)) != -1) {
                switch (opt) {
                        case 'a':
                                strncpy(address, optarg, ADDRESS_STRING_LENGTH);
                                valid_cmd = true;
                                break;
                        case 'p':
                                server_port = strtol(optarg, NULL, 10);
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
                                config->probability = strtol(optarg, NULL, 10) / 100;
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

bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port, char *error_message)
{
        memset(server_sockaddr, 0x0, sizeof(struct sockaddr_in));
	server_sockaddr->sin_family = AF_INET;
	server_sockaddr->sin_port = htons(port);
        
        if (inet_pton(AF_INET, address_string, &server_sockaddr->sin_addr) <= 0) {
                snprintf(error_message, ERR_SIZE, "Unable to convert address from string to internal logical representation"); 
                return false;
        } 

        return true;
}

ssize_t send_request(enum message_type type, const char *filename, size_t filename_length, char *error_message)
{
        gbn_ftp_header_t header;
        ssize_t wsize;

        set_sequence_number(&header, 0);
        set_last(&header, true);
        set_err(&header, false);
        set_ack(&header, false);
        set_message_type(&header, type);

        if ((wsize = gbn_send(sockfd, header, filename, filename_length, &request_sockaddr, config)) == -1)
                snprintf(error_message, ERR_SIZE, "Unable to send request command to server (OP %d)", type);

        #ifdef DEBUG
        printf("[DEBUG] Request segment %ssent\n", (wsize == 0) ? "not " : "");
        #endif
        
        return wsize;
}

ssize_t send_ack(enum message_type type, unsigned int seq_num, bool is_last, char *error_message)
{
        gbn_ftp_header_t header;
        ssize_t wsize;

        set_sequence_number(&header, seq_num);
        set_last(&header, is_last);
        set_ack(&header, true);
        set_message_type(&header, type);

        if ((wsize = gbn_send(sockfd, header, NULL, 0, NULL, config)) == -1)
                snprintf(error_message, ERR_SIZE, "Unable to send ack to server (OP %d)", type);

        #ifdef DEBUG
        printf("[DEBUG] ACK no. %d %ssent\n", seq_num, (wsize == 0) ? "not " : "");
        #endif
        
        return wsize;
}

bool list(char *error_message) 
{
        unsigned int expected_seq_num, last_acked_seq_num;
        enum connection_status status;
        fd_set read_fds, std_fds;
        int retval;
        struct timeval tv;
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        ssize_t recv_size;
        size_t header_size = sizeof(gbn_ftp_header_t);
        struct sockaddr_in addr;

        expected_seq_num = 1;
        last_acked_seq_num = 0;
        status = REQUEST;

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        FD_ZERO(&std_fds);
        FD_SET(sockfd, &std_fds);

        while (status == REQUEST) {
                if (send_request(LIST, NULL, 0, error_message) == -1) 
                        return false; 

                read_fds = std_fds;
                tv.tv_sec = floor((double) config->rto_usec / (double) 1000000);
                tv.tv_usec = config->rto_usec % 1000000;

                if ((retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to get response from server (NEW_PORT)");
                        return false;
                }

                if (retval) {
                        status = CONNECTED;

                        if((recv_size = gbn_receive(sockfd, &recv_header, payload, &addr)) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                                return false;
                        }

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == 0)) {
                                
                                #ifdef DEBUG
                                printf("[DEBUG] Received NEW PORT message\n");
                                #endif

                                if (connect(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
                                        snprintf(error_message, ERR_SIZE, "Connection to server failed");
                                        return false;
                                }
                                if (send_ack(LIST, 0, false, error_message) == -1)
                                        return false;
                        }
                }  
        }

        cls();
        printf("AVAILABLE FILE ON SERVER\n\n");

        while (status == CONNECTED) {
                memset(payload, 0x0, CHUNK_SIZE);

                if((recv_size = gbn_receive(sockfd, &recv_header, payload, &addr)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                        return false;
                }

                #ifdef DEBUG
                printf("[DEBUG] Received chunk no. %d (DIM. %ld)\n", get_sequence_number(recv_header), recv_size - header_size);
                #endif

                if(get_sequence_number(recv_header) == expected_seq_num) {
                        last_acked_seq_num = expected_seq_num;

                        if (write(STDIN_FILENO, payload, recv_size - header_size) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to print out info received from server");
                                return false;  
                        }

                        if (send_ack(LIST, expected_seq_num++, is_last(recv_header), error_message) == -1)
                                return false;

                        if (is_last(recv_header)) {
                                status = QUIT;
                        
                                // Al termine invio 4 ACK per aumentare la probabilità di ricezione da parte del server
                                for (int i = 0; i < LAST_MESSAGE_LOOP - 1; ++i) {
                                        if (send_ack(LIST, last_acked_seq_num, true, error_message) == -1)
                                                return false;
                                }
                        }

                } else {
                        if (send_ack(LIST, last_acked_seq_num, false, error_message) == -1)
                                return false;
                }                        
        }

        printf("Press any key to get back to menu\n");
        getchar();

        close(sockfd);
        return true;
}

bool get_file(char *error_message) 
{
        unsigned int expected_seq_num, last_acked_seq_num;
        enum connection_status status;
        fd_set read_fds, std_fds;
        int retval;
        struct timeval tv;
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        ssize_t recv_size;
        size_t header_size = sizeof(gbn_ftp_header_t);
        struct sockaddr_in addr;
        int fd;
        char filename[CHUNK_SIZE];
        char path[PATH_SIZE];

        expected_seq_num = 1;
        last_acked_seq_num = 0;
        status = REQUEST;

        memset(path, 0x0, PATH_SIZE);
        memset(filename, 0x0, CHUNK_SIZE);

        cls();
        printf("Which file do you want to download? ");
        fflush(stdout);
        get_input(CHUNK_SIZE, filename, true);
        printf("Wait for download ...\n");

        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-download/%s", getenv("USER"), filename);

        if((fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to create file");
                return false;
        } 

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        FD_ZERO(&std_fds);
        FD_SET(sockfd, &std_fds);

        while (status == REQUEST) {
                if (send_request(GET, filename, strlen(filename), error_message) == -1) 
                        return false; 

                read_fds = std_fds;
                tv.tv_sec = floor((double) config->rto_usec / (double) 1000000);
                tv.tv_usec = config->rto_usec % 1000000;

                if ((retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to get response from server (NEW_PORT)");
                        return false;
                }

                if (retval) {
                        status = CONNECTED;

                        if((recv_size = gbn_receive(sockfd, &recv_header, payload, &addr)) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                                return false;
                        }

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == 0) && !is_err(recv_header)) {
                                
                                #ifdef DEBUG
                                printf("[DEBUG] Received NEW PORT message\n");
                                #endif

                                if (connect(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
                                        snprintf(error_message, ERR_SIZE, "Connection to server failed");
                                        return false;
                                }
                                if (send_ack(GET, 0, false, error_message) == -1)
                                        return false;
                        }

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == 0) && is_err(recv_header)) {
                                
                                #ifdef DEBUG
                                printf("[DEBUG] Received error message\n");
                                #endif

                                close(fd);
                                remove(path);
                                snprintf(error_message, ERR_SIZE, "Selected file does not exists");
                                return false;
                        }
                }  
        }

        while (status == CONNECTED) {
                memset(payload, 0x0, CHUNK_SIZE);

                if((recv_size = gbn_receive(sockfd, &recv_header, payload, &addr)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                        return false;
                }

                #ifdef DEBUG
                printf("[DEBUG] Received chunk no. %d (DIM. %ld)\n", get_sequence_number(recv_header), recv_size - header_size);
                #endif

                if(get_sequence_number(recv_header) == expected_seq_num) {
                        last_acked_seq_num = expected_seq_num;

                        if (write(fd, payload, recv_size - header_size) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to print out info received from server");
                                return false;  
                        }

                        if (send_ack(GET, expected_seq_num++, is_last(recv_header), error_message) == -1)
                                return false;

                        if (is_last(recv_header)) {
                                status = QUIT;

                                for (int i = 0; i < LAST_MESSAGE_LOOP - 1; ++i)                                
                                        if (send_ack(GET, last_acked_seq_num, true, error_message) == -1)
                                                return false;
                        }

                } else {
                        if (send_ack(GET, last_acked_seq_num, false, error_message) == -1)
                                return false;
                }
        }

        printf("File succesfully downloaded\nPress return to get back to menu\n");
        getchar();

        close(fd);
        close(sockfd);
        return true;
}

struct put_args *init_put_args(void)
{
        struct put_args *pa;

        if ((pa = malloc(sizeof(struct put_args))) == NULL)
                return NULL;

        memset(pa, 0x0, sizeof(struct put_args));

        if (pthread_mutex_init(&pa->mutex, NULL)) 
                return NULL;
        
        pa->fd = -1;
        pa->base = 1;
        pa->next_seq_num = 1;
        pa->expected_seq_num = 1;
        pa->last_acked_seq_num = 0;
        pa->status = REQUEST;

        return pa;
}

bool dispose_put_args(struct put_args *pa)
{
        if (pthread_mutex_destroy(&pa->mutex)) 
                return false;

        close(pa->fd);
        free(pa);

        return true;
}

bool put_file(char *error_message) 
{
        fd_set read_fds, std_fds;
        int retval;
        struct timeval tv;
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        ssize_t recv_size;
        size_t header_size = sizeof(gbn_ftp_header_t);
        size_t filename_size;
        struct sockaddr_in addr;
        char filename[CHUNK_SIZE];
        char path[PATH_SIZE];
        struct put_args *args;

        memset(path, 0x0, PATH_SIZE);
        memset(filename, 0x0, CHUNK_SIZE);

        args = init_put_args();

        cls();
        printf("Which file do you want to upload to server (full path)? ");
        fflush(stdout);
        retval = get_input(PATH_SIZE, path, true);

        printf("Choose the name for the upload (default: %s)? ", basename(path));
        fflush(stdout);
        filename_size = get_input(CHUNK_SIZE, filename, false);

        if((args->fd = open(path, O_RDONLY)) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to open chosen file");
                return false;
        } 

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        FD_ZERO(&std_fds);
        FD_SET(sockfd, &std_fds);

        while (args->status == REQUEST) {

                if (filename_size) {
                        if (send_request(PUT, filename, strlen(filename), error_message) == -1) 
                                return false; 

                } else {
                        if (send_request(PUT, basename(path), strlen(basename(path)), error_message) == -1) 
                                return false; 
                }

                read_fds = std_fds;
                tv.tv_sec = floor((double) config->rto_usec / (double) 1000000);
                tv.tv_usec = config->rto_usec % 1000000;

                if ((retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to get response from server (NEW_PORT)");
                        return false;
                }

                if (retval) {
                        args->status = CONNECTED;

                        if((recv_size = gbn_receive(sockfd, &recv_header, payload, &addr)) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                                return false;
                        }

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == 0) && !is_err(recv_header)) {
                                
                                #ifdef DEBUG
                                printf("[DEBUG] Received NEW PORT message\n");
                                #endif

                                if (connect(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
                                        snprintf(error_message, ERR_SIZE, "Connection to server failed");
                                        return false;
                                }

                                if (send_ack(PUT, 0, false, error_message) == -1)
                                        return false;

                                if (pthread_create(&args->tid, NULL, put_sender_routine, args)) {
                                        snprintf(error_message, ERR_SIZE, "Unable to spawn sender thread");
                                        return false;
                                }
                        }

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == 0) && is_err(recv_header)) {
                                
                                #ifdef DEBUG
                                printf("[DEBUG] Received error message\n");
                                #endif

                                close(args->fd);
                                snprintf(error_message, ERR_SIZE, "Selected file already exists on server");
                                return false;
                        }
                }  
        }

        while (args->status == CONNECTED) {

                if (gbn_receive(sockfd, &recv_header, NULL, NULL) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to get message from server");
                        return false;
                }

                if(is_ack(recv_header)) {

                        #ifdef DEBUG
                        printf("[DEBUG] ACK no. %d received\n", get_sequence_number(recv_header));
                        #endif

                        if (get_sequence_number(recv_header) >= args->base) {

                                if (pthread_mutex_lock(&args->mutex)) {
                                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        return false;
                                }

                                args->base = get_sequence_number(recv_header) + 1;

                                if (pthread_mutex_unlock(&args->mutex)) {
                                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        return false;
                                }
                        }

                        if (pthread_mutex_lock(&args->mutex)) {
                                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                return false;
                        }

                        if (args->base == args->next_seq_num)
                                memset(&args->start_timer, 0x0, sizeof(struct timeval));
                        else    
                                gettimeofday(&args->start_timer, NULL);


                        if (pthread_mutex_unlock(&args->mutex)) {
                                snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                return false;
                        }

                        if (is_last(recv_header)) {

                                if (pthread_mutex_lock(&args->mutex)) {
                                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        return false;
                                }

                                args->status = QUIT;

                                if (pthread_mutex_unlock(&args->mutex)) {
                                        snprintf(error_message, ERR_SIZE, "Syncronization protocol for worker threads broken (worker_mutex)");
                                        return false;
                                }
                                
                                #ifdef DEBUG
                                printf("[DEBUG] Comunication with server has expired\n");
                                #endif
                        } 
                }
        }

        printf("File succesfully uploaded\nPress return to get back to menu\n");
        getchar();

        pthread_join(args->tid, NULL);
        dispose_put_args(args);
        close(sockfd);
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

int main(int argc, char **argv)
{
        char address_string[ADDRESS_STRING_LENGTH];
        enum app_usages modality;
        char choice;
        char err_mess[ERR_SIZE];

        if (!check_installation()) {
                fprintf(stderr, "Client not installed yet!\n\nPlease run the following command:\n\tsh /path/to/script/install-client.sh\n");
                exit_client(EXIT_FAILURE);
        }

        srand(time(0));

        memset(address_string, 0x0, ADDRESS_STRING_LENGTH);
        memset(err_mess, 0x0, ERR_SIZE);
        
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
                        printf("\n\tusage: gbn-ftp-client [options]\n");
                        printf("\n\tList of available options:\n");
                        printf("\t\t-a [--address]\t<address>\tserver address (IPv4) {REQUIRED}\n");
                        printf("\t\t-p [--port]\t<port>\t\tserver port\n");
                        printf("\t\t-N [--wndsize]\t<size>\t\tWindow size (for GBN)\n");
                        printf("\t\t-t [--rtousec]\t<timeout>\tRetransmition timeout [usec] (for GBN)\n");
                        printf("\t\t-A [--adaptive]\t\t\tTimer adaptative\n");
                        printf("\t\t-P [--prob]\t<percentage>\tLoss probability (from 0 to 1)\n");
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

        if (!set_sockadrr_in(&request_sockaddr, address_string, server_port, err_mess)) {
                perr(err_mess);
                exit_client(EXIT_FAILURE);
        }

        do {
                cls();
                printf("Welcome to GBN-FTP service\n\n");
                printf("*** What do you wanna do? ***\n\n");
                printf("[L]IST all available files\n");
                printf("[P]UT a file on the server\n");
                printf("[G]ET a file from server\n");
                printf("[Q]uit\n");

                choice = multi_choice("\nPick an option", "LPGQ", 4);

                switch (choice) {
                        case 'L': 
                                if (!list(err_mess)) {
                                        perr(err_mess);
                                        choice = 'Q';
                                }       
                                break;
                        case 'P': 
                                if (!put_file(err_mess)) {
                                        perr(err_mess);
                                        choice = 'Q';
                                }       
                                break;
                        case 'G':
                                if (!get_file(err_mess)) {
                                        perr(err_mess);
                                        choice = 'Q';
                                }
                                break;                                
                        case 'Q': 
                                printf("Bye bye\n\n");
                                break;
                        default:
                                fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                                abort();
                }
        } while (choice != 'Q');   
  
        return 0;
}

