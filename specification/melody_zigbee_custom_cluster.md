# Specification: Z2M-configurable buzzer melody

## 1. Objective
Allow Home Assistant / Zigbee2MQTT (Z2M) to set a buzzer melody as text (notes, durations, rests) and trigger playback, while keeping all existing behavior:
- Endpoint 10 stays a standard HA On/Off Light (`ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID` 0x0100). Any On/Off attribute write still plays the melody.
- Endpoints 11 (CO2) and 12 (PM2.5) unchanged.
- Buzzer GPIO20 / LEDC unchanged (`BUZZER_GPIO`, `BUZZER_CHANNEL`, `BUZZER_TIMER`, duty 512 on 10-bit).
- Default melody (no custom set since boot) must be audibly identical to the current fixed tune.

## 2. Relevant existing architecture
- `main/esp_zb_light.c`
  - `buzzer_task()`: `ulTaskNotifyTake`; plays hard-coded sequence via `buzzer_tone(freq, ms)` + `vTaskDelay`.
  - `buzzer_play_melody_async()`: `xTaskNotify(buzzer_task_handle, 0, eNoAction)`. Notifications coalesce (one replay pending max).
  - `zb_attribute_handler()`: on `ESP_ZB_ZCL_CLUSTER_ID_ON_OFF` / `ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID` at `HA_ESP_LIGHT_ENDPOINT` calls `buzzer_play_melody_async()`. Registered via `zb_action_handler()` for `ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID`.
  - `esp_zb_task()`: endpoint 10 created by `esp_zb_on_off_light_ep_create(HA_ESP_LIGHT_ENDPOINT, &light_cfg)` (cfg from `ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG()`). Endpoints 11/12 built manually with `esp_zb_zcl_attr_list_create` + `esp_zb_cluster_list_add_*`. Manufacturer info added via `esp_zcl_utility_add_ep_basic_manufacturer_info` for eps 10/11/12.
  - Note frequencies `NOTE_*` macros at top of file (E5 659, C5 523, G5 784, G4 392 are required to match).
- `main/esp_zb_light.h`: `HA_ESP_LIGHT_ENDPOINT 10`, `ESP_MANUFACTURER_NAME "ESPRESSIF"`, `ESP_MODEL_IDENTIFIER "esp32c6"` (from `CONFIG_IDF_TARGET`).
- `main/CMakeLists.txt`: `SRC_DIRS "."`, so new `.c` files in `main/` build automatically.
- `main/idf_component.yml`: esp-zigbee-lib `~1.6.0`, esp-zboss-lib `~1.6.0`.
- Verified API (managed_components/espressif__esp-zigbee-lib/include):
  - `esp_zb_cluster_list_add_custom_cluster(cluster_list, attr_list, role_mask)` (esp_zigbee_cluster.h:1294)
  - `esp_zb_zcl_attr_list_create(cluster_id)` (esp_zigbee_attribute.h:27)
  - `esp_zb_on_off_cluster_create(esp_zb_on_off_cluster_cfg_t*)` and `esp_zb_cluster_list_add_on_off_cluster(...)` (esp_zigbee_cluster.h:103, 673)
  - `ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID` = 0x1040 (zcl/esp_zigbee_zcl_core.h:170)
  - `esp_zb_zcl_custom_cluster_cmd_req()` exists (esp_zigbee_zcl_command.h:3630) - for sending only, not needed here.
  - Types: `ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING` (0x42), `ESP_ZB_ZCL_ATTR_TYPE_U8` (0x20), `ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY/WRITE_ONLY`.

## 3. Chosen Zigbee interface (decision)
Add a **manufacturer-range custom cluster on endpoint 10**, no manufacturer code in ZCL frames.

| Item | Value |
|---|---|
| Cluster ID | `0xFC00` (name `MELODY_CLUSTER_ID`, define in `esp_zb_light.h`) |
| Role | Server (`ESP_ZB_ZCL_CLUSTER_SERVER_ROLE`) |
| Endpoint | 10 (same as On/Off) |
| Manufacturer code | none (`ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC`) |
| Attr 0x0000 `melody` | CHAR_STRING (0x42), READ+WRITE, max 120 chars. Stores the ACTIVE melody text. |
| Attr 0x0001 `last_error` | U8, READ_ONLY. 0 = OK, else error code (section 6). |
| Cmd 0x00 `play` | Received (client->server), no payload. Triggers playback. |

Rationale: standard HA cluster cannot carry text; attribute write + command give atomic-enough set/trigger; the On/Off path keeps working unchanged.

ESP-Zigbee headers confirm `attribute.data.value` contains the char-string length prefix and `attribute.data.size` is the value size. Firmware uses a 256-byte backing buffer (max one-byte length plus 255 bytes) and separately enforces the 120-character melody limit.

## 4. Melody text grammar (exact)
Input is ASCII. Tokens separated by one or more spaces (0x20). Leading/trailing spaces allowed. Tabs, commas, other chars = syntax error.

```
melody  := ws? token ( ws token )* ws?
token   := note | rest
note    := letter sharp? octave ':' duration
letter  := 'A'..'G' | 'a'..'g'          (case-insensitive)
sharp   := '#'                          (only C#, D#, F#, G#, A#; E# and B# are invalid)
octave  := '3'..'6'
rest    := 'R' ':' duration | 'r' ':' duration
duration:= 1..4 decimal digits (no sign), value 10..2000 (ms)
ws      := 1*(' ')
```

Notes: no flats (`b`). Duration = ms the tone (note) or silence (rest) lasts.

**Default melody** (must equal current fixed tune exactly, 88 chars):
```
E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120 R:350 G4:120 R:350
```
Mapping to current code: E5 120 / delay 30 / E5 120 / delay 150 / E5 120 / delay 150 / C5 120 / delay 30 / E5 120 / delay 150 / G5 120 / delay 350 / G4 120 / delay 350.

## 5. Bounds
| Limit | Value | Constant |
|---|---|---|
| Max text length | 120 chars (fits ZCL char string) | `MELODY_MAX_TEXT` |
| Max tokens | 24 | `MELODY_MAX_STEPS` |
| Duration per token | 10..2000 ms | `MELODY_MIN_MS`, `MELODY_MAX_MS` |
| Total playback time | <= 15000 ms (sum of durations) | `MELODY_MAX_TOTAL_MS` (assumption, tune if needed) |
| Octave | 3..6 | |

Frequency: equal temperament, A4=440 Hz, rounded. Hardcode a 4x12 table (octaves 3..6, semitones C..B) in `melody_parser.c`. Octave 4 = 262,277,294,311,330,349,370,392,415,440,466,494; other octaves = x2 / x0.5 with rounding. Must yield E5=659, C5=523, G5=784, G4=392. Rest = frequency 0.

## 6. Parser and error codes
- New pure-C module `main/melody_parser.h/.c` (NO ESP-IDF includes, no logging) so it can be built on host for tests.
- API (suggested):
  ```c
  typedef struct { uint16_t freq_hz; uint16_t duration_ms; } melody_step_t; // freq 0 = rest
  typedef struct { melody_step_t steps[MELODY_MAX_STEPS]; uint8_t count; uint32_t total_ms; } melody_t;
  uint8_t melody_parse(const char *text, size_t len, melody_t *out); // returns 0 OK or error code
  ```
- Parse into a local `melody_t`; only on success copy into the shared active melody (atomic replacement). Failed write never changes the active melody.

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | Length 0 or > 120 |
| 2 | Syntax error (bad char, missing ':', bad token) |
| 3 | Token count 0 or > 24 |
| 4 | Duration out of 10..2000 |
| 5 | Invalid note (octave not 3..6, E#/B#) |
| 6 | Total duration > 15000 ms |

Order: length -> syntax/token scan -> counts/ranges -> total.

## 7. Task synchronization
- Shared state: `s_melody` (`melody_t`) + `s_melody_attr` (ZCL string buffer) + `s_melody_last_error`, guarded by new `s_melody_mutex` (`xSemaphoreCreateMutex()` in `app_main`, next to `s_sensor_data_mutex`).
- Writer (Zigbee task, `zb_attribute_handler`): copy bounded payload to local buffer -> parse (no mutex) -> take `s_melody_mutex` -> copy struct + canonical string attr + error -> give. Invalid writes restore the last valid string and update the error under the same mutex. A ZCL char string can contain at most 255 bytes plus a length prefix, so the backing ZCL buffer is 256 bytes even though accepted melody text is limited to 120.
- Playback (`buzzer_task`): on each wake, take mutex, `memcpy` active melody into a local `melody_t` snapshot, give mutex, play snapshot. Playback never reads shared state after snapshot. Writes during playback affect the NEXT trigger.
- Trigger coalescing: keep current `xTaskNotify`/`ulTaskNotifyTake` pattern (one pending replay max). Both On/Off and `play` command call `buzzer_play_melody_async()`.
- No Zigbee API calls from `buzzer_task`. Keep `esp_zb_lock` out of buzzer code.
- Keep `buzzer_task` stack at 2048 unless high-water mark (`uxTaskGetStackHighWaterMark`) shows less than ~512 bytes free; then raise to 3072.

## 8. Playback
- `buzzer_play_snapshot(const melody_t *m)`: for each step: if `freq_hz == 0` -> `ledc duty 0`, `vTaskDelay(duration)`; else `buzzer_tone(freq_hz, duration_ms)` (existing function, unchanged).
- Keep `buzzer_init()` and LEDC config unchanged.
- Replace the hard-coded sequence in `buzzer_task` by the snapshot playback; the default melody is the parsed default string (section 4), initialized at boot (`melody_parse(DEFAULT_MELODY, ...)` in `app_main` before the buzzer task starts, or static precomputed table).

## 9. Zigbee implementation changes (esp_zb_light.c / esp_zb_light.h)
1. Endpoint 10: stop using `esp_zb_on_off_light_ep_create` for ep 10 OR keep it and verify a way to append a cluster. Preferred: build ep 10 manually like eps 11/12:
   - `esp_zb_cluster_list_t` with basic, identify, groups, scenes, `esp_zb_on_off_cluster_create(&on_off_cfg)`, and the melody custom cluster. Preserve all standard clusters/config from `ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG()` so the existing endpoint remains equivalent.
   - Endpoint config: `app_profile_id = ESP_ZB_AF_HA_PROFILE_ID`, `app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID`, `app_device_version = 0`, endpoint `HA_ESP_LIGHT_ENDPOINT`.
   - Melody cluster: `esp_zb_zcl_attr_list_create(MELODY_CLUSTER_ID)`; `esp_zb_cluster_add_attr` for 0x0000 (CHAR_STRING, READ|WRITE, pointer to static `s_melody_attr`) and 0x0001 (U8, READ_ONLY, `&s_melody_last_error`); then `esp_zb_cluster_list_add_custom_cluster(cl, attr, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE)`.
   - Must keep `esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, HA_ESP_LIGHT_ENDPOINT, &info)`.
2. `zb_attribute_handler`: keep On/Off branch exactly. Add branch for `cluster == MELODY_CLUSTER_ID`, `attribute.id == 0x0000`: copy string value (verify length prefix), call `melody_set_from_zigbee()` (parse + store + update last_error). Return ESP_OK on success; on failure return error and keep old melody.
3. `zb_action_handler`: add `case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID`: if `cluster == MELODY_CLUSTER_ID && command == 0x00 && dst_endpoint == HA_ESP_LIGHT_ENDPOINT` -> `buzzer_play_melody_async()`. Verify struct `esp_zb_zcl_custom_cluster_command_message_t` field names in `zcl/esp_zigbee_zcl_command.h`.
 4. Do not change endpoints 11/12, reporting configuration thresholds, button logic, display, or sensor tasks. Preserve the auto-rejoin watchdog while avoiding a nested Zigbee lock: report helpers run under the lock already, and LED feedback must occur after that lock is released.

## 10. Edge cases and error handling
- Write with invalid text -> old melody kept, last_error set, log `ESP_LOGW` with code. Never play a partially parsed melody.
- Empty string write -> code 1.
- Write while playing -> applies to next trigger; current playback continues with snapshot.
- Multiple triggers during playback -> one pending replay (same as today).
- Rejoin/leave/factory reset: active melody is RAM only; after reboot default melody returns. (Assumption: no NVS persistence in v1.)
- Non-UTF/ non-ASCII bytes -> syntax error (code 2).
- Length-prefix mismatch (declared length > `data.size - 1`) -> code 1.
- Parser must not index beyond `len`; no `strtol` on unbounded strings; use manual digit loop with overflow guard (max 4 digits).
- On/Off write behaviour unchanged: any On/Off write plays current melody (= default unless changed).

## 11. Security considerations
- Zigbee network-level security unchanged (install-code policy from `INSTALLCODE_POLICY_ENABLE` as today). Any joined device can write the melody; accepted risk (buzzer only, no actuator of consequence). Document it.
- Bounded buffers only; no dynamic allocation in parser; all lengths checked before copy.

## 12. Z2M external converter (new file, e.g. `z2m/esp32c6_melody.js`)
Target: Z2M external converter ES module (`z2m/esp32c6_melody.mjs`). It uses the documented zigbee-herdsman device API; behavior still needs on-device confirmation with the user's Z2M version.
- Fingerprint: `manufacturerName: 'ESPRESSIF'`, `modelID: 'esp32c6'`. Adjust if the device reports different values (check Z2M "Device not supported" log).
- Register the cluster with `device.addCustomCluster('melodyCluster', definition)` in `configure`. Include cluster, attribute and command names; use numeric Herdsman ZCL types (CHAR_STR `0x42`, UINT8 `0x20`) and mark melody writable.
- Exposes:
  - `text` `melody` (access `ALL`: state, set and get), description with grammar summary.
  - `enum` `melody_play` values `['PLAY']`, access SET (fires `play` command).
  - `numeric` `melody_error` 0..6, access `STATE_GET`, with a toZigbee read handler.
- toZigbee:
  - `melody`: `entity.write('melodyCluster', { melody: value })`; return `{ state: { melody: value } }`. Client-side length check (<=120) to fail fast.
  - `melody_play`: `entity.command('melodyCluster', 'play', {}, options)`; return no state.
- fromZigbee: handle `melodyCluster` `melody` and `last_error` attributes -> state keys `melody`, `melody_error`. Also handle the on/off as today (Z2M default).
- Endpoint: `endpoint: (device) => ({ default: 10 })` (or rely on default ep 10 of On/Off; verify).
- `configure`: register the custom cluster; no reporting configuration is needed because attributes can be read on demand.

Usage example:
```
zigbee2mqtt/<name>/set  {"melody":"C5:150 R:50 E5:150 G5:300"}
zigbee2mqtt/<name>/set  {"melody_play":"PLAY"}
```
Send as two messages (ordering inside a single message not guaranteed; verify).
After flashing: Z2M must re-interview the device (Z2M UI "Reconfigure"/"Interview", or re-pair) so it learns the new cluster 0xFC00 on endpoint 10.

## 13. Documentation changes
- `README.md`:
  - Zigbee table: endpoint 10 row add "custom cluster 0xFC00: melody text (0x0000), last_error (0x0001), play cmd (0x00)".
  - Buzzer section: grammar, bounds, default melody, error codes, On/Off behaviour, RAM-only note.
  - Add Z2M usage section (set melody, trigger, re-interview requirement) and converter file path.
  - Keep existing functions list intact.
- Do not change unrelated README sections.
- Add comments in `esp_zb_light.h` for `MELODY_CLUSTER_ID` and limits.

## 14. Tests
Host tests (no hardware), e.g. `test/host/test_melody_parser.c` compiled with gcc (not part of ESP build):
- Default string parses to 14 steps, total_ms = 2050 (7x120 tone + 1210 rest), exact freqs (E5=659, C5=523, G5=784, G4=392).
- Valid: lowercase, sharps (`C#5:100`), rests `r:50`, leading/trailing spaces, 24 tokens, 2000 ms, 10 ms.
- Invalid: empty, only spaces, 121 chars, >24 tokens (length takes precedence because 25 shortest tokens exceed 120 chars), duration 9 / 2001 / 12345, octave 2 / 7, `E#4:100`, `B#4:100`, tab, comma, `C5` (missing duration), `C5:` , `:100`, `X5:100`, total > 15000.
- No out-of-bounds reads for truncated input (run with `-fsanitize=address,undefined`).

On-device manual tests (checklist):
1. Flash, factory-new join, Z2M interview shows `melody`, `melody_play`, `melody_error`.
2. Toggle On/Off in HA -> default melody plays (same as before).
3. Set valid melody, press play -> custom melody plays; On/Off now plays custom melody.
4. Set invalid text -> `melody_error` != 0, previous melody still plays on trigger.
5. Spam play 10x during playback -> at most one replay after current finishes.
6. Sensors CO2/PM2.5 still report; display and button functions unchanged.
7. `buzzer_task` stack high-water mark logged once (temporary).

## 15. Acceptance criteria
- Default behaviour (no set) audibly identical to pre-change firmware.
- Custom melody can be set from Z2M text field and played via `melody_play` button; On/Off also uses it.
- Invalid melody never plays partially and never replaces the active melody.
- All grammar/bounds in section 4/5 enforced; parser error codes 1–6 are covered by host tests. Code 7 is an integration-time mutex contention status, outside the pure parser.
- No change to endpoints 11/12, sensors, button, display, auto-rejoin, GPIO20/LEDC config.
- Build passes with `idf.py build` (ESP-IDF, ESP32-C6 target); no new warnings in new files.
- README and Z2M converter updated.

## 16. Decisions and verified implementation details
1. On/Off plays the currently configured tune; with no write since boot this exactly reproduces the original tune.
2. Melody storage is RAM-only; no NVS writes are introduced.
3. Bounds are fixed at the values in section 5.
4. Zigbee-herdsman documents `device.addCustomCluster(name, clusterDefinition)`; the converter supplies named attrs/commands and numeric ZCL type IDs. Z2M 2.11+ may require enabling external JavaScript converters; exact user installation still requires device validation.
5. `esp_zb_zcl_custom_cluster_command_message_t` carries cluster, endpoint and command ID through `info`; char-string values include a one-byte length prefix in `attribute.data.value`.
6. Manual endpoint creation retains Basic, Identify, Groups, Scenes and On/Off server clusters with the default light values before appending the custom cluster.
7. `melody_parser.{h,c}` is a pure-C module in `main/`, auto-built by `SRC_DIRS "."`; `test/host/test_melody_parser.c` can be compiled independently with a host C compiler.
8. The existing report-failure watchdog starts network steering while its caller holds the Zigbee lock and performs the double-blink after release; this preserves behavior without recursive lock acquisition or blocking the Zigbee stack during the blink.
9. The button-task factory-reset call is protected by the Zigbee API lock because it runs outside stack callbacks.
