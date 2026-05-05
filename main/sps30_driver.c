/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * SPS30 PM2.5 Sensor Driver Implementation
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "sps30_driver.h"

static const char *TAG = "SPS30";

static bool s_sensor_available = false;
static uint8_t s_sensor_addr = SPS30_I2C_ADDR; // Detected address

/* Calculate CRC for SPS30 data (CRC-8, polynomial 0x31, init 0xFF) */
static uint8_t sps30_calc_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* Convert command to TX buffer with CRC */
static void sps30_cmd_to_buf(uint16_t cmd, uint8_t *buf)
{
    buf[0] = (cmd >> 8) & 0xFF;
    buf[1] = cmd & 0xFF;
}

/* Send command to SPS30 - using low-level API for better control */
static esp_err_t sps30_send_cmd(uint16_t cmd, const uint8_t *data, size_t data_len)
{
    uint8_t buf[20];
    size_t len = 2;

    sps30_cmd_to_buf(cmd, buf);

    // Add data with CRC if provided
    if (data != NULL && data_len > 0) {
        for (size_t i = 0; i < data_len; i += 2) {
            buf[len++] = data[i];
            buf[len++] = data[i + 1];
            buf[len++] = sps30_calc_crc(&data[i], 2);
        }
    }

    // Use low-level API for more control over timing
    i2c_cmd_handle_t cmd_handle = i2c_cmd_link_create();
    i2c_master_start(cmd_handle);
    i2c_master_write_byte(cmd_handle, (s_sensor_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd_handle, buf, len, true);
    i2c_master_stop(cmd_handle);
    esp_err_t err = i2c_master_cmd_begin(SPS30_I2C_NUM, cmd_handle, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(cmd_handle);

    return err;
}

/* Read data from SPS30 - using low-level API with repeated start */
static esp_err_t sps30_read_data(uint8_t *data, size_t len)
{
    // Use low-level API for repeated start condition (required by SPS30)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_sensor_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(SPS30_I2C_NUM, cmd, pdMS_TO_TICKS(200));
    i2c_cmd_link_delete(cmd);

    return err;
}

/* Verify CRC for received data */
static bool sps30_verify_crc(const uint8_t *data, size_t data_len, uint8_t received_crc)
{
    uint8_t calc_crc = sps30_calc_crc(data, data_len);
    return calc_crc == received_crc;
}

/* Parse 2 bytes with CRC verification */
static bool sps30_parse_byte_pair(const uint8_t *buf, uint16_t *value)
{
    if (!sps30_verify_crc(buf, 2, buf[2])) {
        return false;
    }
    *value = ((uint16_t)buf[0] << 8) | buf[1];
    return true;
}

/* Convert uint16 to float according to SPS30 specification */
static float sps30_bytes_to_float(const uint8_t *buf)
{
    // SPS30 transmits IEEE 754 float as big-endian
    union {
        uint8_t bytes[4];
        float value;
    } converter;

    converter.bytes[3] = buf[0];
    converter.bytes[2] = buf[1];
    converter.bytes[1] = buf[3];
    converter.bytes[0] = buf[4];

    return converter.value;
}

/* Parse float value with CRC verification */
static bool sps30_parse_float(const uint8_t *buf, float *value)
{
    // Verify CRC for both 2-byte pairs
    if (!sps30_verify_crc(&buf[0], 2, buf[2]) ||
        !sps30_verify_crc(&buf[3], 2, buf[5])) {
        return false;
    }

    *value = sps30_bytes_to_float(buf);
    return true;
}



/* Wake up SPS30 from sleep mode - per datasheet */
static void sps30_wake_up(void)
{
    // Send Start + Stop condition (creates low pulse on SDA), then wake-up command
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_stop(cmd);
    i2c_master_cmd_begin(SPS30_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    
    // Send Wake-up command within 100ms
    sps30_send_cmd(SPS30_CMD_WAKE_UP, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
}

/* Probe a specific I2C address */
static bool sps30_probe_addr(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(SPS30_I2C_NUM, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return (err == ESP_OK);
}

esp_err_t sps30_init(void)
{
    ESP_LOGI(TAG, "Initializing SPS30 on I2C (SDA=%d, SCL=%d)", SPS30_I2C_SDA_PIN, SPS30_I2C_SCL_PIN);

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SPS30_I2C_SDA_PIN,
        .scl_io_num = SPS30_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = SPS30_I2C_FREQ_HZ,
        .clk_flags = 0, // Use default clock
    };

    esp_err_t err = i2c_param_config(SPS30_I2C_NUM, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(SPS30_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    // SPS30 needs time after power-on (datasheet says up to 1.5s for fan to reach speed)
    ESP_LOGI(TAG, "Waiting for SPS30 to boot...");
    vTaskDelay(pdMS_TO_TICKS(500));

    // Wake up sensor (might be in sleep mode) and detect address
    sps30_wake_up();
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Try standard address first, then alternate
    if (sps30_probe_addr(SPS30_I2C_ADDR)) {
        s_sensor_addr = SPS30_I2C_ADDR;
    } else if (sps30_probe_addr(SPS30_I2C_ADDR_ALT)) {
        s_sensor_addr = SPS30_I2C_ADDR_ALT;
    }
    
    // Reset sensor
    sps30_send_cmd(SPS30_CMD_RESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(200));

    // Check if sensor is present by reading serial number
    char serial[32] = {0};
    bool sensor_found = false;
    
    for (int retry = 0; retry < 3; retry++) {
        if (retry > 0) {
            sps30_wake_up();
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        
        err = sps30_get_serial(serial);
        if (err == ESP_OK && strlen(serial) > 0) {
            sensor_found = true;
            break;
        }
    }
    
    if (sensor_found) {
        ESP_LOGI(TAG, "SPS30 detected, serial: %s", serial);
        s_sensor_available = true;
        // Note: Measurement is not started here - task will start/stop it on each read
        // This saves power and extends fan life
    } else {
        ESP_LOGW(TAG, "SPS30 not detected - ensure SEL=GND at power-up");
        s_sensor_available = false;
        // Uninstall I2C driver since sensor is not present
        i2c_driver_delete(SPS30_I2C_NUM);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool sps30_is_available(void)
{
    return s_sensor_available;
}

esp_err_t sps30_start_measurement(void)
{
    // Start measurement command with measurement mode 0x03 (float values)
    uint8_t arg[] = {0x03, 0x00};
    return sps30_send_cmd(SPS30_CMD_START_MEASUREMENT, arg, sizeof(arg));
}

esp_err_t sps30_stop_measurement(void)
{
    return sps30_send_cmd(SPS30_CMD_STOP_MEASUREMENT, NULL, 0);
}

esp_err_t sps30_read_measurement(sps30_reading_t *reading)
{
    if (reading == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(reading, 0, sizeof(sps30_reading_t));
    reading->valid = false;
    reading->error = SPS30_ERR_TIMEOUT;

    if (!s_sensor_available) {
        reading->error = SPS30_ERR_NOT_FOUND;
        return ESP_FAIL;
    }

    sps30_wake_up();
    vTaskDelay(pdMS_TO_TICKS(5));

    // Send read measurement command
    esp_err_t err = sps30_send_cmd(SPS30_CMD_READ_MEASUREMENT, NULL, 0);
    if (err != ESP_OK) {
        reading->error = SPS30_ERR_I2C;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    // Read 60 bytes (10 float values with CRC)
    uint8_t buf[60] = {0};
    err = sps30_read_data(buf, sizeof(buf));
    if (err != ESP_OK) {
        reading->error = SPS30_ERR_I2C;
        return err;
    }

    // Parse all float values
    float values[10];
    for (int i = 0; i < 10; i++) {
        if (!sps30_parse_float(&buf[i * 6], &values[i])) {
            reading->error = SPS30_ERR_CRC;
            return ESP_FAIL;
        }
    }

    reading->pm1_0 = values[0];
    reading->pm2_5 = values[1];
    reading->pm4_0 = values[2];
    reading->pm10 = values[3];
    reading->nc0_5 = values[4];
    reading->nc1_0 = values[5];
    reading->nc2_5 = values[6];
    reading->nc4_0 = values[7];
    reading->nc10 = values[8];
    reading->typical_size = values[9];
    reading->valid = true;
    reading->error = SPS30_OK;

    return ESP_OK;
}

esp_err_t sps30_get_serial(char *serial)
{
    if (serial == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sps30_wake_up();
    vTaskDelay(pdMS_TO_TICKS(5));

    // Send get serial command
    esp_err_t err = sps30_send_cmd(SPS30_CMD_GET_SERIAL, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // Read 48 bytes (serial number is 32 ASCII chars with CRC)
    uint8_t buf[48] = {0};
    err = sps30_read_data(buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    // Parse serial (16 word pairs with CRC)
    int idx = 0;
    for (int i = 0; i < 16 && idx < 31; i++) {
        uint16_t word;
        if (!sps30_parse_byte_pair(&buf[i * 3], &word)) {
            return ESP_FAIL;
        }
        serial[idx++] = (word >> 8) & 0xFF;
        if (idx < 31) {
            serial[idx++] = word & 0xFF;
        }
    }
    serial[idx] = '\0';

    return ESP_OK;
}

esp_err_t sps30_reset(void)
{
    esp_err_t err = sps30_send_cmd(SPS30_CMD_RESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100)); // Wait for reset to complete
    return err;
}

const char* sps30_err_to_str(sps30_err_t err)
{
    switch (err) {
        case SPS30_OK: return "OK";
        case SPS30_ERR_NOT_FOUND: return "Sensor not found";
        case SPS30_ERR_TIMEOUT: return "Timeout";
        case SPS30_ERR_CRC: return "CRC error";
        case SPS30_ERR_I2C: return "I2C error";
        case SPS30_ERR_INVALID_RESPONSE: return "Invalid response";
        default: return "Unknown error";
    }
}
