CFLAGS=-Wall `pkg-config fuse3 --cflags --libs`
SRCS=src/zealfs_fuse.c
BIN=zealfs

all:
	$(CC) $(SRCS) -o $(BIN) $(CFLAGS)

clean:
	rm -f $(BIN)