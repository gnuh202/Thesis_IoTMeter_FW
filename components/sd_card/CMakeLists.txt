idf_component_register(SRCS "sd_card.c"
                    REQUIRES esp_driver_gpio esp_driver_sdspi fatfs sdmmc spi_bus_shared freertos
                    INCLUDE_DIRS "include")
