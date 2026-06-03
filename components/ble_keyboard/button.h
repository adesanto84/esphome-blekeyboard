#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "ble_keyboard.h"
#include <string>

namespace esphome {
namespace ble_keyboard {
class Esp32BleKeyboardButton : public button::Button, public Component {
 public:
  void set_parent(Esp32BleKeyboard *parent) { parent_ = parent; }

  void set_value(std::string value) {
    text_value_ = std::move(value);
    has_text_ = true;
  }
  void set_value(uint8_t first_value) {
    first_value_ = first_value;
    has_first_value_ = true;
  }
  void set_value(uint8_t first_value, uint8_t second_value) {
    first_value_ = first_value;
    second_value_ = second_value;
    has_first_value_ = true;
    has_second_value_ = true;
  }

 protected:
  void press_action() override;

  std::string text_value_;
  uint8_t first_value_{0};
  uint8_t second_value_{0};
  bool has_text_{false};
  bool has_first_value_{false};
  bool has_second_value_{false};

  Esp32BleKeyboard *parent_{nullptr};
};
}  // namespace ble_keyboard
}  // namespace esphome

#endif
