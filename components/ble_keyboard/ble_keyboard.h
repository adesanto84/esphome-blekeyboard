#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include <string>
#include <vector>
#include <queue>

namespace esphome {
namespace ble_keyboard {

typedef uint8_t MediaKeyReport[2];

class Esp32BleKeyboard : public PollingComponent {
 public:
  Esp32BleKeyboard(std::string name, std::string manufacturer_id, uint8_t battery_level, bool reconnect)
      : PollingComponent(1000) {
    name_ = name;
    manufacturer_id_ = manufacturer_id;
    battery_level_ = battery_level;
    reconnect_ = reconnect;
  }

  void setup() override;
  void update() override;

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void set_delay(uint32_t delay_ms = 8) { default_delay_ = delay_ms; }
  void set_release_delay(uint32_t delay_ms = 8) { release_delay_ = delay_ms; }
  void set_battery_level(uint8_t level = 100);

  void set_state_sensor(binary_sensor::BinarySensor *state_sensor) { state_sensor_ = state_sensor; }

  void press(std::string message);
  void press(uint8_t key, bool with_timer = true);
  void press(MediaKeyReport key, bool with_timer = true);
  void press_combination(const std::vector<std::string> &keys, uint32_t hold_ms);
  void release();

  void start();
  void stop();

  bool is_connected();

 protected:
  binary_sensor::BinarySensor *state_sensor_{nullptr};

 private:
  void update_timer();
  void send_keyboard_report(uint8_t modifiers, uint8_t key1 = 0, uint8_t key2 = 0, uint8_t key3 = 0,
                            uint8_t key4 = 0, uint8_t key5 = 0, uint8_t key6 = 0);
  void send_media_report(uint8_t byte0, uint8_t byte1);
  void process_next_print_char();

  std::string name_;
  std::string manufacturer_id_;
  uint8_t battery_level_{100};
  bool reconnect_{true};
  uint32_t default_delay_{100};
  uint32_t release_delay_{50};
  std::string pending_text_;
  size_t text_index_{0};
};

}  // namespace ble_keyboard
}  // namespace esphome

#endif
