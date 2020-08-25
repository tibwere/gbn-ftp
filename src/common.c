#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "gbnftp.h"
#include "common.h"

bool verbose;
extern const struct gbn_config DEFAULT_GBN_CONFIG;

void detailed_perror(const char *message, const char *filename, int line_num)
{
        fprintf(stderr, "%s\n", message);

        if (verbose) {
                fprintf(stderr, "Invalid condition at %s:%d\n", filename, line_num);
                if (errno != 0)
                        fprintf(stderr, "The error was: %s (%d)\n", strerror(errno), errno);
                else
                        fprintf(stderr, "No more detailes available\n");
        }
}

size_t get_input(unsigned int length, char *string, bool not_null)
{
        char c;
        unsigned int i;    

        do {
                for (i = 0; i < length; ++i) {
                        fread(&c, sizeof(char), 1, stdin);
                        if (c == '\n') {
                                string[i] = '\0';
                                break;
                        } else {
                                string[i] = c;  
                        }
                }
        } while (not_null && i == 0);

    
        if (i == length - 1)
                string[i] = '\0';

        if (strlen(string) >= length) {	
                do {
                        c = getchar();
                } while (c != '\n');
        }
        
        return i;
}

char multi_choice(const char *question, const char *choices, int no_choices)
{
    char choices_str[2 * no_choices * sizeof(char)];
    int i, j = 0;

    for (i = 0; i < no_choices; ++i) {
        choices_str[j++] = choices[i];
        choices_str[j++] = '/';
    }
    
    choices_str[j-1] = '\0';

    while (true) {
        printf("%s [%s]: ", question, choices_str);

        char c;
        get_input(1, &c, true);
        c = toupper(c);

        for (i = 0; i < no_choices; ++i) {
            if (c == toupper(choices[i]))
                return c;
        }

        printf("Sorry not compliant input, please retry!\n");
    }
}

struct gbn_config *init_configurations(void) 
{
        struct gbn_config *cfg;

        if ((cfg = malloc(sizeof(struct gbn_config))) == NULL)
                return NULL;

        memcpy(cfg, &DEFAULT_GBN_CONFIG, sizeof(struct gbn_config));

        return cfg;
}

double rand_double(void)
{
        return (double) rand() / (double) RAND_MAX;
}

unsigned long elapsed_usec(const struct timeval *start, const struct timeval *stop)
{
        unsigned long sec;
        unsigned long usec;

        sec = stop->tv_sec - start->tv_sec;
        usec = stop->tv_usec - start->tv_usec;

        return sec * 1000000 + usec; 
}