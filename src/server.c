/*
 * File..: server.c
 * Autore: Simone Tiberi M.0252795
 *
 */

/* LIBRERIE STANDARD */
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
#include <signal.h>


/* LIBRERIE CUSTOM */
#include "gbnftp.h" 
#include "common.h"


/* MACRO IN PRECOMPILAZIONE */
#define ID_STR_LENGTH 64
#define CMD_SIZE 128
#define START_WORKER_PORT 49152
#define MAX_PORT_NUM 65535


/* STRUTTURE DATI */
struct worker_info {
        int socket;                                     /* Socket dedicata per la comunicazione */
        char id_string[ID_STR_LENGTH];                  /* Stringa identificativa del thread utile nelle stampe di debug */
        unsigned short port;                            /* Porta associata al thread worker */
        volatile enum connection_status status;         /* Stato della connessione {FREE, REQUEST, CONNECTED, TIMEOUT, QUIT} */
        struct sockaddr_in client_sockaddr;             /* Struttura sockaddr_in del client utilizzata nelle funzioni di send e receive */
        pthread_mutex_t mutex;                          /* Mutex utilizzato per la sincronizzazione fra main thread e servente i-esimo */
        pthread_mutex_t cond_mutex;                     /* Mutex utilizzato come guardia per la variabile condizionale */
        pthread_cond_t cond_var;                        /* Variabile condizionale utilizzata in un timed_wait nella fase di connessione */
        enum message_type modality;                     /* Modalità scelta d'utilizzo */
        char filename[CHUNK_SIZE];                      /* Nome del file su cui lavorare */
        volatile unsigned int base;                     /* Variabile base del protocollo gbn associata alla connessione */
        volatile unsigned int next_seq_num;             /* variabile next_seq_num del protocollo gbn associata alla connessione */                 
        unsigned int expected_seq_num;                  /* variabile expected_seq_num del protocollo gbn associata alla connessione */
        unsigned int last_acked_seq_num;                /* variabile last_acked_seq_num del protocollo gbn associata alla connessione */
        struct timeval start_timer;                     /* Struttura utilizzata per la gestione del timer */
        pthread_t tid;                                  /* ID del thread servente*/
        int fd;                                         /* Descrittore del file su cui si deve operare */ 
        long number_of_chunks;                          /* Numero totale di chunk da inviare */
        bool to_be_deleted;

        #ifdef TEST
        struct timeval first_ack;                       /* Istante di tempo in cui si riceve il primo ACK */
        struct timeval full_window;                     /* Istante di tempo in cui si invia l'intera prima finestra */
        struct timeval start_tx;                        /* Istante di tempo in cui si inizia ad inviare */
        struct timeval end_tx;                          /* Istante di tempo in cui si termina con l'invio */
        long last_rtt;                                  /* Entita' dell'ultimo RTT stimato */
        #endif
};


/* VARIABILI ESTERNE */
extern bool verbose;
extern char *optarg;
extern int opterr;


/* VARIABILI GLOBALI */
int acceptance_sockfd;
unsigned short int acceptance_port;
struct worker_info *winfo;
struct gbn_adaptive_timeout *adapt;
struct gbn_config *config;
fd_set all_fds;
pthread_rwlock_t tmp_ls_rwlock;
sigset_t t_set;
long concurrenty_connections;
enum app_usages modality;


/* PROTOTIPI */
#ifdef DEBUG
void sig_handler(int signo); 
#else
void sig_handler(__attribute__((unused)) int signo);
#endif 
bool handle_retransmit(long id, int counter); 
ssize_t send_file_chunk(long id);
bool lg_send_new_port_mess(long id);
bool p_send_new_port_mess(long id);
ssize_t p_send_ack(long id, unsigned int seq_num, bool is_last); 
bool update_tmp_ls_file(int id);
bool handle_ack_messages(long id);
void *receiver_routine(void *args);
void *sender_routine(void *args);
void exit_server(int status);
enum app_usages parse_cmd(int argc, char **argv);
int init_socket(unsigned short int port);
long get_available_worker(const struct sockaddr_in *addr, bool *already_handled_ptr);
bool start_sender(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, bool *can_open_ptr);
bool start_receiver(long index, struct sockaddr_in *client_sockaddr, const char *payload, bool *can_open_ptr);
bool reset_worker_info(int id, bool need_destroy, bool need_create);
bool init_worker_info(void);
bool handle_recv(int id);
bool send_error_message(int id);
bool dispose_leaked_resources(void);
bool acceptance_loop(void);
bool check_installation(void); 


/*
 * funzione:	sig_handler
 * 
 * descrizione:	Gestore dei segnali dell'applicazione server
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
        
        exit_server(EXIT_SUCCESS);
}

/*
 * funzione:	is_valid_port
 * 
 * descrizione:	Funzione che verifica se il numero di porta inserito tramite cmdline è valido
 *
 * parametri:   port (unsigned short int):      Numero di porta da testare
 *
 * return:	true    nel caso in cui il numero è valido
 *              false   altrimenti       
 *
 */
bool is_valid_port(unsigned short int port)
{
        bool check_for_std_range = port > 0 && port < MAX_PORT_NUM;
        bool check_for_wrk_range = port < START_WORKER_PORT && port > START_WORKER_PORT + concurrenty_connections - 1; 

        return check_for_std_range && check_for_wrk_range; 
}

/*
 * funzione:	handle_retransmit
 * 
 * descrizione:	Funzione responsabile dell'invio dell'intera finestra a seguito della scadenza di un timeout
 *              n.b.    Implementa la funzionalità timeout dell'automa sender GBN
 *
 * parametri:   id (long):      Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo
 *              counter (int):  Numero di timeout scaduti consecutivamente per la medesima base
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool handle_retransmit(long id, int counter) 
{
        unsigned int base; 
        unsigned int next_seq_num;
        char error_message[ERR_SIZE];
        
        memset(error_message, 0x0, ERR_SIZE);
        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_mutex)", winfo[id].id_string);

        if (pthread_mutex_lock(&winfo[id].mutex)) {
                perr(error_message);
                return false;
        }

        base = winfo[id].base - 1;
        next_seq_num = winfo[id].next_seq_num - 1;
        winfo[id].next_seq_num = winfo[id].base;
        lseek(winfo[id].fd, base * CHUNK_SIZE, SEEK_SET);
        gettimeofday(&winfo[id].start_timer, NULL);

        if (config->is_adaptive) {
                adapt[id].estimatedRTT = MIN(adapt[id].estimatedRTT * counter, MAX_ERTT_SCALE * config->rto_usec);
                adapt[id].devRTT = MIN(adapt[id].devRTT * counter, MAX_DRTT_USEC);
        }

        if (pthread_mutex_unlock(&winfo[id].mutex)) {
                perr(error_message);
                return false;
        }

        for (unsigned int i = base; i < next_seq_num; ++i)
                if (send_file_chunk(id) == -1)
                        return false;

        if (get_status_safe(&winfo[id].status, &winfo[id].mutex) != QUIT)
                set_status_safe(&winfo[id].status, CONNECTED, &winfo[id].mutex);

        return true;
}

/*
 * funzione:	send_file_chunk
 * 
 * descrizione:	Funzione responsabile dell'invio di un chunk del file
 *              n.b.    Implementa la funzionalità rdt_send(data) dell'automa sender GBN
 *                      nel caso in cui sono disponibili ancora numeri di sequenza utilizzabili
 *
 * parametri:   id (long):      Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo
 *
 * return:	Numero di byte inviati (ssize_t)
 *
 */
ssize_t send_file_chunk(long id)
{
        gbn_ftp_header_t header;
        char buff[CHUNK_SIZE];
        char error_message[ERR_SIZE];
        ssize_t rsize;
        ssize_t wsize;

        memset(buff, 0x0, CHUNK_SIZE);
        memset(error_message, 0x0, ERR_SIZE);

        if (winfo[id].modality == LIST) {
                if (pthread_rwlock_rdlock(&tmp_ls_rwlock)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for tmp-ls file broken", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }

                if ((rsize = read(winfo[id].fd, buff, CHUNK_SIZE)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to read from selected file", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }

                if (pthread_rwlock_unlock(&tmp_ls_rwlock)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for tmp-ls file broken", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }

        } else {
                if ((rsize = read(winfo[id].fd, buff, CHUNK_SIZE)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to read from selected file", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }
        }

        if (rsize > 0) {

                set_message_type(&header, winfo[id].modality);
                set_sequence_number(&header, get_gbn_param_safe(&winfo[id].next_seq_num, &winfo[id].mutex));
                set_ack(&header, false);      
                set_err(&header, false);  
                set_last(&header, (get_gbn_param_safe(&winfo[id].next_seq_num, &winfo[id].mutex) == winfo[id].number_of_chunks));  

                #ifdef TEST
                if (get_gbn_param_safe(&winfo[id].next_seq_num, &winfo[id].mutex) == 1)
                        gettimeofday(&winfo[id].start_tx, NULL);
                #endif

                if ((wsize = gbn_send(winfo[id].socket, header, buff, rsize, &winfo[id].client_sockaddr)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to send chunk to client", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }

                #ifdef DEBUG
                printf("{DEBUG} %s Segment no. %d %ssent\n", winfo[id].id_string, get_sequence_number(header), (wsize == 0) ? "not " : "");
                #endif

                if (pthread_mutex_lock(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_mutex)", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }  

                if (config->is_adaptive && winfo[id].status == CONNECTED) {
                        if (adapt[id].restart) {
                                gettimeofday(&adapt[id].saved_tv, NULL);
                                adapt[id].seq_num = winfo[id].next_seq_num;
                                adapt[id].restart = false;
                        }
                }                      

                if (winfo[id].base == winfo[id].next_seq_num) 
                        gettimeofday(&winfo[id].start_timer, NULL);

                #ifdef TEST
                if (winfo[id].next_seq_num == config->N)
                        gettimeofday(&winfo[id].full_window, NULL);
                #endif

                winfo[id].next_seq_num ++;                
                
                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_mutex)", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }

                return wsize; 
        }  

        return 0;    
}

/*
 * funzione:	lg_send_new_port_mess
 * 
 * descrizione:	Funzione responsabile dell'invio del messaggio di NEWPORT al client (richieste di tipo LIST e GET)
 *
 * parametri:   id (long):      Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti
 *
 */
bool lg_send_new_port_mess(long id)
{
        gbn_ftp_header_t header;
        struct timespec ts;
        struct timeval tv;
        int ret;
        ssize_t wsize;
        char error_message[ERR_SIZE];
        char serialized_config[CHUNK_SIZE];
        int i = 0;

        memset(error_message, 0x0, ERR_SIZE);
        memset(serialized_config, 0x0, CHUNK_SIZE);

        set_err(&header, false);
        set_sequence_number(&header, 0);
        set_message_type(&header, winfo[id].modality);
        set_last(&header, false);
        set_ack(&header, false);

        snprintf(serialized_config, CHUNK_SIZE, "%d;%ld;%d", config->N, config->rto_usec, config->is_adaptive);       

        while (get_status_safe(&winfo[id].status, &winfo[id].mutex) == REQUEST) {

                if (i >= MAX_CONNECTION_ATTEMPT)
                        return false;

                ++i;

                if ((wsize = gbn_send(winfo[id].socket, header, serialized_config, CHUNK_SIZE, &winfo[id].client_sockaddr)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to send NEW_PORT message", winfo[id].id_string);
                        perr(error_message);
                        return false;
                }

                #ifdef DEBUG
                printf("{DEBUG} %s NEW PORT segment %ssent\n", winfo[id].id_string, (wsize == 0) ? "not " : "");
                #endif

                if (pthread_mutex_lock(&winfo[id].cond_mutex)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_cond_mutex)", winfo[id].id_string);
                        perr(error_message);
                        return false;
                }

                while (winfo[id].status != CONNECTED) {
                        gettimeofday(&tv, NULL);
                        ts.tv_sec = tv.tv_sec + floor((double) (config->rto_usec + tv.tv_usec) / (double) 1000000);
                        ts.tv_nsec = ((tv.tv_usec + config->rto_usec) % 1000000) * 1000;
                        
                        ret = pthread_cond_timedwait(&winfo[id].cond_var, &winfo[id].cond_mutex, &ts);
                        
                        if (ret > 0 && ret != ETIMEDOUT) {
                                snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_condvar)", winfo[id].id_string);
                                perr(error_message);
                                return false;
                        } else {
                                break;
                        }
                }

                if (pthread_mutex_unlock(&winfo[id].cond_mutex)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_mutex)", winfo[id].id_string);
                        perr(error_message);
                        return false;
                }  

        }
        
        return true;
}

/*
 * funzione:	p_send_new_port_mess
 * 
 * descrizione:	Funzione responsabile dell'invio del messaggio di NEWPORT al client (richieste di tipo PUT)
 *
 * parametri:   id (long):      Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti
 *
 */
bool p_send_new_port_mess(long id)
{
        gbn_ftp_header_t send_header, recv_header;
        int ret;
        ssize_t wsize;
        fd_set read_fds, all_fds;
        struct timeval tv;
        char error_message[ERR_SIZE];
        char serialized_config[CHUNK_SIZE];
        int i = 0;

        set_err(&send_header, false);
        set_sequence_number(&send_header, 0);
        set_message_type(&send_header, PUT);
        set_last(&send_header, false);
        set_ack(&send_header, false);

        FD_ZERO(&all_fds);
        FD_SET(winfo[id].socket, &all_fds);

        memset(error_message, 0x0, ERR_SIZE);
        memset(serialized_config, 0x0, CHUNK_SIZE);

        snprintf(serialized_config, CHUNK_SIZE, "%d;%ld;%d", config->N, config->rto_usec, config->is_adaptive); 

        while (get_status_safe(&winfo[id].status, &winfo[id].mutex) == REQUEST) {

                if (i >= MAX_CONNECTION_ATTEMPT)
                        return false;

                ++i;

                if ((wsize = gbn_send(winfo[id].socket, send_header, serialized_config, CHUNK_SIZE, &winfo[id].client_sockaddr)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to send NEW_PORT message", winfo[id].id_string);
                        perr(error_message);
                        return false;
                }

                #ifdef DEBUG
                printf("{DEBUG} %s NEW PORT segment %ssent\n", winfo[id].id_string, (wsize == 0) ? "not " : "");
                #endif

                read_fds = all_fds;
                tv.tv_sec = floor((double) config->rto_usec / (double) 1000000);
                tv.tv_usec =  config->rto_usec % 1000000;

                if ((ret = select(winfo[id].socket + 1, &read_fds, NULL, NULL, &tv)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to get message (select)", winfo[id].id_string);
                        perr(error_message);
                        return false;
                }

                if (ret) {
                        if (gbn_receive(winfo[id].socket, &recv_header, NULL, &winfo[id].client_sockaddr) == -1) {
                                snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to get message (gbn_receive)", winfo[id].id_string);
                                perr(error_message);
                                return false;
                        }

                        set_status_safe(&winfo[id].status, CONNECTED, &winfo[id].mutex);
                }
        }
        
        return true;
}

/*
 * funzione:	p_send_ack
 * 
 * descrizione:	Funzione responsabile dell'invio di un riscontro nel caso di richieste di tipo PUT
 *
 * parametri:   id (long):              Indice del worker per ottenere le informazioni necessarie
 *                                      dall'array globale winfo
 *              seq_num (unsigned int): Numero di sequenza da riscontrare
 *              is_last (bool):         true    se il segmento e' da marcare con il flag LAST
 *                                      false   altrimenti
 *
 * return:      Numero di byte inviati (ssize_t)
 *
 */
ssize_t p_send_ack(long id, unsigned int seq_num, bool is_last)
{
        gbn_ftp_header_t header;
        ssize_t wsize;
        char error_message[ERR_SIZE];

        set_sequence_number(&header, seq_num);
        set_last(&header, is_last);
        set_ack(&header, true);
        set_err(&header, false);
        set_message_type(&header, PUT);

        if ((wsize = gbn_send(winfo[id].socket, header, NULL, 0, &winfo[id].client_sockaddr)) == -1) {
                memset(error_message, 0x0, ERR_SIZE);
                snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to send ACK %d", winfo[id].id_string, seq_num);
                perr(error_message);
                return -1;
        }

        #ifdef DEBUG
        printf("{DEBUG} %s ACK no. %d %ssent\n", winfo[id].id_string, seq_num, (wsize == 0) ? "not " : "");
        #endif
        
        return wsize;
}

/*
 * funzione:	update_tmp_ls_file
 * 
 * descrizione:	Funzione responsabile dell'aggiornamento del file tmp-ls a seguito dell'inserimento di un nuovo file
 *
 * parametri:   id (long):      Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti
 *
 */
bool update_tmp_ls_file(int id)
{
        FILE *tmp_ls_file;
        char path[PATH_SIZE];
        char error_message[ERR_SIZE];

        memset(path, 0x0, PATH_SIZE);
        memset(error_message, 0x0, ERR_SIZE);

        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/.tmp-ls", getenv("USER"));

        if ((tmp_ls_file = fopen(path, "a")) == NULL) {
                snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to open tmp-ls file", winfo[id].id_string);
                perr(error_message);
                return false;
        }

        if (pthread_rwlock_wrlock(&tmp_ls_rwlock)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for tmp-ls file broken", winfo[id].id_string);
                perr(error_message);
                return false;
        }

        fprintf(tmp_ls_file, "%s\n", winfo[id].filename);

        if (pthread_rwlock_unlock(&tmp_ls_rwlock)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for tmp-ls file broken", winfo[id].id_string);
                perr(error_message);
                return false;
        }

        #ifdef DEBUG
        printf("{DEBUG} %s Updated tmp-ls file\n", winfo[id].id_string);
        #endif

        fclose(tmp_ls_file);
        
        return true;
}

/*
 * funzione:	handle_ack_messages
 * 
 * descrizione:	Funzione responsabile della gestione dell'invio degli ACK nel caso di richiesta PUT (status = CONNECTED)
 *
 * parametri:   id (long):      Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
bool handle_ack_messages(long id)
{       
        char payload[CHUNK_SIZE];
        char error_message[ERR_SIZE];
        fd_set read_fds, all_fds;
        int retval;
        struct timeval tv;
        gbn_ftp_header_t recv_header;
        ssize_t recv_size;
        ssize_t header_size = sizeof(gbn_ftp_header_t);

        FD_ZERO(&all_fds);
        FD_SET(winfo[id].socket, &all_fds);

        memset(error_message, 0x0, ERR_SIZE);

        while (get_status_safe(&winfo[id].status, &winfo[id].mutex) == CONNECTED) {

                read_fds = all_fds;
                tv.tv_sec = MAX_TO_SEC;
                tv.tv_usec =  0;
                
                if ((retval = select(winfo[id].socket + 1, &read_fds, NULL, NULL, &tv)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to receive message (select)", winfo[id].id_string);
                        perr(error_message);
                        return false;                        
                } else if (retval) {

                        memset(payload, 0x0, CHUNK_SIZE);

                        if((recv_size = gbn_receive(winfo[id].socket, &recv_header, payload, &winfo[id].client_sockaddr)) == -1) {
                                snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to receive message (gbn_receive)", winfo[id].id_string);
                                perr(error_message);
                                return false;
                        }

                        #ifdef DEBUG
                        printf("{DEBUG} %s Received chunk no. %d (DIM. %ld)\n", winfo[id].id_string, get_sequence_number(recv_header), recv_size - header_size);
                        #endif

                        if(get_sequence_number(recv_header) == winfo[id].expected_seq_num) {
                                winfo[id].last_acked_seq_num = winfo[id].expected_seq_num;

                                if (write(winfo[id].fd, payload, recv_size - header_size) == -1) {
                                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to save chunk received on file", winfo[id].id_string);
                                        perr(error_message);
                                        return false;  
                                }

                                if (p_send_ack(id, winfo[id].expected_seq_num++, is_last(recv_header)) == -1)
                                        return false;

                                if (is_last(recv_header)) {
                                        printf("{INFO} %s File received succesfully\n", winfo[id].id_string);

                                        for (int i = 0; i < LAST_MESSAGE_LOOP - 1; ++i)                                
                                                if (p_send_ack(id, winfo[id].last_acked_seq_num, true) == -1)
                                                        return false;

                                        if (!update_tmp_ls_file(id))
                                                return false;

                                        winfo[id].to_be_deleted = false;

                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                                }

                        } else {
                                if (p_send_ack(id, winfo[id].last_acked_seq_num, false) == -1)
                                        return false;
                        } 

                } else {
                        set_status_safe(&winfo[id].status, TIMEOUT, &winfo[id].mutex);
                }
        } 

        return true;
}

/*
 * funzione:	set_id_string
 * 
 * descrizione:	Funzione responsabile del impostazione della stringa identificativa
 *
 * parametri:   id (long):      Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo      
 *
 */
void set_id_string(long id)
{
        char modality_str[5];

        memset(modality_str, 0x0, 5);

        switch(winfo[id].modality) {
                case LIST: snprintf(modality_str, 5, "LIST"); break;
                case PUT: snprintf(modality_str, 5, "PUT"); break;
                case GET: snprintf(modality_str, 5, "GET"); break;
                default: snprintf(modality_str, 5, "ZERO"); break;
        }
        
        snprintf(winfo[id].id_string, ID_STR_LENGTH, "[Worker no. %ld - Client connected: %s:%d (OP: %s)]", 
                id, 
                inet_ntoa((winfo[id].client_sockaddr).sin_addr), 
                ntohs((winfo[id].client_sockaddr).sin_port),
                modality_str);
}

/*
 * funzione:	receiver_routine
 * 
 * descrizione:	Entry point del thread spawnato per la ricezione dei chunk dal client in caso di operazione PUT
 *
 * parametri:   args (void *):  Indice associato al thread spawnato per accedere 
 *                              all'array globale winfo (tipo effettivo: long)
 *
 * return:      Valore d'uscita del thread (NULL)	    
 *
 */
void *receiver_routine(void *args) 
{
        long id = (long) args;
        char err_mess[ERR_SIZE];
        char path[PATH_SIZE];

        set_id_string(id);

        memset(err_mess, 0x0, ERR_SIZE);
        memset(path, 0x0, PATH_SIZE);

        printf("{INFO} %s is running right now\n", winfo[id].id_string);

        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/%s", getenv("USER"), winfo[id].filename);

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Unable to set sigmask for worker thread", winfo[id].id_string);
                perr(err_mess);
                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                pthread_exit(NULL); 
        }

        do  {
                switch (winfo[id].status) {

                        case REQUEST:
                                if (!p_send_new_port_mess(id))
                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
  
                                break;

                        case CONNECTED:
                                if (!handle_ack_messages(id)) 
                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                                
                                break;

                        case TIMEOUT:
                                printf("{INFO} %s Failed to receive file\n", winfo[id].id_string);
                                #ifdef DEBUG
                                printf("{DEBUG} %s Maximum wait time expires. Connection aborted!\n", winfo[id].id_string);
                                #endif
                                remove(path);
                                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                                break;

                        default: 
                                break;
                }

        } while (get_status_safe(&winfo[id].status, &winfo[id].mutex) != QUIT);

        printf("{INFO} %s is quitting right now\n", winfo[id].id_string);

        pthread_exit(NULL);
}

/*
 * funzione:	sender_routine
 * 
 * descrizione:	Entry point del thread spawnato per la l'invio dei chunk al client in caso di operazione LIST e GET
 *
 * parametri:   args (void *):  Indice associato al thread spawnato per accedere 
 *                              all'array globale winfo (tipo effettivo: long)
 *
 * return:      Valore d'uscita del thread (NULL)	    
 *
 */
void *sender_routine(void *args) 
{
        long id = (long) args;
        int ret;
        struct timeval tv;
        struct timespec ts;
        char err_mess[ERR_SIZE];
        unsigned short timeout_counter = 0;
        unsigned int last_base_for_timeout = 0;

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Unable to set sigmask for worker thread", winfo[id].id_string);
                perr(err_mess);
                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                pthread_exit(NULL); 
        }

        memset(err_mess, 0x0, ERR_SIZE);
        set_id_string(id);

        printf("{INFO} %s is running right now\n", winfo[id].id_string);

        while (get_status_safe(&winfo[id].status, &winfo[id].mutex) != QUIT) {

                switch (get_status_safe(&winfo[id].status, &winfo[id].mutex)) {

                        case REQUEST:
                                if (!lg_send_new_port_mess(id))
                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
  
                                break;

                        case CONNECTED:
                                if (can_send_more_segment_safe(&winfo[id].base, &winfo[id].next_seq_num, config->N, &winfo[id].mutex)) {
                                        if (send_file_chunk(id) == -1)
                                                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
  
                                        if (get_status_safe(&winfo[id].status, &winfo[id].mutex) != QUIT) {
                                                gettimeofday(&tv, NULL);

                                                if (config->is_adaptive) {
                                                        if (elapsed_usec(&winfo[id].start_timer, &tv) >= get_adaptive_rto_safe(&adapt[id], &winfo[id].mutex))
                                                                set_status_safe(&winfo[id].status, TIMEOUT, &winfo[id].mutex);
                                                } else {
                                                        if (elapsed_usec(&winfo[id].start_timer, &tv) >= config->rto_usec)
                                                                set_status_safe(&winfo[id].status, TIMEOUT, &winfo[id].mutex);
                                                }
                                        }                                
                                } else {
                                        if (pthread_mutex_lock(&winfo[id].cond_mutex)) {
                                                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_cond_mutex)", winfo[id].id_string);
                                                perr(err_mess);
                                                return false;
                                        }

                                        while (!can_send_more_segment_safe(&winfo[id].base, &winfo[id].next_seq_num, config->N, &winfo[id].mutex)) {
                                                if (config->is_adaptive) {
                                                        ts.tv_sec = winfo[id].start_timer.tv_sec + floor((double) (get_adaptive_rto_safe(&adapt[id], &winfo[id].mutex) + winfo[id].start_timer.tv_usec) / (double) 1000000);
                                                        ts.tv_nsec = ((winfo[id].start_timer.tv_usec + get_adaptive_rto_safe(&adapt[id], &winfo[id].mutex)) % 1000000) * 1000;
                                                } else {
                                                        ts.tv_sec = winfo[id].start_timer.tv_sec + floor((double) (config->rto_usec + winfo[id].start_timer.tv_usec) / (double) 1000000);
                                                        ts.tv_nsec = ((winfo[id].start_timer.tv_usec + config->rto_usec) % 1000000) * 1000;                                                       
                                                }
                                                
                                                ret = pthread_cond_timedwait(&winfo[id].cond_var, &winfo[id].cond_mutex, &ts);

                                                if (ret > 0) {
                                                        if (ret != ETIMEDOUT) {
                                                                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_condvar)", winfo[id].id_string);
                                                                perr(err_mess);
                                                                return false;
                                                        } else {
                                                                if (get_status_safe(&winfo[id].status, &winfo[id].mutex) != QUIT)
                                                                        set_status_safe(&winfo[id].status, TIMEOUT, &winfo[id].mutex);
                                                                break;
                                                        }

                                                }
                                        }

                                        if (pthread_mutex_unlock(&winfo[id].cond_mutex)) {
                                                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_cond_mutex)", winfo[id].id_string);
                                                perr(err_mess);
                                                return false;
                                        }
                                }

                                break;

                        case TIMEOUT:

                                #ifdef DEBUG
                                if (config->is_adaptive) {
                                        printf("{DEBUG} %s Timeout event (%d) (%ld usec)\n", 
                                                winfo[id].id_string, 
                                                winfo[id].base, 
                                                adapt[id].estimatedRTT + 4 * adapt[id].devRTT);
                                } else {
                                        printf("{DEBUG} %s Timeout event (%d)\n", 
                                                winfo[id].id_string, 
                                                winfo[id].base);
                                }
                                #endif       

                                if (get_gbn_param_safe(&winfo[id].base, &winfo[id].mutex) == last_base_for_timeout) {

                                        if (timeout_counter < MAX_TO) {
                                                if (!handle_retransmit(id, timeout_counter))
                                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                                        } else  {
                                                printf("{INFO} %s Failed to serve request\n", winfo[id].id_string);
                                                #ifdef DEBUG
                                                printf("{DEBUG} %s Maximum number of timeout reached for %d, abort\n", winfo[id].id_string, winfo[id].base);
                                                #endif

                                                FD_CLR(winfo[id].socket, &all_fds);
                                                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);      
                                        }

                                        ++timeout_counter;     
                                } else {

                                        timeout_counter = 1;
                                        
                                        if (!handle_retransmit(id, timeout_counter))
                                                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);

                                        last_base_for_timeout = get_gbn_param_safe(&winfo[id].base, &winfo[id].mutex);
                                }

                                break;
                        
                        default: 
                                break;
                }

        }

        printf("{INFO} %s is quitting right now\n", winfo[id].id_string);

        #ifdef TEST
        printf("{TEST} %s FIRST ESTIMATED RTT = %ld usec\n", 
                winfo[id].id_string, 
                elapsed_usec(&winfo[id].start_tx, &winfo[id].first_ack));

        printf("{TEST} %s TIME TO SEND A FULL WINDOW (N = %d) = %ld usec\n",
                winfo[id].id_string,
                config->N, 
                elapsed_usec(&winfo[id].start_tx, &winfo[id].full_window));

        if (config->is_adaptive) {
                printf("{TEST} %s LAST ADAPTIVE RTO EVALUATED = %ld usec\n",
                        winfo[id].id_string,
                        winfo[id].last_rtt);
        }

        printf("{TEST} %s SEND TIME %ld usec\n", 
                winfo[id].id_string,
                elapsed_usec(&winfo[id].start_tx, &winfo[id].end_tx));
        #endif

        pthread_exit(NULL);
}

/*
 * funzione:	exit_server
 * 
 * descrizione:	Funzione responsabile della fase di chiusura del server
 *
 * parametri:   status (int):   Valore d'uscita del processo 
 *
 */
void exit_server(int status) 
{
        char path[PATH_SIZE];

        memset(path, 0x0, PATH_SIZE);

        if (winfo) {
                for (int i = 0; i < concurrenty_connections; ++i) {
                        if (get_status_safe(&winfo[i].status, &winfo[i].mutex) != FREE)
                                set_status_safe(&winfo[i].status, QUIT, &winfo[i].mutex);
                }

                for (int i = 0; i < concurrenty_connections; ++i) {
                        if (get_status_safe(&winfo[i].status, &winfo[i].mutex) != FREE) {                               
                                if (winfo[i].tid) {
                                        pthread_join(winfo[i].tid, NULL);

                                        if (winfo[i].modality == PUT && winfo[i].to_be_deleted) {
                                                snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/%s", getenv("USER"), winfo[i].filename);
                                                remove(path);
                                        }
                                }
                        }

                        reset_worker_info(i, true, false);

                        #ifdef DEBUG
                        printf("{DEBUG} [Main thread] Disposed resources used by %d-th worker that's terminated\n", i);
                        #endif
                }
        }

        if (winfo)
                free(winfo);

        if (config)
                if (config->is_adaptive)
                        if (adapt)
                                free(adapt);

        if (config)
                free(config);

        pthread_rwlock_destroy(&tmp_ls_rwlock);

        if (modality == STANDARD)
                printf("\n\nServer is shutting down ...\n\n");
 
        exit(status);
}

/*
 * funzione:	parse_cmd
 * 
 * descrizione:	Funzione responsabile del parsing della command line
 *
 * parametri:   argc (int):             Numero di parametri immessi da linea di comando 
 *              argv (char **):         Vettore delle stringhe immesse da linea di comando 
 *
 * return:      Modalita' d'utilizzo dell'applicazione scelta (enum app_usages)    
 *
 */
enum app_usages parse_cmd(int argc, char **argv)
{
        verbose = true;
        int opt;

        struct option long_options[] = {
                {"port",        required_argument,      0, 'p'},
                {"wndsize",     required_argument,      0, 'N'},
                {"rtousec",     required_argument,      0, 't'},
                {"fixed",       no_argument,            0, 'f'},
                {"tpsize",      required_argument,      0, 's'},
                {"help",        no_argument,            0, 'h'},
                {"version",     no_argument,            0, 'v'},
                {"verbose",     no_argument,            0, 'V'},
                {0,             0,                      0, 0}
        };

        while ((opt = getopt_long(argc, argv, "p:N:t:fs:hvV", long_options, NULL)) != -1) {
                switch (opt) {
                        case 'p':
                                acceptance_port = strtol(optarg, NULL, 10);
                                if (!is_valid_port(acceptance_port))
                                        return ERROR;
                                break;
                        case 'N':
                                config->N = strtol(optarg, NULL, 10);
                                if (!(config->N < MAX_SEQ_NUMBER && config->N > 0))
                                        return ERROR;
                                break;
                        case 't':
                                config->rto_usec = strtol(optarg, NULL, 10);
                                if (config->rto_usec <= 0)
                                        return ERROR;
                                break;
                        case 'f':
                                config->is_adaptive = false;
                                break;
                        case 'h':
                                return (argc != 2) ? ERROR : HELP;
                        case 's':
                                concurrenty_connections = strtol(optarg, NULL, 10);
                                if (concurrenty_connections < 1 && concurrenty_connections > MAX_PORT_NUM - START_WORKER_PORT)
                                        return ERROR;
                                break;
                        case 'V':
                                verbose = true;
                                break;
                        case 'v':
                                return (argc != 2) ? ERROR : VERSION;
                        default:
                                return ERROR;
                }
        }

        return STANDARD;
}

/*
 * funzione:	init_socket
 * 
 * descrizione:	Funzione responsabile dell'apertura di un nuovo descrittore associato ad una socket
 *              e del bind di tale descrittore alla porta
 *
 * parametri:   port (unsigned short int):      Numero di porta per la bind
 *
 * return:      Descrittore inizializzato (int)
 *
 */
int init_socket(unsigned short int port)
{
        int fd;
        struct sockaddr_in addr;
        int enable = 1;

        memset(&addr, 0x0, sizeof(addr));

        if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perr("{ERROR} [Main Thread] Unable to get socket file descriptor");
                return -1;
        }

        if (port != acceptance_port) {
                if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) == -1) {
                        perr("{ERROR} [Main Thread] Unable to set option on socket (REUSEADDR)");
                        return -1;
                }
        }

        addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
        
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
                perr("{ERROR} [Main Thread] Unable to bind address to socket");
                return -1;
        }

        return fd;
}

/*
 * funzione:	get_available_worker
 * 
 * descrizione:	Funzione che restituisce l'indice di un worker libero per gestire una richiesta client
 *              verificando che non vi sia già un servente associato alla medesima richiesta
 *
 * parametri:   addr (const struct sockaddr_in *):      Struttura contenente le informazioni del client richiedente
 *              already_handled_ptr (bool *):           Flag booleano da settare a true se la richiesta è già servita
 *
 * return:      Indice trovato (long)
 *
 */
long get_available_worker(const struct sockaddr_in *addr, bool *already_handled_ptr)
{
        long ret, i;

        i = 0;
        *already_handled_ptr = false;

        while (i < concurrenty_connections) {
                if (memcmp(&winfo[i].client_sockaddr, addr, sizeof(struct sockaddr_in)) == 0) {
                        *already_handled_ptr = true;
                        return -1;
                }

                ++i;
        }

        i = 0;
        ret = -1;

        while (i < concurrenty_connections) {
                if (get_status_safe(&winfo[i].status, &winfo[i].mutex) == FREE) {
                        winfo[i].status = REQUEST;
                        ret = i;
                        break;
                }

                ++i;
        }

        return ret;
}

/*
 * funzione:	start_sender
 * 
 * descrizione:	Funzione responsabile della fase di startup del thread sender
 *
 * parametri:   index (long):                           Indice con cui accedere all'array winfo
 *              client_sockaddr (struct sockaddr_in *): Puntatore alla struttura contenente le info del client
 *              modality (enum message_type):           Tipo di richiesta effettuata      
 *              payload (const char *):                 Payload presente del messaggio di REQUEST ricevuto
 *              can_open_ptr (bool *):                  Flag booleano che viene settato a false se il file non esiste 
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti  
 *
 */
bool start_sender(long index, struct sockaddr_in *client_sockaddr, enum message_type modality, const char *payload, bool *can_open_ptr)
{
        char path[PATH_SIZE];
        char error_message[ERR_SIZE];

        memset(path, 0x0, PATH_SIZE);
        memset(error_message, 0x0, ERR_SIZE);

        memcpy(&winfo[index].client_sockaddr, client_sockaddr, sizeof(struct sockaddr_in));

        winfo[index].modality = modality;
        strncpy(winfo[index].filename, payload, CHUNK_SIZE);

        if (winfo[index].modality == LIST) {
                snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/.tmp-ls", getenv("USER"));
                if ((winfo[index].fd = open(path, O_RDONLY)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to open LS tmp file for %ld-th connection", index);
                        perr(error_message);
                        return false;    
                }
        }

        if (winfo[index].modality == GET) {
                snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/%s", getenv("USER"), winfo[index].filename);
                if ((winfo[index].fd = open(path, O_RDONLY)) == -1) {
                        if (errno == ENOENT) {
                                *can_open_ptr = false;
                                return false;
                        }

                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to retrive requested file for %ld-th connection", index);
                        perr(error_message);
                        return false;
                }
        }
        
        winfo[index].number_of_chunks = ceil((double) lseek(winfo[index].fd, 0, SEEK_END) / (double) CHUNK_SIZE);
        lseek(winfo[index].fd, 0, SEEK_SET);

        if ((winfo[index].socket = init_socket(winfo[index].port)) == -1) 
                return false;

        if (pthread_create(&winfo[index].tid, NULL, sender_routine, (void *) index)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to spawn sender thread for %ld-th client", index);
                perr(error_message);
                return false;  
        }

        return true;
}

/*
 * funzione:	start_receiver
 * 
 * descrizione:	Funzione responsabile della fase di startup del thread receiver
 *
 * parametri:   index (long):                           Indice con cui accedere all'array winfo
 *              client_sockaddr (struct sockaddr_in *): Puntatore alla struttura contenente le info del client
 *              payload (const char *):                 Payload presente del messaggio di REQUEST ricevuto
 *              can_open_ptr (bool *):                  Flag booleano che viene settato a false se il file è già esistente 
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti  
 *
 */
bool start_receiver(long index, struct sockaddr_in *client_sockaddr, const char *payload, bool *can_open_ptr)
{
        char path[PATH_SIZE];
        char error_message[ERR_SIZE];

        memset(path, 0x0, PATH_SIZE);
        memset(error_message, 0x0, ERR_SIZE);

        memcpy(&winfo[index].client_sockaddr, client_sockaddr, sizeof(struct sockaddr_in));
        winfo[index].modality = PUT;
        strncpy(winfo[index].filename, payload, CHUNK_SIZE);

        snprintf(path, PATH_SIZE, "/home/%s/.gbn-ftp-public/%s", getenv("USER"), winfo[index].filename);

        if ((winfo[index].fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)) == -1) {
                if (errno == EEXIST) {
                        *can_open_ptr = false;
                        return false;
                }

                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to retrive requested file for %ld-th connection", index);
                perr(error_message);
                return false;
        }

        if ((winfo[index].socket = init_socket(winfo[index].port)) == -1) 
                return false;

        if (pthread_create(&winfo[index].tid, NULL, receiver_routine, (void *) index)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to spawn receiver thread for %ld-th client", index);
                perr(error_message);
                return false;  
        }

        return true;
}

/*
 * funzione:	reset_worker_info
 * 
 * descrizione:	Funzione responsabile dell'inizializzazione della struttura i-esima worker_info
 *
 * parametri:   id (int):               Indice con cui accedere all'array winfo
 *              need_destroy (bool):    true    se è necessaria la dispose della struttura
 *                                      false   altrimenti
 *              need_create (bool):     true    se la struttura va inizializzata
 *                                      false   altrimenti
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti  
 *
 */
bool reset_worker_info(int id, bool need_destroy, bool need_create)
{
        char error_message[ERR_SIZE];

        memset(error_message, 0x0, ERR_SIZE);

        if (need_destroy) {

                if (pthread_mutex_destroy(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to free metadata used by syncronization protocol for %d-th connection (worker_mutex)", id);
                        perr(error_message);
                        return false;
                }

                if (pthread_mutex_destroy(&winfo[id].cond_mutex)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to free metadata used by syncronization protocol for %d-th connection (worker_mutex)", id);
                        perr(error_message);
                        return false;
                }
                
                if (pthread_cond_destroy(&winfo[id].cond_var)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to free metadata used by syncronization protocol for %d-th connection (worker_cond_var)", id);
                        perr(error_message);
                        return false;
                }

                if (winfo[id].socket != -1)
                         close(winfo[id].socket);
        }

        if (need_create) {
                if (pthread_mutex_init(&winfo[id].mutex, NULL)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to init metadata used by syncronization protocol for %d-th connection (worker_mutex)", id);
                        perr(error_message);
                        return false;
                }

                if (pthread_mutex_init(&winfo[id].cond_mutex, NULL)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to free metadata used by syncronization protocol for %d-th connection (worker_mutex)", id);
                        perr(error_message);
                        return false;
                }

                if (pthread_cond_init(&winfo[id].cond_var, NULL)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to init metadata used by syncronization protocol for %d-th connection (worker_cond_var)", id);
                        perr(error_message);
                        return false;
                }

                winfo[id].socket = -1;
                winfo[id].base = 1;
                winfo[id].next_seq_num = 1;
                winfo[id].expected_seq_num = 1;
                winfo[id].last_acked_seq_num = 0;
                winfo[id].modality = ZERO;
                winfo[id].status = FREE;
                winfo[id].to_be_deleted = true;

                if (config->is_adaptive) {
                        adapt[id].seq_num = 1;
                        adapt[id].restart = true;
                        adapt[id].sampleRTT = config->rto_usec;
                        adapt[id].estimatedRTT = config->rto_usec;
                        adapt[id].devRTT = 0;
                }
        }

        return true;
}

/*
 * funzione:	init_worker_info
 * 
 * descrizione:	Funzione responsabile dell'inizializzazione dell'array winfo
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti  
 *
 */
bool init_worker_info(void) 
{
        if((winfo = calloc(concurrenty_connections, sizeof(struct worker_info))) == NULL) {
                perr("{ERROR} [Main Thread] Unable to allocate metadata for worker threads");
                return false;
        }

        if (config->is_adaptive) {
                if((adapt = calloc(concurrenty_connections, sizeof(struct gbn_adaptive_timeout))) == NULL) {
                        perr("{ERROR} [Main Thread] Unable to allocate metadata for worker threads");
                        return false;
                }
        }

        for (int i = 0; i < concurrenty_connections; ++i) {
                memset(&winfo[i], 0x0, sizeof(struct worker_info));

                if (config->is_adaptive)
                        memset(&adapt[i], 0x0, sizeof(struct gbn_adaptive_timeout));

                winfo[i].port = START_WORKER_PORT + i;
                
                if (!reset_worker_info(i, false, true)) {
                        free(winfo);
                        return false;
                }            
        }

        return true;
}

/*
 * funzione:	handle_recv
 * 
 * descrizione:	Funzione responsabile dell'avanzamento della finestraa a fronte della ricezione 
 *              di riscontri da parte del client      
 *
 * parametri:   id (int):       Indice del worker per ottenere le informazioni necessarie
 *                              dall'array globale winfo
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti      
 *
 */
bool handle_recv(int id) 
{
        gbn_ftp_header_t recv_header;
        char error_message[ERR_SIZE];
        struct timeval tv;

        memset(error_message, 0x0, ERR_SIZE);

        if (gbn_receive(winfo[id].socket, &recv_header, NULL, &winfo[id].client_sockaddr) == -1) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to get message from %d-th client (gbn_receive)", id);
                perr(error_message);
                return false;
        }

        if(!is_ack(recv_header)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Comunication protocol broken by %d-th client", id);
                perr(error_message);
                return false; 
        }

        #ifdef DEBUG
        printf("{DEBUG} %s ACK no. %d received\n", winfo[id].id_string, get_sequence_number(recv_header));
        #endif

        #ifdef TEST
        if (get_sequence_number(recv_header) == 1)
                gettimeofday(&winfo[id].first_ack, NULL);
        #endif

        if (pthread_cond_signal(&winfo[id].cond_var)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_condvar_%d)", id);
                perr(error_message);
                return false;
        }

        if (get_sequence_number(recv_header) == 0) {
                set_status_safe(&winfo[id].status, CONNECTED, &winfo[id].mutex);
                return true;
        }

        if (is_last(recv_header)) {

                #ifdef TEST
                gettimeofday(&winfo[id].end_tx, NULL);
                winfo[id].last_rtt = (config->is_adaptive) ? adapt[id].estimatedRTT + (4 * adapt[id].devRTT) : config->rto_usec;
                #endif 

                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                
                FD_CLR(winfo[id].socket, &all_fds);

                printf("{INFO} [Main Thread] Request from %d-th client served succesfully\n", id);

                #ifdef DEBUG
                printf("{DEBUG} [Main Thread] Comunication with %d-th client has expired\n", id);
                #endif

                return true;
        }

        if (pthread_mutex_lock(&winfo[id].mutex)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_mutex_%d)", id);
                perr(error_message);
                return false;
        }

        if (config->is_adaptive && winfo[id].status == CONNECTED) {
                if (adapt[id].seq_num <= get_sequence_number(recv_header)) {
                        gettimeofday(&tv, NULL);
                        adapt[id].sampleRTT = elapsed_usec(&adapt[id].saved_tv, &tv);
                        adapt[id].estimatedRTT = ((1 - ALPHA) * adapt[id].estimatedRTT) + (ALPHA * adapt[id].sampleRTT);
                        adapt[id].devRTT = ((1 - BETA) * adapt[id].devRTT) + (BETA * ABS(adapt[id].sampleRTT - adapt[id].estimatedRTT));
                        adapt[id].restart = true;

                        #ifdef DEBUG
                        printf("{DEBUG} [Main Thread] Updated value of rto for %d-th connection: %ld usec\n", id, (adapt[id].estimatedRTT + 4 * adapt[id].devRTT));
                        #endif
                }
        }

        winfo[id].base = get_sequence_number(recv_header) + 1;

        if (winfo[id].base == winfo[id].next_seq_num)
                memset(&winfo[id].start_timer, 0x0, sizeof(struct timeval));
        else    
                gettimeofday(&winfo[id].start_timer, NULL);

        if (pthread_mutex_unlock(&winfo[id].mutex)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_mutex_%d)", id);
                perr(error_message);
                return false;
        }

        return true;
}

/*
 * funzione:	send_error_message
 * 
 * descrizione:	Funzione responsabile dell'invio di un segmento di errore a fronte di una richiesta errata
 * 
 * parametri:   id (int):               Indice del worker per ottenere le informazioni necessarie
 *                                      dall'array globale winfo
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti 
 *
 */
bool send_error_message(int id)
{
        ssize_t wsize = 0;
        gbn_ftp_header_t header;
        char error_message[ERR_SIZE];

        memset(error_message, 0x0, ERR_SIZE);

        set_sequence_number(&header, 0);
        set_message_type(&header, winfo[id].modality);
        set_last(&header, true);
        set_ack(&header, false);
        set_err(&header, true);

        for (int i = 0; i < LAST_MESSAGE_LOOP; ++i) {
                if ((wsize += gbn_send(acceptance_sockfd, header, NULL, 0, &winfo[id].client_sockaddr)) == -1) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to send error message to %d-th client", id);
                        perr(error_message);
                        return false;
                }
        }     

        #ifdef DEBUG
        printf("{DEBUG} [Main Thread] Error message %ssent to %d-th client\n", (wsize == 0) ? "not " : "", id);
        #endif
        
        return true;
}

/*
 * funzione:	dispose_leaked_resources
 * 
 * descrizione:	Funzione responsabile della fase di dispose dei metadati associati a connessioni terminate
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti 
 *
 */
bool dispose_leaked_resources(void)
{
        for (int i = 0; i < concurrenty_connections; ++i) {
                
                if (get_status_safe(&winfo[i].status, &winfo[i].mutex) == QUIT) {
                        pthread_join(winfo[i].tid, NULL);

                        if (!reset_worker_info(i, true, true))
                                return false;

                        #ifdef DEBUG
                        printf("{DEBUG} [Main thread] Disposed resources used by %d-th worker that's terminated\n", i);
                        #endif
                }
        }

        return true;
}

/*
 * funzione:	acceptance_loop
 * 
 * descrizione:	Funzione cuore del server, responsabile della fase di ricezione per:
 *                      - nuove connessioni
 *                      - richieste GET
 *                       - richieste LIST
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti 
 *
 */
bool acceptance_loop(void)
{
        int winfo_index, ready_fds, maxfd;
        struct sockaddr_in addr;
        gbn_ftp_header_t header;
        char payload[CHUNK_SIZE];
        fd_set read_fds;
        bool already_handled;
        bool can_open;

        FD_ZERO(&all_fds);
        FD_SET(acceptance_sockfd, &all_fds);
        maxfd = acceptance_sockfd;

        while(true) {
                read_fds = all_fds;
                memset(&addr, 0x0, sizeof(struct sockaddr_in));
                memset(payload, 0x0, CHUNK_SIZE);

                if ((ready_fds = select(maxfd + 1, &read_fds, NULL, NULL, NULL)) == -1) {
                        perr("{ERROR} [Main Thread] Unable to receive command from clients (select)");
                        return false;
                }

                if (FD_ISSET(acceptance_sockfd, &read_fds)) {

                        if (gbn_receive(acceptance_sockfd, &header, payload, &addr) == -1) {
                                perr("{ERROR} [Main Thread] Unable to receive command from clients (gbn_receive)");
                                return false;
                        }

                        #ifdef DEBUG
                        printf("{DEBUG} [Main Thread] Received request from %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                        #endif

                        if (!(is_last(header) && !is_ack(header) && (get_sequence_number(header) == 0))) {

                                #ifdef DEBUG
                                printf("{DEBUG} [Main Thread] Connection protocol broken: request rejected\n");
                                #endif

                                continue;
                        }
                        
                        if ((winfo_index = get_available_worker(&addr, &already_handled)) == -1) {

                                if (already_handled) {
                                        #ifdef DEBUG
                                        printf("{DEBUG} [Main Thread] Reject request from %s:%d (already handled)\n", 
                                                inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                                        #endif
                                } else {
                                        #ifdef DEBUG
                                        printf("{DEBUG} [Main Thread] All workers are busy. Can't handle request\n");
                                        #endif
                                }

                                continue;
                        }

                        #ifdef DEBUG
                        printf("{DEBUG} [Main Thread] Accept request from %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
                        #endif

                        can_open = true;
                        if (get_message_type(header) != PUT) {

                                if (!start_sender(winfo_index, &addr, get_message_type(header), payload, &can_open)) {
                                        if (!can_open) {
                                                if (!send_error_message(winfo_index))
                                                        return false; 

                                                reset_worker_info(winfo_index, true, true);
                                        } else {   
                                                return false;
                                        }
                                } else {
                                        FD_SET(winfo[winfo_index].socket, &all_fds);

                                        if (winfo[winfo_index].socket > maxfd) 
                                                maxfd = winfo[winfo_index].socket;
                                }

                        } else {

                                if (!start_receiver(winfo_index, &addr, payload, &can_open)) {
                                        if (!can_open) {
                                                if (!send_error_message(winfo_index))
                                                        return false; 

                                                reset_worker_info(winfo_index, true, true);
                                        } else {   
                                                return false;
                                        }
                                }

                        }
                }  

                for (int i = 0; (i < concurrenty_connections) && (ready_fds > 0); ++i) {

                        if (winfo[i].socket != -1) {
                                if (FD_ISSET(winfo[i].socket, &read_fds)) {
                                        if (!handle_recv(i))
                                                return false;

                                        --ready_fds;
                                }
                        }
                }

                if (!dispose_leaked_resources())
                        return false;
        }

        return true;
}

/*
 * funzione:	check_installation
 * 
 * descrizione:	Funzione responsabile della verifica della corretta installazione del server
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti       
 *
 */
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
        char choice;

        #ifdef DEBUG
        printf("*** DEBUG MODE ***\n\n\n");
        #endif

        #ifdef TEST
        printf("*** TEST MODE ***\n\n\n");
        #endif

        if (!check_installation()) {
                fprintf(stderr, "Server not installed yet!\n\nPlease run the following command:\n\tsh /path/to/script/install-server.sh\n");
                exit_server(EXIT_FAILURE);
        }

        if (!setup_signals(&t_set, sig_handler))
                exit_server(EXIT_FAILURE);

        if (pthread_rwlock_init(&tmp_ls_rwlock, NULL)) {
                perr("{ERROR [Main Thread] Unable to initialize syncronization protocol for tmp-ls file");
                exit_server(EXIT_FAILURE);
        }

        #ifndef TEST
        srand(time(0));
        #endif

        concurrenty_connections = sysconf(_SC_NPROCESSORS_ONLN) << 2;

        if((config = init_configurations()) == NULL) {
                perr("{ERROR} [Main Thread] Unable to load default configurations for server");
                exit_server(EXIT_FAILURE); 
        }  

        acceptance_port = DEFAULT_PORT;
        modality = parse_cmd(argc, argv);

        switch(modality) {
                case STANDARD: 
                        if ((choice = multi_choice("Do you want to see current settings profile?", "yn", 2)) == 'Y') {
                                printf("\nList of current settings for server:\n\n");
                                printf("N..........: %u\n", config->N);
                                printf("rcvtimeout.: %lu usec\n", config->rto_usec);
                                printf("port.......: %u\n", acceptance_port);
                                printf("adapitve...: %s\n", (config->is_adaptive) ? "true" : "false");
                                printf("probability: %f\n", PROBABILITY);
                                printf("pool size..: %ld\n\n", concurrenty_connections);
                        }
 
                        break;
                case HELP:
                        printf("\n\tusage: gbn-ftp-server [options]\n");
                        printf("\n\tList of available options:\n");
                        printf("\t\t-p [--port]\t<port>\t\tServer port\n");
                        printf("\t\t-N [--wndsize]\t<size>\t\tWindow size (for GBN)\n");
                        printf("\t\t-t [--rtousec]\t<timeout>\tRetransmition timeout [usec] (for GBN)\n");
                        printf("\t\t-s [--tpsize]\t<size>\t\tMax number of concurrenty connections\n");
                        printf("\t\t-f [--fixed]\t\t\tTimer fixed\n");
                        printf("\t\t-v [--version]\t\t\tVersion of gbn-ftp-server\n");
                        printf("\t\t-V [--verbose]\t\t\tPrint verbose version of error\n\n");
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

        if (!init_worker_info())
                exit_server(EXIT_FAILURE);

        if ((acceptance_sockfd = init_socket(acceptance_port)) == -1) 
                exit_server(EXIT_FAILURE);

        printf("\nServer is now listening ... \n");
        fflush(stdout);

        if (!acceptance_loop())
                exit_server(EXIT_FAILURE);

        close(acceptance_sockfd);        
        exit_server(EXIT_SUCCESS);
}