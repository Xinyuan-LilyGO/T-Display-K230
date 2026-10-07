<div align="center" markdown="1">
  <img src="../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 T-Display K230 GPIO and Hardware Map 🌟</h1>

This document records the GPIO, bus, and peripheral assignments expected by the
T-Display K230 BSP. The hardware is split into three groups:

- K230 main board
- optional nRF52840 BLE/audio/sensor base board
- optional nRF9151 cellular/GNSS/keyboard base board

## K230 Main Board

### 40-Pin Expansion Header

The K230 main board exposes the following 40-pin header.

| Left side | Right side |
| --- | --- |
| `ADC0` | `ADC2` |
| `ADC1` | `GND` |
| `GPIO47` | `GPIO43` |
| `GPIO14` | `GPIO46` |
| `GPIO17` | `GPIO18` |
| `GPIO53` | `GPIO52` |
| `GPIO33` | `GPIO62` |
| `GPIO34` | `GPIO32` |
| `GPIO29` | `GPIO31` |
| `GPIO28` | `GPIO30` |
| `GPIO6` | `GPIO26` |
| `GPIO3` | `GPIO5` |
| `GPIO63` | `GPIO27` |
| `GPIO35` | `GPIO4` |
| `GPIO45` | `GPIO2` |
| `GPIO44` | `GPIO42` |
| `GPIO15` | `GPIO16` |
| `GPIO19` | `GPIO20` |
| `5V` | `3V3` |
| `GND` | `USB-IN-5V` |

* With USB-C facing down, the left side of the 2x20 pin header is the reference point.

### Main-Board Devices

| Function | K230 signal | Direction | Peripheral signal | Notes |
| --- | --- | --- | --- | --- |
| BOOT0 button | `GPIO0` | Input | Button input | Idle high, pressed low. The launcher uses it for screen/backlight toggle behavior. |
| PMU INT0 / power key | `GPIO64` | PMU input | PMU input channel 0 | Not a normal GPIO line. Idle low, pressed high. Exposed through the PMU/input power-key path. |
| RM69A10 AMOLED | `GPIO22` | Output | Reset | Used by U-Boot logo path and Linux display bring-up. |
| RM69A10 AMOLED | `GPIO25` | Output | Panel enable | Used by the boot logo path and launcher backlight/display enable path. |
| RM69A10 AMOLED | MIPI DSI | Output | Display data | Default launcher display output. The validated LVGL path uses RGB565 for stable refresh. |
| GT9895 touch | `GPIO37` | I2C SDA | Touch SDA | Shared I2C pins with the optional LT9611 HDMI bridge path. |
| GT9895 touch | `GPIO36` | I2C SCL | Touch SCL | Touch controller I2C address is `0x5D`. |
| GT9895 touch | `GPIO23` | Input | Touch IRQ | Hardware shares this line with the optional LT9611 IRQ. |
| GT9895 touch | `GPIO24` | Output | Touch reset | Hardware shares this line with the optional LT9611 reset. |
| LT9611 HDMI bridge | `GPIO37` | I2C SDA | LT9611 SDA | Optional HDMI bridge path. |
| LT9611 HDMI bridge | `GPIO36` | I2C SCL | LT9611 SCL | Optional HDMI bridge path. |
| LT9611 HDMI bridge | `GPIO23` | Input | LT9611 IRQ | Shared with touch IRQ. |
| LT9611 HDMI bridge | `GPIO24` | Output | LT9611 reset | Shared with touch reset. |
| GC2093 camera | `GPIO49` | I2C SDA | Camera SDA | Camera sensor I2C address is `0x37`. |
| GC2093 camera | `GPIO48` | I2C SCL | Camera SCL | Used with the MIPI CSI camera path. |
| GC2093 camera | MIPI CSI | Input | Camera data | Used by camera preview, capture, RTSP, and AI camera features. |
| DW9714 focus actuator | `GPIO49` | I2C SDA | Focus SDA | Optional VCM on the camera I2C bus; 7-bit address is `0x0C`. |
| DW9714 focus actuator | `GPIO48` | I2C SCL | Focus SCL | Optional VCM on the camera I2C bus. |
| SD card | `GPIO54` | SDIO CMD | SD CMD | Boot/storage SDMMC path. |
| SD card | `GPIO55` | SDIO CLK | SD CLK | Boot/storage SDMMC path. |
| SD card | `GPIO56` | SDIO D0 | SD D0 | Boot/storage SDMMC path. |
| SD card | `GPIO57` | SDIO D1 | SD D1 | Boot/storage SDMMC path. |
| SD card | `GPIO58` | SDIO D2 | SD D2 | Boot/storage SDMMC path. |
| SD card | `GPIO59` | SDIO D3 | SD D3 | Boot/storage SDMMC path. |
| Wi-Fi | SDIO | I/O | RTL8189FS or RTL8723DS Wi-Fi | RTL8723DS Bluetooth requires separate BT UART hardware signals; SDIO covers Wi-Fi only. |
| USB host | USB | I/O | USB Ethernet, USB modem, USB Bluetooth | Supported by the root filesystem packages included by the BSP. |
| SX1262/LR2021 LoRa | `GPIO16` | SPI MOSI | MOSI | Main-board LoRa SPI. |
| SX1262/LR2021 LoRa | `GPIO17` | SPI MISO | MISO | Main-board LoRa SPI. |
| SX1262/LR2021 LoRa | `GPIO15` | SPI SCLK | SCK | Main-board LoRa SPI. |
| SX1262/LR2021 LoRa | `GPIO14` | Output | CS | SPI chip select. |
| SX1262/LR2021 LoRa | `GPIO5` | Output | Reset | Controlled by the RadioLib HAL. |
| SX1262/LR2021 LoRa | `GPIO19` | Input | BUSY | Radio busy status. |
| SX1262/LR2021 LoRa | `GPIO20` | Input | IRQ / DIO1 line to K230 | SX1262 version, DIO1 is connected to the K230 side interrupt line `GPIO20`. LR2021 version, the IRQ DIO number is `DIO11`, which is connected to K230 `GPIO20`. |
| SX1262/LR2021 LoRa | `GPIO44` | Output | Power enable | Enables the LoRa module power path. |

> [!IMPORTANT]
> LR2021 LILYGO 2.4 GHz HF profiles are limited to 4 dBm. The 868/915 MHz
> module version uses the DPX205850DT-4055A1 duplexer and does not include an
> MXD8721 RF switch. DIO8/DIO10 RF-switch control is only used by the 433 MHz
> LR2021 version.

## nRF52840 BLE / Audio / Sensor Base Board

This optional base board provides BLE Central functionality through nRF52840,
AHT20 sensing, and the MAX98357A external I2S amplifier.

| Base-board function | Base-board signal | K230 signal | Direction from K230 | Notes |
| --- | --- | --- | --- | --- |
| nRF52840 UART | `P0.11` / Arduino `11` RX | `GPIO3` / UART1 TX | Output | K230 sends AT commands to nRF52840 on `/dev/ttyS1`, 115200 8N1. |
| nRF52840 UART | `P0.12` / Arduino `12` TX | `GPIO4` / UART1 RX | Input | nRF52840 sends AT responses and BLE events to K230. |
| nRF52840 LED | `P1.00` / Arduino `32` |Blue LED (mounted on the nRF52840 board) | Output from nRF52840 | Communication/activity LED driven by the nRF52840 firmware. |
| nRF52840 debug | USB CDC `Serial` | Host computer | I/O | 115200 baud debug output when the nRF52840 USB-C port is connected. |
| AHT20 sensor | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C address `0x38`. |
| AHT20 sensor | SDA | `GPIO47` / I2C4 SDA | I2C | Shares the expansion I2C bus. |
| MAX98357A amplifier | `DOUT` / `DIN` | `GPIO35` / I2S data | Output | External I2S amplifier data input. |
| MAX98357A amplifier | `CLK` / `BCLK` | `GPIO32` / I2S BCLK | Output | External I2S amplifier bit clock. |
| MAX98357A amplifier | `WS` / `LRCK` | `GPIO33` / I2S LRCK | Output | External I2S amplifier word-select clock. |
| MAX98357A amplifier | Shutdown control | `GPIO34` | Output | High enables the amplifier, low shuts it down. |

## nRF9151 Cellular / GNSS / Keyboard Base Board

This optional base board provides cellular/GNSS through nRF9151, keyboard input,
keyboard backlight, battery gauge, charger, and GPIO expander devices.

| Base-board function | Base-board signal | K230 signal | Direction from K230 | Notes |
| --- | --- | --- | --- | --- |
| nRF9151 enable | Power enable | `GPIO2` | Output | K230 drives this line high to enable the module. |
| nRF9151 UART1 | `P0.26` / RX1 | `GPIO28` / UART3 TX | Output | Main AT command link from K230 to nRF9151, `/dev/ttyS3`, 115200 8N1. |
| nRF9151 UART1 | `P0.27` / TX1 | `GPIO29` / UART3 RX | Input | AT response and URC link from nRF9151 to K230. |
| nRF9151 debug UART2 | `P0.29` / TX2 | External USB-UART RX | Output from nRF9151 | Optional Serial LTE Modem debug log output. |
| nRF9151 debug UART2 | `P0.28` / RX2 | External USB-UART TX | Input to nRF9151 | Optional debug-console input. |
| nRF9151 LED | `P0.23` | Blue LED (mounted inside the PCB board) | Output from nRF9151 | Firmware-controlled run/GNSS status LED. |
| BQ25896 charger | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C address `0x6B`. |
| BQ25896 charger | SDA | `GPIO47` / I2C4 SDA | I2C | Charger and USB/power-source status. |
| BQ27220 battery gauge | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C address `0x55`. |
| BQ27220 battery gauge | SDA | `GPIO47` / I2C4 SDA | I2C | Battery current/capacity telemetry. |
| TCA8418 keyboard | SCL | `GPIO46` / I2C4 SCL | I2C | Linux uses the 7-bit address `0x34` |
| TCA8418 keyboard | SDA | `GPIO47` / I2C4 SDA | I2C | Keyboard matrix controller. |
| TCA8418 keyboard | Reset | `GPIO43` | Output | Keyboard controller reset. |
| TCA8418 keyboard | IRQ | `GPIO42` | Input | Keyboard interrupt line. |
| XL9555 GPIO expander | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C address can be `0x20`-`0x27`; software probes and caches the detected address. |
| XL9555 GPIO expander | SDA | `GPIO47` / I2C4 SDA | I2C | LED and keyboard-base support. |
| DRV2605 haptic driver | SCL | `GPIO46` / I2C4 SCL | I2C | Optional haptic-feedback driver, 7-bit I2C address `0x5A`. |
| DRV2605 haptic driver | SDA | `GPIO47` / I2C4 SDA | I2C | Shares the keyboard-base I2C bus with TCA8418 and XL9555. |
| Keyboard backlight | PWM | `GPIO52` / PWM4 | Output | Userspace drives keyboard-backlight brightness. |

## I2C Address Quick Reference

| 7-bit address | Device | Board group |
| --- | --- | --- |
| `0x0C` | DW9714 focus actuator | Optional camera module |
| `0x37` | GC2093 camera sensor | K230 main board |
| `0x5D` | GT9895 / Goodix touch controller | K230 main board |
| `0x6B` | BQ25896 charger | nRF9151 keyboard base |
| `0x38` | AHT20 temperature/humidity sensor | nRF52840 base |
| `0x20`-`0x27` | XL9555 GPIO expander | nRF9151 keyboard base; address depends on hardware strap/version |
| `0x34` | TCA8418 keyboard controller | nRF9151 keyboard base |
| `0x55` | BQ27220 battery gauge | nRF9151 keyboard base |
| `0x5A` | DRV2605 haptic-feedback driver | Optional nRF9151 keyboard base variant |
