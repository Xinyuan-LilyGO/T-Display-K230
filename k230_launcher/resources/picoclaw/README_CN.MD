# PicoClaw 运行时

构建镜像前可以将 PicoClaw riscv64 运行时放在这里。

支持两种输入：

- `picoclaw`
- `picoclaw_Linux_riscv64.tar.gz`

`k230_launcher/scripts/install_to_sdk.sh` 检测到其中任意一个文件后，会安装到
镜像内的 `/root/picoclaw/bin/picoclaw`。
