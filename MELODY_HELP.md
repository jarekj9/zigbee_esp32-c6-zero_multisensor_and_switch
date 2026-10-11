# Melody control

This guide describes setting and playing melodies on the ESP32-C6 buzzer through Zigbee2MQTT. Replace `<device_name>` with the device name shown in Z2M.

## Prerequisites

Install `z2m/esp32c6_melody.mjs` (the in-repository copy is also at `data/external_converters/esp32c6_melody.mjs`) as an external Zigbee2MQTT converter. Restart Z2M and reconfigure or re-interview the device so the custom cluster's attributes are discovered. If commands fail with `UNSUPPORTED_CLUSTER melodyCluster`, reconfigure it:

```sh
mosquitto_pub -h <mqtt_host> -t zigbee2mqtt/bridge/request/device/configure \
  -m '{"id":"<ieee_address>"}'
```

## Melody syntax and behavior

Send space-separated tokens such as:

```text
C5:150 R:50 E5:150 G5:300
```

Notes are A–G, optionally followed by `#`, with octaves 3–6. `R` is a rest. Durations are 10–2000 ms. Melodies may contain up to 24 tokens, 300 characters total, and 15 seconds of playback. Because of the 24-token limit, the practical length is about 170 characters of notes; extra characters can only be whitespace. Flats and `E#`/`B#` are invalid.

On/Off always plays the hardcoded default tune. `melody_play=PLAY` plays the active custom melody, or the default if no custom melody was committed since boot. The custom melody is RAM-only and is cleared on reboot. Setting `melody` to the empty string clears it back to the default immediately.

## Upload protocol

The converter performs the upload automatically for one MQTT `melody` property:

1. Write `melody_ctrl=START` to clear/open the staging buffer.
2. Append the full melody in sequential `melody_chunk` writes, each up to 48 characters (the firmware accepts up to 79, but the converter stays lower because longer ZCL writes exceed the Zigbee payload limit). The converter waits for each standard ZCL attribute write before sending the next. Chunk boundaries can occur in the middle of a token; no spaces are added.
3. Write `melody_ctrl=COMMIT`. The firmware validates the complete staged text and replaces the active custom melody only on success.

Example upload; no manual chunk messages are needed:

```sh
docker exec -ti mosquitto mosquitto_pub -h 127.0.0.1 \
  -u '<user>' \
  -P '<password>' \
  -t 'zigbee2mqtt/<device_name>/set' \
  -m '{"melody":"E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120 R:350 G4:120 R:350"}'
```

For example, this 88-character tune is split into ZCL writes of up to 48 characters to stay within the Zigbee payload limit. The firmware accepts up to 300 characters in total, and single-shot writes up to 255 characters (Zigbee string limit).

## Melody examples:
Mario:
E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120 R:350 G4:120 R:350

Fur Elise:
E5:150 D#5:150 E5:150 D#5:150 E5:150 B4:150 D5:150 C5:150 A4:400 C4:150 E4:150 A4:150 B4:400 E4:150 G#4:150 B4:150 C5:400

Beep line sonar:
C6:300 R:1500 C6:300 R:1500 C6:300 R:1500 C6:300 R:1500 C6:300 R:1500 C6:300 R:1500 C6:300 R:1500 C6:300

Siren:
G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500 G4:500 C5:500

Laser zap (fast downward sweep):
C6:40 B5:40 A5:40 G5:40 F5:40 E5:40 D5:40 C5:40 B4:40 A4:40 G4:40 F4:40 E4:40 D4:40 C4:40

Good for notification:
E5:100 R:50 A5:100 R:50 C6:200 R:100 B5:100 R:50 E6:300

Mario coin:
B5:80 E6:600

Mario Level UP:
C5:100 E5:100 G5:100 C6:300 R:50 G5:100 C6:600

Heartbeat:
C3:80 R:60 C3:150 R:500 C3:80 R:60 C3:150 R:500 C3:80 R:60 C3:150 R:500 C3:80 R:60 C3:150 R:500 C3:80 R:60 C3:150 R:500 C3:80 R:60 C3:150 R:500


```text
E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120 R:350 G4:120 R:350
```

An empty value starts and commits an empty staging buffer, which clears the custom melody:

```sh
mosquitto_pub -h <mqtt_host> -t 'zigbee2mqtt/<device_name>/set' -m '{"melody":""}'
```

## Play the melody

After Z2M reports that the `melody` set operation has completed, send the separate play action:

```sh
docker exec -ti mosquitto mosquitto_pub -h 127.0.0.1 \
  -u '<user>' \
  -P '<password>' \
  -t 'zigbee2mqtt/<device_name>/set' \
  -m '{"melody_play":"PLAY"}'
```

Z2M/HA automations should sequence the melody set before the play action; allow the set operation to finish before triggering playback.

## Home Assistant automation

Use two `mqtt.publish` actions in this order. The first message is chunked by the converter; trigger playback only after the set operation has finished:

```yaml
action:
  - service: mqtt.publish
    data:
      topic: zigbee2mqtt/<device_name>/set
      payload: '{"melody":"C5:150 R:50 E5:150 G5:300"}'
  - delay: "00:00:02"
  - service: mqtt.publish
    data:
      topic: zigbee2mqtt/<device_name>/set
      payload: '{"melody_play":"PLAY"}'
```

## Errors

The `melody_error` value reports the last firmware operation: `0` success, `1` invalid length, `2` syntax, `3` token count, `4` duration, `5` note, `6` total duration, `7` upload sequence, and `8` unknown control operation. On any failed converter write, the converter attempts to abort staging and read `last_error`; check the Z2M log and `melody_error` state. Failed validation leaves the previous active custom melody unchanged.
