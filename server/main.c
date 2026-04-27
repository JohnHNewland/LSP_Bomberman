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

#include "../common/level_config.h"
#include "../common/protocol.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7890
#define BUFFER_SIZE 8192
#define MAX_CLIENTS 16

#define LOG_LINES 16
#define LOG_LINE_LEN 96

#define SERVER_ID_STR "bbm-server-0.1"

typedef struct {
    int fd;
    player_t p;
} client_t;

static int server_fd = -1;
static client_t clients[MAX_CLIENTS];
static int active_clients_count = 0;
static volatile sig_atomic_t running = 1;
static game_status_t current_game_state = GAME_LOBBY;

static level_config_t level_cfg = {0};
static bool level_cfg_loaded = false;

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
    msg_generic_t hdr = { MSG_ERROR, ID_SERVER, target_id };
    memcpy(buf, &hdr, sizeof(hdr));
    buf[HEADER_LEN]     = (uint8_t)((err_len >> 8) & 0xFF);
    buf[HEADER_LEN + 1] = (uint8_t)(err_len & 0xFF);
    memcpy(buf + HEADER_LEN + 2, err, err_len);
    size_t total = HEADER_LEN + 2 + err_len;
    return (write(fd, buf, total) == (ssize_t)total) ? 0 : -1;
}

static int send_map_to(int fd, uint8_t target_id);

static int send_simple(int fd, uint8_t type, uint8_t target_id) {
    msg_generic_t m = { type, ID_SERVER, target_id };
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int send_set_status(int fd, uint8_t target_id, uint8_t game_status) {
    msg_set_status_t m = {0};
    m.hdr.msg_type  = MSG_SET_STATUS;
    m.hdr.sender_id = ID_SERVER;
    m.hdr.target_id = target_id;
    m.game_status   = game_status;
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int broadcast_simple(uint8_t type, uint8_t sender_id) {
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        msg_generic_t m = { type, sender_id, ID_BROADCAST };
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
}

static int broadcast_set_status(uint8_t game_status) {
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (send_set_status(clients[i].fd, ID_BROADCAST, game_status) == 0) {
            sent++;
        }
    }
    return sent;
}

static void check_match_start(void) {
    if (current_game_state != GAME_LOBBY) return;
    if (active_clients_count < 1) return;
    int ready_count = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd != -1 && clients[i].p.ready) ready_count++;
    }
    if (ready_count != active_clients_count) return;

    current_game_state = GAME_RUNNING;
    broadcast_set_status(GAME_RUNNING);
    log_msg("All %d player(s) ready - match started.", ready_count);
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (level_cfg_loaded) send_map_to(clients[i].fd, clients[i].p.id);
    }
}

static int send_welcome(int fd, uint8_t to_id) {
    msg_welcome_t w = {0};
    w.hdr.msg_type  = MSG_WELCOME;
    w.hdr.sender_id = to_id;
    w.hdr.target_id = to_id;
    snprintf(w.server_id, sizeof(w.server_id), "%s", SERVER_ID_STR);
    w.game_status = (uint8_t)current_game_state;

    uint8_t buf[sizeof(w) + MAX_PLAYERS * sizeof(msg_other_client_t)];
    size_t off = sizeof(w);
    uint8_t count = 0;
    for (int i = 0; i < active_clients_count && count < MAX_PLAYERS; i++) {
        if (clients[i].fd == -1 || clients[i].p.id == to_id) continue;
        msg_other_client_t oc = {0};
        oc.id = clients[i].p.id;
        oc.ready = clients[i].p.ready ? 1 : 0;
        snprintf(oc.name, sizeof(oc.name), "%s", clients[i].p.name);
        memcpy(buf + off, &oc, sizeof(oc));
        off += sizeof(oc);
        count++;
    }
    w.length = count;
    memcpy(buf, &w, sizeof(w));
    return (write(fd, buf, off) == (ssize_t)off) ? 0 : -1;
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
    level_config_free(&level_cfg);
    endwin();
    exit(0);
}

static int send_map_to(int fd, uint8_t target_id) {
    if (!level_cfg_loaded) return 0;
    size_t cells = (size_t)level_cfg.rows * level_cfg.cols;
    size_t total = sizeof(msg_map_t) + cells;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;
    msg_map_t m = {0};
    m.hdr.msg_type  = MSG_MAP;
    m.hdr.sender_id = ID_SERVER;
    m.hdr.target_id = target_id;
    m.H = level_cfg.rows;
    m.W = level_cfg.cols;
    memcpy(buf, &m, sizeof(m));
    for (size_t i = 0; i < cells; i++) {
        buf[sizeof(m) + i] = (uint8_t)level_cell_char(&level_cfg.cells[i]);
    }
    ssize_t w = write(fd, buf, total);
    free(buf);
    return (w == (ssize_t)total) ? 0 : -1;
}

static void broadcast_map(void) {
    if (!level_cfg_loaded) return;
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (send_map_to(clients[i].fd, 0xFE) == 0) sent++;
    }
    log_msg("MAP broadcast to %d client(s)", sent);
}

static void assign_player_starts(void) {
    if (!level_cfg_loaded) return;

    /* indexed 1..8: cell linear index + 1 (0 means slot empty) */
    int starts[LEVEL_MAX_PLAYERS + 1] = {0};
    for (int r = 0; r < level_cfg.rows; r++) {
        for (int c = 0; c < level_cfg.cols; c++) {
            cell_t *cell = level_cell_at(&level_cfg, r, c);
            if (cell->type == CELL_PLAYER_START &&
                cell->player_id >= 1 &&
                cell->player_id <= LEVEL_MAX_PLAYERS) {
                starts[cell->player_id] = r * level_cfg.cols + c + 1;
            }
        }
    }

    int next_slot = 1;
    for (int i = 0; i < active_clients_count; i++) {
        while (next_slot <= LEVEL_MAX_PLAYERS && starts[next_slot] == 0) {
            next_slot++;
        }
        if (next_slot > LEVEL_MAX_PLAYERS) {
            log_msg("No start position for client %d", i + 1);
            break;
        }
        int idx = starts[next_slot] - 1;
        clients[i].p.id = (uint8_t)next_slot;
        clients[i].p.row = (uint16_t)(idx / level_cfg.cols);
        clients[i].p.col = (uint16_t)(idx % level_cfg.cols);
        clients[i].p.alive = true;
        log_msg("Player %d -> start %d at (%u,%u)",
                i + 1, next_slot,
                clients[i].p.row, clients[i].p.col);
        next_slot++;
    }
}

static void load_level_dialog(void) {
    log_msg("Opening level picker...");

    level_entry_t entries[LEVEL_LIST_MAX];
    int n = level_list_dir(LEVEL_DIR, entries, LEVEL_LIST_MAX);
    if (n < 0) {
        log_msg("Cannot open '%s/' (cwd issue?)", LEVEL_DIR);
        return;
    }
    if (n == 0) {
        log_msg("No .map files found in '%s/'.", LEVEL_DIR);
        return;
    }

    nodelay(stdscr, FALSE);
    int sel = 0;
    while (1) {
        erase();
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        (void)cols;
        for (int i = 0; i < rows - 1; i++) mvaddch(i, 0, ' ');

        mvaddstr(1, 2, "+----------------- Pick Level Config -----------------+");
        mvprintw(2, 2, "| Folder: %-44s |", LEVEL_DIR);
        for (int i = 0; i < n; i++) {
            char marker = (i == sel) ? '>' : ' ';
            if (i == sel) attron(A_REVERSE | A_BOLD);
            mvprintw(4 + i, 2, "| %c %-50s |", marker, entries[i].name);
            if (i == sel) attroff(A_REVERSE | A_BOLD);
        }
        mvprintw(5 + n, 2,
                 "| [Up/Down] Move   [Enter] Select   [q/Esc] Cancel    |");
        mvaddstr(6 + n, 2,
                 "+-----------------------------------------------------+");
        refresh();

        int ch = getch();
        if (ch == KEY_UP)        sel = (sel - 1 + n) % n;
        else if (ch == KEY_DOWN) sel = (sel + 1) % n;
        else if (ch == 27 || ch == 'q' || ch == 'Q') {
            nodelay(stdscr, TRUE);
            log_msg("Level load cancelled.");
            return;
        }
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
    }
    nodelay(stdscr, TRUE);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", LEVEL_DIR, entries[sel].name);

    level_config_t tmp = {0};
    char err[96];
    if (level_config_load(path, &tmp, err, sizeof(err)) != 0) {
        log_msg("Level load failed (%s): %s", path, err);
        return;
    }
    level_config_free(&level_cfg);
    level_cfg = tmp;
    level_cfg_loaded = true;
    log_msg("Level loaded: %s (%ux%u, s=%u d=%u r=%u t=%u)",
            entries[sel].name, level_cfg.rows, level_cfg.cols,
            level_cfg.speed, level_cfg.danger_ticks, level_cfg.radius,
            level_cfg.bomb_timer_ticks);
    assign_player_starts();
    broadcast_map();
}

int init_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
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
            clients[client_idx].p.id = (uint8_t)(client_idx + 1);
            clients[client_idx].p.alive = true;

            log_msg("C%d -> HELLO (id=%s, name=%s)",
                    cn, client_id, clients[client_idx].p.name);

            uint8_t pid = clients[client_idx].p.id;
            if (send_welcome(client_fd, pid) == 0) {
                log_msg("C%d <- WELCOME (id=%u)", cn, pid);
            }

            if (level_cfg_loaded) {
                assign_player_starts();
                if (send_map_to(client_fd, pid) == 0) {
                    log_msg("C%d <- MAP (%ux%u)", cn,
                            level_cfg.rows, level_cfg.cols);
                }
            }
            return 0;
        }

        case MSG_PING: {
            log_msg("C%d -> PING", cn);
            send_simple(client_fd, MSG_PONG, clients[client_idx].p.id);
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
            broadcast_simple(MSG_SET_READY, clients[client_idx].p.id);
            check_match_start();
            return 0;
        }

        case MSG_MOVE_ATTEMPT: {
            log_msg("C%d -> MOVE_ATTEMPT", cn);
            return 0;
        }

        case MSG_BOMB_ATTEMPT: {
            log_msg("C%d -> BOMB_ATTEMPT", cn);
            return 0;
        }

        case MSG_SYNC_REQUEST: {
            log_msg("C%d -> SYNC_REQUEST", cn);
            if (current_game_state == GAME_RUNNING && level_cfg_loaded) {
                send_map_to(client_fd, clients[client_idx].p.id);
                log_msg("C%d <- MAP (sync)", cn);
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
        const char *life = clients[i].p.alive ? "Alive" : "DEAD";
        const char *ready = clients[i].p.ready ? "READY" : "not ready";
        mvprintw(y++, x, "- id=%u [%s] (%s, %s) @ (%u,%u)",
                 clients[i].p.id, clients[i].p.name, life, ready,
                 clients[i].p.row, clients[i].p.col);
    }

    y++;
    if (level_cfg_loaded) {
        mvprintw(y++, x, "Level: %ux%u  speed=%u  danger=%ut  radius=%u  bomb=%ut",
                 level_cfg.rows, level_cfg.cols, level_cfg.speed,
                 level_cfg.danger_ticks, level_cfg.radius,
                 level_cfg.bomb_timer_ticks);
    } else {
        mvaddstr(y++, x, "Level: (none loaded - press [l])");
    }

    y++;
    mvaddstr(y++, x, "Controls:");
    mvaddstr(y++, x, "  [l] Load level   [q] Quit Server");

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

    log_msg("Press [l] to load a level configuration file.");

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
        else if (ch == 'l' || ch == 'L') {
            load_level_dialog();
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

                if (active_clients_count < MAX_CLIENTS) {
                    reset_client_data(active_clients_count);
                    clients[active_clients_count].fd = new_sock;
                    active_clients_count++;
                    log_msg("Client %d connected (awaiting HELLO).",
                            active_clients_count);
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
