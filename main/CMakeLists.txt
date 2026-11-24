include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS ${SOLANA_OS_SRCS} ${SOLANA_OS_DEVICE_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event console esp_timer mbedtls
                  esp_app_format app_update esp_partition spi_flash fatfs vfs esp_http_client esp-tls tcp_transport lwip esp_driver_uart bt)

# Walnut-CGB CPU/PPU: -O3 + IRAM on 8-bit dispatch / draw_line (see
# WALNUT_GB_CPU_IRAM). Dual-fetch interpreter stays in flash. gbemu_app.c
# stays -Os (picker, blit, save).
set_property(SOURCE apps/gbemu/walnut_cgb_cpu.c APPEND PROPERTY COMPILE_OPTIONS
    -O3 -Wno-error
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough -Wno-type-limits)

# cJSON (espressif/cjson) parses mesh JSON with a depth cap of 16:
# CONFIG_CJSON_NESTING_LIMIT in sdkconfig.defaults.
