/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * MH-Z19B CO2 Sensor Driver
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MH-Z19B UART Configuration */
#define MHZ19B_UART_NUM         UART_NUM_1
#define MHZ19B_UART_BAUD_RATE   9600
#define MHZ19B_UART_TX_PIN      1
#define MHZ19B_UART_RX_PIN      3
#define MHZ19B_BUF_SIZE         256

/* MH-Z19B Command Frame Structure */
#define MHZ19B_FRAME_START_BYTE 0xFF
#define MHZ19B_CMD_READ_CO2     0x86
#define MHZ19B_CMD_CALIBRATE    0x87

/* Error codes */
typedef enum {
    MHZ19B_OK = 0,
    MHZ19B_ERR_TIMEOUT,
    MHZ19B_ERR_CHECKSUM,
    MHZ19B_ERR_UART,
    MHZ19B_ERR_INVALID_RESPONSE,
} mhz19b_err_t;

/**
 * @brief CO2 reading result structure
 */
typedef struct {
    uint16_t co2_ppm;
    bool valid;
    mhz19b_err_t error;
} mhz19b_reading_t;

/**
 * @brief Initialize MH-Z19B UART communication
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mhz19b_init(void);

/**
 * @brief Read CO2 value from MH-Z19B sensor
 *
 * @param reading Pointer to store the reading result
 * @return esp_err_t ESP_OK on success
 */
esp_err_t mhz19b_read_co2(mhz19b_reading_t *reading);

/**
 * @brief Check if sensor reading is valid (not error/unavailable)
 *
 * @param reading Pointer to reading structure
 * @return true if reading is valid
 */
bool mhz19b_is_reading_valid(const mhz19b_reading_t *reading);

/**
 * @brief Get last error as string
 *
 * @param err Error code
 * @return const char* Error description
 */
const char* mhz19b_err_to_str(mhz19b_err_t err);

#ifdef __cplusplus
} // extern "C"
#endif
