#ifdef USE_ESP32

#include "button.h"
#include "esphome/core/log.h"
#include <string>

namespace esphome {
namespace ble_keyboard {
static const char *const TAG = "ble_keyboard";

void Esp32BleKeyboardButton::press_action() {
  if (has_text_) {
    parent_->press(text_value_);
  } else if (has_second_value_) {
    MediaKeyReport mediaKey = {first_value_, second_value_};
    parent_->press(mediaKey);
  } else if (has_first_value_) {
    parent_->press(first_value_);
  } else {
    parent_->release();
  }
}
}  // namespace ble_keyboard
}  // namespace esphome

#endif
