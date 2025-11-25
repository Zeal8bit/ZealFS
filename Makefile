SRCS=src/zealfs_main.c src/zealfs_v1.c src/zealfs_v2.c src/mbr.c
INCLUDE=include/
BIN=zealfs

CFLAGS=-I$(INCLUDE) -Wall `pkg-config fuse3 --cflags --libs`

all:
	$(CC) $(SRCS) -o $(BIN) $(CFLAGS)

clean:
	rm -f $(BIN)