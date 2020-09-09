/*
 * File..: gbnftp.h
 * Autore: Simone Tiberi M.0252795
 *
 */
#ifndef GBNFTP_H
#define GBNFTP_H

/* LIBRERIE STANDARD */
#include <stdbool.h>
#include <arpa/inet.h>
#include <stdint.h>

/* MACRO DI PRECOMPILAZIONE */
#define MAX_SEQ_NUMBER 134217727 // 2^SEQ_NUM_SIZE - 1

#define ZERO_MASK 0x0
#define LIST_MASK 0x1
#define PUT_MASK 0x2
#define GET_MASK 0x3
#define TYPE_MASK 0x3

#define LAST_MASK 0x4
#define ACK_MASK 0x8
#define ERR_MASK 0x10

#define FLAG_MASK 0x1f

#define SEQ_NUM_SIZE 27
#define BITMASK_SIZE 5
#define TYPE_SIZE 2

#define DEFAULT_PORT 2929
#define CHUNK_SIZE 512

#define ALPHA 0.125
#define BETA 0.25


/* STRUTTURE DATI */
struct gbn_config {
        unsigned int N;         /* Dimensione della finestra */
        long rto_usec;          /* Timeout di ritrasmissione (per modalita' fixed) */
        bool is_adaptive;       /* Flag booleano che settato a true indica la scelta di un timer adattativo */
};

struct gbn_adaptive_timeout {
        struct timeval saved_tv;        /* Istante di tempo in cui viene inviato il pacchetto per cui si deve stimare l'RTT */
        unsigned int seq_num;           /* Numero di sequenza associato al pacchetto di cui si deve stimare l'RTT */
        bool restart;                   /* Flag boolean che settato a true implica che e' possibile procedere con una nuova misurazione */
        long sampleRTT;                 /* Stimatore SRTT ereditato dal protocollo TCP */
        long estimatedRTT;              /* Stimatore ERTT ereditato dal protocollo TCP */ 
        long devRTT;                    /* Stimatore DRTT ereditato dal protocollo TCP */
};

/* STRUTTURA DELL'HEADER DEL PROTOCOLLO
 * 4 BYTE (32 bit)
 * 27 bit dedicati al numero di sequenza (134217727 numeri possibili)
 * 5 bit di flag cosi' organizzati:
 *      - err (1 bit)
 *      - ack (1 bit)
 *      - last (1 bit)
 *      - type (2 bit):
 *              00 -> ZERO
 *              01 -> LIST
 *              10 -> PUT
 *              11 -> GET 
 */
typedef uint32_t gbn_ftp_header_t;


/* ENUMERAZIONI */
enum message_type {ZERO, LIST, PUT, GET};

enum connection_status {
        FREE,           /* Nessuna connessione attiva (idle) */
        REQUEST,        /* Richiesta inviata (in attesa di ricevere una conferma) */
        CONNECTED,      /* Ufficialmente connesso */ 
        TIMEOUT,        /* Fase di ritrasmissione */
        QUIT            /* Fase di terminazione della comunicazione */
};


/* PROTOTIPI */
void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no);
unsigned int get_sequence_number(gbn_ftp_header_t header);
void set_message_type(gbn_ftp_header_t *header, enum message_type type);
enum message_type get_message_type(gbn_ftp_header_t header);
void set_last(gbn_ftp_header_t *header, bool is_last);
bool is_last(gbn_ftp_header_t header);
void set_ack(gbn_ftp_header_t *header, bool is_conn);
bool is_ack(gbn_ftp_header_t header);
void set_err(gbn_ftp_header_t *header, bool is_err);
bool is_err(gbn_ftp_header_t header);
ssize_t gbn_send(int socket, gbn_ftp_header_t header, const void *payload, size_t payload_length, const struct sockaddr_in *sockaddr_in);
ssize_t gbn_receive(int socket, gbn_ftp_header_t *header, void *payload, const struct sockaddr_in *sockaddr_in);

#endif