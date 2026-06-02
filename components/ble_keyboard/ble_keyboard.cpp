#ifdef USE_ESP32

#include "ble_keyboard.h"
#include "esphome/core/log.h"

/* NimBLE host task helpers */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"      // ble_store_config_init (declared via host tree)
#include "esp_nimble_mem.h"      // nimble_platform_mem_* when BLE_STATIC_TO_DYNAMIC is on

/* esp_hid device library: provides esp_hidd_dev_init, esp_hidd_dev_input_set,
 * esp_hidd_dev_battery_set, etc. The NimBLE backend of esp_hid is wired by
 * the `nimble_hidd.c` source that lives inside the esp_hid component. */
#include "esp_hid_common.h"
#include "esp_hidd.h"

/* esp_hid_gap_init / esp_hid_ble_gap_adv_init / _start live in the example
 * directory of ESP-IDF, not in the esp_hid component itself. We vendor the
 * NimBLE-only branch of that file in this component as esp_hid_gap.c. */
#include "esp_hid_gap.h"

namespace esphome {
namespace ble_keyboard {

static const char *const TAG = "ble_keyboard";

/* --- HID Report Map (verbatim from the previous hand-rolled implementation).
 * Report ID 1 = keyboard, Report ID 2 = consumer control. --- */
static const uint8_t kHidReportMap[] = {
    // Keyboard (Report ID 1)
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08,
    0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
    0x25, 0xFF, 0x05, 0x07, 0x19, 0x00,
    0x29, 0xFF, 0x81, 0x00,
    0xC0,
    // Consumer Control (Report ID 2)
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x85, 0x02,
    0x19, 0x00, 0x2A, 0x3C, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x03,
    0x95, 0x01, 0x75, 0x10, 0x81, 0x00,
    0xC0,
};

/* --- Module state --- */
static bool g_connected = false;
static uint8_t keyboard_output_report_[1] = {0};
static char g_device_name[32] = "BLE Keyboard";
static char g_manufacturer_id[32] = "ESPHome";

/* Handle to the esp_hid device. Populated by esp_hidd_dev_init. */
static esp_hidd_dev_t *s_hid_dev = NULL;

/* Startup delay counter. Set by ESP_HIDD_START_EVENT; decremented in
 * update(). Advertising starts after 2s to let the controller finish
 * WiFi scan and free the HCI for BLE advertising. */
static int s_advertising_startup_delay = 0;

/* `ble_store_config_init` lives in NimBLE's store/config source. The header
 * tree does not export it, so we forward-declare it here, mirroring the
 * approach used by the official esp_hid_device example. */
extern "C" void ble_store_config_init(void);

/* --- Host task --- */
extern "C" void nimble_host_task(void *param) {
  ESP_LOGI(TAG, "NimBLE host task started");
  nimble_port_run();
  nimble_port_freertos_deinit();
}

/* --- HID device event handler --- */
static void hidd_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data) {
  esp_hidd_event_t event = (esp_hidd_event_t) id;
  esp_hidd_event_data_t *param = (esp_hidd_event_data_t *) event_data;

  switch (event) {
    case ESP_HIDD_START_EVENT:
      // Defer advertising start by 2s so the controller finishes WiFi scan
      // and the HCI is free for BLE advertising.  PollingComponent::update()
      // counts down and fires esp_hid_ble_gap_adv_start().
      ESP_LOGI(TAG, "HID device stack started; advertising will begin in 2s");
      s_advertising_startup_delay = 2;  // 2 x update_interval (1000ms)
      break;
    case ESP_HIDD_CONNECT_EVENT:
      g_connected = true;
      ESP_LOGI(TAG, "HID device connected");
      break;
    case ESP_HIDD_DISCONNECT_EVENT:
      g_connected = false;
      ESP_LOGI(TAG, "HID device disconnected; reason=%d", param->disconnect.reason);
      // Re-start advertising so a fresh connection can be made without a power cycle.
      // The host is by now fully up, so this call is safe.
      esp_hid_ble_gap_adv_start();
      break;
    case ESP_HIDD_OUTPUT_EVENT:
      // Host wrote the LED state to the Output Report (Caps/Num/Scroll Lock).
      if (param->output.length > 0 && param->output.data != nullptr) {
        memcpy(keyboard_output_report_, param->output.data,
               param->output.length < sizeof(keyboard_output_report_)
                   ? param->output.length
                   : sizeof(keyboard_output_report_));
        ESP_LOGI(TAG, "Keyboard LED output: 0x%02X", keyboard_output_report_[0]);
      }
      break;
    default:
      break;
  }
}

/* --- class methods --- */

void Esp32BleKeyboard::setup() {
  ESP_LOGI(TAG, "Setting up BLE Keyboard (esp_hid device API)");

  strncpy(g_device_name, name_.c_str(), sizeof(g_device_name) - 1);
  g_device_name[sizeof(g_device_name) - 1] = '\0';
  strncpy(g_manufacturer_id, manufacturer_id_.c_str(), sizeof(g_manufacturer_id) - 1);
  g_manufacturer_id[sizeof(g_manufacturer_id) - 1] = '\0';

  // Step 1: initialize the NimBLE host (controller + nimble_port + controller enable).
  esp_err_t err = esp_hid_gap_init(ESP_HID_TRANSPORT_BLE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_hid_gap_init failed: %d", err);
    return;
  }

  // Step 2: set the appearance (HID Keyboard) and the device name in the
  // advertisement fields. This populates the static `fields` struct inside
  // esp_hid_gap.c, which is later used by esp_hid_ble_gap_adv_start.
  err = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD, g_device_name);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_hid_ble_gap_adv_init failed: %d", err);
    return;
  }

  // Override the security parameters that esp_hid_ble_gap_adv_init set.
  // The example uses sm_io_cap = DISP_ONLY + sm_sc = 1, which Windows
  // rejects with status=7 (BLE_SM_ERR_AUTHREQ) on a no-IO keyboard. Switch
  // to LE Legacy Just Works so Windows + NimBLE agree.
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

  // Step 3: build the report maps and the device config.
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

  // Step 4: register the HID device. After this, s_hid_dev is non-NULL and we
  // can send input reports.
  err = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hidd_event_handler, &s_hid_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_hidd_dev_init failed: %d", err);
    return;
  }

  // Step 5: initial battery level, until Home Assistant overrides it.
  esp_hidd_dev_battery_set(s_hid_dev, battery_level_);

  // Step 6: enable NimBLE's bond store backed by NVS. Without this, a reboot
  // loses the LTK and the next connect hits the AUTHREQ loop.
  ble_store_config_init();
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // Step 7: start the NimBLE host task. esp_hid_gap_init() does NOT call
  // esp_nimble_enable() for us (it only initialises the controller and the
  // NimBLE port). The host task runs nimble_port_run(); once it finishes the
  // host-controller sync, esp_hid posts ESP_HIDD_START_EVENT, and our
  // hidd_event_handler() calls esp_hid_ble_gap_adv_start() from there.
  //
  // We MUST NOT call esp_hid_ble_gap_adv_start() here: ble_gap_adv_set_fields
  // returns BLE_HS_EDISABLED (rc=30) when the host has not yet been enabled.
  err = esp_nimble_enable(reinterpret_cast<void *>(nimble_host_task));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_nimble_enable failed: %d", err);
    return;
  }
  ESP_LOGI(TAG, "NimBLE host task started; advertising will begin on host sync");
}

void Esp32BleKeyboard::update() {
  if (state_sensor_ != nullptr) {
    state_sensor_->publish_state(g_connected);
  }
  if (s_advertising_startup_delay > 0) {
    s_advertising_startup_delay--;
    if (s_advertising_startup_delay == 0) {
      ESP_LOGI(TAG, "Advertising startup delay elapsed; starting now");
      esp_hid_ble_gap_adv_start();
    }
  }
}

void Esp32BleKeyboard::set_battery_level(uint8_t level) {
  battery_level_ = level;
  if (s_hid_dev != nullptr) {
    esp_hidd_dev_battery_set(s_hid_dev, level);
  }
}

void Esp32BleKeyboard::send_keyboard_report(uint8_t modifiers, uint8_t key1, uint8_t key2,
                                             uint8_t key3, uint8_t key4, uint8_t key5, uint8_t key6) {
  if (s_hid_dev == nullptr) {
    return;
  }

  // 8-byte keyboard input report: [modifiers, reserved, key1..key6].
  // esp_hidd_dev_input_set takes the payload WITHOUT the Report ID prefix
  // (the library prepends it on the wire).
  uint8_t buffer[8] = { modifiers, 0, key1, key2, key3, key4, key5, key6 };
  esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, /* map_index = */ 0,
                                          /* report_id = */ 1,
                                          buffer, sizeof(buffer));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_hidd_dev_input_set (keyboard) failed: %d", err);
  }
}

void Esp32BleKeyboard::send_media_report(uint8_t byte0, uint8_t byte1) {
  if (s_hid_dev == nullptr) {
    return;
  }

  // 2-byte consumer-control input report. No Report ID prefix here either.
  uint8_t buffer[2] = { byte0, byte1 };
  esp_err_t err = esp_hidd_dev_input_set(s_hid_dev, /* map_index = */ 0,
                                          /* report_id = */ 2,
                                          buffer, sizeof(buffer));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_hidd_dev_input_set (media) failed: %d", err);
  }
}

/* --- ASCII -> HID keycode lookup --- */
struct HidKey {
  uint8_t modifier;
  uint8_t key;
};

static HidKey ascii_to_hid(char c) {
  if (c >= 'a' && c <= 'z') return {0, (uint8_t)(0x04 + (c - 'a'))};
  if (c >= 'A' && c <= 'Z') return {0x02, (uint8_t)(0x04 + (c - 'A'))};
  if (c >= '1' && c <= '9') return {0, (uint8_t)(0x1E + (c - '1'))};
  if (c == '0') return {0, 0x27};
  if (c == '\n') return {0, 0x28};
  if (c == '\r') return {0, 0x28};
  if (c == '\x1B') return {0, 0x29};  // Escape
  if (c == '\b') return {0, 0x2A};   // Backspace
  if (c == '\t') return {0, 0x2B};
  if (c == ' ') return {0, 0x2C};
  if (c == '-') return {0, 0x2D};
  if (c == '_') return {0x02, 0x2D};
  if (c == '=') return {0, 0x2E};
  if (c == '+') return {0x02, 0x2E};
  if (c == '[') return {0, 0x2F};
  if (c == '{') return {0x02, 0x2F};
  if (c == ']') return {0, 0x30};
  if (c == '}') return {0x02, 0x30};
  if (c == '\\') return {0, 0x31};
  if (c == '|') return {0x02, 0x31};
  if (c == ';') return {0, 0x33};
  if (c == ':') return {0x02, 0x33};
  if (c == '\'') return {0, 0x34};
  if (c == '"') return {0x02, 0x34};
  if (c == '`') return {0, 0x35};
  if (c == '~') return {0x02, 0x35};
  if (c == ',') return {0, 0x36};
  if (c == '<') return {0x02, 0x36};
  if (c == '.') return {0, 0x37};
  if (c == '>') return {0x02, 0x37};
  if (c == '/') return {0, 0x38};
  if (c == '?') return {0x02, 0x38};
  if (c == '!') return {0x02, 0x1E};
  if (c == '@') return {0x02, 0x1F};
  if (c == '#') return {0x02, 0x20};
  if (c == '$') return {0x02, 0x21};
  if (c == '%') return {0x02, 0x22};
  if (c == '^') return {0x02, 0x23};
  if (c == '&') return {0x02, 0x24};
  if (c == '*') return {0x02, 0x25};
  if (c == '(') return {0x02, 0x26};
  if (c == ')') return {0x02, 0x27};
  return {0, 0};  // unsupported
}

void Esp32BleKeyboard::press(std::string message) {
  if (!g_connected) {
    ESP_LOGW(TAG, "Not connected, cannot print");
    return;
  }
  for (size_t i = 0; i < message.length(); ++i) {
    HidKey hk = ascii_to_hid(message[i]);
    if (hk.key == 0) {
      ESP_LOGW(TAG, "Unsupported character: 0x%02X", (unsigned char)message[i]);
      continue;
    }
    send_keyboard_report(hk.modifier, hk.key);
    delay(default_delay_);
    send_keyboard_report(0, 0);  // release
    delay(default_delay_);
  }
}

/* --- Modifier dispatch helper --- */
static bool is_modifier(uint8_t key) {
  switch (key) {
    case 0x01: case 0x02: case 0x04: case 0x08:
    case 0x10: case 0x20: case 0x40: case 0x80:
      return true;
    default:
      return false;
  }
}

void Esp32BleKeyboard::press(uint8_t key, bool with_timer) {
  if (!g_connected) {
    ESP_LOGW(TAG, "Not connected, cannot press key");
    return;
  }
  if (with_timer) {
    update_timer();
  }
  if (is_modifier(key)) {
    send_keyboard_report(key, 0);
  } else {
    send_keyboard_report(0, key);
  }
}

void Esp32BleKeyboard::press(MediaKeyReport key, bool with_timer) {
  if (!g_connected) {
    ESP_LOGW(TAG, "Not connected, cannot press media key");
    return;
  }
  if (with_timer) {
    update_timer();
  }
  send_media_report(key[0], key[1]);
}

void Esp32BleKeyboard::release() {
  if (!g_connected) {
    return;
  }
  cancel_timeout(TAG);
  send_keyboard_report(0, 0);
  send_media_report(0, 0);
}

void Esp32BleKeyboard::start() {
  if (reconnect_) {
    esp_hid_ble_gap_adv_start();
  }
}

void Esp32BleKeyboard::stop() {
  // esp_hid does not expose a public "stop advertising" function on the
  // NimBLE backend (it uses raw ble_gap_adv_set_fields / ble_gap_adv_start
  // from inside esp_hid_gap.c). The closest we can do is call
  // esp_hid_gap_deinit/esp_hid_ble_gap_adv_init again to re-arm, but that
  // tears down the whole stack. For a "stop" that just pauses advertising,
  // we use ble_gap_adv_stop() directly - it's a NimBLE host API and the
  // ble_hs header is in our include path.
  ble_gap_adv_stop();
}

bool Esp32BleKeyboard::is_connected() {
  return g_connected;
}

void Esp32BleKeyboard::update_timer() {
  cancel_timeout(TAG);
  set_timeout(TAG, release_delay_, [this]() { this->release(); });
}

}  // namespace ble_keyboard
}  // namespace esphome

#endif  // USE_ESP32
