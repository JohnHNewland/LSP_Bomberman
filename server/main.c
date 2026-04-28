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
    int      fd;
    player_t p;
    time_t   last_recv_at;
    /* When non-zero, holds the time we sent a PING that has not yet been
     * answered by any incoming traffic. Cleared on every read. Per
     * protokols.docx, no PONG within 30 s of a PING is the only timeout
     * condition — silence alone is not. */
    time_t   pong_pending_since;
} client_t;

static int server_fd = -1;
static client_t clients[MAX_CLIENTS];
static int active_clients_count = 0;
static volatile sig_atomic_t running = 1;
static game_status_t current_game_state = GAME_LOBBY;
/* Winner of the most recent finished match. ID_UNKNOWN means draw or
 * never-played. Kept across the GAME_END screen so a HELLO that arrives
 * mid-end-screen can be answered with WINNER per the spec's WELCOME body
 * ("ja statuss ir 2, tad serverim vajadzētu nosūtīt WINNER ziņu"). */
static uint8_t last_winner_id = ID_UNKNOWN;

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
    uint16_t danger_ticks;
} server_bomb_t;

typedef struct {
    bool     active;
    uint8_t  owner_id;
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
static void place_players_at_starts(void);
static void place_one_player_at_start(int client_idx);
static uint8_t pick_free_player_id(void);
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

static int broadcast_explosion(uint8_t type, uint8_t owner_id,
                               uint8_t radius, uint16_t cell) {
    msg_explosion_t m = {0};
    m.hdr.msg_type  = type;
    m.hdr.sender_id = owner_id;
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

static int broadcast_death(uint8_t killer_id, uint8_t player_id) {
    msg_death_t m = {0};
    m.hdr.msg_type  = MSG_DEATH;
    /* sender_id carries the killer (bomb owner). When the death is from
     * walking into a stale explosion whose origin we lost, killer_id is
     * passed as the victim's own id, which the client renders as
     * "self-inflicted / unknown killer". */
    m.hdr.sender_id = killer_id;
    m.hdr.target_id = ID_BROADCAST;
    m.player_id     = player_id;
    int sent = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (write(clients[i].fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) sent++;
    }
    return sent;
}

static void fill_sync_board(msg_sync_board_t *m, const player_t *p) {
    m->hdr.msg_type  = MSG_SYNC_BOARD;
    m->hdr.sender_id = ID_SERVER;
    m->hdr.target_id = ID_BROADCAST;
    m->player_id          = p->id;
    m->alive              = p->alive ? 1 : 0;
    m->lives              = p->lives;
    m->row                = htons(p->row);
    m->col                = htons(p->col);
    m->bomb_count         = p->bomb_count;
    m->bomb_radius        = p->bomb_radius;
    m->bomb_timer_ticks   = htons(p->bomb_timer_ticks);
    m->speed              = htons(p->speed);
}

static int send_sync_board_to(int fd, uint8_t target_id, const player_t *p) {
    msg_sync_board_t m = {0};
    fill_sync_board(&m, p);
    m.hdr.target_id = target_id;
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static void broadcast_sync_board_for(const player_t *p) {
    msg_sync_board_t m = {0};
    fill_sync_board(&m, p);
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        write(clients[i].fd, &m, sizeof(m));
    }
}

static void broadcast_sync_board_all(void) {
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        broadcast_sync_board_for(&clients[i].p);
    }
}

static void send_sync_board_all_to(int fd, uint8_t target_id) {
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        send_sync_board_to(fd, target_id, &clients[i].p);
    }
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
        /* TIMER bonus extends how long the explosion lingers (danger
         * window) by 10 ticks, applied when this player's bomb detonates. */
        case CELL_BONUS_TIMER:  p->danger_extra_ticks += 10; break;
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

static void kill_players_at(uint8_t killer_id, uint16_t row, uint16_t col) {
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (!clients[i].p.alive) continue;
        if (clients[i].p.row == row && clients[i].p.col == col) {
            clients[i].p.alive = false;
            broadcast_death(killer_id, clients[i].p.id);
            log_msg("Player %u died at (%u,%u) (killer=%u)",
                    clients[i].p.id, row, col, killer_id);
        }
    }
}

static int find_explosion_owner_at(uint16_t row, uint16_t col) {
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (!explosions[i].active) continue;
        if (explosions[i].row == row && explosions[i].col == col)
            return explosions[i].owner_id;
        /* Cells in the cross (not the center) are not stored individually,
         * so a walk-into kill on a cross arm will not resolve here. The
         * caller falls back to victim-as-killer in that case. */
    }
    return -1;
}

static int add_explosion(uint8_t owner_id, uint16_t row, uint16_t col,
                         uint8_t radius, uint16_t danger) {
    for (int i = 0; i < MAX_EXPLOSIONS; i++) {
        if (!explosions[i].active) {
            explosions[i].active = true;
            explosions[i].owner_id = owner_id;
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

    /* Send EXPLOSION_START before mutating the map so clients walk
     * the cross with the same cell state the server saw. */
    uint16_t bomb_cell = make_cell_index(b.row, b.col, level_cfg.cols);
    broadcast_explosion(MSG_EXPLOSION_START, b.owner_id, b.radius, bomb_cell);
    log_msg("Bomb detonated at (%u,%u) r=%u", b.row, b.col, b.radius);

    /* Mark the bomb's own cell as a danger zone for the duration of the
     * explosion so a player walking onto it (or already standing on it)
     * dies — see MOVE_ATTEMPT's CELL_EXPLOSION check. */
    cell_t *cell = level_cell_at(&level_cfg, b.row, b.col);
    cell->type = CELL_EXPLOSION;
    cell->player_id = 0;

    kill_players_at(b.owner_id, b.row, b.col);

    static const uint8_t dirs[] = { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
    for (int d = 0; d < 4; d++) {
        int dr, dc;
        dir_delta(dirs[d], &dr, &dc);
        for (int dist = 1; dist <= b.radius; dist++) {
            int r = (int)b.row + dr * dist;
            int c = (int)b.col + dc * dist;
            if (r < 0 || r >= level_cfg.rows ||
                c < 0 || c >= level_cfg.cols) break;
            cell_t *t = level_cell_at(&level_cfg, r, c);
            if (t->type == CELL_HARD_BLOCK) break;

            int chain = find_bomb_at((uint16_t)r, (uint16_t)c);
            if (chain >= 0) bombs[chain].timer_ticks = 1;

            bool was_soft = (t->type == CELL_SOFT_BLOCK);
            if (was_soft) {
                broadcast_block_destroyed(
                    make_cell_index((uint16_t)r, (uint16_t)c, level_cfg.cols));
            }
            /* Overwrite the cell with the live explosion. Soft blocks,
             * bonuses, and empty cells all become danger zones; bombs at
             * the cell are handled via chain detonation above. */
            t->type = CELL_EXPLOSION;
            t->player_id = 0;
            kill_players_at(b.owner_id, (uint16_t)r, (uint16_t)c);
            if (was_soft) break;  /* explosion stops at the soft block */
        }
    }

    uint16_t danger = b.danger_ticks;
    if (danger == 0) danger = DEFAULT_DANGER_TICKS;
    add_explosion(b.owner_id, b.row, b.col, b.radius, danger);
    check_game_end();
}

/* Walks the same cross as detonate_bomb and clears any cell still tagged
 * CELL_EXPLOSION back to CELL_EMPTY when the danger period ends. Stops at
 * the first non-explosion cell (overlapping explosions may have already
 * cleared it). */
static void clear_explosion_cells(uint16_t row, uint16_t col, uint8_t radius) {
    cell_t *center = level_cell_at(&level_cfg, row, col);
    if (center->type == CELL_EXPLOSION) {
        center->type = CELL_EMPTY;
        center->player_id = 0;
    }
    static const uint8_t dirs[] = { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
    for (int d = 0; d < 4; d++) {
        int dr, dc;
        dir_delta(dirs[d], &dr, &dc);
        for (int dist = 1; dist <= radius; dist++) {
            int r = (int)row + dr * dist;
            int c = (int)col + dc * dist;
            if (r < 0 || r >= level_cfg.rows ||
                c < 0 || c >= level_cfg.cols) break;
            cell_t *t = level_cell_at(&level_cfg, r, c);
            if (t->type != CELL_EXPLOSION) break;
            t->type = CELL_EMPTY;
            t->player_id = 0;
        }
    }
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
            uint16_t cell = make_cell_index(explosions[i].row,
                                            explosions[i].col,
                                            level_cfg.cols);
            clear_explosion_cells(explosions[i].row, explosions[i].col,
                                  explosions[i].radius);
            broadcast_explosion(MSG_EXPLOSION_END,
                                explosions[i].owner_id,
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

static int send_winner_to(int fd, uint8_t target_id, uint8_t winner_id) {
    msg_winner_t m = {0};
    m.hdr.msg_type  = MSG_WINNER;
    m.hdr.sender_id = ID_SERVER;
    m.hdr.target_id = target_id;
    m.winner_id     = winner_id;
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
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
    last_winner_id = winner_id;
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
    last_winner_id = ID_UNKNOWN;
    for (int i = 0; i < MAX_BOMBS; i++) bombs[i].active = false;
    for (int i = 0; i < MAX_EXPLOSIONS; i++) explosions[i].active = false;
    for (int i = 0; i < active_clients_count; i++) {
        clients[i].p.alive = true;
        clients[i].p.ready = false;
    }
    if (level_cfg_loaded) {
        restore_level_from_pristine();
        place_players_at_starts();
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

    /* Refuse to enter RUNNING without a level — clients would otherwise
     * receive SET_STATUS=1 and immediately go off the rails since they
     * have no map. The host needs to load a level first. */
    if (!level_cfg_loaded) {
        for (int i = 0; i < active_clients_count; i++) {
            if (clients[i].fd == -1) continue;
            send_error(clients[i].fd, clients[i].p.id,
                       "Host has not loaded a level yet");
        }
        log_msg("All ready, but no level loaded - match start blocked.");
        return;
    }

    current_game_state = GAME_RUNNING;
    broadcast_set_status(GAME_RUNNING);
    log_msg("All %d player(s) ready - match started.", ready_count);
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        if (level_cfg_loaded) send_map_to(clients[i].fd, clients[i].p.id);
    }
    /* Send a SYNC_BOARD for every player so all clients (including any
     * late joiner) have authoritative positions and stats. */
    broadcast_sync_board_all();
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

static void send_disconnect(int fd, uint8_t target_id) {
    msg_generic_t m = { MSG_DISCONNECT, ID_SERVER, target_id };
    write(fd, &m, sizeof(m));
}

void cleanup(int sig) {
    (void)sig;
    printf("\nServer shutting down...\n");

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            send_disconnect(clients[i].fd, clients[i].p.id);
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

/* Map ID (1..LEVEL_MAX_PLAYERS) to its start cell (linear index + 1, or
 * 0 if that slot doesn't exist on the current map). */
static void compute_start_positions(int starts[LEVEL_MAX_PLAYERS + 1]) {
    for (int i = 0; i <= LEVEL_MAX_PLAYERS; i++) starts[i] = 0;
    if (!level_cfg_loaded) return;
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
}

/* Pick the lowest player ID (1..LEVEL_MAX_PLAYERS) that is free among the
 * currently connected clients. With a level loaded, restrict the choice
 * to IDs that actually have a start cell on that map. Returns 0 if no
 * slot is available — caller should refuse the connection. */
static uint8_t pick_free_player_id(void) {
    bool taken[LEVEL_MAX_PLAYERS + 2] = {false};
    for (int i = 0; i < active_clients_count; i++) {
        uint8_t id = clients[i].p.id;
        if (id >= 1 && id <= LEVEL_MAX_PLAYERS) taken[id] = true;
    }
    if (level_cfg_loaded) {
        int starts[LEVEL_MAX_PLAYERS + 1];
        compute_start_positions(starts);
        for (uint8_t id = 1; id <= LEVEL_MAX_PLAYERS; id++) {
            if (starts[id] && !taken[id]) return id;
        }
        return 0;
    }
    for (uint8_t id = 1; id <= LEVEL_MAX_PLAYERS; id++) {
        if (!taken[id]) return id;
    }
    return 0;
}

static void apply_start_defaults(player_t *p, int idx) {
    p->row = (uint16_t)(idx / level_cfg.cols);
    p->col = (uint16_t)(idx % level_cfg.cols);
    p->alive = true;
    p->bomb_count       = 1;
    p->bomb_radius      =
        level_cfg.radius ? level_cfg.radius : DEFAULT_BOMB_RADIUS;
    p->bomb_timer_ticks =
        level_cfg.bomb_timer_ticks ? level_cfg.bomb_timer_ticks
                                   : DEFAULT_BOMB_TIMER_TICKS;
    p->speed            =
        level_cfg.speed ? level_cfg.speed : DEFAULT_PLAYER_SPEED;
    p->danger_extra_ticks = 0;
}

/* Place every existing client at the start cell that matches their
 * already-assigned p.id. Used at game start, on level load, and on
 * return-to-lobby. Never modifies p.id — IDs are stable for the life of
 * the connection. */
static void place_players_at_starts(void) {
    if (!level_cfg_loaded) return;
    int starts[LEVEL_MAX_PLAYERS + 1];
    compute_start_positions(starts);
    for (int i = 0; i < active_clients_count; i++) {
        uint8_t id = clients[i].p.id;
        if (id < 1 || id > LEVEL_MAX_PLAYERS) continue;
        if (starts[id] == 0) {
            log_msg("Player %u has no start on this map.", id);
            continue;
        }
        apply_start_defaults(&clients[i].p, starts[id] - 1);
        log_msg("Player %u -> start at (%u,%u)",
                id, clients[i].p.row, clients[i].p.col);
    }
}

/* Position one freshly-joined client at the start cell for their already
 * picked p.id. */
static void place_one_player_at_start(int client_idx) {
    if (!level_cfg_loaded) return;
    int starts[LEVEL_MAX_PLAYERS + 1];
    compute_start_positions(starts);
    uint8_t id = clients[client_idx].p.id;
    if (id < 1 || id > LEVEL_MAX_PLAYERS) return;
    if (starts[id] == 0) return;
    apply_start_defaults(&clients[client_idx].p, starts[id] - 1);
    log_msg("Player %u -> start at (%u,%u)",
            id, clients[client_idx].p.row, clients[client_idx].p.col);
}

/* Load and activate the map at entries[idx]. Returns 0 on success and
 * fills *out_name with the basename. On failure returns -1 and writes a
 * short reason into err. Side-effects on success: replaces level_cfg,
 * sets level_cfg_loaded, places players at start cells, broadcasts MAP,
 * and checks whether the match is now ready to start. */
static int activate_level_index(const level_entry_t *entries, int n, int idx,
                                char *out_name, size_t out_name_len,
                                char *err, size_t err_len) {
    if (idx < 0 || idx >= n) {
        if (err && err_len) snprintf(err, err_len, "bad index %d", idx);
        return -1;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", LEVEL_DIR, entries[idx].name);

    level_config_t tmp = {0};
    char load_err[64];
    if (level_config_load(path, &tmp, load_err, sizeof(load_err)) != 0) {
        if (err && err_len)
            snprintf(err, err_len, "load failed: %s", load_err);
        return -1;
    }
    int starts = count_player_starts(&tmp);
    if (starts < active_clients_count) {
        if (err && err_len)
            snprintf(err, err_len, "map fits %d, %d connected",
                     starts, active_clients_count);
        level_config_free(&tmp);
        return -1;
    }
    level_config_free(&level_cfg);
    level_cfg = tmp;
    level_cfg_loaded = true;
    level_config_free(&level_cfg_pristine);
    level_cfg_clone(&level_cfg_pristine, &level_cfg);
    if (out_name && out_name_len)
        snprintf(out_name, out_name_len, "%s", entries[idx].name);
    log_msg("Level loaded: %s (%ux%u, max=%d, s=%u d=%u r=%u t=%u)",
            entries[idx].name, level_cfg.rows, level_cfg.cols, starts,
            level_cfg.speed, level_cfg.danger_ticks, level_cfg.radius,
            level_cfg.bomb_timer_ticks);
    place_players_at_starts();
    broadcast_map();
    check_match_start();
    return 0;
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

    char err[96], name[LEVEL_NAME_MAX];
    if (activate_level_index(entries, n, sel, name, sizeof(name),
                             err, sizeof(err)) != 0) {
        log_msg("Level load failed: %s", err);
    }
}

/* Lobby leader = player with the lowest active id. Empty room → 0. */
static uint8_t lobby_leader_id(void) {
    uint8_t lead = 0;
    for (int i = 0; i < active_clients_count; i++) {
        if (clients[i].fd == -1) continue;
        uint8_t id = clients[i].p.id;
        if (id >= 1 && id <= LEVEL_MAX_PLAYERS &&
            (lead == 0 || id < lead)) {
            lead = id;
        }
    }
    return lead;
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

/* Wire-byte length of a message of the given type, given the bytes we
 * have buffered so far (avail). Returns:
 *   > 0 — the full length of the message; caller should slice that many
 *         bytes off buffer and dispatch them.
 *     0 — not enough buffered yet to know (e.g. variable-length header);
 *         caller should wait for more bytes.
 *    -1 — unknown / illegal type.
 */
static ssize_t msg_wire_len(uint8_t type, const char *buf, ssize_t avail) {
    switch (type) {
        case MSG_HELLO:
            return HEADER_LEN + CLIENT_ID_LEN + PLAYER_NAME_LEN;
        case MSG_PING:
        case MSG_PONG:
        case MSG_LEAVE:
        case MSG_DISCONNECT:
        case MSG_SET_READY:
        case MSG_SYNC_REQUEST:
        case MSG_MAP_LIST_REQUEST:
            return HEADER_LEN;
        case MSG_SET_STATUS:
        case MSG_MOVE_ATTEMPT:
            return HEADER_LEN + 1;
        case MSG_BOMB_ATTEMPT:
            return HEADER_LEN + 2;
        case MSG_MAP_SELECT:
            return HEADER_LEN + MAP_NAME_LEN;
        case MSG_ERROR: {
            if (avail < HEADER_LEN + 2) return 0;
            uint16_t elen = ((uint16_t)(uint8_t)buf[HEADER_LEN] << 8) |
                            (uint16_t)(uint8_t)buf[HEADER_LEN + 1];
            return HEADER_LEN + 2 + elen;
        }
        default:
            return -1;
    }
}

int process_message(int client_idx, int client_fd, char *buffer, ssize_t len) {
    if (len <= 0) return -1;
    if (len < 1) return -1;

    /* Stable per-connection identifier for log lines. The clients[]
     * array compacts when other clients disconnect, so the array index
     * shifts — we use the assigned player ID instead, which is stable
     * for the entire lifetime of this client's connection. Before
     * HELLO succeeds the player ID is 0; logs in that window omit the
     * prefix. */
    uint8_t cn = clients[client_idx].p.id;
    msg_type_t type = buffer[0];

    switch (type) {
        case MSG_HELLO: {
            if (len < HEADER_LEN + CLIENT_ID_LEN + PLAYER_NAME_LEN) {
                send_error(client_fd, 0xFF, "Malformed HELLO");
                log_msg("ERROR (malformed HELLO from new client)");
                return 0;
            }
            char client_id[CLIENT_ID_LEN + 1] = {0};
            char player_name[PLAYER_NAME_LEN + 1] = {0};
            memcpy(client_id, buffer + HEADER_LEN, CLIENT_ID_LEN);
            memcpy(player_name, buffer + HEADER_LEN + CLIENT_ID_LEN, PLAYER_NAME_LEN);

            /* Pick the lowest free player ID (and one that has a start
             * cell on the loaded map, if any). This gives reconnecting /
             * post-match joiners the first vacated slot rather than
             * piling up at active_clients_count + 1. IDs are stable for
             * the life of the connection — never reshuffled. */
            uint8_t pid = pick_free_player_id();
            if (pid == 0) {
                send_error(client_fd, ID_UNKNOWN, "Server full for this map");
                send_disconnect(client_fd, ID_UNKNOWN);
                return -1;
            }
            snprintf(clients[client_idx].p.name, MAX_NAME_LEN + 1, "%s", player_name);
            clients[client_idx].p.id = pid;
            clients[client_idx].p.alive = true;

            log_msg("P%u -> HELLO (id=%s, name=%s)",
                    pid, client_id, clients[client_idx].p.name);

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
                log_msg("P%u <- WELCOME", pid);
            }

            if (level_cfg_loaded) {
                place_one_player_at_start(client_idx);
                if (send_map_to(client_fd, pid) == 0) {
                    log_msg("P%u <- MAP (%ux%u)", pid,
                            level_cfg.rows, level_cfg.cols);
                }
            }
            /* If the client joined mid-match, hand it everyone's
             * current state so its rendered board matches the server,
             * and broadcast its own stats so existing clients render the
             * fresh occupant of this (possibly-reused) player ID. */
            if (current_game_state == GAME_RUNNING && level_cfg_loaded) {
                broadcast_sync_board_for(&clients[client_idx].p);
                send_sync_board_all_to(client_fd, pid);
                log_msg("P%u <- SYNC_BOARD x%d (mid-match join)",
                        pid, active_clients_count);
            }
            /* If the client joined during the end screen, the spec's
             * WELCOME body says we should also send WINNER (status==2). */
            else if (current_game_state == GAME_END && level_cfg_loaded) {
                send_sync_board_all_to(client_fd, pid);
                send_winner_to(client_fd, pid, last_winner_id);
                log_msg("P%u <- SYNC_BOARD x%d + WINNER=%u (end-screen join)",
                        pid, active_clients_count, last_winner_id);
            }
            return 0;
        }

        case MSG_PING: {
            log_msg("P%u -> PING", cn);
            send_simple(client_fd, MSG_PONG, clients[client_idx].p.id);
            log_msg("P%u <- PONG", cn);
            return 0;
        }

        case MSG_PONG: {
            /* Heartbeat reply — last_recv_at already refreshed by the
             * read path; nothing else to do. */
            return 0;
        }

        case MSG_LEAVE: {
            log_msg("P%u -> LEAVE", cn);
            return 0;
        }

        case MSG_SET_READY: {
            log_msg("P%u -> SET_READY", cn);
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
            log_msg("P%u -> SET_STATUS %u", cn, s->game_status);
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
            if (!dir_delta(ma->direction, &dr, &dc)) {
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
            /* Player-on-player collision is intentionally allowed: two
             * players may share a cell. Clients render the local player on
             * top so the user always sees their own avatar. */
            clients[client_idx].p.row = (uint16_t)nr;
            clients[client_idx].p.col = (uint16_t)nc;
            uint16_t coord = make_cell_index((uint16_t)nr, (uint16_t)nc,
                                             level_cfg.cols);
            broadcast_moved(clients[client_idx].p.id, coord);
            /* Walking into a lingering explosion cell (the danger zone
             * left behind after the initial flash) kills the player.
             * We attribute the kill to the bomb owner of the matching
             * explosion record; arm cells (non-center) fall back to the
             * victim's own id since we don't track them individually. */
            if (target->type == CELL_EXPLOSION) {
                int owner = find_explosion_owner_at((uint16_t)nr,
                                                    (uint16_t)nc);
                uint8_t kid = owner >= 0
                            ? (uint8_t)owner
                            : clients[client_idx].p.id;
                kill_players_at(kid, (uint16_t)nr, (uint16_t)nc);
                check_game_end();
                return 0;
            }
            if (target->type == CELL_BONUS_SPEED ||
                target->type == CELL_BONUS_RADIUS ||
                target->type == CELL_BONUS_TIMER ||
                target->type == CELL_BONUS_BOMBS) {
                cell_type_t kind = target->type;
                target->type = CELL_EMPTY;
                target->player_id = 0;
                apply_bonus(&clients[client_idx].p, kind);
                broadcast_bonus_retrieved(clients[client_idx].p.id, coord);
                /* Stats changed — refresh clients so HUDs stay accurate. */
                broadcast_sync_board_for(&clients[client_idx].p);
                log_msg("P%u picked up bonus at (%d,%d) (%s)",
                        cn, nr, nc,
                        kind == CELL_BONUS_SPEED  ? "speed"  :
                        kind == CELL_BONUS_RADIUS ? "radius" :
                        kind == CELL_BONUS_TIMER  ? "timer"  : "bombs");
            }
            log_msg("P%u MOVED to (%d,%d)", cn, nr, nc);
            return 0;
        }

        case MSG_BOMB_ATTEMPT: {
            if (len < (ssize_t)sizeof(msg_bomb_attempt_t)) {
                send_error(client_fd, clients[client_idx].p.id,
                           "Bad BOMB_ATTEMPT");
                return 0;
            }
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
            /* Spec rule: bombs may only be placed on the player's current
             * cell. Reject explicitly so a client bug surfaces instead of
             * silently rewriting the cell. */
            const msg_bomb_attempt_t *ba =
                (const msg_bomb_attempt_t *)buffer;
            uint16_t want = ntohs(ba->cell);
            uint16_t have = make_cell_index(p->row, p->col, level_cfg.cols);
            if (want != have) {
                send_error(client_fd, p->id, "Bomb cell != player cell");
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
            uint16_t base_danger = level_cfg.danger_ticks
                                   ? level_cfg.danger_ticks
                                   : DEFAULT_DANGER_TICKS;
            bombs[slot].danger_ticks  =
                (uint16_t)(base_danger + p->danger_extra_ticks);

            cell_t *cell = level_cell_at(&level_cfg, p->row, p->col);
            cell->type = CELL_BOMB;

            uint16_t coord = make_cell_index(p->row, p->col, level_cfg.cols);
            broadcast_bomb(p->id, coord);
            log_msg("P%u -> BOMB at (%u,%u) r=%u t=%u",
                    cn, p->row, p->col, p->bomb_radius, p->bomb_timer_ticks);
            return 0;
        }

        case MSG_SYNC_REQUEST: {
            log_msg("P%u -> SYNC_REQUEST", cn);
            if (current_game_state == GAME_RUNNING && level_cfg_loaded) {
                send_map_to(client_fd, clients[client_idx].p.id);
                send_sync_board_all_to(client_fd, clients[client_idx].p.id);
                log_msg("P%u <- MAP + SYNC_BOARD x%d (sync)",
                        cn, active_clients_count);
            }
            return 0;
        }

        case MSG_MAP_LIST_REQUEST: {
            uint8_t pid = clients[client_idx].p.id;
            log_msg("P%u -> MAP_LIST_REQUEST (state=%d, leader=%u)",
                    pid, (int)current_game_state, lobby_leader_id());
            if (current_game_state != GAME_LOBBY) {
                send_error(client_fd, pid, "Map pick only in lobby");
                log_msg("P%u <- ERROR (not lobby)", pid);
                return 0;
            }
            if (pid != lobby_leader_id()) {
                send_error(client_fd, pid, "Only leader picks the map");
                log_msg("P%u <- ERROR (not leader)", pid);
                return 0;
            }
            level_entry_t entries[LEVEL_LIST_MAX];
            int n = level_list_dir(LEVEL_DIR, entries, LEVEL_LIST_MAX);
            if (n < 0) {
                send_error(client_fd, pid, "Server has no maps dir");
                return 0;
            }
            if (n > 255) n = 255;
            size_t total = sizeof(msg_map_list_t) +
                           (size_t)n * sizeof(msg_map_entry_t);
            uint8_t *out = (uint8_t *)calloc(1, total);
            if (!out) {
                send_error(client_fd, pid, "OOM");
                return 0;
            }
            msg_map_list_t *m = (msg_map_list_t *)out;
            m->hdr.msg_type  = MSG_MAP_LIST;
            m->hdr.sender_id = ID_SERVER;
            m->hdr.target_id = pid;
            msg_map_entry_t *items =
                (msg_map_entry_t *)(out + sizeof(msg_map_list_t));
            int kept = 0;
            for (int i = 0; i < n; i++) {
                char path[256];
                snprintf(path, sizeof(path), "%s/%.*s",
                         LEVEL_DIR, MAP_NAME_LEN - 1, entries[i].name);
                level_config_t probe = {0};
                char perr[64];
                if (level_config_load(path, &probe,
                                      perr, sizeof(perr)) != 0) {
                    log_msg("Skipping unreadable map %s: %s",
                            entries[i].name, perr);
                    continue;
                }
                msg_map_entry_t *e = &items[kept++];
                size_t nlen = strnlen(entries[i].name, MAP_NAME_LEN - 1);
                memcpy(e->name, entries[i].name, nlen);
                e->rows        = probe.rows;
                e->cols        = probe.cols;
                e->max_players = (uint8_t)count_player_starts(&probe);
                level_config_free(&probe);
            }
            m->count = (uint8_t)kept;
            total = sizeof(msg_map_list_t) +
                    (size_t)kept * sizeof(msg_map_entry_t);
            ssize_t wrote = write(client_fd, out, total);
            free(out);
            log_msg("P%u -> MAP_LIST_REQUEST; sent %d entries (%zd bytes)",
                    pid, kept, wrote);
            return 0;
        }

        case MSG_MAP_SELECT: {
            uint8_t pid = clients[client_idx].p.id;
            if (current_game_state != GAME_LOBBY) {
                send_error(client_fd, pid, "Map pick only in lobby");
                return 0;
            }
            if (pid != lobby_leader_id()) {
                send_error(client_fd, pid, "Only leader picks the map");
                return 0;
            }
            const msg_map_select_t *ms =
                (const msg_map_select_t *)buffer;
            char wanted[MAP_NAME_LEN];
            memcpy(wanted, ms->name, MAP_NAME_LEN - 1);
            wanted[MAP_NAME_LEN - 1] = '\0';
            level_entry_t entries[LEVEL_LIST_MAX];
            int n = level_list_dir(LEVEL_DIR, entries, LEVEL_LIST_MAX);
            if (n <= 0) {
                send_error(client_fd, pid, "No maps available");
                return 0;
            }
            int idx = -1;
            for (int i = 0; i < n; i++) {
                if (strcmp(entries[i].name, wanted) == 0) { idx = i; break; }
            }
            if (idx < 0) {
                send_error(client_fd, pid, "Unknown map name");
                log_msg("P%u MAP_SELECT '%s' failed: not found",
                        pid, wanted);
                return 0;
            }
            char err[96];
            if (activate_level_index(entries, n, idx,
                                     NULL, 0, err, sizeof(err)) != 0) {
                send_error(client_fd, pid, err);
                log_msg("P%u MAP_SELECT '%s' failed: %s",
                        pid, wanted, err);
                return 0;
            }
            log_msg("P%u (leader) picked map '%s'", pid, wanted);
            return 0;
        }

        default:
            log_msg("P%u -> unknown (type=%d)", cn, (int)type);
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
                    clients[active_clients_count].last_recv_at = time(NULL);
                    clients[active_clients_count].pong_pending_since = 0;
                    active_clients_count++;
                    log_msg("New client connected (awaiting HELLO).");
                } else {
                    send_disconnect(new_sock, ID_UNKNOWN);
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
                    clients[i].last_recv_at = time(NULL);
                    clients[i].pong_pending_since = 0;
                    /* Drain ALL messages in this read — TCP may have
                     * coalesced more than one. The previous code only
                     * processed buffer[0] and dropped the rest, which is
                     * what made MAP_LIST_REQUESTs sent right after a
                     * PING vanish silently. */
                    ssize_t off = 0;
                    while (off < bytes_read) {
                        if (bytes_read - off < HEADER_LEN) break;
                        uint8_t mtype = (uint8_t)buffer[off];
                        ssize_t mlen = msg_wire_len(mtype, buffer + off,
                                                    bytes_read - off);
                        if (mlen <= 0) {
                            /* Unknown type or partial variable-length
                             * header. Hand it to process_message so it
                             * still logs "unknown" for visibility, then
                             * stop draining this read. */
                            process_message(i, sock, buffer + off,
                                            bytes_read - off);
                            break;
                        }
                        if (mlen > bytes_read - off) break;
                        process_message(i, sock, buffer + off, mlen);
                        off += mlen;
                    }
                } else if (bytes_read == 0) {
                    uint8_t leaving = clients[i].p.id;
                    if (leaving != 0) {
                        log_msg("P%u disconnected.", leaving);
                        broadcast_simple(MSG_LEAVE, leaving);
                    } else {
                        log_msg("Pre-HELLO client disconnected.");
                    }
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
                    uint8_t leaving = clients[i].p.id;
                    if (leaving != 0) {
                        log_msg("P%u connection error.", leaving);
                        broadcast_simple(MSG_LEAVE, leaving);
                    } else {
                        log_msg("Pre-HELLO client connection error.");
                    }
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

        /* Liveness sweep per protokols.docx: send PING after IDLE seconds
         * of silence; only declare timeout if no reply (or any other
         * traffic) arrives within PONG_TIMEOUT_SEC of that PING. */
        time_t now_t = time(NULL);
        for (int i = 0; i < active_clients_count; i++) {
            if (clients[i].fd == -1) continue;
            time_t silent = now_t - clients[i].last_recv_at;
            if (clients[i].pong_pending_since != 0 &&
                now_t - clients[i].pong_pending_since > PONG_TIMEOUT_SEC) {
                log_msg("P%u PONG timeout (%lds since PING) - dropping.",
                        clients[i].p.id,
                        (long)(now_t - clients[i].pong_pending_since));
                send_disconnect(clients[i].fd, clients[i].p.id);
                if (clients[i].p.id != 0)
                    broadcast_simple(MSG_LEAVE, clients[i].p.id);
                close(clients[i].fd);
                clients[i].fd = -1;
            } else if (clients[i].pong_pending_since == 0 &&
                       silent > IDLE_BEFORE_PING_SEC) {
                if (send_simple(clients[i].fd, MSG_PING,
                                clients[i].p.id) == 0) {
                    clients[i].pong_pending_since = now_t;
                }
            }
        }

        // If no activity, sleep briefly to reduce CPU usage
        if (ret == 0) {
             usleep(1000); 
        }
    }

    cleanup(0);
    return 0;
}
