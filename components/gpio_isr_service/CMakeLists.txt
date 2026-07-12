idf_component_register(SRCS "gpio_isr_service.c"
                    REQUIRES esp_driver_gpio freertos
                    INCLUDE_DIRS "include")
