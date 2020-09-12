/*
 * File..: common.c
 * Autore: Simone Tiberi M.0252795
 *
 */

/* LIBRERIE STANDARD */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>


/* LIBRERIE CUSTOM */
#include "gbnftp.h"
#include "common.h"


/* VARIABILI ESTERNE */
extern const struct gbn_config DEFAULT_GBN_CONFIG;


/* VARIABILI GLOBALI */
bool verbose;


/*
 * funzione:    detailed_perror
 * 
 * descrizione:	Funzione responsabile della stampa dei messaggi d'errore per le applicazioni
 *
 * parametri:	message (const char *):         Messaggio d'errore
 *              filename (const char *):        Nome del file dove si è generato l'errore
 *              line_num (int):                 Numero della linea di codice dove si è generato l'errore                  
 *
 */
void detailed_perror(const char *message, const char *filename, int line_num)
{
        fprintf(stderr, "%s\n", message);

        if (verbose) {
                fprintf(stderr, "Invalid condition at %s:%d\n", filename, line_num);
                if (errno != 0)
                        fprintf(stderr, "The error was: %s (%d)\n", strerror(errno), errno);
                else
                        fprintf(stderr, "No more detailes available\n");
        }
}


/*
 * funzione:    get_input
 * 
 * descrizione:	Funzione responsabile della lettura da stdin in maniera safe (senza rischio di buffer overflow)
 *
 * parametri:	length (unsigned int):  Lunghezza massima da leggere
 *              string (char *):        Buffer in cui memorizzare i dati letti
 *              not_null (bool):        Flag booleano per discriminare se la stringa onserita può o meno essere vuota     
 *
 * return:	Numero di byte letti (ssize_t)       
 *
 */
size_t get_input(unsigned int length, char *string, bool not_null)
{
        char c;
        unsigned int i;    

        do {
                for (i = 0; i < length; ++i) {
                        fread(&c, sizeof(char), 1, stdin);
                        if (c == '\n') {
                                string[i] = '\0';
                                break;
                        } else {
                                string[i] = c;  
                        }
                }
        } while (not_null && i == 0);

    
        if (i == length - 1)
                string[i] = '\0';

        if (strlen(string) >= length) {	
                do {
                        c = getchar();
                } while (c != '\n');
        }
        
        return i;
}


/*
 * funzione:    multi_choice
 * 
 * descrizione:	Funzione responsabile della gestione della scelta multipla (e.g. menu del client)
 *
 * parametri:	question (const char *):        Domanda da porre all'utente (tramite stdin)
 *              choices (const char *):         Possibili scelte
 *              no_choices (int):               Numero di scelte possibili
 *
 * return:	Scelta effettuata (char)
 *
 */
char multi_choice(const char *question, const char *choices, int no_choices)
{
    char choices_str[2 * no_choices * sizeof(char)];
    int i, j = 0;

    for (i = 0; i < no_choices; ++i) {
        choices_str[j++] = choices[i];
        choices_str[j++] = '/';
    }
    
    choices_str[j-1] = '\0';

    while (true) {
        printf("%s [%s]: ", question, choices_str);

        char c;
        get_input(1, &c, true);
        c = toupper(c);

        for (i = 0; i < no_choices; ++i) {
            if (c == toupper(choices[i]))
                return c;
        }

        printf("Sorry not compliant input, please retry!\n");
    }
}


/*
 * funzione:    init_configurations
 * 
 * descrizione:	Funzione responsabile dell'allocazione dinamica di una struct gbn_config 
 *              e dell'impostazione dei suoi parametri
 *
 * return:	La struct creata (struct gbn_config *)
 *
 */
struct gbn_config *init_configurations(void) 
{
        struct gbn_config *cfg;

        if ((cfg = malloc(sizeof(struct gbn_config))) == NULL)
                return NULL;

        memcpy(cfg, &DEFAULT_GBN_CONFIG, sizeof(struct gbn_config));

        return cfg;
}


/*
 * funzione:    elapsed_usec
 * 
 * descrizione:	Funzione che calcola delta temporali (in microsecondi [usec])
 *
 * parametri:	start (const struct timeval *): Istante di tempo iniziale
 *              stop (const struct timeval *):  Istante di tempo finale 
 *
 * return:	Tempo trascorso (long) 
 *
 */
long elapsed_usec(const struct timeval *start, const struct timeval *stop)
{
        unsigned long sec;
        unsigned long usec;
        struct timeval emptytv;

        memset(&emptytv, 0x0, sizeof(struct timeval));

        // se la struttura è azzerata implica che il timer è stato stoppato
        if (memcmp(start, &emptytv, sizeof(struct timeval)) == 0)
                return -1;

        sec = stop->tv_sec - start->tv_sec;
        usec = stop->tv_usec - start->tv_usec;

        return sec * 1000000 + usec; 
}


/*
 * funzione:    setup_signals
 * 
 * descrizione:	Funzione che imposta la maschera dei segnali per i thread spawnati 
 *              e l'handler per il main thread
 *
 * parametri:	thread_mask (sigset_t *):       Puntatore ad una maschera di segnali
 *              sig_handler (void (*)(int)):    Funzione adibita alla gestione dei segnali
 *
 * return:	true    nel caso in cui non vi sono errori
 *              false   altrimenti 
 *
 */
bool setup_signals(sigset_t *thread_mask , void (*sig_handler)(int))
{	
	struct sigaction act;
	
	memset(&act, 0, sizeof(struct sigaction));
        
        act.sa_flags = 0;
	act.sa_handler = sig_handler;

        // nel momento in cui viene gestito un segnale tutti gli altri segnali devono essere bloccati
        if (sigfillset(&act.sa_mask) == -1) {
		perr("{ERROR} [Main Thread] failed to initialize signal mask for main thread");
		return false;                
        }

        // impostato l'handler per i segnali di job control
	while (sigaction(SIGINT, &act, NULL) == -1) {
		if (errno != EINTR) {
			perr("{ERROR} [Main Thread] Unable to initialize signal management for main thread (SIGINT)");
			return false;
		}
	}

	while (sigaction(SIGQUIT, &act, NULL) == -1) {
		if (errno != EINTR) {
			perr("{ERROR} [Main Thread] Unable to initialize signal management for main thread (SIGQUIT)");
			return false;
		}
	}	

	while (sigaction(SIGTERM, &act, NULL) == -1) {
		if (errno != EINTR) {
			perr("{ERROR} [Main Thread] Unable to initialize signal management for main thread (SIGTERM)");
			return false;
		}
	}						
			
	while (sigaction(SIGHUP, &act, NULL) == -1) {
		if (errno != EINTR) {
			perr("{ERROR} [Main Thread] Unable to initialize signal management for main thread (SIGHUP)");
			return false;
		}
	}	
	
        // ignorato esplicitamente il SIGPIPE
	act.sa_handler = SIG_IGN;
	while (sigaction(SIGPIPE, &act, NULL) == -1) {
		if (errno != EINTR) {
			perr("{ERROR} [Main Thread] Unable to set to ignore SIGPIPE");
			return false;
		}
	}

        // aggiunti i segnali di job control alla maschera da bloccare per i thread
	if (sigemptyset(thread_mask) == -1) {
		perr("{ERROR} [Main Thread] failed to initialize signal mask for worker thread");
		return false;
	}
	
	if (sigaddset(thread_mask, SIGINT) == -1) {
		perr("{ERROR} [Main Thread] failed to initialize signal mask for worker thread");
		return false;
	}	
	
	if (sigaddset(thread_mask, SIGQUIT) == -1) {
		perr("{ERROR} [Main Thread] failed to initialize signal mask for worker thread");
		return false;
	}	
	
	if (sigaddset(thread_mask, SIGTERM) == -1) {
		perr("{ERROR} [Main Thread] failed to initialize signal mask for worker thread");
		return false;
	}
		
	if (sigaddset(thread_mask, SIGHUP) == -1) {
		perr("{ERROR} [Main Thread] failed to initialize signal mask for worker thread");
		return false;
	} 

        return true;			
}


/*
 * funzione:    get_status_safe
 * 
 * descrizione:	Funzione che effettua il retrive dello stato in maniera atomica
 *              tramite sezione critica
 *
 * parametri:	status (volatile enum connection_status *):     Puntatore ad uno stato
 *              mutex (pthread_mutex_t *):                      Mutex utilizzato per la sezione critica
 *
 * return:	Valore corrente dello stato (enum connection_status)  
 *
 */
enum connection_status get_status_safe(volatile enum connection_status *status, pthread_mutex_t *mutex)
{
        enum connection_status s = QUIT;

        pthread_mutex_lock(mutex);
        s = *status;
        pthread_mutex_unlock(mutex);

        return s;
}


/*
 * funzione:    set_status_safe
 * 
 * descrizione:	Funzione che imposta il nuovo valore dello stato in maniera atomica
 *              tramite sezione critica
 *
 * parametri:	old_status (volatile enum connection_status *): Puntatore allo stato da cambiare
 *              new_status (enum connection_status):            Nuovo valore per lo stato
 *              mutex (pthread_mutex_t *):                      Mutex utilizzato per la sezione critica
 *
 */
void set_status_safe(volatile enum connection_status *old_status, enum connection_status new_status, pthread_mutex_t *mutex)
{
        pthread_mutex_lock(mutex);
        *old_status = new_status;
        pthread_mutex_unlock(mutex);
}


/*
 * funzione:    get_gbn_param_safe
 * 
 * descrizione:	Funzione che effettua il retrive dello valore di un parametro di configurazione
 *              in modo atomico tramite sezione critica
 *
 * parametri:	param (volatile unsigned int *):        Puntatore ad uno paramtro
 *              mutex (pthread_mutex_t *):              Mutex utilizzato per la sezione critica
 *
 * return:	Valore corrente dello parametro richiesto (unsigned int) 
 *
 */
unsigned int get_gbn_param_safe(volatile unsigned int *param, pthread_mutex_t *mutex)
{
        unsigned int p = -1;

        pthread_mutex_lock(mutex);
        p = *param;
        pthread_mutex_unlock(mutex);

        return p;
}


/*
 * funzione:    set_gbn_param_safe
 * 
 * descrizione:	Funzione che imposta il nuovo valore di un paramtro di configurazione
 *              in maniera atomica tramite sezione critica
 *
 * parametri:	old_param (volatile unsigned int *):    Puntatore al parametro da cambiare
 *              new_param (unsigned int):               Nuovo valore per il parametro
 *              mutex (pthread_mutex_t *):              Mutex utilizzato per la sezione critica
 *
 */
void set_gbn_param_safe(volatile unsigned int *old_param, unsigned int new_param, pthread_mutex_t *mutex)
{
        pthread_mutex_lock(mutex);
        *old_param = new_param;
        pthread_mutex_unlock(mutex);
}


/*
 * funzione:    can_send_more_segment_safe
 * 
 * descrizione:	Funzione che verifica se è possibile inviare un nuovo segmento 
 *              in maniera atomica tramite sezione critica
 *
 * parametri:	base (volatile unsigned int *):         Puntatore al parametro base di GBN
 *              next_seq_num (volatile unsigned int *): Puntatore al parametro next sequence number di GBN
 *              N (unsigned int):                       Dimensione della finestra
 *              mutex (pthread_mutex_t *):              Mutex utilizzato per la sezione critica
 *
 * return:	true    nel caso in cui è possibile
 *              false   altrimenti 
 *
 */
bool can_send_more_segment_safe(volatile unsigned int *base, volatile unsigned int *next_seq_num, unsigned int N, pthread_mutex_t * mutex)
{
        bool retval = false;

        pthread_mutex_lock(mutex);
        retval = (*next_seq_num < *base + N);
        pthread_mutex_unlock(mutex);

        return retval;
}


/*
 * funzione:    get_adaptive_rto_safe
 * 
 * descrizione:	Funzione che effettua il retrive del valore aggiornato del timout in ricezione
 *
 * parametri:	adapt (gbn_adaptive_timeout *): Puntatore ad una struct per la gestione del timer adattativo
 *              mutex (pthread_mutex_t *):      Mutex utilizzato per la sezione critica
 *
 * return:	Valore corrente del recv_to (long) 
 *
 */
long get_adaptive_rto_safe(struct gbn_adaptive_timeout *adapt, pthread_mutex_t *mutex)
{
        long rto;

        pthread_mutex_lock(mutex);
        rto = adapt->estimatedRTT + 4 * adapt->devRTT;
        pthread_mutex_unlock(mutex);        
        
        return rto;
}