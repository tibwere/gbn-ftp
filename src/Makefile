IDIR = ../include
CC = gcc
CFLAGS = -I$(IDIR) -Wall -Wextra
ODIR = ../obj
RDIR = ..
LIBS = -pthread
DEPS = ../include/common.h ../include/gbn.h  
S_OBJ = ../obj/server.o ../obj/common.o
SERVER = gbn-ftp-server

$(ODIR)/%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

$(SERVER): $(S_OBJ)
	$(CC) -o $(RDIR)/$@ $^ $(CFLAGS) $(LIBS)

.PHONY: clean

clean:
	rm -f $(ODIR)/*.o
