/*
 * File..: common.h
 * Autore: Simone Tiberi M.0252795
 *
 */
#ifndef COMMON_H
#define COMMON_H

/* MACRO DI PRECOMPILAZIONE */
#define ERR_SIZE 256
#define PATH_SIZE 1024
#define MAX_TO 16
#define LAST_MESSAGE_LOOP 4
#define PROBABILITY 0.01
#define MAX_TO_SEC 10
#define MAX_ERTT_SCALE 1000
#define MAX_DRTT_USEC 25000
#define INC_RATE 1.25
#define MAX_CONNECTION_ATTEMPT 10

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define ABS(x) (((x) > 0) ? (x) : -(x))
#define perr(mess) detailed_perror(mess, __FILE__, __LINE__)


/* ENUMERAZIONI */
enum app_usages {
        STANDARD,       /* Modalità d'uso standard */
        HELP,           /* Richiede la stampa su stdin del help dell'applicazione */
        VERSION,        /* Richiede la stampa su stdin della versione dell'applicazione */
        ERROR           /* Errore nel parsing della command line (parametri inseriti errati) */
};


/* PROTOTIPI */
void detailed_perror(const char *message, const char *filename, int line_num);
size_t get_input(unsigned int length, char *string, bool not_null);
char multi_choice(const char *question, const char *choices, int no_choices);
struct gbn_config *init_configurations(void); 
long elapsed_usec(const struct timeval *start, const struct timeval *stop);
bool setup_signals(sigset_t *thread_mask , void (*sig_handler)(int));
enum connection_status get_status_safe(volatile enum connection_status *status, pthread_mutex_t *mutex);
void set_status_safe(volatile enum connection_status *old_status, enum connection_status new_status, pthread_mutex_t *mutex);
unsigned int get_gbn_param_safe(volatile unsigned int *param, pthread_mutex_t *mutex);
void set_gbn_param_safe(volatile unsigned int *old_param, unsigned int new_param, pthread_mutex_t *mutex);
bool can_send_more_segment_safe(volatile unsigned int *base, volatile unsigned int *next_seq_num, unsigned int N, pthread_mutex_t * mutex);
long get_adaptive_rto_safe(struct gbn_adaptive_timeout *adapt, pthread_mutex_t *mutex);

#endif