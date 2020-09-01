#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include "gbnftp.h"
#include "common.h"

bool verbose;
extern const struct gbn_config DEFAULT_GBN_CONFIG;

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

struct gbn_config *init_configurations(void) 
{
        struct gbn_config *cfg;

        if ((cfg = malloc(sizeof(struct gbn_config))) == NULL)
                return NULL;

        memcpy(cfg, &DEFAULT_GBN_CONFIG, sizeof(struct gbn_config));

        return cfg;
}

double rand_double(void)
{
        return (double) rand() / (double) RAND_MAX;
}

long elapsed_usec(const struct timeval *start, const struct timeval *stop)
{
        unsigned long sec;
        unsigned long usec;
        struct timeval emptytv;

        memset(&emptytv, 0x0, sizeof(struct timeval));

        if (memcmp(start, &emptytv, sizeof(struct timeval)) == 0)
                return -1;

        sec = stop->tv_sec - start->tv_sec;
        usec = stop->tv_usec - start->tv_usec;

        return sec * 1000000 + usec; 
}

bool setup_signals(sigset_t *thread_mask , void (*sig_handler)(int))
{	
	struct sigaction act;
	
	memset(&act, 0, sizeof(struct sigaction));
        
        act.sa_flags = 0;
	act.sa_handler = sig_handler;

        if (sigfillset(&act.sa_mask) == -1) {
		perr("{ERROR} [Main Thread] failed to initialize signal mask for main thread");
		return false;                
        }

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
	
	act.sa_handler = SIG_IGN;
	
	while (sigaction(SIGPIPE, &act, NULL) == -1) {
		if (errno != EINTR) {
			perr("{ERROR} [Main Thread] Unable to set to ignore SIGPIPE");
			return false;
		}
	}

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

long abs_val(long value) {
        return (value >= 0) ? value : -value;
}

enum connection_status get_status_safe(volatile enum connection_status *status, pthread_mutex_t *mutex)
{
        enum connection_status s = QUIT;

        pthread_mutex_lock(mutex);
        s = *status;
        pthread_mutex_unlock(mutex);

        return s;
}

void set_status_safe(volatile enum connection_status *old_status, enum connection_status new_status, pthread_mutex_t *mutex)
{
        pthread_mutex_lock(mutex);
        *old_status = new_status;
        pthread_mutex_unlock(mutex);
}

unsigned int get_gbn_param_safe(volatile unsigned int *param, pthread_mutex_t *mutex)
{
        unsigned int p = -1;

        pthread_mutex_lock(mutex);
        p = *param;
        pthread_mutex_unlock(mutex);

        return p;
}

void set_gbn_param_safe(volatile unsigned int *old_param, volatile unsigned int new_param, pthread_mutex_t *mutex)
{
        pthread_mutex_lock(mutex);
        *old_param = new_param;
        pthread_mutex_unlock(mutex);
}