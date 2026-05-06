/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * SSD1306 OLED Display Driver for Air Quality Monitor
 * Uses software I2C (bit-banged) to avoid conflict with SPS30 sensor
 */

#include "ssd1306_display.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "string.h"
#include "math.h"
#include "stdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "SSD1306_DISPLAY";

/* SSD1306 Commands */
#define SSD1306_CMD_DISPLAY_OFF     0xAE
#define SSD1306_CMD_DISPLAY_ON      0xAF
#define SSD1306_CMD_SET_CONTRAST    0x81
#define SSD1306_CMD_SET_COL_ADDR    0x21
#define SSD1306_CMD_SET_PAGE_ADDR   0x22
#define SSD1306_CMD_SET_MEM_ADDR    0x20
#define SSD1306_CMD_SET_START_LINE  0x40
#define SSD1306_CMD_SET_SEGMENT_REMAP 0xA0
#define SSD1306_CMD_SET_COM_SCAN_DIR 0xC0
#define SSD1306_CMD_SET_COM_PINS    0xDA
#define SSD1306_CMD_SET_MULTIPLEX   0xA8
#define SSD1306_CMD_SET_DISPLAY_OFFSET 0xD3
#define SSD1306_CMD_SET_CLOCK_DIV   0xD5
#define SSD1306_CMD_SET_PRECHARGE   0xD9
#define SSD1306_CMD_SET_VCOM_DETECT 0xDB
#define SSD1306_CMD_CHARGE_PUMP     0x8D

/* Display dimensions */
#define SSD1306_WIDTH               128
#define SSD1306_HEIGHT              64
#define SSD1306_PAGES               8

/* I2C Control byte */
#define SSD1306_CONTROL_CMD         0x00
#define SSD1306_CONTROL_DATA        0x40

static bool s_display_available = false;
static TaskHandle_t s_display_task_handle = NULL;
static SemaphoreHandle_t s_i2c_mutex = NULL;
static uint8_t s_display_addr = SSD1306_I2C_ADDR;

/* Display framebuffer */
static uint8_t s_framebuffer[SSD1306_PAGES][SSD1306_WIDTH];

/* Latest sensor values stored for display */
static uint16_t s_last_co2_ppm = 0;
static float s_last_pm2_5 = 0.0f / 0.0f;
static SemaphoreHandle_t s_data_mutex = NULL;

/* Forward declarations */
static void ssd1306_display_task(void *pvParameter);
static void update_display_content(void);
static void ssd1306_refresh_display(void);
static bool ssd1306_write_command_addr(uint8_t addr, uint8_t cmd);
static bool ssd1306_write_data_addr(uint8_t addr, const uint8_t *data, size_t len);
static void draw_char(int x, int y, char c);
static void draw_string(int x, int y, const char *str);
static void draw_hline(int x, int y, int width);

/* Software I2C functions with better timing */
static inline void i2c_delay(void)
{
    /* Delay for ~50kHz I2C timing (slower for better reliability) */
    for (volatile int i = 0; i < 40; i++) {
        __asm__ volatile ("nop");
    }
}

static inline void sda_low(void)
{
    gpio_set_level(SSD1306_I2C_SDA_PIN, 0);
}

static inline void sda_high(void)
{
    gpio_set_level(SSD1306_I2C_SDA_PIN, 1);
}

static inline void scl_low(void)
{
    gpio_set_level(SSD1306_I2C_SCL_PIN, 0);
}

static inline void scl_high(void)
{
    gpio_set_level(SSD1306_I2C_SCL_PIN, 1);
}

static inline bool sda_read(void)
{
    return gpio_get_level(SSD1306_I2C_SDA_PIN);
}

static void i2c_start(void)
{
    sda_high();
    i2c_delay();
    scl_high();
    i2c_delay();
    sda_low();
    i2c_delay();
    scl_low();
    i2c_delay();
}

static void i2c_stop(void)
{
    sda_low();
    i2c_delay();
    scl_high();
    i2c_delay();
    sda_high();
    i2c_delay();
}

static bool i2c_write_byte(uint8_t data)
{
    for (int i = 0; i < 8; i++) {
        if (data & 0x80) {
            sda_high();
        } else {
            sda_low();
        }
        data <<= 1;
        i2c_delay();
        scl_high();
        i2c_delay();
        scl_low();
        i2c_delay();
    }

    /* Read ACK */
    sda_high();
    gpio_set_direction(SSD1306_I2C_SDA_PIN, GPIO_MODE_INPUT);
    i2c_delay();
    scl_high();
    i2c_delay();
    bool ack = (sda_read() == 0);
    scl_low();
    gpio_set_direction(SSD1306_I2C_SDA_PIN, GPIO_MODE_OUTPUT_OD);
    i2c_delay();

    return ack;
}

static bool ssd1306_write_command_addr(uint8_t addr, uint8_t cmd)
{
    i2c_start();
    bool ack = i2c_write_byte(addr << 1);
    if (!ack) {
        i2c_stop();
        return false;
    }
    i2c_write_byte(SSD1306_CONTROL_CMD);
    i2c_write_byte(cmd);
    i2c_stop();
    return true;
}

static bool ssd1306_write_data_addr(uint8_t addr, const uint8_t *data, size_t len)
{
    i2c_start();
    bool ack = i2c_write_byte(addr << 1);
    if (!ack) {
        i2c_stop();
        return false;
    }
    i2c_write_byte(SSD1306_CONTROL_DATA);
    for (size_t i = 0; i < len; i++) {
        i2c_write_byte(data[i]);
    }
    i2c_stop();
    return true;
}

static bool ssd1306_write_command(uint8_t cmd)
{
    return ssd1306_write_command_addr(s_display_addr, cmd);
}

static bool ssd1306_write_data(const uint8_t *data, size_t len)
{
    return ssd1306_write_data_addr(s_display_addr, data, len);
}

static bool ssd1306_probe_addr(uint8_t addr)
{
    i2c_start();
    bool ack = i2c_write_byte(addr << 1);
    i2c_stop();
    return ack;
}

static void i2c_bus_recovery(void)
{
    ESP_LOGD(TAG, "Performing I2C bus recovery");
    /* Clock out any stuck devices */
    for (int i = 0; i < 9; i++) {
        scl_high();
        i2c_delay();
        scl_low();
        i2c_delay();
    }
    i2c_stop();
}

esp_err_t ssd1306_display_init(void)
{
    ESP_LOGI(TAG, "Initializing SSD1306 on software I2C (SDA=%d, SCL=%d)",
             SSD1306_I2C_SDA_PIN, SSD1306_I2C_SCL_PIN);

    /* Create mutexes */
    s_i2c_mutex = xSemaphoreCreateMutex();
    s_data_mutex = xSemaphoreCreateMutex();
    if (s_i2c_mutex == NULL || s_data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return ESP_FAIL;
    }

    /* Configure GPIO pins as open-drain with pull-ups */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SSD1306_I2C_SDA_PIN) | (1ULL << SSD1306_I2C_SCL_PIN),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set both lines high (idle state) */
    gpio_set_level(SSD1306_I2C_SDA_PIN, 1);
    gpio_set_level(SSD1306_I2C_SCL_PIN, 1);

    /* Wait for display to be ready */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Try bus recovery first */
    i2c_bus_recovery();
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Probe display - try both addresses */
    bool found = false;

    /* Try primary address 0x3C first */
    for (int retry = 0; retry < 3; retry++) {
        if (ssd1306_probe_addr(SSD1306_I2C_ADDR)) {
            s_display_addr = SSD1306_I2C_ADDR;
            found = true;
            ESP_LOGI(TAG, "SSD1306 detected at address 0x%02X", SSD1306_I2C_ADDR);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Try alternate address 0x3D if primary not found */
    if (!found) {
        for (int retry = 0; retry < 3; retry++) {
            if (ssd1306_probe_addr(SSD1306_I2C_ADDR_ALT)) {
                s_display_addr = SSD1306_I2C_ADDR_ALT;
                found = true;
                ESP_LOGI(TAG, "SSD1306 detected at alternate address 0x%02X", SSD1306_I2C_ADDR_ALT);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "SSD1306 display not detected at any address (tried 0x%02X, 0x%02X)",
                 SSD1306_I2C_ADDR, SSD1306_I2C_ADDR_ALT);
        s_display_available = false;
        return ESP_FAIL;
    }

    /* Initialize SSD1306 */
    ssd1306_write_command(SSD1306_CMD_DISPLAY_OFF);
    vTaskDelay(pdMS_TO_TICKS(10));

    ssd1306_write_command(SSD1306_CMD_SET_CLOCK_DIV);
    ssd1306_write_command(0x80);

    ssd1306_write_command(SSD1306_CMD_SET_MULTIPLEX);
    ssd1306_write_command(0x3F);

    ssd1306_write_command(SSD1306_CMD_SET_DISPLAY_OFFSET);
    ssd1306_write_command(0x00);

    ssd1306_write_command(SSD1306_CMD_SET_START_LINE | 0x00);

    ssd1306_write_command(SSD1306_CMD_CHARGE_PUMP);
    ssd1306_write_command(0x14);

    ssd1306_write_command(SSD1306_CMD_SET_MEM_ADDR);
    ssd1306_write_command(0x00);

    ssd1306_write_command(SSD1306_CMD_SET_SEGMENT_REMAP | 0x01);
    ssd1306_write_command(SSD1306_CMD_SET_COM_SCAN_DIR | 0x08);

    ssd1306_write_command(SSD1306_CMD_SET_COM_PINS);
    ssd1306_write_command(0x12);

    ssd1306_write_command(SSD1306_CMD_SET_CONTRAST);
    ssd1306_write_command(0xCF);

    ssd1306_write_command(SSD1306_CMD_SET_PRECHARGE);
    ssd1306_write_command(0xF1);

    ssd1306_write_command(SSD1306_CMD_SET_VCOM_DETECT);
    ssd1306_write_command(0x40);

    ssd1306_write_command(SSD1306_CMD_DISPLAY_ON);

    /* Clear framebuffer */
    memset(s_framebuffer, 0, sizeof(s_framebuffer));

    /* Display initial message */
    draw_string(30, 3, "Ready");
    ssd1306_refresh_display();
    vTaskDelay(pdMS_TO_TICKS(500));
    ssd1306_display_clear();

    ESP_LOGI(TAG, "SSD1306 display initialized successfully at 0x%02X", s_display_addr);
    s_display_available = true;

    /* Create display task */
    xTaskCreate(ssd1306_display_task, "display_task", 4096, NULL, 5, &s_display_task_handle);

    return ESP_OK;
}

bool ssd1306_display_is_available(void)
{
    return s_display_available;
}

/* Simple 5x7 font for basic ASCII characters */
static const uint8_t font_5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},
    {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50},
    {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x08, 0x2A, 0x1C, 0x2A, 0x08},
    {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x00, 0x08, 0x14, 0x22, 0x41},
    {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x41, 0x22, 0x14, 0x08, 0x00},
    {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E},
    {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x01, 0x01},
    {0x3E, 0x41, 0x41, 0x51, 0x32},
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x04, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x03, 0x04, 0x78, 0x04, 0x03},
    {0x61, 0x51, 0x49, 0x45, 0x43},
    {0x00, 0x00, 0x7F, 0x41, 0x41},
    {0x02, 0x04, 0x08, 0x10, 0x20},
    {0x41, 0x41, 0x7F, 0x00, 0x00},
    {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00},
    {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38},
    {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7E, 0x09, 0x01, 0x02},
    {0x08, 0x14, 0x54, 0x54, 0x3C},
    {0x7F, 0x08, 0x04, 0x04, 0x78},
    {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x00, 0x7F, 0x10, 0x28, 0x44},
    {0x00, 0x41, 0x7F, 0x40, 0x00},
    {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78},
    {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C},
    {0x7C, 0x08, 0x04, 0x04, 0x08},
    {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20},
    {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44},
    {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44},
    {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00},
    {0x08, 0x08, 0x2A, 0x1C, 0x08},
};

static void draw_char(int x, int y, char c)
{
    if (x < 0 || x >= SSD1306_WIDTH - 5 || y < 0 || y >= SSD1306_PAGES) {
        return;
    }

    if (c < 32 || c > 126) {
        c = '?';
    }

    const uint8_t *char_data = font_5x7[c - 32];
    for (int i = 0; i < 5; i++) {
        if (x + i < SSD1306_WIDTH) {
            s_framebuffer[y][x + i] = char_data[i];
        }
    }
}

static void draw_string(int x, int y, const char *str)
{
    while (*str && x < SSD1306_WIDTH - 6) {
        draw_char(x, y, *str);
        x += 6;
        str++;
    }
}

static void draw_hline(int x, int y, int width)
{
    if (y < 0 || y >= SSD1306_PAGES * 8) return;
    int page = y / 8;
    int bit = y % 8;
    uint8_t mask = 1 << bit;

    for (int i = 0; i < width && x + i < SSD1306_WIDTH; i++) {
        if (x + i >= 0) {
            s_framebuffer[page][x + i] |= mask;
        }
    }
}

static void ssd1306_refresh_display(void)
{
    if (!s_display_available) return;

    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    ssd1306_write_command(SSD1306_CMD_SET_COL_ADDR);
    ssd1306_write_command(0);
    ssd1306_write_command(SSD1306_WIDTH - 1);

    ssd1306_write_command(SSD1306_CMD_SET_PAGE_ADDR);
    ssd1306_write_command(0);
    ssd1306_write_command(SSD1306_PAGES - 1);

    for (int page = 0; page < SSD1306_PAGES; page++) {
        ssd1306_write_data(s_framebuffer[page], SSD1306_WIDTH);
    }

    xSemaphoreGive(s_i2c_mutex);
}

static void update_display_content(void)
{
    uint16_t co2_ppm = 0;
    float pm2_5 = 0.0f / 0.0f;
    bool co2_valid = false;
    bool pm25_valid = false;

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        co2_ppm = s_last_co2_ppm;
        pm2_5 = s_last_pm2_5;
        xSemaphoreGive(s_data_mutex);
    }

    co2_valid = (co2_ppm > 0 && co2_ppm < 5000);
    pm25_valid = (!isnan(pm2_5) && pm2_5 >= 0.0f && pm2_5 < 1000.0f);

    memset(s_framebuffer, 0, sizeof(s_framebuffer));

    draw_string(20, 0, "Air Quality");
    draw_hline(0, 10, 128);

    char line[20];
    if (co2_valid) {
        snprintf(line, sizeof(line), "CO2: %4u ppm", co2_ppm);
    } else {
        snprintf(line, sizeof(line), "CO2:  --- ppm");
    }
    draw_string(0, 2, line);

    if (pm25_valid) {
        snprintf(line, sizeof(line), "PM2.5: %5.1f", pm2_5);
    } else {
        snprintf(line, sizeof(line), "PM2.5:   ---");
    }
    draw_string(0, 4, line);
    draw_string(0, 5, "ug/m3");

    draw_hline(0, 54, 128);
    draw_string(8, 7, "Zigbee Sensor");

    ssd1306_refresh_display();
}

static void ssd1306_display_task(void *pvParameter)
{
    (void)pvParameter;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ssd1306_display_update(uint16_t co2_ppm, float pm2_5)
{
    if (!s_display_available) {
        return;
    }

    if (xSemaphoreTake(s_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_last_co2_ppm = co2_ppm;
        s_last_pm2_5 = pm2_5;
        xSemaphoreGive(s_data_mutex);
    }

    ssd1306_display_on();
    update_display_content();

    ESP_LOGI(TAG, "Display updated - CO2:%u ppm, PM2.5:%.1f ug/m3", co2_ppm, pm2_5);

    static TimerHandle_t s_off_timer = NULL;
    if (s_off_timer == NULL) {
        s_off_timer = xTimerCreate("disp_off", pdMS_TO_TICKS(SSD1306_DISPLAY_TIME_MS),
                                   pdFALSE, NULL,
                                   (TimerCallbackFunction_t)ssd1306_display_off);
    }
    if (s_off_timer != NULL) {
        xTimerStop(s_off_timer, 0);
        xTimerStart(s_off_timer, 0);
    }
}

void ssd1306_display_off(void)
{
    if (!s_display_available) {
        return;
    }

    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_framebuffer, 0, sizeof(s_framebuffer));
        draw_string(30, 3, "Standby");
        ssd1306_refresh_display();
        xSemaphoreGive(s_i2c_mutex);

        vTaskDelay(pdMS_TO_TICKS(500));

        if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memset(s_framebuffer, 0, sizeof(s_framebuffer));
            ssd1306_refresh_display();
            ssd1306_write_command(SSD1306_CMD_DISPLAY_OFF);
            xSemaphoreGive(s_i2c_mutex);
        }
    }

    ESP_LOGD(TAG, "Display turned off");
}

void ssd1306_display_on(void)
{
    if (!s_display_available) {
        return;
    }

    if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ssd1306_write_command(SSD1306_CMD_DISPLAY_ON);
        xSemaphoreGive(s_i2c_mutex);
    }

    ESP_LOGD(TAG, "Display turned on");
}

void ssd1306_display_clear(void)
{
    if (!s_display_available) {
        return;
    }

    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    ssd1306_refresh_display();
}
