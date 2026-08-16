# Launcher App Development Rules

These rules apply to apps added to the T-Display K230 launcher.

## Hardware Groups

The launcher must treat board peripherals as optional unless the app is built
around a core K230 feature.

| Group | Hardware |
| --- | --- |
| Main K230 board | K230, AHT20, camera, Ethernet, HDMI |
| nRF52840 board | nRF52840 BLE bridge, external I2S amplifier |
| nRF9151 keyboard board | nRF9151, TCA8418/TCA8414 keyboard, BQ27220, BQ25896, keyboard backlight |

## Optional Peripheral Rule

If an app depends on an optional peripheral, probe that peripheral before using
it. If the peripheral is not present, skip that feature and keep the rest of the
app usable.

Examples:

- Battery level and current require `BQ27220`. Do not publish or display
  battery telemetry as valid unless the gauge is detected or a valid kernel
  battery power-supply node is present.
- Charger input and charge-current controls require `BQ25896`. Hide or disable
  charger actions when the charger is not detected.
- Hardware keyboard and keyboard backlight actions require the keyboard base.
  Keep software input available when the keyboard is absent.
- nRF9151 LTE/GNSS features must first verify the modem UART and AT response.
  Meshtastic position features must skip GNSS publishing when nRF9151 is absent.
- AHT20 temperature/humidity should be published only when the sensor read
  succeeds.

## UI Behavior

- Report unavailable optional hardware as unavailable, not as a hard app error.
- Do not block core app workflows because one optional sensor is missing.
- Persist user settings only after the hardware action or validation succeeds.
- Keep landscape and portrait layouts separate when a screen has dense controls.
