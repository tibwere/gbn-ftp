#include <stdio.h>
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

#include "gbnftp.h" 
#include "common.h"


#define ADDRESS_STRING_LENGTH 1024
#define CLS system("clear")

extern bool verbose;
extern char *optarg;
extern int opterr;


int sockfd;
unsigned short int server_port;
struct gbn_config *config;
unsigned int base;
unsigned int next_seq_num;
unsigned int expected_seq_num;

void exit_client(int status);
enum app_usages parse_cmd(int argc, char **argv, char *address);
bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port, char *error_message);
int connect_to_server(const char *address_string, enum message_type type, char *error_message);
void print_info_about_conn(const char* address_string);
void list(const char *address_string);

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
                {"rto",         required_argument,      0, 't'},
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

int connect_to_server(const char *address_string, enum message_type type, char *error_message)
{
        int sockfd;
        gbn_ftp_header_t header;
        struct sockaddr_in server_sockaddr;
        fd_set read_fds;
        struct timeval tv;
        int retval;
        char ack_no[CHUNK_SIZE];

        set_ack(&header, false);
        set_sequence_number(&header, next_seq_num++);
        set_message_type(&header, type);
        set_last(&header, false);

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor (1st step)");
                return -1;
        }

        if (!set_sockadrr_in(&server_sockaddr, address_string, server_port, error_message)) 
                return -1;

        memset(ack_no, 0x0, CHUNK_SIZE);

        while (true) {
                
                FD_ZERO(&read_fds);
                FD_SET(sockfd, &read_fds);
                tv.tv_sec = 0;
                tv.tv_usec = config->rto_usec;

                /* SEND CMD */
                if (gbn_send(sockfd, header, NULL, 0, &server_sockaddr, config) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send LIST command to server");
                        return -1;
                }

                retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv);
                if (retval == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive ACK from server (select)");
                        return -1;
                } 
                if (retval) {
                        memset(&server_sockaddr, 0x0, sizeof(struct sockaddr_in));
                        
                        if(gbn_receive(sockfd, &header, ack_no, &server_sockaddr) == -1) {
                                snprintf(error_message, ERR_SIZE, "Unable to receive ACK from server (gbn_select)");
                                return -1;
                        }

                        if (!is_ack(header)) {
                                snprintf(error_message, ERR_SIZE, "Comunication protocol broken");
                                return -1;
                        }

                        ++base;

                        break;
                }  
        }

        close(sockfd);

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor (2nd step)");
                return -1;
        }

	if (connect(sockfd, (struct sockaddr *) &server_sockaddr, sizeof(struct sockaddr_in)) == -1) {
                snprintf(error_message, ERR_SIZE, "Connection to server failed");
                return -1;
        }

	return sockfd;
}

void list(const char *address_string) 
{
        char err_mess[ERR_SIZE];

        if ((sockfd = connect_to_server(address_string, LIST, err_mess)) == -1)
                exit_client(EXIT_FAILURE);

        close(sockfd);
}

int main(int argc, char **argv)
{
        char address_string[ADDRESS_STRING_LENGTH];
        enum app_usages modality;
        char choice;

        srand(time(0));

        memset(address_string, 0x0, ADDRESS_STRING_LENGTH);

        base = 0;
        next_seq_num = 0;
        expected_seq_num = 0;
        
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
                        printf("\t\t-t [--rto]\t<timeout>\tRetransmition timeout (for GBN)\n");
                        printf("\t\t-A [--adaptive]\t\t\tTimer adaptative\n");
                        printf("\t\t-P [--prob]\t<percentage>\tLoss probability (from 0 to 1)\n");
                        printf("\t\t-h [--version]\t\t\tVersion of gbn-ftp-client\n");
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

        while (true) {
                CLS;
                printf("Welcome to GBN-FTP service\n\n");
                printf("*** What do you wanna do? ***\n\n");
                printf("[L]IST all available files\n");
                printf("[P]UT a file on the server\n");
                printf("[G]ET a file from server\n");
                printf("[Q]uit\n");

                choice = multi_choice("\nPick an option", "LPGQ", 4);

                switch (choice) {
                        case 'L': list(address_string); break;
                        case 'P': 
                        case 'G':
                                printf("Not implemented yet :c\n");
                                break;
                        case 'Q': 
                                printf("Bye bye\n");
                                break;
                        default:
                                fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                                abort();
                }
        }    
  
        return 0;
}

