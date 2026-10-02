# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# ESP32Drop 接收端（AWDL + AirDrop）的 IDF 移植，上游 0BSD，见 README.md。
# IDF port of the ESP32Drop receiver (AWDL + AirDrop); upstream is 0BSD, see README.md.
idf_component_register(
    SRCS
        "src/awdl/port/awdl_port_esp32.cpp"
        "src/airdrop/port/ad_port_esp32.cpp"
        "src/esp32drop.cpp"
    INCLUDE_DIRS "include" "src"
    REQUIRES esp_wifi esp_netif esp_event esp_timer lwip mbedtls nvs_flash heap
)
# 上游按 Arduino 默认警告级别编写。/ Upstream is written against Arduino's default warning level.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-missing-field-initializers -Wno-format -Wno-maybe-uninitialized -Wno-volatile -Wno-comment)
