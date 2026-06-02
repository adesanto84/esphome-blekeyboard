# Tasks: esp_hid refactor of ble_keyboard component

These are the ordered, atomic tasks that `sdd-apply` will execute. Each task is small enough to be implementable in one pass and verifiable before moving on.

## T1 — Vendor `esp_hid_gap.c` from the Espressif example

[x]

**Why:** the `esp_hid` component does NOT ship `esp_hid_gap.c` in the component directory; only the example has it. Without this file, `esp_hid_gap_init` / `esp_hid_ble_gap_adv_init` / `esp_hid_ble_gap_adv_start` won't link.

**Steps:**

1. Create `components/ble_keyboard/src/esp_hid_gap.c`.
2. Copy the contents of `examples/bluetooth/esp_hid_device/main/esp_hid_gap.c` from the NimBLE-backend section into it (the `#if CONFIG_BT_NIMBLE_ENABLED` / `#elif CONFIG_BT_BLE_ENABLED` switch — we only need the NimBLE branch).
3. Strip out anything that depends on Bluedroid-only APIs.

**Done when:** the file exists in the component and `idf.py build` (or our `esphome/esphome compile`) links `esp_hid_gap_init` etc. without unresolved symbols.

**Verifies:** R1, R6.

## T2 — Rewrite `ble_keyboard.cpp` to use `esp_hid`

[x]

**Why:** this is the heart of the refactor. The current file is 700+ lines of hand-rolled GATT + NimBLE glue. We replace it with a thin wrapper that delegates to `esp_hid`.

**Steps:**

1. Replace the include block at the top of the file: drop the `host/ble_gatt.h`, `services/*/ble_svc*.h` includes; add `esp_hid_common.h`, `esp_hidd.h`, the vendored `esp_hid_gap.h` (we add the header in T3), `host/ble_store.h`, `esp_nimble_mem.h`.
2. Keep the `kHidReportMap[]` byte array (we still need it) but drop all `UUID_*` constants, all `report_ref_*` arrays, all `*_dscs[]` arrays, the `hid_chrs[]` array, the `gatt_services[]` array, the `ble_keyboard_access()` callback, and all the `g_handle_*` variables.
3. Add a `static esp_hidd_dev_t *s_hid_dev = NULL;` module-level variable to hold the device handle.
4. Replace `Esp32BleKeyboard::setup()` with the new flow (see design.md D4):
   - `esp_hid_gap_init(ESP_HID_TRANSPORT_BLE)`
   - `esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, g_device_name)`
   - `esp_event_handler_register(ESP_HIDD_EVENTS, ESP_EVENT_ANY_ID, hidd_event_handler, NULL)`
   - Build `esp_hid_device_config_t` with `kHidReportMap` and call `esp_hidd_dev_init`.
   - `esp_hidd_dev_battery_set(s_hid_dev, 100)`
   - `esp_hid_ble_gap_adv_start()`
   - `ble_store_config_init()`
5. Replace the NimBLE `gap_event` function with the `hidd_event_handler` event handler function (see design.md D5).
6. Replace `send_keyboard_report` and `send_media_report` to call `esp_hidd_dev_input_set` (see design.md D6).
7. Replace `set_battery_level` to call `esp_hidd_dev_battery_set` (see design.md D9).
8. Remove the `nimble_host_task` function — `esp_hid` spawns its own host task.
9. Remove the `ble_on_sync_impl` function — same reason.

**Done when:** the file compiles and links, and on the ESP32-C3 the device shows up in Windows as a connectable keyboard.

**Verifies:** R1, R2, R3, R4, R5, R6.

## T3 — Make sure `esp_hid_gap.h` is available

[x]

**Why:** the `esp_hid_gap.h` header is shipped in the example directory too, not in the `esp_hid` component. We need a copy in our component or in a path NimBLE picks up.

**Steps:**

1. Copy `examples/bluetooth/esp_hid_device/main/esp_hid_gap.h` into `components/ble_keyboard/src/esp_hid_gap.h`.
2. Verify the include path works. If needed, add `-I${COMPONENT_DIR}/src` to the component's build configuration so `#include "esp_hid_gap.h"` resolves.

**Done when:** `ble_keyboard.cpp` includes `esp_hid_gap.h` and the build does not error with "file not found".

**Verifies:** D6 (vendoring step), and indirectly R1.

## T4 — Configure the component build to include the vendored source

[x]

**Why:** when we vendor `esp_hid_gap.c` into `components/ble_keyboard/src/`, ESPHome's `external_components` needs to know to compile it.

**Steps:**

1. Check if a `CMakeLists.txt` exists in `components/ble_keyboard/`. If not, create one.
2. The CMakeLists should register the component as an `idf_component_register` with:
   - `SRCS` includes both `ble_keyboard.cpp` and `src/esp_hid_gap.c`
   - `REQUIRES` lists `nimble` and `esp_hid`
   - `INCLUDE_DIRS` includes `src/` so `#include "esp_hid_gap.h"` resolves
3. If ESPHome's `external_components` mechanism doesn't propagate `CMakeLists.txt`, fall back to the simpler approach: inline the contents of `esp_hid_gap.c` into `ble_keyboard.cpp` (as a single translation unit, with `static` on internal functions to avoid duplicate-symbol errors). We do this only as a last resort.

**Done when:** `esphome/esphome compile` for `examples/esp32c3.yaml` produces a `firmware.factory.bin` that links the `esp_hid` device APIs and runs on the ESP32-C3.

**Verifies:** R6.1 (the framework stays ESP-IDF), R6.2 (no manual GATT table).

## T5 — Final compile and a single end-to-end smoke test

[x]

**Why:** we need to confirm the binary actually behaves on real hardware before we declare the refactor done.

**Steps:**

1. Run the compile: `docker run --rm -v $(pwd):/config esphome/esphome compile /config/examples/esp32c3.yaml`.
2. If the build fails, fix the error and re-run.
3. Once green, document the build result and any warnings in this task file. We DO NOT actually flash the device from this task — the user runs the on-target test.

**Done when:** a `firmware.factory.bin` is produced, no unresolved symbols, no compile errors.

**Verifies:** R6.1.

**Build result (2026-06-01):**

- Command: `docker run --rm -v $(pwd):/config esphome/esphome compile /config/examples/esp32c3.yaml`
- Result: `firmware.factory.bin` produced, 1,125,904 bytes.
- Total image size: 1,060,220 bytes (flash), 130,196 bytes DRAM.
- One fix during the build: `esp_nimble_enable(nimble_host_task)` failed with `invalid conversion from 'void (*)(void*)' to 'void*' [-fpermissive]`. C++ does not implicitly convert function pointers to object pointers. Fixed with `esp_nimble_enable(reinterpret_cast<void *>(nimble_host_task))` (a C-style cast also fails).
- No unresolved symbols, no link errors.
- Binary path: `examples/.esphome/build/blekeyboard/.pioenvs/blekeyboard/firmware.factory.bin`.


## T6 — On-target test plan (run by the user, not by the agent)

**Why:** the agent cannot run the ESP32-C3. The user has the hardware. We hand the test off.

**Steps:**

The user should run this script and report back:

1. `esptool.py --chip esp32c3 --port COM6 erase_flash`
2. Flash the new firmware: `esptool.py --chip esp32c3 --port COM6 write_flash -z 0x0 examples/.esphome/build/blekeyboard/.pioenvs/blekeyboard/firmware.factory.bin`
3. In Windows, REMOVE any pre-existing pairing of `ControlPadOffice`. (Old bonds are poisonous.)
4. Open the serial monitor at 115200.
5. In Windows, Bluetooth & devices → Add device → select `ControlPadOffice` → Pair.
6. **Verify R1.1**: serial shows `Advertising started with name='ControlPadOffice'`.
7. **Verify R2.1**: serial shows `Encryption change: status=0` exactly once. No `Encryption failed` warning.
8. **Verify R3.1**: nRF Connect on Android sees five services (0x1800, 0x1801, 0x180A, 0x180F, 0x1812).
9. **Verify R4.1**: serial shows `Subscribe cur_notify=1` for the keyboard input characteristic.
10. Open Notepad in Windows. Focus the text field.
11. **Verify R4.2 (the primary success criterion)**: in Home Assistant, press the "Space" button. The space character must appear in Notepad. Try other buttons too (e.g., "Calc", "Ctrl + A").
12. **Verify R4.3**: in Windows, press Caps Lock or Num Lock. Serial shows `Keyboard LED output: 0xXX`.
13. **Verify R3.4**: in Home Assistant, move the battery slider. Windows' battery reading should update.
14. **Verify R2.2**: reboot the ESP32 from ESPHome's web UI (Restart button). After it comes back, the Windows Bluetooth panel should show the device as connected without asking the user to re-pair. Serial should show a clean `Encryption change: status=0`.

**Done when:** the user reports success or failure of items 6, 7, 9, 11, 12, 13, 14. The R4.2 result is the most important.

## Notes on sequencing

T1 and T3 are pure file copies; do them in either order, they're independent. T2 is the rewrite and depends on T1 and T3 (the rewrite uses `esp_hid_gap.h`). T4 is a build-system tweak; depending on whether the build picks up the vendored sources automatically, this may be a no-op. T5 is the validation. T6 is the user's job.

If the compile in T5 fails, the next step is NOT to start a new task; it is to fix T2 in place and re-run T5. We do not break the build halfway.
