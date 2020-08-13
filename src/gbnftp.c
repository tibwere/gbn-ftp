#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "gbnftp.h"

const struct gbn_config DEFAULT_GBN_CONFIG = {
        16, 1000, false, 0.2
};

void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no) 
{
        *header = (seq_no % MAX_SEQ_NUMBER) << FLAGS_SIZE;
}

unsigned int get_sequence_number(gbn_ftp_header_t header)
{
        return header >> FLAGS_SIZE;
}

void set_message_type(gbn_ftp_header_t *header, enum message_type type)
{
        // reset della maschera di bit
        unsigned int last_flag = *header & LASTMASK;
        *header >>= FLAGS_SIZE;
        *header <<= FLAGS_SIZE;
        *header |= last_flag;

        switch(type) {
                case LIST: *header |= LISTMASK; break;
                case PUT: *header |= PUTMASK; break;
                case GET: *header |= GETMASK; break;
                case RESPONSE: *header |= RESPONSEMASK; break;
                case ACK: *header |= ACKMASK; break;
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();
        }
}

enum message_type get_message_type(gbn_ftp_header_t header)
{
        unsigned int flags = header & FLAGMASK;

        if (flags == LISTMASK)
                return LIST;
        else if (flags == PUTMASK)
                return PUT;
        else if (flags == GETMASK)
                return GET;
        else if (flags == RESPONSEMASK)
                return RESPONSE;
        else if (flags == ACKMASK)
                return ACKMASK;
        else   
                return ERR;
}

void set_last(gbn_ftp_header_t *header, bool is_last)
{
        if (is_last)
                *header |= LASTMASK;
}

bool is_last(gbn_ftp_header_t header)
{
        return ((header & LASTMASK) == LASTMASK) ? true : false;
}

char * make_segment(gbn_ftp_header_t header, char *payload, size_t payload_size)
{
        char *message;
        long unsigned header_size = sizeof(gbn_ftp_header_t);

        if ((message = malloc(header_size + payload_size)) == NULL)
                return NULL;


        snprintf(message, header_size, "%u", header);
        memcpy((message + header_size), payload, payload_size);

        return message;

}

void get_segment(char *message, gbn_ftp_header_t *header, char *payload, size_t message_size)
{
        long unsigned header_size = sizeof(gbn_ftp_header_t);

        char header_raw[header_size + 1];
        
        memcpy(header_raw, message, header_size);
        header_raw[header_size] = '\n';

        *header = strtol(header_raw, NULL, 10);

        memcpy(payload, (message + header_size), message_size - header_size);
}

void init_send_params(struct gbn_send_params *params)
{
        memset(params, 0x0, sizeof(struct gbn_send_params));
        params->base = 0;
        params->next_seq_num = 0;
}

ssize_t gbn_send_ctrl_message(int socket, enum message_type type, const struct sockaddr *dest_addr, struct gbn_send_params *params, struct gbn_config *configs)
{
        gbn_ftp_header_t header;

        unsigned long header_size = sizeof(gbn_ftp_header_t);
        unsigned long addr_size = sizeof(struct sockaddr_in);

        if (params->next_seq_num < params->base + configs->N) {
                set_sequence_number(&header, params->next_seq_num++);
                set_message_type(&header, type);
                set_last(&header, true);
                sendto(socket, make_segment(header, NULL, 0), header_size, MSG_NOSIGNAL, dest_addr, addr_size);
        }

        return 0;
}

/*
ssize_t gbn_send_file(int socket, const void *payload, size_t payload_size, const struct sockaddr *dest_addr, 
        struct gbn_send_params *params, struct gbn_config *configs, enum message_type type)
{
        return 0;
}
*/
