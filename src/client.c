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

#include "gbnftp.h" 
#include "common.h"


#define ADDRESS_STRING_LENGTH 1024

#ifndef DEBUG
        #define cls() printf("\033[2J\033[H")
#else   
        #define cls() 
#endif

extern bool verbose;
extern char *optarg;
extern int opterr;


int sockfd;
unsigned short int server_port;
struct gbn_config *config;
struct sockaddr_in request_sockaddr;


void exit_client(int status);
enum app_usages parse_cmd(int argc, char **argv, char *address);
bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port, char *error_message);
ssize_t send_request(enum message_type type, const char *filename, size_t filename_length, char *error_message);
ssize_t send_ack(enum message_type type, unsigned int seq_num, bool is_last, char *error_message);
bool list(char *error_message);
bool get_file(char *error_message);
bool check_installation(void);


void exit_client(int status) 
{
        /* TODO: implementare chiusura pulita */
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
        set_ack(&header, false);
        set_message_type(&header, type);

        if ((wsize = gbn_send(sockfd, header, filename, filename_length, &request_sockaddr, config)) == -1)
                snprintf(error_message, ERR_SIZE, "Unable to send request command to server (OP %d)", type);

        #ifdef DEBUG
        printf("Request segment %ssent\n", (wsize == 0) ? "not " : "");
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
                snprintf(error_message, ERR_SIZE, "Unable to send request command to server (OP %d)", type);

        #ifdef DEBUG
        printf("ACK no. %d %ssent\n", seq_num, (wsize == 0) ? "not " : "");
        #endif
        
        return wsize;
}

bool list(char *error_message) 
{
        enum connection_status status = REQUEST;
        fd_set read_fds, std_fds;
        int retval;
        struct timeval tv;
        unsigned int expected_seq_num = 1;
        unsigned int last_acked_seq_num = 0;
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        ssize_t recv_size;
        size_t header_size = sizeof(gbn_ftp_header_t);
        struct sockaddr_in addr;


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
                                printf("Received NEW PORT message\n");
                                #endif

                                if (connect(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
                                        snprintf(error_message, ERR_SIZE, "Connection to server failed");
                                        return false;
                                }
                                if (send_ack(LIST, 0, false, error_message) == -1)
                                        return false;
                        }
                        #ifdef DEBUG
                        else
                                printf("Received unexpected message\n");
                        #endif
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
                printf("Received chunk no. %d (DIM. %ld)\n", get_sequence_number(recv_header), recv_size - header_size);
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
                                if (send_ack(LIST, last_acked_seq_num, true, error_message) == -1)
                                        return false;

                                if (send_ack(LIST, last_acked_seq_num, true, error_message) == -1)
                                        return false;

                                if (send_ack(LIST, last_acked_seq_num, true, error_message) == -1)
                                        return false;
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
        enum connection_status status = REQUEST;
        fd_set read_fds, std_fds;
        int retval;
        struct timeval tv;
        unsigned int expected_seq_num = 1;
        unsigned int last_acked_seq_num = 0;
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        ssize_t recv_size;
        size_t header_size = sizeof(gbn_ftp_header_t);
        struct sockaddr_in addr;
        int fd;
        char filename[CHUNK_SIZE];
        char path[PATH_SIZE];

        memset(path, 0x0, PATH_SIZE);
        memset(filename, 0x0, CHUNK_SIZE);

        cls();
        printf("Which file do you want to download? ");
        fflush(stdout);
        get_input(CHUNK_SIZE, filename, false);

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

                        if ((get_sequence_number(recv_header) == 0) && !is_ack(recv_header) && (recv_size - header_size == 0)) {
                                
                                #ifdef DEBUG
                                printf("Received NEW PORT message\n");
                                #endif

                                if (connect(sockfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)) == -1) {
                                        snprintf(error_message, ERR_SIZE, "Connection to server failed");
                                        return false;
                                }
                                if (send_ack(GET, 0, false, error_message) == -1)
                                        return false;
                        }
                        #ifdef DEBUG
                        else
                                printf("Received unexpected message\n");
                        #endif
                }  
        }

        while (status == CONNECTED) {
                memset(payload, 0x0, CHUNK_SIZE);

                if((recv_size = gbn_receive(sockfd, &recv_header, payload, &addr)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                        return false;
                }

                #ifdef DEBUG
                printf("Received chunk no. %d (DIM. %ld)\n", get_sequence_number(recv_header), recv_size - header_size);
                #endif

                if(get_sequence_number(recv_header) == expected_seq_num) {
                        last_acked_seq_num = expected_seq_num;

                        if (write(fd, payload, recv_size - header_size) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to print out info received from server");
                                return false;  
                        }

                        if (send_ack(LIST, expected_seq_num++, is_last(recv_header), error_message) == -1)
                                return false;

                        if (is_last(recv_header)) {
                                status = QUIT;
                                
                                if (send_ack(LIST, last_acked_seq_num, true, error_message) == -1)
                                        return false;

                                if (send_ack(LIST, last_acked_seq_num, true, error_message) == -1)
                                        return false;
                        }

                } else {
                        if (send_ack(LIST, last_acked_seq_num, false, error_message) == -1)
                                return false;
                }
        }

        printf("File succesfully downloaded\nPress return to get back to menu\n");
        getchar();

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
                                printf("Not implemented yet :c\n");
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

