#ifndef MELODY_PARSER_H
#define MELODY_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define MELODY_MAX_TEXT 300
#define MELODY_MAX_STEPS 24
#define MELODY_MIN_MS 10
#define MELODY_MAX_MS 2000
#define MELODY_MAX_TOTAL_MS 15000

typedef struct {
    uint16_t freq_hz; /* 0 represents a rest */
    uint16_t duration_ms;
} melody_step_t;

typedef struct {
    melody_step_t steps[MELODY_MAX_STEPS];
    uint8_t count;
    uint32_t total_ms;
} melody_t;

typedef enum {
    MELODY_ERROR_OK = 0,
    MELODY_ERROR_LENGTH = 1,
    MELODY_ERROR_SYNTAX = 2,
    MELODY_ERROR_STEP_COUNT = 3,
    MELODY_ERROR_DURATION = 4,
    MELODY_ERROR_NOTE = 5,
    MELODY_ERROR_TOTAL_DURATION = 6,
    MELODY_ERROR_SEQUENCE = 7,
    MELODY_ERROR_UNKNOWN_OP = 8,
} melody_error_t;

/* Parse a bounded ASCII melody. The output changes only when parsing succeeds. */
melody_error_t melody_parse(const char *text, size_t len, melody_t *out);

#endif
