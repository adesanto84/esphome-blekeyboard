# Design: esp_hid refactor of ble_keyboard component

This document describes the concrete code changes for the refactor. It maps each spec requirement (R1–R6) to the file(s) and the change(s) needed.

## D1 — Component source layout

We keep the same public file layout:

- `components/ble_keyboard/ble_keyboard.cpp` — implementation
- `components/ble_keyboard/ble_keyboard.h` — public C++ class declaration
- `components/ble_keyboard/__init__.py` — ESPHome codegen
- `components/ble_keyboard/automation.h` — action templates
- `components/ble_keyboard/number.cpp` — battery slider
- `components/ble_keyboard/button.cpp` — Restart, Release, Ctrl+A, Calc, etc.
- `components/ble_keyboard/const.py` — key codes
- `examples/esp32c3.yaml` — example configuration

`ble_keyboard.cpp` is rewritten. All other files are unchanged in this refactor.

## D2 — New includes in `ble_keyboard.cpp`

Drop the low-level NimBLE includes that we no longer touch directly:

```c
// REMOVE these includes
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/bas/ble_svc_bas.h"
#include "services/dis/ble_svc_dis.h"
```

Add the `esp_hid` includes plus the NimBLE host task helpers we still need:

```c
// ADD these includes
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"        // For esp_hid_ble_gap_adv_init / adv_start. We will vendor this in.
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"     // For ble_store_config_init().
#include "esp_nimble_mem.h"     // For nimble_platform_mem_* when BLE_STATIC_TO_DYNAMIC is on.
```

The `esp_hid_gap.h` header is shipped in the `examples/bluetooth/esp_hid_device/main/` directory, not in the `esp_hid` component itself. We will vendor the matching `.c` into our component (see D6).

## D3 — Drop the hand-rolled GATT table

Delete from `ble_keyboard.cpp`:

- `kHidReportMap[]` — we still need this byte sequence; we will hand it to `esp_hid` as an `esp_hid_raw_report_map_t`. (No code change in terms of bytes.)
- All `UUID_*` constants — `esp_hid` resolves the standard UUIDs itself.
- `report_ref_keyboard[]`, `report_ref_media[]`, `report_ref_output[]` — `esp_hid` builds descriptors from the parsed Report Map.
- The `report_map_dscs[]`, `keyboard_report_dscs[]`, `media_report_dscs[]`, `output_report_dscs[]` arrays — `esp_hid` synthesizes them.
- The `hid_chrs[]` array — `esp_hid` builds the characteristic list internally.
- The `gatt_services[]` array — `esp_hid` registers everything in one call.
- `ble_keyboard_access()` — `esp_hid` has its own access callback. We still need to know about writes to the Output Report (the LED byte) so the existing `keyboard_output_report_[]` buffer is kept and an event handler updates it.
- The Report Reference / External Report Reference descriptors we added earlier — `esp_hid` does this.

We keep:

- `keyboard_report_[]`, `media_report_[]`, `boot_key_report_[]`, `keyboard_output_report_[]` as local buffers. We populate them in the `press()` / `release()` / `print()` paths and pass them to `esp_hidd_dev_input_set`.
- The `g_connected` flag, set from the `ESP_HIDD_CONNECT_EVENT` / `ESP_HIDD_DISCONNECT_EVENT` callbacks (not from raw NimBLE gap events).
- The ASCII → HID keycode lookup table.

## D4 — `setup()` becomes a thin wrapper around `esp_hid`

The new `Esp32BleKeyboard::setup()` performs these steps, in this order:

1. `esp_hid_gap_init(ESP_HID_TRANSPORT_BLE)`. Initializes the NimBLE host task, the controller, the standard NimBLE service handles (GAP, GATT, BAS, DIS, the HID service). This is the NimBLE peer of `ble_svc_*_init`.

2. `esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, g_device_name)`. Sets the appearance and device name. This is the function that failed to link the first time we tried it; we will vendor the source from the example to make sure it's available.

3. `esp_event_handler_register(ESP_HIDD_EVENTS, ESP_EVENT_ANY_ID, hidd_event_handler, NULL)`. Subscribes our C++ event handler to HID device events.

4. Build the `esp_hid_device_config_t`:

   ```c
   static esp_hid_raw_report_map_t report_maps[] = {
       { .data = kHidReportMap, .len = sizeof(kHidReportMap) },
   };
   esp_hid_device_config_t hid_config = {
       .vendor_id          = 0x1234,
       .product_id         = 0x0001,
       .version            = 0x0001,
       .device_name        = g_device_name,
       .manufacturer_name  = g_manufacturer_id,
       .serial_number      = "1234567890",
       .report_maps        = report_maps,
       .report_maps_len    = 1,
   };
   ```

5. `esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_handler, &s_hid_dev)`. Registers the device. After this call, `s_hid_dev` is non-NULL and we can send reports.

6. `esp_hidd_dev_battery_set(s_hid_dev, 100)`. Initial battery level, until Home Assistant overrides it.

7. `esp_hid_ble_gap_adv_start()`. Start advertising.

8. `ble_store_config_init()`. Initialize the NimBLE bond store backed by NVS. This is what makes R2.2 work — without it, a reboot loses the bond and the next connect hits the `status=7` loop.

Note that we no longer call `nimble_port_init()` separately. `esp_hid_gap_init` does that for us, and then `esp_hid_ble_gap_adv_init` calls `nimble_port_freertos_init` internally. We must remove our own `nimble_port_freertos_init` call to avoid starting two host tasks.

## D5 — Event handler replaces `gap_event`

The new `hidd_event_handler` is a C function (not a class member) that ESPHome-style code can register with `esp_event_handler_register`. It handles the events we used to handle in the NimBLE `gap_event` callback:

```c
static void hidd_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
    esp_hidd_event_t event = (esp_hidd_event_t) id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *) event_data;

    switch (event) {
        case ESP_HIDD_CONNECT_EVENT:
            g_connected = true;
            break;
        case ESP_HIDD_DISCONNECT_EVENT:
            g_connected = false;
            esp_hid_ble_gap_adv_start();        // Re-start advertising (R1.2)
            break;
        case ESP_HIDD_OUTPUT_EVENT:
            // Windows wrote the LED state to the Output Report.
            memcpy(keyboard_output_report_, param->output.data, param->output.length);
            ESP_LOGI(TAG, "Keyboard LED output: 0x%02X", keyboard_output_report_[0]);
            break;
        default:
            break;
    }
}
```

We no longer need to handle `BLE_GAP_EVENT_CONNECT` / `DISCONNECT` / `MTU` / `CONN_UPDATE` ourselves. `esp_hid` and the NimBLE host handle the lower-level events internally.

For `BLE_GAP_EVENT_CONN_UPDATE_REQ` (R6.3), the example `esp_hid_device` example shows that the NimBLE host's default behavior is to accept it. If we ever need a custom response, we can register a NimBLE gap callback with `ble_hs_cfg.gap_event_cb` instead of `gap_event`. For now, default behavior is enough.

## D6 — `send_keyboard_report` and `send_media_report` use `esp_hid`

The new implementation is a one-liner per function. We no longer need to construct an mbuf, no longer need to remember handle values, no longer need to think about Report ID prefix vs no prefix.

```c
void Esp32BleKeyboard::send_keyboard_report(uint8_t modifiers, uint8_t key1, ...) {
    if (s_hid_dev == nullptr) return;

    // Internal layout: 1 modifier byte, 1 reserved byte, 6 keycode bytes = 8 bytes.
    uint8_t buffer[8] = { modifiers, 0, key1, key2, key3, key4, key5, key6 };
    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0, /* report_id = */ 1,
                                           buffer, sizeof(buffer));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_input_set failed: %d", err);
    }
}

void Esp32BleKeyboard::send_media_report(uint8_t byte0, uint8_t byte1) {
    if (s_hid_dev == nullptr) return;

    uint8_t buffer[2] = { byte0, byte1 };
    esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, 0, /* report_id = */ 2,
                                           buffer, sizeof(buffer));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_input_set (media) failed: %d", err);
    }
}
```

`esp_hidd_dev_input_set` takes the 8 / 2 byte payload WITHOUT the Report ID prefix. The library prepends the Report ID itself when building the notification. This is the behavior we observed in the `esp_hid_device` example log: the consumer-control notification is `40 00` for a Volume Up, with no Report ID.

We keep `key1..key6` parameters and the modifier byte. The internal `keyboard_report_[]` 9-byte buffer (with Report ID prefix) is no longer needed; the modifier byte is part of the 8-byte payload we pass directly.

## D7 — Advertising restart on disconnect

`hidd_event_handler` already handles `ESP_HIDD_DISCONNECT_EVENT` and calls `esp_hid_ble_gap_adv_start()`. This satisfies R1.2.

## D8 — Bond store persistence

In `setup()`, after `esp_hid_gap_init`, we add:

```c
ble_store_config_init();
```

This wires the NimBLE bond store to the NVS partition. Without it, a reboot loses the LTK and the next connect fails with `BLE_SM_ERR_AUTHREQ` (status=7). With it, the next connect uses the persisted LTK and the pairing is automatic.

## D9 — `set_battery_level` no longer touches NimBLE directly

The new `set_battery_level` is also a one-liner:

```c
void Esp32BleKeyboard::set_battery_level(uint8_t level) {
    if (s_hid_dev == nullptr) return;
    esp_hidd_dev_battery_set(s_hid_dev, level);
}
```

The internal `battery_level_` member is kept so the example `yaml` continues to work and so we can publish the value back to Home Assistant if we want to.

## D10 — Vendoring `esp_hid_gap.c`

The `esp_hid` component header `esp_hid_gap.h` declares `esp_hid_gap_init`, `esp_hid_ble_gap_adv_init`, `esp_hid_ble_gap_adv_start`. The implementation lives in the example directory. We vendor it into our component:

```
components/ble_keyboard/src/esp_hid_gap.c   <- vendored from the example
```

And register it as a build source in `components/ble_keyboard/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "ble_keyboard.cpp" "src/esp_hid_gap.c"
                       INCLUDE_DIRS "."
                       REQUIRES "nimble" "esp_hid")
```

If ESPHome's `external_components` mechanism doesn't forward `CMakeLists.txt` correctly, the alternative is to copy the contents of `esp_hid_gap.c` into `ble_keyboard.cpp` as a single translation unit. We will try the `src/` subdirectory first and fall back to inlining if needed.

## D11 — YAML configuration

The example `examples/esp32c3.yaml` already has the sdkconfig options needed for `esp_hid`:

- `CONFIG_BT_NIMBLE_ENABLED: "y"`
- `CONFIG_BT_NIMBLE_HID_SERVICE: "y"`
- `CONFIG_BT_HID_DEVICE_ENABLED: "y"`

(Added in the preparatory commit.) No further changes to the YAML are required. The `ble_keyboard` component block, the Home Assistant `input_text` text sensor, the buttons, the number slider for battery level — all stay the same.

## D12 — Rejected alternatives

We considered and rejected:

- **Keep the current NimBLE implementation and only fix the `setValue + notify` flow manually**. The investigation showed that `ble_gatts_notify_custom` is the correct API; the problem is in the surrounding context. Hand-fixing it requires reproducing the layout, descriptor order, and protocol mode state machine that `esp_hid` already implements. Too much risk.
- **Switch to the Arduino framework**. Forbidden by R6.1.
- **Buy a BLE sniffer and continue diagnosing.** Would work, but costs $15–$20 in hardware and another session of analysis. The risk is that even with a sniffer we discover a bug in NimBLE that we cannot fix. The official path is `esp_hid`, which has its own context that already works.
- **Drop Boot Keyboard Input/Output support and just expose Report Protocol.** The Boot characteristics are useful for BIOS compatibility but not required for Windows 10/11. They can stay in the Report Map and `esp_hid` exposes them automatically.

## D13 — Test plan during the refactor

We will not commit intermediate broken states. The refactor happens in one go:

1. Replace `ble_keyboard.cpp` entirely.
2. Add `src/esp_hid_gap.c` if vendoring is needed.
3. Update `CMakeLists.txt` if needed.
4. Compile.
5. If the compile fails, fix the build, commit the result.
6. Once the build is green, run a sanity check on the ESP32-C3 with the user:
   - Pair, look for `Encryption change: status=0` and `Subscribe cur_notify=1`.
   - Press a button in Home Assistant, look for a character in Windows.
