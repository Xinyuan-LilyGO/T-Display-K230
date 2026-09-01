# Offline Map Tiles

The Meshtastic map view reads offline map tiles from the SD-card root
filesystem:

```text
/root/maps/openstreetmap/<zoom>/<x>/<y>.png
```

The current launcher expects:

- PNG tiles
- 256 x 256 pixels per tile
- Web Mercator / slippy-map tile coordinates
- style folder name: `openstreetmap`
- zoom range used by the launcher: `5` to `14`

Map tiles are not bundled into the default image.

## Download Tiles On The Device

Open `Meshtastic` > `Map`, then tap `Tiles` to open the tile download page.

The board must be connected to Wi-Fi or Ethernet before downloading. When the
user taps `Download`, the launcher checks network availability first. If no
usable network is detected, it shows a prompt and does not start the download
job.

The device-side downloader includes a few common tile sources:

- `OSM`: OpenStreetMap DE tiles
- `OSM-FR`: OpenStreetMap France tiles
- `AMap`: AMap standard map tiles

You can also add a `Custom` tile source. The custom source is persisted and can
be edited or deleted later. A custom URL template must include the `{z}`, `{x}`,
and `{y}` placeholders, for example:

```text
https://your-tile-server.example/tiles/{z}/{x}/{y}.png
```

Downloaded tiles are stored under:

```text
/root/maps/openstreetmap/<zoom>/<x>/<y>.png
```

When `Download` is tapped, the launcher shows a confirmation prompt with the
estimated tile count. Start with a small area and lower zoom levels. The
launcher blocks overly large jobs to avoid excessive network, storage, and
tile-service usage. Public tile services are usually not intended for bulk
large-area high-zoom downloads; follow the selected tile source terms.

## Recommended Download Method

Use a Meshtastic UI compatible tile downloader, then copy the exported tiles to
the device:

- Tile downloader: <https://download.tiles.coalition.space/>
- Prebuilt bundles: <https://download.tiles.coalition.space/bundles>

This service is not affiliated with LILYGO or this repository. Check the service
instructions and map data license before large downloads.

For a compact first test, download only the area you need and avoid very high
zoom levels for a large region. A practical starting point is:

- region: your city or province
- zoom: `5` to `12` for overview and city-level browsing
- zoom: `13` to `14` only for the small area where street-level detail is needed

## Copy Tiles With MTP

1. Download and extract the tile package on your computer.
2. Open the `MTP` app on the T-Display K230.
3. Connect USB to the computer.
4. Copy the tile folder into `/root/maps`.
5. Confirm that at least one tile path looks like:

```text
/root/maps/openstreetmap/12/3346/1731.png
```

If the downloaded package contains `maps/openstreetmap/...`, copy the contents
of `maps` into `/root/maps`.

If the style folder is named `osm`, rename it to `openstreetmap`.

## Copy Tiles Over SSH

When the board is reachable over the network:

```sh
ssh root@192.168.1.100 'mkdir -p /root/maps'
scp -r openstreetmap root@192.168.1.100:/root/maps/
```

If the extracted package contains a top-level `maps` folder:

```sh
scp -r maps/* root@192.168.1.100:/root/maps/
```

Replace `192.168.1.100` with the board IP address.

## Verify On The Device

Open `Meshtastic` > `Map`.

- The map is centered from the current GNSS fix, a cached position, or a nearby
  node position when available.
- If the map is blank, zoom out first. Lower zoom levels need fewer tiles and
  are easier to verify.
- If only gray or missing tiles are shown, check the directory name and tile
  path. The launcher currently looks under `/root/maps/openstreetmap`.
