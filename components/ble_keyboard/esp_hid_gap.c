/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Vendored from examples/bluetooth/esp_hid_device/main/esp_hid_gap.c in the
 * ESP-IDF examples tree, then trimmed to the NimBLE backend only.
 *
 * The original file has #if CONFIG_BT_NIMBLE_ENABLED / #elif CONFIG_BT_BLE_ENABLED
 * / #if !CONFIG_BT_NIMBLE_ENABLED branches that all need to coexist when the
 * example supports both transports. We only target NimBLE, so we keep just
 * the NimBLE-relevant functions and drop the Bluedroid branches entirely.
 *
 * Functions kept:
 *   - esp_hid_gap_init / esp_hid_gap_deinit (NimBLE controller init/deinit)
 *   - init_low_level / deinit_low_level (NimBLE-only)
 *   - esp_hid_ble_gap_adv_init (NimBLE)
 *   - esp_hid_ble_gap_adv_start (NimBLE)
 *   - nimble_hid_gap_event (NimBLE gap callback - called by esp_hid internally)
 *
 * The HID device event handler (ESP_HIDD_*_EVENT) is NOT defined here; the
 * ble_keyboard component registers its own via esp_event_handler_register.
 */

#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hid_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "nimble/ble.h"
#include "host/ble_sm.h"

static const char *TAG = "ESP_HID_GAP";

#define GATT_SVR_SVC_HID_UUID 0x1812

/* Forward declaration required by nimble_hid_gap_event. The real implementation
 * lives in the application's main.c / ble_keyboard.cpp; on NimBLE it is empty
 * for HID devices. We provide a stub so the linker is happy when esp_hid does
 * not call it on every code path. */
extern void ble_hid_task_start_up(void);
__attribute__((weak)) void ble_hid_task_start_up(void) {
    /* The component registers its own ESP_HIDD event handler that does the
     * equivalent work. This stub satisfies the linker for any code path
     * inside esp_hid that calls the weak symbol. */
}

static struct ble_hs_adv_fields fields;
static ble_uuid16_t s_hid_uuid16;

esp_err_t esp_hid_ble_gap_adv_init(uint16_t appearance, const char *device_name)
{
    memset(&fields, 0, sizeof fields);

    /* Flags: general discoverable + BLE-only (BR/EDR unsupported). */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* The example uses ESP_HID_APPEARANCE_GENERIC here, but the
     * ble_keyboard component passes ESP_HID_APPEARANCE_KEYBOARD through
     * the appearance argument. The stack only uses appearance for the
     * advertisement payload; the GATT service appearance is set by
     * esp_hid_dev_init. */
    fields.appearance = appearance;
    fields.appearance_is_present = 1;

    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    memset(&s_hid_uuid16, 0, sizeof(s_hid_uuid16));
    s_hid_uuid16.u.type = BLE_UUID_TYPE_16;
    s_hid_uuid16.value = GATT_SVR_SVC_HID_UUID;

    fields.uuids16 = &s_hid_uuid16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    /* Security parameters. These are the original example values:
     * sm_io_cap = DISP_ONLY, sm_mitm = 1, sm_sc = 1. The ble_keyboard
     * component overrides these from its own setup() (it uses
     * NO_IO + sm_sc = 0 for compatibility with Windows Just Works), so
     * values set here are placeholders that get overridden. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;

    return ESP_OK;
}

static int nimble_hid_gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "connection updated; status=%d",
                 event->conn_update.status);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertise complete; reason=%d",
                 event->adv_complete.reason);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe event; conn_handle=%d attr_handle=%d "
                 "reason=%d prevn=%d curn=%d previ=%d curi=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.reason,
                 event->subscribe.prev_notify,
                 event->subscribe.cur_notify,
                 event->subscribe.prev_indicate,
                 event->subscribe.cur_indicate);
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu update event; conn_handle=%d cid=%d mtu=%d",
                 event->mtu.conn_handle,
                 event->mtu.channel_id,
                 event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change event; status=%d ",
                 event->enc_change.status);
        rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (rc == 0) {
            ble_hid_task_start_up();
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        ESP_LOGI(TAG, "notify_tx event; conn_handle=%d attr_handle=%d "
                 "status=%d is_indication=%d",
                 event->notify_tx.conn_handle,
                 event->notify_tx.attr_handle,
                 event->notify_tx.status,
                 event->notify_tx.indication);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Throw away the old bond and accept the new link. */
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = {0};
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io (DISP) result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            pkey.action = event->passkey.params.action;
            pkey.numcmp_accept = 0;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io (NUMCMP) result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_OOB) {
            pkey.action = event->passkey.params.action;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io (OOB) result: %d", rc);
        } else if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            pkey.action = event->passkey.params.action;
            pkey.passkey = 123456;
            rc = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            ESP_LOGI(TAG, "ble_sm_inject_io (INPUT) result: %d", rc);
        }
        return 0;
    }
    }
    return 0;
}

esp_err_t esp_hid_ble_gap_adv_start(void)
{
    int rc;
    struct ble_gap_adv_params adv_params;
    /* Maximum possible duration for hid device (180s). */
    int32_t adv_duration_ms = 180000;
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;

    /* Resolve the BLE address before starting advertising.  The reference
     * implementation (olegos76/nimble_kbdhid_example) does this in its
     * sync_cb.  Without it ble_gap_adv_set_fields may return rc=4. */
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed; rc=%d", rc);
        return rc;
    }

    /* If advertising was previously started and is still active (or
     * preempted by WiFi coexistence), ble_gap_adv_set_fields returns
     * BLE_HS_EALREADY.  Force a stop first to clear the slave state. */
    ble_gap_adv_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "error setting advertisement data; rc=%d", rc);
        return rc;
    }
    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
    rc = ble_gap_adv_start(own_addr_type, NULL, adv_duration_ms,
                           &adv_params, nimble_hid_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "error enabling advertisement; rc=%d", rc);
        return rc;
    }
    return rc;
}

static esp_err_t init_low_level(uint8_t mode)
{
    esp_err_t ret;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
#if CONFIG_IDF_TARGET_ESP32
    bt_cfg.mode = mode;
#endif
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "esp_bt_controller_mem_release failed: %d", ret);
        return ret;
    }
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %d", ret);
        return ret;
    }

    ret = esp_bt_controller_enable(mode);
    if (ret) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %d", ret);
        return ret;
    }

    ret = esp_nimble_init();
    if (ret) {
        ESP_LOGE(TAG, "esp_nimble_init failed: %d", ret);
        return ret;
    }

    return ret;
}

static esp_err_t deinit_low_level(void)
{
    esp_err_t ret;

    ret = esp_nimble_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_deinit failed: %d", ret);
    }

    ret = esp_bt_controller_disable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_disable failed: %d", ret);
    }

    ret = esp_bt_controller_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_deinit failed: %d", ret);
    }

    return ESP_OK;
}

esp_err_t esp_hid_gap_deinit(void)
{
    esp_err_t ret;

    ret = deinit_low_level();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "deinit_low_level failed: %d", ret);
    }

    return ESP_OK;
}

esp_err_t esp_hid_gap_init(uint8_t mode)
{
    esp_err_t ret;
    if (!mode || mode > ESP_BT_MODE_BTDM) {
        ESP_LOGE(TAG, "Invalid mode given!");
        return ESP_FAIL;
    }

    ret = init_low_level(mode);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}
