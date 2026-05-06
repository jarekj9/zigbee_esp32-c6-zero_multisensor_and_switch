/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * SPS30 PM2.5 Sensor Driver
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPS30 I2C Configuration */
#define SPS30_I2C_NUM           I2C_NUM_0
#define SPS30_I2C_SDA_PIN       21
#define SPS30_I2C_SCL_PIN       22
#define SPS30_I2C_FREQ_HZ       50000   // Use 50kHz for better stability with clock stretching
#define SPS30_I2C_ADDR          0x69
#define SPS30_I2C_ADDR_ALT      0x68    // Alternate address (some variants)

/* SPS30 Commands */
#define SPS30_CMD_START_MEASUREMENT     0x0010
#define SPS30_CMD_STOP_MEASUREMENT      0x0104
#define SPS30_CMD_READ_MEASUREMENT      0x0300
#define SPS30_CMD_WAKE_UP               0x1103
#define SPS30_CMD_RESET                 0xD304
#define SPS30_CMD_GET_SERIAL            0xD033

/* Error codes */
typedef enum {
    SPS30_OK = 0,
    SPS30_ERR_NOT_FOUND,        // Sensor not detected on I2C bus
    SPS30_ERR_TIMEOUT,          // Timeout waiting for data
    SPS30_ERR_CRC,              // CRC mismatch
    SPS30_ERR_I2C,              // I2C communication error
    SPS30_ERR_INVALID_RESPONSE, // Invalid response from sensor
} sps30_err_t;

/**
 * @brief SPS30 measurement data structure
 */
typedef struct {
    float pm1_0;        // PM1.0 mass concentration [ug/m^3]
    float pm2_5;        // PM2.5 mass concentration [ug/m^3]
    float pm4_0;        // PM4.0 mass concentration [ug/m^3]
    float pm10;         // PM10 mass concentration [ug/m^3]
    float nc0_5;        // Number concentration of particles > 0.5um [#/cm^3]
    float nc1_0;        // Number concentration of particles > 1.0um [#/cm^3]
    float nc2_5;        // Number concentration of particles > 2.5um [#/cm^3]
    float nc4_0;        // Number concentration of particles > 4.0um [#/cm^3]
    float nc10;         // Number concentration of particles > 10um [#/cm^3]
    float typical_size; // Typical particle size [um]
    bool valid;         // True if data is valid
    sps30_err_t error;  // Error code
} sps30_reading_t;

/**
 * @brief Initialize SPS30 I2C communication
 *
 * @return esp_err_t ESP_OK on success, ESP_FAIL if sensor not found
 */
esp_err_t sps30_init(void);

/**
 * @brief Check if SPS30 sensor is connected and responding
 *
 * @return true if sensor is available
 */
bool sps30_is_available(void);

/**
 * @brief Start continuous measurement mode
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sps30_start_measurement(void);

/**
 * @brief Stop measurement mode
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sps30_stop_measurement(void);

/**
 * @brief Read measurement data from SPS30
 *
 * @param reading Pointer to store the reading result
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sps30_read_measurement(sps30_reading_t *reading);

/**
 * @brief Get sensor serial number
 *
 * @param serial Buffer to store serial number (min 32 bytes)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sps30_get_serial(char *serial);

/**
 * @brief Reset the sensor
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t sps30_reset(void);

/**
 * @brief Get last error as string
 *
 * @param err Error code
 * @return const char* Error description
 */
const char* sps30_err_to_str(sps30_err_t err);

#ifdef __cplusplus
} // extern "C"
#endif
