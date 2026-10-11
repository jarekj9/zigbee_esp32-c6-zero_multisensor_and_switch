#include "melody_upload.h"

#include <string.h>

void melody_upload_reset(melody_upload_t *upload)
{
    memset(upload, 0, sizeof(*upload));
}

melody_error_t melody_upload_start(melody_upload_t *upload)
{
    upload->state = MELODY_UPLOAD_OPEN;
    upload->len = 0;
    upload->text[0] = '\0';
    upload->error = MELODY_ERROR_OK;
    upload->has_commit_result = false;
    upload->commit_result = MELODY_ERROR_OK;
    return MELODY_ERROR_OK;
}

static void melody_upload_fail(melody_upload_t *upload, melody_error_t error)
{
    upload->state = MELODY_UPLOAD_FAILED;
    upload->len = 0;
    upload->text[0] = '\0';
    upload->error = error;
}

static void melody_upload_finish_commit(melody_upload_t *upload, melody_error_t result)
{
    upload->state = MELODY_UPLOAD_IDLE;
    upload->len = 0;
    upload->text[0] = '\0';
    upload->error = result;
    upload->has_commit_result = true;
    upload->commit_result = result;
}

melody_error_t melody_upload_append(melody_upload_t *upload, const char *chunk, size_t len)
{
    if (upload->state != MELODY_UPLOAD_OPEN) {
        return MELODY_ERROR_SEQUENCE;
    }
    if (chunk == NULL || len == 0 || len > MELODY_CHUNK_MAX ||
        len > (size_t)MELODY_MAX_TEXT - (size_t)upload->len) {
        melody_upload_fail(upload, MELODY_ERROR_LENGTH);
        return MELODY_ERROR_LENGTH;
    }

    memcpy(&upload->text[upload->len], chunk, len);
    upload->len = (uint16_t)(upload->len + len);
    upload->text[upload->len] = '\0';
    return MELODY_ERROR_OK;
}

void melody_upload_abort(melody_upload_t *upload)
{
    melody_error_t previous_error = upload->error;
    upload->state = MELODY_UPLOAD_IDLE;
    upload->len = 0;
    upload->text[0] = '\0';
    upload->error = previous_error;
}

melody_error_t melody_upload_commit(melody_upload_t *upload, melody_t *out_melody, bool *out_clear)
{
    melody_error_t error;

    if (out_clear == NULL) {
        return MELODY_ERROR_LENGTH;
    }
    *out_clear = false;

    if (upload->state == MELODY_UPLOAD_IDLE) {
        return upload->has_commit_result ? upload->commit_result : MELODY_ERROR_SEQUENCE;
    }
    if (upload->state == MELODY_UPLOAD_FAILED) {
        error = upload->error;
        error = error == MELODY_ERROR_OK ? MELODY_ERROR_SEQUENCE : error;
        melody_upload_finish_commit(upload, error);
        return error;
    }

    if (upload->len == 0) {
        *out_clear = true;
        error = MELODY_ERROR_OK;
    } else if (out_melody == NULL) {
        return MELODY_ERROR_LENGTH;
    } else {
        error = melody_parse(upload->text, upload->len, out_melody);
    }

    melody_upload_finish_commit(upload, error);
    return error;
}

melody_error_t melody_upload_control(melody_upload_t *upload, uint8_t control,
                                     melody_t *out_melody, bool *out_clear)
{
    switch (control) {
    case MELODY_CTRL_START:
        return melody_upload_start(upload);
    case MELODY_CTRL_COMMIT:
        return melody_upload_commit(upload, out_melody, out_clear);
    case MELODY_CTRL_ABORT:
        melody_upload_abort(upload);
        return MELODY_ERROR_OK;
    default:
        return MELODY_ERROR_UNKNOWN_OP;
    }
}
