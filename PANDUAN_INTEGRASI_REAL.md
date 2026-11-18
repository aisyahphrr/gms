# GMS Internal — Firmware ESP-IDF + Dashboard REAL

## Isi paket
- `main/main.c`, `gms_web.c`, `gms_store.c`, `gms.h`: kode hardware dan web backend asli dari `GMS_Local_Web_ESP_IDF6` (dipertahankan).
- `main/www/`: aset login, monitoring, settings, CSS dan JavaScript dari UI/UX preview port 8877; sudah disesuaikan agar tidak menampilkan status simulasi saat berjalan di ESP32-S3.
- `main/CMakeLists.txt`: `EMBED_TXTFILES` sudah menyematkan seluruh file `main/www` ke firmware. Tidak perlu menjalankan Python preview server.

## Build & flash
1. Backup firmware/proyek yang sebelumnya berhasil.
2. Buka **folder ini** di VS Code dengan extension ESP-IDF / terminal ESP-IDF 6.0.2.
3. Verifikasi board benar ESP32-S3 dengan flash 16 MB; jika berbeda, ubah konfigurasi dahulu.
4. Jalankan:

```powershell
idf.py set-target esp32s3
idf.py -p COM8 build flash monitor
```

Sesuaikan COM8 dengan port perangkat sebenarnya. Catat **password admin acak dan password AP acak dari serial monitor**. Tidak ada kredensial demo123 pada firmware nyata.

5. Hubungkan laptop ke SSID AP yang dicetak di monitor; buka `http://192.168.4.1/login`.
6. Halaman settings menggunakan API ESP-IDF langsung. Simpan Wi-Fi -> reboot -> sambungkan ulang -> buka halaman settings untuk melihat IP STA aktual (jika koneksi berhasil). Jangan menganggap tombol submit langsung menjamin koneksi berhasil.
7. Simpan host dan token MQTT -> reboot; cek status TLS pada serial monitor dan dashboard.

## Penting tentang hasil integrasi
- Ini adalah integrasi kode **belum diuji flash, kompilasi ESP-IDF, atau hardware fisik** di lingkungan pembuatan paket.
- Firmware sensor dan publish MQTT sengaja tidak diubah. Tetap versi bench; nilai EWIG belum divalidasi dan relay proteksi, OTA, offline queue belum diimplementasikan.
- MQTT host tetap dropdown tiga pilihan; jika host yang tersimpan sebelumnya di NVS berbeda, dropdown secara otomatis menambah satu opsi `Server tersimpan` untuk menghindari kehilangan konfigurasi.
- Panel hasil Wi-Fi menampilkan data aktual `/api/status` **hanya ketika STA online**. IP tampil setelah memuat ulang halaman pasca-reboot. Teks simulasi pada preview sengaja dihilangkan.
- Firmware HTTP lokal **bukan HTTPS**. Pengujian hanya di AP/lab tepercaya; jangan expose port 80 ke internet.
- Login admin awal dan AP password dibuat secara acak pada firmware; lihat `README.md` untuk prosedur pemasangan dan pemulihan.
- ESP32 berpotensi dapat diakses melalui AP 192.168.4.1 meski koneksi STA tidak berhasil.

## Verifikasi integrasi statis
- Endpoint web yang dipakai frontend: `/api/login`, `/api/logout`, `/api/status`, `/api/config`, `/api/config/wifi`, `/api/config/mqtt`, `/api/config/admin`.
- Semua tersedia di `main/gms_web.c`.
- Asset embed terdaftar di `main/CMakeLists.txt`.
