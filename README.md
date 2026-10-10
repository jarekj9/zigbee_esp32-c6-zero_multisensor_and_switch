# ESP32-C6 Zigbee Multi-Sensor Device

## Overview
Zigbee End Device (ED) based on ESP32-C6-Zero with CO2 and PM2.5 sensors, OLED display, button controls, and sound output via Zigbee on/off commands.

## Hardware
- **Sensors:**
  - MHZ19B (UART) - CO2 measurement, 9600 baud
  - SPS30 (I2C) - PM2.5 & PM1.0 mass concentration
  - SSD1306 (I2C) - 128x64 OLED display
- **Actuators:**
  - Buzzer (GPIO20, LEDC) - Melody playback via Zigbee on/off
  - RGB LED (via led_strip driver) - Status indication
- **Input:**
  - Button (GPIO2, debounced 500ms, pull-down, active high) - 3 functions

## Zigbee Configuration
| Endpoint | Device Type | Cluster | Function |
|----------|-------------|---------|----------|
| 10 | HA Light | On/Off, custom `0xFC00` | Buzzer; melody text attr `0x0000`, last_error attr `0x0001`, play cmd `0x00` |
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
- **API:** `buzzer_play_melody_async()` - Plays video game style melody
- Task-based; triggered via task notification by the existing On/Off control or the custom melody `play` command
- The default tune remains the original video-game melody. On/Off continues to trigger playback; when a custom tune is set, On/Off plays that tune.
- Melodies are written as space-separated `NOTE:milliseconds` or `R:milliseconds` tokens. Notes are A–G with optional `#` and octave 3–6, e.g. `C5:150 R:50 E5:150 G5:300`. `R` is silence. Durations are 10–2000 ms; at most 24 tokens and 15 seconds total. In practice, Zigbee2MQTT/Zigbee attribute writes currently accept approximately 80 characters (even though the parser allows 120), so keep melodies at 80 characters or fewer. For example, this shortened Mario melody fits: `E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120 G4:120`. Flats and E#/B# are not supported.
- Configured melodies are held in RAM and reset to the default after reboot.
- Any device already joined to the Zigbee network can write this buzzer-only setting.

#### Home Assistant / Zigbee2MQTT melody control
See [`MELODY_HELP.md`](MELODY_HELP.md) for MQTT command-line examples and Home Assistant automations.

Install `z2m/esp32c6_melody.mjs` as an external Zigbee2MQTT converter (external JavaScript converters may need enabling in current Z2M releases), then re-interview/reconfigure the device. It exposes a `melody` text property and a `melody_play` action in addition to the existing switch.

Set the melody and trigger playback as separate MQTT actions so the write completes before playback:

```yaml
service: mqtt.publish
data:
  topic: zigbee2mqtt/<device_name>/set
  payload: '{"melody":"C5:150 R:50 E5:150 G5:300"}'
```

Then trigger it with the Zigbee2MQTT `melody_play` entity/action (value `PLAY`), or publish:

```yaml
service: mqtt.publish
data:
  topic: zigbee2mqtt/<device_name>/set
  payload: '{"melody_play":"PLAY"}'
```

Alternatively publish the two messages in order to `zigbee2mqtt/<device_name>/set`. The exposed `melody_error` value is `0` for success and `1`–`6` for invalid length, syntax, token count, note duration, note, or total duration. Invalid text does not replace the last valid tune. Z2M registers the custom cluster only during device configure. If `melody` or `melody_play` fails with `UNSUPPORTED_CLUSTER` (for example after a Z2M restart or firmware update), reconfigure the device in the Z2M UI, or publish `{"id":"<ieee>"}` to `zigbee2mqtt/bridge/request/device/configure`. If the entities still do not appear, interview the device again.

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

The standalone parser tests can be run with a host GCC/Clang installation:

```sh
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/melody_parser.c test/host/test_melody_parser.c -o melody_parser_test
./melody_parser_test
```

## Main Entry Point
`esp_zb_light.c` - Initializes all peripherals (GPIO, UART, I2C, LEDC), Zigbee stack, endpoints, and starts FreeRTOS tasks.
