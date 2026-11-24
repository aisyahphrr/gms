#include "gms_ota.h"
#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "GMS_OTA";

static char s_ota_url[256] = {0};
static SemaphoreHandle_t s_ota_sem = NULL;
static bool s_ota_busy = false;

static bool s_pending_verify = false;
static bool s_verified = false;
static int64_t s_verify_start_us = 0;
static const int64_t TIMEOUT_SELF_TEST_US = 60LL * 1000000LL; /* Batas swauji 60 detik */

void gms_ota_self_test_check(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Gagal mendapatkan partisi running");
        return;
    }

    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(running, &app_desc) == ESP_OK) {
        ESP_LOGI(TAG, "Running firmware: %s (v%s, Built: %s %s) pada partisi '%s' [offset: 0x%08" PRIx32 "]",
                 app_desc.project_name, app_desc.version, app_desc.date, app_desc.time,
                 running->label, running->address);
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            s_pending_verify = true;
            s_verified = false;
            s_verify_start_us = esp_timer_get_time();
            ESP_LOGW(TAG, "==========================================================");
            ESP_LOGW(TAG, "PARTISI BARU TERDETEKSI: Status ESP_OTA_IMG_PENDING_VERIFY");
            ESP_LOGW(TAG, "Memulai jendela swauji (Self-Test) 60 detik...");
            ESP_LOGW(TAG, "Kriteria: Wi-Fi/MQTT terhubung & subsistem ADC merespon normal.");
            ESP_LOGW(TAG, "Jika gagal atau batas waktu terlampaui -> Auto-Rollback!");
            ESP_LOGW(TAG, "==========================================================");
        } else {
            ESP_LOGI(TAG, "Partisi saat ini berstatus TERVALIDASI (state=%d)", (int)ota_state);
        }
    }
}

void gms_ota_self_test_feed(bool mqtt_ok, bool adc_ok, bool modbus_ok) {
    if (!s_pending_verify || s_verified) {
        return;
    }

    int64_t elapsed_us = esp_timer_get_time() - s_verify_start_us;
    if (elapsed_us > TIMEOUT_SELF_TEST_US) {
        ESP_LOGE(TAG, "Batas waktu Self-Test 60 detik terlampaui tanpa validasi penuh!");
        ESP_LOGE(TAG, "Memicu pembatalan update firmware (Auto-Rollback)...");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }

    /* Kriteria kelulusan swauji sesuai Dokumen TA Subbab I.6.4:
       Koneksi MQTT berhasil dan sensor ADC terbaca normal */
    if (mqtt_ok && adc_ok) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            s_verified = true;
            s_pending_verify = false;
            ESP_LOGI(TAG, "==========================================================");
            ESP_LOGI(TAG, "SWAUJI (SELF-TEST) LULUS dalam %lld ms!", elapsed_us / 1000);
            ESP_LOGI(TAG, "Partisi firmware baru divalidasi PERMANEN (Rollback dibatalkan).");
            ESP_LOGI(TAG, "Modbus status: %s", modbus_ok ? "OK" : "STANDBY/BENCH");
            ESP_LOGI(TAG, "==========================================================");
        } else {
            ESP_LOGE(TAG, "Gagal memvalidasi partisi: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t gms_ota_start_from_url(const char *url) {
    if (!url || strlen(url) < 8) {
        ESP_LOGE(TAG, "URL OTA tidak valid");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Memulai unduhan firmware OTA dari URL: %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Mengunduh dan menulis biner ke bank partisi cadangan...");
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA BERHASIL! Biner terverifikasi. Me-restart perangkat ke partisi baru...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA GAGAL dieksekusi: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t gms_ota_trigger(const char *url) {
    if (!url || strlen(url) < 8 || strlen(url) >= sizeof(s_ota_url)) {
        ESP_LOGE(TAG, "URL OTA tidak valid atau melebihi kapasitas buffer (maks %u)", (unsigned)sizeof(s_ota_url));
        return ESP_ERR_INVALID_ARG;
    }
    if (s_pending_verify && !s_verified) {
        ESP_LOGW(TAG, "Partisi baru masih dalam swauji (PENDING_VERIFY), tunda pemicu OTA sampai tervalidasi");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota_busy) {
        ESP_LOGW(TAG, "Proses OTA sedang berjalan, abaikan pemicu baru");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ota_sem) {
        s_ota_sem = xSemaphoreCreateBinary();
        if (!s_ota_sem) return ESP_ERR_NO_MEM;
    }

    strncpy(s_ota_url, url, sizeof(s_ota_url) - 1);
    s_ota_url[sizeof(s_ota_url) - 1] = '\0';
    ESP_LOGI(TAG, "Sinyal OTA diterima. Menyerahkan unduhan ke task_ota (Core 1): %s", s_ota_url);
    xSemaphoreGive(s_ota_sem);
    return ESP_OK;
}

void gms_ota_get_partition_info(char *out_buf, size_t max_len) {
    if (!out_buf || max_len == 0) return;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    esp_app_desc_t desc;
    const char *ver = "Unknown";
    if (running && esp_ota_get_partition_description(running, &desc) == ESP_OK) {
        ver = desc.version;
    }

    snprintf(out_buf, max_len, "Running: %s (v%s) | Next Bank: %s",
             running ? running->label : "N/A",
             ver,
             next ? next->label : "None");
}

/*
 * Task OTA (Prioritas 1, Core 1, Event-Driven)
 * Berjalan di latar belakang Core 1 tanpa mengganggu telemetri atau proteksi Core 0.
 */
void task_ota(void *arg) {
    (void)arg;

    if (!s_ota_sem) {
        s_ota_sem = xSemaphoreCreateBinary();
    }

    char info[128] = {0};
    gms_ota_get_partition_info(info, sizeof(info));
    ESP_LOGI(TAG, "task_ota online di Core %d (Prio %d) - %s",
             xPortGetCoreID(), (int)uxTaskPriorityGet(NULL), info);

    for (;;) {
        /* Menunggu sinyal pemicu OTA via Semaphore */
        if (xSemaphoreTake(s_ota_sem, portMAX_DELAY) == pdTRUE) {
            s_ota_busy = true;
            ESP_LOGW(TAG, "==========================================================");
            ESP_LOGW(TAG, "MEMULAI PROSES OTA DARI TASK_OTA (CORE 1):");
            ESP_LOGW(TAG, "Target URL: %s", s_ota_url);
            ESP_LOGW(TAG, "==========================================================");

            esp_err_t ret = gms_ota_start_from_url(s_ota_url);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Proses OTA gagal dieksekusi (%s). Kembali standby.", esp_err_to_name(ret));
            }
            s_ota_busy = false;
        }
    }
}
