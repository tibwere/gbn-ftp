#ifndef COMMON_H
#define COMMON_H

#define perr(mess) interractive_perror(mess, __FILE__, __LINE__)

enum app_usages {
        STANDARD,
        HELP,
        VERSION,
        ERROR
};

void error_handler(const char *message);
void interractive_perror(const char *message, const char *filename, int line_num);
size_t get_input(unsigned int length, char *string, bool not_null);
char multi_choice(const char *question, const char *choices, int no_choices);
struct gbn_config *init_configurations(void); 
double rand_double(void);

#endif