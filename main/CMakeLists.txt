idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_client esp_http_server esp_netif esp_event nvs_flash json app_update mbedtls tcp_transport esp_audio_codec esp32-camera esp_driver_uart
    EMBED_FILES
        "calm_face.png"
        "speak1_face.png"
        "speak2_face.png"
        "shy_face.png"
        "thinking_face.png"
        "blink_half_face.png"
        "blink_closed_face.png"
        "wink_half_face.png"
        "wink_closed_face.png"
        "heart_small_face.png"
        "heart_face.png"
        "nod_soft_face.png"
        "nod_down_face.png"
        "happy_squint_face.png"
        "happy_squint_soft_face.png"
)
