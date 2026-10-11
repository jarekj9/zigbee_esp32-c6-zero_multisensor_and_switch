#ifndef MELODY_UPLOAD_H
#define MELODY_UPLOAD_H

#include "melody_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MELODY_CHUNK_MAX 79

typedef enum {
    MELODY_UPLOAD_IDLE,
    MELODY_UPLOAD_OPEN,
    MELODY_UPLOAD_FAILED,
} melody_upload_state_t;

typedef enum {
    MELODY_CTRL_START = 1,
    MELODY_CTRL_COMMIT = 2,
    MELODY_CTRL_ABORT = 3,
} melody_ctrl_t;

typedef struct {
    melody_upload_state_t state;
    uint16_t len;
    char text[MELODY_MAX_TEXT + 1];
    melody_error_t error;
    bool has_commit_result;
    melody_error_t commit_result;
} melody_upload_t;

void melody_upload_reset(melody_upload_t *upload);
melody_error_t melody_upload_start(melody_upload_t *upload);
melody_error_t melody_upload_append(melody_upload_t *upload, const char *chunk, size_t len);
void melody_upload_abort(melody_upload_t *upload);
melody_error_t melody_upload_commit(melody_upload_t *upload, melody_t *out_melody, bool *out_clear);
melody_error_t melody_upload_control(melody_upload_t *upload, uint8_t control,
                                     melody_t *out_melody, bool *out_clear);

#endif
