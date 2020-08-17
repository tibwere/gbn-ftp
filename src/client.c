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


extern const struct gbn_config DEFAULT_GBN_CONFIG;
extern char *optarg;
extern int opterr;


void exit_client(int status);
enum app_usages parse_cmd(int argc, char **argv, char *address, struct gbn_config *config, unsigned short int *port);
bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port);
int connect_to_server(const char *address_string, unsigned short int port, const struct gbn_config *config);
void print_info_about_conn(const char* address_string, unsigned short int port, struct gbn_config *configs);
void main_menu(int socket, struct gbn_send_params *params, struct gbn_config *config);

void exit_client(int status) 
{
        /* TODO: implementare chiusura pulita */
        exit(status);
}

enum app_usages parse_cmd(int argc, char **argv, char *address, struct gbn_config *config, unsigned short int *port)
{
        int opt;
        bool valid_cmd = false;

        struct option long_options[] = {
                {"address",     required_argument,      0, 'a'},
                {"port",        required_argument,      0, 'p'},
                {"wndsize",     required_argument,      0, 'N'},
                {"rto",         required_argument,      0, 't'},
                {"adaptive",    no_argument,            0, 'A'},
                {"prob",        required_argument,      0, 'P'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "a:p:N:t:A:P:hv", long_options, NULL)) != -1) {
                switch (opt) {
                        case 'a':
                                strncpy(address, optarg, ADDRESS_STRING_LENGTH);
                                valid_cmd = true;
                                break;
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
                                config->probability = strtol(optarg, NULL, 10) / 100;
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
                error_handler("\"inet_pton()\" failed."); 
                return false;
        } 

        return true;
}

int connect_to_server(const char *address_string, unsigned short int port, const struct gbn_config *config)
{
        int sockfd;
        gbn_ftp_header_t header;
        char *conn_message;
        struct sockaddr_in server_sockaddr;
        fd_set read_fds;
        struct timeval tv;
        int retval;

        set_conn(&header, true);
        conn_message = make_segment(header, NULL, 0);

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                error_handler("\"socket()\" failed."); 
                return -1;
        }

        set_sockadrr_in(&server_sockaddr, address_string, port);
        
        while (true) {
                
                FD_ZERO(&read_fds);
                FD_SET(sockfd, &read_fds);
                tv.tv_sec = 0;
                tv.tv_usec = config->rto_usec;

                gbn_send(sockfd, conn_message, sizeof(gbn_ftp_header_t), &server_sockaddr, config);

                retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv);

                if (retval == -1) {
                        error_handler("\"select()\" failed.");
                        return -1;
                } 
                
                if (retval) {
                        recvfrom(sockfd, conn_message, sizeof(gbn_ftp_header_t), 0, (struct sockaddr *) &server_sockaddr, NULL);
                        break;
                }  
        }

        close(sockfd);

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                error_handler("\"socket()\" failed."); 
                return -1;
        }

	if (connect(sockfd, (struct sockaddr *) &server_sockaddr, sizeof(struct sockaddr_in)) == -1) {
                error_handler("\"connect()\" failed.");
                return -1;
        }
	
	return sockfd;
}

void print_info_about_conn(const char* address_string, unsigned short int port, struct gbn_config *configs)
{
        char choice;

        printf("Connecting to %s:%u ...\n", address_string, port);
        choice = multi_choice("Do you want to see more details?", "yn", 2);

        if (choice == 'Y')
        {
                printf("\nOther configs:\n\tWindow size..........: %u\n\tRetransmition timeout: %lu\n\tProbability..........: %.1f\n\tAdapitve timer.......: %s\n\n", 
                        configs->N, configs->rto_usec, configs->probability, (configs->is_adaptive) ? "true" : "false");
                
                printf("Press enter key to continue ...\n");
                getchar();
        }        
}

int list(int socket, struct gbn_send_params *params, struct gbn_config *config)
{
        fd_set read_fds;
        struct timeval tv;
        char *cmd_message = make_cmd_segment(LIST);
        int retval;

        while (true) {
                
                FD_ZERO(&read_fds);
                FD_SET(socket, &read_fds);
                tv.tv_sec = 0;
                tv.tv_usec = config->rto_usec;

                gbn_send(socket, cmd_message, sizeof(gbn_ftp_header_t), NULL, config);

                retval = select(socket + 1, &read_fds, NULL, NULL, &tv);

                if (retval == -1) {
                        error_handler("\"select()\" failed.");
                        return -1;
                } 
                
                if (retval)
                        break;
        }

        params->next_seq_num ++;

        return 0;

}

void main_menu(int socket, struct gbn_send_params *params, struct gbn_config *config)
{
        char choice;

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
                        case 'L': 
                                list(socket, params, config);
                                break;
                        case 'P': 
                        case 'G':
                                printf("Not implemented yet!\n\n");
                                break;
                        case 'Q': printf("Bye bye!\n\n"); return;
                        default:
                                fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                                abort();
                }
        }      
}

int main(int argc, char **argv)
{
        int sockfd;
        unsigned short int server_port;
        char address_string[ADDRESS_STRING_LENGTH];
        struct gbn_config config;
        enum app_usages modality;
        struct gbn_send_params send_params;

        srand(time(0));

        memset(address_string, 0x0, ADDRESS_STRING_LENGTH);

        init_send_params(&send_params);
        memcpy(&config, &DEFAULT_GBN_CONFIG, sizeof(config));
        server_port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, address_string, &config, &server_port);

        switch (modality) {
                case STANDARD: 
                        print_info_about_conn(address_string, server_port, &config);
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
                        exit_client(EXIT_SUCCESS);
                        break;
                case VERSION: 
                        printf("\n\tgbn-ftp-client version 1.0 (developed by tibwere)\n\n");    
                        exit_client(EXIT_SUCCESS);
                        break;
                case ERROR:
                        printf("Unable to parse command line.\n");
                        exit_client(EXIT_FAILURE);   
                        break;                   
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();        
        }   

        if ((sockfd = connect_to_server(address_string, server_port, &config)) == -1)
                exit_client(EXIT_FAILURE);

        printf("Succesfully connected (fd = %d)!\n", sockfd);                   

        main_menu(sockfd, &send_params, &config);

        close(sockfd);  
        return 0;
}

