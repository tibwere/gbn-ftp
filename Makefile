IDIR = ./include
CC = gcc
CFLAGS = -I$(IDIR) -Wall -Wextra
ODIR = ./obj
BDIR = ./bin
SDIR = ./src
LIBS = -pthread
DEPS = ./include/common.h ./include/gbnftp.h
S_OBJ = ./obj/server.o ./obj/common.o ./obj/gbnftp.o
C_OBJ = ./obj/client.o ./obj/common.o ./obj/gbnftp.o
SERVER = gbn-ftp-server
CLIENT = gbn-ftp-client

$(ODIR)/%.o: $(SDIR)/%.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

all: $(SERVER) $(CLIENT)

$(SERVER): $(S_OBJ)
	$(CC) -o $(BDIR)/$@ $^ $(CFLAGS) $(LIBS)

$(CLIENT): $(C_OBJ)
	$(CC) -o $(BDIR)/$@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o
