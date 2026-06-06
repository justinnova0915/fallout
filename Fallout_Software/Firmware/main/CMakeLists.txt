idf_component_register(SRCS "ble.cpp" "main.cpp" "screen.cpp" "magnetic.cpp" "voltmeter.cpp" "input.cpp"
                       REQUIRES spi_flash esp_driver_uart esp_driver_gpio esp_timer bt driver esp_adc esp_driver_ledc esp_driver_i2c nvs_flash
                       INCLUDE_DIRS ""
                       EMBED_FILES "../1.jpg")
