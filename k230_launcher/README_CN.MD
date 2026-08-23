<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 Launcher 🌟</h1>

[English](./README.MD) | [中文](./README_CN.MD)

这个目录存放 LILYGO T-Display K230 手机风格 LVGL 启动器应用。

## 发布范围

本 Launcher 是 `v0.2.0` 镜像使用的用户界面层，面向 BSP 提供的
RM69A10 屏幕、Goodix 触摸、GC2093 摄像头、音频输出、可选键盘底板、
LoRa 模块、nRF52840 和 nRF9151。

主要应用分组：

- 相机、相册、截图、视频、音乐、网络电台、录音、麦克风 FFT、I2S 测试、
  终端、NES、MTP 和屏幕测试
- Wi-Fi、以太网、蓝牙、显示、音频、通知音效、语言、时间、传感器、
  充电器、电池、键盘、F 键热键和系统信息设置
- LoRa profile 测试、LoRaWAN profile 管理、nRF52840 DFU、nRF9151
  蜂窝/GNSS/HTTP/MQTT 工具和 Meshtastic Beta

依赖可选硬件的应用在进入前需要检测对应模块是否存在。如果模块不存在，
Launcher 应显示用户可理解的提示，而不是进入不可用页面。

## 前置条件

先安装基础工具：

```sh
sudo apt update
sudo apt install -y git ca-certificates build-essential make rsync python3
```

请先安装并应用 BSP：

```sh
git clone --recurse-submodules https://github.com/Xinyuan-LilyGO/T-Display-K230.git t-display-k230
cd t-display-k230
./scripts/apply_to_sdk.sh
```

完整 SDK 依赖安装、工具链准备、镜像构建方式请看 BSP 的 README。

## 应用使用说明

每个 launcher 应用的用途和用法请看 [docs/APPS_CN.md](docs/APPS_CN.md)。
离线地图瓦片下载和安装方式请看 [docs/MAPS_CN.md](docs/MAPS_CN.md)。
新增应用和可选外设处理规则请看
[docs/APP_DEVELOPMENT_RULES_CN.md](docs/APP_DEVELOPMENT_RULES_CN.md)。

Meshtastic 当前作为 Beta 应用提供。语音/图片传输需要两端 K230 都使用 LR2021
模块；SX1262 版本仅支持基础文字 mesh 消息。

## 安装到 SDK

将本仓库放在 SDK 旁边，然后执行：

```sh
./k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

脚本会安装：

- `buildroot-overlay/package/k230_phone_ui`
- 在 `k230_canmv_t_display_rm69a10_defconfig` 中启用 `BR2_PACKAGE_K230_PHONE_UI=y`
- `S99zz_k230_phone_ui` 自启动服务
- 默认媒体目录：`/root/music`、`/root/nes`、`/root/videos`、`/root/photos`、`/root/screenshots`、`/root/recordings`、`/root/lorawan`、`/root/meshtastic`、`/root/notification`、`/root/nrf52840/firmware`
- 可选打包媒体：将 `resources/videos` 同步到镜像内 `/root/videos`
- 可选打包媒体：将 `resources/music` 同步到镜像内 `/root/music`
- 可选打包通知音效：将 `resources/notification` 同步到镜像内 `/root/notification`
- `/root/maps` 运行时地图缓存策略；默认镜像不打包地图瓦片

如果资源目录不在默认位置，可以这样指定：

```sh
K230_RESOURCE_DIR=/path/to/resource ./k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

然后编译：

```sh
make -C ./k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig k230_canmv_t_display_rm69a10_defconfig
make -C ./k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig all
```

SD 卡镜像会生成在：

```text
k230_linux_sdk/output/k230_canmv_t_display_rm69a10_defconfig/images/sysimage-sdcard.img
```

## 截图导出

launcher 可以将当前屏幕保存为 PNG 文件，保存目录为 `/root/screenshots`。

支持两种触发方式：

- 点击主页里的 `Screenshot` 图标。
- 使用硬件键盘按下 `FN + LILYGO`。

需要导出截图时，打开 MTP app，将 SD 卡根目录暴露给电脑，然后从 `/root/screenshots` 拷贝图片。

## 用户数据目录

Launcher 会使用设备上的这些目录：

```text
/root/music                本地音乐文件
/root/videos               视频文件
/root/photos               相机照片和缩略图
/root/screenshots          PNG 截图
/root/recordings           录音文件
/root/nes                  用户自行提供的 NES ROM
/root/lorawan              LoRaWAN profile JSON 文件
/root/meshtastic           Meshtastic 频道、profile 和缓存文件
/root/notification         通知音效文件
/root/nrf52840/firmware    nRF52840 DFU 升级包，通常命名为 firmware.zip
/root/maps                 用户自行复制的离线地图瓦片
```

默认镜像不内置 NES ROM 和离线地图瓦片。地图瓦片目录结构和下载方式请看
[docs/MAPS_CN.md](docs/MAPS_CN.md)。

## 注意

launcher 依赖 BSP 提供的硬件接口、系统服务和运行环境。

日常开发时，修改本仓库源码后重新运行 `scripts/install_to_sdk.sh`，再重新构建 SDK 或对应 Buildroot 包。
