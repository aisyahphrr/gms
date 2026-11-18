#include "gms.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "lwip/ip4_addr.h"

/* =====================================================
   GMS INTERNAL WEB SERVER
   ESP32-S3 / ESP-IDF 6.0.2

   Features:
   - Login and session authentication
   - CSRF protection
   - Wi-Fi AP + STA configuration
   - MQTT TLS configuration
   - Admin password management
   - Sensor calibration configuration
   - Sensor threshold configuration
   - Monitoring API with alarm status

   BENCH TEST ONLY
   NO RELAY CONTROL
   ===================================================== */

static const char *TAG = "GMS_WEB";

static httpd_handle_t s_server = NULL;

#define SESSION_SECONDS 1800
#define MAX_BODY 2048

typedef struct {
    char sid[33];
    char csrf[33];
    int64_t until;
} admin_session_t;

static admin_session_t s_session = {0};

static unsigned s_fail_count = 0;
static int64_t s_fail_until = 0;

/* =====================================================
   EMBEDDED WEB ASSETS
   ===================================================== */

extern const char login_html[]
    asm("_binary_login_html_start");

extern const char dashboard_html[]
    asm("_binary_dashboard_html_start");

extern const char settings_html[]
    asm("_binary_settings_html_start");

extern const char style_css[]
    asm("_binary_style_css_start");

extern const char app_js[]
    asm("_binary_app_js_start");

/* =====================================================
   TIME AND SECURITY HELPERS
   ===================================================== */

static int64_t now_sec(void)
{
    return esp_timer_get_time() / 1000000;
}

static void token_hex(char out[33])
{
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));

    const char *hex = "0123456789abcdef";

    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 15];
    }

    out[32] = '\0';
}

static bool constant_text_equal(
    const char *a,
    const char *b
)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);

    if (alen != blen) {
        return false;
    }

    unsigned diff = 0;

    for (size_t i = 0; i < alen; i++) {
        diff |=
            (unsigned char)a[i] ^
            (unsigned char)b[i];
    }

    return diff == 0;
}

static void secure_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(
        req,
        "Cache-Control",
        "no-store, max-age=0"
    );

    httpd_resp_set_hdr(
        req,
        "X-Content-Type-Options",
        "nosniff"
    );

    httpd_resp_set_hdr(
        req,
        "X-Frame-Options",
        "DENY"
    );

    httpd_resp_set_hdr(
        req,
        "Referrer-Policy",
        "no-referrer"
    );

    httpd_resp_set_hdr(
        req,
        "Content-Security-Policy",
        "default-src 'self'; "
        "base-uri 'none'; "
        "object-src 'none'; "
        "frame-ancestors 'none'"
    );
}

/* =====================================================
   JSON RESPONSE
   ===================================================== */

static esp_err_t respond_json(
    httpd_req_t *req,
    int code,
    const char *json
)
{
    secure_headers(req);

    httpd_resp_set_type(
        req,
        "application/json; charset=utf-8"
    );

    switch (code) {
        case 400:
            httpd_resp_set_status(
                req, "400 Bad Request"
            );
            break;

        case 401:
            httpd_resp_set_status(
                req, "401 Unauthorized"
            );
            break;

        case 403:
            httpd_resp_set_status(
                req, "403 Forbidden"
            );
            break;

        case 429:
            httpd_resp_set_status(
                req, "429 Too Many Requests"
            );
            break;

        case 500:
            httpd_resp_set_status(
                req, "500 Internal Server Error"
            );
            break;

        default:
            break;
    }

    return httpd_resp_sendstr(req, json);
}

/* =====================================================
   SESSION AUTHENTICATION
   ===================================================== */

static bool authorized(httpd_req_t *req)
{
    if (
        !s_session.sid[0] ||
        now_sec() > s_session.until
    ) {
        return false;
    }

    char cookie[200] = {0};

    if (
        httpd_req_get_hdr_value_str(
            req,
            "Cookie",
            cookie,
            sizeof(cookie)
        ) != ESP_OK
    ) {
        return false;
    }

    /*
     * Find the exact gms_sid cookie rather than
     * matching an arbitrary substring.
     */

    const char *p = cookie;
    char sid[33] = {0};
    bool found = false;

    while (*p) {
        while (*p == ' ' || *p == ';') {
            p++;
        }

        const char *end = strchr(p, ';');

        if (!end) {
            end = p + strlen(p);
        }

        const char *prefix = "gms_sid=";
        const size_t prefix_len = 8;

        if (
            (size_t)(end - p) >= prefix_len &&
            memcmp(p, prefix, prefix_len) == 0
        ) {
            size_t len =
                (size_t)(end - p) - prefix_len;

            if (len != 32) {
                return false;
            }

            memcpy(
                sid,
                p + prefix_len,
                len
            );

            sid[len] = '\0';
            found = true;
            break;
        }

        p = end;

        if (*p == ';') {
            p++;
        }
    }

    if (
        !found ||
        !constant_text_equal(
            sid,
            s_session.sid
        )
    ) {
        return false;
    }

    s_session.until =
        now_sec() + SESSION_SECONDS;

    return true;
}

static bool csrf_ok(httpd_req_t *req)
{
    if (!authorized(req)) {
        return false;
    }

    char csrf[64] = {0};

    if (
        httpd_req_get_hdr_value_str(
            req,
            "X-GMS-CSRF",
            csrf,
            sizeof(csrf)
        ) != ESP_OK
    ) {
        return false;
    }

    return constant_text_equal(
        csrf,
        s_session.csrf
    );
}

/* =====================================================
   PAGE HANDLERS
   ===================================================== */

static esp_err_t require_page(
    httpd_req_t *req,
    const char *content
)
{
    if (!authorized(req)) {
        secure_headers(req);

        httpd_resp_set_status(
            req, "302 Found"
        );

        httpd_resp_set_hdr(
            req,
            "Location",
            "/login"
        );

        return httpd_resp_sendstr(req, "");
    }

    secure_headers(req);

    httpd_resp_set_type(
        req,
        "text/html; charset=utf-8"
    );

    return httpd_resp_sendstr(req, content);
}

static esp_err_t page_login(httpd_req_t *req)
{
    if (authorized(req)) {
        secure_headers(req);

        httpd_resp_set_status(
            req, "302 Found"
        );

        httpd_resp_set_hdr(
            req,
            "Location",
            "/dashboard"
        );

        return httpd_resp_sendstr(req, "");
    }

    secure_headers(req);

    httpd_resp_set_type(
        req,
        "text/html; charset=utf-8"
    );

    return httpd_resp_sendstr(
        req,
        login_html
    );
}

static esp_err_t page_dash(httpd_req_t *req)
{
    return require_page(
        req,
        dashboard_html
    );
}

static esp_err_t page_settings(httpd_req_t *req)
{
    return require_page(
        req,
        settings_html
    );
}

static esp_err_t get_css(httpd_req_t *req)
{
    secure_headers(req);

    httpd_resp_set_type(
        req,
        "text/css; charset=utf-8"
    );

    return httpd_resp_sendstr(
        req,
        style_css
    );
}

static esp_err_t get_js(httpd_req_t *req)
{
    secure_headers(req);

    httpd_resp_set_type(
        req,
        "application/javascript; charset=utf-8"
    );

    return httpd_resp_sendstr(
        req,
        app_js
    );
}

/* =====================================================
   HTTP BODY READER
   ===================================================== */

static esp_err_t request_body(
    httpd_req_t *req,
    char *dest,
    size_t capacity
)
{
    if (
        req->content_len <= 0 ||
        req->content_len >= capacity ||
        req->content_len > MAX_BODY
    ) {
        return ESP_ERR_INVALID_SIZE;
    }

    int got = 0;
    int want = req->content_len;

    while (got < want) {
        int r = httpd_req_recv(
            req,
            dest + got,
            want - got
        );

        if (r <= 0) {
            return ESP_FAIL;
        }

        got += r;
    }

    dest[want] = '\0';

    return ESP_OK;
}

/* =====================================================
   FORM DECODING
   ===================================================== */

static int dec_hex(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }

    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }

    return -1;
}

static bool decode_form(
    const char *src,
    size_t length,
    char *out,
    size_t cap
)
{
    size_t j = 0;

    for (size_t i = 0; i < length; i++) {
        unsigned char c =
            (unsigned char)src[i];

        if (c == '+') {
            c = ' ';
        }
        else if (c == '%') {
            if (i + 2 >= length) {
                return false;
            }

            int x = dec_hex(src[i + 1]);
            int y = dec_hex(src[i + 2]);

            if (x < 0 || y < 0) {
                return false;
            }

            c = (unsigned char)(
                (x << 4) | y
            );

            i += 2;
        }

        if (
            c == 0 ||
            c < 0x20 ||
            c == 0x7f ||
            j + 1 >= cap
        ) {
            return false;
        }

        out[j++] = (char)c;
    }

    out[j] = '\0';

    return true;
}

static bool form_read(
    const char *body,
    const char *name,
    char *dest,
    size_t capacity
)
{
    size_t keylen = strlen(name);
    const char *p = body;

    while (*p) {
        const char *end = strchr(p, '&');

        if (!end) {
            end = p + strlen(p);
        }

        const char *eq = memchr(
            p,
            '=',
            (size_t)(end - p)
        );

        if (
            eq &&
            (size_t)(eq - p) == keylen &&
            memcmp(p, name, keylen) == 0
        ) {
            return decode_form(
                eq + 1,
                (size_t)(end - eq - 1),
                dest,
                capacity
            );
        }

        p = (*end == '&')
            ? end + 1
            : end;
    }

    return false;
}

/* =====================================================
   INPUT VALIDATION
   ===================================================== */

static bool check_string(
    const char *s,
    size_t maxlen,
    bool allow_empty
)
{
    size_t n = strlen(s);

    if (
        n > maxlen ||
        (!allow_empty && n == 0)
    ) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        if (
            (unsigned char)s[i] < 32 ||
            (unsigned char)s[i] > 126
        ) {
            return false;
        }
    }

    return true;
}

static bool hostname_ok(const char *s)
{
    size_t n = strlen(s);

    if (n == 0 || n > 96) {
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        char c = s[i];

        if (!(
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' ||
            c == '-'
        )) {
            return false;
        }
    }

    return true;
}

static bool ip_ok(const char *s)
{
    ip4_addr_t ip;

    return s[0] &&
        ip4addr_aton(s, &ip);
}

static bool is_all_digits(const char *s)
{
    if (!*s) {
        return false;
    }

    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) {
            return false;
        }
    }

    return true;
}

static void json_string(
    const char *in,
    char *out,
    size_t cap
)
{
    size_t j = 0;

    if (cap < 3) {
        return;
    }

    out[j++] = '"';

    for (
        size_t i = 0;
        in[i] && j + 7 < cap;
        i++
    ) {
        unsigned char c =
            (unsigned char)in[i];

        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        }
        else if (c >= 32 && c < 127) {
            out[j++] = (char)c;
        }
        else {
            out[j++] = '?';
        }
    }

    out[j++] = '"';
    out[j] = '\0';
}

/* =====================================================
   LOGIN API
   ===================================================== */

static esp_err_t post_login(httpd_req_t *req)
{
    if (now_sec() < s_fail_until) {
        return respond_json(
            req,
            429,
            "{\"error\":\"Terlalu banyak percobaan login; coba lagi nanti\"}"
        );
    }

    char body[MAX_BODY + 1] = {0};
    char user[60] = {0};
    char pw[130] = {0};

    if (
        request_body(
            req,
            body,
            sizeof(body)
        ) != ESP_OK ||

        !form_read(
            body,
            "username",
            user,
            sizeof(user)
        ) ||

        !form_read(
            body,
            "password",
            pw,
            sizeof(pw)
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Form login tidak valid\"}"
        );
    }

    if (
        strcmp(user, "admin") != 0 ||
        !gms_store_verify_admin(pw)
    ) {
        s_fail_count++;

        if (s_fail_count >= 5) {
            s_fail_count = 0;
            s_fail_until = now_sec() + 60;
        }

        memset(pw, 0, sizeof(pw));

        return respond_json(
            req,
            401,
            "{\"error\":\"Username atau password salah\"}"
        );
    }

    memset(pw, 0, sizeof(pw));

    s_fail_count = 0;
    s_fail_until = 0;

    token_hex(s_session.sid);
    token_hex(s_session.csrf);

    s_session.until =
        now_sec() + SESSION_SECONDS;

    char cookie[160];

    snprintf(
        cookie,
        sizeof(cookie),
        "gms_sid=%s; HttpOnly; SameSite=Strict; "
        "Path=/; Max-Age=%d",
        s_session.sid,
        SESSION_SECONDS
    );

    secure_headers(req);

    httpd_resp_set_hdr(
        req,
        "Set-Cookie",
        cookie
    );

    httpd_resp_set_type(
        req,
        "application/json"
    );

    return httpd_resp_sendstr(
        req,
        "{\"ok\":true}"
    );
}

/* =====================================================
   LOGOUT API
   ===================================================== */

static esp_err_t post_logout(httpd_req_t *req)
{
    if (!csrf_ok(req)) {
        return respond_json(
            req,
            403,
            "{\"error\":\"Sesi/CSRF tidak valid\"}"
        );
    }

    memset(
        &s_session,
        0,
        sizeof(s_session)
    );

    secure_headers(req);

    httpd_resp_set_hdr(
        req,
        "Set-Cookie",
        "gms_sid=; Path=/; HttpOnly; "
        "SameSite=Strict; Max-Age=0"
    );

    httpd_resp_set_type(
        req,
        "application/json"
    );

    return httpd_resp_sendstr(
        req,
        "{\"ok\":true}"
    );
}

/* =====================================================
   SENSOR ALARM HELPER
   ===================================================== */

static const char *gms_alarm_name(uint8_t alarm)
{
    switch (alarm) {
        case GMS_ALARM_NORMAL:
            return "NORMAL";

        case GMS_ALARM_WARNING:
            return "WARNING";

        case GMS_ALARM_CRITICAL:
            return "CRITICAL";

        case GMS_ALARM_OUT_OF_RANGE:
            return "OUT_OF_RANGE";

        case GMS_ALARM_SENSOR_ERROR:
        default:
            return "SENSOR_ERROR";
    }
}

static void gms_json_float(
    char *out,
    size_t capacity,
    float value,
    bool valid,
    unsigned decimals
)
{
    if (!out || capacity == 0) {
        return;
    }

    if (!valid || !isfinite(value)) {
        snprintf(out, capacity, "null");
        return;
    }

    snprintf(
        out,
        capacity,
        "%.*f",
        (int)decimals,
        (double)value
    );
}

/* BENCH SIMULATION ONLY: separate NVS from actual genset counters.
   Persists at state transitions / about every 60 s of active dashboard polling.
   Does not change relays, sensor measurements, MQTT payload, or engine reality. */
#define SIM_SERVICE_INTERVAL_H 250.0
#define SIM_SERVICE_SOON_H 10.0
static const char *sim_states[] = {"STOPPED", "STARTING", "RUNNING", "STOPPING", "FAULT"};
static int sim_state = 0;
static bool sim_enabled = false;
static int64_t sim_last_us = 0;
static int64_t sim_last_save_us = 0;
static double sim_run_hours = 0.0;
static bool sim_initialized = false;
static void sim_save(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("gms_sim_bench", NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "Sim NVS open: %s", esp_err_to_name(err)); return; }
    /* Store whole elapsed SECONDS in separate simulation namespace.
       Fractional seconds can be lost, at most ~1 second per persistence. */
    uint64_t seconds = (uint64_t)(sim_run_hours * 3600.0);
    err = nvs_set_u64(h, "run_seconds", seconds);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "Sim NVS save: %s", esp_err_to_name(err));
    nvs_close(h);
    if (err == ESP_OK) sim_last_save_us = esp_timer_get_time();
}
static void sim_init(void) {
    if (sim_initialized) return;
    sim_initialized = true;
    nvs_handle_t h;
    if (nvs_open("gms_sim_bench", NVS_READONLY, &h) == ESP_OK) {
        uint64_t seconds = 0;
        if (nvs_get_u64(h, "run_seconds", &seconds) == ESP_OK)
            sim_run_hours = (double)seconds / 3600.0;
        nvs_close(h);
    }
    /* After restart the simulator ALWAYS defaults to disabled/STOPPED. */
    sim_enabled = false; sim_state = 0;
    sim_last_us = esp_timer_get_time();
    sim_last_save_us = sim_last_us;
}
static void sim_tick(void) {
    sim_init();
    int64_t t = esp_timer_get_time();
    if (sim_last_us > 0 && sim_enabled && sim_state == 2 && t > sim_last_us) {
        sim_run_hours += (double)(t - sim_last_us) / 3600000000.0;
    }
    sim_last_us = t;
    if (sim_enabled && sim_state == 2 && t - sim_last_save_us >= 60000000LL) sim_save();
}
/* Single synthetic source for both local dashboard and Mastify.
 * Assumptions are illustrative: 60 kVA 3-phase 400/230V genset, 4 poles,
 * 45% load, PF 0.85 and 0.25 L/kWh. NOT calibrated specifications. */
void gms_bench_snapshot(gms_bench_data_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    sim_tick();
    out->active = sim_enabled;
    out->state = sim_state;
    out->run_hours = sim_run_hours;
    out->maint_due_h = fmax(0.0, SIM_SERVICE_INTERVAL_H - sim_run_hours);
    out->service_status = out->maint_due_h <= 0.0 ? "SERVICE_DUE" :
                          out->maint_due_h <= SIM_SERVICE_SOON_H ? "SERVICE_SOON" : "NORMAL";
    if (!sim_enabled) return;
    /* Illustrative cumulative fuel model, retained while simulation is STOPPED. */
    out->fuel_est_l = out->run_hours * (60.0 * 0.45 * 0.85) * 0.25;
    bool running = (sim_state == 2);
    out->battery_v = running ? 13.8 : 12.6;
    if (!running) return;
    /* Deterministic internal consistency, not a measured waveform. */
    out->freq_hz = 50.0;
    out->rpm_calc = 1500.0; /* 4 poles, synchronous estimate. */
    out->load_pct = 45.0;
    out->v_l1 = 230.0; out->v_l2 = 229.0; out->v_l3 = 231.0;
    out->v_l12 = 400.0; out->v_l23 = 399.0; out->v_l31 = 401.0;
    out->s_kva = 60.0 * out->load_pct / 100.0;
    out->pf = 0.85;
    out->p_kw = out->s_kva * out->pf;
    out->q_kvar = sqrt(out->s_kva*out->s_kva - out->p_kw*out->p_kw);
    const double current_a = out->s_kva * 1000.0 / (sqrt(3.0) * 400.0);
    out->i_l1 = current_a; out->i_l2 = current_a * 0.995;
    out->i_l3 = current_a * 1.005;
}

static esp_err_t post_sim_state(httpd_req_t *req) {
    if (!csrf_ok(req)) return respond_json(req, 403, "{\"error\":\"Sesi/CSRF tidak valid\"}");
    char body[128] = {0};
    if (request_body(req, body, sizeof(body)) != ESP_OK) return respond_json(req, 400, "{\"error\":\"Body tidak valid\"}");
    char state[16] = {0};
    if (!form_read(body, "state", state, sizeof(state))) return respond_json(req, 400, "{\"error\":\"State wajib diisi\"}");
    if (!strcmp(state, "DISABLE")) {
        sim_tick(); sim_save(); sim_enabled = false; sim_state = 0;
        return respond_json(req, 200, "{\"ok\":true}");
    }
    int next = -1;
    for (int i = 0; i < 5; ++i) if (!strcmp(state, sim_states[i])) next = i;
    if (next < 0) return respond_json(req, 400, "{\"error\":\"State tidak dikenal\"}");
    sim_tick();
    bool ok = !sim_enabled ? next == 0 :
        (next == sim_state || (sim_state == 0 && next == 1) ||
         (sim_state == 1 && next == 2) || (sim_state == 2 && next == 3) ||
         (sim_state == 3 && next == 0) || next == 4 ||
         (sim_state == 4 && next == 0));
    if (!ok) return respond_json(req, 409, "{\"error\":\"Transisi tidak valid. Mulai dari STOPPED.\"}");
    if (sim_enabled && sim_state == 2 && next != 2) sim_save();
    sim_enabled = true; sim_state = next; sim_last_us = esp_timer_get_time();
    return respond_json(req, 200, "{\"ok\":true}");
}

/* =====================================================
   UNIVERSAL SENSOR SIMULATION (A0 / A2 / A3), BENCH ONLY.
   Authenticated admin + CSRF, max 120 seconds; never drives outputs.
   ===================================================== */
static esp_err_t post_sensor_test(httpd_req_t *req)
{
    if (!csrf_ok(req))
        return respond_json(req, 403, "{\"error\":\"Sesi/CSRF tidak valid\"}");
    char body[140] = {0};
    if (request_body(req, body, sizeof(body)) != ESP_OK)
        return respond_json(req, 400, "{\"error\":\"Body tidak valid\"}");
    char action[20] = {0};
    if (!form_read(body, "action", action, sizeof(action)))
        return respond_json(req, 400, "{\"error\":\"Action wajib diisi\"}");
    if (!strcmp(action, "STOP")) {
        gms_sensor_test_stop();
        return respond_json(req, 200, "{\"ok\":true,\"active\":false}");
    }
    if (strcmp(action, "START"))
        return respond_json(req, 400, "{\"error\":\"Action tidak dikenal\"}");
    char sensor[20] = {0}, ma_text[20] = {0};
    if (!form_read(body, "sensor", sensor, sizeof(sensor)) ||
        !form_read(body, "ma", ma_text, sizeof(ma_text)))
        return respond_json(req, 400, "{\"error\":\"Sensor dan arus mA wajib diisi\"}");
    int channel = !strcmp(sensor, "temp") ? GMS_SENSOR_TEMP :
                  !strcmp(sensor, "level") ? GMS_SENSOR_LEVEL :
                  !strcmp(sensor, "pressure") ? GMS_SENSOR_PRESSURE : -1;
    errno = 0;
    char *end = NULL;
    float ma = strtof(ma_text, &end);
    if (channel < 0 || end == ma_text || *end || errno != 0 ||
        !isfinite(ma) || ma < 0.0f || ma > 24.0f ||
        !gms_sensor_test_start(channel, ma))
        return respond_json(req, 400, "{\"error\":\"Sensor/arus invalid. Gunakan 0..24 mA\"}");
    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"active\":true,\"channel\":%d,\"ma\":%.3f,\"duration_s\":120}",
             channel, (double)ma);
    return respond_json(req, 200, json);
}

/* =====================================================
   MONITORING STATUS API

   GET /api/status

   Data:
   - Raw ADC
   - Current mA
   - Scaled engineering value
   - Alarm status
   - Wi-Fi and MQTT status
   - EWIG Modbus status

   ===================================================== */

static esp_err_t get_status(httpd_req_t *req)
{
    if (!authorized(req)) {
        return respond_json(
            req,
            401,
            "{\"error\":\"Login diperlukan\"}"
        );
    }

    gms_measurement_t s = {0};

    gms_get_snapshot(&s);

    char ip[16] = {0};
    char ap[40] = {0};

    gms_get_station_ip(ip);
    gms_get_ap_ssid(ap);

    char ipjs[45] = {0};
    char apjs[95] = {0};

    json_string(ip, ipjs, sizeof(ipjs));
    json_string(ap, apjs, sizeof(apjs));

    /*
     * Prepare valid JSON numeric values.
     *
     * Invalid physical values are represented
     * as JSON null rather than NaN.
     */

    char ma_json[GMS_SENSOR_COUNT][24];
    char scaled_json[GMS_SENSOR_COUNT][24];

    for (int i = 0; i < GMS_SENSOR_COUNT; i++) {
        gms_json_float(
            ma_json[i],
            sizeof(ma_json[i]),
            s.ma[i],
            s.adc_ok[i],
            3
        );

        bool scaled_valid =
            s.alarm[i] == GMS_ALARM_NORMAL ||
            s.alarm[i] == GMS_ALARM_WARNING ||
            s.alarm[i] == GMS_ALARM_CRITICAL;

        gms_json_float(
            scaled_json[i],
            sizeof(scaled_json[i]),
            s.scaled[i],
            scaled_valid,
            3
        );
    }

    gms_bench_data_t bench = {0};
    gms_bench_snapshot(&bench);
    char sim_engine_json[32] = "null";
    char sim_hours_json[32] = "null";
    char sim_due_json[32] = "null";
    char sim_service_json[32] = "null";
    /* Counter remains visible after restart, explicitly labelled simulation. */
    snprintf(sim_hours_json, sizeof(sim_hours_json), "%.6f", sim_run_hours);
    double remaining = fmax(0.0, SIM_SERVICE_INTERVAL_H - sim_run_hours);
    snprintf(sim_due_json, sizeof(sim_due_json), "%.3f", remaining);
    const char *service = remaining <= 0.0 ? "SERVICE_DUE" :
                          remaining <= SIM_SERVICE_SOON_H ? "SERVICE_SOON" : "NORMAL";
    snprintf(sim_service_json, sizeof(sim_service_json), "\"%s\"", service);
    if (sim_enabled) snprintf(sim_engine_json, sizeof(sim_engine_json), "\"%s\"", sim_states[sim_state]);
    char bench_json[18][32];
    snprintf(bench_json[0], sizeof(bench_json[0]), bench.active ? "%.3f" : "null", bench.rpm_calc);
    snprintf(bench_json[1], sizeof(bench_json[1]), bench.active ? "%.3f" : "null", bench.battery_v);
    snprintf(bench_json[2], sizeof(bench_json[2]), bench.active ? "%.3f" : "null", bench.freq_hz);
    snprintf(bench_json[3], sizeof(bench_json[3]), bench.active ? "%.3f" : "null", bench.load_pct);
    snprintf(bench_json[4], sizeof(bench_json[4]), bench.active ? "%.3f" : "null", bench.v_l1);
    snprintf(bench_json[5], sizeof(bench_json[5]), bench.active ? "%.3f" : "null", bench.v_l2);
    snprintf(bench_json[6], sizeof(bench_json[6]), bench.active ? "%.3f" : "null", bench.v_l3);
    snprintf(bench_json[7], sizeof(bench_json[7]), bench.active ? "%.3f" : "null", bench.v_l12);
    snprintf(bench_json[8], sizeof(bench_json[8]), bench.active ? "%.3f" : "null", bench.v_l23);
    snprintf(bench_json[9], sizeof(bench_json[9]), bench.active ? "%.3f" : "null", bench.v_l31);
    snprintf(bench_json[10], sizeof(bench_json[10]), bench.active ? "%.3f" : "null", bench.i_l1);
    snprintf(bench_json[11], sizeof(bench_json[11]), bench.active ? "%.3f" : "null", bench.i_l2);
    snprintf(bench_json[12], sizeof(bench_json[12]), bench.active ? "%.3f" : "null", bench.i_l3);
    snprintf(bench_json[13], sizeof(bench_json[13]), bench.active ? "%.3f" : "null", bench.p_kw);
    snprintf(bench_json[14], sizeof(bench_json[14]), bench.active ? "%.3f" : "null", bench.q_kvar);
    snprintf(bench_json[15], sizeof(bench_json[15]), bench.active ? "%.3f" : "null", bench.s_kva);
    snprintf(bench_json[16], sizeof(bench_json[16]), bench.active ? "%.3f" : "null", bench.pf);
    snprintf(bench_json[17], sizeof(bench_json[17]), bench.active ? "%.3f" : "null", bench.fuel_est_l);
    gms_tank_config_t tank;
    gms_tank_get_config(&tank);
    float tank_mm=0.0f, tank_pct=0.0f, tank_liters=0.0f;
    const bool tank_valid_measurement =
        s.alarm[GMS_SENSOR_LEVEL] >= 1 && s.alarm[GMS_SENSOR_LEVEL] <= 3 &&
        gms_tank_measure(s.ma[GMS_SENSOR_LEVEL], &tank_mm, &tank_pct, &tank_liters);
    char tank_mm_json[24], tank_l_json[24];
    gms_json_float(tank_mm_json, sizeof(tank_mm_json), tank_mm, tank_valid_measurement, 2);
    gms_json_float(tank_l_json, sizeof(tank_l_json), tank_liters, tank_valid_measurement, 3);
    char payload[3072];

    int n = snprintf(
        payload,
        sizeof(payload),

        "{"
        "\"csrf\":\"%s\","
        "\"device\":\"GMS-001\","
        "\"bench_mode\":true,"

        "\"sta_online\":%s,"
        "\"mqtt_connected\":%s,"
        "\"mqtt2_connected\":%s,"
        "\"sta_ip\":%s,"
        "\"ap_ip\":\"192.168.4.1\","
        "\"ap_ssid\":%s,"

        "\"uptime_ms\":%" PRIu32 ","
        "\"sensor_test\":{\"active\":%s,\"channel\":%d,"
        "\"ma\":%.3f,\"real_ma\":%.3f,\"remaining_s\":%" PRIu32 "},"

        "\"temp\":{"
        "\"voltage\":%.4f,"
        "\"ma\":%s,"
        "\"scaled\":%s,"
        "\"unit\":\"C\","
        "\"alarm\":%u,"
        "\"alarm_text\":\"%s\","
        "\"raw\":%d,"
        "\"adc_ok\":%s,"
        "\"loop_ok\":%s"
        "},"

        "\"level\":{"
        "\"voltage\":%.4f,"
        "\"ma\":%s,"
        "\"percent\":%s,"
        "\"scaled\":%s,"
        "\"unit\":\"%%\","
        "\"alarm\":%u,"
        "\"alarm_text\":\"%s\","
        "\"raw\":%d,"
        "\"adc_ok\":%s,"
        "\"loop_ok\":%s"
        "},"

        "\"pressure\":{"
        "\"voltage\":%.4f,"
        "\"ma\":%s,"
        "\"bar\":%s,"
        "\"scaled\":%s,"
        "\"unit\":\"bar\","
        "\"alarm\":%u,"
        "\"alarm_text\":\"%s\","
        "\"calibrated\":false,"
        "\"fault_test_active\":%s,"
        "\"real_ma\":%.3f,"
        "\"real_voltage\":%.4f,"
        "\"fault_test_remaining_s\":%" PRIu32 ","
        "\"raw\":%d,"
        "\"adc_ok\":%s,"
        "\"loop_ok\":%s"
        "},"

        "\"tank\":{"
        "\"length_cm\":%.2f,\"width_cm\":%.2f,\"height_cm\":%.2f,"
        "\"height_mm\":%.2f,\"level_mm\":%s,\"volume_l\":%s,"
        "\"two_point_enabled\":%s,\"sensor_range_mm\":5000,"
        "\"level_source\":\"%s\""
        "},"
        "\"ewig\":{"
        "\"modbus_ok\":%s,"
        "\"measurement_valid\":false,"
        "\"raw_r09\":%u,"
        "\"raw_r10\":%u"
        "},"
        /* Hybrid: physical sensors above remain real; genset below synthetic. */
        "\"genset\":{"
        "\"data_source\":\"BENCH_SIMULATION\","
        "\"engine_state\":%s,"
        "\"rpm_calc\":%s,"
        "\"battery_v\":%s,"
        "\"freq_hz\":%s,"
        "\"load_pct\":%s,"
        "\"v_l1\":%s,"
        "\"v_l2\":%s,"
        "\"v_l3\":%s,"
        "\"v_l12\":%s,"
        "\"v_l23\":%s,"
        "\"v_l31\":%s,"
        "\"i_l1\":%s,"
        "\"i_l2\":%s,"
        "\"i_l3\":%s,"
        "\"p_kw\":%s,"
        "\"q_kvar\":%s,"
        "\"s_kva\":%s,"
        "\"pf\":%s,"
        "\"run_hours\":%s,"
        "\"maint_due_h\":%s,"
        "\"service_status\":%s,"
        "\"fuel_est_l\":%s,"
        "\"simulation_mode\":%s"
        "}"
        "}",

        /* Session and network */

        s_session.csrf,

        gms_wifi_online() ? "true" : "false",
        gms_mqtt_online() ? "true" : "false",
        gms_mqtt2_online() ? "true" : "false",

        ipjs,
        apjs,

        s.uptime_ms,
        s.sensor_test_active ? "true" : "false",
        s.sensor_test_active ? s.sensor_test_channel : -1,
        (double)(s.sensor_test_active ? s.sensor_test_ma : 0.0f),
        (double)(s.sensor_test_active
            ? s.sensor_real_ma[s.sensor_test_channel] : 0.0f),
        s.sensor_test_remaining_s,

        /* Temperature A0 */

        (double)s.volts[GMS_SENSOR_TEMP],
        ma_json[GMS_SENSOR_TEMP],
        scaled_json[GMS_SENSOR_TEMP],
        (unsigned)s.alarm[GMS_SENSOR_TEMP],
        gms_alarm_name(s.alarm[GMS_SENSOR_TEMP]),
        (int)s.raw[GMS_SENSOR_TEMP],
        s.adc_ok[GMS_SENSOR_TEMP]
            ? "true" : "false",
        s.loop_ok[GMS_SENSOR_TEMP]
            ? "true" : "false",

        /* Level A2 */

        (double)s.volts[GMS_SENSOR_LEVEL],
        ma_json[GMS_SENSOR_LEVEL],
        scaled_json[GMS_SENSOR_LEVEL],
        scaled_json[GMS_SENSOR_LEVEL],
        (unsigned)s.alarm[GMS_SENSOR_LEVEL],
        gms_alarm_name(s.alarm[GMS_SENSOR_LEVEL]),
        (int)s.raw[GMS_SENSOR_LEVEL],
        s.adc_ok[GMS_SENSOR_LEVEL]
            ? "true" : "false",
        s.loop_ok[GMS_SENSOR_LEVEL]
            ? "true" : "false",

        /* Pressure A3 */

        (double)s.volts[GMS_SENSOR_PRESSURE],
        ma_json[GMS_SENSOR_PRESSURE],
        scaled_json[GMS_SENSOR_PRESSURE],
        scaled_json[GMS_SENSOR_PRESSURE],
        (unsigned)s.alarm[GMS_SENSOR_PRESSURE],
        gms_alarm_name(s.alarm[GMS_SENSOR_PRESSURE]),
        s.pressure_fault_test_active ? "true" : "false",
        (double)s.pressure_real_ma,
        (double)s.pressure_real_voltage,
        s.pressure_fault_test_remaining_s,
        (int)s.raw[GMS_SENSOR_PRESSURE],
        s.adc_ok[GMS_SENSOR_PRESSURE]
            ? "true" : "false",
        s.loop_ok[GMS_SENSOR_PRESSURE]
            ? "true" : "false",

        /* Tank geometry and optional reference correction */
        (double)tank.length_cm, (double)tank.width_cm,
        (double)tank.height_cm, (double)(tank.height_cm * 10.0f),
        tank_mm_json, tank_l_json,
        tank.use_two_point_reference ? "true" : "false",
        (s.sensor_test_active && s.sensor_test_channel == GMS_SENSOR_LEVEL)
            ? "BENCH_SENSOR_TEST" : "REAL_HDL300_A2",

        /* EWIG and clearly marked volatile bench simulation */

        s.ewig_ok ? "true" : "false",
        (unsigned)s.ewig_raw[0],
        (unsigned)s.ewig_raw[1],
        sim_engine_json,
        bench_json[0], /* rpm_calc */
        bench_json[1], /* battery_v */
        bench_json[2], /* freq_hz */
        bench_json[3], /* load_pct */
        bench_json[4], /* v_l1 */
        bench_json[5], /* v_l2 */
        bench_json[6], /* v_l3 */
        bench_json[7], /* v_l12 */
        bench_json[8], /* v_l23 */
        bench_json[9], /* v_l31 */
        bench_json[10], /* i_l1 */
        bench_json[11], /* i_l2 */
        bench_json[12], /* i_l3 */
        bench_json[13], /* p_kw */
        bench_json[14], /* q_kvar */
        bench_json[15], /* s_kva */
        bench_json[16], /* pf */
        sim_hours_json,
        sim_due_json,
        sim_service_json,
        bench_json[17], /* fuel_est_l */
        sim_enabled ? "true" : "false"
    );

    if (
        n < 0 ||
        (size_t)n >= sizeof(payload)
    ) {
        ESP_LOGE(
            TAG,
            "Status JSON buffer insufficient"
        );

        return respond_json(
            req,
            500,
            "{\"error\":\"Status JSON terlalu besar\"}"
        );
    }

    return respond_json(
        req,
        200,
        payload
    );
}

/* =====================================================
   GET WIFI AND MQTT CONFIGURATION

   GET /api/config
   ===================================================== */

static esp_err_t get_config(httpd_req_t *req)
{
    if (!authorized(req)) {
        return respond_json(
            req,
            401,
            "{\"error\":\"Login diperlukan\"}"
        );
    }

    char ssid[100];
    char ip[50];
    char gateway[50];
    char mask[50];
    char dns[50];
    char host[210];

    json_string(
        gms_settings.ssid,
        ssid,
        sizeof(ssid)
    );

    json_string(
        gms_settings.ip,
        ip,
        sizeof(ip)
    );

    json_string(
        gms_settings.gateway,
        gateway,
        sizeof(gateway)
    );

    json_string(
        gms_settings.mask,
        mask,
        sizeof(mask)
    );

    json_string(
        gms_settings.dns,
        dns,
        sizeof(dns)
    );

    json_string(
        gms_settings.mqtt_host,
        host,
        sizeof(host)
    );

    char payload[1400];

    int n = snprintf(
        payload,
        sizeof(payload),

        "{\"csrf\":\"%s\","
        "\"ssid\":%s,"
        "\"wifi_password_saved\":%s,"
        "\"static_ip\":%s,"
        "\"ip\":%s,"
        "\"gateway\":%s,"
        "\"mask\":%s,"
        "\"dns\":%s,"
        "\"mqtt_host\":%s,"
        "\"mqtt_port\":%u,"
        "\"mqtt_token_saved\":%s,"
        "\"interval_s\":%u,"
        "\"mqtt2_enabled\":%s,"
        "\"mqtt2_host\":\"%s\","
        "\"mqtt2_port\":%u,"
        "\"mqtt2_token_saved\":%s,"
        "\"mqtt2_connected\":%s}",

        s_session.csrf,
        ssid,

        gms_settings.wifi_password[0]
            ? "true" : "false",

        gms_settings.static_ip
            ? "true" : "false",

        ip,
        gateway,
        mask,
        dns,
        host,

        (unsigned)gms_settings.mqtt_port,

        gms_settings.mqtt_token[0]
            ? "true" : "false",

        (unsigned)gms_settings.interval_s,
        gms_mqtt2_settings.enabled ? "true" : "false",
        gms_mqtt2_settings.host,
        (unsigned)gms_mqtt2_settings.port,
        gms_mqtt2_settings.token[0] ? "true" : "false",
        gms_mqtt2_online() ? "true" : "false"
    );

    if (
        n < 0 ||
        (size_t)n >= sizeof(payload)
    ) {
        return respond_json(
            req,
            500,
            "{\"error\":\"Respons konfigurasi terlalu besar\"}"
        );
    }

    return respond_json(
        req,
        200,
        payload
    );
}

/* =====================================================
   SAVE WIFI CONFIGURATION

   POST /api/config/wifi
   ===================================================== */

static esp_err_t save_wifi(httpd_req_t *req)
{
    if (!csrf_ok(req)) {
        return respond_json(
            req,
            403,
            "{\"error\":\"Sesi/CSRF tidak valid\"}"
        );
    }

    char body[MAX_BODY + 1] = {0};

    if (
        request_body(
            req,
            body,
            sizeof(body)
        ) != ESP_OK
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Form terlalu besar atau tidak valid\"}"
        );
    }

    gms_settings_t next = gms_settings;

    char mode[8] = {0};
    char pw[65] = {0};

    if (
        !form_read(
            body, "ssid",
            next.ssid,
            sizeof(next.ssid)
        ) ||

        !form_read(
            body, "mode",
            mode,
            sizeof(mode)
        ) ||

        !form_read(
            body, "wifi_password",
            pw,
            sizeof(pw)
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Input SSID/IP belum lengkap\"}"
        );
    }

    if (
        !check_string(
            next.ssid,
            32,
            true
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"SSID tidak valid\"}"
        );
    }

    if (pw[0]) {
        if (
            strlen(pw) < 8 ||
            strlen(pw) > 63
        ) {
            return respond_json(
                req,
                400,
                "{\"error\":\"Password Wi-Fi minimal 8 karakter\"}"
            );
        }

        strcpy(
            next.wifi_password,
            pw
        );
    }

    next.static_ip =
        strcmp(mode, "static") == 0;

    if (
        strcmp(mode, "dhcp") != 0 &&
        !next.static_ip
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Mode IP tidak dikenal\"}"
        );
    }

    if (
        !form_read(
            body, "ip",
            next.ip,
            sizeof(next.ip)
        ) ||

        !form_read(
            body, "gateway",
            next.gateway,
            sizeof(next.gateway)
        ) ||

        !form_read(
            body, "mask",
            next.mask,
            sizeof(next.mask)
        ) ||

        !form_read(
            body, "dns",
            next.dns,
            sizeof(next.dns)
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Form IP tidak lengkap\"}"
        );
    }

    if (
        next.ssid[0] &&
        (
            strlen(next.wifi_password) < 8 ||
            strlen(next.wifi_password) > 63
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Isi password Wi-Fi minimal 8 karakter\"}"
        );
    }

    if (
        next.static_ip &&
        (
            !ip_ok(next.ip) ||
            !ip_ok(next.gateway) ||
            !ip_ok(next.mask) ||
            !ip_ok(next.dns)
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Alamat IP/Gateway/Mask/DNS harus IPv4 valid\"}"
        );
    }

    if (
        gms_store_save_wifi(&next) != ESP_OK
    ) {
        return respond_json(
            req,
            500,
            "{\"error\":\"Gagal menyimpan NVS\"}"
        );
    }

    esp_err_t er = respond_json(
        req,
        200,
        "{\"ok\":true,\"restart\":true,"
        "\"note\":\"ESP32 reboot; alamat AP tetap 192.168.4.1\"}"
    );

    gms_schedule_restart();

    return er;
}

/* =====================================================
   SAVE MQTT CONFIGURATION

   POST /api/config/mqtt
   ===================================================== */

static esp_err_t save_mqtt(httpd_req_t *req)
{
    if (!csrf_ok(req)) {
        return respond_json(
            req,
            403,
            "{\"error\":\"Sesi/CSRF tidak valid\"}"
        );
    }

    char body[MAX_BODY + 1] = {0};
    char port[8] = {0};
    char interval[8] = {0};
    char token[161] = {0};

    if (
        request_body(
            req,
            body,
            sizeof(body)
        ) != ESP_OK
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Form tidak valid\"}"
        );
    }

    gms_settings_t next = gms_settings;

    if (
        !form_read(
            body, "host",
            next.mqtt_host,
            sizeof(next.mqtt_host)
        ) ||

        !form_read(
            body, "port",
            port,
            sizeof(port)
        ) ||

        !form_read(
            body, "interval",
            interval,
            sizeof(interval)
        ) ||

        !form_read(
            body, "token",
            token,
            sizeof(token)
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Form MQTT belum lengkap\"}"
        );
    }

    if (
        !hostname_ok(next.mqtt_host) ||
        !is_all_digits(port) ||
        !is_all_digits(interval)
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Host, port, atau interval tidak valid\"}"
        );
    }

    unsigned long p =
        strtoul(port, NULL, 10);

    unsigned long sec =
        strtoul(interval, NULL, 10);

    if (
        p < 1 ||
        p > 65535 ||
        sec < 5 ||
        sec > 3600
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Port harus 1-65535; interval 5-3600 detik\"}"
        );
    }

    next.mqtt_port = (uint16_t)p;
    next.interval_s = (uint16_t)sec;

    if (token[0]) {
        if (
            !check_string(
                token,
                160,
                false
            )
        ) {
            return respond_json(
                req,
                400,
                "{\"error\":\"Token tidak valid\"}"
            );
        }

        strcpy(
            next.mqtt_token,
            token
        );
    }

    if (
        gms_store_save_mqtt(&next) != ESP_OK
    ) {
        return respond_json(
            req,
            500,
            "{\"error\":\"Gagal menyimpan NVS\"}"
        );
    }

    esp_err_t er = respond_json(
        req,
        200,
        "{\"ok\":true,\"restart\":true,"
        "\"note\":\"MQTT akan dimulai ulang dengan TLS\"}"
    );

    gms_schedule_restart();

    return er;
}

/* =====================================================
    SAVE SECONDARY MQTT, independent NVS; reboot to apply.
    POST /api/config/mqtt2
    ===================================================== */
static esp_err_t save_mqtt2(httpd_req_t *req)
{
    if (!csrf_ok(req))
        return respond_json(req, 403, "{\"error\":\"Sesi/CSRF tidak valid\"}");
    char body[MAX_BODY + 1] = {0};
    char host[97] = {0}, port[8] = {0};
    char token[161] = {0}, enabled[8] = {0};
    if (request_body(req, body, sizeof(body)) != ESP_OK ||
        !form_read(body, "host", host, sizeof(host)) ||
        !form_read(body, "port", port, sizeof(port)) ||
        !form_read(body, "token", token, sizeof(token)) ||
        !form_read(body, "enabled", enabled, sizeof(enabled)))
        return respond_json(req, 400, "{\"error\":\"Form MQTT 2 tidak lengkap\"}");
    if (!hostname_ok(host) || !is_all_digits(port) ||
        (strcmp(enabled,"true") && strcmp(enabled,"false")))
        return respond_json(req, 400, "{\"error\":\"Input MQTT 2 tidak valid\"}");
    unsigned long n = strtoul(port, NULL, 10);
    if (n < 1 || n > 65535)
        return respond_json(req, 400, "{\"error\":\"Port MQTT 2 tidak valid\"}");
    gms_mqtt2_config_t next;
    gms_mqtt2_get_config(&next);
    strcpy(next.host, host);
    next.port = (uint16_t)n;
    next.enabled = strcmp(enabled, "true") == 0;
    if (token[0]) {
        if (!check_string(token,160,false))
            return respond_json(req,400,"{\"error\":\"Token tidak valid\"}");
        strcpy(next.token, token);
    }
    if (next.enabled && !next.token[0])
        return respond_json(req,400,"{\"error\":\"Token diperlukan saat MQTT 2 aktif\"}");
    if (gms_mqtt2_save_config(&next) != ESP_OK)
        return respond_json(req,500,"{\"error\":\"Gagal menyimpan MQTT 2 ke NVS\"}");
    esp_err_t er = respond_json(req,200,
        "{\"ok\":true,\"restart\":true,\"note\":\"MQTT 2 disimpan. Restart untuk menerapkan\"}");
    gms_schedule_restart();
    return er;
}

/* =====================================================
    CHANGE ADMIN PASSWORD

   POST /api/config/admin
   ===================================================== */

static esp_err_t change_admin(httpd_req_t *req)
{
    if (!csrf_ok(req)) {
        return respond_json(
            req,
            403,
            "{\"error\":\"Sesi/CSRF tidak valid\"}"
        );
    }

    char body[MAX_BODY + 1] = {0};
    char old[130] = {0};
    char pw[130] = {0};

    if (
        request_body(
            req,
            body,
            sizeof(body)
        ) != ESP_OK ||

        !form_read(
            body, "old_password",
            old,
            sizeof(old)
        ) ||

        !form_read(
            body, "new_password",
            pw,
            sizeof(pw)
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Input tidak valid\"}"
        );
    }

    esp_err_t er =
        gms_store_change_admin(
            old,
            pw
        );

    memset(old, 0, sizeof(old));
    memset(pw, 0, sizeof(pw));

    if (er != ESP_OK) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Password lama salah atau password baru minimal 12 karakter\"}"
        );
    }

    memset(
        &s_session,
        0,
        sizeof(s_session)
    );

    return respond_json(
        req,
        200,
        "{\"ok\":true,\"note\":\"Password diubah, silakan login ulang\"}"
    );
}

/* =====================================================
   SENSOR CONFIGURATION JSON FORMATTER
   ===================================================== */

static int sensor_json_format(
    char *out,
    size_t capacity,
    const gms_sensor_config_t *c
)
{
    return snprintf(
        out,
        capacity,

        "{"
        "\"input_min_ma\":%.4f,"
        "\"input_max_ma\":%.4f,"
        "\"output_min\":%.4f,"
        "\"output_max\":%.4f,"
        "\"warning_low\":%.4f,"
        "\"critical_low\":%.4f,"
        "\"warning_high\":%.4f,"
        "\"critical_high\":%.4f"
        "}",

        (double)c->input_min_ma,
        (double)c->input_max_ma,
        (double)c->output_min,
        (double)c->output_max,
        (double)c->warning_low,
        (double)c->critical_low,
        (double)c->warning_high,
        (double)c->critical_high
    );
}

/* =====================================================
   GET SENSOR CONFIGURATION

   GET /api/sensors/config
   ===================================================== */

static esp_err_t get_sensor_config(
    httpd_req_t *req
)
{
    if (!authorized(req)) {
        return respond_json(
            req,
            401,
            "{\"error\":\"Login diperlukan\"}"
        );
    }

    gms_sensor_settings_t settings;

    gms_sensor_get_settings(&settings);

    char temp_json[320];
    char level_json[320];
    char pressure_json[320];

    int a = sensor_json_format(
        temp_json,
        sizeof(temp_json),
        &settings.temperature
    );

    int b = sensor_json_format(
        level_json,
        sizeof(level_json),
        &settings.level
    );

    int c = sensor_json_format(
        pressure_json,
        sizeof(pressure_json),
        &settings.pressure
    );

    if (
        a < 0 ||
        (size_t)a >= sizeof(temp_json) ||
        b < 0 ||
        (size_t)b >= sizeof(level_json) ||
        c < 0 ||
        (size_t)c >= sizeof(pressure_json)
    ) {
        return respond_json(
            req,
            500,
            "{\"error\":\"Gagal menyiapkan konfigurasi sensor\"}"
        );
    }

    char payload[1200];

    int n = snprintf(
        payload,
        sizeof(payload),

        "{"
        "\"ok\":true,"
        "\"csrf\":\"%s\","
        "\"temperature\":%s,"
        "\"level\":%s,"
        "\"pressure\":%s"
        "}",

        s_session.csrf,
        temp_json,
        level_json,
        pressure_json
    );

    if (
        n < 0 ||
        (size_t)n >= sizeof(payload)
    ) {
        return respond_json(
            req,
            500,
            "{\"error\":\"Respons sensor terlalu besar\"}"
        );
    }

    return respond_json(
        req,
        200,
        payload
    );
}

/* =====================================================
   STRICT FLOAT PARSER
   ===================================================== */

static bool parse_float_strict(
    const char *text,
    float *out
)
{
    if (
        text == NULL ||
        out == NULL ||
        text[0] == '\0'
    ) {
        return false;
    }

    errno = 0;

    char *end = NULL;

    float value = strtof(
        text,
        &end
    );

    if (
        errno != 0 ||
        end == text ||
        *end != '\0' ||
        !isfinite(value)
    ) {
        return false;
    }

    *out = value;

    return true;
}

static bool sensor_form_float(
    const char *body,
    const char *prefix,
    const char *field,
    float *out
)
{
    char name[64];
    char value[40];

    int n = snprintf(
        name,
        sizeof(name),
        "%s_%s",
        prefix,
        field
    );

    if (
        n < 0 ||
        (size_t)n >= sizeof(name)
    ) {
        return false;
    }

    if (
        !form_read(
            body,
            name,
            value,
            sizeof(value)
        )
    ) {
        return false;
    }

    return parse_float_strict(
        value,
        out
    );
}

/* =====================================================
   READ SENSOR CONFIGURATION FROM FORM
   ===================================================== */

static bool sensor_form_read_config(
    const char *body,
    const char *prefix,
    gms_sensor_config_t *config
)
{
    if (
        body == NULL ||
        prefix == NULL ||
        config == NULL
    ) {
        return false;
    }

    return (
        sensor_form_float(
            body,
            prefix,
            "input_min_ma",
            &config->input_min_ma
        ) &&

        sensor_form_float(
            body,
            prefix,
            "input_max_ma",
            &config->input_max_ma
        ) &&

        sensor_form_float(
            body,
            prefix,
            "output_min",
            &config->output_min
        ) &&

        sensor_form_float(
            body,
            prefix,
            "output_max",
            &config->output_max
        ) &&

        sensor_form_float(
            body,
            prefix,
            "warning_low",
            &config->warning_low
        ) &&

        sensor_form_float(
            body,
            prefix,
            "critical_low",
            &config->critical_low
        ) &&

        sensor_form_float(
            body,
            prefix,
            "warning_high",
            &config->warning_high
        ) &&

        sensor_form_float(
            body,
            prefix,
            "critical_high",
            &config->critical_high
        )
    );
}

/* =====================================================
   SAVE SENSOR CALIBRATION AND THRESHOLD

   POST /api/sensors/config

   Updates:
   - Temperature A0
   - Level A2
   - Pressure A3

   ===================================================== */

static esp_err_t save_sensor_config(
    httpd_req_t *req
)
{
    if (!csrf_ok(req)) {
        return respond_json(
            req,
            403,
            "{\"error\":\"Sesi/CSRF tidak valid\"}"
        );
    }

    char body[MAX_BODY + 1] = {0};

    if (
        request_body(
            req,
            body,
            sizeof(body)
        ) != ESP_OK
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Form sensor terlalu besar atau tidak valid\"}"
        );
    }

    gms_sensor_settings_t next = {0};

    if (
        !sensor_form_read_config(
            body,
            "temperature",
            &next.temperature
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Konfigurasi suhu tidak lengkap atau tidak valid\"}"
        );
    }

    if (
        !sensor_form_read_config(
            body,
            "level",
            &next.level
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Konfigurasi level tidak lengkap atau tidak valid\"}"
        );
    }

    if (
        !sensor_form_read_config(
            body,
            "pressure",
            &next.pressure
        )
    ) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Konfigurasi pressure tidak lengkap atau tidak valid\"}"
        );
    }

    /*
     * Store layer performs:
     * - Input validation
     * - Output validation
     * - Threshold ordering validation
     * - NVS persistence
     * - Active configuration update
     */

    esp_err_t er =
        gms_sensor_save_settings(&next);

    if (er == ESP_ERR_INVALID_ARG) {
        return respond_json(
            req,
            400,
            "{\"error\":\"Rentang kalibrasi atau urutan threshold tidak valid\"}"
        );
    }

    if (er != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Sensor configuration save failed: %s",
            esp_err_to_name(er)
        );

        return respond_json(
            req,
            500,
            "{\"error\":\"Gagal menyimpan konfigurasi sensor ke NVS\"}"
        );
    }

    ESP_LOGI(
        TAG,
        "Sensor calibration and thresholds updated"
    );

    return respond_json(
        req,
        200,
        "{"
        "\"ok\":true,"
        "\"restart\":false,"
        "\"saved\":true,"
        "\"message\":\"Kalibrasi dan threshold berhasil disimpan\""
        "}"
    );
}

/* =====================================================
   TANK GEOMETRY — GET/POST /api/tank/config
   Stored independently from sensor NVS and MQTT settings.
   ===================================================== */
static esp_err_t get_tank_config(httpd_req_t *req)
{
    if (!authorized(req))
        return respond_json(req, 401, "{\"error\":\"Login diperlukan\"}");
    gms_tank_config_t t;
    gms_tank_get_config(&t);
    char result[550];
    int n = snprintf(result, sizeof(result),
        "{\"ok\":true,\"length_cm\":%.3f,\"width_cm\":%.3f,"
        "\"height_cm\":%.3f,\"calibration_enabled\":%s,"
        "\"zero_ma\":%.4f,\"reference_ma\":%.4f,"
        "\"reference_depth_mm\":%.2f}",
        (double)t.length_cm, (double)t.width_cm, (double)t.height_cm,
        t.use_two_point_reference ? "true" : "false",
        (double)t.zero_ma, (double)t.reference_ma, (double)t.reference_depth_mm);
    if (n<0 || (size_t)n >= sizeof(result))
        return respond_json(req, 500, "{\"error\":\"Buffer terlalu kecil\"}");
    return respond_json(req, 200, result);
}

static esp_err_t save_tank_config(httpd_req_t *req)
{
    if (!csrf_ok(req))
        return respond_json(req, 403, "{\"error\":\"Sesi/CSRF tidak valid\"}");
    char body[MAX_BODY + 1] = {0};
    if (request_body(req, body, sizeof(body)) != ESP_OK)
        return respond_json(req, 400, "{\"error\":\"Form tidak valid\"}");
    gms_tank_config_t next = {.version=1};
    char enabled[12] = {0};
    if (!form_read(body, "tank_calibration_enabled", enabled, sizeof(enabled)) ||
        (strcmp(enabled,"0") && strcmp(enabled,"1")) ||
        !sensor_form_float(body,"tank","length_cm", &next.length_cm) ||
        !sensor_form_float(body,"tank","width_cm", &next.width_cm) ||
        !sensor_form_float(body,"tank","height_cm", &next.height_cm) ||
        !sensor_form_float(body,"tank","zero_ma", &next.zero_ma) ||
        !sensor_form_float(body,"tank","reference_ma", &next.reference_ma) ||
        !sensor_form_float(body,"tank","reference_depth_mm", &next.reference_depth_mm))
        return respond_json(req, 400, "{\"error\":\"Parameter tangki tidak lengkap\"}");
    next.use_two_point_reference = strcmp(enabled,"1") == 0;
    esp_err_t er = gms_tank_save_config(&next);
    if (er == ESP_ERR_INVALID_ARG)
        return respond_json(req, 400, "{\"error\":\"Dimensi/kalibrasi tidak valid. Pastikan reference_ma > zero_ma + 0.05 mA\"}");
    if (er != ESP_OK)
        return respond_json(req, 500, "{\"error\":\"Gagal menyimpan ke NVS\"}");
    return respond_json(req, 200,
        "{\"ok\":true,\"note\":\"Konfigurasi tangki disimpan di NVS. Pengukuran diperbarui pada siklus sensor berikutnya.\"}");
}

/* =====================================================
   HTTP SERVER INITIALIZATION
   ===================================================== */

void gms_web_start(void)
{
    httpd_config_t cfg =
        HTTPD_DEFAULT_CONFIG();

    cfg.max_uri_handlers = 24;
    cfg.stack_size = 10240;
    cfg.max_open_sockets = 5;
    cfg.lru_purge_enable = true;

    esp_err_t er = httpd_start(
        &s_server,
        &cfg
    );

    if (er != ESP_OK) {
        ESP_LOGE(
            TAG,
            "HTTP server start failed: %s",
            esp_err_to_name(er)
        );

        return;
    }

#define ROUTE(path, verb, fn) do {                  \
    const httpd_uri_t route = {                    \
        .uri = path,                               \
        .method = verb,                            \
        .handler = fn                              \
    };                                             \
    ESP_ERROR_CHECK(                               \
        httpd_register_uri_handler(                \
            s_server,                              \
            &route                                 \
        )                                          \
    );                                             \
} while (0)

    /* Pages */

    ROUTE("/", HTTP_GET, page_dash);
    ROUTE("/login", HTTP_GET, page_login);
    ROUTE("/login.html", HTTP_GET, page_login);

    ROUTE("/dashboard", HTTP_GET, page_dash);
    ROUTE("/dashboard.html", HTTP_GET, page_dash);

    ROUTE("/settings", HTTP_GET, page_settings);
    ROUTE("/setup.html", HTTP_GET, page_settings);

    /* Static assets */

    ROUTE(
        "/assets/style.css",
        HTTP_GET,
        get_css
    );

    ROUTE(
        "/assets/app.js",
        HTTP_GET,
        get_js
    );

    /* Authentication */

    ROUTE("/api/login", HTTP_POST, post_login);
    ROUTE("/api/logout", HTTP_POST, post_logout);

    /* Monitoring */

    ROUTE("/api/status", HTTP_GET, get_status);
    ROUTE("/api/sim/state", HTTP_POST, post_sim_state);
    ROUTE("/api/bench/sensor-test", HTTP_POST, post_sensor_test);
    ROUTE("/api/tank/config", HTTP_GET, get_tank_config);
    ROUTE("/api/tank/config", HTTP_POST, save_tank_config);

    /* Existing configuration */

    ROUTE("/api/config", HTTP_GET, get_config);

    ROUTE(
        "/api/config/wifi",
        HTTP_POST,
        save_wifi
    );

    ROUTE(
        "/api/config/mqtt",
        HTTP_POST,
        save_mqtt
    );

    ROUTE("/api/config/mqtt2", HTTP_POST, save_mqtt2);

    ROUTE(
        "/api/config/admin",
        HTTP_POST,
        change_admin
    );

    /* Sensor calibration and threshold */

    ROUTE(
        "/api/sensors/config",
        HTTP_GET,
        get_sensor_config
    );

    ROUTE(
        "/api/sensors/config",
        HTTP_POST,
        save_sensor_config
    );

#undef ROUTE

    ESP_LOGI(
        TAG,
        "HTTP dashboard running: "
        "http://192.168.4.1 (AP) or STA IP"
    );

    ESP_LOGI(
        TAG,
        "Sensor configuration API registered"
    );
}