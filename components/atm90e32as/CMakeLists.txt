idf_component_register(SRCS "atm90e32as.c"
                    REQUIRES esp_driver_spi spi_bus_shared
                    INCLUDE_DIRS "include")
