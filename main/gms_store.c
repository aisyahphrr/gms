#include "gms.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

#include "esp_log.h"
#include "esp_random.h"
#include "bootloader_random.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "psa/crypto.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

// =====================================================
// GLOBAL CONFIGURATION
// =====================================================

static const char *TAG = "GMS_CFG";

gms_settings_t gms_settings;

gms_mqtt2_config_t gms_mqtt2_settings;

gms_sensor_settings_t gms_sensor_settings;

static char s_ap_pw[20];

static uint8_t s_admin_salt[16];
static uint8_t s_admin_digest[32];

// Protect sensor settings shared by FreeRTOS tasks.
static portMUX_TYPE s_sensor_mux = portMUX_INITIALIZER_UNLOCKED;

// =====================================================
// DEFAULT SENSOR CONFIGURATION
// =====================================================

/*
 * BENCH TEST DEFAULT VALUES
 *
 * These are demonstration settings only.
 * They are NOT validated industrial safety limits.
 *
 * Temperature and level measurement ranges must be
 * adjusted based on the actual sensor specifications.
 */

static const gms_sensor_settings_t sensor_defaults = {

    // TEMPERATURE - ADS1115 A0
    .temperature = {
        .input_min_ma = 4.0f,
        .input_max_ma = 20.0f,

        .output_min = 0.0f,
        .output_max = 100.0f,

        .warning_low = 10.0f,
        .critical_low = 5.0f,

        .warning_high = 70.0f,
        .critical_high = 90.0f
    },

    // LEVEL - ADS1115 A2
    .level = {
        .input_min_ma = 4.0f,
        .input_max_ma = 20.0f,

        .output_min = 0.0f,
        .output_max = 100.0f,

        .warning_low = 20.0f,
        .critical_low = 10.0f,

        .warning_high = 80.0f,
        .critical_high = 95.0f
    },

    // PRESSURE - ADS1115 A3
    .pressure = {
        .input_min_ma = 4.0f,
        .input_max_ma = 20.0f,

        .output_min = 0.0f,
        .output_max = 10.0f,

        .warning_low = 2.0f,
        .critical_low = 1.0f,

        .warning_high = 7.0f,
        .critical_high = 9.0f
    }
};

// =====================================================
// FUNCTION DECLARATIONS
// =====================================================

static void gms_sensor_load_settings(void);
static void gms_tank_load_config(void);

static bool gms_sensor_config_valid(
    const gms_sensor_config_t *config
);

static bool gms_sensor_settings_valid(
    const gms_sensor_settings_t *settings
);

// =====================================================
// RANDOM PASSWORD GENERATOR
// =====================================================

static void rand_alnum(char *out, size_t len)
{
    const char *charset =
        "ABCDEFGHJKLMNPQRSTUVWXYZ"
        "abcdefghijkmnopqrstuvwxyz"
        "23456789";

    const size_t base = strlen(charset);

    for (size_t i = 0; i < len; i++) {
        out[i] = charset[esp_random() % base];
    }

    out[len] = '\0';
}

// =====================================================
// PASSWORD HASH
// =====================================================

static void digest_password(
    const char *password,
    const uint8_t salt[16],
    uint8_t output[32]
)
{
    uint8_t buf[16 + 128];

    size_t n = strlen(password);

    if (n > 128) {
        n = 128;
    }

    memcpy(buf, salt, 16);
    memcpy(buf + 16, password, n);

    size_t hash_len = 0;

    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        buf,
        n + 16,
        output,
        32,
        &hash_len
    );

    if (status != PSA_SUCCESS || hash_len != 32) {

        ESP_LOGE(
            TAG,
            "SHA256 failed: %d",
            (int)status
        );

        memset(buf, 0, sizeof(buf));
        abort();
    }

    memset(buf, 0, sizeof(buf));
}

// =====================================================
// CONSTANT TIME PASSWORD COMPARISON
// =====================================================

static bool constant_equal(
    const uint8_t *a,
    const uint8_t *b,
    size_t n
)
{
    uint8_t diff = 0;

    for (size_t i = 0; i < n; i++) {
        diff |= a[i] ^ b[i];
    }

    return diff == 0;
}

// =====================================================
// NVS STRING HELPER
// =====================================================

static void get_str(
    nvs_handle_t h,
    const char *key,
    char *dest,
    size_t capacity
)
{
    size_t n = capacity;

    if (nvs_get_str(h, key, dest, &n) != ESP_OK) {
        dest[0] = '\0';
    }
}

// =====================================================
// SENSOR CONFIGURATION VALIDATION
// =====================================================

static bool gms_sensor_config_valid(
    const gms_sensor_config_t *c
)
{
    if (c == NULL) {
        return false;
    }

    const float values[] = {
        c->input_min_ma,
        c->input_max_ma,

        c->output_min,
        c->output_max,

        c->warning_low,
        c->critical_low,

        c->warning_high,
        c->critical_high
    };

    for (
        size_t i = 0;
        i < sizeof(values) / sizeof(values[0]);
        i++
    ) {
        if (!isfinite(values[i])) {
            return false;
        }
    }

    // Input current range validation
    if (
        c->input_min_ma < 0.0f ||
        c->input_max_ma > 25.0f ||
        c->input_min_ma >= c->input_max_ma
    ) {
        return false;
    }

    // Physical output range validation
    if (c->output_min >= c->output_max) {
        return false;
    }

    // Threshold ordering validation
    if (!(
        c->output_min <= c->critical_low &&
        c->critical_low < c->warning_low &&
        c->warning_low < c->warning_high &&
        c->warning_high < c->critical_high &&
        c->critical_high <= c->output_max
    )) {
        return false;
    }

    return true;
}

static bool gms_sensor_settings_valid(
    const gms_sensor_settings_t *settings
)
{
    if (settings == NULL) {
        return false;
    }

    return (
        gms_sensor_config_valid(&settings->temperature) &&
        gms_sensor_config_valid(&settings->level) &&
        gms_sensor_config_valid(&settings->pressure)
    );
}

// =====================================================
// SENSOR CONFIGURATION: GET
// =====================================================

void gms_sensor_get_settings(
    gms_sensor_settings_t *out
)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_sensor_mux);

    *out = gms_sensor_settings;

    taskEXIT_CRITICAL(&s_sensor_mux);
}

// =====================================================
// SENSOR CURRENT SCALING
// =====================================================

float gms_sensor_scale(
    float current_ma,
    const gms_sensor_config_t *config
)
{
    if (
        config == NULL ||
        !isfinite(current_ma)
    ) {
        return NAN;
    }

    float input_range =
        config->input_max_ma -
        config->input_min_ma;

    if (input_range <= 0.0f) {
        return NAN;
    }

    float ratio =
        (current_ma - config->input_min_ma) /
        input_range;

    float output =
        config->output_min +
        ratio *
        (config->output_max - config->output_min);

    return output;
}

// =====================================================
// SENSOR CONFIGURATION: LOAD FROM NVS
// =====================================================

static void gms_sensor_load_settings(void)
{
    // Begin with safe demonstration defaults.
    taskENTER_CRITICAL(&s_sensor_mux);

    gms_sensor_settings = sensor_defaults;

    taskEXIT_CRITICAL(&s_sensor_mux);

    nvs_handle_t h;

    esp_err_t er = nvs_open(
        "gmssensor",
        NVS_READONLY,
        &h
    );

    if (er == ESP_ERR_NVS_NOT_FOUND) {

        ESP_LOGI(
            TAG,
            "Sensor configuration not found; using defaults"
        );

        return;
    }

    if (er != ESP_OK) {

        ESP_LOGW(
            TAG,
            "Cannot open sensor NVS: %s",
            esp_err_to_name(er)
        );

        return;
    }

    gms_sensor_settings_t loaded = {0};

    size_t size = sizeof(loaded);

    er = nvs_get_blob(
        h,
        "config_v1",
        &loaded,
        &size
    );

    nvs_close(h);

    if (
        er == ESP_OK &&
        size == sizeof(loaded) &&
        gms_sensor_settings_valid(&loaded)
    ) {

        taskENTER_CRITICAL(&s_sensor_mux);

        gms_sensor_settings = loaded;

        taskEXIT_CRITICAL(&s_sensor_mux);

        ESP_LOGI(
            TAG,
            "Sensor calibration and threshold loaded from NVS"
        );

    } else {

        ESP_LOGW(
            TAG,
            "Sensor config missing/invalid; using defaults"
        );
    }
}

// =====================================================
// SENSOR CONFIGURATION: SAVE TO NVS
// =====================================================

esp_err_t gms_sensor_save_settings(
    const gms_sensor_settings_t *next
)
{
    if (!gms_sensor_settings_valid(next)) {

        ESP_LOGW(
            TAG,
            "Rejected invalid sensor configuration"
        );

        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;

    esp_err_t er = nvs_open(
        "gmssensor",
        NVS_READWRITE,
        &h
    );

    if (er != ESP_OK) {
        return er;
    }

    er = nvs_set_blob(
        h,
        "config_v1",
        next,
        sizeof(*next)
    );

    if (er == ESP_OK) {
        er = nvs_commit(h);
    }

    nvs_close(h);

    if (er == ESP_OK) {

        taskENTER_CRITICAL(&s_sensor_mux);

        gms_sensor_settings = *next;

        taskEXIT_CRITICAL(&s_sensor_mux);

        ESP_LOGI(
            TAG,
            "Sensor calibration and threshold saved to NVS"
        );

    } else {

        ESP_LOGE(
            TAG,
            "Failed to save sensor configuration: %s",
            esp_err_to_name(er)
        );
    }

    return er;
}

// =====================================================
// STORE INITIALIZATION
// =====================================================

void gms_store_init(void)
{
    esp_err_t er = nvs_flash_init();

    if (
        er == ESP_ERR_NVS_NO_FREE_PAGES ||
        er == ESP_ERR_NVS_NEW_VERSION_FOUND
    ) {

        ESP_LOGW(
            TAG,
            "NVS partition incompatible; resetting NVS for bench provisioning"
        );

        ESP_ERROR_CHECK(nvs_flash_erase());

        er = nvs_flash_init();
    }

    ESP_ERROR_CHECK(er);

    // Enable entropy source before Wi-Fi initialization.
    bootloader_random_enable();

    nvs_handle_t h;

    ESP_ERROR_CHECK(
        nvs_open("gmscfg", NVS_READWRITE, &h)
    );

    memset(
        &gms_settings,
        0,
        sizeof(gms_settings)
    );

    // =================================================
    // LOAD WIFI CONFIGURATION
    // =================================================

    get_str(
        h,
        "ssid",
        gms_settings.ssid,
        sizeof(gms_settings.ssid)
    );

    get_str(
        h,
        "wifi_pw",
        gms_settings.wifi_password,
        sizeof(gms_settings.wifi_password)
    );

    get_str(
        h,
        "ip",
        gms_settings.ip,
        sizeof(gms_settings.ip)
    );

    get_str(
        h,
        "gw",
        gms_settings.gateway,
        sizeof(gms_settings.gateway)
    );

    get_str(
        h,
        "mask",
        gms_settings.mask,
        sizeof(gms_settings.mask)
    );

    get_str(
        h,
        "dns",
        gms_settings.dns,
        sizeof(gms_settings.dns)
    );

    // =================================================
    // LOAD MQTT CONFIGURATION
    // =================================================

    get_str(
        h,
        "host",
        gms_settings.mqtt_host,
        sizeof(gms_settings.mqtt_host)
    );

    get_str(
        h,
        "token",
        gms_settings.mqtt_token,
        sizeof(gms_settings.mqtt_token)
    );

    uint8_t mode = 0;

    nvs_get_u8(h, "static", &mode);

    gms_settings.static_ip = (mode != 0);

    uint16_t val = 0;

    if (nvs_get_u16(h, "port", &val) == ESP_OK) {
        gms_settings.mqtt_port = val;
    } else {
        gms_settings.mqtt_port = 8883;
    }

    val = 0;

    if (
        nvs_get_u16(h, "interval", &val) == ESP_OK &&
        val >= 5 &&
        val <= 3600
    ) {
        gms_settings.interval_s = val;
    } else {
        gms_settings.interval_s = 5;
    }

    if (!gms_settings.mqtt_host[0]) {

        strcpy(
            gms_settings.mqtt_host,
            "mqtt.thingsboard.cloud"
        );
    }

    // =================================================
    // ACCESS POINT PASSWORD
    // =================================================

    get_str(
        h,
        "ap_pw",
        s_ap_pw,
        sizeof(s_ap_pw)
    );

    if (!s_ap_pw[0]) {

        rand_alnum(s_ap_pw, 14);

        ESP_ERROR_CHECK(
            nvs_set_str(h, "ap_pw", s_ap_pw)
        );
    }

    // =================================================
    // ADMIN CREDENTIALS
    // =================================================

    size_t n = sizeof(s_admin_salt);

    esp_err_t e1 = nvs_get_blob(
        h,
        "adm_salt",
        s_admin_salt,
        &n
    );

    n = sizeof(s_admin_digest);

    esp_err_t e2 = nvs_get_blob(
        h,
        "adm_hash",
        s_admin_digest,
        &n
    );

    if (
        e1 != ESP_OK ||
        e2 != ESP_OK
    ) {

        char pw[21];

        rand_alnum(pw, 18);

        esp_fill_random(
            s_admin_salt,
            sizeof(s_admin_salt)
        );

        digest_password(
            pw,
            s_admin_salt,
            s_admin_digest
        );

        ESP_ERROR_CHECK(
            nvs_set_blob(
                h,
                "adm_salt",
                s_admin_salt,
                sizeof(s_admin_salt)
            )
        );

        ESP_ERROR_CHECK(
            nvs_set_blob(
                h,
                "adm_hash",
                s_admin_digest,
                sizeof(s_admin_digest)
            )
        );

        ESP_LOGW(
            TAG,
            "================= FIRST BOOT ================="
        );

        ESP_LOGW(
            TAG,
            "GMS WEB username: admin"
        );

        ESP_LOGW(
            TAG,
            "GMS WEB initial password: %s",
            pw
        );

        ESP_LOGW(
            TAG,
            "SAVE password NOW; not logged again after reboot"
        );

        ESP_LOGW(
            TAG,
            "=============================================="
        );

        memset(pw, 0, sizeof(pw));
    }

    // Commit Wi-Fi, MQTT and admin configuration.
    ESP_ERROR_CHECK(nvs_commit(h));

    nvs_close(h);

    // =================================================
    // LOAD SENSOR CALIBRATION AND THRESHOLD
    // =================================================

    // MQTT 2 disimpan di namespace terpisah agar MQTT 1 tidak berubah.
    memset(&gms_mqtt2_settings, 0, sizeof(gms_mqtt2_settings));
    strcpy(gms_mqtt2_settings.host, "178.128.107.238");
    gms_mqtt2_settings.port = 1886;
    nvs_handle_t m2;
    if (nvs_open("gmsmqtt2", NVS_READONLY, &m2) == ESP_OK) {
        get_str(m2, "host", gms_mqtt2_settings.host,
                sizeof(gms_mqtt2_settings.host));
        if (!gms_mqtt2_settings.host[0])
            strcpy(gms_mqtt2_settings.host, "178.128.107.238");
        get_str(m2, "token", gms_mqtt2_settings.token,
                sizeof(gms_mqtt2_settings.token));
        uint16_t port2 = 0;
        if (nvs_get_u16(m2, "port", &port2) == ESP_OK && port2)
            gms_mqtt2_settings.port = port2;
        uint8_t enabled2 = 0;
        nvs_get_u8(m2, "enabled", &enabled2);
        gms_mqtt2_settings.enabled = enabled2 != 0;
        nvs_close(m2);
    }

    gms_sensor_load_settings();
    gms_tank_load_config();

    bootloader_random_disable();

    ESP_LOGI(
        TAG,
        "GMS configuration initialization completed"
    );
}

// =====================================================
// GET ACCESS POINT PASSWORD
// =====================================================

void gms_store_get_ap_password(char out[20])
{
    strcpy(out, s_ap_pw);
}

// =====================================================
// VERIFY ADMIN PASSWORD
// =====================================================

bool gms_store_verify_admin(
    const char *password
)
{
    if (
        password == NULL ||
        strlen(password) > 128
    ) {
        return false;
    }

    uint8_t digest[32];

    digest_password(
        password,
        s_admin_salt,
        digest
    );

    bool ok = constant_equal(
        digest,
        s_admin_digest,
        sizeof(digest)
    );

    memset(
        digest,
        0,
        sizeof(digest)
    );

    return ok;
}

// =====================================================
// CHANGE ADMIN PASSWORD
// =====================================================

esp_err_t gms_store_change_admin(
    const char *old_password,
    const char *new_password
)
{
    if (
        old_password == NULL ||
        new_password == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!gms_store_verify_admin(old_password)) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t len = strlen(new_password);

    if (
        len < 12 ||
        len > 64
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t new_salt[16];
    uint8_t new_digest[32];

    esp_fill_random(
        new_salt,
        sizeof(new_salt)
    );

    digest_password(
        new_password,
        new_salt,
        new_digest
    );

    nvs_handle_t h;

    esp_err_t er = nvs_open(
        "gmscfg",
        NVS_READWRITE,
        &h
    );

    if (er != ESP_OK) {
        return er;
    }

    er = nvs_set_blob(
        h,
        "adm_salt",
        new_salt,
        sizeof(new_salt)
    );

    if (er == ESP_OK) {

        er = nvs_set_blob(
            h,
            "adm_hash",
            new_digest,
            sizeof(new_digest)
        );
    }

    if (er == ESP_OK) {
        er = nvs_commit(h);
    }

    nvs_close(h);

    if (er == ESP_OK) {

        memcpy(
            s_admin_salt,
            new_salt,
            sizeof(new_salt)
        );

        memcpy(
            s_admin_digest,
            new_digest,
            sizeof(new_digest)
        );
    }

    memset(new_salt, 0, sizeof(new_salt));
    memset(new_digest, 0, sizeof(new_digest));

    return er;
}

// =====================================================
// SAVE WIFI CONFIGURATION
// =====================================================

esp_err_t gms_store_save_wifi(
    const gms_settings_t *next
)
{
    if (next == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;

    esp_err_t er = nvs_open(
        "gmscfg",
        NVS_READWRITE,
        &h
    );

    if (er != ESP_OK) {
        return er;
    }

    er = nvs_set_str(
        h,
        "ssid",
        next->ssid
    );

    if (er == ESP_OK) {
        er = nvs_set_str(
            h,
            "wifi_pw",
            next->wifi_password
        );
    }

    if (er == ESP_OK) {
        er = nvs_set_u8(
            h,
            "static",
            next->static_ip ? 1 : 0
        );
    }

    if (er == ESP_OK) {
        er = nvs_set_str(
            h,
            "ip",
            next->ip
        );
    }

    if (er == ESP_OK) {
        er = nvs_set_str(
            h,
            "gw",
            next->gateway
        );
    }

    if (er == ESP_OK) {
        er = nvs_set_str(
            h,
            "mask",
            next->mask
        );
    }

    if (er == ESP_OK) {
        er = nvs_set_str(
            h,
            "dns",
            next->dns
        );
    }

    if (er == ESP_OK) {
        er = nvs_commit(h);
    }

    nvs_close(h);

    if (er == ESP_OK) {

        strcpy(
            gms_settings.ssid,
            next->ssid
        );

        strcpy(
            gms_settings.wifi_password,
            next->wifi_password
        );

        gms_settings.static_ip = next->static_ip;

        strcpy(gms_settings.ip, next->ip);
        strcpy(gms_settings.gateway, next->gateway);
        strcpy(gms_settings.mask, next->mask);
        strcpy(gms_settings.dns, next->dns);
    }

    return er;
}

// =====================================================
// SAVE MQTT CONFIGURATION
// =====================================================

esp_err_t gms_store_save_mqtt(
    const gms_settings_t *next
)
{
    if (next == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;

    esp_err_t er = nvs_open(
        "gmscfg",
        NVS_READWRITE,
        &h
    );

    if (er != ESP_OK) {
        return er;
    }

    er = nvs_set_str(
        h,
        "host",
        next->mqtt_host
    );

    if (er == ESP_OK) {
        er = nvs_set_str(
            h,
            "token",
            next->mqtt_token
        );
    }

    if (er == ESP_OK) {
        er = nvs_set_u16(
            h,
            "port",
            next->mqtt_port
        );
    }

    if (er == ESP_OK) {
        er = nvs_set_u16(
            h,
            "interval",
            next->interval_s
        );
    }

    if (er == ESP_OK) {
        er = nvs_commit(h);
    }

    nvs_close(h);

    if (er == ESP_OK) {

        strcpy(
            gms_settings.mqtt_host,
            next->mqtt_host
        );

        strcpy(
            gms_settings.mqtt_token,
            next->mqtt_token
        );

        gms_settings.mqtt_port = next->mqtt_port;
        gms_settings.interval_s = next->interval_s;
    }

    return er;
}

void gms_mqtt2_get_config(gms_mqtt2_config_t *out)
{
    if (out) *out = gms_mqtt2_settings;
}

esp_err_t gms_mqtt2_save_config(const gms_mqtt2_config_t *next)
{
    if (!next || !next->host[0] || next->port == 0 ||
        (next->enabled && !next->token[0])) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open("gmsmqtt2", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "host", next->host);
    if (err == ESP_OK) err = nvs_set_str(h, "token", next->token);
    if (err == ESP_OK) err = nvs_set_u16(h, "port", next->port);
    if (err == ESP_OK) err = nvs_set_u8(h, "enabled", next->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) gms_mqtt2_settings = *next;
    return err;
}


// =====================================================
// TANK BENCH CONFIG — independent NVS namespace (gms_tank).
// Dimensions are INSIDE dimensions of a rectangular tank.
// Two point calibration disabled until stable reference values are measured.
// =====================================================

static const gms_tank_config_t s_tank_defaults = {
    .version = 1,
    .length_cm = 25.0f,
    .width_cm = 17.0f,
    .height_cm = 20.0f,
    .use_two_point_reference = false,
    .zero_ma = 4.000f,
    .reference_ma = 4.480f,
    .reference_depth_mm = 150.0f
};
static gms_tank_config_t s_tank;
static portMUX_TYPE s_tank_mux = portMUX_INITIALIZER_UNLOCKED;

static bool tank_valid(const gms_tank_config_t *t)
{
    if (!t || t->version != 1) return false;
    const float v[] = {t->length_cm,t->width_cm,t->height_cm,
                       t->zero_ma,t->reference_ma,t->reference_depth_mm};
    for (size_t i=0; i<sizeof(v)/sizeof(v[0]); ++i)
        if (!isfinite(v[i])) return false;
    if (t->length_cm <= 0.0f || t->length_cm > 1000.0f ||
        t->width_cm <= 0.0f || t->width_cm > 1000.0f ||
        t->height_cm <= 0.0f || t->height_cm > 500.0f) return false;
    if (t->zero_ma < 3.6f || t->zero_ma > 20.0f ||
        t->reference_ma < 3.6f || t->reference_ma > 21.0f ||
        t->reference_ma <= t->zero_ma + 0.050f ||
        t->reference_depth_mm < 10.0f ||
        t->reference_depth_mm > 5000.0f ||
        (t->use_two_point_reference &&
         t->reference_depth_mm > (t->height_cm * 10.0f))) return false;
    return true;
}

void gms_tank_get_config(gms_tank_config_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_tank_mux);
    *out = s_tank;
    taskEXIT_CRITICAL(&s_tank_mux);
}

static void gms_tank_load_config(void)
{
    s_tank = s_tank_defaults;
    nvs_handle_t h;
    if (nvs_open("gmstank", NVS_READONLY, &h) != ESP_OK) return;
    gms_tank_config_t readback = {0};
    size_t n = sizeof(readback);
    esp_err_t er = nvs_get_blob(h, "cfg_v1", &readback, &n);
    nvs_close(h);
    if (er == ESP_OK && n == sizeof(readback) && tank_valid(&readback)) {
        taskENTER_CRITICAL(&s_tank_mux);
        s_tank = readback;
        taskEXIT_CRITICAL(&s_tank_mux);
        ESP_LOGI(TAG, "Tank geometry loaded: %.1f x %.1f x %.1f cm",
                 (double)readback.length_cm,(double)readback.width_cm,
                 (double)readback.height_cm);
    }
}

esp_err_t gms_tank_save_config(const gms_tank_config_t *next)
{
    if (!tank_valid(next)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t er = nvs_open("gmstank", NVS_READWRITE, &h);
    if (er != ESP_OK) return er;
    er = nvs_set_blob(h, "cfg_v1", next, sizeof(*next));
    if (er == ESP_OK) er = nvs_commit(h);
    nvs_close(h);
    if (er == ESP_OK) {
        taskENTER_CRITICAL(&s_tank_mux);
        s_tank = *next;
        taskEXIT_CRITICAL(&s_tank_mux);
    }
    return er;
}

bool gms_tank_measure(float current_ma, float *mm, float *percent, float *liters)
{
    if (!isfinite(current_ma) || current_ma < 3.8f || current_ma > 20.5f)
        return false;
    gms_tank_config_t cfg;
    gms_tank_get_config(&cfg);
    if (!tank_valid(&cfg)) return false;

    // Sensor's factory nominal transfer: 4-20mA = 0-5000 mm.
    // OPTIONAL two-point correction maps measured currents onto real water depth.
    float depth = cfg.use_two_point_reference
        ? (current_ma - cfg.zero_ma) * cfg.reference_depth_mm /
          (cfg.reference_ma - cfg.zero_ma)
        : (current_ma - 4.0f) * (5000.0f / 16.0f);
    if (!isfinite(depth)) return false;

    // Clamp tank fill, never claim an impossible negative / overfull volume.
    const float height_mm = cfg.height_cm * 10.0f;
    depth = fmaxf(0.0f, fminf(height_mm, depth));
    if (mm) *mm = depth;
    if (percent) *percent = 100.0f * depth / height_mm;
    if (liters) *liters = cfg.length_cm * cfg.width_cm * (depth / 10.0f) / 1000.0f;
    return true;
}
