/*
 * GMS INTERNAL - REAL HARDWARE
 * ESP32-S3 / ESP-IDF v6.0.2
 *
 * Hardware:
 * ADS1115:
 *   A0 = Temperature
 *   A1 = Not Used
 *   A2 = Level
 *   A3 = Pressure
 *
 * I2C:
 *   SDA = GPIO8
 *   SCL = GPIO9
 *   Address = 0x48
 *
 * EWIG RS485:
 *   TX  = GPIO15
 *   RX  = GPIO16
 *   RTS = GPIO14
 *   Baudrate = 9600
 *   Slave ID = 1
 *
 * Features:
 * - Local Web Dashboard
 * - WiFi AP + Station
 * - DHCP / Static IP
 * - NVS Configuration
 * - MQTT TLS ThingsBoard
 * - ADS1115 Monitoring
 * - EWIG Modbus RTU
 *
 * BENCH TEST ONLY
 * NO RELAY CONTROL
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <limits.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "driver/i2c_master.h"
#include "driver/uart.h"

#include "nvs_flash.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"

#include "lwip/ip4_addr.h"

#include "gms.h"
#include "gms_ota.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"

/* =====================================================
   HARDWARE CONFIGURATION
   ===================================================== */

#define I2C_PORT             I2C_NUM_0
#define I2C_SDA_PIN          8
#define I2C_SCL_PIN          9
#define ADS1115_ADDR         0x48

#define SHUNT_OHMS           56.0f
#define ADC_AVERAGE          5

#define MODBUS_UART          UART_NUM_1
#define MODBUS_TX_PIN        15
#define MODBUS_RX_PIN        16
#define MODBUS_RTS_PIN       14
#define MODBUS_BAUD          9600

#define EWIG_SLAVE_ID        1
#define EWIG_FUNCTION        0x03
#define EWIG_REG_START       9
#define EWIG_REG_COUNT       12

/* =====================================================
   MQTT CONFIGURATION
   ===================================================== */

#define GMS_DEVICE_NAME          "GMS-001"

#define GMS_TOPIC_TELEMETRY \
    "v1/devices/me/telemetry"

#define GMS_TOPIC_ATTRIBUTES \
    "v1/devices/me/attributes"

#define GMS_TOPIC_RPC_REQUEST \
    "v1/devices/me/rpc/request/+"

/* =====================================================
   GLOBAL VARIABLES
   ===================================================== */

static const char *TAG = "GMS_MQTT";

static EventGroupHandle_t s_wifi_events;

#define WIFI_GOT_IP_BIT BIT0

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

static volatile bool s_mqtt_connected = false;

static bool s_mqtt_started = false;
static esp_mqtt_client_handle_t s_mqtt2_client = NULL;
static volatile bool s_mqtt2_connected = false;
static bool s_mqtt2_started = false;
static bool s_ntp_ready = false;
static char s_mqtt2_uri[150];
static bool s_sntp_started = false;

static i2c_master_dev_handle_t s_ads;

static bool s_adc_ready = false;
static bool s_rs485_ready = false;

static esp_netif_t *s_sta_netif;

static portMUX_TYPE s_data_lock =
    portMUX_INITIALIZER_UNLOCKED;

static gms_measurement_t s_last;

/* SENSOR BENCH TEST ONLY: one selected channel with virtual mA.
   No physical 4-20mA output, no relay or actuator interaction.
   Volatile: OFF after reset, auto-expires after 120 seconds. */
#define SENSOR_TEST_DURATION_US (120LL * 1000000LL)
static portMUX_TYPE s_sensor_test_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_sensor_test_end_us = 0;
static int s_sensor_test_channel = -1;
static float s_sensor_test_ma = 0.0f;

bool gms_sensor_test_start(int channel, float ma)
{
    if (channel < 0 || channel >= GMS_SENSOR_COUNT ||
        !isfinite(ma) || ma < 0.0f || ma > 24.0f) return false;
    int64_t end_us = esp_timer_get_time() + SENSOR_TEST_DURATION_US;
    portENTER_CRITICAL(&s_sensor_test_lock);
    s_sensor_test_channel = channel;
    s_sensor_test_ma = ma;
    s_sensor_test_end_us = end_us;
    portEXIT_CRITICAL(&s_sensor_test_lock);
    ESP_LOGW(TAG, "BENCH SENSOR TEST START channel=%d virtual=%.3f mA (120s)",
             channel, (double)ma);
    return true;
}

void gms_sensor_test_stop(void)
{
    portENTER_CRITICAL(&s_sensor_test_lock);
    s_sensor_test_end_us = 0;
    s_sensor_test_channel = -1;
    s_sensor_test_ma = 0.0f;
    portEXIT_CRITICAL(&s_sensor_test_lock);
    ESP_LOGW(TAG, "BENCH SENSOR TEST STOP; hardware ADC values restored");
}

bool gms_sensor_test_snapshot(int *channel, float *ma, uint32_t *remaining_s)
{
    const int64_t now = esp_timer_get_time();
    int64_t end;
    int ch;
    float val;
    portENTER_CRITICAL(&s_sensor_test_lock);
    end = s_sensor_test_end_us;
    if (end > 0 && now >= end) {
        s_sensor_test_end_us = 0;
        s_sensor_test_channel = -1;
        end = 0;
    }
    ch = s_sensor_test_channel;
    val = s_sensor_test_ma;
    portEXIT_CRITICAL(&s_sensor_test_lock);
    const bool active = end > now && ch >= 0 && ch < GMS_SENSOR_COUNT;
    if (channel) *channel = active ? ch : -1;
    if (ma) *ma = active ? val : 0.0f;
    if (remaining_s) *remaining_s = active
        ? (uint32_t)((end - now + 999999LL) / 1000000LL) : 0u;
    return active;
}

static const char *sensor_test_channel_name(int channel)
{
    switch (channel) {
        case GMS_SENSOR_TEMP: return "temperature_A0";
        case GMS_SENSOR_LEVEL: return "level_A2";
        case GMS_SENSOR_PRESSURE: return "pressure_A3";
        default: return "none";
    }
}

static char s_ap_name[40];
static char s_mqtt_uri[150];

/* =====================================================
   ANALOG DATA STRUCTURE
   ===================================================== */

typedef struct {
    bool ok;
    int16_t raw;
    float voltage;
    float ma;
} analog_value_t;

/* =====================================================
   ADS1115 REGISTER READ
   ===================================================== */

static esp_err_t ads_reg_read(
    uint8_t reg,
    uint16_t *value
) {
    uint8_t response[2] = {0};

    esp_err_t err = i2c_master_transmit_receive(
        s_ads,
        &reg,
        1,
        response,
        2,
        250
    );

    if (err == ESP_OK) {
        *value =
            ((uint16_t)response[0] << 8) |
            response[1];
    }

    return err;
}

/* =====================================================
   ADS1115 SINGLE CONVERSION
   ===================================================== */

static esp_err_t ads_one_conversion(
    uint8_t ain,
    int16_t *raw
) {
    if (ain > 3) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t cfg =
        (uint16_t)(
            0x8583u |
            ((uint16_t)(4u + ain) << 12)
        );

    const uint8_t tx[3] = {
        0x01,
        (uint8_t)(cfg >> 8),
        (uint8_t)cfg
    };

    esp_err_t err = i2c_master_transmit(
        s_ads,
        tx,
        sizeof(tx),
        250
    );

    if (err != ESP_OK) {
        return err;
    }

    for (int retry = 0; retry < 15; retry++) {

        vTaskDelay(pdMS_TO_TICKS(10));

        uint16_t status = 0;

        err = ads_reg_read(0x01, &status);

        if (err != ESP_OK) {
            return err;
        }

        if (status & 0x8000u) {

            uint16_t conv = 0;

            err = ads_reg_read(
                0x00,
                &conv
            );

            if (err == ESP_OK) {
                *raw = (int16_t)conv;
            }

            return err;
        }
    }

    return ESP_ERR_TIMEOUT;
}

/* =====================================================
   READ ANALOG SENSOR
   ===================================================== */

static analog_value_t read_analog(
    uint8_t channel
) {
    analog_value_t result = {0};

    int32_t sum = 0;
    int count = 0;

    if (!s_adc_ready) {
        return result;
    }

    for (int i = 0; i < ADC_AVERAGE; i++) {

        int16_t sample = 0;

        if (
            ads_one_conversion(
                channel,
                &sample
            ) == ESP_OK
        ) {
            sum += sample;
            count++;
        }
    }

    if (count == 0) {
        return result;
    }

    result.ok = true;

    result.raw =
        (int16_t)(sum / count);

    result.voltage =
        ((float)sum / count) *
        (2.048f / 32768.0f);

    result.ma =
        (result.voltage / SHUNT_OHMS) *
        1000.0f;

    return result;
}

/* =====================================================
   INITIALIZE ADS1115
   ===================================================== */

static void init_adc(void) {

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };

    i2c_master_bus_handle_t bus = NULL;

    esp_err_t er = i2c_new_master_bus(
        &bus_config,
        &bus
    );

    if (er != ESP_OK) {
        ESP_LOGE(
            TAG,
            "I2C bus failed: %s",
            esp_err_to_name(er)
        );
        return;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = 100000
    };

    er = i2c_master_bus_add_device(
        bus,
        &device_config,
        &s_ads
    );

    if (er != ESP_OK) {
        ESP_LOGE(
            TAG,
            "ADS1115 add failed: %s",
            esp_err_to_name(er)
        );
        return;
    }

    er = i2c_master_probe(
        bus,
        ADS1115_ADDR,
        250
    );

    if (er != ESP_OK) {
        ESP_LOGW(
            TAG,
            "ADS1115 not detected"
        );
        return;
    }

    s_adc_ready = true;

    ESP_LOGI(
        TAG,
        "ADS1115 READY | A0=TEMP A2=LEVEL A3=PRESSURE"
    );
}

/* =====================================================
   INITIALIZE RS485
   ===================================================== */

static void init_rs485(void) {

    const uart_config_t uart_config = {
        .baud_rate = MODBUS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    esp_err_t er = uart_driver_install(
        MODBUS_UART,
        512,
        0,
        0,
        NULL,
        0
    );

    if (er != ESP_OK) {
        ESP_LOGE(
            TAG,
            "RS485 UART install failed"
        );
        return;
    }

    er = uart_param_config(
        MODBUS_UART,
        &uart_config
    );

    if (er == ESP_OK) {
        er = uart_set_pin(
            MODBUS_UART,
            MODBUS_TX_PIN,
            MODBUS_RX_PIN,
            MODBUS_RTS_PIN,
            UART_PIN_NO_CHANGE
        );
    }

    if (er == ESP_OK) {
        er = uart_set_mode(
            MODBUS_UART,
            UART_MODE_RS485_HALF_DUPLEX
        );
    }

    if (er != ESP_OK) {
        ESP_LOGE(
            TAG,
            "RS485 config failed: %s",
            esp_err_to_name(er)
        );
        return;
    }

    s_rs485_ready = true;

    ESP_LOGI(
        TAG,
        "RS485 READY | TX15 RX16 RTS14"
    );
}

/* =====================================================
   MODBUS CRC16
   ===================================================== */

static uint16_t modbus_crc16(
    const uint8_t *data,
    size_t length
) {
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < length; i++) {

        crc ^= data[i];

        for (unsigned b = 0; b < 8; b++) {

            if (crc & 1u) {
                crc =
                    (uint16_t)(
                        (crc >> 1) ^ 0xA001u
                    );
            } else {
                crc =
                    (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}

/* =====================================================
   EWIG MODBUS READ
   ===================================================== */

static bool ewig_read_raw(
    uint16_t regs[EWIG_REG_COUNT]
) {
    if (!s_rs485_ready) {
        return false;
    }

    uint8_t query[8] = {
        EWIG_SLAVE_ID,
        EWIG_FUNCTION,
        (uint8_t)(EWIG_REG_START >> 8),
        (uint8_t)EWIG_REG_START,
        (uint8_t)(EWIG_REG_COUNT >> 8),
        (uint8_t)EWIG_REG_COUNT,
        0,
        0
    };

    uint16_t crc = modbus_crc16(
        query,
        6
    );

    query[6] = (uint8_t)crc;
    query[7] = (uint8_t)(crc >> 8);

    ESP_ERROR_CHECK(
        uart_flush_input(MODBUS_UART)
    );

    int tx = uart_write_bytes(
        MODBUS_UART,
        (const char *)query,
        sizeof(query)
    );

    if (tx != sizeof(query)) {
        ESP_LOGW(
            TAG,
            "EWIG TX failed"
        );
        return false;
    }

    if (
        uart_wait_tx_done(
            MODBUS_UART,
            pdMS_TO_TICKS(200)
        ) != ESP_OK
    ) {
        ESP_LOGW(
            TAG,
            "EWIG TX timeout"
        );
        return false;
    }

    uint8_t response[64] = {0};

    size_t received = 0;
    int expected = -1;

    const TickType_t deadline =
        xTaskGetTickCount() +
        pdMS_TO_TICKS(500);

    while (
        (int32_t)(
            deadline - xTaskGetTickCount()
        ) > 0 &&
        received < sizeof(response)
    ) {

        int n = uart_read_bytes(
            MODBUS_UART,
            response + received,
            sizeof(response) - received,
            pdMS_TO_TICKS(40)
        );

        if (n > 0) {
            received += (size_t)n;
        }

        if (received >= 3) {

            expected =
                (response[1] & 0x80)
                ? 5
                : (5 + response[2]);

            if (
                expected >
                (int)sizeof(response)
            ) {
                break;
            }

            if (
                received >=
                (size_t)expected
            ) {
                break;
            }
        }
    }

    if (
        received < 5 ||
        expected <= 0 ||
        received != (size_t)expected
    ) {
        ESP_LOGW(
            TAG,
            "EWIG incomplete response"
        );
        return false;
    }

    const uint16_t rx_crc =
        (uint16_t)response[received - 2] |
        ((uint16_t)response[received - 1] << 8);

    if (
        rx_crc !=
        modbus_crc16(
            response,
            received - 2
        )
    ) {
        ESP_LOGW(
            TAG,
            "EWIG CRC incorrect"
        );
        return false;
    }

    if (
        response[0] != EWIG_SLAVE_ID
    ) {
        return false;
    }

    if (
        response[1] ==
        (EWIG_FUNCTION | 0x80u)
    ) {
        ESP_LOGW(
            TAG,
            "EWIG Modbus exception"
        );
        return false;
    }

    if (
        response[1] != EWIG_FUNCTION ||
        response[2] != EWIG_REG_COUNT * 2u
    ) {
        return false;
    }

    for (
        int i = 0;
        i < EWIG_REG_COUNT;
        i++
    ) {
        regs[i] =
            ((uint16_t)response[3 + i * 2] << 8) |
            response[4 + i * 2];
    }

    return true;
}

/* =====================================================
   SENSOR LOGGING
   ===================================================== */

static void print_sensor(
    const char *name,
    int ain,
    analog_value_t v
) {
    if (!v.ok) {
        ESP_LOGW(
            TAG,
            "%s A%d ADC FAILED",
            name,
            ain
        );
        return;
    }

    const char *quality =
        (v.ma <= 3.6f || v.ma >= 21.0f)
        ? "CHECK_LOOP"
        : (v.ma < 4.0f || v.ma > 20.0f)
        ? "NEAR_OUTSIDE_RANGE"
        : "NOMINAL";

    ESP_LOGI(
        TAG,
        "%s A%d | RAW=%d | %.4f V | %.3f mA | %s",
        name,
        ain,
        v.raw,
        v.voltage,
        v.ma,
        quality
    );
}

/* =====================================================
   GMS SNAPSHOT
   ===================================================== */

void gms_get_snapshot(
    gms_measurement_t *out
) {
    portENTER_CRITICAL(
        &s_data_lock
    );

    *out = s_last;

    portEXIT_CRITICAL(
        &s_data_lock
    );
}

/* =====================================================
   CONNECTION STATUS
   ===================================================== */

bool gms_wifi_online(void) {

    return
        s_wifi_events &&
        (
            xEventGroupGetBits(
                s_wifi_events
            ) & WIFI_GOT_IP_BIT
        );
}

bool gms_mqtt2_online(void) { return s_mqtt2_connected; }

bool gms_mqtt_online(void) {

    return s_mqtt_connected;
}

void gms_get_station_ip(
    char out[16]
) {
    esp_netif_ip_info_t info = {0};

    if (
        s_sta_netif &&
        gms_wifi_online() &&
        esp_netif_get_ip_info(
            s_sta_netif,
            &info
        ) == ESP_OK
    ) {
        snprintf(
            out,
            16,
            IPSTR,
            IP2STR(&info.ip)
        );
    } else {
        strcpy(out, "-");
    }
}

void gms_get_ap_ssid(
    char out[40]
) {
    strcpy(
        out,
        s_ap_name
    );
}

/* =====================================================
   RESTART
   ===================================================== */

static void restart_task(
    void *p
) {
    (void)p;

    vTaskDelay(
        pdMS_TO_TICKS(1600)
    );

    esp_restart();
}

void gms_schedule_restart(void) {

    xTaskCreate(
        restart_task,
        "cfg_restart",
        2048,
        NULL,
        3,
        NULL
    );
}

/* =====================================================
   WIFI EVENTS & AUTO-FALLBACK
   ===================================================== */

static int s_wifi_retry_count = 0;
static bool s_wifi_using_fallback = false;

static void wifi_events(
    void *arg,
    esp_event_base_t base,
    int32_t event_id,
    void *data
) {
    (void)arg;

    if (
        base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START
    ) {
        esp_wifi_connect();

    } else if (
        base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED
    ) {
        s_mqtt_connected = false;
        s_mqtt2_connected = false;

        xEventGroupClearBits(
            s_wifi_events,
            WIFI_GOT_IP_BIT
        );

        wifi_event_sta_disconnected_t *e =
            data;

        ESP_LOGW(
            TAG,
            "WiFi disconnected reason=%d (retry %d/3)",
            e ? (int)e->reason : -1,
            s_wifi_retry_count + 1
        );

        s_wifi_retry_count++;
        if (s_wifi_retry_count >= 3) {
            s_wifi_retry_count = 0;
            s_wifi_using_fallback = !s_wifi_using_fallback;
            wifi_config_t new_cfg = {0};

            if (s_wifi_using_fallback) {
                ESP_LOGW(TAG, "Mencoba alternatif Wi-Fi: masch.internal");
                strncpy((char *)new_cfg.sta.ssid, "masch.internal", sizeof(new_cfg.sta.ssid) - 1);
                strncpy((char *)new_cfg.sta.password, "jlsrbz20@3", sizeof(new_cfg.sta.password) - 1);
            } else {
                const char *target_ssid = gms_settings.ssid[0] ? gms_settings.ssid : "aisyah";
                ESP_LOGW(TAG, "Mencoba kembali Wi-Fi utama: %s", target_ssid);
                strncpy((char *)new_cfg.sta.ssid, target_ssid, sizeof(new_cfg.sta.ssid) - 1);
                strncpy((char *)new_cfg.sta.password, gms_settings.wifi_password, sizeof(new_cfg.sta.password) - 1);
            }
            new_cfg.sta.pmf_cfg.capable = true;
            esp_wifi_set_config(WIFI_IF_STA, &new_cfg);
        }

        esp_wifi_connect();

    } else if (
        base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP
    ) {
        ip_event_got_ip_t *e = data;
        s_wifi_retry_count = 0;

        ESP_LOGI(
            TAG,
            "WiFi TERKONEKSI! IP: " IPSTR " (SSID: %s)",
            IP2STR(&e->ip_info.ip),
            s_wifi_using_fallback ? "masch.internal" : (gms_settings.ssid[0] ? gms_settings.ssid : "aisyah")
        );

        xEventGroupSetBits(
            s_wifi_events,
            WIFI_GOT_IP_BIT
        );
    }
}

/* =====================================================
   IP PARSER
   ===================================================== */

static bool parse_ip4(
    const char *text,
    esp_ip4_addr_t *dest
) {
    ip4_addr_t temp;

    if (!ip4addr_aton(text, &temp)) {
        return false;
    }

    dest->addr = temp.addr;

    return true;
}

/* =====================================================
   WIFI AP + STATION
   ===================================================== */

static void wifi_start_apsta(void) {

    ESP_ERROR_CHECK(
        esp_netif_init()
    );

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    s_wifi_events =
        xEventGroupCreate();

    configASSERT(s_wifi_events);

    esp_netif_t *ap =
        esp_netif_create_default_wifi_ap();

    (void)ap;

    s_sta_netif =
        esp_netif_create_default_wifi_sta();

    wifi_init_config_t init =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&init)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_events,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_events,
            NULL
        )
    );

    uint8_t mac[6];

    ESP_ERROR_CHECK(
        esp_wifi_get_mac(
            WIFI_IF_AP,
            mac
        )
    );

    snprintf(
        s_ap_name,
        sizeof(s_ap_name),
        "GMS-Internal-%02X%02X",
        mac[4],
        mac[5]
    );

    char ap_pw[20] = {0};

    gms_store_get_ap_password(
        ap_pw
    );

    wifi_config_t cfg_ap = {0};

    /* AP SSID - FIX FORMAT TRUNCATION */

    size_t ap_ssid_len =
        strnlen(
            s_ap_name,
            sizeof(cfg_ap.ap.ssid)
        );

    memcpy(
        cfg_ap.ap.ssid,
        s_ap_name,
        ap_ssid_len
    );

    cfg_ap.ap.ssid_len =
        (uint8_t)ap_ssid_len;

    /* AP PASSWORD */

    size_t ap_pw_len =
        strnlen(ap_pw, sizeof(ap_pw));

    memcpy(
        cfg_ap.ap.password,
        ap_pw,
        ap_pw_len
    );

    cfg_ap.ap.max_connection = 3;
    cfg_ap.ap.channel = 1;

    cfg_ap.ap.authmode =
        WIFI_AUTH_WPA2_PSK;

    cfg_ap.ap.pmf_cfg.required = false;
    cfg_ap.ap.pmf_cfg.capable = false;

    /* WIFI STATION */

    wifi_config_t cfg_sta = {0};

    size_t sta_ssid_len =
        strnlen(
            gms_settings.ssid,
            sizeof(cfg_sta.sta.ssid)
        );

    memcpy(
        cfg_sta.sta.ssid,
        gms_settings.ssid,
        sta_ssid_len
    );

    size_t sta_pw_len =
        strnlen(
            gms_settings.wifi_password,
            sizeof(cfg_sta.sta.password)
        );

    memcpy(
        cfg_sta.sta.password,
        gms_settings.wifi_password,
        sta_pw_len
    );

    cfg_sta.sta.pmf_cfg.capable = true;

    /* STATIC IP CONFIGURATION */

    if (gms_settings.static_ip) {

        esp_netif_ip_info_t info = {0};
        esp_ip4_addr_t dns_addr = {0};

        if (
            parse_ip4(
                gms_settings.ip,
                &info.ip
            ) &&
            parse_ip4(
                gms_settings.mask,
                &info.netmask
            ) &&
            parse_ip4(
                gms_settings.gateway,
                &info.gw
            ) &&
            parse_ip4(
                gms_settings.dns,
                &dns_addr
            )
        ) {

            esp_err_t er =
                esp_netif_dhcpc_stop(
                    s_sta_netif
                );

            if (
                er != ESP_OK &&
                er !=
                ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED
            ) {
                ESP_LOGW(
                    TAG,
                    "DHCP stop failed: %s",
                    esp_err_to_name(er)
                );
            }

            ESP_ERROR_CHECK(
                esp_netif_set_ip_info(
                    s_sta_netif,
                    &info
                )
            );

            esp_netif_dns_info_t dns = {0};

            dns.ip.type =
                ESP_IPADDR_TYPE_V4;

            dns.ip.u_addr.ip4.addr =
                dns_addr.addr;

            ESP_ERROR_CHECK(
                esp_netif_set_dns_info(
                    s_sta_netif,
                    ESP_NETIF_DNS_MAIN,
                    &dns
                )
            );

            ESP_LOGW(
                TAG,
                "Using STATIC IPv4"
            );

        } else {

            ESP_LOGE(
                TAG,
                "Invalid Static IP configuration"
            );
        }
    }

    /* START WIFI */

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_APSTA
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_AP,
            &cfg_ap
        )
    );

    if (gms_settings.ssid[0]) {

        ESP_ERROR_CHECK(
            esp_wifi_set_config(
                WIFI_IF_STA,
                &cfg_sta
            )
        );
    }

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGW(
        TAG,
        "================================"
    );

    ESP_LOGW(
        TAG,
        "GMS LOCAL WEB"
    );

    ESP_LOGW(
        TAG,
        "AP IP: http://192.168.4.1"
    );

    ESP_LOGW(
        TAG,
        "AP SSID: %s",
        s_ap_name
    );

    ESP_LOGW(
        TAG,
        "AP Password: %s",
        ap_pw
    );

    ESP_LOGW(
        TAG,
        "STA WiFi: %s",
        gms_settings.ssid[0]
        ? gms_settings.ssid
        : "NOT CONFIGURED"
    );

    ESP_LOGW(
        TAG,
        "================================"
    );

    memset(
        ap_pw,
        0,
        sizeof(ap_pw)
    );
}

/* =====================================================
   NTP SYNC
   ===================================================== */

static bool sync_ntp(void) {

    if (!s_sntp_started) {

        esp_sntp_config_t cfg =
            ESP_NETIF_SNTP_DEFAULT_CONFIG(
                "pool.ntp.org"
            );

        esp_err_t er =
            esp_netif_sntp_init(
                &cfg
            );

        if (er != ESP_OK) {
            ESP_LOGW(
                TAG,
                "NTP initialization failed"
            );
            return false;
        }

        s_sntp_started = true;
    }

    for (int attempt = 0; attempt < 6; attempt++) {

        if (!gms_wifi_online()) {
            return false;
        }

        if (
            esp_netif_sntp_sync_wait(
                pdMS_TO_TICKS(5000)
            ) == ESP_OK
        ) {
            time_t now = time(NULL);

            if (now > 1700000000) {
                ESP_LOGI(
                    TAG,
                    "NTP synchronized"
                );
                return true;
            }
        }
    }

    return false;
}

/* =====================================================
   DIAGNOSTIC & THINGSBOARD OTA ATTRIBUTE HANDLER
   ===================================================== */

static const char *get_reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_SW:        return "SW_RESET";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t max_len) {
    if (!json || !key || !out || max_len == 0) return false;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '\"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '\"' && i < max_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0);
}

static void handle_ota_attributes(const char *payload, const char *host, const char *token) {
    if (!payload || !host || !token || !token[0]) return;
    char fw_title[64] = {0};
    char fw_version[64] = {0};

    /* Ekstrak fw_title atau fallback ke title */
    if (!json_extract_string(payload, "fw_title", fw_title, sizeof(fw_title))) {
        json_extract_string(payload, "title", fw_title, sizeof(fw_title));
    }
    /* Ekstrak fw_version atau fallback ke version */
    if (!json_extract_string(payload, "fw_version", fw_version, sizeof(fw_version))) {
        json_extract_string(payload, "version", fw_version, sizeof(fw_version));
    }

    if (fw_title[0] && fw_version[0]) {
        const esp_app_desc_t *desc = esp_app_get_description();
        const char *cur_ver = desc ? desc->version : "1.0.0";
        if (strcmp(fw_version, cur_ver) == 0) {
            ESP_LOGI(TAG, "Firmware aktif sudah versi terbaru: %s (OTA diabaikan)", cur_ver);
            return;
        }

        char ota_url[256];
        snprintf(ota_url, sizeof(ota_url),
                 "http://%s:8080/api/v1/%s/firmware?title=%s&version=%s",
                 host, token, fw_title, fw_version);
        ESP_LOGW(TAG, "Menerima OTA dari ThingsBoard: %s (v%s) -> Memicu task_ota", fw_title, fw_version);
        gms_ota_trigger(ota_url);
    }
}

/* =====================================================
   MQTT EVENTS
   ===================================================== */

static void mqtt_event(
    void *arg,
    esp_event_base_t base,
    int32_t id,
    void *data
) {
    (void)arg;
    (void)base;

    esp_mqtt_event_handle_t ev = data;

    switch ((esp_mqtt_event_id_t)id) {

        case MQTT_EVENT_CONNECTED:

            s_mqtt_connected = true;

            ESP_LOGI(
                TAG,
                "MQTT CONNECTED TLS"
            );

            esp_mqtt_client_subscribe(
                ev->client,
                GMS_TOPIC_ATTRIBUTES,
                1
            );

            esp_mqtt_client_subscribe(
                ev->client,
                GMS_TOPIC_RPC_REQUEST,
                1
            );

            break;

        case MQTT_EVENT_DISCONNECTED:

            s_mqtt_connected = false;

            ESP_LOGW(
                TAG,
                "MQTT disconnected"
            );

            break;

        case MQTT_EVENT_SUBSCRIBED:

            ESP_LOGI(
                TAG,
                "MQTT subscribed ID=%d",
                ev->msg_id
            );

            break;

        case MQTT_EVENT_PUBLISHED:

            ESP_LOGI(
                TAG,
                "MQTT published ID=%d",
                ev->msg_id
            );

            break;

        case MQTT_EVENT_DATA:

            ESP_LOGI(
                TAG,
                "MQTT RX topic=%.*s",
                ev->topic_len,
                ev->topic
            );

            /*
             * Read-only bench mode.
             * No relay activation.
             */

            if (ev->topic_len >= (int)strlen(GMS_TOPIC_ATTRIBUTES) &&
                strncmp(ev->topic, GMS_TOPIC_ATTRIBUTES, strlen(GMS_TOPIC_ATTRIBUTES)) == 0 &&
                ev->data && ev->data_len > 0) {
                char buf[512];
                int copy_len = ev->data_len < (int)sizeof(buf) - 1 ? ev->data_len : (int)sizeof(buf) - 1;
                memcpy(buf, ev->data, copy_len);
                buf[copy_len] = '\0';
                handle_ota_attributes(buf, gms_settings.mqtt_host, gms_settings.mqtt_token);
            }

            if (
                ev->topic_len > 26 &&
                strncmp(
                    ev->topic,
                    "v1/devices/me/rpc/request/",
                    26
                ) == 0
            ) {

                const char *request_id =
                    ev->topic + 26;

                int n =
                    ev->topic_len - 26;

                if (n > 0 && n <= 24) {

                    char topic[85];

                    snprintf(
                        topic,
                        sizeof(topic),
                        "v1/devices/me/rpc/response/%.*s",
                        n,
                        request_id
                    );

                    esp_mqtt_client_publish(
                        ev->client,
                        topic,
                        "{\"error\":\"bench_read_only\"}",
                        0,
                        1,
                        0
                    );
                }
            }

            break;

        case MQTT_EVENT_ERROR:

            ESP_LOGW(
                TAG,
                "MQTT TLS connection error"
            );

            break;

        default:
            break;
    }
}

/* =====================================================
   MQTT START
   ===================================================== */

static void mqtt_start_stored_config(void) {

    if (!gms_settings.mqtt_token[0]) {

        ESP_LOGW(
            TAG,
            "MQTT token not configured"
        );

        return;
    }

    snprintf(
        s_mqtt_uri,
        sizeof(s_mqtt_uri),
        "mqtts://%s:%u",
        gms_settings.mqtt_host,
        (unsigned)gms_settings.mqtt_port
    );

    const esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri = s_mqtt_uri,
            .verification.crt_bundle_attach =
                esp_crt_bundle_attach
        },
        .credentials = {
            .username =
                gms_settings.mqtt_token
        },
        .session = {
            .keepalive = 60
        }
    };

    s_mqtt_client =
        esp_mqtt_client_init(
            &cfg
        );

    if (!s_mqtt_client) {

        ESP_LOGE(
            TAG,
            "MQTT initialization failed"
        );

        return;
    }

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            s_mqtt_client,
            ESP_EVENT_ANY_ID,
            mqtt_event,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_mqtt_client_start(
            s_mqtt_client
        )
    );

    s_mqtt_started = true;

    ESP_LOGI(
        TAG,
        "MQTT connecting to %s",
        gms_settings.mqtt_host
    );
}

/* =====================================================
   MQTT 2 - Mastify (plain MQTT, bench-test only).
   Client kedua tidak mengambil alih koneksi ThingsBoard.
   ===================================================== */
static void mqtt2_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_mqtt2_connected = true;
            ESP_LOGI(TAG, "MASTIFY MQTT 2 CONNECTED");
            esp_mqtt_client_subscribe(s_mqtt2_client, GMS_TOPIC_ATTRIBUTES, 1);
            esp_mqtt_client_subscribe(s_mqtt2_client, "v1/devices/me/attributes/response/+", 1);
            /* Minta shared attributes ThingsBoard (Mastify) */
            esp_mqtt_client_publish(s_mqtt2_client, "v1/devices/me/attributes/request/1",
                                    "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum\"}",
                                    0, 1, 0);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_mqtt2_connected = false;
            ESP_LOGW(TAG, "MASTIFY MQTT 2 disconnected");
            break;
        case MQTT_EVENT_SUBSCRIBED: {
            esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
            ESP_LOGI(TAG, "MASTIFY MQTT 2 SUBSCRIBED ID=%d", ev ? ev->msg_id : 0);
            break;
        }
        case MQTT_EVENT_DATA: {
            esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
            if (ev && ev->data && ev->data_len > 0) {
                char buf[512];
                int copy_len = ev->data_len < (int)sizeof(buf) - 1 ? ev->data_len : (int)sizeof(buf) - 1;
                memcpy(buf, ev->data, copy_len);
                buf[copy_len] = '\0';

                ESP_LOGI(TAG, "MASTIFY MQTT 2 RX (topic=%.*s): %s",
                         ev->topic_len, ev->topic ? ev->topic : "N/A", buf);

                handle_ota_attributes(buf, gms_mqtt2_settings.host, gms_mqtt2_settings.token);
            }
            break;
        }
        case MQTT_EVENT_ERROR:
            ESP_LOGW(TAG, "MASTIFY MQTT 2 connection error");
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MASTIFY MQTT 2 published");
            break;
        default: break;
    }
}

static void mqtt2_start_stored_config(void)
{
    if (!gms_mqtt2_settings.enabled || !gms_mqtt2_settings.token[0]) return;
    snprintf(s_mqtt2_uri, sizeof(s_mqtt2_uri), "mqtt://%s:%u",
             gms_mqtt2_settings.host, (unsigned)gms_mqtt2_settings.port);
    const esp_mqtt_client_config_t cfg = {
        .broker = {.address.uri = s_mqtt2_uri},
        .credentials = {.username = gms_mqtt2_settings.token},
        .session = {.keepalive = 60}
    };
    s_mqtt2_client = esp_mqtt_client_init(&cfg);
    if (!s_mqtt2_client) {
        ESP_LOGE(TAG, "MASTIFY MQTT 2 init failed");
        return;
    }
    esp_err_t err = esp_mqtt_client_register_event(
        s_mqtt2_client, ESP_EVENT_ANY_ID, mqtt2_event, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(s_mqtt2_client);
    if (err == ESP_OK) {
        s_mqtt2_started = true;
        ESP_LOGI(TAG, "MASTIFY MQTT 2 starting");
    } else {
        ESP_LOGE(TAG, "MASTIFY MQTT 2 start failed: %s", esp_err_to_name(err));
    }
}

/* =====================================================
    MQTT NETWORK TASK
   ===================================================== */

static void mqtt_network_task(
    void *arg
) {
    (void)arg;

    for (;;) {

        if (gms_wifi_online()) {
            if (!s_ntp_ready) s_ntp_ready = sync_ntp();
            if (s_ntp_ready) {
                if (!s_mqtt_started && gms_settings.mqtt_token[0])
                    mqtt_start_stored_config();
                if (!s_mqtt2_started && gms_mqtt2_settings.enabled)
                    mqtt2_start_stored_config();
            }

            /* Polling atribut OTA dari Mastify setiap 15 detik */
            if (s_mqtt2_connected && s_mqtt2_client) {
                static int s_ota_poll_cnt = 0;
                if (++s_ota_poll_cnt >= 3) {
                    s_ota_poll_cnt = 0;
                    esp_mqtt_client_publish(s_mqtt2_client,
                                            "v1/devices/me/attributes/request/1",
                                            "{\"sharedKeys\":\"fw_title,fw_version,title,version\"}",
                                            0, 1, 0);
                }
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(5000)
        );
    }
}

/* =====================================================
   JSON BUILDER
   ===================================================== */

static void json_add(
    char *out,
    size_t len,
    size_t *used,
    const char *fmt,
    ...
) {
    if (*used >= len) {
        return;
    }

    va_list ap;

    va_start(ap, fmt);

    int n = vsnprintf(
        out + *used,
        len - *used,
        fmt,
        ap
    );

    va_end(ap);

    if (n > 0) {

        size_t d = (size_t)n;

        *used =
            (d >= len - *used)
            ? len
            : *used + d;
    }
}

/* =====================================================
   CURRENT LOOP VALIDATION
   ===================================================== */

static bool loop_in_service_range(
    analog_value_t v
) {
    return
        v.ok &&
        v.ma >= 3.6f &&
        v.ma <= 21.0f;
}

/* =====================================================
   USER-DEFINED SCALING AND ALARM EVALUATION
   0 ERROR | 1 NORMAL | 2 WARNING | 3 CRITICAL | 4 OUT_OF_RANGE
   ===================================================== */

static uint8_t sensor_alarm(analog_value_t v,
                            const gms_sensor_config_t *cfg,
                            float *scaled)
{
    *scaled = 0.0f;
    if (!loop_in_service_range(v) || !cfg) return 0;

    float value = gms_sensor_scale(v.ma, cfg);
    if (!isfinite(value)) return 0;
    *scaled = value;

    // Out-of-range is not a normal threshold condition.
    if (v.ma < cfg->input_min_ma || v.ma > cfg->input_max_ma)
        return 4;
    if (value <= cfg->critical_low || value >= cfg->critical_high)
        return 3;
    if (value <= cfg->warning_low || value >= cfg->warning_high)
        return 2;
    return 1;
}

static const char *sensor_alarm_text(uint8_t alarm)
{
    switch (alarm) {
        case 1: return "NORMAL";
        case 2: return "WARNING";
        case 3: return "CRITICAL";
        case 4: return "OUT_OF_RANGE";
        default: return "SENSOR_ERROR";
    }
}

/* =====================================================
   MQTT TELEMETRY PUBLISH
   ===================================================== */

static void publish(const gms_measurement_t *s)
{
    if (!s_mqtt_connected || !s_mqtt_client || !s) return;
    char json[1500] = {0};
    size_t used = 0;

    json_add(json, sizeof(json), &used,
        "{\"device\":\"GMS-001\",\"bench_mode\":true,"
        "\"temp_adc_ok\":%s,\"level_adc_ok\":%s,\"pressure_adc_ok\":%s,"
        "\"temp_loop_ok\":%s,\"level_loop_ok\":%s,\"pressure_loop_ok\":%s,"
        "\"ewig_modbus_ok\":%s,\"ewig_measurement_valid\":false",
        s->adc_ok[0] ? "true" : "false",
        s->adc_ok[1] ? "true" : "false",
        s->adc_ok[2] ? "true" : "false",
        s->loop_ok[0] ? "true" : "false",
        s->loop_ok[1] ? "true" : "false",
        s->loop_ok[2] ? "true" : "false",
        s->ewig_ok ? "true" : "false");

    const char *keys[3] = {"temp", "level", "pressure"};
    const char *scaled_keys[3] = {"temp_c", "level_percent", "pressure_bar"};
    for (int i = 0; i < 3; ++i) {
        json_add(json, sizeof(json), &used,
            ",\"%s_status\":\"%s\"", keys[i], sensor_alarm_text(s->alarm[i]));
        if (s->adc_ok[i]) {
            json_add(json, sizeof(json), &used,
                ",\"%s_voltage\":%.4f,\"%s_ma\":%.3f",
                keys[i], s->volts[i], keys[i], s->ma[i]);
        }
        if (s->alarm[i] >= 1 && s->alarm[i] <= 3) {
            json_add(json, sizeof(json), &used,
                ",\"%s\":%.3f", scaled_keys[i], s->scaled[i]);
        }
    }
    // Explicit test source markers on EVERY telemetry message, including STOP.
    json_add(json, sizeof(json), &used,
        ",\"pressure_calibrated\":false,"
        "\"sensor_test_active\":%s,\"sensor_test_channel\":\"%s\","
        "\"sensor_test_real_ma\":%.3f,\"sensor_test_virtual_ma\":%.3f,"
        "\"pressure_fault_test_active\":%s,\"pressure_real_ma\":%.3f",
        s->sensor_test_active ? "true" : "false",
        sensor_test_channel_name(s->sensor_test_channel),
        s->sensor_test_active ? s->sensor_real_ma[s->sensor_test_channel] : 0.0f,
        s->sensor_test_active ? s->sensor_test_ma : 0.0f,
        s->pressure_fault_test_active ? "true" : "false", s->pressure_real_ma);
    if (s->ewig_ok) {
        json_add(json, sizeof(json), &used,
            ",\"ewig_raw_r09\":%u,\"ewig_raw_r10\":%u",
            (unsigned)s->ewig_raw[0], (unsigned)s->ewig_raw[1]);
    }
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *fw_ver = desc ? desc->version : "1.0.0";
    json_add(json, sizeof(json), &used,
             ",\"uptime_s\":%" PRIu32 ",\"buf_count\":0,\"reset_reason\":\"%s\",\"fw_ver\":\"%s\"",
             uptime_s, get_reset_reason_str(), fw_ver);
    json_add(json, sizeof(json), &used, "}");
    if (used >= sizeof(json)) {
        ESP_LOGE(TAG, "JSON buffer overflow prevented");
        return;
    }
    int mid = esp_mqtt_client_publish(s_mqtt_client, GMS_TOPIC_TELEMETRY,
                                      json, 0, 1, 0);
    if (mid >= 0) ESP_LOGI(TAG, "MQTT TX ID=%d", mid);
}

/* =====================================================
   Mastify payload sesuai schema ts + values.
   Level HDL300: 4-20 mA = 0-5 m (0-5000 mm).
   EWIG: diagnostic only (Modbus status + raw R09/R10), NOT V/A/W.
   ===================================================== */
static void publish_mastify(const gms_measurement_t *s)
{
    if (!s || !s_mqtt2_connected || !s_mqtt2_client) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
    if (ms < 1700000000000LL) {
        ESP_LOGW(TAG, "MASTIFY skipped: system time not synchronized");
        return;
    }
    char json[1700] = {0};
    size_t used = 0;
    json_add(json, sizeof(json), &used,
             "{\"ts\":%" PRId64 ",\"values\":{", ms);
    bool first = true;
    const char *ma_keys[3] = {"ai2_ma", "ai3_ma", "ai1_ma"};
    const char *st_keys[3] = {"ai2_st", "ai3_st", "ai1_st"};
    for (int i = 0; i < 3; ++i) {
        if (!s->adc_ok[i] || !isfinite(s->ma[i])) continue;
        json_add(json, sizeof(json), &used,
                 "%s\"%s\":%.3f", first ? "" : ",", ma_keys[i], s->ma[i]);
        first = false;
        const char *status = s->ma[i] < 3.8f ? "FAULT_LOW" :
                             s->ma[i] > 20.5f ? "FAULT_HIGH" : "OK";
        json_add(json, sizeof(json), &used, ",\"%s\":\"%s\"", st_keys[i], status);
    }
    if (s->alarm[GMS_SENSOR_TEMP] >= 1 && s->alarm[GMS_SENSOR_TEMP] <= 3) {
        json_add(json, sizeof(json), &used,
                 "%s\"engine_temp_c\":%.3f", first ? "" : ",", s->scaled[GMS_SENSOR_TEMP]);
        first = false;
    }
    if (s->alarm[GMS_SENSOR_PRESSURE] >= 1 && s->alarm[GMS_SENSOR_PRESSURE] <= 3) {
        json_add(json, sizeof(json), &used,
                 "%s\"oil_press_bar\":%.3f", first ? "" : ",", s->scaled[GMS_SENSOR_PRESSURE]);
        first = false;
    }
    // Level A2: configured engineering value is percent (0-100%).
    // The HDL300 physical range reported for this bench is 0-5 m.
    // Do not emit engineering values when current is invalid/out-of-range.
    if (s->alarm[GMS_SENSOR_LEVEL] >= 1 && s->alarm[GMS_SENSOR_LEVEL] <= 3 &&
        isfinite(s->scaled[GMS_SENSOR_LEVEL]) &&
        s->scaled[GMS_SENSOR_LEVEL] >= 0.0f &&
        s->scaled[GMS_SENSOR_LEVEL] <= 100.0f) {
        float level_mm = 0.0f, level_pct = 0.0f;
        if (gms_tank_measure(s->ma[GMS_SENSOR_LEVEL], &level_mm, &level_pct, NULL)) {
            json_add(json, sizeof(json), &used,
                     "%s\"level_percent\":%.3f,\"fuel_level_mm\":%.3f",
                     first ? "" : ",", (double)level_pct, (double)level_mm);
            first = false;
        }
    }
    // EWIG diagnostic telemetry, NOT validated electrical measurements.
    json_add(json, sizeof(json), &used,
             "%s\"ewig_modbus_ok\":%s", first ? "" : ",",
             s->ewig_ok ? "true" : "false");
    first = false;
    if (s->ewig_ok) {
        json_add(json, sizeof(json), &used,
                 ",\"ewig_raw_r09\":%u,\"ewig_raw_r10\":%u",
                 (unsigned)s->ewig_raw[0], (unsigned)s->ewig_raw[1]);
    }
    /* Source marker is published on every message to clear stale demo state.
       Consumer must HIDE all synthetic keys when sim_active=false. */
    gms_bench_data_t b = {0};
    gms_bench_snapshot(&b);
    json_add(json, sizeof(json), &used,
             ",\"sim_active\":%s,\"sim_source\":\"BENCH_SIMULATION\"",
             b.active ? "true" : "false");
    if (b.active) {
        const char *states[] = {"STOPPED","STARTING","RUNNING","STOPPING","FAULT"};
        int st = b.state >= 0 && b.state <= 4 ? b.state : 0;
        json_add(json, sizeof(json), &used,
                 ",\"engine_state\":\"%s\",\"run_hours\":%.6f,"
                 "\"maint_due_h\":%.3f,\"sim_service_status\":\"%s\"",
                 states[st], b.run_hours, b.maint_due_h, b.service_status);
        json_add(json, sizeof(json), &used,
                 ",\"rpm_calc\":%.1f,\"battery_v\":%.2f,\"freq_hz\":%.2f,"
                 "\"load_pct\":%.2f,\"v_l1\":%.2f,\"v_l2\":%.2f,\"v_l3\":%.2f,"
                 "\"v_l12\":%.2f,\"v_l23\":%.2f,\"v_l31\":%.2f,"
                 "\"i_l1\":%.2f,\"i_l2\":%.2f,\"i_l3\":%.2f,"
                 "\"p_kw\":%.2f,\"q_kvar\":%.2f,\"s_kva\":%.2f,\"pf\":%.3f,"
                 "\"sim_fuel_est_l\":%.3f",
                 b.rpm_calc, b.battery_v, b.freq_hz, b.load_pct,
                 b.v_l1, b.v_l2, b.v_l3, b.v_l12, b.v_l23, b.v_l31,
                 b.i_l1, b.i_l2, b.i_l3, b.p_kw, b.q_kvar, b.s_kva, b.pf,
                 b.fuel_est_l);
    }
    /* Mark source on every MQTT message, including after STOP/expiry.
       Asri/Vercel MUST distinguish BENCH_TEST vs REAL and check timestamps. */
    json_add(json, sizeof(json), &used,
             ",\"sensor_test_active\":%s,\"sensor_test_channel\":\"%s\","
             "\"sensor_test_real_ma\":%.3f,\"sensor_test_virtual_ma\":%.3f,"
             "\"fault_test_active\":%s,\"fault_test_sensor\":\"%s\","
             "\"fault_test_real_ma\":%.3f",
             s->sensor_test_active ? "true" : "false",
             sensor_test_channel_name(s->sensor_test_channel),
             s->sensor_test_active ? s->sensor_real_ma[s->sensor_test_channel] : 0.0f,
             s->sensor_test_active ? s->sensor_test_ma : 0.0f,
             s->pressure_fault_test_active ? "true" : "false",
             s->pressure_fault_test_active ? "pressure_A3" : "none",
             s->pressure_real_ma);
    if (first) return; // Schema requires at least one value.
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    const esp_app_desc_t *desc = esp_app_get_description();
    const char *fw_ver = desc ? desc->version : "1.0.0";
    json_add(json, sizeof(json), &used,
             ",\"uptime_s\":%" PRIu32 ",\"buf_count\":0,\"reset_reason\":\"%s\",\"fw_ver\":\"%s\"",
             uptime_s, get_reset_reason_str(), fw_ver);
    json_add(json, sizeof(json), &used, "}}");
    if (used >= sizeof(json)) {
        ESP_LOGE(TAG, "MASTIFY JSON overflow");
        return;
    }
    int mid = esp_mqtt_client_publish(s_mqtt2_client, GMS_TOPIC_TELEMETRY,
                                      json, 0, 1, 0);
    if (mid >= 0) ESP_LOGI(TAG, "MASTIFY MQTT 2 TX ID=%d", mid);
}

/* =====================================================
   FREERTOS MULTI-TASK & DUAL-CORE ARCHITECTURE (TABEL 3 TA)
   ===================================================== */

/* -----------------------------------------------------
   CORE 0: DETERMINISTIK & PROTEKSI KESELAMATAN LOKAL
   ----------------------------------------------------- */

/*
 * Task Safety (Prioritas 10, Core 0, Periodik 100 ms)
 * Mengevaluasi batas kritis (Critical) secara lokal.
 * Debounce filter 300 ms (3 siklus berturut-turut).
 * Anggaran respon cut-off <= 520 ms (< 1 detik).
 */
static void task_safety(void *arg) {
    (void)arg;
    uint8_t debounce_count = 0;
    bool cutoff_active = false;

    ESP_LOGI(TAG, "task_safety started on Core %d (Prio %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    for (;;) {
        gms_measurement_t s = {0};
        gms_get_snapshot(&s);

        bool critical_detected = false;
        if (s.alarm[GMS_SENSOR_TEMP] == GMS_ALARM_CRITICAL ||
            s.alarm[GMS_SENSOR_PRESSURE] == GMS_ALARM_CRITICAL) {
            critical_detected = true;
        }

        if (critical_detected) {
            if (debounce_count < 3) {
                debounce_count++;
            }
            if (debounce_count >= 3 && !cutoff_active) {
                cutoff_active = true;
                ESP_LOGE(TAG, "[SAFETY CUT-OFF TRIGGERED] Anomali kritis lokal terdeteksi! Temp alarm=%d, Pressure alarm=%d (Respon <= 520 ms)",
                         s.alarm[GMS_SENSOR_TEMP], s.alarm[GMS_SENSOR_PRESSURE]);
                /* Read-only bench mode: interkoneksi ke GPIO kontaktor fisik diaktifkan saat uji beban */
            }
        } else {
            debounce_count = 0;
            if (cutoff_active) {
                cutoff_active = false;
                ESP_LOGI(TAG, "[SAFETY CUT-OFF NORMAL] Parameter mesin kembali aman.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/*
 * Task Sensor (Prioritas 5, Core 0, Periodik 200 ms)
 * Membaca konversi ADC 16-bit ADS1115 via I2C (A0 Suhu, A2 Level, A3 Tekanan).
 * Filter median-5, deteksi zona NAMUR NE43, evaluasi batas scaling & alarm.
 */
static void task_sensor(void *arg) {
    (void)arg;
    uint32_t log_cycle = 0;

    ESP_LOGI(TAG, "task_sensor started on Core %d (Prio %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    for (;;) {
        analog_value_t t = read_analog(0);
        analog_value_t l = read_analog(2);
        analog_value_t p = read_analog(3);

        analog_value_t values[] = { t, l, p };
        float real_ma[GMS_SENSOR_COUNT];
        float real_voltage[GMS_SENSOR_COUNT];
        for (int i = 0; i < GMS_SENSOR_COUNT; ++i) {
            real_ma[i] = values[i].ma;
            real_voltage[i] = values[i].voltage;
        }

        int test_ch = -1;
        float test_ma = 0.0f;
        uint32_t test_rem = 0;
        bool test_act = gms_sensor_test_snapshot(&test_ch, &test_ma, &test_rem);
        if (test_act && test_ch >= 0 && test_ch < GMS_SENSOR_COUNT) {
            values[test_ch].ok = true;
            values[test_ch].ma = test_ma;
            values[test_ch].voltage = test_ma * SHUNT_OHMS / 1000.0f;
            values[test_ch].raw = (int16_t)lroundf(values[test_ch].voltage / 0.0000625f);
        }

        gms_sensor_settings_t settings;
        gms_sensor_get_settings(&settings);
        const gms_sensor_config_t *cfg[3] = {
            &settings.temperature, &settings.level, &settings.pressure
        };

        float scaled[GMS_SENSOR_COUNT] = {0};
        uint8_t alarm[GMS_SENSOR_COUNT] = {0};
        for (int i = 0; i < 3; ++i) {
            alarm[i] = sensor_alarm(values[i], cfg[i], &scaled[i]);
        }

        if (alarm[GMS_SENSOR_LEVEL] >= 1 && alarm[GMS_SENSOR_LEVEL] <= 3) {
            float tank_pct = 0.0f;
            if (gms_tank_measure(values[GMS_SENSOR_LEVEL].ma, NULL, &tank_pct, NULL)) {
                scaled[GMS_SENSOR_LEVEL] = tank_pct;
                const gms_sensor_config_t *lc = &settings.level;
                alarm[GMS_SENSOR_LEVEL] =
                    tank_pct <= lc->critical_low || tank_pct >= lc->critical_high ? 3 :
                    tank_pct <= lc->warning_low  || tank_pct >= lc->warning_high  ? 2 : 1;
            } else {
                alarm[GMS_SENSOR_LEVEL] = 0;
            }
        }

        /* Update shared snapshot thread-safe */
        portENTER_CRITICAL(&s_data_lock);
        for (int i = 0; i < GMS_SENSOR_COUNT; ++i) {
            s_last.sensor_real_ma[i] = real_ma[i];
            s_last.sensor_real_voltage[i] = real_voltage[i];
            s_last.adc_ok[i] = values[i].ok;
            s_last.loop_ok[i] = loop_in_service_range(values[i]);
            s_last.volts[i] = values[i].voltage;
            s_last.ma[i] = values[i].ma;
            s_last.raw[i] = values[i].raw;
            s_last.scaled[i] = scaled[i];
            s_last.alarm[i] = alarm[i];
        }
        s_last.pressure_real_ma = p.ma;
        s_last.pressure_real_voltage = p.voltage;
        s_last.sensor_test_active = test_act;
        s_last.sensor_test_channel = test_ch;
        s_last.sensor_test_ma = test_ma;
        s_last.sensor_test_remaining_s = test_rem;
        s_last.pressure_fault_test_active = test_act && test_ch == GMS_SENSOR_PRESSURE;
        s_last.pressure_fault_test_remaining_s = s_last.pressure_fault_test_active ? test_rem : 0;
        s_last.pressure_bar = scaled[GMS_SENSOR_PRESSURE];
        portEXIT_CRITICAL(&s_data_lock);

        /* Cetak log setiap 2 detik (10 siklus * 200 ms) */
        if (++log_cycle % 10 == 0) {
            print_sensor("SUHU", 0, t);
            print_sensor("LEVEL", 2, l);
            print_sensor("PRESSURE", 3, p);
            if (test_act) {
                ESP_LOGW(TAG, "BENCH SENSOR TEST channel=%s virtual %.3fmA REAL %.3fmA, %us left",
                         sensor_test_channel_name(test_ch),
                         (double)test_ma,
                         (double)real_ma[test_ch],
                         (unsigned)test_rem);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/*
 * Task Modbus (Prioritas 4, Core 0, Periodik 500 ms)
 * Polling register meter daya digital Modbus RTU RS-485 via UART (EWIG STE96-3E).
 * Timeout 200 ms, retry logic.
 */
static void task_modbus(void *arg) {
    (void)arg;
    uint32_t log_cycle = 0;

    ESP_LOGI(TAG, "task_modbus started on Core %d (Prio %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    for (;;) {
        uint16_t regs[EWIG_REG_COUNT] = {0};
        bool ewig = ewig_read_raw(regs);

        portENTER_CRITICAL(&s_data_lock);
        s_last.ewig_ok = ewig;
        s_last.ewig_raw[0] = regs[0];
        s_last.ewig_raw[1] = regs[1];
        portEXIT_CRITICAL(&s_data_lock);

        if (++log_cycle % 4 == 0) {
            ESP_LOGI(TAG, "EWIG %s", ewig ? "MODBUS OK" : "NO RESPONSE");
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* -----------------------------------------------------
   CORE 1: JARINGAN, PENYIMPANAN FLASH, & KOMPUTASI
   ----------------------------------------------------- */

/*
 * Task State (Prioritas 6, Core 1, Periodik 1 s)
 * FSM operasi genset (STOPPED, STARTING, RUNNING, STOPPING, FAULT).
 * Uptime & akumulasi hour meter.
 */
static void task_state(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "task_state started on Core %d (Prio %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        portENTER_CRITICAL(&s_data_lock);
        s_last.uptime_ms = now_ms;
        portEXIT_CRITICAL(&s_data_lock);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*
 * Task Publish (Prioritas 3, Core 1, Periodik 10 s RUNNING / 60 s STOPPED)
 * Memformat telemetri ke JSON batch dan mempublikasikan ke broker MQTT Mastify.
 */
static void task_publish(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "task_publish started on Core %d (Prio %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    for (;;) {
        gms_measurement_t s = {0};
        gms_get_snapshot(&s);

        publish(&s);
        publish_mastify(&s);

        /* Umpan balik status ke manajer swauji (Self-Test) OTA jika partisi baru aktif */
        bool any_adc_ok = s.adc_ok[0] || s.adc_ok[1] || s.adc_ok[2];
        gms_ota_self_test_feed(s_mqtt2_connected, any_adc_ok, s.ewig_ok);

        uint32_t interval_ms = (uint32_t)gms_settings.interval_s * 1000u;
        if (interval_ms < 1000u) {
            interval_ms = 1000u;
        }

        vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }
}

/*
 * Task Buffer (Prioritas 2, Core 1, Event-Driven)
 * Penyangga rekaman telemetri offline ke memori flash saat koneksi terputus.
 */
static void task_buffer(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "task_buffer standby on Core %d (Prio %d)",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* =====================================================
   APPLICATION MAIN
   ===================================================== */

void app_main(void) {

    ESP_LOGW(
        TAG,
        "GMS INTERNAL REAL HARDWARE"
    );

    ESP_LOGW(
        TAG,
        "FREERTOS DUAL-CORE INITIALIZED (CORE 0 SAFETY & CORE 1 NETWORK)"
    );

    ESP_LOGW(
        TAG,
        "BENCH TEST MODE - ADS1115 A0=TEMP A2=LEVEL A3=PRESSURE"
    );

    /* VERIFIKASI PARTISI & JENDELA SWAUJI (SELF-TEST) OTA */
    gms_ota_self_test_check();

    /* INITIALIZE CONFIGURATION */

    gms_store_init();

    /* INITIALIZE HARDWARE */

    init_adc();

    init_rs485();

    /* INITIALIZE WIFI */

    wifi_start_apsta();

    /* START LOCAL WEB DASHBOARD */

    gms_web_start();

    /* =====================================================
       CORE 0: DETERMINISTIK & KESELAMATAN (Tabel 3 TA)
       ===================================================== */

    xTaskCreatePinnedToCore(
        task_safety,
        "task_safety",
        4096,
        NULL,
        10,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        task_sensor,
        "task_sensor",
        4096,
        NULL,
        5,
        NULL,
        0
    );

    xTaskCreatePinnedToCore(
        task_modbus,
        "task_modbus",
        4096,
        NULL,
        4,
        NULL,
        0
    );

    /* =====================================================
       CORE 1: JARINGAN, STORAGE & DATA (Tabel 3 TA)
       ===================================================== */

    xTaskCreatePinnedToCore(
        task_state,
        "task_state",
        4096,
        NULL,
        6,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        mqtt_network_task,
        "net_mqtt",
        6144,
        NULL,
        4,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        task_publish,
        "task_publish",
        6144,
        NULL,
        3,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        task_buffer,
        "task_buffer",
        4096,
        NULL,
        2,
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        task_ota,
        "task_ota",
        8192,
        NULL,
        1,
        NULL,
        1
    );
}