<div align="center" markdown="1">
  <img src="../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 T-Display K230 GPIO 和硬件连接表 🌟</h1>

本文档记录 T-Display K230 BSP 预期使用的 GPIO、总线和外设连接关系。硬件按
三组区分：

- K230 主板
- 可选 nRF52840 BLE / 音频 / 传感器底板
- 可选 nRF9151 蜂窝 / GNSS / 键盘底板

## K230 主板

### 40Pin 扩展排针

K230 主板引出的 40Pin 排针如下。

| 左侧 | 右侧 |
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

### 主板设备

| 功能 | K230 信号 | 方向 | 外设信号 | 说明 |
| --- | --- | --- | --- | --- |
| BOOT0 按键 | `GPIO0` | 输入 | 按键输入 | 空闲高电平，按下低电平。launcher 用于屏幕和背光开关逻辑。 |
| PMU INT0 / 电源键 | `GPIO64` | PMU 输入 | PMU input channel 0 | 不是普通 GPIO。空闲低电平，按下高电平。通过 PMU/input 电源键路径暴露。 |
| RM69A10 AMOLED | `GPIO22` | 输出 | 屏幕 reset | U-Boot 启动 logo 和 Linux 显示初始化都会使用。 |
| RM69A10 AMOLED | `GPIO25` | 输出 | 屏幕 enable | 启动 logo 和 launcher 背光/显示使能路径使用。 |
| RM69A10 AMOLED | MIPI DSI | 输出 | 显示数据 | 默认 launcher 显示输出。已验证 LVGL 路径使用 RGB565 更稳定。 |
| GT9895 触摸 | `GPIO37` | I2C SDA | Touch SDA | 和可选 LT9611 HDMI 桥共用 I2C 引脚。 |
| GT9895 触摸 | `GPIO36` | I2C SCL | Touch SCL | 触摸控制器 I2C 地址是 `0x5D`。 |
| GT9895 触摸 | `GPIO23` | 输入 | Touch IRQ | 硬件上和可选 LT9611 IRQ 共用。 |
| GT9895 触摸 | `GPIO24` | 输出 | Touch reset | 硬件上和可选 LT9611 reset 共用。 |
| LT9611 HDMI 桥 | `GPIO37` | I2C SDA | LT9611 SDA | 可选 HDMI 桥路径。 |
| LT9611 HDMI 桥 | `GPIO36` | I2C SCL | LT9611 SCL | 可选 HDMI 桥路径。 |
| LT9611 HDMI 桥 | `GPIO23` | 输入 | LT9611 IRQ | 和触摸 IRQ 共用。 |
| LT9611 HDMI 桥 | `GPIO24` | 输出 | LT9611 reset | 和触摸 reset 共用。 |
| GC2093 摄像头 | `GPIO49` | I2C SDA | Camera SDA | 摄像头 I2C 地址是 `0x37`。 |
| GC2093 摄像头 | `GPIO48` | I2C SCL | Camera SCL | 配合 MIPI CSI 摄像头路径使用。 |
| GC2093 摄像头 | MIPI CSI | 输入 | 摄像头数据 | 用于相机预览、拍照、RTSP 和 AI 摄像头功能。 |
| SD 卡 | `GPIO54` | SDIO CMD | SD CMD | 启动/存储 SDMMC 路径。 |
| SD 卡 | `GPIO55` | SDIO CLK | SD CLK | 启动/存储 SDMMC 路径。 |
| SD 卡 | `GPIO56` | SDIO D0 | SD D0 | 启动/存储 SDMMC 路径。 |
| SD 卡 | `GPIO57` | SDIO D1 | SD D1 | 启动/存储 SDMMC 路径。 |
| SD 卡 | `GPIO58` | SDIO D2 | SD D2 | 启动/存储 SDMMC 路径。 |
| SD 卡 | `GPIO59` | SDIO D3 | SD D3 | 启动/存储 SDMMC 路径。 |
| Wi-Fi | SDIO | I/O | RTL8189FS 或 RTL8723DS Wi-Fi | RTL8723DS 蓝牙需要额外接出 BT UART；SDIO 只负责 Wi-Fi。 |
| USB host | USB | I/O | USB 以太网、USB modem、USB 蓝牙 | BSP rootfs 已包含相关运行包。 |
| SX1262/LR2021 LoRa | `GPIO16` | SPI MOSI | MOSI | 主板 LoRa SPI。 |
| SX1262/LR2021 LoRa | `GPIO17` | SPI MISO | MISO | 主板 LoRa SPI。 |
| SX1262/LR2021 LoRa | `GPIO15` | SPI SCLK | SCK | 主板 LoRa SPI。 |
| SX1262/LR2021 LoRa | `GPIO14` | 输出 | CS | SPI 片选。 |
| SX1262/LR2021 LoRa | `GPIO5` | 输出 | Reset | 由 RadioLib HAL 控制。 |
| SX1262/LR2021 LoRa | `GPIO19` | 输入 | BUSY | LoRa 模组 busy 状态。 |
| SX1262/LR2021 LoRa | `GPIO20` | 输入 | IRQ / DIO1 到 K230 | SX1262 DIO1 接到该引脚。LR2021 芯片侧 IRQ DIO 编号为 `11`，K230 侧中断线是 `GPIO20`。 |
| SX1262/LR2021 LoRa | `GPIO44` | 输出 | 电源使能 | 控制 LoRa 模组电源路径。 |

## nRF52840 BLE / 音频 / 传感器底板

这块可选底板通过 nRF52840 提供 BLE Central 功能，同时包含 AHT20 传感器和
MAX98357A 外部 I2S 功放。

| 底板功能 | 底板信号 | K230 信号 | 从 K230 看方向 | 说明 |
| --- | --- | --- | --- | --- |
| nRF52840 UART | `P0.11` / Arduino `11` RX | `GPIO3` / UART1 TX | 输出 | K230 通过 `/dev/ttyS1` 向 nRF52840 发送 AT 命令，115200 8N1。 |
| nRF52840 UART | `P0.12` / Arduino `12` TX | `GPIO4` / UART1 RX | 输入 | nRF52840 向 K230 返回 AT 响应和 BLE 事件。 |
| nRF52840 LED | `P1.00` / Arduino `32` | 底板本地 LED | nRF52840 输出 | nRF52840 固件驱动的通信/活动指示灯。 |
| nRF52840 调试 | USB CDC `Serial` | 电脑 | I/O | nRF52840 USB-C 接到电脑时，115200 波特率输出调试日志。 |
| AHT20 传感器 | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C 地址 `0x38`。 |
| AHT20 传感器 | SDA | `GPIO47` / I2C4 SDA | I2C | 共用扩展 I2C 总线。 |
| MAX98357A 功放 | `DOUT` / `DIN` | `GPIO35` / I2S data | 输出 | 外部 I2S 功放数据输入。 |
| MAX98357A 功放 | `CLK` / `BCLK` | `GPIO32` / I2S BCLK | 输出 | 外部 I2S 功放位时钟。 |
| MAX98357A 功放 | `WS` / `LRCK` | `GPIO33` / I2S LRCK | 输出 | 外部 I2S 功放左右声道时钟。 |
| MAX98357A 功放 | Shutdown 控制 | `GPIO34` | 输出 | 高电平使能功放，低电平关断。 |

## nRF9151 蜂窝 / GNSS / 键盘底板

这块可选底板提供 nRF9151 蜂窝/GNSS、键盘输入、键盘背光、电池电量计、
充电器和 GPIO 扩展器。

| 底板功能 | 底板信号 | K230 信号 | 从 K230 看方向 | 说明 |
| --- | --- | --- | --- | --- |
| nRF9151 使能 | Power enable | `GPIO2` | 输出 | K230 拉高该引脚使能模组。 |
| nRF9151 UART1 | `P0.26` / RX1 | `GPIO28` / UART3 TX | 输出 | K230 到 nRF9151 的主 AT 命令链路，`/dev/ttyS3`，115200 8N1。 |
| nRF9151 UART1 | `P0.27` / TX1 | `GPIO29` / UART3 RX | 输入 | nRF9151 到 K230 的 AT 响应和 URC 链路。 |
| nRF9151 调试 UART2 | `P0.29` / TX2 | 外部 USB-UART RX | nRF9151 输出 | 可选 Serial LTE Modem 调试日志输出。 |
| nRF9151 调试 UART2 | `P0.28` / RX2 | 外部 USB-UART TX | nRF9151 输入 | 可选调试串口输入。 |
| nRF9151 LED | `P0.23` | 底板本地 LED | nRF9151 输出 | 固件控制的运行/GNSS 状态 LED。 |
| BQ25896 充电器 | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C 地址 `0x6B`。 |
| BQ25896 充电器 | SDA | `GPIO47` / I2C4 SDA | I2C | 充电器和 USB/供电来源状态。 |
| BQ27220 电量计 | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C 地址 `0x55`。 |
| BQ27220 电量计 | SDA | `GPIO47` / I2C4 SDA | I2C | 电池电流/容量信息。 |
| TCA8418 键盘 | SCL | `GPIO46` / I2C4 SCL | I2C | Linux 使用 7-bit 地址 `0x34`；部分原理图会用 `0x69` 表示 8-bit/read 地址。 |
| TCA8418 键盘 | SDA | `GPIO47` / I2C4 SDA | I2C | 键盘矩阵控制器。 |
| TCA8418 键盘 | Reset | `GPIO43` | 输出 | 键盘控制器 reset。 |
| TCA8418 键盘 | IRQ | `GPIO42` | 输入 | 键盘中断线。 |
| XL9555 GPIO 扩展器 | SCL | `GPIO46` / I2C4 SCL | I2C | 7-bit I2C 地址 `0x20`。 |
| XL9555 GPIO 扩展器 | SDA | `GPIO47` / I2C4 SDA | I2C | LED 和键盘底板支持。 |
| 键盘背光 | PWM | `GPIO52` / PWM4 | 输出 | 用户态调节键盘背光亮度。 |

## I2C 地址速查

| 7-bit 地址 | 设备 | 板卡分组 |
| --- | --- | --- |
| `0x20` | XL9555 GPIO 扩展器 | nRF9151 键盘底板 |
| `0x34` | TCA8418 键盘控制器 | nRF9151 键盘底板 |
| `0x37` | GC2093 摄像头 | K230 主板 |
| `0x38` | AHT20 温湿度传感器 | nRF52840 底板 |
| `0x55` | BQ27220 电池电量计 | nRF9151 键盘底板 |
| `0x5D` | GT9895 / Goodix 触摸控制器 | K230 主板 |
| `0x6B` | BQ25896 充电器 | nRF9151 键盘底板 |

## 地址和硬件版本说明

- GT9895 触摸控制器 I2C 地址是 `0x5D`。
- TCA8418 键盘控制器在 Linux 里通常使用 7-bit I2C 地址 `0x34`。如果原理图
  写的是 `0x69`，它是移位后的 8-bit/read 地址写法。
- `GPIO42` 是本 BSP 默认的 TCA8418 键盘中断。
- 部分 RTL8723DS 蓝牙硬件版本可能将 BT UART RTS/CTS 接到 K230 的
  `GPIO42`/`GPIO43` 等引脚，需要使用匹配的 DTS/overlay。
