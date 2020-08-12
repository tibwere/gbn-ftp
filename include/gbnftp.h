#ifndef GBNFTP_H
#define GBNFTP_H

#include <stdbool.h>

#define MAX_SEQ_NUMBER 536870911
#define LISTMASK 0x0
#define LASTMASK 0x1
#define PUTMASK 0x2
#define GETMASK 0x4
#define ACKMASK 0x6

struct gbn_config {
        unsigned int N;
        unsigned long rto_msec;
        bool is_adaptive;
        float probability;
};

/* STRUTTURA DELL'HEADER DEL PROTOCOLLO
 * 4 BYTE (32 bit)
 * 29 bit dedicati al numero di sequenza (536870911 numeri possibili)
 * 3 bit di flag cosi' organizzati:
 *      - type (2 bit):
 *              00 -> LIST
 *              01 -> PUT
 *              10 -> GET
 *              11 -> ACK
 *      - last (1 bit) 
 */
typedef unsigned int gbn_ftp_header_t;

enum packet_type {LIST, PUT, GET, ACK, ERR};

void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no);
unsigned int get_sequence_number(gbn_ftp_header_t header);
void set_packet_type(gbn_ftp_header_t *header, enum packet_type type);
enum packet_type get_packet_type(gbn_ftp_header_t header);
void set_last(gbn_ftp_header_t *header, bool is_last);
bool is_last(gbn_ftp_header_t header);

#endif