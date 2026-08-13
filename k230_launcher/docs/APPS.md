# T-Display K230 Launcher App Guide

This guide explains the apps shipped with the T-Display K230 LVGL launcher.

## General Controls

- The home screen is a scrollable app grid. Tap an icon to open an app.
- Back navigation is available from the top-left back button, the configured edge-swipe gesture, or `Esc` on the hardware keyboard when keyboard back navigation is enabled.
- Display orientation, font size, page transition effects, brightness, and screen timeout are configured in `Settings` > `Display`.
- Language is configured in `Settings` > `Language`.
- Audio output and volume defaults are configured in `Settings` > `Audio`.
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
| `LoRa` | Controls SX1262 or LR2021 modules with Factory, Chat, Listen, Auto TX, and Continuous TX modes. | Select or edit a profile, then choose the operating mode. Continuous TX shows an antenna warning before transmitting a carrier. |
| `LoRaWAN` | Manages LoRaWAN profiles and runs OTAA/ABP-style profile tests through the LoRa module. | Load an existing profile, create or edit a profile, validate key lengths, then run the selected profile. Optional simulated telemetry can be enabled. |
| `NES` | Loads `.nes` ROMs from `/root/nes` and runs them with touch controls or the hardware keyboard. | Select a ROM from the list. Keyboard controls: arrows or `W/A/S/D` for direction, `Enter` for Start, `Space` or `Shift` for Select, `U/I/O` for A, `H/J/K` for B, and `Esc` to stop or return to the ROM list. |
| `AI` | Opens the bundled K230 AI demo interface when model resources are installed. | Use it to run supported local AI demos and view their results on the screen. |
| `RTSP` | Starts or stops camera RTSP streaming and shows the stream address. | Enable streaming, then open the displayed URL from a computer on the same network. Stop RTSP before using the `Camera` app. |
| `Wi-Fi` | Scans Wi-Fi networks, saves passwords, connects, and reconnects saved networks. | Turn Wi-Fi on, tap a network, enter a password of at least 8 characters, and wait for the connection result. |
| `Bluetooth` | Scans named BLE devices, connects to a selected device, and displays GATT services, characteristics, and descriptors when available. | Turn Bluetooth on, tap a device, confirm connection, then inspect the device page. |
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
| `USB Modem` | Displays USB modem detection and connection status for supported USB cellular modems. | Connect a modem to USB, open the app, and inspect the detected state and logs. |
| `Settings` | Groups network, display, language, time, audio, keyboard, sensor, and system settings. | Open a category, adjust values, and return. Most user settings are persisted. |
| `System` | Shows system information such as memory, CPU, thermal, display, and network state. | Open it for a compact runtime status overview. |
| `Display` | Adjusts brightness, orientation, font size, transition effects, and screen timeout. | Select the desired display behavior. Screen timeout also turns off keyboard backlight when a keyboard is present. |
| `Screen Test` | Shows solid colors and patterns for display color and bad-pixel checks. | Cycle through the test screens and inspect the panel. |
| `HDMI` | Shows HDMI status and controls the optional HDMI boot/test path when the matching hardware is present. | Use it only with an attached HDMI bridge/display path. AMOLED remains the normal launcher display. |
| `Touch` | Visualizes touchscreen contacts and touch trails. | Place one or more fingers on the panel to check touch reporting. |
| `Motion` | Runs a simple animation scene for display refresh and UI smoothness checks. | Open it to visually inspect animation stability and the reported LVGL frame rate. |
| `About` | Shows board, software, and build information. | Open it to identify the installed launcher/BSP image. |
| `Reboot` | Reboots the board after confirmation. | Use it when a clean software reboot is needed. |

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

