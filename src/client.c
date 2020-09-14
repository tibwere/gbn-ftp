/*
 * File..: client.c
 * Autore: Simone Tiberi M.0252795
 *
 */

/* LIBRERIE STANDARD */               
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


/* LIBRERIE CUSTOM */
#include "gbnftp.h" 
#include "common.h"


/* MACRO DI PRECOMPILAZIONE */
#define ADDRESS_STRING_LENGTH 1024
#define HEADER_FILE_LENGTH 241

#ifndef DEBUG
        /* sequenza speciale che permette di pulire lo schermo */
        #define cls() printf("\033[2J\033[H")
#else   
        #define cls() 
#endif

#define l_request_loop(status_ptr, can_connect) request_loop(-1, LIST, NULL, status_ptr, NULL, can_connect)
#define g_request_loop(writefd, filename, status_ptr, delete_file, can_connect) request_loop(writefd, GET, filename, status_ptr, delete_file, can_connect)
#define p_request_loop(writefd, filename, status_ptr, already_exists, can_connect) request_loop(writefd, PUT, filename, status_ptr, already_exists, can_connect)


/* STRUTTURE DATI */
struct put_args {
        int fd;                                 /* Descrittore del file su cui si deve operare */
        unsigned int base;                      /* Variabile base del protocollo gbn associata alla connessione */
        unsigned int next_seq_num;              /* Variabile next_seq_num del protocollo gbn associata alla connessione */
        unsigned int expected_seq_num;          /* Variabile expected_seq_num del protocollo gbn associata alla connessione */
        unsigned int last_acked_seq_num;        /* Variabile last_acked_seq_num del protocollo gbn associata alla connessione */
        pthread_mutex_t mutex;                  /* Mutex utilizzato per la sincronizzazione fra main thread e servente */
        pthread_mutex_t cond_mutex;             /* Mutex utilizzato come guardia per la variabile condizionale */
        pthread_cond_t cond_var;                /* Variabile condizionale utilizzata in un timed_wait nella fase di connessione */
        enum connection_status status;          /* Stato della connessione {FREE, REQUEST, CONNECTED, TIMEOUT, QUIT} */
        struct timeval start_timer;             /* Struttura utilizzata per la gestione del timer */
        pthread_t tid;                          /* ID del thread servente */
        long number_of_chunks;                  /* Numero totale di chunk da inviare */
};


/* VARIABILI ESTERNE */
extern bool verbose;
extern char *optarg;
extern int opterr;


/* VARIABILI GLOBALI */
int sockfd;
struct gbn_config *config;
struct sockaddr_in request_sockaddr;
unsigned short int server_port;
struct put_args *args;
struct gbn_adaptive_timeout *adapt;
sigset_t t_set;
char header[HEADER_FILE_LENGTH];
enum app_usages modality;


/* PROTOTIPI */
void sig_handler(int signo); 
void deserialize_configuration(struct gbn_config *config, char *ser);
ssize_t send_file_chunk(void);
bool handle_retransmit(int counter);
void *put_sender_routine(void *dummy);
void exit_client(int status); 
enum app_usages parse_cmd(int argc, char **argv, char *address);
bool set_sockadrr_in(struct sockaddr_in *server_sockaddr, const char *address_string, unsigned short int port);
ssize_t send_request(enum message_type type, const char *filename, size_t filename_length);
ssize_t send_ack(enum message_type type, unsigned int seq_num, bool is_last);
bool request_loop(int writefd, enum message_type type, const char *filename, enum connection_status *status_ptr, bool *error_ret, bool *can_connect);
bool lg_connect_loop(int writefd, enum message_type type, enum connection_status *status_ptr);
bool p_connect_loop(void);
bool list(void);
bool get_file(void);
bool init_put_args(void);
bool dispose_put_args(void);
bool put_file(void);
bool check_installation(void);
bool load_header();

                                      
/*
 * funzione:	sig_handler
 * 
 * descrizione:	Gestore dei segnali dell'applicazione client
 *
 * parametri:	signo (int):    Numero del segnale catturato
 *
 */
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

/*
 * funzione:	deserialize_configuration
 * 
 * descrizione:	Parser della struct di configurazione a partire dalla rappresentazione a stringa
 *
 * parametri:	config (struct gbn_config *):   Puntatore alla struct da configurare
 *              ser (char *):                   Stringa da converire
 *
 */
void deserialize_configuration(struct gbn_config *config, char *ser)
{
        config->N = strtol(strtok(ser, ";"), NULL, 10);
        config->rto_usec = strtol(strtok(NULL, ";"), NULL, 10);
        config->is_adaptive = strtol(strtok(NULL, ";"), NULL, 10);

        return;
}

/*
 * funzione:	send_file_chunk
 * 
 * descrizione:	Funzione responsabile dell'invio di un chunk del file
 *              n.b.    Implementa la funzionalità rdt_send(data) dell'automa sender GBN
 *                      nel caso in cui sono disponibili ancora numeri di sequenza utilizzabili
 *
 * return:	Numero di byte inviati (ssize_t)
 *
 */
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
                set_last(&header, (args->next_seq_num == args->number_of_chunks));   

                if ((wsize = gbn_send(sockfd, header, buff, rsize, NULL)) == -1) {
                        if (errno != ECONNREFUSED)
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

                if (config->is_adaptive && args->status == CONNECTED) {
                        if (adapt->restart) {
                                gettimeofday(&adapt->saved_tv, NULL);
                                adapt->seq_num = args->next_seq_num;
                                adapt->restart = false;
                        }
                }

                if (args->base == args->next_seq_num) 
                        gettimeofday(&args->start_timer, NULL);

                args->next_seq_num++;

                if (pthread_mutex_unlock(&args->mutex)) {
                        perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                        return -1;
                }

                return wsize; 
        }  

        return 0;    
}

/*
 * funzione:	handle_retransmit
 * 
 * descrizione:	Funzione responsabile dell'invio dell'intera finestra a seguito della scadenza di un timeout
 *              n.b.    Implementa la funzionalità timeout dell'automa sender GBN
 *
 * parametri:   counter (int):  Numero di timeout scaduti consecutivamente per la medesima base
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool handle_retransmit(int counter) 
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
        
        if (config->is_adaptive) {
                adapt->estimatedRTT = MIN(adapt->estimatedRTT * counter, MAX_ERTT_SCALE * config->rto_usec);
                adapt->devRTT = MIN(adapt->devRTT * counter, MAX_DRTT_USEC);
        }

        if (pthread_mutex_unlock(&args->mutex)) {
                perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                return -1;
        }

        for (unsigned int i = base; i < next_seq_num; ++i)
                if (send_file_chunk() == -1)
                        return false;

        if (get_status_safe(&args->status, &args->mutex) != QUIT)
                set_status_safe(&args->status, CONNECTED, &args->mutex);

        return true;
}

/*
 * funzione:	put_sender_routine
 * 
 * descrizione:	Entry point del thread spawnato per l'invio dei chunk al server in caso di operazione PUT
 *
 * parametri:   dummy (void *): Valore non utilizzato (NULL)
 *
 * return:      Valore d'uscita del thread (NULL)	    
 *
 */
void *put_sender_routine(__attribute__((unused)) void *dummy) 
{
        int ret;
        struct timeval tv;
        struct timespec ts;
        unsigned short timeout_counter = 0;
        unsigned int last_base_for_timeout = 0;

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
                                } else {

                                        if (pthread_mutex_lock(&args->cond_mutex)) {
                                                perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_cond_mutex)");
                                                return false;
                                        }

                                        while (args->next_seq_num >= args->base + config->N) {

                                                if (config->is_adaptive) {
                                                        ts.tv_sec = args->start_timer.tv_sec + floor((double) (get_adaptive_rto_safe(adapt, &args->mutex) + args->start_timer.tv_usec) / (double) 1000000);
                                                        ts.tv_nsec = ((args->start_timer.tv_usec + get_adaptive_rto_safe(adapt, &args->mutex)) % 1000000) * 1000;
                                                } else {
                                                        ts.tv_sec = args->start_timer.tv_sec + floor((double) (config->rto_usec + args->start_timer.tv_usec) / (double) 1000000);
                                                        ts.tv_nsec = ((args->start_timer.tv_usec + config->rto_usec) % 1000000) * 1000;                                                       
                                                }
                                                
                                                ret = pthread_cond_timedwait(&args->cond_var, &args->cond_mutex, &ts);

                                                if (ret > 0) {
                                                        if (ret != ETIMEDOUT) {
                                                                perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_condvar)");
                                                                return false;
                                                        } else {
                                                                if (get_status_safe(&args->status, &args->mutex) != QUIT)
                                                                        set_status_safe(&args->status, TIMEOUT, &args->mutex);
                                                                break;
                                                        }

                                                }
                                        }

                                        if (pthread_mutex_unlock(&args->cond_mutex)) {
                                                perr("{ERROR} [Sender Thread] Syncronization protocol for worker threads broken (worker_cond_mutex)");
                                                return false;
                                        }
                                }

                                break;

                        case TIMEOUT:

                                #ifdef DEBUG
                                if (config->is_adaptive) {
                                        printf("{DEBUG} [Sender Thread] Timeout event (%d) (%ld usec)\n", 
                                                args->base, 
                                                adapt->estimatedRTT + 4 * adapt->devRTT);
                                } else {
                                        printf("{DEBUG} [Sender Thread] Timeout event (%d)\n", args->base);
                                }
                                #endif       

                                if (get_gbn_param_safe(&args->base, &args->mutex) == last_base_for_timeout) {

                                        if (timeout_counter < MAX_TO) {
                                                if (!handle_retransmit(timeout_counter))
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
                                        
                                        if (!handle_retransmit(timeout_counter))
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

/*
 * funzione:	exit_client
 * 
 * descrizione:	Funzione responsabile della fase di chiusura del client
 *
 * parametri:   status (int):   Valore d'uscita del processo 
 *
 */
void exit_client(int status) 
{
        if (args) {
                set_status_safe(&args->status, QUIT, &args->mutex);
                pthread_join(args->tid, NULL);
        }

        dispose_put_args();

        if (config) {
                if (config->is_adaptive) {
                        if (adapt) {
                                free(adapt);
                        }
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

/*
 * funzione:	parse_cmd
 * 
 * descrizione:	Funzione responsabile del parsing della command line
 *
 * parametri:   argc (int):             Numero di parametri immessi da linea di comando 
 *              argv (char **):         Vettore delle stringhe immesse da linea di comando 
 *              address (char *):       Stringa che rappresenta l'indirizzo a cui ci si vuole connettere
 *
 * return:      Modalita' d'utilizzo dell'applicazione scelta (enum app_usages)    
 *
 */
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
                                return ERROR;
                }
        }

        return (valid_cmd) ? STANDARD : ERROR;
}

/*
 * funzione:	set_sockadrr_in
 * 
 * descrizione:	Funzione responsabile dell'impostazione dei parametri della struttura sockaddr_in
 *
 * parametri:   server_sockaddr (struct sockaddr_in *): Puntatore alla struttra sockaddr_in da impostare        
 *              address_string (const char *):          Stringa rappresentante l'indirizzo da inserire 
 *              port (unsigned short int):              Numero di porta da impostare       
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti    
 *
 */
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

/*
 * funzione:	send_request
 * 
 * descrizione:	Funzione responsabile dell'invio di un segmento di request per la prima fase della connessione a 3 vie
 * 
 * parametri:   type (enum message_type):       Tipo di richiesta da inoltrare
 *              filename (const char *):        Nome del file da inviare/ricevere (NULL nel caso di richiesta LIST)      
 *              filename_length (size_t):       Lunghezza del nome del file (0 nel caso di richiesta LIST)
 *
 * return:	Numero di byte inviati (ssize_t)
 *
 */
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

/*
 * funzione:	send_ack
 * 
 * descrizione:	Funzione responsabile dell'invio di un segmento di ACK
 * 
 * parametri:   type (enum message_type):       Tipo di richiesta da inoltrare
 *              seq_num (unsigned int):         Numero di sequenza da riscontrare      
 *              is_last (size_t):               true    se riscontra un segmento di tipo LAST
 *                                              false   altrimenti
 *
 * return:	Numero di byte inviati al receiver (ssize_t)
 *
 */
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

/*
 * funzione:	request_loop
 * 
 * descrizione:	Funzione responsabile della gestione della fase di richiesta connessione al server
 *
 * parametri:   writefd (int):                          Descrittore file da chiudere nel caso di failure
 *              type (enum message_type):               Tipo di richiesta
 *              filename (const char *):                Nome del file da inviare/ricevere (NULL nel caso di richiesta LIST) 
 *              status_ptr (enum connection_status *):  Puntatore ad uno stato di connessione
 *              error_ret (bool *):                     Puntatore ad una variabile booleana da settare a true in caso di errore
 *              can_connect (bool *):                   Puntatore ad una variabile booleana da settare a false nel caso in cui non ci si riesca a connettere                      
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool request_loop(int writefd, enum message_type type, const char *filename, enum connection_status *status_ptr, bool *error_ret, bool *can_connect)
{
        struct timeval tv;
        int retval;
        ssize_t recv_size;
        gbn_ftp_header_t recv_header;
        struct sockaddr_in addr;
        size_t header_size = sizeof(gbn_ftp_header_t);
        fd_set read_fds, std_fds;
        char config_ser[CHUNK_SIZE];
        int i = 1;

        FD_ZERO(&std_fds);
        FD_SET(sockfd, &std_fds);

        memset(&addr, 0x0, sizeof(struct sockaddr_in));
        memset(config_ser, 0x0, CHUNK_SIZE);

        while(*status_ptr == REQUEST) {

                if (i >= MAX_CONNECTION_ATTEMPT) {
                        *can_connect = false;
                        return false;
                }

                #ifdef DEBUG
                printf("{DEBUG} [Main Thread] Connecting to server (try no. %d)\n", i);
                #endif

                if (send_request(type, filename, (filename) ? strlen(filename) : 0) == -1) 
                        return false; 

                read_fds = std_fds;
                tv.tv_sec = floor(i * (double) config->rto_usec / (double) 1000000);
                tv.tv_usec = (i * config->rto_usec) % 1000000;
                ++i;

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

/*
 * funzione:	lg_connect_loop
 * 
 * descrizione:	Funzione responsabile della gestione della fase di scambio messaggi (status = CONNECTED) per le richieste LIST e GET
 *
 * parametri:   writefd (int):                          Descrittore file su cui scrivere
 *              type (enum message_type):               Tipo di richiesta
 *              status_ptr (enum connection_status *):  Puntatore ad uno stato di connessione
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool lg_connect_loop(int writefd, enum message_type type, enum connection_status *status_ptr)
{
        unsigned int expected_seq_num, last_acked_seq_num;
        gbn_ftp_header_t recv_header;
        char payload[CHUNK_SIZE];
        ssize_t recv_size;
        size_t header_size = sizeof(gbn_ftp_header_t);
        fd_set read_fds, std_fds;
        struct timeval tv;
        int retval;

        expected_seq_num = 1;
        last_acked_seq_num = 0;

        FD_ZERO(&std_fds);
        FD_SET(sockfd, &std_fds);

        while (*status_ptr == CONNECTED) {
                memset(payload, 0x0, CHUNK_SIZE);

                read_fds = std_fds;
                tv.tv_sec = MAX_TO_SEC;
                tv.tv_usec = 0;

                if ((retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv)) == -1) {
                        perr("{ERROR} [Main Thread] Unable to receive pkt from server (select)");
                        return false;        
                }

                if (retval) {

                        if((recv_size = gbn_receive(sockfd, &recv_header, payload, NULL)) == -1) {
                                if (errno == ECONNREFUSED) {
                                        printf("Connection with server lost\n");
                                        return false;
                                }
                                
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
                } else {
                        printf("Unable to contact anymore the server, please try again later\n");
                        return false;
                }
        }

        return true;
}

/*
 * funzione:	p_connect_loop
 * 
 * descrizione:	Funzione responsabile della gestione della ricezione degli ACK nel caso di richiesta PUT (status = CONNECTED)
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool p_connect_loop(void)
{
        gbn_ftp_header_t recv_header;
        struct timeval tv;

        while (args->status != QUIT) {

                if (gbn_receive(sockfd, &recv_header, NULL, NULL) == -1) {
                        if (errno == ECONNREFUSED) {
                                printf("Connection with server lost\n");
                                return false;
                        }
                        
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

                if (pthread_cond_signal(&args->cond_var)) {
                        perr("{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_condvar)");
                        return false;
                }

                if (is_last(recv_header)) {

                        set_status_safe(&args->status, QUIT, &args->mutex);
                        
                        #ifdef DEBUG
                        printf("{DEBUG} [Main Thread] Comunication with server has expired\n");
                        #endif

                        break;
                }

                if (pthread_mutex_lock(&args->mutex)) {
                        perr("{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_mutex)");
                        return false;
                }

                if (config->is_adaptive && args->status == CONNECTED) {
                        if (adapt->seq_num <= get_sequence_number(recv_header)) {
                                gettimeofday(&tv, NULL);
                                adapt->sampleRTT = elapsed_usec(&adapt->saved_tv, &tv);
                                adapt->estimatedRTT = ((1 - ALPHA) * adapt->estimatedRTT) + (ALPHA * adapt->sampleRTT);
                                adapt->devRTT = ((1 - BETA) * adapt->devRTT) + (BETA * ABS(adapt->sampleRTT - adapt->estimatedRTT));
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
        }

        return true;
}

/*
 * funzione:	list
 * 
 * descrizione:	Funzione responsabile dell'intera esecuzione dell'operazione LIST
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool list(void) 
{
        enum connection_status status = REQUEST;
        bool can_connect;

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perr("{ERROR} [Main Thread] Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        can_connect = true;
        if (!l_request_loop(&status, &can_connect)) {
                if (!can_connect) {
                        printf("\nServer is busy...\nPlease press enter to get back to menu and try again later\n");
                        getchar();
                        return true;
                } 

                return false;
        }

        cls();
        printf("AVAILABLE FILE ON SERVER\n\n");

        if (!lg_connect_loop(STDIN_FILENO, LIST, &status))
                return false;

        close(sockfd);

        printf("\n\nPress any key to get back to menu ");
        fflush(stdout);
        getchar();

        return true;
}

/*
 * funzione:	get_file
 * 
 * descrizione:	Funzione responsabile dell'intera esecuzione dell'operazione GET
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool get_file(void) 
{
        enum connection_status status;
        int fd;
        char filename[CHUNK_SIZE];
        char path[PATH_SIZE];
        bool delete_file = false;
        bool can_connect;
        char choice;

        status = REQUEST;

        memset(path, 0x0, PATH_SIZE);
        memset(filename, 0x0, CHUNK_SIZE);

get_filename_g:
        cls();
        printf("Which file do you want to download? ");
        fflush(stdout);
        get_input(CHUNK_SIZE, filename, true);

        if (filename[0] == '.') {
                printf("Hidden file cannot be requested to server\nPlease press enter and retry\n");
                getchar();
                goto get_filename_g;
        }

        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-download/%s", getenv("USER"), filename);

        if((fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
                if (errno != EEXIST) {
                        perr("{ERROR} [Main Thread] Unable to create a local copy of remote file");
                        return false;
                } else {
                        choice = multi_choice("Do you want to overwrite existing file?", "yn", 2);

                        if (choice == 'Y') {
                                if((fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
                                        perr("{ERROR} [Main Thread] Unable to create a local copy of remote file");
                                        return false;
                                }
                        } else {
                                return true;
                        }
                }
        }          

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perr("{ERROR} [Main Thread] Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        can_connect = true;
        if (!g_request_loop(fd, filename, &status, &delete_file, &can_connect)) {
                if (!can_connect) {
                        printf("\nServer is busy...\nPlease press enter to get back to menu and try again later\n");
                        getchar();
                        return true;
                }

                if (delete_file) {
                        printf("Selected file does not exists on server\nPress enter to get back to menu\n");
                        getchar();
                        remove(path);
                        return true;
                } 

                return false;
        }

        printf("Wait for download ...\n");
        if (!lg_connect_loop(fd, GET, &status))
                return false;

        close(fd);
        close(sockfd);

        printf("\n\nFile succesfully downloaded\nPress enter to get back to menu\n");
        getchar();

        return true;
}

/*
 * funzione:	init_put_args
 * 
 * descrizione:	Funzione responsabile dell'inizializzazione della struct args per la richiesta PUT
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
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

        if (pthread_mutex_init(&args->cond_mutex, NULL)) {
                perr("{ERROR} [Main Thread] Unable to allocate metadata for PUT operation (pthread_mutex_init)");
                return false;
        }

        if (pthread_cond_init(&args->cond_var, NULL)) {
                perr("{ERROR} [Main Thread] Unable to allocate metadata for PUT operation (pthread_cond_init)");
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

/*
 * funzione:	dispose_put_args
 * 
 * descrizione:	Funzione responsabile della deallocazione delle aree di memoria allocate per la richiesta PUT
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool dispose_put_args(void)
{
        if (args != NULL) {
                if (pthread_mutex_destroy(&args->mutex)) {
                        perr("{ERROR} [Main Thread] unable to free metadata used for PUT operation");
                        return false;
                }

                if (pthread_mutex_destroy(&args->cond_mutex)) {
                        perr("{ERROR} [Main Thread] unable to free metadata used for PUT operation");
                        return false;
                }

                if (pthread_cond_destroy(&args->cond_var)) {
                        perr("{ERROR} [Main Thread] unable to free metadata used for PUT operation");
                        return false;
                }

                if (args->fd != -1)
                        close(args->fd);

                free(args);
                args = NULL;

                if (config) {
                        if (config->is_adaptive) {
                                free(adapt);
                                adapt = NULL;
                        }
                }

        }

        return true;
}

/*
 * funzione:	put_file
 * 
 * descrizione:	Funzione responsabile dell'intera esecuzione dell'operazione PUT
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool put_file(void) 
{
        size_t filename_size;
        char filename[CHUNK_SIZE];
        char path[PATH_SIZE];
        char choice;
        bool already_exists = false;
        bool can_connect;
        struct stat buf;

        if (!init_put_args())
                return false;

        while(true) {
        
                memset(path, 0x0, PATH_SIZE);
                memset(filename, 0x0, CHUNK_SIZE);

                cls();
                printf("Which file do you want to upload to server (full path)? ");
                fflush(stdout);
                get_input(PATH_SIZE, path, true);

get_filename_p:
                printf("Choose the name for the upload (default: %s)? ", basename(path));
                fflush(stdout);
                filename_size = get_input(CHUNK_SIZE, filename, false);

                if (filename_size > 0) {
                        if (filename[0] == '.') {
                                printf("File cannot start by '.'\nPlease press enter and retry\n");
                                getchar();
                                goto get_filename_p;
                        }
                } else {
                        if (basename(path)[0] == '.') {
                                printf("File cannot start by '.'\nPlease press enter and retry\n");
                                getchar();
                                goto get_filename_p;
                        }
                }

                if((args->fd = open(path, O_RDONLY)) == -1) {
                        if (errno == ENOENT) {
                                printf("Selected file does not exists\n");
                                choice = multi_choice("Do you want to retry?", "yn", 2);

                                if (choice == 'Y') {
                                        continue;
                                } else {
                                        dispose_put_args();
                                        return true;
                                }

                        }
                        perr("{ERROR} [Main Thread] Unable to open chosen file");
                        return false;
                }

                if (fstat(args->fd, &buf) != 0) {
                        perr("{ERROR} [Main Thread] Unable to check if selected file is a directory");                        
                        return false;
                }

                if (S_ISDIR(buf.st_mode)) {
                        printf("Selected file is a directory\nPress enter and get back to menu\n");
                        getchar();

                        dispose_put_args();
                        return true;
                }

                args->number_of_chunks = ceil((double) lseek(args->fd, 0, SEEK_END) / (double) CHUNK_SIZE);
                lseek(args->fd, 0, SEEK_SET);

                break;
        }

        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perr("{ERROR [Main Thread] Unable to get socket file descriptor (REQUEST)");
                return false;
        }

        can_connect = true;
        if (!p_request_loop(args->fd, (filename_size) ? filename : basename(path), &args->status, &already_exists, &can_connect)) {
                if (!can_connect) {
                        dispose_put_args();
                        printf("\nServer is busy...\nPlease press enter to get back to menu and try again later\n");
                        getchar();
                        return true;
                }

                if (already_exists) {
                        dispose_put_args();
                        printf("Selected file already exists on server\nPress enter to get back to menu\n");
                        getchar();
                        return true;
                }

                return false;                
        }

        if (pthread_create(&args->tid, NULL, put_sender_routine, NULL)) {
                perr("{ERROR} [Main Thread] Unable to spawn sender thread");
                return false;
        }

        printf("Upload started\n");

        if (!p_connect_loop())
                return false;

        pthread_join(args->tid, NULL);

        pthread_sigmask(SIG_BLOCK, &t_set, NULL);
        dispose_put_args();
        pthread_sigmask(SIG_UNBLOCK, &t_set, NULL);

        close(sockfd);

        #ifdef DEBUG
        printf("{DEBUG} [Main Thread] Joined sender thread\n");
        #endif

        printf("\nFile succesfully uploaded\nPress enter to get back to menu\n");
        getchar();

        return true;
}

/*
 * funzione:	check_installation
 * 
 * descrizione:	Funzione responsabile della verifica della corretta installazione del client
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
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

/*
 * funzione:	load_header
 * 
 * descrizione:	Funzione responsabile del caricamento di un header per l'applicazione in stile figlet
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
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

        if ((fd = open(strncat(dirname(path), "/../assets/header.txt", 22), O_RDONLY)) == -1) {
                perr("{ERROR} [Main Thread] Unable to load figlet header");
                return false;
        }

        if (read(fd, header, HEADER_FILE_LENGTH) == -1) {
                perr("{ERROR} [Main Thread] Unable to read from figlet file");
                return false;
        }

        return true;
}

/*
 * funzione:	main
 * 
 * descrizione:	Entry point dell'applicazione
 *
 * parametri:   argc (int):             Numero di parametri immessi da linea di comando 
 *              argv (char **):         Vettore delle stringhe immesse da linea di comando 
 *
 * return:      Valore d'uscita del processo    
 *
 */
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

        #ifndef TEST
        srand(time(0));
        #endif

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
                        printf("\t\t-p [--port]\t<port>\t\tServer port\n");
                        printf("\t\t-v [--version]\t\t\tVersion of gbn-ftp-client\n");
                        printf("\t\t-V [--verbose]\t\t\tPrint verbose version of error\n\n");
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