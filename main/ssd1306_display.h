/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * SSD1306 OLED Display Driver for Air Quality Monitor
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SSD1306 I2C Configuration */
#define SSD1306_I2C_SDA_PIN     14
#define SSD1306_I2C_SCL_PIN     15
#define SSD1306_I2C_ADDR        0x3C
#define SSD1306_I2C_ADDR_ALT    0x3D

/* Display timing */
#define SSD1306_DISPLAY_TIME_MS 5000  /* Display stays on for 5 seconds */

/**
 * @brief Initialize SSD1306 display
 *
 * @return esp_err_t ESP_OK on success, ESP_FAIL if display not found
 */
esp_err_t ssd1306_display_init(void);

/**
 * @brief Check if display is connected and available
 *
 * @return true if display is available
 */
bool ssd1306_display_is_available(void);

/**
 * @brief Update display with sensor values
 *        Display will automatically turn off after SSD1306_DISPLAY_TIME_MS
 *
 * @param co2_ppm CO2 value in ppm (0 = invalid/unavailable)
 * @param pm2_5 PM2.5 value in ug/m3 (negative or NaN = invalid/unavailable)
 */
void ssd1306_display_update(uint16_t co2_ppm, float pm2_5);

/**
 * @brief Turn off display (sleep mode)
 */
void ssd1306_display_off(void);

/**
 * @brief Turn on display
 */
void ssd1306_display_on(void);

/**
 * @brief Clear display
 */
void ssd1306_display_clear(void);

#ifdef __cplusplus
} // extern "C"
#endif
