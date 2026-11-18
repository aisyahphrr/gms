from pathlib import Path
import re
import sys

# ==================================================
# GMS INTERNAL - ESP-IDF 6.0.2 REPAIR
# Target Hardware: ESP32-S3
#
# ADS1115:
# A0 = Temperature
# A1 = Not Used
# A2 = Level
# A3 = Pressure
#
# EWIG:
# UART1 TX15 RX16 RTS14
#
# This script repairs the existing firmware.
# It does NOT replace dashboard or sensor logic.
# ==================================================

ROOT = Path(__file__).resolve().parent
MAIN = ROOT / "main"

MAIN_C = MAIN / "main.c"
WEB_C = MAIN / "gms_web.c"
STORE_C = MAIN / "gms_store.c"
CMAKE = MAIN / "CMakeLists.txt"


def read_file(path):
    if not path.exists():
        print(f"ERROR: File tidak ditemukan: {path}")
        sys.exit(1)

    return path.read_text(encoding="utf-8")


def save_file(path, content):
    backup = path.with_name(path.name + ".bak")

    if not backup.exists():
        backup.write_bytes(path.read_bytes())

    path.write_text(content, encoding="utf-8")

    print(f"OK: {path.name}")


def replace_required(content, old, new, description):
    if old in content:
        return content.replace(old, new)

    if new in content:
        print(f"Sudah diperbaiki: {description}")
        return content

    raise RuntimeError(
        f"Pola tidak ditemukan: {description}. "
        "File asli tidak diubah."
    )


# ==================================================
# 1. FIX MAIN.C
# ==================================================

def fix_main():

    code = read_file(MAIN_C)

    # Fix password AP:
    # Source buffer ap_pw hanya berukuran 20 byte.
    # Jangan membaca menggunakan batas 64 byte.

    code, count = re.subn(
        r"strnlen\(\s*ap_pw\s*,\s*"
        r"sizeof\(cfg_ap\.ap\.password\)\s*\)",
        "strnlen(ap_pw, sizeof(ap_pw))",
        code
    )

    if count:
        print("FIX: AP password buffer")

    # Fix misleading indentation pressure.

    code = code.replace(
        "if(bar<0)bar=0;if(bar>10)bar=10;",
        "if (bar < 0) { bar = 0; }\n"
        "        if (bar > 10) { bar = 10; }"
    )

    # Fix AP SSID snprintf truncation.
    # Digunakan hanya apabila kode lama masih ada.

    old_ap = (
        'snprintf((char *)cfg_ap.ap.ssid,'
        'sizeof(cfg_ap.ap.ssid),"%s",s_ap_name);'
    )

    new_ap = (
        "size_t ap_ssid_len = "
        "strnlen(s_ap_name, sizeof(cfg_ap.ap.ssid));\n"
        "    memcpy(cfg_ap.ap.ssid, s_ap_name, ap_ssid_len);"
    )

    if old_ap in code:
        code = code.replace(old_ap, new_ap)

    # Fix panjang SSID AP.

    code = code.replace(
        "cfg_ap.ap.ssid_len="
        "strlen((char *)cfg_ap.ap.ssid);",
        "cfg_ap.ap.ssid_len = "
        "(uint8_t)strnlen("
        "s_ap_name, sizeof(cfg_ap.ap.ssid));"
    )

    # Fix Station SSID.

    old_sta = (
        'snprintf((char *)cfg_sta.sta.ssid,'
        'sizeof(cfg_sta.sta.ssid),"%s",gms_settings.ssid);'
    )

    new_sta = (
        "memcpy(cfg_sta.sta.ssid, "
        "gms_settings.ssid, "
        "strnlen(gms_settings.ssid, "
        "sizeof(cfg_sta.sta.ssid)));"
    )

    if old_sta in code:
        code = code.replace(old_sta, new_sta)

    # Fix Station Password.

    old_pw = (
        'snprintf((char *)cfg_sta.sta.password,'
        'sizeof(cfg_sta.sta.password),'
        '"%s",gms_settings.wifi_password);'
    )

    new_pw = (
        "memcpy(cfg_sta.sta.password, "
        "gms_settings.wifi_password, "
        "strnlen(gms_settings.wifi_password, "
        "sizeof(cfg_sta.sta.password)));"
    )

    if old_pw in code:
        code = code.replace(old_pw, new_pw)

    # Periksa konfigurasi ADC tetap ada.

    if "read_analog(0)" not in code and \
       "read_analog(\n            0" not in code:
        print("INFO: Periksa mapping ADC secara manual")

    save_file(MAIN_C, code)


# ==================================================
# 2. FIX GMS_WEB.C
# ==================================================

def fix_web():

    code = read_file(WEB_C)

    # Fix request_body()

    code = replace_required(
        code,
        "if(r<=0)return ESP_FAIL;got+=r;",
        "if (r <= 0) {\n"
        "            return ESP_FAIL;\n"
        "        }\n"
        "        got += r;",
        "request_body"
    )

    # Fix json_string()

    code = replace_required(
        code,
        """if(cap<3)return;out[j++]='"';""",
        """if (cap < 3) {\n"""
        """        return;\n"""
        """    }\n"""
        """    out[j++] = '"';""",
        "json_string"
    )

    # Fix configuration JSON buffer.

    code = code.replace(
        "char payload[700];",
        "char payload[1024];"
    )

    save_file(WEB_C, code)


# ==================================================
# 3. FIX GMS_STORE.C
# ==================================================

def fix_store():

    code = read_file(STORE_C)

    # ESP-IDF 6 menggunakan Mbed TLS 4.
    # Migrasi legacy SHA256 ke PSA Crypto.

    if '#include "mbedtls/sha256.h"' in code:
        code = code.replace(
            '#include "mbedtls/sha256.h"',
            '#include "psa/crypto.h"'
        )

    if "#include <stdlib.h>" not in code:
        code = code.replace(
            "#include <stdint.h>",
            "#include <stdint.h>\n#include <stdlib.h>"
        )

    old_hash = (
        "mbedtls_sha256(buf,n+16,output,0);"
    )

    new_hash = """
    size_t hash_len = 0;

    psa_status_t status = psa_hash_compute(
        PSA_ALG_SHA_256,
        buf,
        n + 16,
        output,
        32,
        &hash_len
    );

    if (
        status != PSA_SUCCESS ||
        hash_len != 32
    ) {
        ESP_LOGE(
            TAG,
            "SHA256 failed: %d",
            (int)status
        );

        abort();
    }
"""

    if old_hash in code:
        code = code.replace(old_hash, new_hash)

    elif "psa_hash_compute" not in code:
        raise RuntimeError(
            "Implementasi SHA256 tidak dikenali. "
            "gms_store.c tidak diubah."
        )

    save_file(STORE_C, code)


# ==================================================
# 4. VERIFY CMAKE
# ==================================================

def verify_cmake():

    code = read_file(CMAKE)

    required = [
        "mbedtls",
        "esp_wifi",
        "esp_netif",
        "esp_http_server",
        "nvs_flash",
    ]

    missing = [
        item
        for item in required
        if item not in code
    ]

    if missing:
        raise RuntimeError(
            "Dependency CMake belum lengkap: "
            + ", ".join(missing)
        )

    print("OK: CMake dependencies")


# ==================================================
# 5. MAIN REPAIR
# ==================================================

def main():

    print("=" * 50)
    print("GMS INTERNAL FIRMWARE REPAIR")
    print("ESP-IDF 6.0.2 / ESP32-S3")
    print("=" * 50)

    if not MAIN.exists():
        print("ERROR: Folder main tidak ditemukan")
        sys.exit(1)

    try:
        # Validate prerequisites before changing files.
        verify_cmake()

        # Repair original source files.
        fix_main()
        fix_web()
        fix_store()

    except Exception as error:
        print(f"\nERROR: {error}")
        print("Periksa file .bak sebelum mencoba ulang.")
        sys.exit(1)

    print()
    print("=" * 50)
    print("PERBAIKAN SOURCE CODE SELESAI")
    print("=" * 50)

    print("Firmware hardware dipertahankan")
    print("Dashboard dipertahankan")
    print("ADS1115 A0/A2/A3 dipertahankan")
    print("EWIG RS485 dipertahankan")
    print("MQTT TLS dipertahankan")

    print()
    print("NEXT:")
    print("idf.py set-target esp32s3")
    print("idf.py build")


if __name__ == "__main__":
    main()