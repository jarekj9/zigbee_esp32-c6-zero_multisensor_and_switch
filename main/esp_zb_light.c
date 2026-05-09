/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier:  LicenseRef-Included
 *
 * Zigbee HA_on_off_light Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "esp_zb_light.h"
#include "mhz19b_driver.h"
#include "sps30_driver.h"
#include "ssd1306_display.h"
#include "zcl/esp_zigbee_zcl_pm2_5_measurement.h"

#if !defined ZB_ED_ROLE
#error Define ZB_ED_ROLE in idf.py menuconfig to compile light (End Device) source code.
#endif

#define BUTTON_GPIO GPIO_NUM_2
#define BUZZER_GPIO GPIO_NUM_20
#define BUZZER_CHANNEL LEDC_CHANNEL_1
#define BUZZER_TIMER LEDC_TIMER_1
#define BUTTON_DEBOUNCE_MS 500
#define REJOIN_COOLDOWN_MS 5000

// Note frequencies in Hz
#define NOTE_E5  659
#define NOTE_C5  523
#define NOTE_G5  784
#define NOTE_G4  392
#define NOTE_E4  330
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_A4S 466
#define NOTE_D5  587
#define NOTE_F5  698

static const char *TAG = "ESP_ZB_ON_OFF_LIGHT";
static volatile uint32_t last_interrupt_time = 0;
static TaskHandle_t buzzer_task_handle = NULL;
static TaskHandle_t co2_sensor_task_handle = NULL;
static TaskHandle_t pm25_sensor_task_handle = NULL;
static esp_zb_zcl_reporting_info_t co2_reporting_info;
static esp_zb_zcl_reporting_info_t pm25_reporting_info;

/* Static variables for CO2 cluster attributes (must persist for Zigbee stack) */
/* ZCL Carbon Dioxide Measurement uses single precision float (0.0 - 1.0 range) */
/* Value = ppm / 1000000.0 (e.g., 1007 ppm = 0.001007) */
static float s_co2_measured_value = 0.0f / 0.0f;  // NaN = invalid/unavailable
static float s_co2_min_value = 0.0f;              // 0 ppm = 0.0
static float s_co2_max_value = 0.5f;              // 500000 ppm = 0.5 (max reasonable)

    /* Static variables for PM2.5 cluster attributes (must persist for Zigbee stack) */
/* PM2.5 value in ug/m3 (Home Assistant expects raw value, not ZCL normalized) */
static float s_pm25_measured_value = 0.0f / 0.0f; // NaN = invalid/unavailable
static float s_pm25_min_value = 0.0f;             // 0 ug/m3
static float s_pm25_max_value = 500.0f;           // 500 ug/m3 max
static float s_pm25_tolerance = 10.0f;            // 10 ug/m3 tolerance

/* Flag to indicate Zigbee is ready for attribute updates */
static volatile bool s_zigbee_ready = false;

/* Latest sensor values for display */
static uint16_t s_latest_co2_ppm = 0;
static float s_latest_pm2_5 = 0.0f / 0.0f;  /* NaN = invalid */
static SemaphoreHandle_t s_sensor_data_mutex = NULL;

// Declarations:
void ssd1306_display_set_always_on(bool enable);


/********************* LED Functions **************************/
static void blink_led(int times, uint32_t on_time_ms, uint32_t off_time_ms)
{
    for (int i = 0; i < times; i++) {
        light_driver_set_power(1);
        vTaskDelay(pdMS_TO_TICKS(on_time_ms));
        light_driver_set_power(0);
        vTaskDelay(pdMS_TO_TICKS(off_time_ms));
    }
}
/********************* Buzzer Functions **************************/
static void buzzer_tone(uint32_t frequency, uint32_t duration_ms)
{
    ledc_set_freq(LEDC_LOW_SPEED_MODE, BUZZER_TIMER, frequency);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_CHANNEL);
}

static void buzzer_task(void *pvParameter)
{
    while(1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // Playful video game style melody
        buzzer_tone(NOTE_E5, 120);
        vTaskDelay(pdMS_TO_TICKS(30));
        buzzer_tone(NOTE_E5, 120);
        vTaskDelay(pdMS_TO_TICKS(150));
        buzzer_tone(NOTE_E5, 120);
        vTaskDelay(pdMS_TO_TICKS(150));
        
        buzzer_tone(NOTE_C5, 120);
        vTaskDelay(pdMS_TO_TICKS(30));
        buzzer_tone(NOTE_E5, 120);
        vTaskDelay(pdMS_TO_TICKS(150));
        buzzer_tone(NOTE_G5, 120);
        vTaskDelay(pdMS_TO_TICKS(350));
        
        buzzer_tone(NOTE_G4, 120);
        vTaskDelay(pdMS_TO_TICKS(350));
    }
}

static void buzzer_play_melody_async(void)
{
    if(buzzer_task_handle != NULL) {
        xTaskNotify(buzzer_task_handle, 0, eNoAction);
    }
}

static void buzzer_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BUZZER_TIMER,
        .freq_hz = NOTE_C5,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);
    
    ledc_channel_config_t channel_conf = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&channel_conf);
}

/********************* Button Functions **************************/
static void IRAM_ATTR button_isr_handler(void* arg)
{
    uint32_t now = xTaskGetTickCountFromISR();
    
    if ((now - last_interrupt_time) > pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
        last_interrupt_time = now;
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xTaskNotifyFromISR((TaskHandle_t)arg, 0, eNoAction, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static void button_task(void *pvParameter)
{
    static TickType_t last_rejoin_time = 0;
    static bool s_display_always_on = false;

    while(1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        TickType_t press_start = xTaskGetTickCount();
        vTaskDelay(pdMS_TO_TICKS(100));

        while(gpio_get_level(BUTTON_GPIO) == 1) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        TickType_t press_duration = xTaskGetTickCount() - press_start;
        uint32_t press_ms = pdTICKS_TO_MS(press_duration);

        if(press_ms >= 5000) {
            ESP_LOGW(TAG, "Factory reset triggered");
            blink_led(5, 100, 100);
            esp_zb_factory_reset();

        } else if(press_ms >= 500) {
            TickType_t current_time = xTaskGetTickCount();

            if ((current_time - last_rejoin_time) > pdMS_TO_TICKS(REJOIN_COOLDOWN_MS)) {
                ESP_LOGI(TAG, "Button pressed - triggering rejoin");

                esp_zb_lock_acquire(portMAX_DELAY);
                esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                esp_zb_lock_release();

                if (err == ESP_OK) {
                    last_rejoin_time = current_time;
                    ESP_LOGI(TAG, "Rejoin initiated successfully");
                    blink_led(2, 100, 100);
                } else {
                    ESP_LOGW(TAG, "Rejoin failed: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGI(TAG, "Button ignored - cooldown active");
            }

        } else {
            // Short press: toggle display always-on
            s_display_always_on = !s_display_always_on;
            ESP_LOGI(TAG, "Display always-on: %s", s_display_always_on ? "ON" : "OFF");

            if (s_display_always_on) {
                ssd1306_display_set_always_on(true);
                blink_led(1, 200, 0);
            } else {
                ssd1306_display_set_always_on(false);
                blink_led(2, 100, 100);
            }
        }
    }
}

/********************* Zigbee Functions **************************/
static esp_err_t deferred_driver_init(void)
{
    light_driver_init(LIGHT_DEFAULT_OFF);
    return ESP_OK;
}

/********************* CO2 Sensor Functions **************************/

/* Helper function to send explicit attribute report to coordinator */
static void co2_send_report(void)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,  /* Coordinator */
            .dst_endpoint = 1,                /* Coordinator endpoint */
            .src_endpoint = HA_ESP_CO2_SENSOR_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
        .manuf_specific = 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
        .attributeID = ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID,
    };

    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send CO2 report: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "CO2 report sent successfully");
    }
}

static void co2_sensor_task(void *pvParameter)
{
    mhz19b_reading_t reading;
    TickType_t last_wake_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "CO2 sensor task started, reporting every %d seconds", CO2_REPORTING_INTERVAL_SEC);

    while (1) {
        // Only read sensor if Zigbee is ready
        if (!s_zigbee_ready) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            last_wake_time = xTaskGetTickCount();  // Reset after delay
            continue;
        }

        esp_err_t err = mhz19b_read_co2(&reading);

        if (err == ESP_OK && reading.valid) {
            ESP_LOGI(TAG, "CO2 reading: %u ppm", reading.co2_ppm);

            // Store latest value for display
            if (xSemaphoreTake(s_sensor_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_latest_co2_ppm = reading.co2_ppm;
                xSemaphoreGive(s_sensor_data_mutex);
            }

            // Update the static variable and report via Zigbee
            if (esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
                /* Convert ppm to ZCL float value (0.0 - 1.0 range) */
                /* ZCL Carbon Dioxide: value = ppm / 1000000.0 */
                s_co2_measured_value = (float)reading.co2_ppm / 1000000.0f;

                esp_zb_zcl_set_attribute_val(HA_ESP_CO2_SENSOR_ENDPOINT,
                                              ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
                                              ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                              ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID,
                                              &s_co2_measured_value,
                                              false);

                /* Send explicit report to coordinator */
                co2_send_report();

                esp_zb_lock_release();
            } else {
                ESP_LOGW(TAG, "Failed to acquire Zigbee lock for CO2 reporting");
            }

            // Update display with latest values
            float pm2_5_display = 0.0f / 0.0f;
            if (xSemaphoreTake(s_sensor_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                pm2_5_display = s_latest_pm2_5;
                xSemaphoreGive(s_sensor_data_mutex);
            }
            ssd1306_display_update(reading.co2_ppm, pm2_5_display);
        } else {
            ESP_LOGW(TAG, "CO2 sensor read failed: %s", mhz19b_err_to_str(reading.error));

            // Report invalid value (unavailable - NaN)
            if (esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
                s_co2_measured_value = 0.0f / 0.0f;  /* NaN = invalid */

                esp_zb_zcl_set_attribute_val(HA_ESP_CO2_SENSOR_ENDPOINT,
                                              ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
                                              ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                              ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID,
                                              &s_co2_measured_value,
                                              false);

                /* Send explicit report to coordinator */
                co2_send_report();

                esp_zb_lock_release();
            }
        }

        // Wait for next interval
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(CO2_REPORTING_INTERVAL_SEC * 1000));
    }
}

/********************* PM2.5 Sensor Functions **************************/

/* Helper function to send explicit attribute report for PM2.5 to coordinator */
static void pm25_send_report(float raw_value_ug_m3)
{
    // Report PM2.5 Measurement cluster (raw ug/m3 value)
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,  /* Coordinator */
            .dst_endpoint = 1,                /* Coordinator endpoint */
            .src_endpoint = HA_ESP_PM25_SENSOR_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
        .manuf_specific = 0,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
        .attributeID = ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID,
    };

    ESP_LOGI(TAG, "Sending PM2.5 report to coordinator (value=%.2f ug/m3)", raw_value_ug_m3);
    
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send PM2.5 report: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PM2.5 report sent successfully");
    }
}

static void pm25_sensor_task(void *pvParameter)
{
    sps30_reading_t reading;
    TickType_t last_wake_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "PM2.5 sensor task started, reporting every %d seconds", PM25_REPORTING_INTERVAL_SEC);

    while (1) {
        // Only read sensor if Zigbee is ready and sensor is available
        if (!s_zigbee_ready) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            last_wake_time = xTaskGetTickCount();
            continue;
        }

        // Check if sensor is available
        if (!sps30_is_available()) {
            ESP_LOGD(TAG, "SPS30 not available, skipping PM2.5 reading");
            vTaskDelay(pdMS_TO_TICKS(PM25_REPORTING_INTERVAL_SEC * 1000));
            last_wake_time = xTaskGetTickCount();
            continue;
        }

        // Start measurement (fan starts spinning)
        ESP_LOGI(TAG, "Starting SPS30 measurement...");
        esp_err_t err = sps30_start_measurement();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start SPS30 measurement: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(PM25_REPORTING_INTERVAL_SEC * 1000));
            last_wake_time = xTaskGetTickCount();
            continue;
        }

        // Wait for fan to stabilize
        ESP_LOGI(TAG, "Waiting for SPS30 fan to stabilize...");
        vTaskDelay(pdMS_TO_TICKS(15000));

        // Collect multiple samples and average them
        const int NUM_SAMPLES = 5;
        float samples[NUM_SAMPLES];
        int valid_count = 0;

        for (int i = 0; i < NUM_SAMPLES; i++) {
            err = sps30_read_measurement(&reading);
            if (err == ESP_OK && reading.valid) {
                samples[valid_count++] = reading.pm2_5;
                ESP_LOGD(TAG, "PM2.5 sample %d: %.2f ug/m3", i + 1, reading.pm2_5);
            }
            if (i < NUM_SAMPLES - 1) {
                vTaskDelay(pdMS_TO_TICKS(1000)); // SPS30 outputs at ~1Hz
            }
        }

        // Calculate averaged value with outlier rejection (drop min and max if >= 3 valid)
        float pm25_avg = 0.0f / 0.0f; // NaN = invalid
        if (valid_count >= 3) {
            float min_val = samples[0], max_val = samples[0], sum = 0.0f;
            for (int i = 0; i < valid_count; i++) {
                if (samples[i] < min_val) min_val = samples[i];
                if (samples[i] > max_val) max_val = samples[i];
                sum += samples[i];
            }
            // Drop min and max, average the rest
            sum -= min_val + max_val;
            pm25_avg = sum / (valid_count - 2);
            ESP_LOGI(TAG, "PM2.5 averaged: %.2f ug/m3 (from %d samples, dropped min=%.2f max=%.2f)",
                     pm25_avg, valid_count, min_val, max_val);
        } else if (valid_count > 0) {
            // Not enough for outlier rejection, plain average
            float sum = 0.0f;
            for (int i = 0; i < valid_count; i++) sum += samples[i];
            pm25_avg = sum / valid_count;
            ESP_LOGI(TAG, "PM2.5 averaged: %.2f ug/m3 (from %d samples, no outlier rejection)",
                     pm25_avg, valid_count);
        }

        if (valid_count > 0) {
            // Store latest value for display
            if (xSemaphoreTake(s_sensor_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_latest_pm2_5 = pm25_avg;
                xSemaphoreGive(s_sensor_data_mutex);
            }

            // Update the static variable and report via Zigbee
            if (esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
                s_pm25_measured_value = pm25_avg;

                ESP_LOGI(TAG, "Setting PM2.5 attribute to %.2f ug/m3 (endpoint %d)",
                         s_pm25_measured_value, HA_ESP_PM25_SENSOR_ENDPOINT);

                esp_err_t set_err = esp_zb_zcl_set_attribute_val(HA_ESP_PM25_SENSOR_ENDPOINT,
                                              ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
                                              ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                              ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID,
                                              &s_pm25_measured_value,
                                              false);

                if (set_err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to set PM2.5 attribute: %s", esp_err_to_name(set_err));
                }

                pm25_send_report(pm25_avg);
                esp_zb_lock_release();
            } else {
                ESP_LOGW(TAG, "Failed to acquire Zigbee lock for PM2.5 reporting");
            }

            // Update display with latest values
            uint16_t co2_ppm_display = 0;
            if (xSemaphoreTake(s_sensor_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                co2_ppm_display = s_latest_co2_ppm;
                xSemaphoreGive(s_sensor_data_mutex);
            }
            // ssd1306_display_update(co2_ppm_display, pm25_avg); // another screen flash after fan stops
        } else {
            ESP_LOGW(TAG, "PM2.5 sensor read failed: %s", sps30_err_to_str(reading.error));

            // Report invalid value (unavailable - NaN)
            if (esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
                s_pm25_measured_value = 0.0f / 0.0f;

                esp_zb_zcl_set_attribute_val(HA_ESP_PM25_SENSOR_ENDPOINT,
                                              ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
                                              ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                              ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID,
                                              &s_pm25_measured_value,
                                              false);

                pm25_send_report(0.0f / 0.0f);
                esp_zb_lock_release();
            }
        }

        // Stop measurement to turn off fan
        sps30_stop_measurement();
        ESP_LOGI(TAG, "SPS30 measurement stopped, fan off");

        // Wait for next interval
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(PM25_REPORTING_INTERVAL_SEC * 1000));
    }
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG, "Failed to start Zigbee commissioning");
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in %s factory-reset mode", esp_zb_bdb_is_factory_new() ? "" : "non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Device rebooted");
                /* Device is already on network, signal that Zigbee is ready */
                s_zigbee_ready = true;
            }
        } else {
            ESP_LOGW(TAG, "Failed to initialize Zigbee stack (status: %s)", esp_err_to_name(err_status));
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t extended_pan_id;
            esp_zb_get_extended_pan_id(extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully (Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x, PAN ID: 0x%04hx, Channel:%d, Short Address: 0x%04hx)",
                     extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
                     extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
                     esp_zb_get_pan_id(), esp_zb_get_current_channel(), esp_zb_get_short_address());
            /* Signal that Zigbee is ready for attribute updates */
            s_zigbee_ready = true;
        } else {
            ESP_LOGI(TAG, "Network steering was not successful (status: %s)", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb, ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s", esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    esp_err_t ret = ESP_OK;
    bool light_state = 0;

    ESP_RETURN_ON_FALSE(message, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(message->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG, "Received message: error status(%d)", message->info.status);
    
    ESP_LOGI(TAG, "Received message: endpoint(%d), cluster(0x%x), attribute(0x%x), data size(%d)", 
             message->info.dst_endpoint, message->info.cluster, message->attribute.id, message->attribute.data.size);
    
    if (message->info.dst_endpoint == HA_ESP_LIGHT_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID && message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                light_state = message->attribute.data.value ? *(bool *)message->attribute.data.value : light_state;
                ESP_LOGI(TAG, "Light sets to %s", light_state ? "On" : "Off");
                //light_driver_set_power(light_state);
                
                // Play melody when triggered
                buzzer_play_melody_async();
            }
        }
    }
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    esp_err_t ret = ESP_OK;
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        ret = zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
        break;
    default:
        ESP_LOGW(TAG, "Receive Zigbee action(0x%x) callback", callback_id);
        break;
    }
    return ret;
}

static void esp_zb_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&zb_nwk_cfg);

    /* === Endpoint 10: On/Off Light (Buzzer control) - keep original === */
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(HA_ESP_LIGHT_ENDPOINT, &light_cfg);

    /* === Endpoint 11: Carbon Dioxide Sensor === */
    /* Create cluster list for CO2 sensor */
    esp_zb_cluster_list_t *co2_cluster_list = esp_zb_zcl_cluster_list_create();

    /* Create and add basic cluster */
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_cluster_list_add_basic_cluster(co2_cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Create and add identify cluster */
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = 0,
    };
    esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(&identify_cfg);
    esp_zb_cluster_list_add_identify_cluster(co2_cluster_list, identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Create Carbon Dioxide Measurement cluster manually */
    esp_zb_attribute_list_t *co2_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT);

    /* Add MeasuredValue attribute (0x0000) - single precision float */
    /* MUST use static/global variables - Zigbee stores pointers! */
    esp_zb_cluster_add_attr(co2_cluster, ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
                            ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID,
                            ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ESP_ZB_ZCL_ATTR_ACCESS_REPORTING | ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                            &s_co2_measured_value);

    /* Add MinMeasuredValue attribute (0x0001) */
    esp_zb_cluster_add_attr(co2_cluster, ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
                            ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MIN_MEASURED_VALUE_ID,
                            ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                            &s_co2_min_value);

    /* Add MaxMeasuredValue attribute (0x0002) */
    esp_zb_cluster_add_attr(co2_cluster, ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT,
                            ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MAX_MEASURED_VALUE_ID,
                            ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                            &s_co2_max_value);

    /* Add CO2 measurement cluster to cluster list */
    esp_zb_cluster_list_add_carbon_dioxide_measurement_cluster(co2_cluster_list, co2_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Add CO2 sensor endpoint to list */
    esp_zb_endpoint_config_t co2_ep_config = {
        .endpoint = HA_ESP_CO2_SENSOR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, co2_cluster_list, co2_ep_config);

    /* === Endpoint 12: PM2.5 Sensor === */
    /* Create cluster list for PM2.5 sensor */
    esp_zb_cluster_list_t *pm25_cluster_list = esp_zb_zcl_cluster_list_create();

    /* Create and add basic cluster for PM2.5 */
    esp_zb_attribute_list_t *pm25_basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_cluster_list_add_basic_cluster(pm25_cluster_list, pm25_basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Create and add identify cluster for PM2.5 */
    esp_zb_attribute_list_t *pm25_identify_cluster = esp_zb_identify_cluster_create(&identify_cfg);
    esp_zb_cluster_list_add_identify_cluster(pm25_cluster_list, pm25_identify_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Create PM2.5 Measurement cluster manually */
    esp_zb_attribute_list_t *pm25_cluster = esp_zb_zcl_attr_list_create(ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT);

    /* Add MeasuredValue attribute (0x0000) - single precision float */
    /* MUST use static/global variables - Zigbee stores pointers! */
    esp_zb_cluster_add_attr(pm25_cluster, ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
                            ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID,
                            ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ESP_ZB_ZCL_ATTR_ACCESS_REPORTING | ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                            &s_pm25_measured_value);

    /* Add MinMeasuredValue attribute (0x0001) */
    esp_zb_cluster_add_attr(pm25_cluster, ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
                            ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MIN_MEASURED_VALUE_ID,
                            ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                            &s_pm25_min_value);

    /* Add MaxMeasuredValue attribute (0x0002) */
    esp_zb_cluster_add_attr(pm25_cluster, ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
                            ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MAX_MEASURED_VALUE_ID,
                            ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                            &s_pm25_max_value);

    /* Add Tolerance attribute (0x0003) */
    esp_zb_cluster_add_attr(pm25_cluster, ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT,
                            ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_TOLERANCE_ID,
                            ESP_ZB_ZCL_ATTR_TYPE_SINGLE, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
                            &s_pm25_tolerance);

    /* Add PM2.5 measurement cluster to cluster list */
    esp_zb_cluster_list_add_pm2_5_measurement_cluster(pm25_cluster_list, pm25_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Create Analog Input cluster for PM2.5 (backup for compatibility) */
    esp_zb_analog_input_cluster_cfg_t analog_input_cfg = {
        .out_of_service = false,
        .present_value = 0.0f,
        .status_flags = 0,
    };
    esp_zb_attribute_list_t *analog_input_cluster = esp_zb_analog_input_cluster_create(&analog_input_cfg);
    esp_zb_cluster_list_add_analog_input_cluster(pm25_cluster_list, analog_input_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    /* Add PM2.5 sensor endpoint to list */
    esp_zb_endpoint_config_t pm25_ep_config = {
        .endpoint = HA_ESP_PM25_SENSOR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, pm25_cluster_list, pm25_ep_config);

    /* Add manufacturer info to all endpoints */
    zcl_basic_manufacturer_info_t info = {
        .manufacturer_name = ESP_MANUFACTURER_NAME,
        .model_identifier = ESP_MODEL_IDENTIFIER,
    };
    esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, HA_ESP_LIGHT_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, HA_ESP_CO2_SENSOR_ENDPOINT, &info);
    esp_zcl_utility_add_ep_basic_manufacturer_info(ep_list, HA_ESP_PM25_SENSOR_ENDPOINT, &info);

    /* Register device */
    esp_zb_device_register(ep_list);

    /* Register action handler */
    esp_zb_core_action_handler_register(zb_action_handler);

    /* Configure reporting for CO2 sensor */
    co2_reporting_info.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    co2_reporting_info.ep = HA_ESP_CO2_SENSOR_ENDPOINT;
    co2_reporting_info.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_CARBON_DIOXIDE_MEASUREMENT;
    co2_reporting_info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    co2_reporting_info.attr_id = ESP_ZB_ZCL_ATTR_CARBON_DIOXIDE_MEASUREMENT_MEASURED_VALUE_ID;
    co2_reporting_info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    co2_reporting_info.dst.short_addr = 0x0000; /* Coordinator */
    co2_reporting_info.dst.endpoint = 1;        /* Typical coordinator endpoint */
    co2_reporting_info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    co2_reporting_info.u.send_info.min_interval = 30;   /* Minimum 30 seconds between reports */
    co2_reporting_info.u.send_info.max_interval = 120;  /* Maximum 120 seconds between reports */
    co2_reporting_info.u.send_info.delta.f32 = 0.00005f; /* Report if CO2 changes by 50 ppm (50/1000000) */
    co2_reporting_info.u.send_info.def_min_interval = 30;
    co2_reporting_info.u.send_info.def_max_interval = 120;

    esp_zb_zcl_update_reporting_info(&co2_reporting_info);

    /* Configure reporting for PM2.5 sensor */
    pm25_reporting_info.direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND;
    pm25_reporting_info.ep = HA_ESP_PM25_SENSOR_ENDPOINT;
    pm25_reporting_info.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_PM2_5_MEASUREMENT;
    pm25_reporting_info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    pm25_reporting_info.attr_id = ESP_ZB_ZCL_ATTR_PM2_5_MEASUREMENT_MEASURED_VALUE_ID;
    pm25_reporting_info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    pm25_reporting_info.dst.short_addr = 0x0000; /* Coordinator */
    pm25_reporting_info.dst.endpoint = 1;        /* Typical coordinator endpoint */
    pm25_reporting_info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    pm25_reporting_info.u.send_info.min_interval = 30;   /* Minimum 30 seconds between reports */
    pm25_reporting_info.u.send_info.max_interval = 120;  /* Maximum 120 seconds between reports */
    pm25_reporting_info.u.send_info.delta.f32 = 0.5f;    /* Report if PM2.5 changes by 0.5 ug/m3 */
    pm25_reporting_info.u.send_info.def_min_interval = 30;
    pm25_reporting_info.u.send_info.def_max_interval = 120;

    esp_zb_zcl_update_reporting_info(&pm25_reporting_info);

    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));

    // Create mutex for sensor data sharing
    s_sensor_data_mutex = xSemaphoreCreateMutex();

    // Display setup (non-critical - continues if display not connected)
    esp_err_t display_err = ssd1306_display_init();
    if (display_err != ESP_OK) {
        ESP_LOGW(TAG, "Display initialization failed, continuing without display");
    }

    // Button setup
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE
    };
    gpio_config(&io_conf);
    
    // Buzzer setup
    buzzer_init();
    xTaskCreate(buzzer_task, "buzzer_task", 2048, NULL, 5, &buzzer_task_handle);

    // Button task
    TaskHandle_t button_task_handle = NULL;
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, &button_task_handle);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, (void*)button_task_handle);

    // CO2 Sensor setup
    ESP_ERROR_CHECK(mhz19b_init());
    xTaskCreate(co2_sensor_task, "co2_sensor_task", 4096, NULL, 5, &co2_sensor_task_handle);

    // PM2.5 Sensor setup (SPS30) - non-critical, won't fail if sensor not connected
    esp_err_t sps30_err = sps30_init();
    if (sps30_err == ESP_OK) {
        xTaskCreate(pm25_sensor_task, "pm25_sensor_task", 4096, NULL, 5, &pm25_sensor_task_handle);
    } else {
        ESP_LOGW(TAG, "SPS30 initialization failed, continuing without PM2.5 sensor");
    }

    xTaskCreate(esp_zb_task, "Zigbee_main", 8192, NULL, 5, NULL);
}
