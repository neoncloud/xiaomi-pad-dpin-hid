#ifndef DIGITIZER_REPORTS_H
#define DIGITIZER_REPORTS_H

#include <stdbool.h>
#include <stdint.h>

enum {
    DIGITIZER_TOUCH_ID = 1,
    DIGITIZER_PEN_ID = 3,
    DIGITIZER_SLOTS = 10,
    STOCK_TOUCH_BYTES = 62,
    STOCK_PEN_BYTES = 10,
    DIGITIZER_TOUCH_BYTES = STOCK_TOUCH_BYTES,
    DIGITIZER_PEN_BYTES = 12,
    DIGITIZER_SWIPE_ID = 4,
    DIGITIZER_SWIPE_BYTES = 9,
    DIGITIZER_BARREL = 1u << 1,
    DIGITIZER_SECONDARY_BARREL = 1u << 4,
    DIGITIZER_STOCK_PEN_PRESSURE_MAX = 8191,
    DIGITIZER_PEN_PRESSURE_MAX = 16383,
};

typedef struct {
    uint16_t x;
    uint16_t y;
    bool down;
    bool confidence;
} digitizer_contact;

static uint16_t digitizer_u16(const uint8_t *data) {
    return (uint16_t)(data[0] | (data[1] << 8));
}

static void digitizer_put_u16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void digitizer_touch_record(uint8_t out[6], unsigned int flags, unsigned int id,
                                   const digitizer_contact *point) {
    out[0] = (uint8_t)(flags | (point->confidence << 2));
    out[1] = (uint8_t)id;
    digitizer_put_u16(out + 2, point->x);
    digitizer_put_u16(out + 4, point->y);
}

#endif
