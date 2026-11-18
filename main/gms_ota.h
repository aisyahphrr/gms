#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inisialisasi dan verifikasi jendela swauji (Self-Test) 60 detik pasca-reboot OTA.
 * Mengacu pada Subbab I.6.4 dokumen TA GMS:
 * Jika firmware berjalan pada partisi baru berstatus ESP_OTA_IMG_PENDING_VERIFY,
 * fungsi ini memantau kesiapan Wi-Fi/MQTT, ADC I2C, dan Modbus RS-485.
 * Jika valid dalam <= 60 detik, panggil esp_ota_mark_app_valid_cancel_rollback().
 */
void gms_ota_self_test_check(void);

/**
 * @brief Memperbarui status kesiapan subsistem untuk kelulusan self-test OTA.
 * @param mqtt_ok Status keterhubungan broker MQTT
 * @param adc_ok Status sampling ADC ADS1115
 * @param modbus_ok Status komunikasi Modbus RS-485
 */
void gms_ota_self_test_feed(bool mqtt_ok, bool adc_ok, bool modbus_ok);

/**
 * @brief Memulai proses unduhan berkas biner firmware baru melalui protokol HTTPS/HTTP.
 * Biner akan diunduh secara asinkron dan ditulis ke partisi OTA yang sedang pasif.
 * @param url Alamat URL tempat biner .bin firmware berada
 * @return ESP_OK jika proses unduhan dimulai dengan sukses
 */
esp_err_t gms_ota_start_from_url(const char *url);

/**
 * @brief Mendapatkan informasi partisi firmware yang sedang aktif dan deskripsi versi.
 * @param out_buf Buffer untuk menyimpan teks status partisi
 * @param max_len Panjang maksimum buffer
 */
void gms_ota_get_partition_info(char *out_buf, size_t max_len);

/**
 * @brief Task FreeRTOS OTA (Prioritas 1, Core 1).
 */
void task_ota(void *arg);

#ifdef __cplusplus
}
#endif
