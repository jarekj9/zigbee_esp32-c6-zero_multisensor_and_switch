/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * MH-Z19B CO2 Sensor Driver Implementation
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "mhz19b_driver.h"

static const char *TAG = "MH-Z19B";

/* MH-Z19B Read CO2 Command Frame */
static const uint8_t mhz19b_cmd_read[] = {
    0xFF,  // Start byte
    0x01,  // Sensor number
    0x86,  // Command: read CO2
    0x00,  // Reserved
    0x00,  // Reserved
    0x00,  // Reserved
    0x00,  // Reserved
    0x00,  // Reserved
    0x79   // Checksum
};

static uint8_t calc_checksum(const uint8_t *data, size_t len)
{
    uint8_t checksum = 0;
    for (size_t i = 1; i < len; i++) {
        checksum += data[i];
    }
    return 0xFF - checksum + 1;
}

static bool verify_checksum(const uint8_t *response)
{
    uint8_t expected = calc_checksum(response, 8);
    return expected == response[8];
}

esp_err_t mhz19b_init(void)
{
    ESP_LOGI(TAG, "Initializing MH-Z19B on UART%d (TX=%d, RX=%d)",
             MHZ19B_UART_NUM, MHZ19B_UART_TX_PIN, MHZ19B_UART_RX_PIN);

    uart_config_t uart_config = {
        .baud_rate = MHZ19B_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(MHZ19B_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(MHZ19B_UART_NUM, MHZ19B_UART_TX_PIN, MHZ19B_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(MHZ19B_UART_NUM, MHZ19B_BUF_SIZE, MHZ19B_BUF_SIZE,
                              0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    // Flush any garbage in buffer
    uart_flush(MHZ19B_UART_NUM);

    ESP_LOGI(TAG, "MH-Z19B initialized successfully");
    return ESP_OK;
}

esp_err_t mhz19b_read_co2(mhz19b_reading_t *reading)
{
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Initialize with invalid reading
    reading->co2_ppm = 0;
    reading->valid = false;
    reading->error = MHZ19B_ERR_TIMEOUT;

    // Flush RX buffer before sending command
    uart_flush_input(MHZ19B_UART_NUM);

    // Send read command
    int tx_bytes = uart_write_bytes(MHZ19B_UART_NUM,
                                    (const char *)mhz19b_cmd_read,
                                    sizeof(mhz19b_cmd_read));
    if (tx_bytes != sizeof(mhz19b_cmd_read)) {
        ESP_LOGE(TAG, "Failed to send command: wrote %d bytes", tx_bytes);
        reading->error = MHZ19B_ERR_UART;
        return ESP_FAIL;
    }

    // Wait for response (9 bytes expected)
    uint8_t response[9];
    int rx_bytes = uart_read_bytes(MHZ19B_UART_NUM, response, 9,
                                   pdMS_TO_TICKS(500));

    if (rx_bytes < 0) {
        ESP_LOGE(TAG, "UART read error");
        reading->error = MHZ19B_ERR_UART;
        return ESP_FAIL;
    }

    if (rx_bytes < 9) {
        ESP_LOGE(TAG, "Timeout waiting for response (got %d bytes)", rx_bytes);
        reading->error = MHZ19B_ERR_TIMEOUT;
        return ESP_FAIL;
    }

    // Validate response structure
    if (response[0] != 0xFF) {
        ESP_LOGE(TAG, "Invalid start byte: 0x%02X", response[0]);
        reading->error = MHZ19B_ERR_INVALID_RESPONSE;
        return ESP_FAIL;
    }

    if (response[1] != 0x86) {
        ESP_LOGE(TAG, "Invalid command byte: 0x%02X", response[1]);
        reading->error = MHZ19B_ERR_INVALID_RESPONSE;
        return ESP_FAIL;
    }

    // Verify checksum
    if (!verify_checksum(response)) {
        ESP_LOGE(TAG, "Checksum mismatch");
        reading->error = MHZ19B_ERR_CHECKSUM;
        return ESP_FAIL;
    }

    // Extract CO2 value (high byte first)
    reading->co2_ppm = ((uint16_t)response[2] << 8) | response[3];
    reading->valid = true;
    reading->error = MHZ19B_OK;

    ESP_LOGD(TAG, "CO2 reading: %u ppm", reading->co2_ppm);
    return ESP_OK;
}

bool mhz19b_is_reading_valid(const mhz19b_reading_t *reading)
{
    return (reading != NULL) && reading->valid;
}

const char* mhz19b_err_to_str(mhz19b_err_t err)
{
    switch (err) {
        case MHZ19B_OK: return "OK";
        case MHZ19B_ERR_TIMEOUT: return "Timeout";
        case MHZ19B_ERR_CHECKSUM: return "Checksum error";
        case MHZ19B_ERR_UART: return "UART error";
        case MHZ19B_ERR_INVALID_RESPONSE: return "Invalid response";
        default: return "Unknown error";
    }
}
