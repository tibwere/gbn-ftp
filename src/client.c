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

#include "gbnftp.h" 
#include "common.h"


#define ADDRESS_STRING_LENGTH 1024
#define cls() system("clear")

extern bool verbose;
extern char *optarg;
extern int opterr;


int sockfd;
unsigned short int server_port;
struct gbn_config *config;


void exit_client(int status);
enum app_usages parse_cmd(int argc, char **argv, char *address);
bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port, char *error_message);
int connect_to_server(const char *address_string, enum message_type type, const char *filename, size_t filename_length, char *error_message);
void print_info_about_conn(const char* address_string);
bool list(const char *address_string, char *error_message);
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

int connect_to_server(const char *address_string, enum message_type type, const char *filename, size_t filename_length, char *error_message)
{
        int sockfd;
        gbn_ftp_header_t header;
        struct sockaddr_in server_sockaddr;

        set_ack(&header, false);
        set_sequence_number(&header, 1);
        set_message_type(&header, type);
        set_last(&header, false);

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                snprintf(error_message, ERR_SIZE, "Unable to get socket file descriptor (1st step)");
                return -1;
        }

        if (!set_sockadrr_in(&server_sockaddr, address_string, server_port, error_message)) 
                return -1;


        while (true) {

                /* SEND CMD */
                if (gbn_send(sockfd, header, filename, filename_length, &server_sockaddr, config) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send LIST command to server");
                        return -1;
                }

                memset(&server_sockaddr, 0x0, sizeof(struct sockaddr_in));
                        
                if(gbn_receive(sockfd, &header, NULL, &server_sockaddr) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive ACK from server (gbn_receive)");
                        return -1;
                }

                if (!(is_ack(header) && (get_sequence_number(header) == 1))) {
                        snprintf(error_message, ERR_SIZE, "Comunication protocol broken");
                        return -1;
                }
                        
                break; 
        }

	if (connect(sockfd, (struct sockaddr *) &server_sockaddr, sizeof(struct sockaddr_in)) == -1) {
                snprintf(error_message, ERR_SIZE, "Connection to server failed");
                return -1;
        }        

	return sockfd;
}

bool get_file(const char *address_string, char *error_message) 
{
        unsigned int expected_seq_num = 1;
        unsigned int last_acked_seq_num = 0;
        char payload[CHUNK_SIZE];
        gbn_ftp_header_t recv_header;
        gbn_ftp_header_t send_header;
        ssize_t recv_size;
        char filename[CHUNK_SIZE];
        char full_path[2 * CHUNK_SIZE];
        int fd;

        size_t header_size = sizeof(gbn_ftp_header_t);

        memset(full_path, 0x0, 2 * CHUNK_SIZE);
        memset(filename, 0x0, CHUNK_SIZE);

        printf("Which file do you want to download? ");
        fflush(stdout);
        get_input(CHUNK_SIZE, filename, false);

        snprintf(full_path, 2 * CHUNK_SIZE, "/home/%s/.gbn-ftp-download/%s", getenv("USER"), filename);

        if((fd = open(full_path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
                snprintf(error_message, ERR_SIZE, "Unable to create file");
                return false;
        }        
        
        set_ack(&send_header, true);
        set_message_type(&send_header, GET);
        set_last(&send_header, false);

        printf("\nWait for downlaod ...\n\n");

        if ((sockfd = connect_to_server(address_string, GET, filename, strlen(filename), error_message)) == -1)
                return false;

        do {
                memset(payload, 0x0, CHUNK_SIZE);

                if((recv_size = gbn_receive(sockfd, &recv_header, payload, NULL)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                        return false;
                }

                if (write(fd, payload, recv_size - header_size) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to print out info received from server");
                        return false;  
                }

                if(get_sequence_number(recv_header) == expected_seq_num) 
                        set_sequence_number(&send_header, expected_seq_num++);
                else
                        set_sequence_number(&send_header, last_acked_seq_num);
                
                if (is_last(recv_header))  {
                        set_last(&send_header, true);
                } 

                if (gbn_send(sockfd, send_header, NULL, 0, NULL, config) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send ACK to server");
                        return false;
                }

        } while(!is_last(recv_header));

        printf("\n\nFile succesfully downloaded!\nPress enter key to get back to menu\n");
        getchar();

        close(sockfd);
        return true;
}

bool list(const char *address_string, char *error_message) 
{
        unsigned int expected_seq_num = 1;
        unsigned int last_acked_seq_num = 0;
        char payload[CHUNK_SIZE];
        gbn_ftp_header_t recv_header;
        gbn_ftp_header_t send_header;
        ssize_t recv_size;

        set_ack(&send_header, true);
        set_message_type(&send_header, LIST);
        set_last(&send_header, false);

        cls();
        printf("AVAILABLE FILE ON SERVER\n\n");

        if ((sockfd = connect_to_server(address_string, LIST, NULL, 0, error_message)) == -1)
                return false;

        do {
                memset(payload, 0x0, CHUNK_SIZE);

                if((recv_size = gbn_receive(sockfd, &recv_header, payload, NULL)) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to receive pkt from server (gbn_receive)");
                        return false;
                }

                if (write(STDIN_FILENO, payload, recv_size) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to print out info received from server");
                        return false;  
                }

                printf("Ricevuto SEQNO: %d (LAST = %d)\n", get_sequence_number(recv_header), is_last(recv_header));

                if(get_sequence_number(recv_header) == expected_seq_num) 
                        set_sequence_number(&send_header, expected_seq_num++);
                else
                        set_sequence_number(&send_header, last_acked_seq_num);

                if (is_last(recv_header))  {
                        set_last(&send_header, true);
                }

                if (gbn_send(sockfd, send_header, NULL, 0, NULL, config) == -1) {
                        snprintf(error_message, ERR_SIZE, "Unable to send ACK to server");
                        return false;
                }

                printf("Inviato ACK %d (LAST %d)\n", get_sequence_number(send_header), is_last(send_header));

        } while(!is_last(recv_header));

        printf("\n\nPress enter key to get back to menu\n");
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
                                if (!list(address_string, err_mess)) {
                                        perr(err_mess);
                                        choice = 'Q';
                                }       
                                break;
                        case 'P': 
                                printf("Not implemented yet :c\n");
                                break;
                        case 'G':
                                if (!get_file(address_string, err_mess)) {
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

