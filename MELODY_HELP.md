# Melody control

This guide shows how to configure and play custom melodies on the ESP32-C6 buzzer through Zigbee2MQTT.

Replace `<device_name>` with the device name shown in Zigbee2MQTT. The melody is stored in RAM and returns to the default melody after reboot.

## Prerequisites

Install `z2m/esp32c6_melody.mjs` as an external Zigbee2MQTT converter and reconfigure the device. If commands fail with `UNSUPPORTED_CLUSTER melodyCluster`, reconfigure it again:

```sh
mosquitto_pub -h <mqtt_host> -t zigbee2mqtt/bridge/request/device/configure \
  -m '{"id":"0xa085e3fffeae3688"}'
```

## Melody syntax

Send space-separated tokens:

```text
C5:150 R:50 E5:150 G5:300
```

Notes are `A`–`G`, optionally followed by `#`, with octaves 3–6. `R` is a rest. Durations are milliseconds from 10 to 2000. A melody can contain up to 24 tokens, 120 characters, and 15 seconds total. Flats and `E#`/`B#` are not supported.

## MQTT command line

Set a melody:

```sh
docker exec -ti mosquitto mosquitto_pub -h 127.0.0.1 \
  -u '<user>' \
  -P '<password>' \
  -t 'zigbee2mqtt/0xa085e3fffeae3688/set' \
  -m '{"melody":"E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120"}'
```

Play the configured melody. Send this as a separate command after the set command has completed:

```sh
docker exec -ti mosquitto mosquitto_pub -h 127.0.0.1 \
  -u '<user>' \
  -P '<password>' \
  -t 'zigbee2mqtt/0xa085e3fffeae3688/set' \
  -m '{"melody_play":"PLAY"}'
```


Do not depend on the two MQTT publishes arriving in the correct order when using scripts. Wait for the first publish to complete before sending `melody_play`.

## Home Assistant automation

Add action → Call service, then select:
mqtt.publish
Configure:
Topic
zigbee2mqtt/0xa085e3fffeae3688/set
Payload
{"melody":"E5:120 R:30 E5:120 R:150 E5:120 R:150 C5:120 R:30 E5:120 R:150 G5:120"}
Add a 1-second delay, then add another mqtt.publish action:
{"melody_play":"PLAY"}
You can also switch the automation editor to YAML mode and paste the complete automation from MELODY_HELP.md.

## Melody errors

The `melody_error` value is `0` for success. Values `1`–`6` mean invalid length, syntax, token count, note duration, note, or total duration. Invalid text does not replace the last valid melody.
