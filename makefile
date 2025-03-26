CC = gcc
CFLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -O2
DEPS = udp_test.h

all: udp_server udp_client

udp_server: udp_server.c $(DEPS)
	$(CC) $(CFLAGS) -o udp_server udp_server.c

udp_client: udp_client.c $(DEPS)
	$(CC) $(CFLAGS) -o udp_client udp_client.c

clean:
	rm -f udp_server udp_client

.PHONY: all clean
