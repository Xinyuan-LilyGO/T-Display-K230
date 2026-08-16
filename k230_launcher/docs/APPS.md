<div align="center" markdown="1">
  <img src="../../.github/LilyGo_logo.png" alt="LilyGo logo" width="100"/>
</div>

<h1 align = "center">🌟 T-Display K230 Launcher App Guide 🌟</h1>

This guide explains the apps shipped with the T-Display K230 LVGL launcher.

## General Controls

- The home screen is a scrollable app grid. Tap an icon to open an app.
- Back navigation is available from the top-left back button, the configured edge-swipe gesture, or `Esc` on the hardware keyboard when keyboard back navigation is enabled.
- Edge-swipe back can be enabled in `Settings` > `Display`. When enabled, swipe inward from the left or right screen edge to return to the previous page. On rotated screens, use the visible left/right edge of the current display orientation.
- Display orientation, font size, page transition effects, brightness, and screen timeout are configured in `Settings` > `Display`.
- Language is configured in `Settings` > `Language`. The default language is English, and the default configuration includes Chinese, English, and Japanese.
- Audio output and default volume are configured in `Settings` > `Audio`. The default output uses an external I2S device.
- Incoming message notification sound is configured in `Settings` > `Notifications`.
- If the keyboard base is attached, hardware keyboard input can replace the on-screen keyboard.

## Storage Folders

The launcher creates these folders in the target root filesystem:

| Folder | Used by |
| --- | --- |
| `/root/music` | Local music player |
| `/root/videos` | Video player |
| `/root/nes` | NES ROM browser |
| `/root/photos` | Camera captures and gallery |
| `/root/screenshots` | Screenshot app and keyboard screenshot shortcut |
| `/root/recordings` | Recorder app |
| `/root/lorawan` | LoRaWAN profile files |
| `/root/notification` | Notification sound files |
| `/root/nrf52840/firmware` | nRF52840 DFU application update packages |

Use the `MTP` app to expose these folders to a host computer over USB.

## Apps

| App | What it does | Basic use |
| --- | --- | --- |
| `Camera` | Shows a live camera preview, captures photos, opens the photo viewer, and can overlay face-detection boxes when the model files are installed. | Stop `RTSP` first if it is active. Tap the shutter button to save a photo to `/root/photos`. Use the flip buttons to adjust preview orientation. Tap the thumbnail to browse photos. |
| `Music` | Plays local audio files from `/root/music` with album art, playlist browsing, seek bar, volume control, repeat, and shuffle modes. | Select a song from the playlist, then use previous, play/pause, next, repeat, and shuffle controls. Playback can continue after leaving the app. |
| `Video` | Browses videos from `/root/videos` as thumbnails and plays them full screen without stretching the picture. | Tap a video thumbnail to play. Swipe left or right during playback to move to the next or previous video. Audio follows the selected audio output route. |
| `Radio` | Plays network radio streams and custom stream URLs. | Select a preset or enter a custom URL, then use play/pause and volume controls. Network access must be available. |
| `Record` | Records from the board microphone, lists recording files, plays them back, and deletes recordings. | Tap record to start and stop. Open the list to play or delete files from `/root/recordings`. |
| `Mic FFT` | Displays a live microphone spectrum and basic level information. | Use it to check microphone input and adjust the displayed gain for easier signal inspection. |
| `LoRa` | Controls SX1262 or LR2021 modules with Factory, Chat, Listen, Auto TX, and Continuous TX modes. | Select or edit a profile, then choose the operating mode. Continuous TX is for engineering RF testing only, is not recommended for normal use, and must only be used with a suitable antenna connected. |
| `Meshtastic` | Controls the standalone `k230_meshtastic_probe` daemon for a minimal Meshtastic-style LoRa mesh text path, node cache, event log, and profile settings. | The daemon starts when the app opens. Use the settings icon to set region, preset, channel, PSK, TX power, node/from/to/hop/ACK. Type in the message field to send text and open the node page to inspect the node cache. New received messages play the sound selected in `Settings` > `Notifications`. |
| `LoRaWAN` | Manages LoRaWAN profiles and runs OTAA profile tests through the LoRa module. | Load an existing profile, create or edit a profile, validate key lengths, save it, then run the selected profile. Optional simulated telemetry can be enabled. |
| `NES` | Loads `.nes` ROMs from `/root/nes` and runs them with touch controls or the hardware keyboard. | NES ROMs are not bundled by default. Add your own ROM files to `/root/nes`, then select a ROM from the list. Keyboard controls: arrows or `W/A/S/D` for direction, `Enter` for Start, `Space` or `Shift` for Select, `U/I/O` for A, `H/J/K` for B, and `Esc` to stop or return to the ROM list. |
| `AI` | Opens the bundled K230 AI demo interface when model resources are installed. | Use it to run supported local AI demos and view their results on the screen. |
| `RTSP` | Starts or stops camera RTSP streaming and shows the stream address. | Enable streaming, then open the displayed URL from a computer on the same network. Stop RTSP before using the `Camera` app. |
| `Halow` | Sends or receives camera video frames between two boards over an Ethernet-connected Wi-Fi HaLow link. | Connect the HaLow device to the Ethernet port, set the local static IP, enter the peer IP, then choose `Start TX` on the camera side and `Start RX` on the display side. Resolution presets are `320x240`, `640x480`, and `720p`; the default is `320x240`. Received frames are stored under `/root/videos/halow_rx`. |
| `Wi-Fi` | Scans Wi-Fi networks, saves passwords, connects, and reconnects saved networks. | Turn Wi-Fi on, tap a network, enter a password of at least 8 characters, and wait for the connection result. |
| `Bluetooth` | Scans named BLE devices, connects to a selected device, and displays GATT services, characteristics, and descriptors when available. | Turn Bluetooth on, tap a device, confirm connection, then inspect the device page. |
| `nRF DFU` | Updates the onboard nRF52840 AT firmware over the K230 UART link. | Open `MTP`, copy the Adafruit/nrfutil application package as `/root/nrf52840/firmware/firmware.zip`, return to `nRF DFU`, then tap `Update nRF52840`. The app checks the current AT version and the package version before writing; identical versions are rejected. The UI is locked during the update and shows progress; do not power off the board. On failure, check `/tmp/k230_nrf52840_dfu_ui.log`. |
| `MTP` | Enables USB MTP file access for the SD-card/root filesystem folders. | Tap the MTP control, connect USB to a host computer, and manage files such as photos, screenshots, music, videos, and ROMs. |
| `Gallery` | Shows captured photos as thumbnails and opens them in a full-screen viewer. | Tap a thumbnail to view it. Swipe left or right to browse adjacent photos. |
| `Screenshot` | Saves the current screen as a PNG under `/root/screenshots`. | Tap the app icon, or press `FN + LILYGO` on the hardware keyboard. Export files with `MTP`. |
| `Terminal` | Provides an on-device terminal with special keys and font-size controls. | Type commands with the on-screen keyboard or hardware keyboard. Use the special-key row for terminal control keys. |
| `I2S Test` | Generates audio test output for the internal audio route or the MAX98357A external amplifier. | Select the audio output in `Settings` > `Audio`, then run the tone test. |
| `I2C Scan` | Scans available I2C buses and highlights detected device addresses. | Use it to verify AHT20, BQ25896, BQ27220, XL9555, TCA8418, and other board devices. |
| `Battery` | Shows battery gauge, charger, voltage, current, capacity, and related telemetry when supported devices are present. | Open the page to view current battery and charging state. Charger controls are available through Settings when supported. |
| `Keyboard` | Tests the TCA8418 keyboard matrix, keyboard interrupt path, key mapping, and keyboard backlight. | Press hardware keys to light the matching key. Adjust keyboard backlight from the page or Settings. |
| `LED Test` | Controls XL9555-driven LEDs. | Toggle LEDs from the page. The board LED logic is active-low where noted by the UI. |
| `Cellular` | Tests the nRF9151 serial modem path, SIM state, LTE status, GNSS, NMEA output, and satellite C/N0 bars. | Power the modem, test AT link, check SIM, start GNSS, then monitor fix status and signal bars. |
| `USB Modem` | Debug page for USB modem detection and connection status. | This page is currently for engineering/debug visibility only. It does not provide a complete end-user dialing or data-session workflow yet. |
| `Settings` | Groups network, display, language, time, audio, keyboard, sensor, and system settings. | Open a category, adjust values, and return. Most user settings are persisted. |
| `System` | Shows system information such as memory, CPU, thermal, display, and network state. | Open it for a compact runtime status overview. |
| `Display` | Adjusts brightness, orientation, font size, transition effects, and screen timeout. | Select the desired display behavior. Screen timeout also turns off keyboard backlight when a keyboard is present. |
| `Screen Test` | Shows solid colors and patterns for display color and bad-pixel checks. | Cycle through the test screens and inspect the panel. |
| `HDMI` | Shows HDMI status and controls the optional HDMI boot/test path when the matching hardware is present. | Use it only with an attached HDMI bridge/display path. AMOLED remains the normal launcher display. |
| `Touch` | Visualizes touchscreen contacts and touch trails. | Place one or more fingers on the panel to check touch reporting. |
| `Motion` | Runs a simple animation scene for display refresh and UI smoothness checks. | Open it to visually inspect animation stability and the reported LVGL frame rate. |
| `About` | Shows board, software, and build information. | Open it to identify the installed launcher/BSP image. |
| `Reboot` | Reboots the board after confirmation. | Use it when a clean software reboot is needed. |

## LoRaWAN Profile Setup

The `LoRaWAN` app stores profile files under `/root/lorawan`.

1. Open `LoRaWAN`.
2. Tap `New` to create a profile, or `Load` to select an existing `*.json` profile.
3. Choose the `Region` button that matches your LoRaWAN network. Supported region buttons are `EU868`, `US915`, `AU915`, `EU433`, `CN470`, `AS923`, `AS923_2`, `AS923_3`, `AS923_4`, `KR920`, and `IN865`.
4. Fill in `JoinEUI`, `DevEUI`, `AppKey`, and `NwkKey`.
5. `JoinEUI` and `DevEUI` must be 16 hexadecimal digits. `AppKey` and `NwkKey` must be 32 hexadecimal digits.
6. Set `Sub-band` when required by the selected region or network server. Use `0` for the default.
7. Set `Interval` in seconds. The app enforces a minimum interval of 60 seconds.
8. Set `FPort` from `1` to `223`.
9. Enable `Confirmed uplink` only when the network expects confirmed frames. Enable `ADR` when the network should manage data rate.
10. Enable simulated temperature upload if you want the app to send generated temperature payloads for testing.
11. Tap `Save profile`, enter or accept a file name such as `lora_wan_0.json`, then return to the main LoRaWAN page.
12. Tap `Run` to prepare the radio, join through OTAA, and send payloads with the saved profile.

Save the profile before running it. The app refuses to run profiles with unsaved changes or invalid key lengths.

## Settings Pages

Some functions are reached through `Settings` instead of direct home icons:

| Settings page | Purpose |
| --- | --- |
| `Network` | Entry point for Wi-Fi, Ethernet, and Bluetooth settings. |
| `Ethernet` | Shows wired network status when an Ethernet adapter is present. |
| `Language` | Selects the launcher language. |
| `Date & time` | Sets NTP and timezone options. |
| `Audio` | Selects output route and default volume. |
| `Keyboard settings` | Controls keyboard detection, keyboard back navigation, and keyboard backlight behavior. |
| `Sensors` | Shows AHT20 temperature/humidity and related sensor status. |
| `Charger` | Shows and configures BQ25896 charger options when present. |
