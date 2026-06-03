/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Vendored from examples/bluetooth/esp_hid_device/main/esp_hid_gap.h in the
 * ESP-IDF examples tree. The matching .c implementation lives in
 * components/ble_keyboard/esp_hid_gap.c (also vendored).
 *
 * Reason: the `esp_hid` component shipped with ESP-IDF does NOT include
 * `esp_hid_gap.c`; only the example directory has it. We vendor the NimBLE
 * branch of that file so we can call esp_hid_gap_init / esp_hid_ble_gap_adv_*
 * without depending on a separate component.
 */

#ifndef _ESP_HID_GAP_H_
#define _ESP_HID_GAP_H_

#define HIDD_IDLE_MODE 0x00
#define HIDD_BLE_MODE 0x01
#define HIDD_BT_MODE 0x02
#define HIDD_BTDM_MODE 0x03

#if CONFIG_BT_HID_DEVICE_ENABLED
#if CONFIG_BT_BLE_ENABLED
#define HID_DEV_MODE HIDD_BTDM_MODE
#else
#define HID_DEV_MODE HIDD_BT_MODE
#endif
#elif CONFIG_BT_BLE_ENABLED
#define HID_DEV_MODE HIDD_BLE_MODE
#elif CONFIG_BT_NIMBLE_ENABLED
#define HID_DEV_MODE HIDD_BLE_MODE
#else
#define HID_DEV_MODE HIDD_IDLE_MODE
#endif

#include "esp_err.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "esp_hid_common.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_hid_gap_init(uint8_t mode);
esp_err_t esp_hid_gap_deinit(void);

esp_err_t esp_hid_ble_gap_adv_init(uint16_t appearance, const char *device_name);
esp_err_t esp_hid_ble_gap_adv_start(void);

#ifdef __cplusplus
}
#endif

#endif /* _ESP_HID_GAP_H_ */
