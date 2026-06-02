# Refactor BLE Keyboard to use esp_hid device API

## Problem

The current ESPHome BLE Keyboard component on the `esp-idf-migration` branch implements an HID keyboard from scratch using low-level NimBLE APIs (custom `ble_gatt_svc_def`, `ble_gatts_add_svcs`, `ble_gatts_notify_custom`). The result works ~90% of the way: pairing, GATT discovery, advertising, battery service, LED output, reconnection all work. But keystrokes do not reach the host:

- Windows completes pairing (`Encryption change: status=0`).
- Windows subscribes to notifications (`Subscribe cur_notify=1`).
- NimBLE reports `rc=0` on `ble_gatts_notify_custom` from our code.
- But Windows silently discards the notifications and never applies the keystroke.

This state is the result of 30+ commits trying to make our custom GATT table work, comparing to other examples, and chasing the issue from multiple angles. We have logs, nRF Connect verification of the GATT database, and even external AI review. The conclusion: the same NimBLE stack combined with Espressif's `esp_hid` device API DOES work with Windows (per the official `esp_hid_device` example), but our hand-rolled implementation does not.

## Root cause analysis

Working hypothesis: `ble_gatts_notify_custom` in our implementation produces packets that Windows rejects. The exact cause is unclear without a BLE sniffer, but the Espressif `esp_hid` device library uses the same NimBLE primitives (`ble_gatts_notify_custom`) internally and DOES work with Windows. The difference is the surrounding context that `esp_hid` sets up:

- Service table layout (how `ble_svc_hid` orders characteristics and descriptors).
- Protocol mode handling.
- Battery service include with HID.
- Connection state tracking.

Re-implementing all of this by hand has hit a wall. Switching to the official `esp_hid` library is the path Espressif themselves support and test.

## Goal

Replace the hand-rolled HID service implementation in `components/ble_keyboard/` with one that uses Espressif's `esp_hid` device library, while preserving:

- The public ESPHome component API (the `ble_keyboard::` namespace, `Esp32BleKeyboard` class, the `press()`, `release()`, `combination()`, `print()`, `set_battery_level()`, `start()`, `stop()` methods).
- The YAML configuration in `examples/esp32c3.yaml` (no breaking changes there).
- Home Assistant integration via the existing `number.cpp` (battery level slider) and `button.cpp` (release, Ctrl+A, Calc, Start/Stop advertising).

## Non-goals

- Switching the ESPHome framework away from ESP-IDF (the user explicitly said the default framework is ESP-IDF and we must keep it).
- Restoring a pre-existing Arduino-based implementation.
- Adding new HID features (mouse, consumer controls beyond what we have today). If the consumer-control path stops working after the refactor we will diagnose it as a follow-up; for now we just want the keyboard path to work end-to-end.

## Success criteria

A user flashing the firmware on an ESP32-C3 must be able to:

1. See `ControlPadOffice` in the Windows Bluetooth add-device list.
2. Pair the device. The serial log must show `Encryption change: status=0` and `Subscribe cur_notify=1` events.
3. Press a button in Home Assistant labeled "Space" (or any other key) and have the character typed into a focused Windows text field.
4. Trigger a "Release" button and have the keyboard state cleared.
5. See the battery slider in Home Assistant update Windows' battery reading (the reverse direction — slider value changing the displayed battery in Windows).
6. Re-flash the firmware without losing the ability to reconnect (no need to remove and re-pair).

The success criteria are observable from the serial log and from the Windows Bluetooth panel; no BLE sniffer is required.

## Approach

Use Espressif's `esp_hid` component with the NimBLE backend. The component already implements everything we need:

- `esp_hidd_dev_init` registers a HID device with all the standard services (HID, Battery, Device Information).
- `esp_hidd_dev_input_set` sends an INPUT report to the host. Internally it uses NimBLE primitives but in a context that the host accepts.
- `esp_hid_ble_gap_adv_init` and `esp_hid_ble_gap_adv_start` set up advertising with the right appearance and service UUIDs.

We do NOT need to write any GATT service definition, descriptor, or notification code by hand. The component does it.

The component will keep the same public API (the `Esp32BleKeyboard` C++ class) and the same internal data flow:

- `press()` / `release()` / `combination()` / `print()` build HID reports.
- Instead of calling `ble_gatts_notify_custom`, they call `esp_hidd_dev_input_set(s_hid_dev, 0, report_id, buffer, length)`.
- The connection state is observed via `esp_hidd_dev_event_handler_register` callbacks (`ESP_HIDD_CONNECT_EVENT` / `ESP_HIDD_DISCONNECT_EVENT`) instead of raw NimBLE `BLE_GAP_EVENT_CONNECT` / `DISCONNECT`.
- The battery slider in Home Assistant still calls `set_battery_level`, which calls `esp_hidd_dev_battery_set`.

## Risks

- The `esp_hid` component is not a default dependency of ESPHome. We need to make sure the component is registered with PlatformIO's `ESP-IDF` build (likely through `cmake_requires` or by listing the component in the build) so that `nimble_hidd.c` is compiled into the firmware.
- The `esp_hid` example depends on `esp_hid_gap.c` which is shipped in the example directory, not in the component itself. We will either vendor that file into our component or fold the relevant bits into our component's `setup()`.
- Advertising re-start on disconnect has to be re-implemented via the `ESP_HIDD_DISCONNECT_EVENT` callback.
- The `ble_store_config_init()` (NVS-backed bond store) needs to be wired in the NimBLE init, otherwise reconnection after reboot will keep hitting the `status=7 AUTHREQ` loop we saw earlier.

## Open questions

- Should we keep the separate boot keyboard / boot keyboard output characteristics, or rely solely on the report-mode characteristics? The boot characteristics are required by the HOGP spec for BIOS/legacy compatibility, so we'll keep them — but only if `esp_hid` lets us configure that. (If not, we drop them and the success criteria still hold for normal Windows use.)

## Reference material

- Espressif example: `examples/bluetooth/esp_hid_device/main/esp_hid_device_main.c`
- Component header: `components/esp_hid/include/esp_hidd.h` and `esp_hid_common.h`
- Working demo logs we captured earlier, in particular the consumer-control notification `40 00` (no Report ID prefix) which is what the host expects in a notification, contrasted with the 9-byte payload we were sending.
