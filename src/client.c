#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "gbnftp.h" 
#include "common.h"

#define ADDRESS_STRING_LENGTH 1024
#define CLS system("clear")

int sockfd;
unsigned short int server_port;
struct sockaddr_in server_addr;
struct gbn_config config;
struct gbn_send_params params;

extern const struct gbn_config DEFAULT_GBN_CONFIG;
extern char *optarg;
extern int opterr;

void exit_client(int status) 
{
        /* TODO: implementare chiusura pulita */
        exit(status);
}

enum app_usages parse_cmd(int argc, char **argv, char *address)
{
        int opt;
        bool valid_cmd = false;

        struct option long_options[] = {
                {"address",     required_argument,      0, 'a'},
                {"port",        required_argument,      0, 'p'},
                {"windowsize",  required_argument,      0, 'N'},
                {"rto",         required_argument,      0, 't'},
                {"adaptive",    no_argument,            0, 'A'},
                {"probability", required_argument,      0, 'P'},
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
                                server_port = strtol(optarg, NULL, 10);
                                break;
                        case 'N':
                                if (strtol(optarg, NULL, 10) < MAX_SEQ_NUMBER / 2)
                                        config.N = strtol(optarg, NULL, 10);
                                break;
                        case 't':
                                config.rto_msec = strtol(optarg, NULL, 10);
                                break;
                        case 'A':
                                config.is_adaptive = true;
                                break;
                        case 'P':
                                config.probability = strtol(optarg, NULL, 10) / 100;
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

int connect_to_server(const char *address_string)
{
        int fd;

        if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                error_handler("\"socket()\" failed."); 
                return -1;
        }

        memset(&server_addr, 0x0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, address_string, &server_addr.sin_addr) <= 0) {
                error_handler("\"inet_pton()\" failed."); 
                return -1;
        }        
	
	if (connect(fd, (struct sockaddr *) &server_addr, sizeof(struct sockaddr_in)) == -1) {
                error_handler("\"connect()\" failed.");
                return -1;
        }
	
	return fd;
}

void print_info_about_conn(const char* address_string)
{
        char choice;

        printf("Connecting to %s:%u ...\n", address_string, server_port);
        choice = multi_choice("Do you want to see more details?", "yn", 2);

        if (choice == 'Y')
        {
                printf("\nOther configs:\n\tWindow size..........: %u\n\tRetransmition timeout: %lu\n\tProbability..........: %.1f\n\tAdapitve timer.......: %s\n\n", 
                        config.N, config.rto_msec, config.probability, (config.is_adaptive) ? "true" : "false");
                
                printf("Press enter key to continue ...\n");
                getchar();
        }        
}

void main_menu()
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
                                gbn_send_ctrl_message(sockfd, LIST, (struct sockaddr *)&server_addr, &params, &config);
                                break;
                        case 'P':
                                gbn_send_ctrl_message(sockfd, PUT, (struct sockaddr *)&server_addr, &params, &config);
                                break; 
                        case 'G':
                                gbn_send_ctrl_message(sockfd, GET, (struct sockaddr *)&server_addr, &params, &config);
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
        enum app_usages modality;
        char address_string[ADDRESS_STRING_LENGTH];

        memset(address_string, 0x0, ADDRESS_STRING_LENGTH);

        memcpy(&config, &DEFAULT_GBN_CONFIG, sizeof(config));
        server_port = DEFAULT_PORT;

        modality = parse_cmd(argc, argv, address_string);

        switch (modality) {
                case STANDARD: 
                        print_info_about_conn(address_string);
                        break;
                case HELP:
                case VERSION: 
                        printf("Not yet implemented\n");    
                        exit_client(EXIT_FAILURE);
                        break;
                case ERROR:
                        printf("Unable to parse command line.\n");
                        exit_client(EXIT_FAILURE);   
                        break;                   
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();        
        }   

        if ((sockfd = connect_to_server(address_string)) == -1)
                exit_client(EXIT_FAILURE);

        printf("Succesfully connected!\n");                
        init_send_params(&params);   

        main_menu();

        close(sockfd);  
        return 0;
}

