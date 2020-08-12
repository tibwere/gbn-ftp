#include "gbnftp.h"

void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no) 
{
        *header = (seq_no % MAX_SEQ_NUMBER) << 3;
}

unsigned int get_sequence_number(gbn_ftp_header_t header)
{
        return header >> 3;
}

void set_packet_type(gbn_ftp_header_t *header, enum packet_type type)
{
        switch(type) {
                case LIST: *header |= LISTMASK; break;
                case PUT: *header |= PUTMASK; break;
                case GET: *header |= GETMASK; break;
                case ACK: *header |= ACKMASK; break;
                default: break;
        }
}

enum packet_type get_packet_type(gbn_ftp_header_t header)
{
        unsigned int flags = header & ACKMASK;

        if (flags == LISTMASK)
                return LIST;
        else if (flags == PUTMASK)
                return PUT;
        else if (flags == GETMASK)
                return GET;
        else if (flags == ACKMASK)
                return ACKMASK;
        else   
                return ERR;
}

void set_last(gbn_ftp_header_t *header, bool is_last)
{
        if (is_last)
                *header |= 0x1;
}

bool is_last(gbn_ftp_header_t header)
{
        return ((header & LASTMASK) == LASTMASK) ? true : false;
}
