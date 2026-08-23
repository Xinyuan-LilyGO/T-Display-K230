# Offline map tile resources

Map tiles are not bundled into firmware images.

The launcher reads offline tiles from:

```text
/root/maps/openstreetmap/<zoom>/<x>/<y>.png
```

Use the MTP app or SSH to copy downloaded tiles to `/root/maps` on the device.

See:

- [../../docs/MAPS.md](../../docs/MAPS.md)
- [../../docs/MAPS_CN.md](../../docs/MAPS_CN.md)
