idf_component_register(SRCS "hmi_bsp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES i2c_bus lcd2004_i2c pcf8575 freertos)
