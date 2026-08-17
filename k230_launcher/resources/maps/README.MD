# Runtime map cache

Map tiles are not bundled into firmware images.

The launcher reads offline tiles from `/root/maps` on the device. A future map
download tool can populate that directory by selected region and zoom level.
