idf_component_register(
    SRCS "mcp2515.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_spi esp_driver_gpio
)
