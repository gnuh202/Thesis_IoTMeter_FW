idf_component_register(SRCS "io_expander.c"
                    REQUIRES esp_driver_gpio i2c_bus pcf8574 freertos
                    INCLUDE_DIRS "include")
