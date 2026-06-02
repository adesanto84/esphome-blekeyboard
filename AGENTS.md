# AGENTS.md — ESPHome BLE Keyboard

> Compact guardrails for OpenCode sessions. If a fact is obvious from filenames or standard ESPHome docs, it is omitted.

## What this repo is

ESPHome **external component** that emulates a BLE HID keyboard on ESP32 variants. Hybrid codebase: Python codegen (`__init__.py`, `const.py`) + C++ runtime (`*.cpp`, `*.h`, `esp_hid_gap.c`).

Fork migrated from Arduino → **ESP-IDF native NimBLE** for ESP32-C3/C6/H2 compatibility.

## Architecture

- **Single component directory:** `components/ble_keyboard/`
- **Runtime entry:** `Esp32BleKeyboard` class in `ble_keyboard.cpp/.h`
- **Vendored gap layer:** `esp_hid_gap.c/.h` — copied from ESP-IDF examples (not part of the `esp_hid` component itself). Do not delete or replace with upstream headers.
- **Standalone support:** `CMakeLists.txt` + `Kconfig.projbuild` allow use outside ESPHome (plain PlatformIO + ESP-IDF).
- **Examples:** `examples/esp32.yaml` (legacy ESP32), `examples/esp32c3.yaml` (C3 devkit).

## Hard constraints

### Framework = ESP-IDF only
Arduino framework is **not supported**. Never add Arduino-only libraries (e.g., `NimBLE-Arduino`, `ESP32 BLE Keyboard`).

### `esp_hid` component must be un-excluded
ESPHome excludes `esp_hid` from ESP-IDF builds by default to save flash. The codegen (`__init__.py`) calls:
```python
include_builtin_idf_component("esp_hid")
```
If this is removed, the build fails with missing `esp_hidd_dev_init` / `esp_hidd_dev_input_set`.

### `CONFIG_BT_NIMBLE_HID_SERVICE` is mandatory
The NimBLE backend of `esp_hid` (`esp_ble_hidd_dev_init` in `nimble_hidd.c`) is compiled out unless this Kconfig option is set. The build will fail with:
```
undefined reference to `esp_ble_hidd_dev_init'
```
This option is forced in two places:
1. Python codegen: `add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_HID_SERVICE", True)`
2. `Kconfig.projbuild` (for standalone PlatformIO projects)

Also forced in codegen:
- `CONFIG_BT_ENABLED`
- `CONFIG_BT_BLE_ENABLED`
- `CONFIG_BT_NIMBLE_ENABLED`

## C++ implementation gotchas

### Advertising startup delay
In `setup()`, `esp_nimble_enable()` starts the host task. The host must finish controller sync before advertising can begin. Calling `esp_hid_ble_gap_adv_start()` immediately in `setup()` returns `BLE_HS_EDISABLED` (rc=30).

Fix: `hidd_event_handler()` sets `s_advertising_startup_delay = 2` on `ESP_HIDD_START_EVENT`. `update()` (1s polling interval) counts it down and then starts advertising.

### Security settings for Windows compatibility
The component targets no-IO keyboards. Pairing uses LE Legacy Just Works:
```cpp
ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
ble_hs_cfg.sm_bonding = 1;
ble_hs_cfg.sm_mitm = 0;
ble_hs_cfg.sm_sc = 0;
```
Changing these to Secure Connections or Display capability will break pairing with Windows.

### Bond store
`ble_store_config_init()` is called in `setup()` so the LTK survives reboots. Without it, reconnection hits an AUTHREQ loop.

### HID Report IDs
- **Report ID 1** — keyboard (8-byte report: modifiers + 6 keys)
- **Report ID 2** — consumer/media control (2-byte report)
`esp_hidd_dev_input_set()` is called **without** Report ID prefix (the library prepends it).

## Code conventions

- Wrap all C++ implementation in `#ifdef USE_ESP32`
- Use `esphome/core/log.h` macros (`ESP_LOGI`, `ESP_LOGE`, `ESP_LOGW`)
- Namespace: `esphome::ble_keyboard`
- Python: `AUTO_LOAD = ["binary_sensor", "number", "button"]`, `CODEOWNERS = ["@dmamontov", "@adesanto84"]`

## How to verify changes

There is no traditional unit-test suite. Correctness is verified by compiling the example YAMLs.

### Preferred: Docker (`esphome/esphome:latest`)
When ESPHome is not installed locally (or the local venv/entrypoint is broken), compile with the official Docker image:

```bash
# For legacy ESP32
docker run --rm -v "$(pwd):/config" esphome/esphome:latest \
  esphome compile examples/esp32.yaml

# For ESP32-C3 (primary target)
docker run --rm -v "$(pwd):/config" esphome/esphome:latest \
  esphome compile examples/esp32c3.yaml
```

- Do **not** use the local custom image `esphome-ble:latest`; it has entrypoint problems.
- The repo includes a `pyvenv.cfg` pointing to a Python 3.10 venv (`/opt/homebrew/opt/python@3.10/bin`) which may or may not be present.

## What to avoid

- Do not re-introduce Arduino framework dependencies.
- Do not remove `esp_hid_gap.c` — it is not available in the ESP-IDF component tree.
- Do not change the 2-second advertising delay or security parameters without testing against Windows.
- Do not assume `esp_hid` headers are automatically available; `include_builtin_idf_component` is required.
- Do not introduce `esp_bt_controller_mem_release` failure workarounds that rely on `#if CONFIG_BT_CLASSIC_ENABLED`; use error-code filtering instead (see C3/C6/H2 constraint below).
- Do not delete bonds from the ESP side alone during debugging. **Windows keeps its own bond copy**; deleting from the ESP only causes an AUTHREQ reconnect loop (`status=7` → delete → reconnect → `status=7`). See "Debugging bond issues" below.

## Board-specific gotchas

### ESP32-C3/C6/H2 — `ESP_ERR_NOT_SUPPORTED` on Classic BT release
`esp_hid_gap.c:242-246` calls `esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)`, which returns `ESP_ERR_NOT_SUPPORTED` on BLE-only chips (C3, C6, H2) because they have no Classic BT controller. That error was treated as fatal (`if (ret != ESP_OK)`) and aborted setup.

Fix: filter specifically for `ESP_ERR_NOT_SUPPORTED` and allow the flow to continue:
```cpp
if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
    // only this branch aborts
}
```
Using `#if CONFIG_BT_CLASSIC_ENABLED` was rejected as an alternative because Kconfig can claim Classic BT is enabled on a chip that physically lacks it.

## Debugging bond issues

If pairing works but reconnection fails in an AUTHREQ loop (`status=7`), or Windows shows "Remove device" and keeps reconnecting, **never delete bonds from the ESP side alone**. Windows caches its Long-Term Key (LTK) independently. The only clean-slate procedure is:

1. `esptool.py erase_flash` on the ESP (full NVS wipe)
2. Remove the device from **Windows Bluetooth settings**
3. Pair fresh from scratch

Skipping step 2 will recreate the loop because Windows sends its stale LTK, the ESP rejects it with `status=7`, and any code that auto-deletes the bond on rejection causes infinite disconnect-reconnect cycles.
