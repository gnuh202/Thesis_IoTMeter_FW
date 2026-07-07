idf_component_register(SRCS "pcf8574.c"
                    REQUIRES esp_driver_i2c freertos
                    INCLUDE_DIRS "include")
