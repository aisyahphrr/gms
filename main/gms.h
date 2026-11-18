#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

// =====================================================
// GMS INTERNAL - ESP32-S3
// SENSOR CALIBRATION & THRESHOLD VERSION
// =====================================================

// =====================================================
// 1. WIFI, IP & MQTT SETTINGS
// =====================================================

typedef struct {
    char ssid[33];
    char wifi_password[65];

    bool static_ip;

    char ip[16];
    char gateway[16];
    char mask[16];
    char dns[16];

    char mqtt_host[97];
    char mqtt_token[161];

    uint16_t mqtt_port;
    uint16_t interval_s;

} gms_settings_t;

// MQTT kedua terpisah dari konfigurasi ThingsBoard utama.
typedef struct {
    bool enabled;
    char host[97];
    uint16_t port;
    char token[161];
} gms_mqtt2_config_t;

extern gms_mqtt2_config_t gms_mqtt2_settings;
void gms_mqtt2_get_config(gms_mqtt2_config_t *out);
esp_err_t gms_mqtt2_save_config(const gms_mqtt2_config_t *next);
bool gms_mqtt2_online(void);

// =====================================================
// 2. SENSOR CALIBRATION & THRESHOLD CONFIGURATION
// =====================================================

/*
 * Konfigurasi ini dapat diubah pengguna melalui dashboard.
 *
 * input_min_ma / input_max_ma:
 * Rentang sinyal arus transmitter.
 *
 * output_min / output_max:
 * Rentang nilai fisik sensor.
 *
 * critical_low / warning_low:
 * Batas peringatan bawah.
 *
 * warning_high / critical_high:
 * Batas peringatan atas.
 */

typedef struct {
    float input_min_ma;
    float input_max_ma;

    float output_min;
    float output_max;

    float warning_low;
    float critical_low;

    float warning_high;
    float critical_high;

} gms_sensor_config_t;

// =====================================================
// 3. CONFIGURATION FOR THREE ADS1115 SENSORS
// =====================================================

/*
 * Sensor index mapping:
 *
 * [0] Temperature -> ADS1115 A0
 * [1] Level       -> ADS1115 A2
 * [2] Pressure    -> ADS1115 A3
 */

#define GMS_SENSOR_COUNT 3

#define GMS_SENSOR_TEMP     0
#define GMS_SENSOR_LEVEL    1
#define GMS_SENSOR_PRESSURE 2

typedef struct {
    gms_sensor_config_t temperature;
    gms_sensor_config_t level;
    gms_sensor_config_t pressure;

} gms_sensor_settings_t;

// =====================================================
// 3A. TANK GEOMETRY & OPTIONAL TWO-POINT REFERENCE
// This config is independent of the physical 0-5 m HDL300 range.
// =====================================================

typedef struct {
    uint32_t version;
    float length_cm;
    float width_cm;
    float height_cm;
    bool use_two_point_reference;
    float zero_ma;            // observed air-free current
    float reference_ma;       // observed current at reference_depth_mm
    float reference_depth_mm; // verified water column above sensing port
} gms_tank_config_t;

void gms_tank_get_config(gms_tank_config_t *out);
esp_err_t gms_tank_save_config(const gms_tank_config_t *next);
// Converts physical level current into mm, fill % and liters. Returns false on invalid input.
bool gms_tank_measure(float current_ma, float *mm, float *percent, float *liters);

// =====================================================
// 4. SENSOR ALARM STATUS
// =====================================================

/*
 * SENSOR_ERROR:
 * ADC gagal atau sinyal loop tidak valid.
 *
 * NORMAL:
 * Nilai berada dalam batas normal.
 *
 * WARNING:
 * Nilai melewati warning_low atau warning_high.
 *
 * CRITICAL:
 * Nilai melewati critical_low atau critical_high.
 *
 * OUT_OF_RANGE:
 * Nilai arus berada di luar rentang input konfigurasi.
 *
 * Threshold hanya digunakan untuk monitoring.
 * Tidak digunakan untuk aktivasi relay/shutdown.
 */

typedef enum {
    GMS_ALARM_SENSOR_ERROR = 0,
    GMS_ALARM_NORMAL       = 1,
    GMS_ALARM_WARNING      = 2,
    GMS_ALARM_CRITICAL     = 3,
    GMS_ALARM_OUT_OF_RANGE = 4

} gms_alarm_status_t;

// =====================================================
// 5. MEASUREMENT DATA
// =====================================================

typedef struct {

    // ---------------------------------------------
    // Hardware status
    // ---------------------------------------------

    bool adc_ok[GMS_SENSOR_COUNT];
    bool loop_ok[GMS_SENSOR_COUNT];

    bool ewig_ok;

    // ---------------------------------------------
    // Raw ADC measurements
    // ---------------------------------------------

    float volts[GMS_SENSOR_COUNT];
    float ma[GMS_SENSOR_COUNT];

    int16_t raw[GMS_SENSOR_COUNT];

    // ---------------------------------------------
    // Calibrated / scaled engineering values
    // ---------------------------------------------

    /*
     * scaled[0] = Temperature (deg C)
     * scaled[1] = Level (%)
     * scaled[2] = Pressure (bar)
     *
     * Values are based on user-defined scaling.
     * They are not necessarily metrologically
     * calibrated measurements.
     */

    float scaled[GMS_SENSOR_COUNT];

    // ---------------------------------------------
    // Threshold evaluation status
    // ---------------------------------------------

    /*
     * alarm[0] = Temperature status
     * alarm[1] = Level status
     * alarm[2] = Pressure status
     */

    uint8_t alarm[GMS_SENSOR_COUNT];

    // ---------------------------------------------
    // Backward compatibility
    // ---------------------------------------------

    /*
     * Field lama tetap dipertahankan agar kode
     * yang masih menggunakan pressure_bar
     * tidak langsung mengalami error.
     *
     * Harus disinkronkan dengan scaled[2].
     */

    float pressure_bar;

    /* SENSOR SIMULATION (BENCH ONLY): one channel at a time, 120s timeout.
       Physical ADC readings remain available separately. */
    bool sensor_test_active;
    int sensor_test_channel;  /* -1 none; 0 TEMP A0; 1 LEVEL A2; 2 PRESSURE A3 */
    float sensor_test_ma;
    float sensor_real_ma[GMS_SENSOR_COUNT];
    float sensor_real_voltage[GMS_SENSOR_COUNT];
    uint32_t sensor_test_remaining_s;
    /* Retained for compatibility with the Tahap 8 pressure UI/API. */
    bool pressure_fault_test_active;
    float pressure_real_ma;
    float pressure_real_voltage;
    uint32_t pressure_fault_test_remaining_s;

    // ---------------------------------------------
    // EWIG Modbus RTU
    // ---------------------------------------------

    uint16_t ewig_raw[2];

    // ---------------------------------------------
    // System uptime
    // ---------------------------------------------

    uint32_t uptime_ms;

} gms_measurement_t;

// =====================================================
// 6. GLOBAL SETTINGS
// =====================================================

extern gms_settings_t gms_settings;

/*
 * Pertahankan deklarasi global agar kompatibel
 * dengan gms_store.c yang sudah kita buat.
 *
 * Untuk akses dari beberapa FreeRTOS task,
 * gunakan gms_sensor_get_settings().
 */

extern gms_sensor_settings_t gms_sensor_settings;

// =====================================================
// 7. SENSOR CONFIGURATION FUNCTIONS
// =====================================================

// Mengambil konfigurasi sensor aktif secara aman.
void gms_sensor_get_settings(
    gms_sensor_settings_t *out
);

// Memvalidasi dan menyimpan konfigurasi ke NVS.
esp_err_t gms_sensor_save_settings(
    const gms_sensor_settings_t *next
);

// Konversi arus mA menjadi nilai engineering.
float gms_sensor_scale(
    float current_ma,
    const gms_sensor_config_t *config
);

// =====================================================
// 8. NVS STORAGE FUNCTIONS
// =====================================================

void gms_store_init(void);

void gms_store_get_ap_password(
    char out[20]
);

bool gms_store_verify_admin(
    const char *password
);

esp_err_t gms_store_change_admin(
    const char *old_password,
    const char *new_password
);

esp_err_t gms_store_save_wifi(
    const gms_settings_t *next
);

esp_err_t gms_store_save_mqtt(
    const gms_settings_t *next
);

// =====================================================
// 9. MEASUREMENT SNAPSHOT
// =====================================================

void gms_get_snapshot(
    gms_measurement_t *out
);

// =====================================================
// 10. NETWORK STATUS
// =====================================================

bool gms_wifi_online(void);

bool gms_mqtt_online(void);

void gms_get_station_ip(
    char out[16]
);

void gms_get_ap_ssid(
    char out[40]
);

// =====================================================
// 11. SYSTEM CONTROL
// =====================================================

void gms_schedule_restart(void);

// =====================================================
// 12. WEB SERVER
// =====================================================

/* Volatile BENCH ONLY software injection (no physical current output).
   One selected sensor at a time; settings endpoint requires login + CSRF. */
bool gms_sensor_test_start(int channel, float ma);
void gms_sensor_test_stop(void);
bool gms_sensor_test_snapshot(int *channel, float *ma, uint32_t *remaining_s);

void gms_web_start(void);

/* BENCH ONLY: source-separated simulated genset parameters.
 * Never use these fields to actuate relays or safety protection. */
typedef struct {
    bool active;
    int state;
    double run_hours, maint_due_h;
    const char *service_status;
    double rpm_calc, battery_v, freq_hz, load_pct;
    double v_l1, v_l2, v_l3, v_l12, v_l23, v_l31;
    double i_l1, i_l2, i_l3;
    double p_kw, q_kvar, s_kva, pf, fuel_est_l;
} gms_bench_data_t;
void gms_bench_snapshot(gms_bench_data_t *out);
