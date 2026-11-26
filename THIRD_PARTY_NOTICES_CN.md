# 第三方组件与许可证说明

本启动器包含多个模拟器核心和相关第三方组件。各组件保留自己的版权和
许可证条件。本文件是模拟器相关条款的汇总，完整许可证原文仍位于各自
源码目录中。

## ROM 和游戏内容

本仓库和默认 SD 卡镜像不包含商业游戏 ROM、卡带转储、BIOS、游戏音乐、
游戏美术或游戏说明书。启动器只提供本地文件浏览和模拟器运行功能。用户
必须自行确认 ROM 的取得和使用在所在司法辖区具有必要权利。请勿将 ROM、
商业游戏封面或其他受版权保护的游戏资源提交到本仓库。

游戏机和游戏名称仅用于说明支持的文件格式和模拟器目标，不表示获得游戏机
厂商或游戏发行商的授权、背书或关联关系。

## 模拟器组件

### NES：nofrendo

- 许可证：GNU Library General Public License version 2（LGPL-2.0）。
- 原文：`k230_launcher/k230_phone_ui/src/third_party/nofrendo/COPYING.LGPL`。
- 核心被链接进 `k230_phone_ui`。分发者必须保留 LGPL 通知，并根据实际
  二进制分发方式履行 LGPL 要求；在适用时，应允许用户使用修改后的核心
  重新链接程序。

### Mega Drive：Gwenesis 及 CPU 组件

- Gwenesis 许可证：GNU Affero General Public License version 3（AGPL-3.0）。
- 原文：`k230_launcher/k230_phone_ui/src/third_party/gwenesis/LICENSE`。
- Gwenesis 快照和 K230 修改见
  `k230_launcher/k230_phone_ui/src/third_party/gwenesis/README.k230.md`。
- 随附的 Z80 替代实现使用 zlib/libpng 许可证，原文位于
  `k230_launcher/k230_phone_ui/src/third_party/chips/LICENSE`。
- 随附的 68000 替代实现使用 MIT 许可证，原文位于
  `k230_launcher/k230_phone_ui/src/third_party/musashi/LICENSE`。
- SoftFloat 源码还有额外说明，见
  `k230_launcher/k230_phone_ui/src/third_party/musashi/softfloat/README.txt`。
- 该核心静态链接进启动器。分发二进制或商业产品前，必须审查 AGPL 的
  对应源代码、许可证通知和 User Product 等要求。

### Game Boy 和 Game Boy Color：SameBoy

- 许可证：Expat license（MIT 风格许可证）。
- 原文：`k230_launcher/k230_phone_ui/src/third_party/sameboy/LICENSE`。
- 使用的 SameBoy 可移植核心版本、提交和裁剪范围见
  `k230_launcher/k230_phone_ui/src/third_party/sameboy/README.k230.md`。
- 按许可证要求，在源码和二进制分发中保留版权和许可声明。

### SNES：Snes9x 2005

- 许可证：Snes9x 特殊非商业许可证，并包含 GPL 许可组件。
- 原文：`k230_launcher/k230_phone_ui/src/third_party/snes9x/src/LICENSE`。
- 导入的源码快照和 K230 修改见
  `k230_launcher/k230_phone_ui/src/third_party/snes9x/README.k230.md`。
- Snes9x 许可证允许个人、教育和其他非商业用途，但要求保留其通知；
  对商业用途，包括商业产品、收费软件包以及为商业产品进行宣传，许可证
  明确要求先取得相关版权持有人的许可。未经许可，不要将该核心用于商业
  产品或商业分发。

## 发布检查清单

发布源码或二进制镜像时：

1. 保留本文件和所有引用的上游许可证原文。
2. 保留版权声明，并注明 K230 侧的修改。
3. 对 LGPL 或 AGPL 组件，按实际分发形式提供许可证要求的对应源代码
   和重新链接所需材料。
4. 不要打包或链接到未经授权的商业 ROM、BIOS、封面、截图、音乐或其他
   游戏资源。
5. 任何商业使用前重新检查 Snes9x 的非商业限制并取得必要许可。

本文件是工程合规汇总，不构成法律意见。商业产品、收费分发或包含第三方
ROM 等受版权保护资源的发布，应由具备相关经验的律师进行审查。
