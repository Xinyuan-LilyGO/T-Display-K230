<div align="center" markdown="1">
  <img src="../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 LilyGo T-Display K230 Launcher 🌟</h1>

这个目录存放 LILYGO T-Display K230 手机风格 LVGL 启动器应用。

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

## 安装到 SDK

将本仓库放在 SDK 旁边，然后执行：

```sh
./k230_launcher/scripts/install_to_sdk.sh ./k230_linux_sdk
```

脚本会安装：

- `buildroot-overlay/package/k230_phone_ui`
- 在 `k230_canmv_t_display_rm69a10_defconfig` 中启用 `BR2_PACKAGE_K230_PHONE_UI=y`
- `S99zz_k230_phone_ui` 自启动服务
- 默认媒体目录：`/root/music`、`/root/nes`、`/root/videos`、`/root/photos`、`/root/screenshots`、`/root/recordings`、`/root/lorawan`
- 可选打包媒体：将 `resources/videos` 同步到镜像内 `/root/videos`
- 可选打包媒体：将 `resources/music` 同步到镜像内 `/root/music`

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

## 注意

launcher 依赖 BSP 提供的硬件接口、系统服务和运行环境。

日常开发时，修改本仓库源码后重新运行 `scripts/install_to_sdk.sh`，再重新构建 SDK 或对应 Buildroot 包。
