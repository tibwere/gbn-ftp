#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "gbnftp.h"

void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no) 
{
        *header = (seq_no % MAX_SEQ_NUMBER) << 3;
}

unsigned int get_sequence_number(gbn_ftp_header_t header)
{
        return header >> 3;
}

void set_message_type(gbn_ftp_header_t *header, enum message_type type)
{
        switch(type) {
                case LIST: *header |= LISTMASK; break;
                case PUT: *header |= PUTMASK; break;
                case GET: *header |= GETMASK; break;
                case ACK: *header |= ACKMASK; break;
                default: break;
        }
}

enum message_type get_message_type(gbn_ftp_header_t header)
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

char * make_message(gbn_ftp_header_t header, char *payload, size_t payload_size)
{
        char *message;
        long unsigned header_size = sizeof(gbn_ftp_header_t);

        if ((message = malloc(header_size + payload_size)) == NULL)
                return NULL;


        snprintf(message, header_size, "%u", header);
        memcpy((message + header_size), payload, payload_size);

        return message;

}

void get_message(char *message, gbn_ftp_header_t *header, char *payload, size_t message_size)
{
        long unsigned header_size = sizeof(gbn_ftp_header_t);

        char header_raw[header_size + 1];
        
        memcpy(header_raw, message, header_size);
        header_raw[header_size] = '\n';

        *header = strtol(header_raw, NULL, 10);

        memcpy(payload, (message + header_size), message_size - header_size);
}
