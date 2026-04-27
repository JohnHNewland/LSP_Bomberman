all:
	gcc -std=c99 -lncurses -Wall -Wextra -pedantic -O3 -march=native client/main.c -o client_bbm && echo "client made" && gcc -std=c99 -Wall -Wextra -pedantic -O3 -march=native server/main.c -o server_bbm && echo "server made"
client:
	gcc -std=c99 -lncurses -Wall -Wextra -pedantic -O3 -march=native client/main.c -o client_bbm
server:
	gcc -std=c99 -lncurses -Wall -Wextra -pedantic -O3 -march=native server/main.c -o server_bbm
start_client:
	./client_bbm
start_server:
	./server_bbm
