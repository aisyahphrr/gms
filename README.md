# GMS Internal — ESP32-S3 Local Web + MQTT (ESP-IDF 6.0.2)

**Versi demonstrasi/bench, bukan perangkat proteksi produksi.**

Folder lengkap: firmware C ESP-IDF, web UI responsif, login berbasis session/cookie, konfigurasi Wi-Fi/IP/MQTT di NVS, pembacaan tiga sensor ADS1115, komunikasi Modbus EWIG, dan publish/subscribe MQTT TLS ke ThingsBoard-compatible broker.

## 0. Arsitektur

```
Suhu (A0)  ─┐
Level (A2) ─┼─ ADS1115 / I2C SDA8 SCL9 ─┐
Tekanan(A3)┘                             ├─ ESP32-S3 ── Wi-Fi STA ─ MQTT/TLS ─ ThingsBoard / Mastify*
EWIG ── Modbus RS485 UART1 TX15 RX16 RTS14 ┘           │
                                                      └─ HTTP web dashboard
                                                        AP: 192.168.4.1
                                                        STA: alamat IP dari DHCP/static
```

* Mastify harus memiliki broker/API MQTT yang kompatibel. Integrasi data melalui REST API dashboard terpisah **belum** disertakan, dan ini bukan klon platform Mastify.

## 1. Yang sudah ada

- **Login admin:** username `admin`, password awal acak 18 karakter; ditampilkan **sekali** di Serial Monitor saat instalasi pertama. Password hanya disimpan sebagai salted SHA-256 di NVS; sumber entropi RNG perangkat diaktifkan saat inisialisasi sebelum Wi-Fi.
- **Access Point pemulihan:** `GMS-Internal-XXXX` selalu menyala, password AP random 14 karakter dan ditampilkan di Serial Monitor tiap boot; login web tetap wajib. `http://192.168.4.1`.
- **STA Wi-Fi:** setelah diatur melalui halaman settings, tampil `GMS STA IP: ...` di terminal. Browser juga bisa akses `http://<IP_STA>` jika jaringan mengizinkan perangkat saling terhubung.
- **Konfigurasi:** SSID dan password Wi-Fi; DHCP vs static IP (IP, gateway, netmask, DNS); broker MQTT TLS host, port, token dan interval publish; ubah password admin.
- **Sensor:** A0 suhu, A2 level, A3 tekanan melalui ADS1115 address 0x48, VDD 3.3 V; 56-ohm shunt *terpisah* pada masing-masing 2-wire loop transmitter yang sudah diverifikasi. Output raw, volt, mA dan validitas loop. Tekanan 0–10 bar = estimasi tanpa kalibrasi; level persen = posisi sinyal, bukan meter; suhu mA = bukan °C.
- **EWIG:** FC03 holding registers 9–20, slave ID 1, 9600 8N1; hanya komunikasi CRC dan raw R09/R10, **bukan pengukuran listrik valid**.
- **MQTT:** Topics `v1/devices/me/telemetry`, attributes dan RPC request read-only. Koneksi TLS menggunakan bundle sertifikat, SNTP sebelum koneksi. RPC tidak boleh mengendalikan relay.
- **Dashboard:** monitoring perangkat lokal dan grafik histori sementara **dalam browser**; status invalid berbeda dengan angka valid. Pengaturan melalui halaman lain setelah login.

## 2. Instalasi cepat di Windows / VS Code

1. Ekstrak ZIP di folder baru (jangan timpa project MQTT yang sudah berhasil; backup dahulu).
2. Buka folder `GMS_INTERNAL_REAL_ESP_IDF6` (folder yang berisi CMakeLists.txt) di VS Code menggunakan terminal **ESP-IDF v6.0.2**.
3. Board harus **ESP32-S3** dan memiliki flash **16 MB** sesuai pengujian hardware yang sebelumnya terdeteksi 16384k. Periksa kembali modul fisik; jika beda, jangan langsung gunakan default flash 16MB tanpa penyesuaian.
4. Pada terminal ESP-IDF PowerShell:

```powershell
Remove-Item Env:IDF_TARGET -ErrorAction SilentlyContinue
idf.py set-target esp32s3
idf.py -p COM8 build flash monitor
```

5. Pada **boot pertama**, catat:

```
GMS WEB username: admin
GMS WEB initial password: <acak 18 karakter>
Join Wi-Fi SSID: GMS-Internal-XXXX
AP Wi-Fi password: <acak 14 karakter>
GMS LOCAL WEB: http://192.168.4.1
```

Jika baris awal terlewat dan belum pernah masuk, pemulihan *bench* memerlukan reset NVS (`idf.py erase-flash` kemudian flash ulang). **Perintah ini akan menghapus SEMUA konfigurasi NVS**; jangan lakukan bila ada pengaturan lain yang belum dicadangkan. Tidak ada password admin bawaan universal.

6. Sambungkan laptop ke AP `GMS-Internal-XXXX`, buka `http://192.168.4.1`, login `admin` dengan password yang dicatat.
7. Klik **Konfigurasi**, isi SSID dan password Wi-Fi 2.4 GHz, simpan → ESP32 restart.
8. Jika terhubung ke Wi-Fi STA, Serial Monitor menampilkan **GMS STA IP:** diikuti alamat IP. Kedua akses AP dan STA tersedia; AP bisa berpindah channel mengikuti STA (karena radio yang sama).
9. Klik **Konfigurasi → MQTT**, isi host `mqtt.thingsboard.cloud`, port **8883**, dan **token device GMS-001** milik sendiri. Token MQTT disimpan di NVS, **tidak ditampilkan balik ke browser**. Simpan → restart.
10. Status MQTT/TLS bisa dicek di terminal dan halaman monitoring. Pastikan **Latest Telemetry** perangkat GMS-001 pada ThingsBoard memiliki waktu update terbaru.

## 3. Keterangan keamanan penting

- Server web di ESP-IDF ini memakai **HTTP, bukan HTTPS**. Cookie session diberi `HttpOnly`, `SameSite=Strict`, kontrol CSRF, pembatasan login, dan kata sandi admin di-*hash* dengan garam acak, **tetapi HTTP tetap tidak mengenkripsi lalu lintas/password saat login**. Gunakan hanya AP bench yang dipercaya atau jaringan laboratorium yang terisolasi. **Jangan buka port 80 ke internet**, jangan gunakan sebagai production-grade authentication.
- Password Wi-Fi dan token MQTT disimpan di NVS demo **tanpa enkripsi flash otomatis**. Untuk produksi diperlukan NVS encryption/flash encryption, HTTPS/TLS web, provisioning dan kontrol akses perusahaan.
- Token MQTT demo dikirim ke broker melalui MQTT/TLS saja; **tidak** dikirim ke frontend JavaScript. Screenshot yang menunjukkan password/token harus disamarkan. Rotasi credential yang sebelumnya tersebar direkomendasikan.
- Jangan mengaktifkan tindakan safety/cut-off melalui web, RPC atau layanan cloud. Rangkaian relay, OTA rollback, & offline flash buffering **belum diimplementasikan**.
- AP softAP selalu aktif untuk pemulihan; ini adalah keputusan bench, bukan default produksi.
- IP statis harus sesuai subnet dan tidak konflik DHCP/reservasi kantor. Jika salah, gunakan AP `192.168.4.1` untuk memperbaiki.

## 4. Perilaku sensor dan batas hardware

- PSU sensor 24 VDC dan ADC 3.3 V terpisah; A0/A2/A3 hanya menerima tegangan shunt rendah setelah pengukuran aman.
- Tiga transmitter 4–20 mA membutuhkan tiga resistor shunt sendiri dan common GND untuk rangkaian **non-isolated** yang sudah diverifikasi. Bukan rangkaian proteksi input final.
- Loop di bawah 3.6 mA / di atas 21 mA ditandai **CHECK LOOP** dan nilai tidak diklaim valid pada grafik, status, maupun cloud.
- EWIG di sisi AC220V tidak diubah oleh firmware ini. Wiring AC harus ditangani penanggung jawab kelistrikan.

## 5. Edit source

```
GMS_INTERNAL_REAL_ESP_IDF6/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── .gitignore
├── README.md
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── gms.h
    ├── gms_store.c    # pengaturan NVS & hash password
    ├── gms_web.c      # HTTP, login, session, API & saving
    ├── main.c         # hardware/AFE/EWIG/Wi-Fi/MQTT
    └── www/
        ├── login.html
        ├── dashboard.html
        ├── settings.html
        ├── style.css
        └── app.js
```

HTML/CSS/JS tertanam saat build menggunakan `EMBED_TXTFILES`. Mengubah UI membutuhkan `idf.py build flash monitor` lagi. Mengubah SSID/IP/MQTT lewat web **tidak** perlu build ulang, cukup restart otomatis dari perangkat.

## 6. Pengujian minimum setelah flash

1. AP muncul & login tidak bisa dilewati tanpa password.
2. Password awal dapat digunakan sekali lalu dapat diubah melalui Settings.
3. Sensor A0, A2, A3 muncul dengan data yang cocok dengan Serial Monitor dan validitas yang tepat.
4. IP Wi-Fi STA tercatat; ketika Wi-Fi STA gagal, AP tetap bisa dipakai.
5. Publish MQTT tiap 5 detik di ThingsBoard GMS-001 setelah host/token diisi.
6. Update IP dan MQTT lewat dashboard; pastikan konfigurasi bertahan setelah reboot.
7. Sebelum produksi audit ulang kegagalan login, pemulihan kredensial, TLS web, persistensi, storage, unit sensor dan safety.

## 7. Verifikasi build

Project disusun untuk ESP-IDF v6.0.2. **Belum dilakukan kompilasi silang aktual dengan `idf.py build` pada board pengguna**, sehingga jika muncul pesan `FAILED:` atau `error:` pertama, kirim 20-30 baris error tersebut untuk perbaikan. Jangan langsung menggunakan project sebagai firmware produksi.
#   g m s  
 