/*
        FILE DI PROVA PER TESTARE IL COORDINAMENTO DEI DUE THREAD PER CIASCUNA RICHIESTA
        STRINGA DI COMPILAZIONE: gcc src/prova.c src/gbnftp.c src/common.c -o bin/prova -pthread -Iinclude/. -Wall -Wextra
*/

#include <pthread.h>
#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>

#include "gbnftp.h"
#include "common.h"

extern const struct gbn_config DEFAULT_GBN_CONFIG;

struct thread_arguments {
        struct gbn_config *configs;
        struct gbn_send_params *params;
        pthread_mutex_t mutex;
        pthread_cond_t cond_var;
        int pipe_fd;
};

void *recv_worker(void * args) 
{
        struct thread_arguments *conn_info = (struct thread_arguments *)args;
        fd_set read_fds;
        struct timeval tv;
        int retval;
        char mess[1024];

        while (true) {
                
                FD_ZERO(&read_fds);
                FD_SET(STDIN_FILENO, &read_fds);
                tv.tv_sec = 5;
                tv.tv_usec = 0;//conn_info->configs->rto_usec;

                retval = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &tv);
                if (retval == -1) {
                        error_handler("\"select()\" failed.");
                        return NULL;
                } else if (retval) {
                        memset(mess, 0x0, 1024);

                        get_input(1024, mess, false);
                        write(conn_info->pipe_fd, mess, strlen(mess));

                        if (pthread_mutex_lock(&conn_info->mutex)) {
                                error_handler("\"mutex_lock()\" failed.");
                                return NULL;
                        }
                        
                        conn_info->params->base++;

                        if (pthread_mutex_unlock(&conn_info->mutex)) {
                                error_handler("\"mutex_unlock()\" failed.");
                                return NULL;
                        }

                        if (pthread_cond_signal(&conn_info->cond_var)) {
                                error_handler("\"cond_signal()\" failed.");
                                return NULL;
                        }

                }  else {
                        write(conn_info->pipe_fd, "TIMER", 5);
                }
        }
}

void *send_worker(void *args)
{
        struct thread_arguments *conn_info = (struct thread_arguments *)args;
        int i = 0;
        fd_set read_fds;
        int retval;
        char mess[1024];

        if (pthread_mutex_lock(&conn_info->mutex)) {
                error_handler("\"mutex_lock()\" failed.");
                return NULL;
        }

        while(true) {

                FD_ZERO(&read_fds);
                FD_SET(conn_info->pipe_fd, &read_fds);

                retval = select(conn_info->pipe_fd + 1, &read_fds, NULL, NULL, NULL);

                if (retval == -1) {
                        error_handler("\"select()\" failed.");
                        return NULL;
                } else {
                        read(conn_info->pipe_fd, mess, 1024);
                }

                while(conn_info->params->next_seq_num >= conn_info->params->base + conn_info->configs->N) {
                        if (pthread_cond_wait(&conn_info->cond_var, &conn_info->mutex)) {
                                error_handler("\"cond_wait()\" failed.");
                                return NULL;
                        }
                }

                conn_info->params->next_seq_num ++;

                if (pthread_mutex_unlock(&conn_info->mutex)) {
                        error_handler("\"mutex_lock()\" failed.");
                        return NULL;
                }

                printf("Mess: %s (i = %d)\n", mess, i++);
        }
  
}

void create_workers() 
{
        struct thread_arguments *rargs, *wargs;
        pthread_t dummy;
        int pipe_fds[2];

        rargs = malloc(sizeof(struct thread_arguments));
        wargs = malloc(sizeof(struct thread_arguments));

        rargs->configs = malloc(sizeof(struct gbn_config));
        memcpy(rargs->configs, &DEFAULT_GBN_CONFIG, sizeof(struct gbn_config));

        rargs->params = malloc(sizeof(struct gbn_send_params));
        init_send_params(rargs->params);

        pthread_mutex_init(&rargs->mutex, NULL);

        pthread_cond_init(&rargs->cond_var, NULL);

        memcpy(wargs, rargs, sizeof(struct thread_arguments));

        pipe(pipe_fds);

        wargs->pipe_fd = pipe_fds[0];
        rargs->pipe_fd = pipe_fds[1];

        pthread_create(&dummy, NULL, recv_worker, rargs);
        pthread_create(&dummy, NULL, send_worker, wargs);
}

int main(void) 
{
        create_workers();

        pause();
}