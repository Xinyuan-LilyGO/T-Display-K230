# T-Display K230 Hardware Pin Map

This file records the board assumptions baked into this BSP.

| Function | K230 pins / nodes | Notes |
| --- | --- | --- |
| AMOLED | RM69A10 MIPI DSI, reset GPIO22, backlight/power GPIO25 | 568x1232 portrait panel, RGB565 LVGL path recommended. |
| Touch | GT9895 / Goodix Berlin over I2C | Kernel driver supports type-B multitouch slots. |
| Camera | GC2093 on I2C4 address 0x37, MIPI CSI | VVCAM defaults to GC2093. |
| Wi-Fi | RTL8189FS SDIO, optional RTL8723DS SDIO | RTL8723DS Bluetooth requires separate BT UART hardware. |
| nRF52840 | K230 UART1 TX GPIO3, RX GPIO4 | Used by the optional BLE bridge firmware. |
| nRF9151 | EN GPIO2, UART3 TX GPIO28, UART3 RX GPIO29 | UART3 conflict pins IO50/IO51 are forced back to GPIO. |
| LoRa SX1262/LR2021 | SCLK GPIO15, MOSI GPIO16, MISO GPIO17, CS GPIO14, RST GPIO5, BUSY GPIO19, IRQ/DIO1 GPIO20, power enable GPIO44 | LR2021 chip-side IRQ DIO number is 11; K230 IRQ line is GPIO20. |
| MAX98357A | BCLK GPIO32, LRCK GPIO33, DIN GPIO35, SHUTDOWN GPIO34 | GPIO34 high enables the amplifier, low shuts it down. |
| AHT20 | I2C4 alternate SDA GPIO47, SCL GPIO46, address 0x38 | Shares I2C4 controller with camera pins; userspace may use GPIO bitbang fallback. |
| TCA8418 keyboard | I2C4 address 0x34, IRQ GPIO42 | IRQ mode is the intended default. |
| Keyboard backlight | GPIO52 / PWM4 | U-Boot keeps it low early; userspace drives PWM. |
| XL9555 | I2C4 address 0x20 | Used for LEDs and keyboard-base support. |
| BQ25896 | I2C4 address 0x6B | Charger control and USB/power source state. |
| BQ27220 | I2C4 address 0x55 | Battery gauge current/capacity telemetry. |
| PMU INT0 / power key | PMU input channel, Linux input KEY_POWER | Used for long-press shutdown and wake flow. |
