# Versi 0.1 — GMS local web bench
- Sensor ADS1115 A0/A2/A3 dan EWIG raw diambil dari firmware serial+MQTT yang telah diuji pada perangkat.
- AP+STA: AP pemulihan, IP STA saat terhubung.
- HTTP dashboard local: login admin, live monitoring, perangkat, pengaturan Wi-Fi/IP/MQTT, ganti admin password.
- MQTT over TLS; publish compatible telemetry GMS-001, subscribe read-only.
- Unit suhu/level tidak diklaim terkalibrasi. Tidak ada aksi cut-off relay.
- Masih membutuhkan validasi build, pengujian perangkat dan audit keamanan sebelum produksi.
