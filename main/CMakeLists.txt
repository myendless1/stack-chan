idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_client esp_netif esp_event nvs_flash json app_update mbedtls tcp_transport esp_audio_codec esp32-camera esp_driver_uart
    EMBED_FILES "happy_face.png" "calm_face.png" "speak1_face.png" "speak2_face.png"
)
