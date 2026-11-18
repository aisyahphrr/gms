#include "gms_calc.h"
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "GMS_CALC";

#define NVS_NAMESPACE          "gms_state"
#define NVS_KEY_RUN_SEC        "run_sec"
#define NVS_KEY_KWH_CALC       "kwh_calc"

#define DEFAULT_RATED_KVA      200.0f
#define DEFAULT_MAINT_INTERVAL 250.0f  /* Interval servis 250 jam */
#define DEFAULT_POLE_PAIRS     2.0f    /* Generator 4 kutub (1500 RPM @ 50 Hz) */

static portMUX_TYPE s_calc_lock = portMUX_INITIALIZER_UNLOCKED;
static gms_derived_metrics_t s_metrics;

static uint32_t s_run_seconds = 0;
static uint32_t s_nvs_save_counter = 0;
static uint8_t  s_running_confirm_count = 0;
static uint8_t  s_stopped_confirm_count = 0;

static const char *STATE_NAMES[] = {
    "STOPPED",
    "STARTING",
    "RUNNING",
    "STOPPING",
    "FAULT"
};

void gms_calc_init(void) {
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_metrics.state = GMS_ENGINE_STOPPED;
    s_metrics.state_str = STATE_NAMES[0];
    s_metrics.maint_due_h = DEFAULT_MAINT_INTERVAL;

    /* Muat akumulasi jam kerja dan kWh dari NVS */
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint32_t sec = 0;
        if (nvs_get_u32(handle, NVS_KEY_RUN_SEC, &sec) == ESP_OK) {
            s_run_seconds = sec;
            s_metrics.run_hours = (float)sec / 3600.0f;
            float rem = fmodf(s_metrics.run_hours, DEFAULT_MAINT_INTERVAL);
            s_metrics.maint_due_h = DEFAULT_MAINT_INTERVAL - rem;
            ESP_LOGI(TAG, "NVS loaded: Hour Meter = %.2f h (%lu s)",
                     s_metrics.run_hours, (unsigned long)sec);
        }

        uint32_t kwh_raw = 0;
        if (nvs_get_u32(handle, NVS_KEY_KWH_CALC, &kwh_raw) == ESP_OK) {
            s_metrics.e_kwh_calc = (float)kwh_raw / 100.0f;
            ESP_LOGI(TAG, "NVS loaded: E_kWh_calc = %.2f kWh", s_metrics.e_kwh_calc);
        }
        nvs_close(handle);
    } else {
        ESP_LOGI(TAG, "NVS state belum ada, menginisialisasi dari 0");
    }
}

static void save_state_to_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, NVS_KEY_RUN_SEC, s_run_seconds);
        uint32_t kwh_raw = (uint32_t)(s_metrics.e_kwh_calc * 100.0f);
        nvs_set_u32(handle, NVS_KEY_KWH_CALC, kwh_raw);
        nvs_commit(handle);
        nvs_close(handle);
        ESP_LOGD(TAG, "State tersimpan ke NVS: %.2f h", (float)s_run_seconds / 3600.0f);
    }
}

static float calc_nema_unbalance(float v1, float v2, float v3) {
    float avg = (v1 + v2 + v3) / 3.0f;
    if (avg < 1.0f) return 0.0f;

    float dev1 = fabsf(v1 - avg);
    float dev2 = fabsf(v2 - avg);
    float dev3 = fabsf(v3 - avg);

    float max_dev = dev1;
    if (dev2 > max_dev) max_dev = dev2;
    if (dev3 > max_dev) max_dev = dev3;

    return (max_dev / avg) * 100.0f;
}

void gms_calc_update_1s(float v_l1, float v_l2, float v_l3,
                       float i_l1, float i_l2, float i_l3,
                       float p_kw, float freq_hz,
                       float oil_press_bar, bool critical_alarm) {

    /* 1. Evaluasi Finite State Machine (FSM) */
    float v_avg = (v_l1 + v_l2 + v_l3) / 3.0f;
    bool freq_valid = (freq_hz >= 45.0f && freq_hz <= 55.0f);
    bool volt_valid = (v_avg >= 180.0f);
    bool engine_spinning = (freq_valid && volt_valid) || (oil_press_bar >= 1.0f);

    gms_engine_state_t next_state = s_metrics.state;

    if (critical_alarm) {
        next_state = GMS_ENGINE_FAULT;
        s_running_confirm_count = 0;
        s_stopped_confirm_count = 0;
    } else {
        switch (s_metrics.state) {
            case GMS_ENGINE_STOPPED:
                if (engine_spinning) {
                    next_state = GMS_ENGINE_STARTING;
                    s_running_confirm_count = 1;
                }
                break;

            case GMS_ENGINE_STARTING:
                if (engine_spinning) {
                    if (++s_running_confirm_count >= 5) { /* 5 detik stabil */
                        next_state = GMS_ENGINE_RUNNING;
                        s_running_confirm_count = 0;
                    }
                } else {
                    next_state = GMS_ENGINE_STOPPED;
                    s_running_confirm_count = 0;
                }
                break;

            case GMS_ENGINE_RUNNING:
                if (!engine_spinning && (v_avg < 50.0f || oil_press_bar < 0.5f)) {
                    next_state = GMS_ENGINE_STOPPING;
                    s_stopped_confirm_count = 1;
                }
                break;

            case GMS_ENGINE_STOPPING:
                if (!engine_spinning) {
                    if (++s_stopped_confirm_count >= 10) { /* 10 detik berhenti */
                        next_state = GMS_ENGINE_STOPPED;
                        s_stopped_confirm_count = 0;
                    }
                } else {
                    next_state = GMS_ENGINE_RUNNING;
                    s_stopped_confirm_count = 0;
                }
                break;

            case GMS_ENGINE_FAULT:
                if (!critical_alarm && !engine_spinning) {
                    next_state = GMS_ENGINE_STOPPED;
                }
                break;
        }
    }

    /* 2. Akumulasi Jam Kerja (Hour Meter) & Energi Aktif */
    if (next_state == GMS_ENGINE_RUNNING) {
        s_run_seconds++;
        if (p_kw > 0.0f) {
            s_metrics.e_kwh_calc += (p_kw / 3600.0f);
        }

        /* Simpan ke NVS setiap 60 detik */
        if (++s_nvs_save_counter >= 60) {
            s_nvs_save_counter = 0;
            save_state_to_nvs();
        }
    }

    float run_h = (float)s_run_seconds / 3600.0f;
    float rem_maint = DEFAULT_MAINT_INTERVAL - fmodf(run_h, DEFAULT_MAINT_INTERVAL);

    /* 3. Kalkulasi Turunan 3 Fasa */
    float s_kva = 0.0f;
    if (v_l1 > 10.0f && i_l1 >= 0.0f) s_kva += (v_l1 * i_l1) / 1000.0f;
    if (v_l2 > 10.0f && i_l2 >= 0.0f) s_kva += (v_l2 * i_l2) / 1000.0f;
    if (v_l3 > 10.0f && i_l3 >= 0.0f) s_kva += (v_l3 * i_l3) / 1000.0f;

    float load_pct = (s_kva / DEFAULT_RATED_KVA) * 100.0f;
    float v_unbal = calc_nema_unbalance(v_l1, v_l2, v_l3);
    float i_unbal = calc_nema_unbalance(i_l1, i_l2, i_l3);
    float rpm = (freq_hz > 1.0f) ? (freq_hz * 60.0f / DEFAULT_POLE_PAIRS) : 0.0f;

    /* 4. Perbarui shared struct */
    portENTER_CRITICAL(&s_calc_lock);
    s_metrics.state = next_state;
    s_metrics.state_str = STATE_NAMES[next_state];
    s_metrics.run_hours = run_h;
    s_metrics.maint_due_h = rem_maint;
    s_metrics.s_kva = s_kva;
    s_metrics.load_pct = load_pct;
    s_metrics.v_unbal_pct = v_unbal;
    s_metrics.i_unbal_pct = i_unbal;
    s_metrics.rpm_calc = rpm;
    portEXIT_CRITICAL(&s_calc_lock);
}

void gms_calc_get_metrics(gms_derived_metrics_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_calc_lock);
    *out = s_metrics;
    portEXIT_CRITICAL(&s_calc_lock);
}
