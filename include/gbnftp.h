#ifndef GBNFTP_H
#define GBNFTP_H

#include <stdbool.h>
#include <arpa/inet.h>

#define MAX_SEQ_NUMBER 268435455 

#define LISTMASK 0x0
#define GETMASK 0x1
#define PUTMASK 0x2
#define ARMASK 0x3
#define LASTMASK 0x4
#define CONNMASK 0x8
#define TYPEMASK 0x3
#define FLAGMASK 0xe

#define SEQ_NUM_SIZE 28
#define FLAGS_SIZE 4

#define DEFAULT_PORT 2929
#define CHUNK_SIZE 512

struct gbn_config {
        unsigned int N;
        unsigned long rto_usec;
        bool is_adaptive;
        float probability;
};

/* STRUTTURA DELL'HEADER DEL PROTOCOLLO
 * 4 BYTE (32 bit)
 * 28 bit dedicati al numero di sequenza (268435455 numeri possibili)
 * 4 bit di flag cosi' organizzati:
 *      - connection (1 bit)
 *      - last (1 bit)
 *      - type (2 bit):
 *              00 -> LIST
 *              01 -> PUT
 *              10 -> GET
 *              11 -> ACK (or RESPONSE) 
 */
typedef unsigned int gbn_ftp_header_t;

enum message_type {LIST, PUT, GET, ACK_OR_RESP, ERR};

void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no);
unsigned int get_sequence_number(gbn_ftp_header_t header);
void set_message_type(gbn_ftp_header_t *header, enum message_type type);
enum message_type get_message_type(gbn_ftp_header_t header);
void set_last(gbn_ftp_header_t *header, bool is_last);
bool is_last(gbn_ftp_header_t header);
void set_conn(gbn_ftp_header_t *header, bool is_conn);
bool is_conn(gbn_ftp_header_t header);
ssize_t gbn_send(int socket, gbn_ftp_header_t header, const void *payload, size_t payload_length, const struct sockaddr_in *sockaddr_in, const struct gbn_config *configs);
ssize_t gbn_receive(int socket, gbn_ftp_header_t *header, char *payload, const struct sockaddr_in *sockaddr_in);
bool is_syn_pkt(gbn_ftp_header_t header);
bool is_synack_pkt(gbn_ftp_header_t header, const char *payload);
bool is_ack_pkt(gbn_ftp_header_t header, const char *payload, unsigned int *ack_no_ptr);

#endif