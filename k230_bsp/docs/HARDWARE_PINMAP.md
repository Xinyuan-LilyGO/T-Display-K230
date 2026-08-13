# T-Display K230 GPIO and Hardware Map

This document records the GPIO, bus, and peripheral assignments expected by the
T-Display K230 BSP.

## GPIO Map

| K230 GPIO | Direction | Connected function | Signal / role | Notes |
| --- | --- | --- | --- | --- |
| `GPIO0` | Input | BOOT0 user button | Button input | Idle high, pressed low. The launcher uses it for screen/backlight toggle behavior. |
| `GPIO2` | Output | nRF9151 | `EN` | High enables the nRF9151 module. |
| `GPIO3` | UART1 TX | nRF52840 or RTL8723DS BT UART variant | K230 TX to module RX | Default BLE bridge preset uses UART1. RTL8723DS Bluetooth also needs a BT UART if that hardware variant is assembled. |
| `GPIO4` | UART1 RX | nRF52840 or RTL8723DS BT UART variant | K230 RX from module TX | Paired with `GPIO3`. |
| `GPIO5` | Output | SX1262/LR2021 | LoRa reset | Active level is controlled by the RadioLib HAL. |
| `GPIO14` | Output | SX1262/LR2021 | SPI chip select | LoRa SPI CS. |
| `GPIO15` | Output | SX1262/LR2021 | SPI SCLK | LoRa SPI clock. |
| `GPIO16` | Output | SX1262/LR2021 | SPI MOSI | LoRa SPI host-to-radio data. |
| `GPIO17` | Input | SX1262/LR2021 | SPI MISO | LoRa SPI radio-to-host data. |
| `GPIO19` | Input | SX1262/LR2021 | `BUSY` | Radio busy status. |
| `GPIO20` | Input | SX1262/LR2021 | IRQ / DIO1 line to K230 | SX1262 uses DIO1 on this K230 line. LR2021 chip-side IRQ DIO number is `11`, while the K230 interrupt line is `GPIO20`. |
| `GPIO22` | Output | RM69A10 AMOLED | Panel reset | Used by U-Boot logo path and Linux display bring-up. |
| `GPIO23` | Input | GT9895 touch / optional LT9611 HDMI bridge | Shared IRQ | Hardware may share the touch IRQ with the LT9611 IRQ. |
| `GPIO24` | Output | GT9895 touch / optional LT9611 HDMI bridge | Shared reset | Hardware may share the touch reset with the LT9611 reset. |
| `GPIO25` | Output | RM69A10 AMOLED | Panel power/backlight enable | Used by the boot logo path and launcher backlight control path. |
| `GPIO28` | UART3 TX | nRF9151 | K230 TX to nRF9151 RX | 115200 8N1, no RTS/CTS. |
| `GPIO29` | UART3 RX | nRF9151 | K230 RX from nRF9151 TX | 115200 8N1, no RTS/CTS. |
| `GPIO32` | I2S | MAX98357A | `BCLK` | External I2S amplifier bit clock. |
| `GPIO33` | I2S | MAX98357A | `LRCK` / `WS` | External I2S amplifier word-select clock. |
| `GPIO34` | Output | MAX98357A | `SHUTDOWN` | High enables the amplifier, low shuts it down. |
| `GPIO35` | I2S | MAX98357A | `DIN` | External I2S amplifier data input. |
| `GPIO36` | I2C3 SCL | Optional LT9611 HDMI bridge | `SCL` | Optional HDMI bridge control bus. |
| `GPIO37` | I2C3 SDA | Optional LT9611 HDMI bridge | `SDA` | Optional HDMI bridge control bus. |
| `GPIO42` | Input | TCA8418 keyboard | Keyboard interrupt | Default keyboard-base interrupt line. If a hardware variant routes RTL8723DS BT UART RTS here, the DTS/overlay must match that variant. |
| `GPIO43` | UART CTS | RTL8723DS BT UART variant | BT UART CTS | Only used when the RTL8723DS Bluetooth UART hardware variant is assembled. |
| `GPIO44` | Output | SX1262/LR2021 | LoRa module power enable | Enables the LoRa module power path. |
| `GPIO46` | I2C4 SCL | Keyboard base / sensors | `SCL` | Used for the board I2C device group when present. |
| `GPIO47` | I2C4 SDA | Keyboard base / sensors | `SDA` | Used for the board I2C device group when present. |
| `GPIO52` | PWM4 / Output | Keyboard backlight | Backlight PWM | U-Boot initializes it low early; userspace drives PWM brightness. |
| `GPIO64` | PMU input | INT0 / power key | PMU input channel 0 | Not a normal GPIO line. Idle low, pressed high. Exposed through the PMU/input power-key path. |

## Non-GPIO Buses and Devices

| Function | Interface | Address / node | Notes |
| --- | --- | --- | --- |
| RM69A10 AMOLED | MIPI DSI | Panel timing in display DTB and U-Boot logo path | Default launcher display. The validated LVGL path uses RGB565 for stable refresh. |
| GT9895 / Goodix Berlin touch | I2C + IRQ/reset GPIOs | Input event device from Linux touch driver | The BSP uses the full Goodix Berlin/Nottingham driver path with type-B multitouch slot reporting. |
| GC2093 camera | MIPI CSI + I2C | I2C address `0x37` | Used by camera preview, capture, RTSP, and AI camera features. |
| RTL8189FS Wi-Fi | SDIO | SDIO controller | Default SDIO Wi-Fi option. |
| RTL8723DS Wi-Fi | SDIO | SDIO controller | Optional SDIO Wi-Fi option. Bluetooth requires the separate BT UART hardware signals. |
| nRF52840 BLE bridge | UART1 | `/dev/ttyS1` | Used by the BLE launcher page when the nRF52840 bridge firmware is installed. |
| nRF9151 cellular/GNSS | UART3 + `GPIO2` enable | `/dev/ttyS3`, 115200 8N1 | Used by the Cellular app for AT link, SIM, LTE, GNSS, NMEA, and C/N0 display. |
| SX1262 / LR2021 LoRa | SPI + GPIO control | RadioLib HAL | SX1262 and LR2021 are auto-detected by the launcher where supported. |
| AHT20 | I2C4 group | `0x38` | Temperature and humidity sensor. |
| TCA8418 | I2C4 group | `0x34` | Keyboard matrix controller. |
| XL9555 | I2C4 group | `0x20` | GPIO expander for LED and keyboard-base support. |
| BQ25896 | I2C4 group | `0x6B` | Charger and USB/power-source status. |
| BQ27220 | I2C4 group | `0x55` | Battery gauge telemetry. |
| MAX98357A | I2S + shutdown GPIO | External amplifier route | Audio output route can be selected from the launcher settings. |
| USB Ethernet / USB modem / USB Bluetooth | USB host | Linux USB stack | Supported by the root filesystem packages included by the BSP. |
| Optional LT9611 HDMI bridge | MIPI DSI + I2C3 + GPIO23/24 | I2C3 control bus | Optional hardware path. AMOLED remains the default display output. |

## I2C Address Quick Reference

| 7-bit address | Device |
| --- | --- |
| `0x20` | XL9555 GPIO expander |
| `0x34` | TCA8418 keyboard controller |
| `0x37` | GC2093 camera sensor |
| `0x38` | AHT20 temperature/humidity sensor |
| `0x55` | BQ27220 battery gauge |
| `0x6B` | BQ25896 charger |
