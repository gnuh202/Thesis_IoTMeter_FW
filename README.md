# Luận văn firmware

ESP-IDF firmware project.

## Structure

```text
main/
├── app_main.c
└── app/
    ├── app_tasks.c/.h
    ├── uart_tasks.c/.h
    └── pcf8574_task.c/.h

components/
├── i2c_bus/
└── pcf8574/
```

## Behavior

- UART1 transmits `Hello: x` with `x = 0..255`, period 500 ms.
- UART2 logs received data to console.
- PCF8574 demo task toggles P0 every 500 ms and logs port value.

## Build

```bash
idf.py build
```
