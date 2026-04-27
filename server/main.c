#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/select.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ncurses.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7890
#define BUFFER_SIZE 256 
#define MAX_CLIENTS 16 

#define MAX_PLAYERS 8
#define TICKS_PER_SECOND 20
#define MAX_NAME_LEN 15

#define LOG_LINES 16
#define LOG_LINE_LEN 96

#define CLIENT_ID_LEN 20
#define PLAYER_NAME_LEN 30
#define HEADER_LEN 3

typedef enum {
    GAME_LOBBY = 0,
    GAME_RUNNING = 1,
    GAME_END = 2
} game_status_t;

typedef enum {
    DIR_UP = 0,
    DIR_DOWN = 1,
    DIR_LEFT = 2,
    DIR_RIGHT = 3
} direction_t;

typedef enum {
    BONUS_NONE = 0,
    BONUS_SPEED = 1,
    BONUS_RADIUS = 2,
    BONUS_TIMER = 3
} bonus_type_t;

typedef enum {
    MSG_HELLO = 0,
    MSG_WELCOME = 1,
    MSG_DISCONNECT = 2,
    MSG_PING = 3,
    MSG_PONG = 4,
    MSG_LEAVE = 5,
    MSG_ERROR = 6,
    MSG_SET_READY = 10,
    MSG_SET_STATUS = 20,
    MSG_WINNER = 23,
    MSG_MOVE_ATTEMPT = 30,
    MSG_BOMB_ATTEMPT = 31,
    MSG_MOVED = 40,
    MSG_BOMB = 41,
    MSG_EXPLOSION_START = 42,
    MSG_EXPLOSION_END = 43,
    MSG_DEATH = 44,
    MSG_BONUS_AVAILABLE = 45,
    MSG_BONUS_RETRIEVED = 46,
    MSG_BLOCK_DESTROYED = 47,
    MSG_SYNC_BOARD = 100,
    MSG_SYNC_REQUEST = 101
} msg_type_t;

typedef struct {
    uint8_t id;
    uint8_t lives; 
    char name[MAX_NAME_LEN + 1];
    uint16_t row;
    uint16_t col;
    bool alive;
    bool ready;
    uint8_t bomb_count;
    uint8_t bomb_radius;
    uint16_t bomb_timer_ticks;
    uint16_t speed;
} player_t;

typedef struct {
    uint8_t owner_id;
    uint16_t row;
    uint16_t col;
    uint8_t radius;
    uint16_t timer_ticks;
} bomb_t;

static inline uint16_t make_cell_index(uint16_t row, uint16_t col, uint16_t cols) {
    return (uint16_t)(row * cols + col);
}

static inline void split_cell_index(uint16_t index, uint16_t cols, uint16_t *row, uint16_t *col) {
    *row = (uint16_t)(index / cols);
    *col = (uint16_t)(index % cols);
}

typedef struct {
    int fd;
    player_t p;
} client_t;

static int server_fd = -1;
static client_t clients[MAX_CLIENTS];
static int active_clients_count = 0;
static volatile sig_atomic_t running = 1;
static game_status_t current_game_state = GAME_LOBBY;

static char log_buf[LOG_LINES][LOG_LINE_LEN];
static int log_count = 0;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    int slot = log_count % LOG_LINES;
    va_start(ap, fmt);
    vsnprintf(log_buf[slot], LOG_LINE_LEN, fmt, ap);
    va_end(ap);
    log_count++;
}

static int send_error(int fd, uint8_t target_id, const char *err) {
    uint8_t buf[256];
    size_t max_payload = sizeof(buf) - HEADER_LEN - 2;
    size_t err_len = strlen(err);
    if (err_len > max_payload) err_len = max_payload;
    buf[0] = MSG_ERROR;
    buf[1] = 0xFF;
    buf[2] = target_id;
    buf[3] = (uint8_t)((err_len >> 8) & 0xFF);
    buf[4] = (uint8_t)(err_len & 0xFF);
    memcpy(buf + HEADER_LEN + 2, err, err_len);
    size_t total = HEADER_LEN + 2 + err_len;
    return (write(fd, buf, total) == (ssize_t)total) ? 0 : -1;
}

void cleanup(int sig) {
    (void)sig;
    printf("\nServer shutting down...\n");
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            close(clients[i].fd);
            clients[i].fd = -1;
        }
    }
    if (server_fd != -1) close(server_fd);
    endwin();
    exit(0);
}

int init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    clear();
    return 0;
}

void reset_client_data(int index) {
    clients[index].fd = -1;
    memset(&clients[index].p, 0, sizeof(player_t));
}

int process_message(int client_idx, int client_fd, char *buffer, ssize_t len) {
    if (len <= 0) return -1;
    if (len < 1) return -1;

    int cn = client_idx + 1;
    msg_type_t type = buffer[0];

    switch (type) {
        case MSG_HELLO: {
            if (len < HEADER_LEN + CLIENT_ID_LEN + PLAYER_NAME_LEN) {
                send_error(client_fd, 0xFF, "Malformed HELLO");
                log_msg("C%d <- ERROR (malformed HELLO)", cn);
                return 0;
            }
            char client_id[CLIENT_ID_LEN + 1] = {0};
            char player_name[PLAYER_NAME_LEN + 1] = {0};
            memcpy(client_id, buffer + HEADER_LEN, CLIENT_ID_LEN);
            memcpy(player_name, buffer + HEADER_LEN + CLIENT_ID_LEN, PLAYER_NAME_LEN);

            snprintf(clients[client_idx].p.name, MAX_NAME_LEN + 1, "%s", player_name);
            clients[client_idx].p.id = client_idx;
            clients[client_idx].p.alive = true;

            log_msg("C%d -> HELLO (id=%s, name=%s)", cn, client_id, clients[client_idx].p.name);

            char welcome_buf[256];
            snprintf(welcome_buf, sizeof(welcome_buf), "Welcome %s to Bomberman!", clients[client_idx].p.name);
            write(client_fd, welcome_buf, strlen(welcome_buf));
            log_msg("C%d <- WELCOME", cn);
            return 0;
        }

        case MSG_PING: {
            log_msg("C%d -> PING", cn);
            char pong_buf[256];
            snprintf(pong_buf, sizeof(pong_buf), "PONG");
            write(client_fd, pong_buf, strlen(pong_buf));
            log_msg("C%d <- PONG", cn);
            return 0;
        }

        case MSG_LEAVE: {
            log_msg("C%d -> LEAVE", cn);
            return 0;
        }

        case MSG_SET_READY: {
            log_msg("C%d -> SET_READY", cn);
            clients[client_idx].p.ready = true;
            return 0;
        }

        case MSG_MOVE_ATTEMPT: {
            log_msg("C%d -> MOVE_ATTEMPT", cn);
            char ack[64];
            snprintf(ack, sizeof(ack), "MSG_MOVED");
            write(client_fd, ack, strlen(ack));
            log_msg("C%d <- MOVED", cn);
            return 0;
        }

        case MSG_BOMB_ATTEMPT: {
            log_msg("C%d -> BOMB_ATTEMPT", cn);
            char ack[64];
            snprintf(ack, sizeof(ack), "MSG_BOMB");
            write(client_fd, ack, strlen(ack));
            log_msg("C%d <- BOMB", cn);
            return 0;
        }

        case MSG_SYNC_REQUEST: {
            log_msg("C%d -> SYNC_REQUEST", cn);
            if (current_game_state == GAME_RUNNING) {
                char sync_buf[1024];
                snprintf(sync_buf, sizeof(sync_buf), "SYNC_BOARD_DATA");
                write(client_fd, sync_buf, strlen(sync_buf));
                log_msg("C%d <- SYNC_BOARD", cn);
            } else {
                log_msg("C%d <- (sync ignored: not running)", cn);
            }
            return 0;
        }

        default:
            log_msg("C%d -> unknown (type=%d)", cn, (int)type);
            break;
    }
    return 0;
}

void draw_ui() {
    int y = 1, x = 1;

    erase();
    mvaddstr(y++, x, "=== Bomberman Server Status ===");
    mvaddstr(y++, x, "State: ");
    
    switch(current_game_state) {
        case GAME_LOBBY: 
            mvprintw(y++, x, "LOBBY [WAITING FOR PLAYERS]\n"); 
            break;
        case GAME_RUNNING: 
            mvprintw(y++, x, "RUNNING [GAME IN PROGRESS]\n"); 
            break;
        case GAME_END: 
            mvprintw(y++, x, "ENDED\n"); 
            break;
    }

    // Fixed mvaddstr usage: y, x, string (no %d inside string)
    mvprintw(y++, x, "Active Players: %d / %d", active_clients_count, MAX_PLAYERS);
    
    for (int i = 0; i < active_clients_count; i++) {
        if(clients[i].p.alive) {
            // Use %s for string, not %d. Handle potential null terminators if needed.
            mvprintw(y++, x, "- [%s] (Alive)", clients[i].p.name);
        } else {
            mvprintw(y++, x, "- [%s] (DEAD)", clients[i].p.name);
        }
    }

    y++;
    mvaddstr(y++, x, "Controls:");
    mvaddstr(y++, x, "  [q] Quit Server");

    y++;
    mvaddstr(y++, x, "=== Logs ===");
    int log_start = (log_count > LOG_LINES) ? (log_count - LOG_LINES) : 0;
    for (int i = log_start; i < log_count; i++) {
        mvprintw(y++, x, "  %s", log_buf[i % LOG_LINES]);
    }

    refresh();
}

int main(int argc, char *argv[]) {
    char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;

    if (argc == 2) {
        char *colon = strchr(argv[1], ':');
        if (colon) {
            *colon = '\0';        // null-terminate host at the colon
            host = argv[1];
            port = atoi(colon + 1);
        } else {
            host = argv[1];       // no port specified, use default
        }
    }
    else if (argc == 1) {
        // uses default values, continue
    }
    else {
        printf("Wrong server IP. Use ./server_bbm IP:PORT");
        exit(1);
    }

    printf("Starting Bomberman Server...\n");
    printf("Binding to: %s:%d\n", host, port);
    
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    init_ncurses();

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        perror("Invalid address"); return 1;
    }

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); return 1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen"); return 1;
    }

    printf("Bomberman Server listening on %s:%d\n", host, port);
    printf("Config: MaxPlayers=%d, TickRate=%dHz\n", MAX_PLAYERS, TICKS_PER_SECOND);

    while (running) {
        draw_ui();

        int ch = getch();
        if(ch == 'q') {
            cleanup(0);
        }
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        
        int max_fd = server_fd;
        for (int i = 0; i < active_clients_count; i++) {
            if (clients[i].fd != -1) {
                // Ensure we don't compare with -1
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
                FD_SET(clients[i].fd, &readfds);
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // Handle New Connection
        if (FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
            
            if (new_sock >= 0) {
                // Set non-blocking
                int flags = fcntl(new_sock, F_GETFL, 0);
                fcntl(new_sock, F_SETFL, flags | O_NONBLOCK);

                // Add to client list
                if (active_clients_count < MAX_CLIENTS) {
                    reset_client_data(active_clients_count);
                    clients[active_clients_count].fd = new_sock;

                    // Send initial request to identify client
                    char hello_req[64];
                    snprintf(hello_req, sizeof(hello_req), "HELLO_REQ");
                    write(new_sock, hello_req, strlen(hello_req));

                    active_clients_count++;
                    log_msg("Client %d connected.", active_clients_count);
                    log_msg("C%d <- HELLO_REQ", active_clients_count);
                } else {
                    close(new_sock);
                    log_msg("Connection refused: server full.");
                }
            }
        }

        // Handle Data from Clients
        for (int i = 0; i < active_clients_count; i++) {
            int sock = clients[i].fd;
            if (sock == -1) continue;

            if (FD_ISSET(sock, &readfds)) {
                char buffer[BUFFER_SIZE];
                ssize_t bytes_read = read(sock, buffer, sizeof(buffer) - 1);
                
                if (bytes_read > 0) {
                    process_message(i, sock, buffer, bytes_read);
                } else if (bytes_read == 0) {
                    // Client disconnected gracefully
                    log_msg("Client %d disconnected.", i + 1);
                    // Remove from array (shift elements left)
                    int j = i;
                    while (j < active_clients_count - 1) {
                        clients[j] = clients[j + 1];
                        j++;
                    }
                    active_clients_count--;
                    continue;
                } else {
                    // Error reading (broken pipe, etc.)
                    log_msg("Client %d connection error.", i + 1);
                    close(sock);
                    clients[i].fd = -1;
                    // Logic to handle disconnect in next iteration or immediate removal?
                    // Immediate removal logic is complex with shifting. 
                    // We will mark as closed and let the loop continue.
                    // To prevent out of bounds access in next iteration, we need to handle removal carefully.
                    // For simplicity, we'll just close and let the next iteration's cleanup logic handle it 
                    // OR we can do a simple decrement if it was the last one processed.
                    // Better approach: Mark as disconnected and remove immediately in this loop?
                    // Let's mark as disconnected and rely on the 'else' block to decrement count?
                    // No, standard pattern is: close socket, set fd=-1, then remove from list at end of loop or next iteration.
                    // To keep it simple and safe, we will just close and set -1. 
                    // We will handle the array shift in a dedicated cleanup pass if needed, 
                    // but for now, let's assume the 'else if (bytes_read == 0)' handles the clean removal.
                    // If read returns error (-1), we close and set -1.
                }
            }
        }
        
        // Cleanup logic: Shift active_clients_count down if any socket was marked -1 in this loop?
        // The previous loop handles 'bytes_read == 0' removal.
        // If read returned error, we set fd=-1. We should remove it to keep count accurate.
        int current_active = active_clients_count;
        int new_active = 0;
        for(int i=0; i<current_active; ++i) {
            if(clients[i].fd != -1) {
                clients[new_active++] = clients[i];
            }
        }
        active_clients_count = new_active;

        // If no activity, sleep briefly to reduce CPU usage
        if (ret == 0) {
             usleep(1000); 
        }
    }

    cleanup(0);
    return 0;
}
