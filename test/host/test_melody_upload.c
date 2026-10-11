#include "melody_upload.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_MELODY "E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120 R:350 G4:120 R:350"

static void append_range(melody_upload_t *upload, size_t start, size_t end)
{
    while (start < end) {
        size_t chunk_len = end - start;
        if (chunk_len > MELODY_CHUNK_MAX) chunk_len = MELODY_CHUNK_MAX;
        assert(melody_upload_append(upload, &DEFAULT_MELODY[start], chunk_len) == MELODY_ERROR_OK);
        start += chunk_len;
    }
}

static void test_split_upload(void)
{
    const size_t text_len = strlen(DEFAULT_MELODY);
    melody_t expected;
    assert(melody_parse(DEFAULT_MELODY, text_len, &expected) == MELODY_ERROR_OK);

    for (size_t split_at = 1; split_at < text_len; ++split_at) {
        melody_upload_t upload;
        melody_t actual;
        bool clear = true;
        melody_upload_reset(&upload);
        assert(melody_upload_start(&upload) == MELODY_ERROR_OK);

        append_range(&upload, 0, split_at);
        append_range(&upload, split_at, text_len);

        assert(melody_upload_commit(&upload, &actual, &clear) == MELODY_ERROR_OK);
        assert(!clear);
        assert(memcmp(&expected, &actual, sizeof(expected)) == 0);
        assert(upload.state == MELODY_UPLOAD_IDLE && upload.len == 0);
        melody_t untouched_retry = {0};
        memset(&actual, 0xa5, sizeof(actual));
        untouched_retry = actual;
        bool retry_clear = true;
        assert(melody_upload_commit(&upload, &actual, &retry_clear) == MELODY_ERROR_OK);
        assert(!retry_clear && memcmp(&untouched_retry, &actual, sizeof(actual)) == 0);
    }
}

static void test_chunk_bounds_and_sequence(void)
{
    melody_upload_t upload;
    melody_t melody;
    bool clear = false;
    char chunk[MELODY_CHUNK_MAX + 1];
    memset(chunk, 'x', sizeof(chunk));

    melody_upload_reset(&upload);
    assert(melody_upload_append(&upload, "C5:100", 6) == MELODY_ERROR_SEQUENCE);
    assert(melody_upload_append(&upload, "", 0) == MELODY_ERROR_SEQUENCE);
    assert(melody_upload_control(&upload, MELODY_CTRL_COMMIT, &melody, &clear) == MELODY_ERROR_SEQUENCE);
    assert(melody_upload_start(&upload) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, chunk, MELODY_CHUNK_MAX) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, chunk, MELODY_CHUNK_MAX + 1) == MELODY_ERROR_LENGTH);
    assert(upload.state == MELODY_UPLOAD_FAILED && upload.len == 0);
    assert(melody_upload_append(&upload, "C5:100", 6) == MELODY_ERROR_SEQUENCE);
    assert(melody_upload_commit(&upload, &melody, &clear) == MELODY_ERROR_LENGTH);
    assert(!clear && upload.state == MELODY_UPLOAD_IDLE);

    assert(melody_upload_start(&upload) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, "", 0) == MELODY_ERROR_LENGTH);
    assert(upload.state == MELODY_UPLOAD_FAILED);
    assert(melody_upload_start(&upload) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, NULL, 0) == MELODY_ERROR_LENGTH);
    assert(upload.state == MELODY_UPLOAD_FAILED);
    assert(melody_upload_start(&upload) == MELODY_ERROR_OK);
    assert(upload.len == 0 && upload.error == MELODY_ERROR_OK);
}

static void test_total_length_and_failed_parse(void)
{
    melody_upload_t upload;
    melody_t output;
    melody_t untouched;
    bool clear = false;
    char chunk[MELODY_CHUNK_MAX];
    memset(chunk, 'X', sizeof(chunk));
    memset(&output, 0x5a, sizeof(output));
    untouched = output;

    melody_upload_reset(&upload);
    assert(melody_upload_start(&upload) == MELODY_ERROR_OK);
    for (int i = 0; i < 3; ++i) {
        assert(melody_upload_append(&upload, chunk, 79) == MELODY_ERROR_OK);
    }
    assert(melody_upload_append(&upload, chunk, 63) == MELODY_ERROR_OK);
    assert(upload.len == MELODY_MAX_TEXT && upload.text[MELODY_MAX_TEXT] == '\0');
    assert(melody_upload_append(&upload, "X", 1) == MELODY_ERROR_LENGTH);
    assert(upload.state == MELODY_UPLOAD_FAILED);

    assert(melody_upload_start(&upload) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, "C5:9", 4) == MELODY_ERROR_OK);
    assert(melody_upload_commit(&upload, &output, &clear) == MELODY_ERROR_DURATION);
    assert(!clear && memcmp(&output, &untouched, sizeof(output)) == 0);
    assert(melody_upload_control(&upload, MELODY_CTRL_ABORT, &output, &clear) == MELODY_ERROR_OK);
    assert(melody_upload_commit(&upload, &output, &clear) == MELODY_ERROR_DURATION);
    assert(memcmp(&output, &untouched, sizeof(output)) == 0);
}

static void test_clear_abort_and_unknown_control(void)
{
    melody_upload_t upload;
    melody_t output;
    bool clear = false;

    melody_upload_reset(&upload);
    assert(melody_upload_control(&upload, 0, &output, &clear) == MELODY_ERROR_UNKNOWN_OP);
    assert(upload.state == MELODY_UPLOAD_IDLE);
    assert(melody_upload_control(&upload, 4, &output, &clear) == MELODY_ERROR_UNKNOWN_OP);
    assert(melody_upload_control(&upload, 255, &output, &clear) == MELODY_ERROR_UNKNOWN_OP);
    assert(melody_upload_control(&upload, MELODY_CTRL_START, &output, &clear) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, "discard me", 10) == MELODY_ERROR_OK);
    assert(melody_upload_control(&upload, MELODY_CTRL_START, &output, &clear) == MELODY_ERROR_OK);
    assert(upload.state == MELODY_UPLOAD_OPEN && upload.len == 0 && upload.text[0] == '\0');
    assert(melody_upload_control(&upload, MELODY_CTRL_COMMIT, &output, &clear) == MELODY_ERROR_OK);
    assert(clear && upload.state == MELODY_UPLOAD_IDLE);
    assert(melody_upload_control(&upload, MELODY_CTRL_COMMIT, &output, &clear) == MELODY_ERROR_OK);
    assert(!clear && upload.state == MELODY_UPLOAD_IDLE);
    assert(melody_upload_control(&upload, MELODY_CTRL_ABORT, &output, &clear) == MELODY_ERROR_OK);
    assert(melody_upload_control(&upload, MELODY_CTRL_COMMIT, &output, &clear) == MELODY_ERROR_OK);
    assert(!clear && upload.state == MELODY_UPLOAD_IDLE);

    assert(melody_upload_control(&upload, MELODY_CTRL_START, &output, &clear) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, "discard", 7) == MELODY_ERROR_OK);
    melody_error_t open_error = upload.error;
    assert(melody_upload_control(&upload, MELODY_CTRL_ABORT, &output, &clear) == MELODY_ERROR_OK);
    assert(upload.state == MELODY_UPLOAD_IDLE && upload.len == 0 && upload.text[0] == '\0');
    assert(upload.error == open_error);

    assert(melody_upload_control(&upload, MELODY_CTRL_START, &output, &clear) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, "", 0) == MELODY_ERROR_LENGTH);
    melody_error_t previous_error = upload.error;
    assert(melody_upload_control(&upload, MELODY_CTRL_ABORT, &output, &clear) == MELODY_ERROR_OK);
    assert(upload.state == MELODY_UPLOAD_IDLE && upload.len == 0);
    assert(upload.text[0] == '\0' && upload.error == previous_error);
    assert(melody_upload_control(&upload, MELODY_CTRL_COMMIT, &output, &clear) == MELODY_ERROR_SEQUENCE);

    assert(melody_upload_control(&upload, MELODY_CTRL_START, &output, &clear) == MELODY_ERROR_OK);
    assert(melody_upload_append(&upload, "C5:100", 6) == MELODY_ERROR_OK);
    assert(melody_upload_commit(&upload, &output, NULL) == MELODY_ERROR_LENGTH);
    assert(upload.state == MELODY_UPLOAD_OPEN && upload.len == 6);
    assert(melody_upload_commit(&upload, NULL, &clear) == MELODY_ERROR_LENGTH);
    assert(upload.state == MELODY_UPLOAD_OPEN && upload.len == 6);
    assert(melody_upload_commit(&upload, &output, &clear) == MELODY_ERROR_OK);
    assert(!clear && output.count == 1);
}

int main(void)
{
    test_split_upload();
    test_chunk_bounds_and_sequence();
    test_total_length_and_failed_parse();
    test_clear_abort_and_unknown_control();
    puts("melody upload tests passed");
    return 0;
}
