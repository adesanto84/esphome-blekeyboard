# CMakeLists.txt for the ble_keyboard component.
#
# This file is what makes the component usable from a plain PlatformIO +
# ESP-IDF project (i.e. outside the ESPHome external-components machinery).
# The ESPHome build copies this whole directory under
# `src/esphome/components/ble_keyboard/` and globs `*.cpp` and `*.c` from
# it, so this file is harmless for ESPHome. For a standalone PlatformIO
# project (e.g. the user's `controlpadoffice` env on Windows) this file
# is what tells ESP-IDF to actually compile `ble_keyboard.cpp` and the
# vendored `esp_hid_gap.c`, and to pull in the `nimble` and `esp_hid`
# components as dependencies.
#
# The Kconfig.projbuild in the same directory auto-selects
# `BT_NIMBLE_HID_SERVICE`, so the linker does not fail with
# "undefined reference to `esp_ble_hidd_dev_init'".

idf_component_register(
    SRCS
        "ble_keyboard.cpp"
        "button.cpp"
        "esp_hid_gap.c"
        "number.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        "esp_event"
        "esp_hid"
        "nimble"
    PRIV_REQUIRES
        "bt"
)
