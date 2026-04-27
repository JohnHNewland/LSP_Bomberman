#include "protocol.h"

uint16_t make_cell_index(uint16_t row, uint16_t col, uint16_t cols) {
    return (uint16_t)(row * cols + col);
}

void split_cell_index(uint16_t index, uint16_t cols,
                      uint16_t *row, uint16_t *col) {
    *row = (uint16_t)(index / cols);
    *col = (uint16_t)(index % cols);
}
