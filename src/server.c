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

#include "gbnftp.h" 
#include "common.h"

#define ID_STR_LENGTH 64
#define CMD_SIZE 128
#define START_WORKER_PORT 49152
#define MAX_PORT_NUM 65535


struct worker_info {
        int socket;                                     /* socket dedicata per la comunicazione */
        char id_string[ID_STR_LENGTH];                  /* stringa identificativa del thread utile nelle stampe di debug */
        unsigned short port;                            /* porta associata al thread worker */
        volatile enum connection_status status;         /* stato della connessione {FREE, REQUEST, CONNECTED, TIMEOUT, QUIT} */
        struct sockaddr_in client_sockaddr;             /* struttura sockaddr_in del client utilizzata nelle funzioni di send e receive */
        pthread_mutex_t mutex;                          /* mutex utilizzato per la sincronizzazione fra main thread e servente i-esimo */
        pthread_mutex_t cond_mutex;
        pthread_cond_t cond_var;                        /* variabile condizionale utilizzata in un timed_wait nella fase di connessione */
        enum message_type modality;                     /* modalità scelta d'utilizzo */
        char filename[CHUNK_SIZE];                      /* nome del file su cui lavorare */
        volatile unsigned int base;                     /* variabile base del protocollo gbn associata alla connessione */
        volatile unsigned int next_seq_num;             /* variabile next_seq_num del protocollo gbn associata alla connessione */                 
        unsigned int expected_seq_num;                  /* variabile expected_seq_num del protocollo gbn associata alla connessione */
        unsigned int last_acked_seq_num;                /* variabile last_acked_seq_num del protocollo gbn associata alla connessione */
        struct timeval start_timer;                     /* struttura utilizzata per la gestione del timer */
        pthread_t tid;                                  /* ID del thread servent e*/
        int fd;                                         /* descrittore del file su cui si deve operare */ 
};


extern bool verbose;
extern char *optarg;
extern int opterr;


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


#ifdef DEBUG
void sig_handler(int signo); 
#else
void sig_handler(__attribute__((unused)) int signo);
#endif 
bool handle_retransmit(long id); 
ssize_t send_file_chunk(long id);
ssize_t lg_send_new_port_mess(long id);
ssize_t p_send_new_port_mess(long id);
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
bool send_error_message(int id, int acc_socket);
bool dispose_leaked_resources();
bool acceptance_loop(int acc_socket);
bool check_installation(void); 


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

bool is_valid_port(unsigned short int port)
{
        bool check_for_std_range = port > 0 && port < MAX_PORT_NUM;
        bool check_for_wrk_range = port < START_WORKER_PORT && port > START_WORKER_PORT + concurrenty_connections - 1; 

        return check_for_std_range && check_for_wrk_range; 
}

bool handle_retransmit(long id) 
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

        if (pthread_mutex_unlock(&winfo[id].mutex)) {
                perr(error_message);
                return false;
        }

        for (unsigned int i = base; i < next_seq_num; ++i)
                if (send_file_chunk(id) == -1)
                        return false;

        set_status_safe(&winfo[id].status, CONNECTED, &winfo[id].mutex);

        return true;
}

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
                set_sequence_number(&header, winfo[id].next_seq_num);
                set_ack(&header, false);      
                set_err(&header, false);  
                set_last(&header, (rsize < CHUNK_SIZE));  

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

                if (config->is_adaptive) {
                        if (adapt[id].restart) {
                                gettimeofday(&adapt[id].saved_tv, NULL);
                                adapt[id].seq_num = winfo[id].next_seq_num;
                                adapt[id].restart = false;
                        }
                }                      

                if (winfo[id].base == winfo[id].next_seq_num) 
                        gettimeofday(&winfo[id].start_timer, NULL);

                if (pthread_mutex_unlock(&winfo[id].mutex)) {
                        snprintf(error_message, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_mutex)", winfo[id].id_string);
                        perr(error_message);
                        return -1;
                }

                winfo[id].next_seq_num ++;

                return wsize; 
        }  

        return 0;    
}

ssize_t lg_send_new_port_mess(long id)
{
        gbn_ftp_header_t header;
        struct timespec ts;
        struct timeval tv;
        int ret;
        ssize_t wsize;
        char error_message[ERR_SIZE];
        char serialized_config[CHUNK_SIZE];

        memset(error_message, 0x0, ERR_SIZE);
        memset(serialized_config, 0x0, CHUNK_SIZE);

        set_err(&header, false);
        set_sequence_number(&header, 0);
        set_message_type(&header, winfo[id].modality);
        set_last(&header, false);
        set_ack(&header, false);

        serialize_configuration(config, serialized_config);        

        while (get_status_safe(&winfo[id].status, &winfo[id].mutex) == REQUEST) {

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

ssize_t p_send_new_port_mess(long id)
{
        gbn_ftp_header_t send_header, recv_header;
        int ret;
        ssize_t wsize;
        fd_set read_fds, all_fds;
        struct timeval tv;
        char error_message[ERR_SIZE];
        char serialized_config[CHUNK_SIZE];

        set_err(&send_header, false);
        set_sequence_number(&send_header, 0);
        set_message_type(&send_header, PUT);
        set_last(&send_header, false);
        set_ack(&send_header, false);

        FD_ZERO(&all_fds);
        FD_SET(winfo[id].socket, &all_fds);

        memset(error_message, 0x0, ERR_SIZE);
        memset(serialized_config, 0x0, CHUNK_SIZE);

        serialize_configuration(config, serialized_config);

        while (get_status_safe(&winfo[id].status, &winfo[id].mutex) == REQUEST) {

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
                        return -1;
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

        memset(error_message, 0x0, ERR_SIZE);

        if ((wsize = gbn_send(winfo[id].socket, header, NULL, 0, &winfo[id].client_sockaddr)) == -1) {
                snprintf(error_message, ERR_SIZE, "{ERROR} %s Unable to send ACK %d", winfo[id].id_string, seq_num);
                perr(error_message);
                return -1;
        }

        #ifdef DEBUG
        printf("{DEBUG} %s ACK no. %d %ssent\n", winfo[id].id_string, seq_num, (wsize == 0) ? "not " : "");
        #endif
        
        return wsize;
}

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

                                        for (int i = 0; i < LAST_MESSAGE_LOOP - 1; ++i)                                
                                                if (p_send_ack(id, winfo[id].last_acked_seq_num, true) == -1)
                                                        return false;

                                        if (!update_tmp_ls_file(id))
                                                return false;

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

void *receiver_routine(void *args) 
{
        long id = (long) args;
        char err_mess[ERR_SIZE];

        snprintf(winfo[id].id_string, ID_STR_LENGTH, "[Worker no. %ld - Client connected: %s:%d (OP: %d)]", 
                id, 
                inet_ntoa((winfo[id].client_sockaddr).sin_addr), 
                ntohs((winfo[id].client_sockaddr).sin_port),
                winfo[id].modality);

        memset(err_mess, 0x0, ERR_SIZE);

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Unable to set sigmask for worker thread", winfo[id].id_string);
                perr(err_mess);
                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                pthread_exit(NULL); 
        }

        do  {
                switch (winfo[id].status) {

                        case REQUEST:
                                if (p_send_new_port_mess(id) == -1)
                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
  
                                break;

                        case CONNECTED:
                                if (!handle_ack_messages(id)) 
                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                                
                                break;

                        case TIMEOUT:
                                #ifdef DEBUG
                                printf("{DEBUG} %s Maximum wait time expires. Connection aborted!\n", winfo[id].id_string);
                                #endif
                                
                                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                                break;

                        default: 
                                break;
                }

        } while (get_status_safe(&winfo[id].status, &winfo[id].mutex) != QUIT);

        #ifdef DEBUG
        printf("{DEBUG} %s is quitting right now\n", winfo[id].id_string);
        #endif

        pthread_exit(NULL);
}

void *sender_routine(void *args) 
{
        long id = (long) args;
        int ret;
        struct timeval tv;
        struct timespec ts;
        char err_mess[ERR_SIZE];
        unsigned short timeout_counter = 0;
        unsigned int last_base_for_timeout = get_gbn_param_safe(&winfo[id].base, &winfo[id].mutex);

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Unable to set sigmask for worker thread", winfo[id].id_string);
                perr(err_mess);
                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                pthread_exit(NULL); 
        }

        memset(err_mess, 0x0, ERR_SIZE);

        snprintf(winfo[id].id_string, ID_STR_LENGTH, "[Worker no. %ld - Client connected: %s:%d (OP: %d)]", 
                id, 
                inet_ntoa((winfo[id].client_sockaddr).sin_addr), 
                ntohs((winfo[id].client_sockaddr).sin_port),
                winfo[id].modality);

        while (get_status_safe(&winfo[id].status, &winfo[id].mutex) != QUIT) {

                switch (get_status_safe(&winfo[id].status, &winfo[id].mutex)) {

                        case REQUEST:
                                if (lg_send_new_port_mess(id) == -1)
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

                                        while (winfo[id].next_seq_num >= winfo[id].base + config->N) {
                                                ts.tv_sec = winfo[id].start_timer.tv_sec + floor((double) (config->rto_usec + winfo[id].start_timer.tv_usec) / (double) 1000000);
                                                ts.tv_nsec = ((winfo[id].start_timer.tv_usec + config->rto_usec) % 1000000) * 1000;
                                                
                                                ret = pthread_cond_timedwait(&winfo[id].cond_var, &winfo[id].cond_mutex, &ts);

                                                if (ret > 0) {
                                                        if (ret != ETIMEDOUT) {
                                                                snprintf(err_mess, ERR_SIZE, "{ERROR} %s Syncronization protocol for worker threads broken (worker_condvar)", winfo[id].id_string);
                                                                perr(err_mess);
                                                                return false;
                                                        } else {
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
                                printf("{DEBUG} %s Timeout event (%d)\n", winfo[id].id_string, winfo[id].base);
                                #endif       

                                if (get_gbn_param_safe(&winfo[id].base, &winfo[id].mutex) == last_base_for_timeout) {

                                        if (timeout_counter < MAX_TO) {
                                                if (!handle_retransmit(id))
                                                        set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);
                                        } else  {
                                                #ifdef DEBUG
                                                printf("{DEBUG} %s Maximum number of timeout reached for %d, abort\n", winfo[id].id_string, winfo[id].base);
                                                #endif

                                                FD_CLR(winfo[id].socket, &all_fds);
                                                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);      
                                        }
                                                
                                        ++timeout_counter;
                                } else {

                                        timeout_counter = 1;
                                        
                                        if (!handle_retransmit(id))
                                                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);

                                        last_base_for_timeout = get_gbn_param_safe(&winfo[id].base, &winfo[id].mutex);
                                }

                                break;
                        
                        default: 
                                break;
                }

        }

        #ifdef DEBUG
        printf("{DEBUG} %s is quitting right now\n", winfo[id].id_string);
        #endif

        pthread_exit(NULL);
}

void exit_server(int status) 
{
        if (winfo) {
                for (int i = 0; i < concurrenty_connections; ++i) {
                        if (get_status_safe(&winfo[i].status, &winfo[i].mutex) != FREE)
                                set_status_safe(&winfo[i].status, QUIT, &winfo[i].mutex);
                }

                for (int i = 0; i < concurrenty_connections; ++i) {
                        if (get_status_safe(&winfo[i].status, &winfo[i].mutex) != FREE) {                               
                                if (winfo[i].tid)
                                        pthread_join(winfo[i].tid, NULL);
                        }

                        reset_worker_info(i, true, false);

                        #ifdef DEBUG
                        printf("{DEBUG} [Main thread] Disposed resources used by %d-th worker that's terminated\n", i);
                        #endif
                }
        }

        if (winfo)
                free(winfo);

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
                                if (!(config->N < MAX_SEQ_NUMBER / 2 && config->N > 0))
                                        return ERROR;
                                break;
                        case 't':
                                config->rto_usec = strtol(optarg, NULL, 10);
                                if (config->rto_usec < 1)
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
                                fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                                abort();
                }
        }

        return STANDARD;
}

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

        if ((winfo[index].socket = init_socket(winfo[index].port)) == -1) 
                return false;

        if (pthread_create(&winfo[index].tid, NULL, sender_routine, (void *) index)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Unable to spawn sender thread for %ld-th client", index);
                perr(error_message);
                return false;  
        }

        return true;
}

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

        if (get_sequence_number(recv_header) == 0) {
                set_status_safe(&winfo[id].status, CONNECTED, &winfo[id].mutex);
                return true;
        }

        if (is_last(recv_header)) {

                if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                        perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                        return false;                                
                }

                set_status_safe(&winfo[id].status, QUIT, &winfo[id].mutex);

                if (pthread_sigmask(SIG_UNBLOCK, &t_set, NULL)) {
                        perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                        return false;                                
                }
                
                FD_CLR(winfo[id].socket, &all_fds);

                #ifdef DEBUG
                printf("{DEBUG} [Main Thread] Comunication with %d-th client has expired\n", id);
                #endif

                return true;
        }

        if (pthread_sigmask(SIG_BLOCK, &t_set, NULL)) {
                perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                return false;                                
        }

        if (pthread_mutex_lock(&winfo[id].mutex)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_mutex_%d)", id);
                perr(error_message);
                return false;
        }

        if (config->is_adaptive) {
                if (adapt[id].seq_num <= get_sequence_number(recv_header)) {
                        gettimeofday(&tv, NULL);
                        adapt[id].sampleRTT = elapsed_usec(&adapt[id].saved_tv, &tv);
                        adapt[id].estimatedRTT = ((1 - ALPHA) * adapt[id].estimatedRTT) + (ALPHA * adapt[id].sampleRTT);
                        adapt[id].devRTT = ((1 - BETA) * adapt[id].devRTT) + (BETA * abs_long(adapt[id].sampleRTT - adapt[id].estimatedRTT));
                        adapt[id].seq_num = get_sequence_number(recv_header);
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

        if (pthread_sigmask(SIG_UNBLOCK, &t_set, NULL)) {
                perr("{ERROR} [Main Thread] Block of signals before critical section failed");
                return false;                                
        }

        if (pthread_cond_signal(&winfo[id].cond_var)) {
                snprintf(error_message, ERR_SIZE, "{ERROR} [Main Thread] Syncronization protocol for worker threads broken (worker_condvar_%d)", id);
                perr(error_message);
                return false;
        }

        return true;
}

bool send_error_message(int id, int acc_socket)
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
                if ((wsize += gbn_send(acc_socket, header, NULL, 0, &winfo[id].client_sockaddr)) == -1) {
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

bool acceptance_loop(int acc_socket)
{
        int winfo_index, ready_fds, maxfd;
        struct sockaddr_in addr;
        gbn_ftp_header_t header;
        char payload[CHUNK_SIZE];
        fd_set read_fds;
        bool already_handled;
        bool can_open;

        FD_ZERO(&all_fds);
        FD_SET(acc_socket, &all_fds);
        maxfd = acc_socket;

        while(true) {
                read_fds = all_fds;
                memset(&addr, 0x0, sizeof(struct sockaddr_in));
                memset(payload, 0x0, CHUNK_SIZE);

                if ((ready_fds = select(maxfd + 1, &read_fds, NULL, NULL, NULL)) == -1) {
                        perr("{ERROR} [Main Thread] Unable to receive command from clients (select)");
                        return false;
                }

                if (FD_ISSET(acc_socket, &read_fds)) {

                        if (gbn_receive(acc_socket, &header, payload, &addr) == -1) {
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
                                                if (!send_error_message(winfo_index, acc_socket))
                                                        return false; 
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
                                                if (!send_error_message(winfo_index, acc_socket))
                                                        return false; 
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
        char choice;

        #ifdef DEBUG
        printf("*** DEBUG MODE ***\n\n\n");
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

        srand(time(0));
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
                                printf("rcvtimeout.: %lu\n", config->rto_usec);
                                printf("port.......: %u\n", acceptance_port);
                                printf("adapitve...: %s\n\n", (config->is_adaptive) ? "true" : "false");
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

        printf("\nServer is now listening ... ");
        fflush(stdout);

        if (!acceptance_loop(acceptance_sockfd))
                exit_server(EXIT_FAILURE);

        close(acceptance_sockfd);        
        exit_server(EXIT_SUCCESS);
}

