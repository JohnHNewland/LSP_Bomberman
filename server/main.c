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
#include <ifaddrs.h>
#include <netdb.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ncurses.h>

#include "../common/level_config.h"
#include "../common/protocol.h"

#define DEFAULT_HOST "0.0.0.0"
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
static level_config_t level_cfg_pristine = {0};
static bool level_cfg_loaded = false;

#define MAX_BOMBS 64
#define MAX_EXPLOSIONS 64
#define DEFAULT_BOMB_RADIUS 1
#define DEFAULT_BOMB_TIMER_TICKS 60
#define DEFAULT_DANGER_TICKS 10
#define DEFAULT_PLAYER_SPEED 4

typedef struct {
    bool     active;
    uint8_t  owner_id;
    uint16_t row, col;
    uint8_t  radius;
    uint16_t timer_ticks;
} server_bomb_t;

typedef struct {
    bool     active;
    uint16_t row, col;
    uint8_t  radius;
    uint16_t end_in_ticks;
} server_explosion_t;

static server_bomb_t      bombs[MAX_BOMBS];
static server_explosion_t explosions[MAX_EXPLOSIONS];

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

static int  send_map_to(int fd, uint8_t target_id);
static void broadcast_map(void);
static void assign_player_starts(void);
static void check_game_end(void);
static void check_match_start(void);
static void restore_level_from_pristine(void);

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

static int broadcast_bomb(uint8_t player_id, uint16_t cell) {
    msg_bomb_t m = {0};
    m.hdr.msg_type  = MSG_BOMB;
    m.hdr.sender_id = player_id;
    m.hdr.target_id = ID_BROADCAST;
    m.player_id     = player_id;
    m.cell          = htons(cell);
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
}

static int broadcast_explosion(uint8_t type, uint8_t radius, uint16_t cell) {
    msg_explosion_t m = {0};
    m.hdr.msg_type  = type;
    m.hdr.sender_id = ID_SERVER;
    m.hdr.target_id = ID_BROADCAST;
    m.radius        = radius;
    m.cell          = htons(cell);
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
}

static int broadcast_block_destroyed(uint16_t cell) {
    msg_block_destroyed_t m = {0};
    m.hdr.msg_type  = MSG_BLOCK_DESTROYED;
    m.hdr.sender_id = ID_SERVER;
    m.hdr.target_id = ID_BROADCAST;
    m.cell          = htons(cell);
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
}

static int broadcast_death(uint8_t player_id) {
    msg_death_t m = {0};
    m.hdr.msg_type  = MSG_DEATH;
    m.hdr.sender_id = ID_SERVER;
    m.hdr.target_id = ID_BROADCAST;
    m.player_id     = player_id;
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
}

static int broadcast_bonus_retrieved(uint8_t player_id, uint16_t cell) {
    msg_bonus_retrieved_t m = {0};
    m.hdr.msg_type  = MSG_BONUS_RETRIEVED;
    m.hdr.sender_id = player_id;
    m.hdr.target_id = ID_BROADCAST;
    m.player_id     = player_id;
    m.cell          = htons(cell);
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
}

static void apply_bonus(player_t *p, cell_type_t bonus) {
    switch (bonus) {
        case CELL_BONUS_SPEED:  p->speed++; break;
        case CELL_BONUS_RADIUS: p->bomb_radius++; break;
        case CELL_BONUS_TIMER:  p->bomb_timer_ticks += 10; break;
        case CELL_BONUS_BOMBS:  p->bomb_count++; break;
        default: break;
    }
}

static int count_bombs_owned(uint8_t player_id) {
    int n = 0;
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (bombs[i].active && bombs[i].owner_id == player_id) n++;
    }
    return n;
}

static int find_bomb_at(uint16_t row, uint16_t col) {
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (bombs[i].active && bombs[i].row == row && bombs[i].col == col) return i;
    }
    return -1;
}

static void kill_players_at(uint16_t row, uint16_t col) {
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (!clients[i].p.alive) continue;
        if (clients[i].p.row == row && clients[i].p.col == col) {
            clients[i].p.alive = false;
            broadcast_death(clients[i].p.id);
            log_msg("Player %u died at (%u,%u)",
                    clients[i].p.id, row, col);
        }
    }
}

static int add_explosion(uint16_t row, uint16_t col, uint8_t radius,
                         uint16_t danger) {
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (!explosions[i].active) {
            explosions[i].active = true;
            explosions[i].row = row;
            explosions[i].col = col;
            explosions[i].radius = radius;
            explosions[i].end_in_ticks = danger ? danger : 1;
            return i;
        }
    }
    return -1;
}

static void detonate_bomb(int idx) {
    if (idx < 0 || idx >= MAX_BOMBS) return;
    if (!bombs[idx].active) return;
    server_bomb_t b = bombs[idx];
    bombs[idx].active = false;

    cell_t *cell = level_cell_at(&level_cfg, b.row, b.col);
    if (cell->type == CELL_BOMB) cell->type = CELL_EMPTY;

    /* Send EXPLOSION_START before mutating the map so clients walk
     * the cross with the same cell state the server saw. */
    uint16_t bomb_cell = (uint16_t)(b.row * level_cfg.cols + b.col);
    broadcast_explosion(MSG_EXPLOSION_START, b.radius, bomb_cell);
    log_msg("Bomb detonated at (%u,%u) r=%u", b.row, b.col, b.radius);

    kill_players_at(b.row, b.col);

    static const int drs[] = {-1, 1, 0, 0};
    static const int dcs[] = { 0, 0,-1, 1};
    for (int d = 0; d < 4; d++) {
        for (int dist = 1; dist <= b.radius; dist++) {
            int r = (int)b.row + drs[d] * dist;
            int c = (int)b.col + dcs[d] * dist;
            if (r < 0 || r >= level_cfg.rows ||
                c < 0 || c >= level_cfg.cols) break;
            cell_t *t = level_cell_at(&level_cfg, r, c);
            if (t->type == CELL_HARD_BLOCK) break;

            int chain = find_bomb_at((uint16_t)r, (uint16_t)c);
            if (chain >= 0) bombs[chain].timer_ticks = 1;

            if (t->type == CELL_SOFT_BLOCK) {
                t->type = CELL_EMPTY;
                t->player_id = 0;
                broadcast_block_destroyed(
                    (uint16_t)(r * level_cfg.cols + c));
                break;  /* explosion stops at the soft block */
            }
            if (t->type == CELL_BONUS_SPEED ||
                t->type == CELL_BONUS_RADIUS ||
                t->type == CELL_BONUS_TIMER ||
                t->type == CELL_BONUS_BOMBS) {
                t->type = CELL_EMPTY;
                t->player_id = 0;
            }
            kill_players_at((uint16_t)r, (uint16_t)c);
        }
    }

    uint16_t danger = level_cfg.danger_ticks;
    if (danger == 0) danger = DEFAULT_DANGER_TICKS;
    add_explosion(b.row, b.col, b.radius, danger);
    check_game_end();
}

static void server_tick(void) {
    if (current_game_state != GAME_RUNNING) return;
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!bombs[i].active) continue;
        if (bombs[i].timer_ticks > 0) bombs[i].timer_ticks--;
        if (bombs[i].timer_ticks == 0) detonate_bomb(i);
    }
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (!explosions[i].active) continue;
        if (explosions[i].end_in_ticks > 0) explosions[i].end_in_ticks--;
        if (explosions[i].end_in_ticks == 0) {
            uint16_t cell = (uint16_t)(explosions[i].row * level_cfg.cols
                                       + explosions[i].col);
            broadcast_explosion(MSG_EXPLOSION_END,
                                explosions[i].radius, cell);
            explosions[i].active = false;
        }
    }
}

static int broadcast_moved(uint8_t player_id, uint16_t coord) {
    msg_moved_t m = {0};
    m.hdr.msg_type  = MSG_MOVED;
    m.hdr.sender_id = player_id;
    m.hdr.target_id = ID_BROADCAST;
    m.player_id     = player_id;
    m.coord         = htons(coord);
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
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

static int broadcast_winner(uint8_t winner_id) {
    msg_winner_t m = {0};
    m.hdr.msg_type  = MSG_WINNER;
    m.hdr.sender_id = ID_SERVER;
    m.hdr.target_id = ID_BROADCAST;
    m.winner_id     = winner_id;
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
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

static void end_game(uint8_t winner_id) {
    if (current_game_state == GAME_END) return;
    current_game_state = GAME_END;
    for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = false;
    for (int i = 0; i < MAX_EXPLOSIONS; i++) explosions[i].active = false;
    broadcast_set_status(GAME_END);
    broadcast_winner(winner_id);
    if (winner_id == ID_UNKNOWN)
        log_msg("Game ended: draw (no survivors)");
    else
        log_msg("Game ended. Winner: id=%u", winner_id);
}

static void check_game_end(void) {
    if (current_game_state != GAME_RUNNING) return;
    int alive = 0;
    uint8_t winner = ID_UNKNOWN;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (clients[i].p.alive) {
            alive++;
            winner = clients[i].p.id;
        }
    }
    if (alive <= 1) end_game(alive == 1 ? winner : ID_UNKNOWN);
}

static void return_to_lobby(void) {
    if (current_game_state == GAME_LOBBY) return;
    current_game_state = GAME_LOBBY;
    for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = false;
    for (int i = 0; i < MAX_EXPLOSIONS; i++) explosions[i].active = false;
    for (int i = 0; i < active_clients_count; i++) {
        clients[i].p.alive = true;
        clients[i].p.ready = false;
    }
    if (level_cfg_loaded) {
        restore_level_from_pristine();
        assign_player_starts();
    }
    broadcast_set_status(GAME_LOBBY);
    if (level_cfg_loaded) broadcast_map();
    log_msg("Returned to lobby (map reset).");
}

static void check_match_start(void) {
    if (current_game_state != GAME_LOBBY) return;
    if (active_clients_count < 2) return;
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
    level_config_free(&level_cfg_pristine);
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

static int level_cfg_clone(level_config_t *dst, const level_config_t *src) {
    if (!dst || !src || !src->cells) return -1;
    size_t total = (size_t)src->rows * src->cols;
    free(dst->cells);
    dst->cells = (cell_t *)calloc(total, sizeof(cell_t));
    if (!dst->cells) { dst->rows = dst->cols = 0; return -1; }
    memcpy(dst->cells, src->cells, total * sizeof(cell_t));
    dst->rows             = src->rows;
    dst->cols             = src->cols;
    dst->speed            = src->speed;
    dst->danger_ticks     = src->danger_ticks;
    dst->radius           = src->radius;
    dst->bomb_timer_ticks = src->bomb_timer_ticks;
    return 0;
}

static void restore_level_from_pristine(void) {
    if (!level_cfg_pristine.cells || !level_cfg.cells) return;
    if (level_cfg_pristine.rows != level_cfg.rows ||
        level_cfg_pristine.cols != level_cfg.cols) return;
    size_t total = (size_t)level_cfg.rows * level_cfg.cols;
    memcpy(level_cfg.cells, level_cfg_pristine.cells,
           total * sizeof(cell_t));
}

static int count_player_starts(const level_config_t *cfg) {
    if (!cfg || !cfg->cells) return 0;
    int n = 0;
    size_t total = (size_t)cfg->rows * cfg->cols;
    for (size_t i = 0; i < total; i++) {
        if (cfg->cells[i].type == CELL_PLAYER_START) n++;
    }
    return n;
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
        clients[i].p.bomb_count       = 1;
        clients[i].p.bomb_radius      =
            level_cfg.radius ? level_cfg.radius : DEFAULT_BOMB_RADIUS;
        clients[i].p.bomb_timer_ticks =
            level_cfg.bomb_timer_ticks ? level_cfg.bomb_timer_ticks
                                       : DEFAULT_BOMB_TIMER_TICKS;
        clients[i].p.speed            =
            level_cfg.speed ? level_cfg.speed : DEFAULT_PLAYER_SPEED;
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
    int starts = count_player_starts(&tmp);
    if (starts < active_clients_count) {
        log_msg("Level only fits %d player(s) - %d connected. Cancelled.",
                starts, active_clients_count);
        level_config_free(&tmp);
        return;
    }
    level_config_free(&level_cfg);
    level_cfg = tmp;
    level_cfg_loaded = true;
    level_config_free(&level_cfg_pristine);
    level_cfg_clone(&level_cfg_pristine, &level_cfg);
    log_msg("Level loaded: %s (%ux%u, max=%d, s=%u d=%u r=%u t=%u)",
            entries[sel].name, level_cfg.rows, level_cfg.cols, starts,
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

            if (level_cfg_loaded) {
                int cap = count_player_starts(&level_cfg);
                if (active_clients_count > cap) {
                    send_error(client_fd, ID_UNKNOWN, "Server full for this map");
                    return -1;
                }
            }
            snprintf(clients[client_idx].p.name, MAX_NAME_LEN + 1, "%s", player_name);
            clients[client_idx].p.id = (uint8_t)(client_idx + 1);
            clients[client_idx].p.alive = true;

            log_msg("C%d -> HELLO (id=%s, name=%s)",
                    cn, client_id, clients[client_idx].p.name);

            uint8_t pid = clients[client_idx].p.id;

            /* Forward HELLO to other clients so they learn about the new
             * player (per the protocol's "pārsūtāma" property). */
            msg_hello_t fwd = {0};
            fwd.hdr.msg_type  = MSG_HELLO;
            fwd.hdr.sender_id = pid;
            fwd.hdr.target_id = ID_BROADCAST;
            memcpy(fwd.client_id,   client_id,   CLIENT_ID_LEN);
            memcpy(fwd.player_name, player_name, PLAYER_NAME_LEN);
            for (int i = 0; i < active_clients_count; i++) {
                if (i == client_idx) continue;
                if (clients[i].fd == -1) continue;
                write(clients[i].fd, &fwd, sizeof(fwd));
            }

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

        case MSG_SET_STATUS: {
            if (len < (ssize_t)sizeof(msg_set_status_t)) {
                send_error(client_fd, clients[client_idx].p.id,
                           "Bad SET_STATUS");
                return 0;
            }
            const msg_set_status_t *s =
                (const msg_set_status_t *)buffer;
            log_msg("C%d -> SET_STATUS %u", cn, s->game_status);
            if (s->game_status == GAME_LOBBY &&
                current_game_state == GAME_END) {
                return_to_lobby();
            } else {
                send_error(client_fd, clients[client_idx].p.id,
                           "Status change not allowed");
            }
            return 0;
        }

        case MSG_MOVE_ATTEMPT: {
            if (len < (ssize_t)sizeof(msg_move_attempt_t)) {
                send_error(client_fd, clients[client_idx].p.id,
                           "Bad MOVE_ATTEMPT");
                return 0;
            }
            if (current_game_state != GAME_RUNNING || !level_cfg_loaded) {
                send_error(client_fd, clients[client_idx].p.id,
                           "Game not running");
                return 0;
            }
            if (!clients[client_idx].p.alive) {
                send_error(client_fd, clients[client_idx].p.id,
                           "Dead players can't move");
                return 0;
            }
            const msg_move_attempt_t *ma =
                (const msg_move_attempt_t *)buffer;
            int dr = 0, dc = 0;
            switch ((char)ma->direction) {
                case DIR_UP:    dr = -1; break;
                case DIR_DOWN:  dr =  1; break;
                case DIR_LEFT:  dc = -1; break;
                case DIR_RIGHT: dc =  1; break;
                default:
                    send_error(client_fd, clients[client_idx].p.id,
                               "Bad direction");
                    return 0;
            }
            int nr = (int)clients[client_idx].p.row + dr;
            int nc = (int)clients[client_idx].p.col + dc;
            if (nr < 0 || nr >= level_cfg.rows ||
                nc < 0 || nc >= level_cfg.cols) {
                send_error(client_fd, clients[client_idx].p.id,
                           "Out of bounds");
                return 0;
            }
            cell_t *target = level_cell_at(&level_cfg, nr, nc);
            if (target->type == CELL_HARD_BLOCK ||
                target->type == CELL_SOFT_BLOCK ||
                target->type == CELL_BOMB) {
                send_error(client_fd, clients[client_idx].p.id, "Blocked");
                return 0;
            }
            for (int i = 0; i < active_clients_count; i++) {
                if (i == client_idx) continue;
                if (clients[i].fd == -1 || !clients[i].p.alive) continue;
                if (clients[i].p.row == (uint16_t)nr &&
                    clients[i].p.col == (uint16_t)nc) {
                    send_error(client_fd, clients[client_idx].p.id,
                               "Player there");
                    return 0;
                }
            }
            clients[client_idx].p.row = (uint16_t)nr;
            clients[client_idx].p.col = (uint16_t)nc;
            uint16_t coord =
                (uint16_t)(nr * level_cfg.cols + nc);
            broadcast_moved(clients[client_idx].p.id, coord);
            if (target->type == CELL_BONUS_SPEED ||
                target->type == CELL_BONUS_RADIUS ||
                target->type == CELL_BONUS_TIMER ||
                target->type == CELL_BONUS_BOMBS) {
                cell_type_t kind = target->type;
                target->type = CELL_EMPTY;
                target->player_id = 0;
                apply_bonus(&clients[client_idx].p, kind);
                broadcast_bonus_retrieved(clients[client_idx].p.id, coord);
                log_msg("C%d picked up bonus at (%d,%d) (%s)",
                        cn, nr, nc,
                        kind == CELL_BONUS_SPEED  ? "speed"  :
                        kind == CELL_BONUS_RADIUS ? "radius" :
                        kind == CELL_BONUS_TIMER  ? "timer"  : "bombs");
            }
            log_msg("C%d MOVED to (%d,%d)", cn, nr, nc);
            return 0;
        }

        case MSG_BOMB_ATTEMPT: {
            if (current_game_state != GAME_RUNNING || !level_cfg_loaded) {
                send_error(client_fd, clients[client_idx].p.id,
                           "Game not running");
                return 0;
            }
            player_t *p = &clients[client_idx].p;
            if (!p->alive) {
                send_error(client_fd, p->id, "Dead players cannot bomb");
                return 0;
            }
            if (count_bombs_owned(p->id) >= p->bomb_count) {
                send_error(client_fd, p->id, "No bombs available");
                return 0;
            }
            if (find_bomb_at(p->row, p->col) >= 0) {
                send_error(client_fd, p->id, "Bomb already here");
                return 0;
            }
            int slot = -1;
            for (int i = 0; i < MAX_BOMBS; i++) {
                if (!bombs[i].active) { slot = i; break; }
            }
            if (slot < 0) {
                send_error(client_fd, p->id, "Bomb pool full");
                return 0;
            }
            bombs[slot].active        = true;
            bombs[slot].owner_id      = p->id;
            bombs[slot].row           = p->row;
            bombs[slot].col           = p->col;
            bombs[slot].radius        = p->bomb_radius;
            bombs[slot].timer_ticks   = p->bomb_timer_ticks;

            cell_t *cell = level_cell_at(&level_cfg, p->row, p->col);
            cell->type = CELL_BOMB;

            uint16_t coord = (uint16_t)(p->row * level_cfg.cols + p->col);
            broadcast_bomb(p->id, coord);
            log_msg("C%d -> BOMB at (%u,%u) r=%u t=%u",
                    cn, p->row, p->col, p->bomb_radius, p->bomb_timer_ticks);
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

    log_msg("Listening on %s:%d (port %d)", host, port, port);
    if (strcmp(host, "0.0.0.0") == 0 || strcmp(host, "::") == 0) {
        struct ifaddrs *ifa = NULL;
        if (getifaddrs(&ifa) == 0) {
            for (struct ifaddrs *p = ifa; p != NULL; p = p->ifa_next) {
                if (!p->ifa_addr) continue;
                if (p->ifa_addr->sa_family != AF_INET) continue;
                char ip[INET_ADDRSTRLEN] = {0};
                struct sockaddr_in *sin =
                    (struct sockaddr_in *)p->ifa_addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                log_msg("  %s -> %s:%d", p->ifa_name, ip, port);
            }
            freeifaddrs(ifa);
        }
        log_msg("Forward TCP port %d on your router for internet play.",
                port);
    }

    struct timespec last_tick;
    clock_gettime(CLOCK_MONOTONIC, &last_tick);
    const long tick_ms = 1000 / TICKS_PER_SECOND;

    while (running) {
        draw_ui();

        int ch = getch();
        if(ch == 'q') {
            cleanup(0);
        }
        else if (ch == 'l' || ch == 'L') {
            load_level_dialog();
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_tick.tv_sec) * 1000
                        + (now.tv_nsec - last_tick.tv_nsec) / 1000000;
        while (elapsed_ms >= tick_ms) {
            server_tick();
            last_tick.tv_nsec += tick_ms * 1000000;
            if (last_tick.tv_nsec >= 1000000000) {
                last_tick.tv_sec++;
                last_tick.tv_nsec -= 1000000000;
            }
            elapsed_ms -= tick_ms;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);

        int max_fd = server_fd;
        for (int i = 0; i < active_clients_count; i++) {
            if (clients[i].fd != -1) {
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
                FD_SET(clients[i].fd, &readfds);
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = (suseconds_t)(tick_ms * 1000);

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

                int cap = MAX_CLIENTS;
                if (level_cfg_loaded) {
                    int map_cap = count_player_starts(&level_cfg);
                    if (map_cap < cap) cap = map_cap;
                }
                if (active_clients_count < cap) {
                    reset_client_data(active_clients_count);
                    clients[active_clients_count].fd = new_sock;
                    active_clients_count++;
                    log_msg("Client %d connected (awaiting HELLO).",
                            active_clients_count);
                } else {
                    close(new_sock);
                    log_msg("Connection refused: %s",
                            level_cfg_loaded ? "map is full" : "server full");
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
                    log_msg("Client %d disconnected.", i + 1);
                    uint8_t leaving = clients[i].p.id;
                    if (leaving != 0)
                        broadcast_simple(MSG_LEAVE, leaving);
                    int j = i;
                    while (j < active_clients_count - 1) {
                        clients[j] = clients[j + 1];
                        j++;
                    }
                    active_clients_count--;
                    check_match_start();
                    check_game_end();
                    continue;
                } else {
                    log_msg("Client %d connection error.", i + 1);
                    uint8_t leaving = clients[i].p.id;
                    if (leaving != 0)
                        broadcast_simple(MSG_LEAVE, leaving);
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
