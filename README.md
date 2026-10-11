# ESP32-C6 Zigbee Multi-Sensor Device

## Overview
Zigbee End Device (ED) based on ESP32-C6-Zero with CO2 and PM2.5 sensors, OLED display, button controls, and sound output via Zigbee On/Off and custom melody commands.

## Hardware
- **Sensors:**
  - MHZ19B (UART) - CO2 measurement, 9600 baud
  - SPS30 (I2C) - PM2.5 & PM1.0 mass concentration
  - SSD1306 (I2C) - 128x64 OLED display
- **Actuators:**
  - Buzzer (GPIO20, LEDC) - Default/custom melody playback via Zigbee
  - RGB LED (via led_strip driver) - Status indication
- **Input:**
  - Button (GPIO2, debounced 500ms, pull-down, active high) - 3 functions

## Zigbee Configuration
| Endpoint | Device Type | Cluster | Function |
|----------|-------------|---------|----------|
| 10 | HA Light | On/Off, custom `0xFC00` | Buzzer; melody attr `0x0000`, last_error `0x0001`, upload control `0x0002`, upload chunk `0x0003`, play cmd `0x00` |
| 11 | CO2 Sensor | Carbon Dioxide Measurement | CO2 reporting (60s interval) |
| 12 | PM2.5 Sensor | PM2.5 Measurement | PM2.5 reporting (120s interval) |

Cluster attributes use standard ZCL: CO2 normalized to 0.0-1.0 (ppm/1000000), PM2.5 in µg/m³.

## Pin Configuration
| GPIO | Function | Notes |
|------|----------|-------|
| 1,3 | MHZ19B UART | TX/RX |
| 14,15 | SSD1306 software I2C | SDA/SCL |
| 21,22 | SPS30 I2C | SDA/SCL (hardware I2C_NUM_0) |
| 2 | Button | Active high, pull-down, debounced |
| 20 | Buzzer | LEDC PWM output |

## Core Functions

### Button Control (Interrupt + Task)
- **Short press (<500ms):** Toggle display always-on mode
- **Medium press (500ms-5s):** Trigger Zigbee network rejoin (5s cooldown)
- **Long press (≥5s):** Factory reset & LED blink pattern

### Buzzer (LEDC + Task Notification)
- **API:** `buzzer_play_melody_async(source)` - Requests default or active melody playback
- Task-based; On/Off and the custom melody `play` command request playback independently.
- **On/Off always plays the hardcoded default melody**, even when a custom melody is active. The custom `play` command plays the active custom melody, or the default if no custom melody has been committed since boot.
- Melodies are space-separated `NOTE:milliseconds` or `R:milliseconds` tokens. Notes are A–G with optional `#` and octave 3–6, e.g. `C5:150 R:50 E5:150 G5:300`. `R` is silence. Durations are 10–2000 ms; at most 24 tokens and 15 seconds total. Flats and E#/B# are not supported. Maximum melody text is 300 characters. Read-back via the `melody` attribute is truncated to 255 characters (Zigbee string limit).
- The Z2M converter writes `melody_ctrl=START`, ordered `melody_chunk` values (up to 48 characters each; the firmware accepts up to 79, but longer chunks exceed the Zigbee APS payload limit and fail with `ApsDataRequest … InvalidValue`), then `melody_ctrl=COMMIT`; it waits for every ZCL write. These are the ZCL `melody_ctrl` (attribute `0x0002`) and `melody_chunk` (`0x0003`) writes for the start/append/commit steps. The complete concatenated text is parsed at commit, so chunk boundaries may fall anywhere in the text. A failed upload preserves the previous custom melody. Sending an empty melody clears the custom melody and restores play-command fallback to the default without rebooting.
- The original default tune remains the video-game melody. Custom melodies are held in RAM and reset on reboot.
- Any device already joined to the Zigbee network can write this buzzer-only setting.

#### Home Assistant / Zigbee2MQTT melody control
See [`MELODY_HELP.md`](MELODY_HELP.md) for MQTT command-line examples and Home Assistant automations.

Install `z2m/esp32c6_melody.mjs` (also mirrored at `data/external_converters/esp32c6_melody.mjs`) as an external Zigbee2MQTT converter (external JavaScript converters may need enabling in current Z2M releases), then restart Z2M and re-interview/reconfigure the device. It exposes a `melody` text property and a `melody_play` action in addition to the existing switch.

Set the melody. The converter automatically writes start, one or more chunks, and commit in order, waiting for each write:

```yaml
service: mqtt.publish
data:
  topic: zigbee2mqtt/<device_name>/set
  payload: '{"melody":"C5:150 R:50 E5:150 G5:300"}'
```

After the upload completes, trigger custom playback with the Zigbee2MQTT `melody_play` entity/action (value `PLAY`), or publish:

```yaml
service: mqtt.publish
data:
  topic: zigbee2mqtt/<device_name>/set
  payload: '{"melody_play":"PLAY"}'
```

An empty `{"melody":""}` clears the custom melody. The exposed `melody_error` is 0 for success; 1–6 indicate length, syntax, token count, duration, note, or total-duration failures; 7 is an upload-sequence error; 8 is an unknown upload operation. Invalid text never replaces the active custom melody. Z2M registers the custom cluster only during device configure. If `melody` or `melody_play` fails with `UNSUPPORTED_CLUSTER` (for example after a Z2M restart or firmware update), reconfigure the device in the Z2M UI, or publish `{"id":"<ieee>"}` to `zigbee2mqtt/bridge/request/device/configure`. If the entities still do not appear, interview the device again.

### Sensors (Periodic Tasks)
- **CO2 Task:** Reads MHZ19B every 60s, updates attribute, sends ZCL report
- **PM2.5 Task:** Reads SPS30 every 120s, updates attribute, sends ZCL report
- Both tasks wait for Zigbee ready flag before reading
- Thread-safe sensor data storage via mutex

### Display (I2C)
- **API:** `ssd1306_display_update(co2_ppm, pm2_5)` - Updates display with current readings
- Auto-off after 5s (configurable via `ssd1306_display_set_always_on()`)
- Software I2C on GPIO14/15; SPS30 uses a separate hardware I2C bus on GPIO21/22.

### LED Blink Status
- `blink_led(times, on_ms, off_ms)` - Synchronous blink pattern
- Used for button feedback and network events

## FreeRTOS Tasks
1. **button_task** - Debounce handler, network steering, factory reset
2. **buzzer_task** - Melody playback (notification-driven)
3. **co2_sensor_task** - CO2 periodic reading & Zigbee reporting
4. **pm25_sensor_task** - PM2.5 periodic reading & Zigbee reporting

## Key Synchronization
- `s_zigbee_ready` flag - Prevents sensor reads before network join
- `s_sensor_data_mutex` - Protects latest sensor values for display
- `esp_zb_lock_acquire/release` - Zigbee stack thread safety

## Auto-Rejoin Watchdog
If 5 consecutive Zigbee reports fail, the device automatically triggers a network rejoin (same primitive as the medium button press). Counter resets on successful join/report. Logs `Auto-rejoin triggered: co2/pm2.5 report failures` and double-blinks the LED. Tunable via `REJOIN_AFTER_FAILURES` in `esp_zb_light.c`.

## Build and Flash
Do it via vs code extensions (ESP-IDF) - Build/Flash button

The standalone parser and upload-session tests can be run with host GCC/Clang (these do not alter the ESP-IDF VS Code build):

```sh
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/melody_parser.c test/host/test_melody_parser.c -o melody_parser_test
./melody_parser_test
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/melody_parser.c main/melody_upload.c test/host/test_melody_upload.c -o melody_upload_test
./melody_upload_test
```

## Main Entry Point
`esp_zb_light.c` - Initializes all peripherals (GPIO, UART, I2C, LEDC), Zigbee stack, endpoints, and starts FreeRTOS tasks.
