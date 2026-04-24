idf_component_register(SRCS "main.c" "config_manager.c" "status_led.c" "storage_manager.c" "wifi_manager.c" "time_sync.c" "camera_driver.c" "video_recorder.c" "web_server.c" "mjpeg_streamer.c" "webdav_client.c" "ftp_client.c" "nas_uploader.c" "cJSON.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_http_server espressif__esp32-camera driver spiffs fatfs sdmmc esp_http_client tcp_transport mbedtls esp_netif esp_event esp_timer freertos esp_system log)
