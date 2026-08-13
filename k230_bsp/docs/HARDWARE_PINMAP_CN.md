# T-Display K230 GPIO 和硬件连接表

本文档记录 T-Display K230 BSP 预期使用的 GPIO、总线和外设连接关系。

## GPIO 列表

| K230 GPIO | 方向 | 连接外设 | 信号 / 用途 | 说明 |
| --- | --- | --- | --- | --- |
| `GPIO0` | 输入 | BOOT0 用户按键 | 按键输入 | 空闲高电平，按下低电平。launcher 用于屏幕和背光开关逻辑。 |
| `GPIO2` | 输出 | nRF9151 | `EN` | 高电平使能 nRF9151 模组。 |
| `GPIO3` | UART1 TX | nRF52840 或 RTL8723DS BT UART 版本 | K230 TX 到模组 RX | 默认 BLE bridge 预设使用 UART1。如果装配 RTL8723DS 蓝牙 UART 版本，也需要该 UART。 |
| `GPIO4` | UART1 RX | nRF52840 或 RTL8723DS BT UART 版本 | K230 RX 接模组 TX | 与 `GPIO3` 配对使用。 |
| `GPIO5` | 输出 | SX1262/LR2021 | LoRa reset | 有效电平由 RadioLib HAL 控制。 |
| `GPIO14` | 输出 | SX1262/LR2021 | SPI 片选 | LoRa SPI CS。 |
| `GPIO15` | 输出 | SX1262/LR2021 | SPI SCLK | LoRa SPI 时钟。 |
| `GPIO16` | 输出 | SX1262/LR2021 | SPI MOSI | 主控到 LoRa 的 SPI 数据。 |
| `GPIO17` | 输入 | SX1262/LR2021 | SPI MISO | LoRa 到主控的 SPI 数据。 |
| `GPIO19` | 输入 | SX1262/LR2021 | `BUSY` | LoRa 模组 busy 状态。 |
| `GPIO20` | 输入 | SX1262/LR2021 | IRQ / DIO1 到 K230 | SX1262 的 DIO1 接到该 K230 引脚。LR2021 芯片侧 IRQ DIO 编号为 `11`，K230 侧中断线是 `GPIO20`。 |
| `GPIO22` | 输出 | RM69A10 AMOLED | 屏幕 reset | U-Boot 启动 logo 和 Linux 显示初始化都会使用。 |
| `GPIO23` | 输入 | GT9895 触摸 / 可选 LT9611 HDMI 桥 | 共享 IRQ | 硬件上触摸 IRQ 可能与 LT9611 IRQ 共用。 |
| `GPIO24` | 输出 | GT9895 触摸 / 可选 LT9611 HDMI 桥 | 共享 reset | 硬件上触摸 reset 可能与 LT9611 reset 共用。 |
| `GPIO25` | 输出 | RM69A10 AMOLED | 屏幕电源 / 背光使能 | 启动 logo 和 launcher 背光控制路径使用。 |
| `GPIO28` | UART3 TX | nRF9151 | K230 TX 到 nRF9151 RX | 115200 8N1，无 RTS/CTS。 |
| `GPIO29` | UART3 RX | nRF9151 | K230 RX 接 nRF9151 TX | 115200 8N1，无 RTS/CTS。 |
| `GPIO32` | I2S | MAX98357A | `BCLK` | 外部 I2S 功放位时钟。 |
| `GPIO33` | I2S | MAX98357A | `LRCK` / `WS` | 外部 I2S 功放左右声道时钟。 |
| `GPIO34` | 输出 | MAX98357A | `SHUTDOWN` | 高电平使能功放，低电平关断。 |
| `GPIO35` | I2S | MAX98357A | `DIN` | 外部 I2S 功放数据输入。 |
| `GPIO36` | I2C3 SCL | 可选 LT9611 HDMI 桥 | `SCL` | 可选 HDMI 桥控制总线。 |
| `GPIO37` | I2C3 SDA | 可选 LT9611 HDMI 桥 | `SDA` | 可选 HDMI 桥控制总线。 |
| `GPIO42` | 输入 | TCA8418 键盘 | 键盘中断 | 默认键盘底板中断线。如果某个硬件版本将 RTL8723DS BT UART RTS 接到这里，需要匹配对应 DTS/overlay。 |
| `GPIO43` | UART CTS | RTL8723DS BT UART 版本 | BT UART CTS | 仅在实际装配 RTL8723DS 蓝牙 UART 硬件版本时使用。 |
| `GPIO44` | 输出 | SX1262/LR2021 | LoRa 电源使能 | 控制 LoRa 模组电源路径。 |
| `GPIO46` | I2C4 SCL | 键盘底板 / 传感器 | `SCL` | 板载 I2C 设备组使用。 |
| `GPIO47` | I2C4 SDA | 键盘底板 / 传感器 | `SDA` | 板载 I2C 设备组使用。 |
| `GPIO52` | PWM4 / 输出 | 键盘背光 | 背光 PWM | U-Boot 早期会初始化为低电平，用户态使用 PWM 调节亮度。 |
| `GPIO64` | PMU 输入 | INT0 / 电源键 | PMU input channel 0 | 不是普通 GPIO。空闲低电平，按下高电平。通过 PMU/input 电源键路径暴露。 |

## 非 GPIO 总线和设备

| 功能 | 接口 | 地址 / 节点 | 说明 |
| --- | --- | --- | --- |
| RM69A10 AMOLED | MIPI DSI | 显示 DTB 和 U-Boot logo 路径中的屏参 | 默认 launcher 显示输出。已验证 LVGL 路径使用 RGB565 更稳定。 |
| GT9895 / Goodix Berlin 触摸 | I2C + IRQ/reset GPIO | Linux 触摸 input 设备 | BSP 使用完整 Goodix Berlin/Nottingham 驱动路径，支持 type-B 多点触摸 slot 上报。 |
| GC2093 摄像头 | MIPI CSI + I2C | I2C 地址 `0x37` | 用于相机预览、拍照、RTSP 和 AI 摄像头功能。 |
| RTL8189FS Wi-Fi | SDIO | SDIO 控制器 | 默认 SDIO Wi-Fi 选项。 |
| RTL8723DS Wi-Fi | SDIO | SDIO 控制器 | 可选 SDIO Wi-Fi 选项。蓝牙需要额外接出 BT UART。 |
| nRF52840 BLE bridge | UART1 | `/dev/ttyS1` | 安装 nRF52840 bridge 固件后供 BLE 页面使用。 |
| nRF9151 蜂窝/GNSS | UART3 + `GPIO2` 使能 | `/dev/ttyS3`，115200 8N1 | 蜂窝应用用于 AT 链路、SIM、LTE、GNSS、NMEA 和 C/N0 显示。 |
| SX1262 / LR2021 LoRa | SPI + GPIO 控制 | RadioLib HAL | launcher 在支持的路径中会自动探测 SX1262 和 LR2021。 |
| AHT20 | I2C4 设备组 | `0x38` | 温湿度传感器。 |
| TCA8418 | I2C4 设备组 | `0x34` | 键盘矩阵控制器。 |
| XL9555 | I2C4 设备组 | `0x20` | LED 和键盘底板支持用 GPIO 扩展器。 |
| BQ25896 | I2C4 设备组 | `0x6B` | 充电器和 USB/供电来源状态。 |
| BQ27220 | I2C4 设备组 | `0x55` | 电池电量计。 |
| MAX98357A | I2S + shutdown GPIO | 外部功放输出路径 | 可在 launcher 设置中选择音频输出路径。 |
| USB 以太网 / USB modem / USB 蓝牙 | USB host | Linux USB 协议栈 | BSP rootfs 已包含相关运行包。 |
| 可选 LT9611 HDMI 桥 | MIPI DSI + I2C3 + GPIO23/24 | I2C3 控制总线 | 可选硬件路径。默认显示输出仍然是 AMOLED。 |

## I2C 地址速查

| 7-bit 地址 | 设备 |
| --- | --- |
| `0x20` | XL9555 GPIO 扩展器 |
| `0x34` | TCA8418 键盘控制器 |
| `0x37` | GC2093 摄像头 |
| `0x38` | AHT20 温湿度传感器 |
| `0x55` | BQ27220 电池电量计 |
| `0x6B` | BQ25896 充电器 |

