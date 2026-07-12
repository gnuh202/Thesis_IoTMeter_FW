idf_component_register(SRCS "lcd2004_i2c.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pcf8574 esp_driver_i2c freertos)
