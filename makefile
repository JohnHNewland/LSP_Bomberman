CFLAGS = -std=c99 -Wall -Wextra -pedantic -O3 -march=native
LDLIBS = -lncurses
COMMON = common/level_config.c common/protocol.c

all:
	gcc $(CFLAGS) -D_GNU_SOURCE client/main.c $(COMMON) -o client_bbm $(LDLIBS) && echo "client made" && gcc $(CFLAGS) server/main.c $(COMMON) -o server_bbm $(LDLIBS) && echo "server made"
create_client:
	gcc $(CFLAGS) -D_GNU_SOURCE client/main.c $(COMMON) -o client_bbm $(LDLIBS)
create_server:
	gcc $(CFLAGS) server/main.c $(COMMON) -o server_bbm $(LDLIBS)
start_client:
	./client_bbm
start_server:
	./server_bbm
