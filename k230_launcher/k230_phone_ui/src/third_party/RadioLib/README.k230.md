# RadioLib Vendor Snapshot for K230

This directory contains the minimal RadioLib source snapshot used by the
K230 phone UI LoRa app.

- Upstream: https://github.com/jgromes/RadioLib
- Branch: `master`
- Commit: `0795caa41c6350a2f862137cfc22528c2aaad2bc`
- License: MIT, see `license.txt`

Only the files needed for SX1262 LoRa operation are copied here. The K230 app
provides its own Linux HAL in `../../ui_lora.cpp`, using spidev for SPI and
libgpiod for GPIO and DIO1 edge events.
