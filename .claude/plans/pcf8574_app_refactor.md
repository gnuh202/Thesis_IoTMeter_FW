Plan: refactor ESP-IDF app to componentized structure

1. Rename project/app away from example wording
   - Root CMake: project(uart_echo) -> project(firmware_app) or similar neutral name.
   - Rename main source: main/uart_echo_example_main.c -> main/app_main.c.
   - main/CMakeLists.txt: update SRCS, deps.
   - Remove "Echo Example" / CONFIG_EXAMPLE_* naming from code/Kconfig.

2. Add app layer for tasks
   - Create main/app/ or main/app_tasks/ under main component.
   - Proposed files:
     - main/app/app_tasks.h
     - main/app/app_tasks.c
     - main/app/uart_tasks.h
     - main/app/uart_tasks.c
     - main/app/pcf8574_task.h
     - main/app/pcf8574_task.c
   - app_main.c only calls:
     - app_tasks_start();
   - app_tasks.c initializes subsystems and creates tasks.

3. Add i2c_bus component
   - components/i2c_bus/CMakeLists.txt
   - components/i2c_bus/include/i2c_bus.h
   - components/i2c_bus/i2c_bus.c
   - Use ESP-IDF 5.x new I2C master API: driver/i2c_master.h.
   - API:
     - esp_err_t i2c_bus_init(void);
     - i2c_master_bus_handle_t i2c_bus_get_handle(void);
   - Config via Kconfig.projbuild:
     - APP_I2C_PORT_NUM
     - APP_I2C_SDA_IO
     - APP_I2C_SCL_IO
     - APP_I2C_CLK_SPEED_HZ
   - Default pins need choosing. I will use SDA=8, SCL=9 unless you request other pins.

4. Add pcf8574 component
   - components/pcf8574/CMakeLists.txt
   - components/pcf8574/include/pcf8574.h
   - components/pcf8574/pcf8574.c
   - Depends on esp_driver_i2c.
   - API:
     - pcf8574_create(bus, address, &handle)
     - pcf8574_delete(handle)
     - pcf8574_write_port(handle, value)
     - pcf8574_read_port(handle, &value)
     - pcf8574_write_pin(handle, pin, level)
     - pcf8574_read_pin(handle, pin, &level)
   - Internal cached output_state for per-pin writes.

5. Add simple PCF8574 demo task
   - main/app/pcf8574_task.c
   - Init I2C bus.
   - Create PCF8574 device at configurable address.
   - Simple demo: toggle P0 every 500ms and log current port value.
   - Config:
     - APP_PCF8574_I2C_ADDR default 0x20
     - APP_PCF8574_TASK_STACK_SIZE default 3072
     - APP_PCF8574_TASK_PRIORITY default 5

6. Refactor UART task names/config
   - Rename CONFIG_EXAMPLE_* -> CONFIG_APP_*.
   - Keep existing behavior:
     - UART1 sends "Hello: x" 0..255 each 500ms.
     - UART2 logs received data.
   - Files:
     - main/app/uart_tasks.c
     - main/app/uart_tasks.h
   - Config:
     - APP_UART1_* / APP_UART2_* / APP_UART_TASK_STACK_SIZE.

7. Update sdkconfig symbols
   - Replace old CONFIG_EXAMPLE_* entries with CONFIG_APP_* equivalents so code compiles immediately.
   - Note: idf.py menuconfig/build can regenerate if environment available.

8. Build check
   - Try idf.py build.
   - If idf.py unavailable, report exact command failure and what to run in ESP-IDF terminal.

Resulting tree:

uart_echo/
├── CMakeLists.txt
├── sdkconfig
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── app_main.c
│   └── app/
│       ├── app_tasks.h
│       ├── app_tasks.c
│       ├── uart_tasks.h
│       ├── uart_tasks.c
│       ├── pcf8574_task.h
│       └── pcf8574_task.c
└── components/
    ├── i2c_bus/
    │   ├── CMakeLists.txt
    │   ├── include/i2c_bus.h
    │   └── i2c_bus.c
    └── pcf8574/
        ├── CMakeLists.txt
        ├── include/pcf8574.h
        └── pcf8574.c

Assumption needing approval: I2C default pins SDA=GPIO8, SCL=GPIO9, PCF8574 addr=0x20.