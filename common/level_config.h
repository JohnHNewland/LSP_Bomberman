#ifndef LEVEL_CONFIG_H
#define LEVEL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LEVEL_MAX_DIM 255
#define LEVEL_MAX_PLAYERS 8
#define LEVEL_DIR "maps"
#define LEVEL_NAME_MAX 64
#define LEVEL_LIST_MAX 32

typedef enum {
    CELL_EMPTY = 0,
    CELL_HARD_BLOCK,
    CELL_SOFT_BLOCK,
    CELL_BOMB,
    CELL_PLAYER_START,
    CELL_BONUS_SPEED,
    CELL_BONUS_RADIUS,
    CELL_BONUS_TIMER,
    CELL_BONUS_BOMBS
} cell_type_t;

typedef struct {
    cell_type_t type;
    uint8_t     player_id;
} cell_t;

typedef struct {
    uint8_t  rows;
    uint8_t  cols;
    uint16_t speed;
    uint16_t danger_ticks;
    uint8_t  radius;
    uint16_t bomb_timer_ticks;
    cell_t  *cells;
} level_config_t;

typedef struct {
    char name[LEVEL_NAME_MAX];
} level_entry_t;

cell_t *level_cell_at(const level_config_t *cfg, int r, int c);
char    level_cell_char(const cell_t *cell);
bool    level_parse_cell(char ch, cell_t *out);
int     level_config_load(const char *path, level_config_t *out,
                          char *err, size_t err_len);
void    level_config_free(level_config_t *cfg);
int     level_entry_cmp(const void *a, const void *b);
int     level_list_dir(const char *dir_path, level_entry_t *out, int cap);

#endif
