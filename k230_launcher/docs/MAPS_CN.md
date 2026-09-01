# 离线地图瓦片

Meshtastic 地图页面会从 SD 卡根文件系统读取离线地图瓦片：

```text
/root/maps/openstreetmap/<zoom>/<x>/<y>.png
```

当前 Launcher 期望：

- PNG 瓦片
- 每张瓦片 256 x 256 像素
- Web Mercator / slippy-map 瓦片坐标
- style 目录名称：`openstreetmap`
- Launcher 使用的缩放等级：`5` 到 `14`

默认镜像不内置地图瓦片。

## 在设备上下载瓦片

打开 `Meshtastic` > `Map`，点击 `Tiles` 进入瓦片下载页面。

下载前需要先连接 Wi-Fi 或以太网。点击 `Download` 时，Launcher 会先检查网络状态；
如果没有可用网络，会提示用户先连接网络，不会启动下载任务。

设备端内置了几个常用瓦片源，进入 `Tiles` 后可以直接选择：

- `OSM`：OpenStreetMap DE 瓦片
- `OSM-FR`：OpenStreetMap France 瓦片
- `AMap`：高德标准地图瓦片

也可以使用 `Custom` 添加自定义瓦片源。自定义源会持久化保存，可以再次编辑或删除。
自定义 URL 模板必须包含 `{z}`、`{x}`、`{y}` 三个占位符，例如：

```text
https://your-tile-server.example/tiles/{z}/{x}/{y}.png
```

下载时会保存到：

```text
/root/maps/openstreetmap/<zoom>/<x>/<y>.png
```

点击 `Download` 后会先显示确认提示和预计下载数量。建议先选择小范围和较低 zoom 等级测试。
设备会估算瓦片数量，单次下载数量过大时会阻止开始，避免占用过多网络、存储和瓦片服务资源。
公共瓦片服务通常不适合批量下载大范围高 zoom 瓦片，请遵守所选瓦片源的使用条款。

## 推荐下载方式

使用兼容 Meshtastic UI 的瓦片下载工具，然后将导出的瓦片复制到设备：

- 瓦片下载工具：<https://download.tiles.coalition.space/>
- 预生成瓦片包：<https://download.tiles.coalition.space/bundles>

该服务不隶属于 LILYGO 或本仓库。大量下载前请先阅读服务说明和地图数据授权要求。

第一次测试建议只下载实际需要的区域，不要对大范围区域下载过高 zoom 等级：

- 区域：所在城市或省份
- zoom：`5` 到 `12` 适合概览和城市级浏览
- zoom：`13` 到 `14` 只建议用于需要街道级细节的小范围区域

## 通过 MTP 复制瓦片

1. 在电脑上下载并解压瓦片包。
2. 在 T-Display K230 上打开 `MTP` 应用。
3. 通过 USB 连接电脑。
4. 将瓦片目录复制到 `/root/maps`。
5. 确认至少有一个瓦片路径类似：

```text
/root/maps/openstreetmap/12/3346/1731.png
```

如果下载包解压后是 `maps/openstreetmap/...`，请将 `maps` 里面的内容复制到
`/root/maps`。

如果 style 目录名称是 `osm`，请改名为 `openstreetmap`。

## 通过 SSH 复制瓦片

设备可以联网 SSH 时：

```sh
ssh root@192.168.1.100 'mkdir -p /root/maps'
scp -r openstreetmap root@192.168.1.100:/root/maps/
```

如果解压出来包含顶层 `maps` 目录：

```sh
scp -r maps/* root@192.168.1.100:/root/maps/
```

请将 `192.168.1.100` 替换为实际设备 IP。

## 在设备上验证

打开 `Meshtastic` > `Map`。

- 地图会根据当前 GNSS 定位、缓存位置或附近节点位置居中显示。
- 如果地图是空白，先缩小地图。低 zoom 等级需要的瓦片更少，更容易验证。
- 如果只显示灰色或缺失瓦片，请检查目录名称和瓦片路径。当前 Launcher 从
  `/root/maps/openstreetmap` 读取瓦片。
