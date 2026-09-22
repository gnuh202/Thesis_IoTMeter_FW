idf_component_register(SRCS "ds1307.c"
                    REQUIRES esp_driver_i2c freertos
                    INCLUDE_DIRS "include")
