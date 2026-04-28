#include "protocol.h"

uint16_t make_cell_index(uint16_t row, uint16_t col, uint16_t cols) {
    return (uint16_t)(row * cols + col);
}

void split_cell_index(uint16_t index, uint16_t cols,
                      uint16_t *row, uint16_t *col) {
    *row = (uint16_t)(index / cols);
    *col = (uint16_t)(index % cols);
}

bool dir_delta(uint8_t dir, int *dr, int *dc) {
    switch (dir) {
        case DIR_UP:    *dr = -1; *dc =  0; return true;
        case DIR_DOWN:  *dr =  1; *dc =  0; return true;
        case DIR_LEFT:  *dr =  0; *dc = -1; return true;
        case DIR_RIGHT: *dr =  0; *dc =  1; return true;
        default: return false;
    }
}
