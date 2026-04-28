#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include <netdb.h>
#include <time.h>
#include <ncurses.h>

#define WELCOME_TIMEOUT_SEC   30
/* Per protokols.docx: send PING after IDLE_BEFORE_PING_SEC of silence,
 * and only consider the peer timed out if no PONG (or any other reply)
 * arrives within PONG_TIMEOUT_SEC of that PING. Silence alone is not a
 * timeout — that was the old bug that dropped idle lobby clients. */
#define IDLE_BEFORE_PING_SEC  15
#define PONG_TIMEOUT_SEC      30

#include "../common/level_config.h"
#include "../common/protocol.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7890
#define BUFFER_SIZE 8192

#define CLIENT_ID_STR "bbm-client-0.1"
#define DEFAULT_PLAYER_NAME "player"

#define PAIR_TITLE    1
#define PAIR_BORDER   2
#define PAIR_HOTKEY   3
#define PAIR_SELECTED 4
#define PAIR_NORMAL   5
#define PAIR_STATUS   6
#define PAIR_INFO     7
#define PAIR_HARD     8
#define PAIR_SOFT     9
#define PAIR_FLOOR   10
#define PAIR_FLOOR2  11
#define PAIR_BOMB    12
#define PAIR_B_SPEED 13
#define PAIR_B_RAD   14
#define PAIR_B_TIMER 15
#define PAIR_B_BOMBS 16
#define PAIR_PLAYER1 17
#define PAIR_PLAYER2 18
#define PAIR_PLAYER3 19
#define PAIR_PLAYER4 20
#define PAIR_PLAYER5 21
#define PAIR_PLAYER6 22
#define PAIR_PLAYER7 23
#define PAIR_PLAYER8 24
#define PAIR_EXPL    25

static int sock_fd = -1;
static volatile sig_atomic_t running = 1;

static level_config_t level_cfg = {0};
static bool level_cfg_loaded = false;
static uint8_t my_player_id = ID_UNKNOWN;
/* True only when we received a WINNER message in the current session for
 * the current GAME_END. Used to suppress the end-of-match popup on a
 * fresh reconnect into a stale GAME_END state — the rejoining player
 * didn't participate, so we shouldn't claim anyone "won". */
static bool    match_winner_observed = false;
static bool me_ready = false;
static game_status_t game_state = GAME_LOBBY;
static uint8_t last_winner_id = ID_UNKNOWN;

typedef struct {
    bool     active;
    bool     alive;
    uint16_t row;
    uint16_t col;
    uint8_t  bomb_count;
    uint8_t  bomb_radius;
    uint16_t bomb_timer_ticks;
    uint16_t speed;
    uint16_t danger_extra_ticks;
    uint8_t  lives;
} client_player_t;

static client_player_t players[MAX_PLAYERS + 1] = {0};

static void reset_session_state(void) {
    me_ready = false;
    my_player_id = ID_UNKNOWN;
    game_state = GAME_LOBBY;
    level_cfg_loaded = false;
    match_winner_observed = false;
    level_config_free(&level_cfg);
    memset(players, 0, sizeof(players));
}

static const char *BANNER[] = {
    " ____     ___    __  __   ____    _____   ____    __  __      _      _   _ ",
    "| __ )   / _ \\  |  \\/  | | __ )  | ____| |  _ \\  |  \\/  |    / \\    | \\ | |",
    "|  _ \\  | | | | | |\\/| | |  _ \\  |  _|   | |_) | | |\\/| |   / _ \\   |  \\| |",
    "| |_) | | |_| | | |  | | | |_) | | |___  |  _ <  | |  | |  / ___ \\  | |\\  |",
    "|____/   \\___/  |_|  |_| |____/  |_____| |_| \\_\\ |_|  |_| /_/   \\_\\ |_| \\_|",
};
#define BANNER_H ((int)(sizeof(BANNER) / sizeof(BANNER[0])))

static int send_leave(int fd) {
    msg_generic_t m = { MSG_LEAVE, my_player_id, ID_SERVER };
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static void cleanup(int sig) {
    (void)sig;
    if (sock_fd != -1) {
        send_leave(sock_fd);
        close(sock_fd);
        sock_fd = -1;
    }
    level_config_free(&level_cfg);
    endwin();
    exit(0);
}

static void init_colors(void) {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(PAIR_TITLE,    COLOR_YELLOW, -1);
    init_pair(PAIR_BORDER,   COLOR_CYAN,   -1);
    init_pair(PAIR_HOTKEY,   COLOR_GREEN,  -1);
    init_pair(PAIR_SELECTED, COLOR_BLACK,  COLOR_YELLOW);
    init_pair(PAIR_NORMAL,   COLOR_WHITE,  -1);
    init_pair(PAIR_STATUS,   COLOR_RED,    -1);
    init_pair(PAIR_INFO,     COLOR_CYAN,   -1);
    /* Hard walls own bg=blue. Players never use bg=blue so they stay
     * visually distinct against walls. */
    init_pair(PAIR_HARD,     COLOR_BLUE,    COLOR_BLUE);
    init_pair(PAIR_SOFT,     COLOR_BLACK,   COLOR_YELLOW);
    init_pair(PAIR_FLOOR,    COLOR_WHITE,   COLOR_BLACK);
    init_pair(PAIR_FLOOR2,   COLOR_WHITE,   COLOR_BLACK);
    init_pair(PAIR_BOMB,     COLOR_RED,     COLOR_BLACK);
    init_pair(PAIR_B_SPEED,  COLOR_BLACK,   COLOR_CYAN);
    init_pair(PAIR_B_RAD,    COLOR_WHITE,   COLOR_MAGENTA);
    init_pair(PAIR_B_TIMER,  COLOR_BLACK,   COLOR_WHITE);
    init_pair(PAIR_B_BOMBS,  COLOR_YELLOW,  COLOR_RED);
    init_pair(PAIR_PLAYER1,  COLOR_WHITE,   COLOR_RED);
    init_pair(PAIR_PLAYER2,  COLOR_WHITE,   COLOR_GREEN);
    init_pair(PAIR_PLAYER3,  COLOR_BLACK,   COLOR_CYAN);
    init_pair(PAIR_PLAYER4,  COLOR_BLACK,   COLOR_YELLOW);
    init_pair(PAIR_PLAYER5,  COLOR_WHITE,   COLOR_MAGENTA);
    init_pair(PAIR_PLAYER6,  COLOR_YELLOW,  COLOR_RED);
    init_pair(PAIR_PLAYER7,  COLOR_BLACK,   COLOR_GREEN);
    init_pair(PAIR_PLAYER8,  COLOR_BLACK,   COLOR_WHITE);
    init_pair(PAIR_EXPL,     COLOR_YELLOW,  COLOR_RED);
}

static void init_ncurses(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    set_escdelay(25);
    init_colors();
    clear();
}

#define CONNECT_TIMEOUT_SEC 5

static int connect_to_server(const char *host, int port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    int fd = -1;
    int last_err = ECONNREFUSED;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { last_err = errno; continue; }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) break;
        if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
            last_err = errno;
            close(fd); fd = -1;
            continue;
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = CONNECT_TIMEOUT_SEC, .tv_usec = 0 };
        int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            last_err = (sel == 0) ? ETIMEDOUT : errno;
            close(fd); fd = -1;
            continue;
        }

        int sockerr = 0;
        socklen_t slen = sizeof(sockerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &slen) < 0
            || sockerr != 0) {
            last_err = sockerr ? sockerr : errno;
            close(fd); fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd < 0) { errno = last_err; return -1; }
    return fd;
}

static int send_hello(int fd, const char *player_name) {
    msg_hello_t m = {0};
    m.hdr.msg_type  = MSG_HELLO;
    m.hdr.sender_id = ID_UNKNOWN;
    m.hdr.target_id = ID_SERVER;
    size_t cid = strlen(CLIENT_ID_STR);
    if (cid > sizeof(m.client_id)) cid = sizeof(m.client_id);
    memcpy(m.client_id, CLIENT_ID_STR, cid);
    size_t pn = strlen(player_name);
    if (pn > sizeof(m.player_name)) pn = sizeof(m.player_name);
    memcpy(m.player_name, player_name, pn);
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int send_ping(int fd) {
    msg_generic_t m = { MSG_PING, ID_UNKNOWN, ID_SERVER };
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int send_pong(int fd) {
    msg_generic_t m = { MSG_PONG, my_player_id, ID_SERVER };
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int send_set_ready(int fd) {
    msg_generic_t m = { MSG_SET_READY, ID_UNKNOWN, ID_SERVER };
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int send_set_status(int fd, uint8_t target_status) {
    msg_set_status_t m = {0};
    m.hdr.msg_type  = MSG_SET_STATUS;
    m.hdr.sender_id = my_player_id;
    m.hdr.target_id = ID_SERVER;
    m.game_status   = target_status;
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int send_move_attempt(int fd, char dir) {
    msg_move_attempt_t m = {0};
    m.hdr.msg_type  = MSG_MOVE_ATTEMPT;
    m.hdr.sender_id = my_player_id;
    m.hdr.target_id = ID_SERVER;
    m.direction = (uint8_t)dir;
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

static int send_bomb_attempt(int fd, uint16_t cell) {
    msg_bomb_attempt_t m = {0};
    m.hdr.msg_type  = MSG_BOMB_ATTEMPT;
    m.hdr.sender_id = my_player_id;
    m.hdr.target_id = ID_SERVER;
    m.cell          = htons(cell);
    return (write(fd, &m, sizeof(m)) == (ssize_t)sizeof(m)) ? 0 : -1;
}

/* Returns: >0 = bytes read, 0 = peer closed, -1 = nothing ready (errno=EAGAIN)
 * or real recv error (errno set accordingly).
 */
static ssize_t poll_recv(int fd, char *buf, size_t cap) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) return -1;
    if (ret == 0) { errno = EAGAIN; return -1; }
    return read(fd, buf, cap);
}

static void draw_box(int y, int x, int h, int w) {
    attron(COLOR_PAIR(PAIR_BORDER));
    mvaddch(y, x, '+');
    mvaddch(y, x + w - 1, '+');
    mvaddch(y + h - 1, x, '+');
    mvaddch(y + h - 1, x + w - 1, '+');
    for (int i = 1; i < w - 1; i++) {
        mvaddch(y, x + i, '-');
        mvaddch(y + h - 1, x + i, '-');
    }
    for (int i = 1; i < h - 1; i++) {
        mvaddch(y + i, x, '|');
        mvaddch(y + i, x + w - 1, '|');
    }
    attroff(COLOR_PAIR(PAIR_BORDER));
}

static void draw_centered(int y, int width_avail, const char *s,
                          int color_pair, int attr) {
    int len = (int)strlen(s);
    int x = (width_avail - len) / 2;
    if (x < 0) x = 0;
    attron(COLOR_PAIR(color_pair) | attr);
    mvaddstr(y, x, s);
    attroff(COLOR_PAIR(color_pair) | attr);
}

static void draw_banner(int y, int width_avail) {
    int bw = (int)strlen(BANNER[0]);
    if (width_avail < bw + 4) {
        draw_centered(y + BANNER_H / 2, width_avail,
                      "B O M B E R M A N", PAIR_TITLE, A_BOLD);
        return;
    }
    int x = (width_avail - bw) / 2;
    if (x < 0) x = 0;
    attron(COLOR_PAIR(PAIR_TITLE) | A_BOLD);
    for (int i = 0; i < BANNER_H; i++) {
        mvaddstr(y + i, x, BANNER[i]);
    }
    attroff(COLOR_PAIR(PAIR_TITLE) | A_BOLD);
}

static void draw_status_bar(const char *help, const char *msg, int msg_pair) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int y = rows - 1;
    move(y, 0);
    clrtoeol();
    if (help && help[0]) {
        attron(COLOR_PAIR(PAIR_INFO));
        mvaddstr(y, 1, help);
        attroff(COLOR_PAIR(PAIR_INFO));
    }
    if (msg && msg[0]) {
        int x = cols - (int)strlen(msg) - 1;
        if (x < 0) x = 0;
        attron(COLOR_PAIR(msg_pair) | A_BOLD);
        mvaddstr(y, x, msg);
        attroff(COLOR_PAIR(msg_pair) | A_BOLD);
    }
}

#define MENU_ITEM_COUNT 5
static const char *MENU_ITEMS[MENU_ITEM_COUNT] = {
    "Join server",
    "Edit server address",
    "Edit player name",
    "Load level config",
    "Quit",
};
static const char MENU_HOTKEYS[MENU_ITEM_COUNT] = { 'j', 'h', 'n', 'l', 'q' };

static void draw_menu(const char *host, int port,
                      const char *player_name, int sel,
                      const char *status, int status_pair) {
    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    draw_box(0, 0, rows - 1, cols);

    int banner_y = 2;
    draw_banner(banner_y, cols);

    int sub_y = banner_y + BANNER_H + 1;
    draw_centered(sub_y, cols, "[ Network Client v0.1 ]", PAIR_INFO, 0);

    int info_y = sub_y + 2;
    int info_x = cols / 2 - 18;
    if (info_x < 4) info_x = 4;

    attron(COLOR_PAIR(PAIR_NORMAL));
    mvaddstr(info_y,     info_x, "Server : ");
    mvaddstr(info_y + 1, info_x, "Player : ");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    attron(COLOR_PAIR(PAIR_INFO) | A_BOLD);
    mvprintw(info_y,     info_x + 9, "%s:%d", host, port);
    mvprintw(info_y + 1, info_x + 9, "%s", player_name);
    attroff(COLOR_PAIR(PAIR_INFO) | A_BOLD);

    int menu_y = info_y + 3;
    int menu_x = cols / 2 - 16;
    if (menu_x < 4) menu_x = 4;

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int y = menu_y + i;
        if (i == sel) {
            attron(COLOR_PAIR(PAIR_SELECTED) | A_BOLD);
            mvprintw(y, menu_x, "  > %-22s ", MENU_ITEMS[i]);
            attroff(COLOR_PAIR(PAIR_SELECTED) | A_BOLD);
        } else {
            attron(COLOR_PAIR(PAIR_NORMAL));
            mvprintw(y, menu_x, "    %-22s ", MENU_ITEMS[i]);
            attroff(COLOR_PAIR(PAIR_NORMAL));
        }
        attron(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
        mvprintw(y, menu_x + 30, "[%c]", MENU_HOTKEYS[i]);
        attroff(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    }

    int cfg_y = menu_y + MENU_ITEM_COUNT + 1;
    int cfg_x = menu_x;
    if (level_cfg_loaded) {
        attron(COLOR_PAIR(PAIR_INFO));
        mvprintw(cfg_y, cfg_x,
                 "Level: %ux%u  speed=%u  radius=%u  bomb=%ut  danger=%ut",
                 level_cfg.rows, level_cfg.cols, level_cfg.speed,
                 level_cfg.radius, level_cfg.bomb_timer_ticks,
                 level_cfg.danger_ticks);
        attroff(COLOR_PAIR(PAIR_INFO));
    } else {
        attron(COLOR_PAIR(PAIR_NORMAL));
        mvaddstr(cfg_y, cfg_x, "Level: (none loaded)");
        attroff(COLOR_PAIR(PAIR_NORMAL));
    }

    draw_status_bar(" [Up/Down] Navigate  [Enter] Select  [j] Join  [h] Host  [n] Name  [l] Load  [q] Quit",
                    status, status_pair);
    refresh();
}

#define CELL_W 3
#define CELL_H 2

static int player_pair(uint8_t pid) {
    switch (pid) {
        case 1: return PAIR_PLAYER1;
        case 2: return PAIR_PLAYER2;
        case 3: return PAIR_PLAYER3;
        case 4: return PAIR_PLAYER4;
        case 5: return PAIR_PLAYER5;
        case 6: return PAIR_PLAYER6;
        case 7: return PAIR_PLAYER7;
        case 8: return PAIR_PLAYER8;
        default: return PAIR_PLAYER1;
    }
}

static void cell_glyphs(const cell_t *cell, int r, int c,
                        int *pair, int *attr,
                        char top[CELL_W + 1], char bot[CELL_W + 1]) {
    (void)r; (void)c;
    *attr = A_BOLD;
    switch (cell->type) {
        case CELL_EMPTY:
            *pair = PAIR_FLOOR;
            *attr = A_DIM;
            memcpy(top, "   ", 4);
            memcpy(bot, "   ", 4);
            break;
        case CELL_HARD_BLOCK:
            /* Solid block — fg=bg=blue plus spaces gives a clean wall. */
            *pair = PAIR_HARD;
            *attr = 0;
            memcpy(top, "   ", 4);
            memcpy(bot, "   ", 4);
            break;
        case CELL_SOFT_BLOCK:
            *pair = PAIR_SOFT;
            memcpy(top, "[#]", 4);
            memcpy(bot, "[#]", 4);
            break;
        case CELL_BOMB:
            *pair = PAIR_BOMB;
            memcpy(top, " o ", 4);
            memcpy(bot, "(_)", 4);
            break;
        case CELL_PLAYER_START: {
            *pair = player_pair(cell->player_id);
            top[0] = ' ';
            top[1] = (char)('0' + cell->player_id);
            top[2] = ' ';
            top[3] = '\0';
            memcpy(bot, "/V\\", 4);
            break;
        }
        case CELL_BONUS_SPEED:
            *pair = PAIR_B_SPEED;
            memcpy(top, " A ", 4);
            memcpy(bot, ">>>", 4);
            break;
        case CELL_BONUS_RADIUS:
            *pair = PAIR_B_RAD;
            memcpy(top, " R ", 4);
            memcpy(bot, "<*>", 4);
            break;
        case CELL_BONUS_TIMER:
            *pair = PAIR_B_TIMER;
            memcpy(top, " T ", 4);
            memcpy(bot, "(C)", 4);
            break;
        case CELL_BONUS_BOMBS:
            *pair = PAIR_B_BOMBS;
            memcpy(top, " N ", 4);
            memcpy(bot, "+*+", 4);
            break;
        case CELL_EXPLOSION:
            *pair = PAIR_EXPL;
            memcpy(top, "\\|/", 4);
            memcpy(bot, "/|\\", 4);
            break;
    }
}

static void draw_cell(int y, int x, const cell_t *cell, int r, int c) {
    int pair = PAIR_FLOOR, attr = 0;
    char top[CELL_W + 1] = "   ", bot[CELL_W + 1] = "   ";
    cell_glyphs(cell, r, c, &pair, &attr, top, bot);
    attron(COLOR_PAIR(pair) | attr);
    mvaddstr(y,     x, top);
    mvaddstr(y + 1, x, bot);
    attroff(COLOR_PAIR(pair) | attr);
}

static void draw_legend(int y, int x) {
    struct { int pair; const char *t; const char *b; const char *label; } items[] = {
        { PAIR_HARD,    "   ", "   ", "Hard wall"     },
        { PAIR_SOFT,    "[#]", "[#]", "Soft block"    },
        { PAIR_FLOOR,   "   ", "   ", "Floor"         },
        { PAIR_PLAYER1, " 1 ", "/V\\", "Player start" },
        { PAIR_BOMB,    " o ", "(_)", "Bomb"          },
        { PAIR_EXPL,    "\\|/", "/|\\", "Explosion"   },
        { PAIR_B_SPEED, " A ", ">>>", "Speed +1"      },
        { PAIR_B_RAD,   " R ", "<*>", "Radius +1"     },
        { PAIR_B_TIMER, " T ", "(C)", "Timer +10t"    },
        { PAIR_B_BOMBS, " N ", "+*+", "Bombs +1"      },
    };
    int n = (int)(sizeof(items) / sizeof(items[0]));
    attron(COLOR_PAIR(PAIR_NORMAL) | A_BOLD);
    mvaddstr(y, x, "Legend");
    attroff(COLOR_PAIR(PAIR_NORMAL) | A_BOLD);
    for (int i = 0; i < n; i++) {
        int row = y + 1 + i * 2;
        attron(COLOR_PAIR(items[i].pair) | A_BOLD);
        mvaddstr(row,     x, items[i].t);
        mvaddstr(row + 1, x, items[i].b);
        attroff(COLOR_PAIR(items[i].pair) | A_BOLD);
        attron(COLOR_PAIR(PAIR_NORMAL));
        mvprintw(row, x + 4, "%s", items[i].label);
        attroff(COLOR_PAIR(PAIR_NORMAL));
    }
}

static void preview_level(void) {
    if (!level_cfg_loaded) return;
    nodelay(stdscr, FALSE);

    while (1) {
        erase();
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        draw_box(0, 0, rows - 1, cols);

        draw_centered(1, cols, "=== Level Preview ===", PAIR_TITLE, A_BOLD);

        int info_y = 3;
        int info_x = 4;
        attron(COLOR_PAIR(PAIR_NORMAL));
        mvaddstr(info_y, info_x, "Size:    ");
        mvaddstr(info_y + 1, info_x, "Speed:   ");
        mvaddstr(info_y + 2, info_x, "Bomb:    ");
        mvaddstr(info_y + 3, info_x, "Radius:  ");
        mvaddstr(info_y + 4, info_x, "Danger:  ");
        attroff(COLOR_PAIR(PAIR_NORMAL));
        attron(COLOR_PAIR(PAIR_INFO) | A_BOLD);
        mvprintw(info_y,     info_x + 9, "%ux%u",
                 level_cfg.rows, level_cfg.cols);
        mvprintw(info_y + 1, info_x + 9, "%u blocks/s", level_cfg.speed);
        mvprintw(info_y + 2, info_x + 9, "%u ticks", level_cfg.bomb_timer_ticks);
        mvprintw(info_y + 3, info_x + 9, "%u blocks", level_cfg.radius);
        mvprintw(info_y + 4, info_x + 9, "%u ticks", level_cfg.danger_ticks);
        attroff(COLOR_PAIR(PAIR_INFO) | A_BOLD);

        int legend_x = info_x;
        int legend_y = info_y + 6;
        draw_legend(legend_y, legend_x);

        int map_x = 32;
        int map_y = info_y;
        int avail_w = cols - map_x - 4;
        int avail_h = rows - map_y - 4;
        int draw_cols = level_cfg.cols;
        int draw_rows = level_cfg.rows;
        if (draw_cols * CELL_W > avail_w) draw_cols = avail_w / CELL_W;
        if (draw_rows * CELL_H > avail_h) draw_rows = avail_h / CELL_H;
        if (draw_cols < 0) draw_cols = 0;
        if (draw_rows < 0) draw_rows = 0;

        int frame_w = draw_cols * CELL_W;
        int frame_h = draw_rows * CELL_H;

        attron(COLOR_PAIR(PAIR_BORDER) | A_BOLD);
        for (int i = -1; i <= frame_w; i++) {
            mvaddch(map_y - 1,         map_x + i, '=');
            mvaddch(map_y + frame_h,   map_x + i, '=');
        }
        for (int i = -1; i <= frame_h; i++) {
            mvaddch(map_y + i, map_x - 1,        '|');
            mvaddch(map_y + i, map_x + frame_w,  '|');
        }
        mvaddch(map_y - 1,        map_x - 1,        '+');
        mvaddch(map_y - 1,        map_x + frame_w,  '+');
        mvaddch(map_y + frame_h,  map_x - 1,        '+');
        mvaddch(map_y + frame_h,  map_x + frame_w,  '+');
        attroff(COLOR_PAIR(PAIR_BORDER) | A_BOLD);

        for (int r = 0; r < draw_rows; r++) {
            for (int c = 0; c < draw_cols; c++) {
                cell_t cell = *level_cell_at(&level_cfg, r, c);
                for (int pi = 1; pi <= MAX_PLAYERS; pi++) {
                    if (players[pi].active && players[pi].alive &&
                        players[pi].row == r && players[pi].col == c) {
                        cell.type = CELL_PLAYER_START;
                        cell.player_id = (uint8_t)pi;
                        break;
                    }
                }
                draw_cell(map_y + r * CELL_H,
                          map_x + c * CELL_W, &cell, r, c);
            }
        }

        if (draw_cols < level_cfg.cols || draw_rows < level_cfg.rows) {
            attron(COLOR_PAIR(PAIR_STATUS) | A_BOLD);
            mvprintw(map_y + frame_h + 1, map_x,
                     "(clipped: %dx%d of %ux%u, resize terminal)",
                     draw_rows, draw_cols, level_cfg.rows, level_cfg.cols);
            attroff(COLOR_PAIR(PAIR_STATUS) | A_BOLD);
        }

        draw_status_bar(" [q/Esc/Enter] Return to menu", "", PAIR_INFO);
        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27 ||
            ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
    }

    nodelay(stdscr, TRUE);
}

static void load_config_dialog(char *status, size_t status_cap, int *status_pair) {
    level_entry_t entries[LEVEL_LIST_MAX];
    int n = level_list_dir(LEVEL_DIR, entries, LEVEL_LIST_MAX);
    if (n < 0) {
        snprintf(status, status_cap, "Cannot open '%s/'", LEVEL_DIR);
        *status_pair = PAIR_STATUS;
        return;
    }
    if (n == 0) {
        snprintf(status, status_cap, "No .map files in '%s/'", LEVEL_DIR);
        *status_pair = PAIR_STATUS;
        return;
    }

    nodelay(stdscr, FALSE);
    int sel = 0;
    while (1) {
        erase();
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        draw_box(0, 0, rows - 1, cols);
        draw_centered(2, cols, "=== Pick Level Config ===", PAIR_TITLE, A_BOLD);
        int x = cols / 2 - 20;
        if (x < 4) x = 4;
        mvprintw(4, x, "Folder: %s/  (%d file%s)",
                 LEVEL_DIR, n, n == 1 ? "" : "s");
        for (int i = 0; i < n; i++) {
            int y = 6 + i;
            if (i == sel) {
                attron(COLOR_PAIR(PAIR_SELECTED) | A_BOLD);
                mvprintw(y, x, "  > %-30s ", entries[i].name);
                attroff(COLOR_PAIR(PAIR_SELECTED) | A_BOLD);
            } else {
                attron(COLOR_PAIR(PAIR_NORMAL));
                mvprintw(y, x, "    %-30s ", entries[i].name);
                attroff(COLOR_PAIR(PAIR_NORMAL));
            }
        }
        draw_status_bar(" [Up/Down] Move   [Enter] Select   [q/Esc] Cancel",
                        "", PAIR_INFO);
        refresh();

        int ch = getch();
        if (ch == KEY_UP)        sel = (sel - 1 + n) % n;
        else if (ch == KEY_DOWN) sel = (sel + 1) % n;
        else if (ch == 27 || ch == 'q' || ch == 'Q') {
            nodelay(stdscr, TRUE);
            return;
        }
        else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
    }
    nodelay(stdscr, TRUE);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", LEVEL_DIR, entries[sel].name);

    level_config_t tmp = {0};
    char err[96];
    if (level_config_load(path, &tmp, err, sizeof(err)) == 0) {
        level_config_free(&level_cfg);
        level_cfg = tmp;
        level_cfg_loaded = true;
        snprintf(status, status_cap, "Loaded %s (%ux%u)",
                 entries[sel].name, level_cfg.rows, level_cfg.cols);
        *status_pair = PAIR_INFO;
        preview_level();
        return;
    }
    snprintf(status, status_cap, "Load failed: %s", err);
    *status_pair = PAIR_STATUS;
}

static void edit_server(char *host_buf, size_t host_cap, int *port) {
    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);

    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    draw_box(0, 0, rows - 1, cols);

    draw_centered(2, cols, "=== Edit Server Address ===", PAIR_TITLE, A_BOLD);

    int x = cols / 2 - 22;
    if (x < 4) x = 4;
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvprintw(5, x, "Current : %s:%d", host_buf, *port);
    mvprintw(7, x, "Format  : HOST[:PORT]  (HOST may be IP or hostname)");
    mvprintw(8, x, "Examples: 127.0.0.1   192.168.1.5:9000   srv.example.com");
    mvprintw(9, x, "Empty input keeps current values.");
    mvaddstr(11, x, "New : ");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    draw_status_bar(" Type address and press Enter (Esc to cancel)",
                    "", PAIR_INFO);
    refresh();

    char tmp[256] = {0};
    move(11, x + 6);
    if (getnstr(tmp, sizeof(tmp) - 1) == OK && tmp[0] != '\0') {
        char *colon = strchr(tmp, ':');
        if (colon) {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) *port = p;
        }
        if (tmp[0] != '\0') {
            snprintf(host_buf, host_cap, "%s", tmp);
        }
    }

    nodelay(stdscr, TRUE);
    noecho();
    curs_set(0);
}

static void edit_name(char *buf, size_t cap) {
    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);

    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    draw_box(0, 0, rows - 1, cols);

    draw_centered(2, cols, "=== Edit Player Name ===", PAIR_TITLE, A_BOLD);

    int x = cols / 2 - 18;
    if (x < 4) x = 4;
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvprintw(5, x, "Current : %s", buf);
    mvprintw(7, x, "Max %d chars. Enter to confirm.", (int)cap - 1);
    mvprintw(8, x, "Empty input keeps current name.");
    mvaddstr(10, x, "New : ");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    draw_status_bar(" Type new name and press Enter", "", PAIR_INFO);
    refresh();

    char tmp[PLAYER_NAME_LEN + 1] = {0};
    move(10, x + 6);
    if (getnstr(tmp, (int)(cap - 1)) == OK && tmp[0] != '\0') {
        snprintf(buf, cap, "%s", tmp);
    }

    nodelay(stdscr, TRUE);
    noecho();
    curs_set(0);
}

static void draw_ui(const char *host, int port, const char *player_name,
                    const char *last_recv, const char *status) {
    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    draw_box(0, 0, rows - 1, cols);

    draw_centered(1, cols, "B O M B E R M A N  -  Connected", PAIR_TITLE, A_BOLD);

    int x = 4, y = 4;
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvaddstr(y,     x, "Server : ");
    mvaddstr(y + 1, x, "Player : ");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    attron(COLOR_PAIR(PAIR_INFO) | A_BOLD);
    mvprintw(y,     x + 9, "%s:%d", host, port);
    mvprintw(y + 1, x + 9, "%s", player_name);
    attroff(COLOR_PAIR(PAIR_INFO) | A_BOLD);
    y += 3;

    attron(COLOR_PAIR(PAIR_NORMAL));
    mvaddstr(y++, x, "Last server message:");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    attron(COLOR_PAIR(PAIR_INFO));
    mvprintw(y++, x + 2, "\"%s\"", last_recv);
    attroff(COLOR_PAIR(PAIR_INFO));
    y++;

    attron(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    mvaddstr(y, x, "[p]");
    attroff(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvaddstr(y++, x + 4, "Send PING");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    attron(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    mvaddstr(y, x, "[m]");
    attroff(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvaddstr(y++, x + 4, level_cfg_loaded
                          ? "View map (received)"
                          : "View map (none yet)");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    attron(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    mvaddstr(y, x, "[SPC]");
    attroff(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    attron(COLOR_PAIR(PAIR_NORMAL));
    if (game_state == GAME_RUNNING) {
        mvaddstr(y++, x + 6, "In game (WASD move, SPACE bomb)");
    } else if (game_state == GAME_END) {
        mvaddstr(y++, x + 6, "New match (return to lobby)");
    } else if (me_ready) {
        mvaddstr(y++, x + 6, "Ready (waiting for others)");
    } else {
        mvaddstr(y++, x + 6, "Start match (set ready)");
    }
    attroff(COLOR_PAIR(PAIR_NORMAL));

    attron(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    mvaddstr(y, x, "[q]");
    attroff(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvaddstr(y++, x + 4, "Quit");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    y++;
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvprintw(y++, x, "State: %s",
             game_state == GAME_RUNNING ? "RUNNING" :
             game_state == GAME_END     ? "ENDED"   : "LOBBY");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    if ((game_state == GAME_RUNNING || game_state == GAME_END) &&
        my_player_id >= 1 && my_player_id <= MAX_PLAYERS &&
        players[my_player_id].active) {
        const client_player_t *me = &players[my_player_id];
        int pcolor = player_pair(my_player_id);
        attron(COLOR_PAIR(pcolor) | A_BOLD);
        mvprintw(y++, x, " YOU: P%u (%s) ",
                 my_player_id,
                 me->alive ? "alive" : "dead - spectating");
        attroff(COLOR_PAIR(pcolor) | A_BOLD);
        attron(COLOR_PAIR(PAIR_INFO));
        mvprintw(y++, x + 2,
                 "bombs  : %u",   me->bomb_count);
        mvprintw(y++, x + 2,
                 "radius : %u",   me->bomb_radius);
        mvprintw(y++, x + 2,
                 "speed  : %u",   me->speed);
        mvprintw(y++, x + 2,
                 "fuse   : %ut",  me->bomb_timer_ticks);
        mvprintw(y++, x + 2,
                 "linger : +%ut", me->danger_extra_ticks);
        attroff(COLOR_PAIR(PAIR_INFO));
    }

    if (game_state == GAME_RUNNING && level_cfg_loaded) {
        int map_x = 38;
        int map_y = 3;
        int avail_w = cols - map_x - 4;
        int avail_h = rows - map_y - 3;
        int dc = level_cfg.cols;
        int dr = level_cfg.rows;
        if (dc * CELL_W > avail_w) dc = avail_w / CELL_W;
        if (dr * CELL_H > avail_h) dr = avail_h / CELL_H;
        if (dc < 0) dc = 0;
        if (dr < 0) dr = 0;

        int frame_w = dc * CELL_W;
        int frame_h = dr * CELL_H;

        attron(COLOR_PAIR(PAIR_BORDER) | A_BOLD);
        for (int i = -1; i <= frame_w; i++) {
            mvaddch(map_y - 1,         map_x + i, '=');
            mvaddch(map_y + frame_h,   map_x + i, '=');
        }
        for (int i = -1; i <= frame_h; i++) {
            mvaddch(map_y + i, map_x - 1,        '|');
            mvaddch(map_y + i, map_x + frame_w,  '|');
        }
        mvaddch(map_y - 1,        map_x - 1,        '+');
        mvaddch(map_y - 1,        map_x + frame_w,  '+');
        mvaddch(map_y + frame_h,  map_x - 1,        '+');
        mvaddch(map_y + frame_h,  map_x + frame_w,  '+');
        attroff(COLOR_PAIR(PAIR_BORDER) | A_BOLD);

        for (int r = 0; r < dr; r++) {
            for (int c = 0; c < dc; c++) {
                cell_t cell = *level_cell_at(&level_cfg, r, c);
                for (int pi = 1; pi <= MAX_PLAYERS; pi++) {
                    if (players[pi].active && players[pi].alive &&
                        players[pi].row == r && players[pi].col == c) {
                        cell.type = CELL_PLAYER_START;
                        cell.player_id = (uint8_t)pi;
                        break;
                    }
                }
                draw_cell(map_y + r * CELL_H,
                          map_x + c * CELL_W, &cell, r, c);
            }
        }
    }

    if (game_state == GAME_END && match_winner_observed) {
        int rows2, cols2;
        getmaxyx(stdscr, rows2, cols2);
        int by = rows2 / 2 - 3;
        int bw = 48;
        int bx = (cols2 - bw) / 2;
        if (bx < 1) bx = 1;
        attron(COLOR_PAIR(PAIR_TITLE) | A_BOLD);
        for (int i = 0; i < bw; i++) {
            mvaddch(by,     bx + i, '=');
            mvaddch(by + 6, bx + i, '=');
        }
        for (int i = 1; i < 6; i++) {
            mvaddch(by + i, bx,          '|');
            mvaddch(by + i, bx + bw - 1, '|');
        }
        char line1[] = "G A M E   O V E R";
        char line2[64];
        if (last_winner_id == ID_UNKNOWN) {
            snprintf(line2, sizeof(line2), "Draw - no survivors");
        } else if (last_winner_id == my_player_id) {
            snprintf(line2, sizeof(line2),
                     "You won! (id=%u)", last_winner_id);
        } else {
            snprintf(line2, sizeof(line2),
                     "Player %u wins", last_winner_id);
        }
        char line3[] = "[SPACE] new match   [q] quit";
        mvaddstr(by + 2, bx + (bw - (int)strlen(line1)) / 2, line1);
        mvaddstr(by + 3, bx + (bw - (int)strlen(line2)) / 2, line2);
        mvaddstr(by + 5, bx + (bw - (int)strlen(line3)) / 2, line3);
        attroff(COLOR_PAIR(PAIR_TITLE) | A_BOLD);
    }

    if (game_state == GAME_RUNNING) {
        draw_status_bar(" [WASD/Arrows] Move   [SPACE] Bomb   [m] Map   [q] Quit",
                        status, PAIR_STATUS);
    } else if (game_state == GAME_END) {
        draw_status_bar(" [SPACE] New match (back to lobby)   [q] Quit",
                        status, PAIR_STATUS);
    } else {
        draw_status_bar(" [p] PING   [m] Map   [SPACE] Start   [q] Quit",
                        status, PAIR_STATUS);
    }
    refresh();
}

int main(int argc, char *argv[]) {
    char host[256];
    snprintf(host, sizeof(host), "%s", DEFAULT_HOST);
    int port = DEFAULT_PORT;
    const char *initial_name = DEFAULT_PLAYER_NAME;

    if (argc >= 2) {
        char *colon = strchr(argv[1], ':');
        if (colon) {
            *colon = '\0';
            snprintf(host, sizeof(host), "%s", argv[1]);
            port = atoi(colon + 1);
        } else {
            snprintf(host, sizeof(host), "%s", argv[1]);
        }
    }
    if (argc >= 3) initial_name = argv[2];
    if (argc > 3) {
        printf("Usage: %s [HOST[:PORT]] [PLAYER_NAME]\n", argv[0]);
        return 1;
    }

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    signal(SIGPIPE, SIG_IGN);

    init_ncurses();

    char player_name[PLAYER_NAME_LEN + 1];
    strncpy(player_name, initial_name, sizeof(player_name) - 1);
    player_name[sizeof(player_name) - 1] = '\0';

    char menu_status[128] = "";
    int menu_status_pair = PAIR_STATUS;
    int sel = 0;

    while (running) {
    bool joined = false;

    while (!joined && running) {
        draw_menu(host, port, player_name, sel, menu_status, menu_status_pair);

        int ch = getch();
        if (ch == ERR) { usleep(20000); continue; }

        int activate_idx = -1;
        switch (ch) {
            case KEY_UP:
                sel = (sel + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
                break;
            case KEY_DOWN:
                sel = (sel + 1) % MENU_ITEM_COUNT;
                break;
            case '\n':
            case '\r':
            case KEY_ENTER:
                activate_idx = sel;
                break;
            case 'j': case 'J':
                activate_idx = 0;
                break;
            case 'h': case 'H':
                activate_idx = 1;
                break;
            case 'n': case 'N':
                activate_idx = 2;
                break;
            case 'l': case 'L':
                activate_idx = 3;
                break;
            case 'q': case 'Q':
                activate_idx = 4;
                break;
            default:
                break;
        }

        if (activate_idx == 0) {
            sel = 0;
            snprintf(menu_status, sizeof(menu_status),
                     "Connecting to %.80s:%d...", host, port);
            menu_status_pair = PAIR_INFO;
            draw_menu(host, port, player_name, sel, menu_status, menu_status_pair);

            sock_fd = connect_to_server(host, port);
            if (sock_fd < 0) {
                snprintf(menu_status, sizeof(menu_status),
                         "Connect failed: %s", strerror(errno));
                menu_status_pair = PAIR_STATUS;
            } else if (send_hello(sock_fd, player_name) != 0) {
                snprintf(menu_status, sizeof(menu_status),
                         "HELLO send failed: %s", strerror(errno));
                menu_status_pair = PAIR_STATUS;
                close(sock_fd);
                sock_fd = -1;
            } else {
                joined = true;
                snprintf(menu_status, sizeof(menu_status),
                         "Connected. Waiting for WELCOME...");
                menu_status_pair = PAIR_INFO;
            }
        } else if (activate_idx == 1) {
            sel = 1;
            edit_server(host, sizeof(host), &port);
        } else if (activate_idx == 2) {
            sel = 2;
            edit_name(player_name, sizeof(player_name));
        } else if (activate_idx == 3) {
            sel = 3;
            load_config_dialog(menu_status, sizeof(menu_status),
                               &menu_status_pair);
        } else if (activate_idx == 4) {
            cleanup(0);
        }

        usleep(20000);
    }

    char last_recv[BUFFER_SIZE] = "(none)";
    char status[64] = "connected";
    bool disconnected = false;
    bool welcome_received = false;
    time_t hello_sent_at  = time(NULL);
    time_t last_recv_at   = hello_sent_at;
    /* When non-zero, time we sent a PING that hasn't been answered by any
     * incoming traffic yet. Cleared on every read; checked against
     * PONG_TIMEOUT_SEC to declare a peer timeout per protokols.docx. */
    time_t pong_pending_since = 0;

    while (running && !disconnected) {
        uint8_t buf[BUFFER_SIZE];
        ssize_t n = poll_recv(sock_fd, (char *)buf, sizeof(buf));
        if (n > 0) {
            last_recv_at = time(NULL);
            pong_pending_since = 0;
            size_t off = 0;
            while ((size_t)n - off >= sizeof(msg_generic_t)) {
                size_t avail = (size_t)n - off;
                const msg_generic_t *hdr =
                    (const msg_generic_t *)(buf + off);
                size_t consumed = 0;

                switch (hdr->msg_type) {
                    case MSG_WELCOME: {
                        if (avail < sizeof(msg_welcome_t)) goto stop;
                        const msg_welcome_t *w =
                            (const msg_welcome_t *)(buf + off);
                        size_t total = sizeof(msg_welcome_t) +
                            (size_t)w->length * sizeof(msg_other_client_t);
                        if (avail < total) goto stop;
                        char sid[SERVER_ID_LEN + 1] = {0};
                        memcpy(sid, w->server_id, SERVER_ID_LEN);
                        my_player_id = hdr->sender_id;
                        game_state = (game_status_t)w->game_status;
                        /* New session: forget any winner from a prior
                         * match the server may still be parked in. */
                        match_winner_observed = false;
                        memset(players, 0, sizeof(players));
                        if (my_player_id >= 1 &&
                            my_player_id <= MAX_PLAYERS) {
                            players[my_player_id].active = true;
                            players[my_player_id].alive  = true;
                        }
                        const msg_other_client_t *oc =
                            (const msg_other_client_t *)(buf + off
                                                  + sizeof(msg_welcome_t));
                        for (uint8_t k = 0; k < w->length; k++) {
                            uint8_t id = oc[k].id;
                            if (id >= 1 && id <= MAX_PLAYERS) {
                                players[id].active = true;
                                players[id].alive  = true;
                            }
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "WELCOME id=%u status=%u others=%u srv=%s",
                                 my_player_id, w->game_status,
                                 w->length, sid);
                        welcome_received = true;
                        consumed = total;
                        break;
                    }
                    case MSG_HELLO: {
                        if (avail < sizeof(msg_hello_t)) goto stop;
                        uint8_t id = hdr->sender_id;
                        if (id >= 1 && id <= MAX_PLAYERS) {
                            /* HELLO may be for a freshly reused ID — wipe
                             * the previous occupant's cached stats so we
                             * don't render stale loadout/position until
                             * the next SYNC_BOARD arrives. */
                            memset(&players[id], 0, sizeof(players[id]));
                            players[id].active = true;
                            players[id].alive  = true;
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "HELLO id=%u joined", id);
                        consumed = sizeof(msg_hello_t);
                        break;
                    }
                    case MSG_SET_STATUS: {
                        if (avail < sizeof(msg_set_status_t)) goto stop;
                        const msg_set_status_t *s =
                            (const msg_set_status_t *)(buf + off);
                        game_state = (game_status_t)s->game_status;
                        snprintf(last_recv, sizeof(last_recv),
                                 "SET_STATUS = %u", s->game_status);
                        if (game_state == GAME_RUNNING) {
                            last_winner_id = ID_UNKNOWN;
                            match_winner_observed = false;
                            for (int k = 1; k <= MAX_PLAYERS; k++) {
                                if (players[k].active)
                                    players[k].alive = true;
                            }
                            snprintf(status, sizeof(status),
                                     "Match started!");
                        } else if (game_state == GAME_LOBBY) {
                            me_ready = false;
                            last_winner_id = ID_UNKNOWN;
                            match_winner_observed = false;
                            for (int k = 1; k <= MAX_PLAYERS; k++) {
                                if (players[k].active)
                                    players[k].alive = true;
                            }
                            snprintf(status, sizeof(status),
                                     "Back to lobby");
                        } else if (game_state == GAME_END) {
                            snprintf(status, sizeof(status),
                                     "Match ended");
                        }
                        consumed = sizeof(msg_set_status_t);
                        break;
                    }
                    case MSG_SET_READY:
                        snprintf(last_recv, sizeof(last_recv),
                                 "SET_READY id=%u", hdr->sender_id);
                        if (hdr->sender_id == my_player_id) me_ready = true;
                        consumed = sizeof(msg_generic_t);
                        break;
                    case MSG_PING:
                        send_pong(sock_fd);
                        snprintf(last_recv, sizeof(last_recv),
                                 "PING from server -> PONG");
                        consumed = sizeof(msg_generic_t);
                        break;
                    case MSG_PONG:
                        snprintf(last_recv, sizeof(last_recv), "PONG");
                        consumed = sizeof(msg_generic_t);
                        break;
                    case MSG_DISCONNECT:
                        snprintf(last_recv, sizeof(last_recv),
                                 "DISCONNECT from server");
                        disconnected = true;
                        snprintf(status, sizeof(status),
                                 "server requested disconnect");
                        consumed = sizeof(msg_generic_t);
                        break;
                    case MSG_LEAVE: {
                        uint8_t id = hdr->sender_id;
                        if (id >= 1 && id <= MAX_PLAYERS) {
                            players[id].active = false;
                            players[id].alive  = false;
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "LEAVE id=%u", id);
                        consumed = sizeof(msg_generic_t);
                        break;
                    }
                    case MSG_ERROR: {
                        if (avail < sizeof(msg_generic_t) + 2) goto stop;
                        uint16_t elen =
                            ((uint16_t)(buf + off)[sizeof(msg_generic_t)] << 8) |
                            (buf + off)[sizeof(msg_generic_t) + 1];
                        size_t total = sizeof(msg_generic_t) + 2 + elen;
                        if (avail < total) goto stop;
                        size_t copy = elen < sizeof(last_recv) - 16
                                      ? elen : sizeof(last_recv) - 16;
                        snprintf(last_recv, sizeof(last_recv),
                                 "ERROR: %.*s", (int)copy,
                                 (const char *)(buf + off +
                                                sizeof(msg_generic_t) + 2));
                        consumed = total;
                        break;
                    }
                    case MSG_MAP: {
                        if (avail < sizeof(msg_map_t)) goto stop;
                        const msg_map_t *m = (const msg_map_t *)(buf + off);
                        if (m->H == 0 || m->W == 0) {
                            consumed = sizeof(msg_map_t);
                            break;
                        }
                        size_t need = (size_t)m->H * m->W;
                        size_t total = sizeof(msg_map_t) + need;
                        if (avail < total) goto stop;
                        level_config_t tmp = {0};
                        tmp.rows = m->H;
                        tmp.cols = m->W;
                        tmp.cells =
                            (cell_t *)calloc(need, sizeof(cell_t));
                        if (!tmp.cells) {
                            consumed = total;
                            break;
                        }
                        bool ok = true;
                        for (size_t i = 0; i < need; i++) {
                            if (!level_parse_cell(
                                    (char)(buf + off)[sizeof(*m) + i],
                                    &tmp.cells[i])) {
                                ok = false; break;
                            }
                        }
                        if (ok) {
                            for (size_t i = 0; i < need; i++) {
                                if (tmp.cells[i].type == CELL_PLAYER_START) {
                                    uint8_t pid = tmp.cells[i].player_id;
                                    if (pid >= 1 && pid <= MAX_PLAYERS &&
                                        players[pid].active) {
                                        players[pid].row =
                                            (uint16_t)(i / m->W);
                                        players[pid].col =
                                            (uint16_t)(i % m->W);
                                        players[pid].alive = true;
                                    }
                                    tmp.cells[i].type = CELL_EMPTY;
                                    tmp.cells[i].player_id = 0;
                                }
                            }
                            level_config_free(&level_cfg);
                            level_cfg = tmp;
                            level_cfg_loaded = true;
                            snprintf(last_recv, sizeof(last_recv),
                                     "MAP %ux%u from server", m->H, m->W);
                            snprintf(status, sizeof(status),
                                     "MAP received (press [m])");
                        } else {
                            free(tmp.cells);
                            snprintf(status, sizeof(status),
                                     "MAP parse error");
                        }
                        consumed = total;
                        break;
                    }
                    case MSG_MOVED: {
                        if (avail < sizeof(msg_moved_t)) goto stop;
                        const msg_moved_t *mv =
                            (const msg_moved_t *)(buf + off);
                        uint16_t coord = ntohs(mv->coord);
                        if (level_cfg_loaded && level_cfg.cols > 0 &&
                            mv->player_id >= 1 &&
                            mv->player_id <= MAX_PLAYERS) {
                            players[mv->player_id].active = true;
                            players[mv->player_id].alive  = true;
                            players[mv->player_id].row =
                                coord / level_cfg.cols;
                            players[mv->player_id].col =
                                coord % level_cfg.cols;
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "MOVED id=%u -> %u",
                                 mv->player_id, coord);
                        consumed = sizeof(msg_moved_t);
                        break;
                    }
                    case MSG_DEATH: {
                        if (avail < sizeof(msg_death_t)) goto stop;
                        const msg_death_t *d =
                            (const msg_death_t *)(buf + off);
                        if (d->player_id >= 1 &&
                            d->player_id <= MAX_PLAYERS) {
                            players[d->player_id].alive = false;
                        }
                        if (d->player_id == my_player_id) {
                            snprintf(status, sizeof(status),
                                     "You died!");
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "DEATH id=%u", d->player_id);
                        consumed = sizeof(msg_death_t);
                        break;
                    }
                    case MSG_WINNER: {
                        if (avail < sizeof(msg_winner_t)) goto stop;
                        const msg_winner_t *w =
                            (const msg_winner_t *)(buf + off);
                        last_winner_id = w->winner_id;
                        match_winner_observed = true;
                        snprintf(last_recv, sizeof(last_recv),
                                 "WINNER id=%u", w->winner_id);
                        if (w->winner_id == ID_UNKNOWN) {
                            snprintf(status, sizeof(status),
                                     "Draw - no survivors");
                        } else if (w->winner_id == my_player_id) {
                            snprintf(status, sizeof(status),
                                     "You won!");
                        } else {
                            snprintf(status, sizeof(status),
                                     "Player %u won", w->winner_id);
                        }
                        consumed = sizeof(msg_winner_t);
                        break;
                    }
                    case MSG_BOMB: {
                        if (avail < sizeof(msg_bomb_t)) goto stop;
                        const msg_bomb_t *b =
                            (const msg_bomb_t *)(buf + off);
                        uint16_t coord = ntohs(b->cell);
                        if (level_cfg_loaded && level_cfg.cols > 0) {
                            uint16_t r = coord / level_cfg.cols;
                            uint16_t c = coord % level_cfg.cols;
                            if (r < level_cfg.rows && c < level_cfg.cols) {
                                cell_t *cl =
                                    level_cell_at(&level_cfg, r, c);
                                cl->type = CELL_BOMB;
                                cl->player_id = b->player_id;
                            }
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "BOMB id=%u @ %u",
                                 b->player_id, coord);
                        consumed = sizeof(msg_bomb_t);
                        break;
                    }
                    case MSG_EXPLOSION_START: {
                        if (avail < sizeof(msg_explosion_t)) goto stop;
                        const msg_explosion_t *e =
                            (const msg_explosion_t *)(buf + off);
                        uint16_t coord = ntohs(e->cell);
                        if (level_cfg_loaded && level_cfg.cols > 0) {
                            int br = coord / level_cfg.cols;
                            int bc = coord % level_cfg.cols;
                            cell_t *cell0 =
                                level_cell_at(&level_cfg, br, bc);
                            cell0->type = CELL_EXPLOSION;
                            static const int drs[] = {-1,1,0,0};
                            static const int dcs[] = {0,0,-1,1};
                            for (int d = 0; d < 4; d++) {
                                for (int dist = 1;
                                     dist <= e->radius; dist++) {
                                    int rr = br + drs[d] * dist;
                                    int cc = bc + dcs[d] * dist;
                                    if (rr < 0 || rr >= level_cfg.rows ||
                                        cc < 0 || cc >= level_cfg.cols)
                                        break;
                                    cell_t *cl =
                                        level_cell_at(&level_cfg, rr, cc);
                                    if (cl->type == CELL_HARD_BLOCK) break;
                                    bool soft =
                                        (cl->type == CELL_SOFT_BLOCK);
                                    cl->type = CELL_EXPLOSION;
                                    if (soft) break;
                                }
                            }
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "BOOM r=%u @ %u", e->radius, coord);
                        consumed = sizeof(msg_explosion_t);
                        break;
                    }
                    case MSG_EXPLOSION_END: {
                        if (avail < sizeof(msg_explosion_t)) goto stop;
                        const msg_explosion_t *e =
                            (const msg_explosion_t *)(buf + off);
                        uint16_t coord = ntohs(e->cell);
                        if (level_cfg_loaded && level_cfg.cols > 0) {
                            int br = coord / level_cfg.cols;
                            int bc = coord % level_cfg.cols;
                            cell_t *cell0 =
                                level_cell_at(&level_cfg, br, bc);
                            if (cell0->type == CELL_EXPLOSION)
                                cell0->type = CELL_EMPTY;
                            static const int drs[] = {-1,1,0,0};
                            static const int dcs[] = {0,0,-1,1};
                            for (int d = 0; d < 4; d++) {
                                for (int dist = 1;
                                     dist <= e->radius; dist++) {
                                    int rr = br + drs[d] * dist;
                                    int cc = bc + dcs[d] * dist;
                                    if (rr < 0 || rr >= level_cfg.rows ||
                                        cc < 0 || cc >= level_cfg.cols)
                                        break;
                                    cell_t *cl =
                                        level_cell_at(&level_cfg, rr, cc);
                                    if (cl->type == CELL_HARD_BLOCK) break;
                                    if (cl->type == CELL_EXPLOSION)
                                        cl->type = CELL_EMPTY;
                                }
                            }
                        }
                        consumed = sizeof(msg_explosion_t);
                        break;
                    }
                    case MSG_BLOCK_DESTROYED: {
                        if (avail < sizeof(msg_block_destroyed_t)) goto stop;
                        const msg_block_destroyed_t *bd =
                            (const msg_block_destroyed_t *)(buf + off);
                        uint16_t coord = ntohs(bd->cell);
                        if (level_cfg_loaded && level_cfg.cols > 0) {
                            uint16_t r = coord / level_cfg.cols;
                            uint16_t c = coord % level_cfg.cols;
                            if (r < level_cfg.rows && c < level_cfg.cols) {
                                cell_t *cl =
                                    level_cell_at(&level_cfg, r, c);
                                if (cl->type != CELL_EXPLOSION) {
                                    cl->type = CELL_EMPTY;
                                    cl->player_id = 0;
                                }
                            }
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "BLOCK_DESTROYED @ %u", coord);
                        consumed = sizeof(msg_block_destroyed_t);
                        break;
                    }
                    case MSG_BONUS_AVAILABLE: {
                        if (avail < sizeof(msg_bonus_available_t)) goto stop;
                        const msg_bonus_available_t *bn =
                            (const msg_bonus_available_t *)(buf + off);
                        uint16_t coord = ntohs(bn->cell);
                        if (level_cfg_loaded && level_cfg.cols > 0) {
                            uint16_t r = coord / level_cfg.cols;
                            uint16_t c = coord % level_cfg.cols;
                            if (r < level_cfg.rows && c < level_cfg.cols) {
                                cell_t *cl =
                                    level_cell_at(&level_cfg, r, c);
                                switch (bn->bonus_type) {
                                    case BONUS_SPEED:
                                        cl->type = CELL_BONUS_SPEED;  break;
                                    case BONUS_RADIUS:
                                        cl->type = CELL_BONUS_RADIUS; break;
                                    case BONUS_TIMER:
                                        cl->type = CELL_BONUS_TIMER;  break;
                                    case BONUS_BOMBS:
                                        cl->type = CELL_BONUS_BOMBS;  break;
                                    default: break;
                                }
                            }
                        }
                        consumed = sizeof(msg_bonus_available_t);
                        break;
                    }
                    case MSG_SYNC_BOARD: {
                        if (avail < sizeof(msg_sync_board_t)) goto stop;
                        const msg_sync_board_t *sb =
                            (const msg_sync_board_t *)(buf + off);
                        uint8_t pid = sb->player_id;
                        if (pid >= 1 && pid <= MAX_PLAYERS) {
                            players[pid].active             = true;
                            players[pid].alive              = sb->alive ? true : false;
                            players[pid].lives              = sb->lives;
                            players[pid].row                = ntohs(sb->row);
                            players[pid].col                = ntohs(sb->col);
                            players[pid].bomb_count         = sb->bomb_count;
                            players[pid].bomb_radius        = sb->bomb_radius;
                            players[pid].bomb_timer_ticks   = ntohs(sb->bomb_timer_ticks);
                            players[pid].speed              = ntohs(sb->speed);
                            players[pid].danger_extra_ticks = ntohs(sb->danger_extra_ticks);
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "SYNC_BOARD id=%u @(%u,%u) %s lives=%u",
                                 pid,
                                 players[pid].row, players[pid].col,
                                 sb->alive ? "alive" : "dead",
                                 sb->lives);
                        consumed = sizeof(msg_sync_board_t);
                        break;
                    }
                    case MSG_BONUS_RETRIEVED: {
                        if (avail < sizeof(msg_bonus_retrieved_t)) goto stop;
                        const msg_bonus_retrieved_t *br =
                            (const msg_bonus_retrieved_t *)(buf + off);
                        uint16_t coord = ntohs(br->cell);
                        if (level_cfg_loaded && level_cfg.cols > 0) {
                            uint16_t r = coord / level_cfg.cols;
                            uint16_t c = coord % level_cfg.cols;
                            if (r < level_cfg.rows && c < level_cfg.cols) {
                                cell_t *cl =
                                    level_cell_at(&level_cfg, r, c);
                                cl->type = CELL_EMPTY;
                                cl->player_id = 0;
                            }
                        }
                        snprintf(last_recv, sizeof(last_recv),
                                 "BONUS picked id=%u @ %u",
                                 br->player_id, coord);
                        consumed = sizeof(msg_bonus_retrieved_t);
                        break;
                    }
                    default:
                        snprintf(last_recv, sizeof(last_recv),
                                 "msg type=%u", hdr->msg_type);
                        consumed = sizeof(msg_generic_t);
                        break;
                }
                if (consumed == 0) break;
                off += consumed;
            }
            stop: ;
        } else if (n == 0) {
            disconnected = true;
            snprintf(status, sizeof(status), "server closed connection");
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            disconnected = true;
            snprintf(status, sizeof(status), "recv error: %s", strerror(errno));
        }

        if (!disconnected) {
            time_t now = time(NULL);
            if (!welcome_received &&
                now - hello_sent_at > WELCOME_TIMEOUT_SEC) {
                disconnected = true;
                snprintf(status, sizeof(status),
                         "no WELCOME within %ds - giving up",
                         WELCOME_TIMEOUT_SEC);
            } else if (welcome_received) {
                if (pong_pending_since != 0 &&
                    now - pong_pending_since > PONG_TIMEOUT_SEC) {
                    disconnected = true;
                    snprintf(status, sizeof(status),
                             "no PONG within %ds - peer timeout",
                             PONG_TIMEOUT_SEC);
                } else if (pong_pending_since == 0 &&
                           now - last_recv_at > IDLE_BEFORE_PING_SEC) {
                    if (send_ping(sock_fd) == 0) {
                        pong_pending_since = now;
                    }
                }
            }
        }

        draw_ui(host, port, player_name, last_recv, status);

        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            cleanup(0);
        } else if (ch == 'p' || ch == 'P') {
            if (send_ping(sock_fd) == 0) {
                if (pong_pending_since == 0)
                    pong_pending_since = time(NULL);
                snprintf(status, sizeof(status), "PING sent");
            } else {
                snprintf(status, sizeof(status),
                         "PING failed: %s", strerror(errno));
                if (errno == EPIPE || errno == ECONNRESET ||
                    errno == EBADF  || errno == ENOTCONN) {
                    disconnected = true;
                }
            }
        } else if (ch == 'm' || ch == 'M') {
            if (level_cfg_loaded) {
                preview_level();
            } else {
                snprintf(status, sizeof(status), "No map loaded yet");
            }
        } else if (ch == ' ' && game_state == GAME_END) {
            if (send_set_status(sock_fd, GAME_LOBBY) != 0) {
                snprintf(status, sizeof(status),
                         "Return-to-lobby failed: %s", strerror(errno));
                if (errno == EPIPE || errno == ECONNRESET ||
                    errno == EBADF  || errno == ENOTCONN) {
                    disconnected = true;
                }
            } else {
                snprintf(status, sizeof(status), "Returning to lobby...");
            }
        } else if (ch == ' ' && game_state == GAME_LOBBY) {
            if (me_ready) {
                snprintf(status, sizeof(status), "Already ready");
            } else if (send_set_ready(sock_fd) == 0) {
                me_ready = true;
                snprintf(status, sizeof(status),
                         "Ready - waiting for others");
            } else {
                snprintf(status, sizeof(status),
                         "SET_READY failed: %s", strerror(errno));
                if (errno == EPIPE || errno == ECONNRESET ||
                    errno == EBADF  || errno == ENOTCONN) {
                    disconnected = true;
                }
            }
        } else if (game_state == GAME_RUNNING) {
            char dir = 0;
            if      (ch == KEY_UP    || ch == 'w' || ch == 'W') dir = DIR_UP;
            else if (ch == KEY_DOWN  || ch == 's' || ch == 'S') dir = DIR_DOWN;
            else if (ch == KEY_LEFT  || ch == 'a' || ch == 'A') dir = DIR_LEFT;
            else if (ch == KEY_RIGHT || ch == 'd' || ch == 'D') dir = DIR_RIGHT;
            if (dir != 0) {
                if (send_move_attempt(sock_fd, dir) != 0) {
                    snprintf(status, sizeof(status),
                             "MOVE failed: %s", strerror(errno));
                    if (errno == EPIPE || errno == ECONNRESET ||
                        errno == EBADF  || errno == ENOTCONN) {
                        disconnected = true;
                    }
                }
            } else if (ch == ' ' && my_player_id != ID_UNKNOWN &&
                       my_player_id >= 1 && my_player_id <= MAX_PLAYERS &&
                       level_cfg_loaded) {
                uint16_t cell = (uint16_t)(
                    players[my_player_id].row * level_cfg.cols +
                    players[my_player_id].col);
                if (send_bomb_attempt(sock_fd, cell) != 0) {
                    snprintf(status, sizeof(status),
                             "BOMB failed: %s", strerror(errno));
                    if (errno == EPIPE || errno == ECONNRESET ||
                        errno == EBADF  || errno == ENOTCONN) {
                        disconnected = true;
                    }
                }
            }
        }

        usleep(20000);
    }

    if (sock_fd != -1) {
        /* Voluntary close — let the server know we're going. */
        send_leave(sock_fd);
        close(sock_fd);
        sock_fd = -1;
    }
    snprintf(menu_status, sizeof(menu_status),
             "Disconnected: %s", status);
    menu_status_pair = PAIR_STATUS;
    sel = 0;
    reset_session_state();
    }

    cleanup(0);
    return 0;
}
