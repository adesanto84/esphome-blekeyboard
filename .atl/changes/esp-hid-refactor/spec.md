# Specification: esp_hid refactor of ble_keyboard component

This spec captures the OBSERVABLE behavior of the refactored component. The point of the spec is to be testable: each requirement ends with `## verify` describing how a human (or an automated test) confirms the behavior holds.

## R1 — Advertising

### R1.1 Device advertises as a keyboard

After boot, the device must broadcast a connectable advertisement.

- Appearance = `0x03C1` (HID Keyboard).
- Service UUID `0x1812` (HID) is included in the advertisement data.
- Device name is `ControlPadOffice` (or whatever the user configured in YAML).

#### verify
- From the serial log: `Advertising started with name='ControlPadOffice'` appears within 2 s of boot.
- From nRF Connect on Android: scanning lists `ControlPadOffice` with a `HID` service UUID in the advertisement.

### R1.2 Reconnection re-runs the advertiser

When the host disconnects, the device must start advertising again so a fresh connection can be made without a power-cycle.

#### verify
- Pair the device with Windows.
- Disconnect from the Windows Bluetooth panel.
- Within 5 s, the device must reappear as a connectable device (no need to re-flash).

## R2 — Pairing and bonding

### R2.1 First-time pairing succeeds

A device that has never been paired must complete a full LE Legacy Just Works pairing within 5 s of the host requesting it.

- Security manager: `sm_bonding=1`, `sm_mitm=0`, `sm_sc=0`, `sm_io_cap=NO_IO`, `sm_our_key_dist=ENC`, `sm_their_key_dist=ENC`.
- The pairing must NOT loop with `Encryption change: status=7` (BLE_SM_ERR_AUTHREQ) on a freshly erased device.

#### verify
- Erase flash with `esptool.py --chip esp32c3 --port COM6 erase_flash`.
- Flash and let boot settle.
- In Windows, add the device. Confirm the serial log shows `Encryption change: status=0` exactly once and no `Encryption failed` warning.

### R2.2 Reconnection uses the stored bond

After a power-cycle with the bond intact, reconnecting the device to Windows must NOT require the user to remove and re-pair the device.

- No `Encryption change: status=7` after the first successful pairing.
- Windows does not need to re-prompt the user for a passkey.

#### verify
- After a successful first pairing, reboot the ESP32 with `esptool.py --chip esp32c3 --port COM6 --before default_reset --after hard_reset run` (or just `Restart` from the ESPHome web UI).
- The serial log must show a clean `Encryption change: status=0` on the next connection.
- The Windows Bluetooth panel must show the device as connected without prompting.

## R3 — GATT database

### R3.1 Required services are present

After connection, the central must be able to discover, at minimum, these primary services:

- `0x1800` Generic Access
- `0x1801` Generic Attribute
- `0x180A` Device Information
- `0x180F` Battery Service
- `0x1812` Human Interface Device

#### verify
- Run `nRF Connect` on Android. Connect to the device. The "GATT" tab must show all five services.

### R3.2 HID Information characteristic returns a valid descriptor

Reading the `0x2A4A` characteristic must return a 4-byte value with `bcdHID = 0x0111` and flags byte = `0x02` (NormallyConnectable).

#### verify
- In nRF Connect, read the `0x2A4A` characteristic under the HID service. The value must be `11 01 00 02` (little-endian 0x0111, country 0, flags 0x02).

### R3.3 HID Report Map is readable

Reading the `0x2A4B` characteristic must return a non-empty Report Map that declares at least:

- A keyboard Input report (Usage Page 0x01, Usage 0x06, Report ID 1).
- A consumer-control Input report (Usage Page 0x0C, Usage 0x01, Report ID 2).

#### verify
- In nRF Connect, read the `0x2A4B` characteristic. The value must be the byte sequence defined in `kHidReportMap` in the component (the exact bytes are in the source).

### R3.4 Battery characteristic is readable and writable

- Reading the `0x2A19` (Battery Level) characteristic returns a value between 0 and 100.
- Writing the value from Home Assistant updates what Windows shows.

#### verify
- In nRF Connect, read the `0x2A19` characteristic. The initial value must be 100.
- In Home Assistant, move the "Battery level" slider to 50. In Windows, the device's battery reading in the Bluetooth panel must change to 50 (it can take a few seconds).

## R4 — Notifications (the broken path)

### R4.1 Windows subscribes to the keyboard input report

After GATT discovery completes, the host must write `0x0001` to the Client Characteristic Configuration descriptor (CCCD, 0x2902) of the keyboard Input Report (UUID 0x2A4D, Report ID 1).

#### verify
- After Windows pairs, the serial log must show `Subscribe cur_notify=1 conn_handle=1 attr_handle=<some handle>`. The `attr_handle` value is the handle that the component will later use to send notifications.

### R4.2 A keystroke sent from Home Assistant reaches Windows

The Home Assistant buttons in the example YAML map to `press(...)` calls. These calls must result in the corresponding HID keystroke being applied on the Windows side.

#### verify
- Open a text editor (Notepad) in Windows and focus the text field.
- In Home Assistant, press the "Space" button (or any other configured key).
- The character must appear in the focused text field.

This is the primary success criterion. Everything else in this spec exists to make this work.

### R4.3 LED output remains functional

Windows sends a byte to the `0x2A4D` Output Report (Report ID 1) when the user toggles Caps Lock, Num Lock, or Scroll Lock. The component must accept that byte without error.

#### verify
- The serial log must show `GATT access: uuid=0x2A4D op=1 ... Keyboard LED output: 0xXX` when the user presses Caps Lock or Num Lock on the physical keyboard while the focus is in any Windows text field.
- This is a regression check: it confirms the GATT write path is intact.

## R5 — Public API of the component (regression)

The refactor must not break the public C++ API that ESPHome uses:

- `Esp32BleKeyboard::setup()`
- `Esp32BleKeyboard::set_name(...)`
- `Esp32BleKeyboard::set_manufacturer_id(...)`
- `Esp32BleKeyboard::set_battery_level(uint8_t)`
- `Esp32BleKeyboard::press(std::string)`
- `Esp32BleKeyboard::press(uint8_t key, bool with_timer = true)`
- `Esp32BleKeyboard::press(MediaKeyReport key, bool with_timer = true)`
- `Esp32BleKeyboard::release()`
- `Esp32BleKeyboard::start()` (start advertising)
- `Esp32BleKeyboard::stop()` (stop advertising)
- `Esp32BleKeyboard::is_connected()`
- `Esp32BleKeyboard::update()` (called by ESPHome loop)

The Python helper module in `components/ble_keyboard/__init__.py` and the action handlers in `automation.h` must continue to compile and link.

#### verify
- The project compiles without errors after the refactor.
- The same `examples/esp32c3.yaml` file is still valid and produces a working binary.
- No header file (`ble_keyboard.h`) public signature is removed.

## R6 — Constraints

### R6.1 The framework stays ESP-IDF

The component must keep working on `framework: type: esp-idf` in the YAML. Switching to `type: arduino` is explicitly out of scope.

### R6.2 No manual GATT table definitions

After the refactor, the component must NOT define its own `struct ble_gatt_svc_def` array for the HID service, must NOT register characteristics with `ble_gatts_add_svcs`, and must NOT call `ble_gatts_notify_custom` for the input reports. All of those responsibilities move to `esp_hid`.

(Boilerplate for the Battery service, Device Information, and Generic Access can still be done with the standard NimBLE service inits like `ble_svc_bas_init`, `ble_svc_dis_init`, `ble_svc_gap_init`, but the HID service must come from `esp_hid`.)

### R6.3 Connection parameter negotiation stays responsive

Windows will request a connection parameter update (`BLE_GAP_EVENT_CONN_UPDATE_REQ`). The component must accept it (return 0 from the gap event handler) so that Windows can shorten the connection interval to ~30 ms.
