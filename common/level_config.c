#include "level_config.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cell_t *level_cell_at(const level_config_t *cfg, int r, int c) {
    return &cfg->cells[r * cfg->cols + c];
}

char level_cell_char(const cell_t *cell) {
    switch (cell->type) {
        case CELL_EMPTY:        return '.';
        case CELL_HARD_BLOCK:   return 'H';
        case CELL_SOFT_BLOCK:   return 'S';
        case CELL_BOMB:         return 'B';
        case CELL_PLAYER_START: return (char)('0' + cell->player_id);
        case CELL_BONUS_SPEED:  return 'A';
        case CELL_BONUS_RADIUS: return 'R';
        case CELL_BONUS_TIMER:  return 'T';
        case CELL_BONUS_BOMBS:  return 'N';
    }
    return '?';
}

bool level_parse_cell(char ch, cell_t *out) {
    if (ch >= '1' && ch <= '8') {
        out->type = CELL_PLAYER_START;
        out->player_id = (uint8_t)(ch - '0');
        return true;
    }
    out->player_id = 0;
    switch (ch) {
        case '.': out->type = CELL_EMPTY;        return true;
        case 'H': out->type = CELL_HARD_BLOCK;   return true;
        case 'S': out->type = CELL_SOFT_BLOCK;   return true;
        case 'B': out->type = CELL_BOMB;         return true;
        case 'A': out->type = CELL_BONUS_SPEED;  return true;
        case 'R': out->type = CELL_BONUS_RADIUS; return true;
        case 'T': out->type = CELL_BONUS_TIMER;  return true;
        case 'N': out->type = CELL_BONUS_BOMBS;  return true;
        default:                                  return false;
    }
}

void level_config_free(level_config_t *cfg) {
    if (!cfg) return;
    free(cfg->cells);
    cfg->cells = NULL;
    cfg->rows = cfg->cols = 0;
}

int level_config_load(const char *path, level_config_t *out,
                      char *err, size_t err_len) {
    if (!path || !out) {
        if (err && err_len) snprintf(err, err_len, "null arguments");
        return -1;
    }
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) {
        if (err && err_len) snprintf(err, err_len, "cannot open '%s'", path);
        return -1;
    }

    unsigned rows, cols, speed, danger, radius, timer;
    int got = fscanf(f, "%u %u %u %u %u %u", &rows, &cols, &speed,
                     &danger, &radius, &timer);
    if (got != 6) {
        if (err && err_len)
            snprintf(err, err_len, "header needs 6 numbers (got %d)", got);
        fclose(f);
        return -1;
    }
    if (rows == 0 || cols == 0 ||
        rows > LEVEL_MAX_DIM || cols > LEVEL_MAX_DIM) {
        if (err && err_len)
            snprintf(err, err_len, "bad size %ux%u (max %dx%d)",
                     rows, cols, LEVEL_MAX_DIM, LEVEL_MAX_DIM);
        fclose(f);
        return -1;
    }

    out->rows = (uint8_t)rows;
    out->cols = (uint8_t)cols;
    out->speed = (uint16_t)speed;
    out->danger_ticks = (uint16_t)danger;
    out->radius = (uint8_t)radius;
    out->bomb_timer_ticks = (uint16_t)timer;
    out->cells = (cell_t *)calloc((size_t)rows * cols, sizeof(cell_t));
    if (!out->cells) {
        if (err && err_len) snprintf(err, err_len, "out of memory");
        fclose(f);
        return -1;
    }

    int seen_players[LEVEL_MAX_PLAYERS + 1] = {0};
    size_t need = (size_t)rows * cols;
    size_t filled = 0;
    int c;
    while (filled < need && (c = fgetc(f)) != EOF) {
        if (isspace((unsigned char)c)) continue;
        cell_t cell;
        if (!level_parse_cell((char)c, &cell)) {
            if (err && err_len)
                snprintf(err, err_len,
                         "invalid cell '%c' at index %zu", (char)c, filled);
            level_config_free(out);
            fclose(f);
            return -1;
        }
        if (cell.type == CELL_PLAYER_START) {
            if (seen_players[cell.player_id]) {
                if (err && err_len)
                    snprintf(err, err_len,
                             "duplicate start for player %u", cell.player_id);
                level_config_free(out);
                fclose(f);
                return -1;
            }
            seen_players[cell.player_id] = 1;
        }
        out->cells[filled++] = cell;
    }
    fclose(f);

    if (filled != need) {
        if (err && err_len)
            snprintf(err, err_len, "expected %zu cells, got %zu", need, filled);
        level_config_free(out);
        return -1;
    }
    return 0;
}

int level_entry_cmp(const void *a, const void *b) {
    return strcmp(((const level_entry_t *)a)->name,
                  ((const level_entry_t *)b)->name);
}

int level_list_dir(const char *dir_path, level_entry_t *out, int cap) {
    DIR *d = opendir(dir_path);
    if (!d) return -1;
    int n = 0;
    struct dirent *de;
    while (n < cap && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len >= LEVEL_NAME_MAX) continue;
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcmp(dot, ".map") != 0) continue;
        memcpy(out[n].name, de->d_name, len + 1);
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof(level_entry_t), level_entry_cmp);
    return n;
}
