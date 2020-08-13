#ifndef GBNFTP_H
#define GBNFTP_H

#include <stdbool.h>
#include <arpa/inet.h>

#define MAX_SEQ_NUMBER 268435455 

#define LISTMASK 0x0
#define LASTMASK 0x1
#define PUTMASK 0x2
#define GETMASK 0x4
#define RESPONSEMASK 0x6
#define ACKMASK 0x8
#define FLAGMASK 0xe

#define SEQ_NUM_SIZE 28
#define FLAGS_SIZE 4

#define DEFAULT_PORT 2929
#define CHUNK_SIZE 512

struct gbn_config {
        unsigned int N;
        unsigned long rto_msec;
        bool is_adaptive;
        float probability;
};

struct gbn_send_params {
        unsigned int base;
        unsigned int next_seq_num;
};

/* STRUTTURA DELL'HEADER DEL PROTOCOLLO
 * 4 BYTE (32 bit)
 * 28 bit dedicati al numero di sequenza (268435455 numeri possibili)
 * 4 bit di flag cosi' organizzati:
 *      - type (3 bit):
 *              000 -> LIST
 *              001 -> PUT
 *              010 -> GET
 *              011 -> RESPONSE
 *              100 -> ACK
 *      - last (1 bit) 
 */
typedef unsigned int gbn_ftp_header_t;

enum message_type {LIST, PUT, GET, ACK, RESPONSE, ERR};

void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no);
unsigned int get_sequence_number(gbn_ftp_header_t header);
void set_message_type(gbn_ftp_header_t *header, enum message_type type);
enum message_type get_message_type(gbn_ftp_header_t header);
void set_last(gbn_ftp_header_t *header, bool is_last);
bool is_last(gbn_ftp_header_t header);
char * make_segment(gbn_ftp_header_t header, char *payload, size_t payload_size);
void get_segment(char *message, gbn_ftp_header_t *header, char *payload, size_t message_size);
void init_send_params(struct gbn_send_params *params);
ssize_t gbn_send_ctrl_message(int socket, enum message_type type, const struct sockaddr *dest_addr, struct gbn_send_params *params, struct gbn_config *configs);

#endif