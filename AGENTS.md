# AGENTS.md

Expert guidance for coding the ESP32-C6-Zero Zigbee multi-sensor (ESP-IDF). Hardware reference: https://www.waveshare.com/wiki/ESP32-C6-Zero

## Project summary
- Zigbee End Device with MHZ19B (CO2), SPS30 (PM2.5), SSD1306 OLED, button, and buzzer.
- Endpoints: 10 = On/Off + buzzer custom cluster `0xFC00`; 11 = CO2; 12 = PM2.5.
- Full protocol, pins, and button/buzzer behaviour are in `README.md`. Read it before changing behaviour.

## Build, flash, test
- Build and flash through the VS Code ESP-IDF extension (Build/Flash button).
- Host parser tests (run after any change to melody parsing):
  ```sh
  gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/melody_parser.c test/host/test_melody_parser.c -o melody_parser_test
  ./melody_parser_test
  gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main main/melody_parser.c main/melody_upload.c test/host/test_melody_upload.c -o melody_upload_test
  ./melody_upload_test
  ```
- Do not claim a firmware change works without a successful build; say clearly when you could not test on hardware.

## Code map
- `main/esp_zb_light.c`: entry point, peripheral init, Zigbee stack, endpoints, task creation.
- `main/melody_parser.c`: melody text parsing and validation (limits: 24 tokens, 300 chars, 15 s total, notes 10-2000 ms).
- Buzzer, sensor, display, button, and LED logic live alongside these files in `main/`.

## Rules
- Keep `README.md` in sync with any protocol, endpoint, pin, or behaviour change.
- Melody rules must match the README exactly: A-G with optional `#`, octave 3-6, `R` for rest, no flats, no E#/B#. Parser error codes 0-6 and upload error codes 7-8 must stay in sync with the Z2M converter.
- Melodies are RAM-only and reset on reboot. Do not add flash storage without asking.
- Keep Zigbee stack calls inside `esp_zb_lock_acquire/release`. Protect shared sensor data with `s_sensor_data_mutex`.
- Do not read unrelated files or commit secrets.

## Zigbee2MQTT (home server)
- Host: `debian@192.168.0.26`. Compose dir: `/home/debian/skrypty/docker/zigbee2mqtt`.
- External converter: `data/external_converters/esp32c6_melody.mjs` (source copy: `z2m/esp32c6_melody.mjs`). Keep both in sync.
- Logs: `docker logs -n 200 zigbee2mqtt`.
- Known gotcha: after a Z2M restart the custom cluster is unknown until the device is reconfigured. Symptom: `UNSUPPORTED_CLUSTER melodyCluster`. Fix: reconfigure the device in the Z2M UI, or publish `{"id":"0xa085e3fffeae3688"}` to `zigbee2mqtt/bridge/request/device/configure`.
