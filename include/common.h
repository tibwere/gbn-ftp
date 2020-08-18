#ifndef COMMON_H
#define COMMON_H

enum app_usages {
        STANDARD,
        HELP,
        VERSION,
        ERROR
};

void error_handler(const char *message);
size_t get_input(unsigned int length, char *string, bool not_null);
char multi_choice(const char *question, const char *choices, int no_choices);
struct gbn_config *init_configurations(void); 
double rand_double(void);

#endif