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
#include <ncurses.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 7890
#define BUFFER_SIZE 256

#define CLIENT_ID_LEN 20
#define PLAYER_NAME_LEN 30
#define HEADER_LEN 3
#define ID_UNKNOWN 0xFF
#define ID_SERVER 0xFF
#define HELLO_MSG_LEN (HEADER_LEN + CLIENT_ID_LEN + PLAYER_NAME_LEN)

#define CLIENT_ID_STR "bbm-client-0.1"
#define DEFAULT_PLAYER_NAME "player"

#define PAIR_TITLE    1
#define PAIR_BORDER   2
#define PAIR_HOTKEY   3
#define PAIR_SELECTED 4
#define PAIR_NORMAL   5
#define PAIR_STATUS   6
#define PAIR_INFO     7

typedef enum {
    MSG_HELLO = 0,
    MSG_WELCOME = 1,
    MSG_DISCONNECT = 2,
    MSG_PING = 3,
    MSG_PONG = 4,
    MSG_LEAVE = 5,
    MSG_ERROR = 6
} msg_type_t;

static int sock_fd = -1;
static volatile sig_atomic_t running = 1;

static const char *BANNER[] = {
    " ____     ___    __  __   ____    _____   ____    __  __      _      _   _ ",
    "| __ )   / _ \\  |  \\/  | | __ )  | ____| |  _ \\  |  \\/  |    / \\    | \\ | |",
    "|  _ \\  | | | | | |\\/| | |  _ \\  |  _|   | |_) | | |\\/| |   / _ \\   |  \\| |",
    "| |_) | | |_| | | |  | | | |_) | | |___  |  _ <  | |  | |  / ___ \\  | |\\  |",
    "|____/   \\___/  |_|  |_| |____/  |_____| |_| \\_\\ |_|  |_| /_/   \\_\\ |_| \\_|",
};
#define BANNER_H ((int)(sizeof(BANNER) / sizeof(BANNER[0])))

static void cleanup(int sig) {
    (void)sig;
    if (sock_fd != -1) close(sock_fd);
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
}

static void init_ncurses(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    init_colors();
    clear();
}

static int connect_to_server(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static int send_hello(int fd, const char *player_name) {
    uint8_t msg[HELLO_MSG_LEN] = {0};
    msg[0] = MSG_HELLO;
    msg[1] = ID_UNKNOWN;
    msg[2] = ID_SERVER;
    size_t cid_len = strnlen(CLIENT_ID_STR, CLIENT_ID_LEN);
    size_t name_len = strnlen(player_name, PLAYER_NAME_LEN);
    memcpy(&msg[HEADER_LEN], CLIENT_ID_STR, cid_len);
    memcpy(&msg[HEADER_LEN + CLIENT_ID_LEN], player_name, name_len);
    return (write(fd, msg, HELLO_MSG_LEN) == HELLO_MSG_LEN) ? 0 : -1;
}

static int send_ping(int fd) {
    uint8_t msg[HEADER_LEN];
    msg[0] = MSG_PING;
    msg[1] = ID_UNKNOWN;
    msg[2] = ID_SERVER;
    return (write(fd, msg, sizeof(msg)) == (ssize_t)sizeof(msg)) ? 0 : -1;
}

static ssize_t poll_recv(int fd, char *buf, size_t cap) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret <= 0) return 0;
    return read(fd, buf, cap - 1);
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

#define MENU_ITEM_COUNT 3
static const char *MENU_ITEMS[MENU_ITEM_COUNT] = {
    "Join server",
    "Edit player name",
    "Quit",
};
static const char MENU_HOTKEYS[MENU_ITEM_COUNT] = { 'j', 'n', 'q' };

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

    draw_status_bar(" [Up/Down] Navigate   [Enter] Select   [n] Name   [j] Join   [q] Quit",
                    status, status_pair);
    refresh();
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
    mvaddstr(y, x, "[q]");
    attroff(COLOR_PAIR(PAIR_HOTKEY) | A_BOLD);
    attron(COLOR_PAIR(PAIR_NORMAL));
    mvaddstr(y++, x + 4, "Quit");
    attroff(COLOR_PAIR(PAIR_NORMAL));

    draw_status_bar(" [p] Send PING   [q] Quit", status, PAIR_STATUS);
    refresh();
}

int main(int argc, char *argv[]) {
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    const char *initial_name = DEFAULT_PLAYER_NAME;

    if (argc >= 2) {
        char *colon = strchr(argv[1], ':');
        if (colon) {
            *colon = '\0';
            host = argv[1];
            port = atoi(colon + 1);
        } else {
            host = argv[1];
        }
    }
    if (argc >= 3) initial_name = argv[2];
    if (argc > 3) {
        printf("Usage: %s [IP[:PORT]] [PLAYER_NAME]\n", argv[0]);
        return 1;
    }

    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    init_ncurses();

    char player_name[PLAYER_NAME_LEN + 1];
    strncpy(player_name, initial_name, sizeof(player_name) - 1);
    player_name[sizeof(player_name) - 1] = '\0';

    char menu_status[128] = "";
    int menu_status_pair = PAIR_STATUS;
    int sel = 0;
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
            case 'n': case 'N':
                activate_idx = 1;
                break;
            case 'q': case 'Q':
                activate_idx = 2;
                break;
            default:
                break;
        }

        if (activate_idx == 0) {
            sel = 0;
            snprintf(menu_status, sizeof(menu_status),
                     "Connecting to %s:%d...", host, port);
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
            }
        } else if (activate_idx == 1) {
            sel = 1;
            edit_name(player_name, sizeof(player_name));
        } else if (activate_idx == 2) {
            cleanup(0);
        }

        usleep(20000);
    }

    char last_recv[BUFFER_SIZE] = "(none)";
    char status[64] = "connected";

    while (running) {
        char buf[BUFFER_SIZE];
        ssize_t n = poll_recv(sock_fd, buf, sizeof(buf));
        if (n > 0) {
            if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;
            buf[n] = '\0';
            strncpy(last_recv, buf, sizeof(last_recv) - 1);
            last_recv[sizeof(last_recv) - 1] = '\0';
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            snprintf(status, sizeof(status), "disconnected");
        }

        draw_ui(host, port, player_name, last_recv, status);

        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            cleanup(0);
        } else if (ch == 'p' || ch == 'P') {
            if (send_ping(sock_fd) == 0) {
                snprintf(status, sizeof(status), "PING sent");
            } else {
                snprintf(status, sizeof(status), "PING failed: %s", strerror(errno));
            }
        }

        usleep(20000);
    }

    cleanup(0);
    return 0;
}
