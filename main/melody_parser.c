#include "melody_parser.h"

#include <stdbool.h>
#include <string.h>

/* Rounded equal-temperament frequencies, octaves 3 through 6, C through B. */
static const uint16_t s_note_frequencies[4][12] = {
    {131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247},
    {262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494},
    {523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988},
    {1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976},
};

static char ascii_upper(char ch)
{
    return (ch >= 'a' && ch <= 'z') ? (char)(ch - ('a' - 'A')) : ch;
}

melody_error_t melody_parse(const char *text, size_t len, melody_t *out)
{
    melody_t parsed = {0};
    size_t pos = 0;

    if (text == NULL || out == NULL || len == 0 || len > MELODY_MAX_TEXT) {
        return MELODY_ERROR_LENGTH;
    }

    while (pos < len && text[pos] == ' ') {
        ++pos;
    }
    while (pos < len) {
        melody_step_t step = {0};
        uint32_t duration = 0;
        char note;
        bool is_rest;
        unsigned digit_count = 0;

        if (parsed.count >= MELODY_MAX_STEPS) {
            return MELODY_ERROR_STEP_COUNT;
        }

        note = ascii_upper(text[pos++]);
        is_rest = note == 'R';
        if (is_rest) {
            step.freq_hz = 0;
        } else {
            int semitone;
            switch (note) {
            case 'C': semitone = 0; break;
            case 'D': semitone = 2; break;
            case 'E': semitone = 4; break;
            case 'F': semitone = 5; break;
            case 'G': semitone = 7; break;
            case 'A': semitone = 9; break;
            case 'B': semitone = 11; break;
            default: return MELODY_ERROR_SYNTAX;
            }
            if (pos < len && text[pos] == '#') {
                if (note == 'E' || note == 'B') {
                    return MELODY_ERROR_NOTE;
                }
                ++semitone;
                ++pos;
            }
            if (pos >= len || text[pos] < '3' || text[pos] > '6') {
                return MELODY_ERROR_NOTE;
            }
            step.freq_hz = s_note_frequencies[(unsigned)(text[pos++] - '3')][semitone];
        }

        if (pos >= len || text[pos++] != ':') {
            return MELODY_ERROR_SYNTAX;
        }
        if (pos >= len || text[pos] < '0' || text[pos] > '9') {
            return MELODY_ERROR_SYNTAX;
        }
        while (pos < len && text[pos] >= '0' && text[pos] <= '9') {
            if (++digit_count > 4) {
                return MELODY_ERROR_DURATION;
            }
            if (duration > 2000) {
                return MELODY_ERROR_DURATION;
            }
            duration = duration * 10 + (uint32_t)(text[pos++] - '0');
            if (duration > 2000) {
                return MELODY_ERROR_DURATION;
            }
        }
        if (duration < MELODY_MIN_MS || duration > MELODY_MAX_MS) {
            return MELODY_ERROR_DURATION;
        }
        step.duration_ms = (uint16_t)duration;

        if (pos < len && text[pos] != ' ') {
            return MELODY_ERROR_SYNTAX;
        }
        if (parsed.total_ms + duration > MELODY_MAX_TOTAL_MS) {
            return MELODY_ERROR_TOTAL_DURATION;
        }
        parsed.total_ms += duration;
        parsed.steps[parsed.count++] = step;

        while (pos < len && text[pos] == ' ') {
            ++pos;
        }
    }

    if (parsed.count == 0) {
        return MELODY_ERROR_STEP_COUNT;
    }
    memcpy(out, &parsed, sizeof(parsed));
    return MELODY_ERROR_OK;
}
