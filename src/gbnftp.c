#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "gbnftp.h"
#include "common.h"

const struct gbn_config DEFAULT_GBN_CONFIG = {
        8, 100000, false, 0.01
};

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

unsigned int get_sequence_number(gbn_ftp_header_t header)
{
        return header >> BITMASK_SIZE;
}

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

void set_last(gbn_ftp_header_t *header, bool is_last)
{
        if (is_last)
                *header |= LAST_MASK;
        else
                *header &= ~LAST_MASK;
}

bool is_last(gbn_ftp_header_t header)
{
        return ((header & LAST_MASK) == LAST_MASK) ? true : false;
}

void set_ack(gbn_ftp_header_t *header, bool is_conn)
{
        if (is_conn)
                *header |= ACK_MASK;
        else
                *header &= ~ACK_MASK;
        
}

bool is_ack(gbn_ftp_header_t header)
{
        return ((header & ACK_MASK) == ACK_MASK) ? true : false;
}

void set_err(gbn_ftp_header_t *header, bool is_err)
{
        if (is_err)
                *header |= ERR_MASK;
        else
                *header &= ~ERR_MASK;
        
}

bool is_err(gbn_ftp_header_t header)
{
        return ((header & ERR_MASK) == ERR_MASK) ? true : false;
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

        if (rand_double() > configs->probability) {
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