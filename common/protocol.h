#ifndef BBM_PROTOCOL_H
#define BBM_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#define ID_SERVER     0xFF
#define ID_BROADCAST  0xFE
#define ID_UNKNOWN    0xFF

#define CLIENT_ID_LEN     20
#define SERVER_ID_LEN     20
#define PLAYER_NAME_LEN   30
#define MAX_NAME_LEN      15
#define MAX_PLAYERS       8
#define TICKS_PER_SECOND  20

#define IDLE_BEFORE_PING_SEC  15
#define PONG_TIMEOUT_SEC      30

typedef enum {
    GAME_LOBBY   = 0,
    GAME_RUNNING = 1,
    GAME_END     = 2
} game_status_t;

typedef enum {
    DIR_UP    = 'U',
    DIR_DOWN  = 'D',
    DIR_LEFT  = 'L',
    DIR_RIGHT = 'R'
} direction_t;

typedef enum {
    BONUS_NONE   = 0,
    BONUS_SPEED  = 1,
    BONUS_RADIUS = 2,
    BONUS_TIMER  = 3,
    /* Extends the spec's bonus enum: the spec body lists "+1 bomb" as a valid
     * bonus and the map syntax uses 'N' for it, but the spec's typedef forgot
     * to enumerate it. Wire byte for the type field is 4. */
    BONUS_BOMBS  = 4
} bonus_type_t;

typedef enum {
    MSG_HELLO            = 0,
    MSG_WELCOME          = 1,
    MSG_DISCONNECT       = 2,
    MSG_PING             = 3,
    MSG_PONG             = 4,
    MSG_LEAVE            = 5,
    MSG_ERROR            = 6,
    MSG_MAP              = 7,
    MSG_SET_READY        = 10,
    MSG_SET_STATUS       = 20,
    MSG_WINNER           = 23,
    MSG_MOVE_ATTEMPT     = 30,
    MSG_BOMB_ATTEMPT     = 31,
    MSG_MOVED            = 40,
    MSG_BOMB             = 41,
    MSG_EXPLOSION_START  = 42,
    MSG_EXPLOSION_END    = 43,
    MSG_DEATH            = 44,
    /* Reserved for hidden-bonus reveals. Bonuses are currently placed at
     * map-load time and are visible to all clients via MSG_MAP, so the
     * server never emits this. The client has a handler ready for when
     * hidden bonuses ship. */
    MSG_BONUS_AVAILABLE  = 45,
    MSG_BONUS_RETRIEVED  = 46,
    MSG_BLOCK_DESTROYED  = 47,
    MSG_SYNC_BOARD       = 100,
    MSG_SYNC_REQUEST     = 101,
    /* Lobby-only, leader-only map picker. Not in protokols.docx (the spec
     * mandates leader-driven map selection but leaves the wire format
     * open). Uses IDs above the spec's range so it doesn't collide with
     * future spec additions. */
    MSG_MAP_LIST_REQUEST = 110,
    MSG_MAP_LIST         = 111,
    MSG_MAP_SELECT       = 112
} msg_type_t;

typedef struct {
    uint8_t msg_type;  // ziņas tips, kas nosaka datu struktūru
    uint8_t sender_id;
    uint8_t target_id; // adresāta ID. 255=server. 254=broadcast.
    // payload
} msg_generic_t;

#define HEADER_LEN ((int)sizeof(msg_generic_t))

typedef struct {
    msg_generic_t hdr;
    char          client_id[CLIENT_ID_LEN];
    char          player_name[PLAYER_NAME_LEN];
} msg_hello_t;

typedef struct {
    msg_generic_t hdr;
    char          server_id[SERVER_ID_LEN];
    uint8_t       game_status;
    uint8_t       length;
} msg_welcome_t;

typedef struct {
    uint8_t id;
    uint8_t ready;
    char    name[PLAYER_NAME_LEN];
} msg_other_client_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       H;
    uint8_t       W;
} msg_map_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       game_status;
} msg_set_status_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       winner_id;
} msg_winner_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       direction;
} msg_move_attempt_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       player_id;
    uint16_t      coord;
} msg_moved_t;

typedef struct {
    msg_generic_t hdr;
    uint16_t      cell;
} msg_bomb_attempt_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       player_id;
    uint16_t      cell;
} msg_bomb_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       radius;
    uint16_t      cell;
} msg_explosion_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       player_id;
} msg_death_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       bonus_type;
    uint16_t      cell;
} msg_bonus_available_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       player_id;
    uint16_t      cell;
} msg_bonus_retrieved_t;

typedef struct {
    msg_generic_t hdr;
    uint16_t      cell;
} msg_block_destroyed_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t  player_id;
    uint8_t  alive;
    uint8_t  lives;
    uint16_t row;
    uint16_t col;
    uint8_t  bomb_count;
    uint8_t  bomb_radius;
    uint16_t bomb_timer_ticks;
    uint16_t speed;
} msg_sync_board_t;

#define MAP_NAME_LEN 64

/* One entry per available map in the server's maps/ folder. Per spec
 * (protokols.docx, "Serveris nodod klientam... laukuma izmērs,
 * spēlētāju skaits, laukuma izkārtojums"): size and player count are
 * included here so the leader can pick informed; the layout itself
 * arrives in MAP after the leader sends MAP_SELECT. */
typedef struct {
    char     name[MAP_NAME_LEN];
    uint8_t  rows;
    uint8_t  cols;
    uint8_t  max_players;
} msg_map_entry_t;

typedef struct {
    msg_generic_t hdr;
    uint8_t       count;
    /* Followed by count * msg_map_entry_t. */
} msg_map_list_t;

typedef struct {
    msg_generic_t hdr;
    char          name[MAP_NAME_LEN];
} msg_map_select_t;

typedef struct {
    uint8_t  id;
    uint8_t  lives;
    char     name[MAX_NAME_LEN + 1];
    uint16_t row;
    uint16_t col;
    bool     alive;
    bool     ready;
    uint8_t  bomb_count;
    uint8_t  bomb_radius;
    uint16_t bomb_timer_ticks;
    uint16_t speed;
    /* Extra explosion-lingering ticks granted by BONUS_TIMER pickups.
     * Added to the level's danger_ticks when this player's bomb detonates. */
    uint16_t danger_extra_ticks;
} player_t;

typedef struct {
    uint8_t  owner_id;
    uint16_t row;
    uint16_t col;
    uint8_t  radius;
    uint16_t timer_ticks;
} bomb_t;

uint16_t make_cell_index(uint16_t row, uint16_t col, uint16_t cols);
void     split_cell_index(uint16_t index, uint16_t cols,
                          uint16_t *row, uint16_t *col);
bool     dir_delta(uint8_t dir, int *dr, int *dc);

#endif
