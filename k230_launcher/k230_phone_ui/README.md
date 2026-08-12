# k230_phone_ui source

这个目录是 `k230_phone_ui` 应用源码。安装到 SDK 时，
`k230_launcher/scripts/install_to_sdk.sh` 会把这个目录同步到：

```text
buildroot-overlay/package/k230_phone_ui/
```

`src/fonts/ALIBABA-PUHUITI-MEDIUM.TTF` 会随应用安装到
`/root/app/k230_phone_ui/fonts/`，用于 LVGL FreeType 中文显示。
`src/fonts/NotoSansJP-Regular.otf` 会随应用安装到同一目录，用于日语界面显示。
如果后续需要替换日语字体，保持文件名 `NotoSansJP-Regular.otf` 不变即可。

手动同步命令：

```bash
rsync -a --delete \
  k230_launcher/k230_phone_ui/ \
  k230_linux_sdk/buildroot-overlay/package/k230_phone_ui/
```

同步后可单独重编应用：

```bash
make -C k230_linux_sdk CONF=k230_canmv_t_display_rm69a10_defconfig k230_phone_ui-rebuild
```
