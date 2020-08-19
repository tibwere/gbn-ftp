#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "gbnftp.h"
#include "common.h"

const struct gbn_config DEFAULT_GBN_CONFIG = {
        16, 1000000, false, 0.2
};

void set_sequence_number(gbn_ftp_header_t *header, unsigned int seq_no) 
{
        int flags = *header & FLAGMASK;
        int seq = (seq_no % MAX_SEQ_NUMBER) << FLAGS_SIZE;

        *header = seq | flags;
}

unsigned int get_sequence_number(gbn_ftp_header_t header)
{
        return header >> FLAGS_SIZE;
}

void set_message_type(gbn_ftp_header_t *header, enum message_type type)
{
        *header >>= 2;
        *header <<= 2;

        switch(type) {
                case LIST: *header |= LISTMASK; break;
                case PUT: *header |= PUTMASK; break;
                case GET: *header |= GETMASK; break;
                case ACK_OR_RESP: *header |= ARMASK; break;
                default:
                        fprintf(stderr, "Invalid condition at %s:%d\n", __FILE__, __LINE__);
                        abort();
        }
}

enum message_type get_message_type(gbn_ftp_header_t header)
{
        unsigned int flags = header & TYPEMASK;

        if (flags == LISTMASK)
                return LIST;
        else if (flags == PUTMASK)
                return PUT;
        else if (flags == GETMASK)
                return GET;
        else if (flags == ARMASK)
                return ACK_OR_RESP;
        else   
                return ERR;
}

void set_last(gbn_ftp_header_t *header, bool is_last)
{
        if (is_last)
                *header |= LASTMASK;
        else
                *header &= ~LASTMASK;
}

bool is_last(gbn_ftp_header_t header)
{
        return ((header & LASTMASK) == LASTMASK) ? true : false;
}

void set_conn(gbn_ftp_header_t *header, bool is_conn)
{
        if (is_conn)
                *header |= CONNMASK;
        else
                *header &= ~CONNMASK;
        
}

bool is_conn(gbn_ftp_header_t header)
{
        return ((header & CONNMASK) == CONNMASK) ? true : false;
}

static char * make_segment(gbn_ftp_header_t header, const char *payload, size_t payload_size)
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

static void get_segment(char *message, gbn_ftp_header_t *header, char *payload, size_t message_size)
{
        long unsigned header_size = sizeof(gbn_ftp_header_t);
        
        if (header != NULL) {
                memcpy(header, message, header_size);
                *header = ntohl(*header);
        }
        
        if (payload != NULL)
                memcpy(payload, (message + header_size), message_size - header_size);
}

ssize_t gbn_send(int socket, gbn_ftp_header_t header, const void *payload, size_t payload_length, const struct sockaddr_in *sockaddr_in, const struct gbn_config *configs)
{
        ssize_t send_size;
        char *message; 
        size_t length = payload_length + sizeof(gbn_ftp_header_t);
        
        if((message = make_segment(header, payload, payload_length)) == NULL) 
                return -1;

        // una volta implementato il timer rimuovere l'or true
        if (rand_double() > configs->probability || true) {
                if (sockaddr_in)
                        send_size = sendto(socket, message, length, MSG_NOSIGNAL, (struct sockaddr *) sockaddr_in, sizeof(struct sockaddr_in));
                else
                        send_size = send(socket, message, length, MSG_NOSIGNAL);
                
                free(message);
                return send_size;
                        
        } else {
                return 0;
        }
}

ssize_t gbn_receive(int socket, gbn_ftp_header_t *header, char *payload, const struct sockaddr_in *sockaddr_in)
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

        get_segment(message, header, payload, received_size);

        return received_size;
}


bool is_syn_pkt(gbn_ftp_header_t header)
{
        return is_conn(header) && (get_sequence_number(header) == 0);
}

bool is_synack_pkt(gbn_ftp_header_t header, const char *payload) 
{
        return is_conn(header) && (get_sequence_number(header) == 0) && (strncmp(payload, "0", 1) == 0) && get_message_type(header) == ACK_OR_RESP;

}

bool is_ack_pkt(gbn_ftp_header_t header, const char *payload, unsigned int *ack_no_ptr)
{
        bool result = (get_message_type(header) == ACK_OR_RESP);

        if (result)
                *ack_no_ptr = strtol(payload, NULL, 10);
        
        return result;
}


