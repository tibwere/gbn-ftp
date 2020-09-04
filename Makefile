# N.B. ESECUZIONE CON VALGRIND
# valgrind --leak-check=full --track-origins=yes --show-leak-kinds=all ./<prog> <options>

IDIR = ./include
CC = gcc
CFLAGS = -I$(IDIR) -Wall -Wextra -DDEBUG -ggdb3
ODIR = ./obj
BDIR = ./bin
SDIR = ./src
LIBS = -pthread -lm
DEPS = ./include/common.h ./include/gbnftp.h
SOBJ = ./obj/server.o 
COBJ = ./obj/client.o
OBJ = ./obj/common.o ./obj/gbnftp.o
SERVER = gbn-ftp-server
CLIENT = gbn-ftp-client

$(ODIR)/%.o: $(SDIR)/%.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: $(SERVER) $(CLIENT)

$(SERVER): $(SOBJ) $(OBJ)
	$(CC) -o $(BDIR)/$@ $^ $(CFLAGS) $(LIBS)

$(CLIENT): $(COBJ) $(OBJ)
	$(CC) -o $(BDIR)/$@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o
