# Specification: Z2M-configurable buzzer melody (chunked upload, active/default state)

Status: implemented specification (v2). Supersedes the v1 text that said On/Off plays the active melody.
Scope: defines the implementation and its verification expectations. Remaining hardware/Z2M checks are called out in section 17.

## 1. Objective
Let Zigbee2MQTT (Z2M) / Home Assistant:
1. Upload a melody text of up to 300 chars using several short writes (each chunk **at most 79 chars**, i.e. `<80`), because single long ZCL writes are unreliable in practice.
2. Commit the upload atomically (valid melody replaces the active one; invalid one is rejected and nothing changes).
3. Clear the custom melody back to the default without reboot (empty commit).
4. Trigger playback with `play`.

Playback rules (fixed):
- **On/Off** (endpoint 10 `ON_OFF` attribute write) **always plays the hardcoded DEFAULT melody**, regardless of any custom melody.
- **`play` command** (cluster `0xFC00`, cmd `0x00`) plays the **ACTIVE** melody if a custom one is committed, otherwise the DEFAULT melody.

Unchanged: endpoints 11 (CO2) and 12 (PM2.5), sensors, display, button, auto-rejoin, GPIO20 / LEDC config, `buzzer_tone()` (duty 512, 10-bit).

## 2. Pre-change architecture (migration context)
- `main/esp_zb_light.c`
  - Constants: `DEFAULT_MELODY` (line 42), `MELODY_ATTR_ID 0x0000`, `MELODY_ERROR_ATTR_ID 0x0001`, `MELODY_PLAY_COMMAND_ID 0x00`.
  - State: `s_melody_mutex`, `s_active_melody` (`melody_t`), `s_active_melody_text[301]`, `s_melody_attr[256]` (ZCL char-string backing buffer, `[0]` = length), `s_melody_last_error`.
  - `buzzer_task()`: `ulTaskNotifyTake`, then copies `s_active_melody` under mutex and plays snapshot. Always plays ACTIVE today (**must change**: On/Off must play DEFAULT).
  - `buzzer_play_melody_async()`: `xTaskNotify(handle, 0, eNoAction)` (coalesces to one pending replay).
  - `buzzer_play_snapshot()`, `buzzer_tone()`, `buzzer_silence()`, `buzzer_init()` (keep as is).
  - `melody_set_error()`: rewrites `s_melody_attr` from `s_active_melody_text`, sets `s_melody_last_error`.
  - `melody_attribute_write()`: handles only attr `0x0000`; parses single write up to 300 chars; on success replaces active; on failure keeps old active.
  - `zb_attribute_handler()`: On/Off branch at `HA_ESP_LIGHT_ENDPOINT` calls `buzzer_play_melody_async()`; melody branch calls `melody_attribute_write()`. Both run in the Zigbee task.
  - `zb_action_handler()`: `ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID` branch: cluster `MELODY_CLUSTER_ID`, cmd `0x00` -> `buzzer_play_melody_async()`.
  - `esp_zb_task()`: endpoint 10 built manually (basic, identify, groups, scenes, on_off, custom cluster `melody_cluster` with attrs `0x0000` CHAR_STRING RW and `0x0001` U8 RO via `esp_zb_cluster_list_add_custom_cluster`). Endpoints 11/12 unchanged.
  - `app_main()`: creates `s_melody_mutex`, parses `DEFAULT_MELODY` into `s_active_melody` and writes `s_melody_attr`; creates `buzzer_task` (stack 2048, prio 5).
- `main/esp_zb_light.h`: `MELODY_CLUSTER_ID 0xFC00`, `HA_ESP_LIGHT_ENDPOINT 10`.
- `main/melody_parser.{h,c}`: pure C, no ESP includes. `MELODY_MAX_TEXT 300`, `MELODY_MAX_STEPS 24`, `MELODY_MIN_MS 10`, `MELODY_MAX_MS 2000`, `MELODY_MAX_TOTAL_MS 15000`. `melody_parse(text, len, out)` writes `out` only on success. Parser errors `MELODY_ERROR_OK..MELODY_ERROR_TOTAL_DURATION` are 0..6; upload protocol adds `MELODY_ERROR_SEQUENCE=7` and `MELODY_ERROR_UNKNOWN_OP=8`.
- `main/CMakeLists.txt`: `SRC_DIRS "." ...`, `INCLUDE_DIRS "."` -> any new `main/*.c` / `.h` builds automatically.
- `test/host/test_melody_parser.c`: plain `assert` tests, built with gcc (command in README/AGENTS).
- `z2m/esp32c6_melody.mjs` (source copy; AGENTS says keep in sync with `data/external_converters/esp32c6_melody.mjs` on the server). Current converter: attr `melody` (0x42, write), `last_error` (0x20), cmd `play`; `melody` write = single write of up to 300 chars.
- `README.md` (Zigbee table, Buzzer section, Z2M section) and `MELODY_HELP.md` describe the **old** behaviour (On/Off plays custom tune; 80-char practical limit) and must be updated.
- `progress.md` tracks implementation and verification progress; it is not part of firmware behavior.

Verified SDK facts (`managed_components/espressif__esp-zigbee-lib/include/zcl/esp_zigbee_zcl_command.h:2408`): `esp_zb_zcl_custom_cluster_command_message_t` = `{ info; struct { uint16_t size; void *value; } data; }`. Attribute writes arrive as `esp_zb_zcl_set_attr_value_message_t` with `attribute.data.{type,size,value}`; for CHAR_STRING, `value[0]` is the length prefix.

**Confirmed:** `ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY` exists in the project's esp-zigbee-lib headers (`esp_zigbee_zcl_common.h`).

## 3. Zigbee interface (decision)

Cluster `0xFC00` (`MELODY_CLUSTER_ID`), server role, endpoint 10, no manufacturer code. Same cluster as v1; **new attributes added**, existing IDs kept for migration.

| ID | Name | Type | Access | Semantics |
|---|---|---|---|---|
| `0x0000` | `melody` | CHAR_STRING (0x42) | READ + WRITE | **Read:** effective text = custom ACTIVE text if committed, else DEFAULT text. **Write:** single-shot atomic set up to 300 chars (see 3.1). Backing buffer `s_melody_attr[256]`. |
| `0x0001` | `last_error` | U8 (0x20) | READ_ONLY | Result of the last melody write/ctrl/commit. 0 = OK, else code from section 6. |
| `0x0002` | `melody_ctrl` | U8 (0x20) | WRITE_ONLY | Session control: `1` START, `2` COMMIT, `3` ABORT. Other values -> code 8. |
| `0x0003` | `melody_chunk` | CHAR_STRING (0x42) | WRITE_ONLY | Append a chunk to the open staging session. Length 1..79. |

| Cmd | Name | Payload | Effect |
|---|---|---|---|
| `0x00` | `play` | none | Play ACTIVE if committed, else DEFAULT. |

### 3.1 `melody` 0x0000 single-shot write (backward-compatible path)
- Length 0 -> **clear**: ACTIVE becomes "none" (DEFAULT plays). Success.
- Length 1..300 -> parse; success replaces ACTIVE; failure keeps ACTIVE, sets `last_error`.
- Length >300 or declared length larger than available data -> code 1.
- Does NOT touch the staging session (section 4).

### 3.2 Chunked upload protocol (sequential writes)
Sequence for a new melody:
1. Write `melody_ctrl = 1` (START): staging cleared, session OPEN, `last_error = 0`.
2. Write `melody_chunk = <part>` for each part, each 1..79 chars, in order, concatenated raw (no separator added; a split may fall inside a token).
3. Write `melody_ctrl = 2` (COMMIT).

Rules:
- Staging is capped at `MELODY_MAX_TEXT` (300) total bytes.
- Chunk when session IDLE -> code 7. Session-state validation takes precedence over chunk length, so an empty chunk in IDLE is also code 7.
- Chunk length 0 or >79 -> code 1, session -> FAILED (staging discarded; `last_error` = 1).
- Chunk that would exceed 300 total -> code 1, session -> FAILED.
- Chunk while session FAILED -> code 7 (session unchanged, staging stays discarded).
- START always resets to OPEN with empty staging and `last_error = 0` (also from FAILED: this is the recovery path).
- ABORT: staging discarded, session IDLE, `last_error` **unchanged** (so the converter can still read the failure code). If a completed COMMIT result is cached, ABORT leaves that cache intact because there is no open upload to abort.
- COMMIT:
  - Session IDLE with no cached result -> code 7.
  - Session FAILED -> return stored failure code (already in `last_error`); session -> IDLE; ACTIVE unchanged. **Empty commit is NOT a clear in this case** (incompatible with failed upload).
  - Session OPEN, staged length 0 -> **clear**: ACTIVE becomes none, DEFAULT plays. `last_error = 0`. Session -> IDLE. (Decision from v2 open question: empty commit clears custom active without reboot.)
  - Session OPEN, staged length > 0 -> `melody_parse` the whole staging; OK -> ACTIVE replaced, `last_error = 0`; error -> ACTIVE unchanged, `last_error = code`. Session -> IDLE in both cases (retry requires START).
  - A repeated COMMIT after a completed COMMIT returns the cached result and does not apply changes again. START clears this cache to make retransmitted commit requests idempotent.
- Unknown `melody_ctrl` value -> code 8, session unchanged.
- A chunk arriving with no START is always rejected; no implicit start.

Sessions are not timed out in v2; an open session remains until START, COMMIT, or ABORT.

### 3.3 Why attributes, not commands with payload
Attribute writes are acknowledged per write, so Z2M can await each step in order; this matches the existing `melody` write path. Commands with payload were considered (SDK supports `data.size/value`) but they need new Z2M command-parameter typing and were rejected for v2.

## 4. Melody state model (firmware)

Two melody slots + one staging buffer:

| Name | Where | Lifetime |
|---|---|---|
| `s_default_melody` (`melody_t`) | parsed once in `app_main` from `DEFAULT_MELODY`; read-only after boot; no mutex needed | RAM, constant |
| `s_active_melody` (`melody_t`) + `s_active_custom` (bool) + `s_active_melody_text[301]` | committed custom melody; `s_active_custom == false` = none | RAM, reset at boot |
| staging (`melody_upload_t`, pure-C, section 5) | open upload session | RAM, Zigbee task only |
| `s_melody_attr[256]`, `s_melody_last_error` | ZCL-visible state | RAM |

- `s_active_*`, `s_melody_attr`, `s_melody_last_error`, and the complete staging-session state/buffer are accessed only under `s_melody_mutex`.
- Staging currently originates in `zb_attribute_handler` (Zigbee task), but it still uses the shared melody mutex as required; do not rely on Zigbee-task serialization as its sole protection.
- Boot: `s_active_custom = false`, staging IDLE, `last_error = 0`, `s_melody_attr` = DEFAULT text. Nothing is persisted (no NVS).
- Migration: v1 had no `s_active_custom`; v1 active always existed (default at boot). In v2 "active" means "custom committed"; default is a separate constant. Remove v1 behaviour where On/Off played `s_active_melody`.

## 5. New pure-C module: upload session (host-testable)

Files: `main/melody_upload.h`, `main/melody_upload.c` (NO ESP includes, no logging). Include guard `MELODY_UPLOAD_H`.

```c
#define MELODY_CHUNK_MAX 79            /* max bytes per melody_chunk write, <80 */
typedef enum { MELODY_UPLOAD_IDLE, MELODY_UPLOAD_OPEN, MELODY_UPLOAD_FAILED } melody_upload_state_t;
typedef struct {
    melody_upload_state_t state;
    uint8_t len;                       /* staged bytes, 0..MELODY_MAX_TEXT */
    char text[MELODY_MAX_TEXT + 1];
    melody_error_t error;              /* code stored when FAILED */
    bool has_commit_result;             /* duplicate COMMIT response cache */
    melody_error_t commit_result;
} melody_upload_t;

typedef enum { MELODY_CTRL_START = 1, MELODY_CTRL_COMMIT = 2, MELODY_CTRL_ABORT = 3 } melody_ctrl_t;

void melody_upload_reset(melody_upload_t *u);                              /* IDLE, empty */
melody_error_t melody_upload_start(melody_upload_t *u);                    /* OPEN, staging empty, error 0 */
melody_error_t melody_upload_append(melody_upload_t *u, const char *chunk, size_t len);
void melody_upload_abort(melody_upload_t *u);                              /* IDLE, staging empty, error kept */
/* COMMIT: parses staging internally. out_clear is required; NULL returns LENGTH
 * without consuming the session. For nonempty OPEN staging, out_melody is required;
 * NULL returns LENGTH without consuming the session. Outputs are written only on success/clear.
 * Returns MELODY_ERROR_OK with *out_clear = true for empty OPEN commit (caller sets ACTIVE none).
 * Returns MELODY_ERROR_OK with *out_clear = false and *out_melody filled for valid text.
 * Returns error code otherwise; *out_melody untouched. Valid commits close the session;
 * retries return the cached result without rewriting out_melody. */
melody_error_t melody_upload_commit(melody_upload_t *u, melody_t *out_melody, bool *out_clear);
melody_error_t melody_upload_control(melody_upload_t *u, uint8_t control,
                                     melody_t *out_melody, bool *out_clear);
```
- Error codes reuse `melody_error_t` extended with `MELODY_ERROR_SEQUENCE = 7` and `MELODY_ERROR_UNKNOWN_OP = 8` (add to `melody_parser.h` enum).
- `melody_upload_fail()` is an internal helper in `melody_upload.c`; it is not part of the public upload-session API.
- Control dispatch (`melody_ctrl` 1/2/3 -> START/COMMIT/ABORT, other -> code 8) is implemented by `melody_upload_control()`.
- Append never reads beyond `len`; uses `memcpy` only after bounds checks.

## 6. Error codes (sync with converter)
| Code | Name | Meaning |
|---|---|---|
| 0 | OK | success |
| 1 | LENGTH | single write >300; chunk length 0 or >79; staged total >300; empty chunk |
| 2 | SYNTAX | bad char, missing `:`, bad token |
| 3 | STEP_COUNT | 0 or >24 tokens |
| 4 | DURATION | duration not 10..2000 |
| 5 | NOTE | octave not 3..6, E#/B#, flats |
| 6 | TOTAL_DURATION | total > 15000 ms |
| 7 | SEQUENCE | chunk/initial commit without START; chunk after FAILED; commit after ABORT when no completed-COMMIT result is cached |
| 8 | UNKNOWN_OP | `melody_ctrl` value not 1..3 |

v1 code 7 ("mutex contention") is **dropped**; the mutex is taken with `portMAX_DELAY` as today, so no busy code is needed. Parser order unchanged: length -> syntax/token scan -> counts/ranges -> total.

## 7. Task synchronization and playback

- Notification with value: replace `xTaskNotify(h, 0, eNoAction)` with
  `xTaskNotify(h, src, eSetValueWithOverwrite)` where `src = PLAY_SRC_DEFAULT (0)` or `PLAY_SRC_ACTIVE (1)`.
  - On/Off -> `PLAY_SRC_DEFAULT`.
  - `play` cmd -> `PLAY_SRC_ACTIVE`.
  - Coalescing: at most one pending request; latest source wins (**decision**).
- `buzzer_task`: `xTaskNotifyWait(0, ULONG_MAX, &src, portMAX_DELAY)`, then:
  - `PLAY_SRC_DEFAULT`: play `s_default_melody` (no mutex needed; constant).
  - `PLAY_SRC_ACTIVE`: under `s_melody_mutex` copy `s_active_melody` to a local snapshot if `s_active_custom`, else use default; release; play snapshot.
- Playback never reads shared state after the snapshot. Writes during playback affect the next trigger.
- Stack for `buzzer_task` stays 2048; raise to 3072 only if `uxTaskGetStackHighWaterMark` < ~512 bytes.
- No Zigbee API calls from buzzer code.

## 8. Playback helpers (unchanged)
- `buzzer_play_snapshot(const melody_t *m)`: `freq 0` -> `buzzer_silence(ms)`, else `buzzer_tone(freq, ms)`.
- `buzzer_init()` and LEDC config unchanged.

## 9. Zigbee implementation changes (`main/esp_zb_light.c`, `main/esp_zb_light.h`)
1. `esp_zb_light.h`: add defines/comments for `MELODY_ATTR_*` IDs `0x0002` (`MELODY_CTRL_ATTR_ID`), `0x0003` (`MELODY_CHUNK_ATTR_ID`) next to `MELODY_CLUSTER_ID`. Keep `MELODY_CLUSTER_ID`.
2. `esp_zb_task()` endpoint 10 custom cluster: add
   - `0x0002` U8, `ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY`, backing `s_melody_ctrl_value` (static uint8_t).
   - `0x0003` CHAR_STRING, `ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY`, backing `s_melody_chunk_attr[256]` (static; stack stores pointers).
   - keep `0x0000` (READ_WRITE, `s_melody_attr`) and `0x0001` (READ_ONLY, `&s_melody_last_error`).
   - Keep endpoint 10 setup otherwise unchanged.
3. `zb_attribute_handler()`: keep On/Off branch but change `buzzer_play_melody_async()` to `buzzer_play_melody_async(PLAY_SRC_DEFAULT)`. Melody branch dispatch by `attribute.id`:
   - `0x0000` -> `melody_single_write()` (3.1).
   - `0x0002` -> `melody_ctrl_write()` -> `melody_upload_control` (START/ABORT) or COMMIT path (copy staged text before commit clears it; apply under mutex).
   - `0x0003` -> `melody_chunk_write()` -> `melody_upload_append` under `s_melody_mutex`.
   - Verify CHAR_STRING type and `data.size >= 1` before reading `value[0]`; declared length `value[0] <= data.size - 1`.
   - Each handler returns `ESP_OK` on success, `ESP_ERR_INVALID_ARG` on any melody error (so the Zigbee default response reports failure); always set `s_melody_last_error` (under mutex) before returning.
   - U8 ctrl value read from `message->attribute.data.value` (type U8, `data.size == 1`).
4. `zb_action_handler()` `ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID`: `cmd 0x00` -> `buzzer_play_melody_async(PLAY_SRC_ACTIVE)`. Keep endpoint/cluster checks.
5. `melody_set_error()`/`melody_attribute_write()` replaced by: `melody_refresh_attr_locked()` (rebuild `s_melody_attr` from ACTIVE-or-DEFAULT text) and the handlers above. Under `s_melody_mutex` every change to `s_active_*`, `s_melody_attr`, `s_melody_last_error` must be followed by a `melody_refresh_attr_locked()`.
6. `app_main()`: parse `DEFAULT_MELODY` into `s_default_melody` (not `s_active_melody`); set `s_active_custom = false`; `melody_upload_reset(&s_upload)`; `s_melody_attr` = default text; `s_melody_last_error = 0`. Create `s_melody_mutex` before use (already the case).
7. Do not change endpoints 11/12, reporting, button, display, sensors, auto-rejoin, or `esp_zb_lock` usage. Zigbee stack calls stay in `esp_zb_lock_*` (attribute writes are already inside stack callbacks).

## 10. Z2M external converter (`z2m/esp32c6_melody.mjs`, mirror to server `data/external_converters/esp32c6_melody.mjs`)
- `melodyCluster.attributes`: add `melody_ctrl: {ID: 0x0002, type: 0x20, write: true}` and `melody_chunk: {ID: 0x0003, type: 0x42, write: true}`. Keep `melody` (`write: true`) and `last_error` (0x20). Keep `play` command.
- Chunk split constant `CHUNK_MAX = 48` (firmware accepts up to 79; converter stays lower, see section 11); `MELODY_MAX = 300`.
- `tzMelody.convertSet(entity, key, value, meta)`, value string:
  1. Client checks: string type; length 0..300; ASCII printable only (`/^[\x20-\x7e]*$/`). Throw on failure before any write (no code sent).
  2. Split into chunks of <= 48 chars in order (empty value -> zero chunks).
  3. **Sequentially, each awaited**: `write({melody_ctrl: 1})`; for each chunk `write({melody_chunk: chunk})`; `write({melody_ctrl: 2})`.
   4. On any write failure: best effort `write({melody_ctrl: 3})` (ABORT), then `read(['last_error'])`, then throw an upload error. Do not retry chunks (duplicate risk, see section 11).
   5. After a successful COMMIT write, read `last_error`; Herdsman returns a key/value object (e.g. `{last_error: 0}`). Throw with the reported code if nonzero. This detects a rejected parse/commit even if a write response was accepted. The read response also updates the exposed `melody_error` state, including clearing stale errors after success.
   6. If status read fails after all writes were acknowledged, preserve the write result and return `{state: {melody: value}}`; the diagnostic read is best effort.
- `tzMelody.convertGet`: read `melody`.
- `fzMelody`: unchanged mapping (`melody`, `last_error` -> `melody_error`). Also map report/read of `melody_ctrl`? No (write-only, do not map).
- `tzMelodyPlay`: unchanged (`command('melodyCluster','play')`).
- `tzMelodyError`: unchanged.
- Exposes: update `melody` description: "up to 300 chars, sent in 79-char chunks; empty value clears to default". Keep `melody_play` and `melody_error`.
- `configure`: `device.addCustomCluster('melodyCluster', melodyCluster)` (unchanged); **re-run after updating** so new attribute IDs are known.
- ASSUMPTION: Herdsman accepts writes to attributes with `write: true` and surfaces a ZCL failure status as a rejected promise. Verify on the user's Z2M version (`docker logs -n 200 zigbee2mqtt`).
- Keep the `UNSUPPORTED_CLUSTER melodyCluster` known gotcha documented.

## 11. Edge cases and error handling
- Converter sends chunks sequentially; each write awaits the default response, so chunk order is preserved.
- **Converter chunk size is 48 chars** (`MELODY_CHUNK_MAX` in the .mjs). Field test: a 79-char chunk failed with `ApsDataRequest failed with status: InvalidValue` (ZCL/APS/NWK overhead exceeds the frame payload). The firmware still accepts 1..79 per chunk; the converter stays lower for margin.
- Zigbee retransmission could duplicate a chunk (no sequence index in v2): result would be a wrong melody that still parses or fails with code 2-6. **Accepted v2 limitation**; mitigated by the parser and by the user re-sending. Duplicate COMMIT requests are idempotent.
- Chunk split inside a token is allowed; only the concatenated text is validated.
- Write while playing -> applies to the next trigger (snapshot).
- Multiple triggers during playback -> one pending replay (latest source wins).
- Invalid text never plays partially; never replaces ACTIVE.
- Reboot / rejoin / leave: staging and ACTIVE reset at boot (`s_active_custom = false`); a leave does not clear ACTIVE (RAM only; **decision**: only reboot clears).
- Factory reset: device reboots -> ACTIVE reset to default.
- Non-ASCII bytes -> code 2 (device); converter rejects client-side first.
- Declared CHAR_STRING length mismatch -> code 1.
- `melody_ctrl` with a non-U8 or incorrectly sized value -> code 1; a malformed/empty chunk in an OPEN session -> code 1 and the session becomes FAILED.
- No `strtol`; no dynamic allocation in parser or session; all lengths checked before copy.
- On/Off write behaviour: any On/Off write plays DEFAULT (**changed from v1**, where it played active).

## 12. Tests

### 12.1 Host parser tests (`test/host/test_melody_parser.c`) - keep v1 cases, add:
- Error code 7/8 are not produced by the parser (regression guard).
- Existing cases unchanged (default = 14 steps, total 2050, freqs 659/523/784/392, boundary tokens, 300-char max, 301 -> LENGTH, etc.).

### 12.2 Host upload-session tests (NEW `test/host/test_melody_upload.c`, compile with `main/melody_parser.c main/melody_upload.c`):
- START -> append "C5:100 " (7 chars) -> append "E5:100" -> COMMIT -> OK, staged text equals concatenation, parse result equals single `melody_parse` of the whole string.
- Chunk exactly 79 chars OK; 80 chars -> LENGTH (1) and session FAILED.
- Chunk length 0 -> LENGTH (1).
- Staged total 300 OK; 301st byte -> LENGTH (1), session FAILED.
- Append while IDLE -> SEQUENCE (7); append while FAILED -> SEQUENCE (7), staging stays empty.
- Empty append while IDLE -> SEQUENCE (7); session error precedes chunk-length validation.
- COMMIT in initial IDLE -> SEQUENCE (7).
- Duplicate COMMIT after a successful/failed commit returns the cached result and does not change ACTIVE again.
- START from FAILED -> OPEN, `error` cleared, staging empty.
- START twice -> staging reset (no concatenation of old data).
- ABORT from OPEN -> IDLE, staging empty, error code unchanged.
- ABORT from FAILED -> IDLE, staging empty, failed chunk error preserved; a later COMMIT is a sequence error.
- ctrl value 0, 4, 255 -> UNKNOWN_OP (8), state unchanged.
- COMMIT OPEN + empty staging -> clear signal, no parse error, session IDLE.
- COMMIT FAILED -> returns stored code, session IDLE, clear signal **false**.
- COMMIT OPEN + invalid text (e.g. `C5:9`) -> DURATION (4), ACTIVE output untouched (caller contract), session IDLE.
- COMMIT with a missing required output pointer -> LENGTH (1), staging/session unchanged.
- Upload result equal to single-shot parse for every valid vector in 12.1 (split at every position for the default melody).
- Run with `-fsanitize=address,undefined`.

### 12.3 On-device manual checklist (ESP32-C6, Z2M):
1. Flash; factory-new join; Z2M interview shows `melody`, `melody_play`, `melody_error`; reconfigure after converter update.
2. Toggle On/Off in HA -> DEFAULT melody plays (even after a custom melody was committed).
3. Set a melody of 100 chars via converter (2 chunks) -> `melody_error` 0; `play` -> custom melody plays.
4. Set melody with bad token -> converter throws with code; previous ACTIVE still plays on `play`.
5. Empty `melody` value -> `play` plays DEFAULT; `melody` read returns default text.
6. Single-shot `melody` writes still accept up to 300 chars; the converter uses chunked writes to avoid coordinator transport limits.
7. Chunk without START (manual MQTT/zcl) -> code 7.
8. Spam `play` 10x -> at most one replay after current playback.
9. Reboot -> ACTIVE gone, `play` plays DEFAULT.
10. CO2/PM2.5 reporting, display, button, auto-rejoin unchanged.
11. Temporary: log `buzzer_task` stack high-water mark once.

### 12.4 Build
- `idf.py build` (via VS Code ESP-IDF extension Build button) must succeed; no new warnings in `melody_upload.c`.

## 13. Documentation changes
- `README.md`:
  - Zigbee table, endpoint 10 row: add attrs `0x0002` ctrl, `0x0003` chunk.
  - Buzzer section: On/Off plays DEFAULT (replace "when a custom tune is set, On/Off plays that tune"); `play` plays custom-or-default; chunk upload protocol (START/chunks/COMMIT, <=79-char chunks, 300 total); empty commit clears; error codes 0-8; RAM-only note; replace the existing "approximately 80 characters" paragraph by the chunk rule.
  - Z2M section: converter re-install + reconfigure requirement after this change.
  - Keep unrelated sections intact.
- `MELODY_HELP.md`: replace the single-write example with the chunked flow (or describe that the converter does it); update error table to 0-8; update "On/Off" semantics if mentioned (currently not mentioned explicitly - keep it accurate); keep the `UNSUPPORTED_CLUSTER` note.
- `AGENTS.md`: keep its host-test commands and error-code guidance aligned with the added upload module (run parser and upload tests as separate host executables).
- `z2m/esp32c6_melody.mjs`: source of truth; copy to server path (manual step, not done by the agent).

## 14. Migration
- Firmware v1 -> v2:
  - On/Off behaviour changes from "active" to "DEFAULT". Users who relied on On/Off playing a custom tune must use `play` instead.
  - `melody` 0x0000 still accepts up to 300 characters for backward compatibility; the updated converter uses ordered chunks for all melody messages.
  - `melody` read now returns the effective text (custom or default) instead of always the last active text; at boot it is the default text (same as v1).
  - New attributes `0x0002`, `0x0003`; existing `0x0000`, `0x0001`, cmd `0x00` IDs unchanged.
- Z2M: install converter v2 (both copies), restart Z2M, **reconfigure device** so the new attributes are registered.
- Old converter (v1) still uses one-shot writes and supports up to 300 characters at the firmware parser. Use the updated converter's chunks for melodies whose single ZCL write may exceed coordinator transport limits; only writes over 300 characters fail with code 1.
- No NVS schema or flash changes.

## 15. Security considerations
- Out of scope per owner instruction. Behaviour unchanged: any joined device can write melody attrs and trigger the buzzer (accepted risk).
- Still required: bounded buffers, length checks before copy, no dynamic allocation in parser/session.

## 16. Acceptance criteria
- On/Off always plays the DEFAULT melody, regardless of any custom melody.
- `play` plays the committed custom melody, or DEFAULT if none is committed.
- Chunk writes are each 1..79 chars; total staged <= 300; START/chunk/COMMIT flow works from Z2M with sequential awaited writes.
- A failed upload never changes ACTIVE and never plays partially.
- Empty COMMIT (after START, no chunks) clears ACTIVE to DEFAULT without reboot; empty COMMIT after a failed upload is rejected with the stored code.
- Reboot resets ACTIVE and staging.
- Error codes 0-8 match README, converter, and tests.
- Host tests for parser and upload session pass with ASan/UBSan.
- `idf.py build` passes; endpoints 11/12, sensors, button, display, auto-rejoin unchanged.
- README, MELODY_HELP.md, and Z2M converter (both copies) updated.

## 17. Decisions and remaining verification
Decisions:
1. Chunk limit 79 chars (`MELODY_CHUNK_MAX`); total 300 (`MELODY_MAX_TEXT`).
2. Attribute-based chunking (`melody_ctrl` 0x0002, `melody_chunk` 0x0003); commands with payload rejected for v2.
3. Empty COMMIT after START clears ACTIVE to DEFAULT without reboot; not allowed when the session FAILED (returns stored code).
4. Single-shot `melody` write up to 300 chars remains accepted for compatibility; empty clears.
5. On/Off -> DEFAULT always; `play` -> ACTIVE else DEFAULT.
6. Pending playback: one, latest source wins.
7. ACTIVE survives BDB leave/rejoin; reset only by reboot / factory reset (RAM).
8. Error code 7 redefined as SEQUENCE; new code 8 UNKNOWN_OP; v1 "mutex contention" code dropped.
9. Session state lives in pure-C `melody_upload.{h,c}`; ACTIVE state lives in `esp_zb_light.c` under `s_melody_mutex`.
10. A completed COMMIT result is cached until START; duplicate COMMIT requests return that result without mutating ACTIVE.

Resolved during implementation:
- `ESP_ZB_ZCL_ATTR_ACCESS_WRITE_ONLY` is present in the installed SDK.
- The converter accepts the empty `melody` string and maps it to START + COMMIT with no chunks.
- Herdsman `Endpoint.read()` resolves to an attribute key/value object, used to inspect `last_error` after commit.

Remaining device verification:
- Confirm write-failure responses and converter behavior with the user's Z2M/ESP-IDF versions and the physical device.
- Retransmitted chunks can duplicate because the protocol has no sequence index; accepted v2 limitation.
- Open staging sessions have no idle timeout in v2.
</content>
