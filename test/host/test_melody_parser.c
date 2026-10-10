#include "melody_parser.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define DEFAULT_MELODY "E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120 R:350 G4:120 R:350"

static void expect_error(const char *text, melody_error_t expected)
{
    melody_t melody = {0};
    assert(melody_parse(text, strlen(text), &melody) == expected);
}

int main(void)
{
    melody_t melody = {0};
    const char *valid = " c#5:10 r:2000 ";
    assert(melody_parse(DEFAULT_MELODY, strlen(DEFAULT_MELODY), &melody) == MELODY_ERROR_OK);
    assert(melody.count == 14 && melody.total_ms == 2050);
    assert(melody.steps[0].freq_hz == 659 && melody.steps[6].freq_hz == 523);
    assert(melody.steps[10].freq_hz == 784 && melody.steps[12].freq_hz == 392);
    assert(melody_parse(valid, strlen(valid), &melody) == MELODY_ERROR_OK);
    assert(melody.steps[0].freq_hz == 554 && melody.steps[1].freq_hz == 0);
    const char *more_sharps = "D#4:100 F#4:100 G#4:100 A#4:100";
    assert(melody_parse(more_sharps, strlen(more_sharps), &melody) == MELODY_ERROR_OK);
    assert(melody.steps[0].freq_hz == 311 && melody.steps[1].freq_hz == 370);
    assert(melody.steps[2].freq_hz == 415 && melody.steps[3].freq_hz == 466);
    const char *octave_bounds = "C3:100 C6:100";
    assert(melody_parse(octave_bounds, strlen(octave_bounds), &melody) == MELODY_ERROR_OK);
    assert(melody.steps[0].freq_hz == 131 && melody.steps[1].freq_hz == 1047);
    char max_tokens[120];
    size_t offset = 0;
    for (unsigned i = 0; i < MELODY_MAX_STEPS; ++i) {
        if (i != 0) max_tokens[offset++] = ' ';
        memcpy(&max_tokens[offset], "R:10", 4);
        offset += 4;
    }
    assert(offset == 119);
    assert(melody_parse(max_tokens, offset, &melody) == MELODY_ERROR_OK);
    assert(melody.count == MELODY_MAX_STEPS && melody.total_ms == 240);
    max_tokens[offset++] = ' ';
    assert(offset == MELODY_MAX_TEXT);
    assert(melody_parse(max_tokens, offset, &melody) == MELODY_ERROR_OK);
    char too_many_tokens[125];
    offset = 0;
    for (unsigned i = 0; i < MELODY_MAX_STEPS + 1; ++i) {
        if (i != 0) too_many_tokens[offset++] = ' ';
        memcpy(&too_many_tokens[offset], "R:10", 4);
        offset += 4;
    }
    assert(melody_parse(too_many_tokens, offset, &melody) == MELODY_ERROR_LENGTH);
    expect_error("", MELODY_ERROR_LENGTH);
    expect_error("   ", MELODY_ERROR_STEP_COUNT);
    expect_error("C5:10\tD5:10", MELODY_ERROR_SYNTAX);
    expect_error("C5:9", MELODY_ERROR_DURATION);
    expect_error("C5:00009", MELODY_ERROR_DURATION);
    expect_error("C5:2001", MELODY_ERROR_DURATION);
    expect_error("C5:12345", MELODY_ERROR_DURATION);
    expect_error("E#4:100", MELODY_ERROR_NOTE);
    expect_error("B#4:100", MELODY_ERROR_NOTE);
    expect_error("Cb5:100", MELODY_ERROR_NOTE);
    expect_error("Db5:100", MELODY_ERROR_NOTE);
    expect_error("C5:100,", MELODY_ERROR_SYNTAX);
    expect_error("C5", MELODY_ERROR_SYNTAX);
    expect_error("C5:", MELODY_ERROR_SYNTAX);
    expect_error(":100", MELODY_ERROR_SYNTAX);
    expect_error("X5:100", MELODY_ERROR_SYNTAX);
    expect_error("C2:100", MELODY_ERROR_NOTE);
    expect_error("C7:100", MELODY_ERROR_NOTE);
    expect_error("R:2000 R:2000 R:2000 R:2000 R:2000 R:2000 R:2000 R:2000",
                 MELODY_ERROR_TOTAL_DURATION);
    const char *exact_total = "C5:2000 C5:2000 C5:2000 C5:2000 C5:2000 C5:2000 C5:2000 C5:1000";
    assert(melody_parse(exact_total, strlen(exact_total), &melody) == MELODY_ERROR_OK);
    assert(melody.total_ms == MELODY_MAX_TOTAL_MS);
    expect_error("C5:2000 C5:2000 C5:2000 C5:2000 C5:2000 C5:2000 C5:2000 C5:1010",
                 MELODY_ERROR_TOTAL_DURATION);

    /* A failed parse must leave caller's previously valid output unchanged. */
    melody_t before = melody;
    assert(melody_parse("C5:9", strlen("C5:9"), &melody) == MELODY_ERROR_DURATION);
    assert(memcmp(&melody, &before, sizeof(melody)) == 0);

    char too_long[MELODY_MAX_TEXT + 2];
    memset(too_long, ' ', sizeof(too_long));
    too_long[sizeof(too_long) - 1] = '\0';
    assert(melody_parse(too_long, MELODY_MAX_TEXT + 1, &melody) == MELODY_ERROR_LENGTH);

    /* The parser honors len and does not require a terminating NUL byte. */
    const char truncated[] = {'C', '5', ':'};
    assert(melody_parse(truncated, sizeof(truncated), &melody) == MELODY_ERROR_SYNTAX);
    puts("melody parser tests passed");
    return 0;
}
