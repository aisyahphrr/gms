idf_component_register(
    SRCS "main.c" "gms_web.c" "gms_store.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_uart esp_wifi esp_netif esp_event esp_http_server
             nvs_flash mbedtls esp_timer esp_hw_support bootloader_support esp-tls lwip
    EMBED_TXTFILES "www/login.html" "www/dashboard.html" "www/settings.html"
                   "www/style.css" "www/app.js"
)
