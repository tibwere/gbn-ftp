/*
 * File..: gbnftp.c
 * Autore: Simone Tiberi M.0252795
 *
 */

 /* LIBRERIE STANDARD */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>


/* LIBRERIE CUSTOM */
#include "gbnftp.h"
#include "common.h"


/* CONFIGURAZIONE DI DEFAULT */
const struct gbn_config DEFAULT_GBN_CONFIG = {
        4, 500, true
};

/*
 * funzione:    set_sequence_number
 * 
 * descrizione:	Funzione che permette di impostare il numero di sequenza in un header
 *
 * parametri:	header (gbn_ftp_header *):      Puntatore ad un'intestazione
 *              seq_no (unsigned int):          Numero di sequenza da impostare
 *
 */
void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no) 
{
        if (seq_no >= MAX_SEQ_NUMBER) {
                fprintf(stderr, "Sequence number must be smaller then %d\n", MAX_SEQ_NUMBER);
                abort();
        }

        int flags = *header & BITMASK_SIZE;
        int seq = seq_no << BITMASK_SIZE;

        *header = seq | flags;
}


/*
 * funzione:    get_sequence_number
 * 
 * descrizione:	Funzione che permette di effettuare il retrive del numero di sequenza da un header
 *
 * parametri:	header (gbn_ftp_header):        Intestazione da cui si vuole prelevare il numero di sequenza
 *
 * return:      Numero di sequenza associato all'intestazione (unsigned int)
 *      
 */
unsigned int get_sequence_number(gbn_ftp_header_t header)
{
        return header >> BITMASK_SIZE;
}


/*
 * funzione:    set_message_type
 * 
 * descrizione:	Funzione che permette di impostare il tipo di messaggio in un header
 *
 * parametri:	header (gbn_ftp_header *):      Puntatore ad un'intestazione
 *              type (enum message_type):       Tipo da impostare
 *
 */
void set_message_type(gbn_ftp_header_t *header, enum message_type type)
{
        *header >>= TYPE_SIZE;
        *header <<= TYPE_SIZE;

        switch(type) {
                case ZERO: *header |= ZERO_MASK; break; 
                case LIST: *header |= LIST_MASK; break;
                case PUT: *header |= PUT_MASK; break;
                case GET: *header |= GET_MASK; break;
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();
        }
}


/*
 * funzione:    get_message_type
 * 
 * descrizione:	Funzione che permette di effettuare il retrive del tipo di messaggio da un header
 *
 * parametri:	header (gbn_ftp_header):        Intestazione da cui si vuole prelevare il numero di sequenza
 *
 * return:      Tipo di messaggio associato all'intestazione (enum message_type)
 *      
 */
enum message_type get_message_type(gbn_ftp_header_t header)
{
        unsigned int flags = header & TYPE_MASK;

        if (flags == LIST_MASK)
                return LIST;
        else if (flags == PUT_MASK)
                return PUT;
        else if (flags == GET_MASK)
                return GET;
        else   
                return ZERO;
}


/*
 * funzione:    set_last
 * 
 * descrizione:	Funzione che permette di impostare il flag LAST in un header
 * 
 * parametri:	header (gbn_ftp_header *):      Puntatore ad un'intestazione
 *              is_last (bool):                 true    se il segmento e' marcato con il flag LAST
 *                                              false   altrimenti
 *
 */
void set_last(gbn_ftp_header_t *header, bool is_last)
{
        if (is_last)
                *header |= LAST_MASK;
        else
                *header &= ~LAST_MASK;
}


/*
 * funzione:    is_last
 * 
 * descrizione:	Funzione che permette di verificare se un intestazione ha settato o meno il flag LAST
 *
 * parametri:	header (gbn_ftp_header):        Intestazione da cui si vuole effettuare il check
 *
 * return:	true    se il segmento e' marcato con il flag LAST
 *              false   altrimenti 
 *      
 */
bool is_last(gbn_ftp_header_t header)
{
        return ((header & LAST_MASK) == LAST_MASK) ? true : false;
}


/*
 * funzione:    set_ack
 * 
 * descrizione:	Funzione che permette di impostare il flag ACK in un header
 * 
 * parametri:	header (gbn_ftp_header *):      Puntatore ad un'intestazione
 *              is_ack (bool):                  true    se il segmento e' marcato con il flag ACK
 *                                              false   altrimenti
 *
 */
void set_ack(gbn_ftp_header_t *header, bool is_ack)
{
        if (is_ack)
                *header |= ACK_MASK;
        else
                *header &= ~ACK_MASK;
        
}


/*
 * funzione:    is_ack
 * 
 * descrizione:	Funzione che permette di verificare se un intestazione ha settato o meno il flag ACK
 *
 * parametri:	header (gbn_ftp_header):        Intestazione da cui si vuole effettuare il check
 *
 * return:	true    se il segmento e' marcato con il flag ACK
 *              false   altrimenti 
 *      
 */
bool is_ack(gbn_ftp_header_t header)
{
        return ((header & ACK_MASK) == ACK_MASK) ? true : false;
}


/*
 * funzione:    set_err
 * 
 * descrizione:	Funzione che permette di impostare il flag ERR in un header
 * 
 * parametri:	header (gbn_ftp_header *):      Puntatore ad un'intestazione
 *              is_err (bool):                  true    se il segmento e' marcato con il flag ERR
 *                                              false   altrimenti
 *
 */
void set_err(gbn_ftp_header_t *header, bool is_err)
{
        if (is_err)
                *header |= ERR_MASK;
        else
                *header &= ~ERR_MASK;
        
}


/*
 * funzione:    is_err
 * 
 * descrizione:	Funzione che permette di verificare se un intestazione ha settato o meno il flag ERR
 *
 * parametri:	header (gbn_ftp_header):        Intestazione da cui si vuole effettuare il check
 *
 * return:	true    se il segmento e' marcato con il flag ERR
 *              false   altrimenti 
 *      
 */
bool is_err(gbn_ftp_header_t header)
{
        return ((header & ERR_MASK) == ERR_MASK) ? true : false;
}


/*
 * funzione:    make_segment
 * 
 * descrizione:	Funzione responsabile dell'allocazione dinamica di un buffer in cui memorizza
 *              la concatenazione di header e payload
 *
 * parametri:	header (gbn_ftp_header):        Intestazione del segmento
 *              payload (const void *):         Payload del segmento
 *              payload_size (size_t):          Lunghezza del payload
 *
 * return:	Puntatore all'area di memoria allocata 
 *      
 */
static char * make_segment(gbn_ftp_header_t header, const void *payload, size_t payload_size)
{
        char *message;
        long unsigned header_size = sizeof(gbn_ftp_header_t);
        gbn_ftp_header_t header_temp = htonl(header);

        if ((message = malloc(header_size + payload_size)) == NULL)
                return NULL;

        memcpy(message, &header_temp, header_size);
        memcpy((message + header_size), payload, payload_size);

        return message;
}


/*
 * funzione:    get_segment
 * 
 * descrizione:	Funzione responsabile del parsing di un segmento ricevuto
 *              scomponendolo in intestazione e payload
 *
 * parametri:	message (const void *):         Messaggio ricevuto
 *              header (gbn_ftp_header *):      Puntatore ad un'area di memoria dove memorizzare l'intestazione
 *              payload (void *):               Puntatore ad un'area di memoria dove memorizzare il payload
 *              message_size (size_t):          Lunghezza del messaggio
 *      
 */
static void get_segment(const void *message, gbn_ftp_header_t *header, void *payload, size_t message_size)
{
        long unsigned header_size = sizeof(gbn_ftp_header_t);
        
        if (header != NULL) {
                memcpy(header, message, header_size);
                *header = ntohl(*header);
        }

        if (payload != NULL)
                memcpy(payload, (message + header_size), message_size - header_size);
}


/*
 * funzione:    gbn_send
 * 
 * descrizione:	Funzione responsabile dell'invio di un segmento
 *
 * parametri:	socket (int):                                   Descrittore associato alla socket verso cui inviare il segmento
 *              header (gbn_ftp_header):                        Intestazione del segmento da inviare
 *              payload (const void *):                         Payload del segmento da inviare
 *              payload_length (size_t):                        Lunghezza del payload
 *              sockaddr_in (const struct sockaddr_in *):       Struttura contenente le informazioni del destinatario
 *
 * return:      Numero di byte inviati (ssize_t)
 *      
 */
ssize_t gbn_send(int socket, gbn_ftp_header_t header, const void *payload, size_t payload_length, const struct sockaddr_in *sockaddr_in)
{
        ssize_t send_size;
        char *message; 
        size_t length = payload_length + sizeof(gbn_ftp_header_t);
        double rndval = (double) rand() / (double) RAND_MAX;
        
        if((message = make_segment(header, payload, payload_length)) == NULL) 
                return -1;

        if (rndval > PROBABILITY) {
                if (sockaddr_in)
                        send_size = sendto(socket, message, length, MSG_NOSIGNAL, (struct sockaddr *) sockaddr_in, sizeof(struct sockaddr_in));
                else
                        send_size = send(socket, message, length, MSG_NOSIGNAL);
                
                free(message);
                return send_size;
                        
        } else {
                free(message);
                return 0;
        }
}


/*
 * funzione:    gbn_receive
 * 
 * descrizione:	Funzione responsabile della ricezione di un segmento
 *
 * parametri:	socket (int):                                   Descrittore associato alla socket da cui ricevere il segmento
 *              header (gbn_ftp_header *):                      Puntatore ad un'area di memoria dove salvare l'intestazione del segmento ricevuto
 *              payload (const void *):                         Puntatore ad'un area di memoria dove salvare il payload del segmento ricevuto
 *              sockaddr_in (const struct sockaddr_in *):       Puntatore ad una struttura dove salvare le informazioni del mittente
 *
 * return:      Numero di byte ricevuti (ssize_t)
 *      
 */
ssize_t gbn_receive(int socket, gbn_ftp_header_t *header, void *payload, const struct sockaddr_in *sockaddr_in)
{
        ssize_t received_size;
        socklen_t sockaddr_size = sizeof(struct sockaddr_in);
        size_t message_size = sizeof(gbn_ftp_header_t) + CHUNK_SIZE;
        char message[message_size];
        
        memset(message, 0, message_size);

        if (sockaddr_in)
                received_size = recvfrom(socket, message, message_size, 0, (struct sockaddr *) sockaddr_in, &sockaddr_size);
        else
                received_size = recv(socket, message, message_size, 0);

        if (received_size != -1)
                get_segment(message, header, payload, received_size);

        return received_size;
}