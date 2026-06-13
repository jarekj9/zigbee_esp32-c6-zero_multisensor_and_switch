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
  - Button (GPIO2, debounced 500ms, pull-up) - 3 functions

## Zigbee Configuration
| Endpoint | Device Type | Cluster | Function |
|----------|-------------|---------|----------|
| 10 | HA Light | On/Off | Buzzer control (plays melody) |
| 11 | CO2 Sensor | Carbon Dioxide Measurement | CO2 reporting (60s interval) |
| 12 | PM2.5 Sensor | PM2.5 Measurement | PM2.5 reporting (120s interval) |

Cluster attributes use standard ZCL: CO2 normalized to 0.0-1.0 (ppm/1000000), PM2.5 in µg/m³.

## Pin Configuration
| GPIO | Function | Notes |
|------|----------|-------|
| 1,3 | MHZ19B UART | TX/RX |
| 14,15 | SSD1306 I2C | SDA/SCL (I2C_NUM_0) |
| 21,22 | SPS30 I2C | SDA/SCL (I2C_NUM_0, shared) |
| 2 | Button | Active low, debounced |
| 20 | Buzzer | LEDC PWM output |

## Core Functions

### Button Control (Interrupt + Task)
- **Short press (<500ms):** Toggle display always-on mode
- **Medium press (500ms-5s):** Trigger Zigbee network rejoin (5s cooldown)
- **Long press (≥5s):** Factory reset & LED blink pattern

### Buzzer (LEDC + Task Notification)
- **API:** `buzzer_play_melody_async()` - Plays video game style melody
- Task-based; triggered via task notification on Zigbee on/off commands
- Single melody with note frequencies defined (E5, C5, G5, etc.)

### Sensors (Periodic Tasks)
- **CO2 Task:** Reads MHZ19B every 60s, updates attribute, sends ZCL report
- **PM2.5 Task:** Reads SPS30 every 120s, updates attribute, sends ZCL report
- Both tasks wait for Zigbee ready flag before reading
- Thread-safe sensor data storage via mutex

### Display (I2C)
- **API:** `ssd1306_display_update(co2_ppm, pm2_5)` - Updates display with current readings
- Auto-off after 5s (configurable via `ssd1306_display_set_always_on()`)
- Dual I2C bus (shared with SPS30 at I2C_NUM_0)

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

## Build and Flash
Do it via vs code extensions (ESP-IDF) - Build/Flash button

## Main Entry Point
`esp_zb_light.c` - Initializes all peripherals (GPIO, UART, I2C, LEDC), Zigbee stack, endpoints, and starts FreeRTOS tasks.
