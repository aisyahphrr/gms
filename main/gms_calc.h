#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GMS_ENGINE_STOPPED  = 0,
    GMS_ENGINE_STARTING = 1,
    GMS_ENGINE_RUNNING  = 2,
    GMS_ENGINE_STOPPING = 3,
    GMS_ENGINE_FAULT    = 4
} gms_engine_state_t;

typedef struct {
    gms_engine_state_t state;
    const char *state_str;
    float run_hours;
    float maint_due_h;
    float s_kva;
    float load_pct;
    float v_unbal_pct;
    float i_unbal_pct;
    float rpm_calc;
    float e_kwh_calc;
} gms_derived_metrics_t;

/**
 * @brief Inisialisasi modul kalkulasi dan memuat akumulasi hour meter dari NVS.
 */
void gms_calc_init(void);

/**
 * @brief Memperbarui mesin status (FSM), hour meter, dan kalkulasi turunan 3 fasa.
 * Dipanggil periodik 1 detik oleh task_state di Core 1.
 * 
 * @param v_l1 Tegangan fasa 1 (V)
 * @param v_l2 Tegangan fasa 2 (V)
 * @param v_l3 Tegangan fasa 3 (V)
 * @param i_l1 Arus fasa 1 (A)
 * @param i_l2 Arus fasa 2 (A)
 * @param i_l3 Arus fasa 3 (A)
 * @param p_kw Daya aktif (kW)
 * @param freq_hz Frekuensi (Hz)
 * @param oil_press_bar Tekanan oli (bar)
 * @param critical_alarm True jika ada alarm kritis
 */
void gms_calc_update_1s(float v_l1, float v_l2, float v_l3,
                       float i_l1, float i_l2, float i_l3,
                       float p_kw, float freq_hz,
                       float oil_press_bar, bool critical_alarm);

/**
 * @brief Mengambil salinan thread-safe dari hasil kalkulasi dan status mesin terbaru.
 */
void gms_calc_get_metrics(gms_derived_metrics_t *out);

#ifdef __cplusplus
}
#endif
