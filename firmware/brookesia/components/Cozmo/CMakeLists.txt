idf_component_register(
    SRCS "assets/img_app_cozmo.c" "Cozmo.cpp" "CozmoVoz.cpp"
    # chaves em secrets.h (fora do git); modelo em secrets.example.h
    INCLUDE_DIRS "."
    REQUIRES brookesia_core CozmoFace XiaozhiApp bsp_extra esp_websocket_client esp-sr esp_audio_effects json mbedtls esp_wifi esp_http_client
    WHOLE_ARCHIVE
)
