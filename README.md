# ESPHome BLE Keyboard

Component for ESPHome that allows you to emulate a Bluetooth keyboard.

Originally created by [Dmitry Mamontov](https://github.com/dmamontov).
Fork migrated to **ESP-IDF native NimBLE** by [adesanto84](https://github.com/adesanto84) for ESP32-C3/C6/H2 compatibility.

## Features

- Emulates a Bluetooth Low Energy (BLE) keyboard
- Works with **ESP-IDF framework** (no Arduino dependencies)
- Compatible with **ESP32, ESP32-C3, ESP32-C6, ESP32-H2**
- Supports standard keys, media keys, and combinations
- Configurable press/release delays
- Battery level reporting
- Connection state binary sensor
- Automation actions: `print`, `press`, `release`, `combination`, `start`, `stop`

## Requirements

- ESPHome **2026.2.0** or newer
- `framework: esp-idf` (Arduino framework is **not supported**)
- NimBLE enabled in `sdkconfig_options`

## Installation

Add to your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/adesanto84/esphome-blekeyboard
      ref: esp-idf-migration

ble_keyboard:
  name: "MyBleKeyboard"
  manufacturer_id: "ESPHome"
  battery_level: 100
  reconnect: true
  buttons: true
```

### ESP-IDF Configuration (required for C3/C6/H2)

```yaml
esp32:
  board: esp32-c3-devkitm-1
  variant: ESP32C3
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_BT_ENABLED: y
      CONFIG_BT_NIMBLE_ENABLED: y
      CONFIG_BT_NIMBLE_ROLE_BROADCASTER: y
      CONFIG_BT_NIMBLE_ROLE_PERIPHERAL: y
```

## Automation Example

```yaml
button:
  - platform: template
    name: "Type Hello"
    on_press:
      - ble_keyboard.print:
          id: ble_keyboard_1
          text: "Hello World"
```

## Breaking Changes from Original

- **Framework:** Changed from Arduino to ESP-IDF. If you were using `framework: arduino`, switch to `framework: esp-idf`.
- **No external libraries:** Removed dependency on `NimBLE-Arduino` and `ESP32 BLE Keyboard`. All HID logic is now native NimBLE.

## License

MIT License. See [LICENSE](LICENSE) for details.
