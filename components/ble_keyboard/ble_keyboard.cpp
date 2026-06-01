#ifdef USE_ESP32

#include "ble_keyboard.h"
#include "esphome/core/log.h"

/* NimBLE native headers from ESP-IDF 5.x */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/bas/ble_svc_bas.h"
#include "services/dis/ble_svc_dis.h"

namespace esphome {
namespace ble_keyboard {

static const char *const TAG = "ble_keyboard";

/* --- HID Report Map with Report IDs: 1=keyboard, 2=consumer control --- */
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

static uint8_t keyboard_report_[9];  // [Report ID=1, mods, reserved, k1..k6]
static uint8_t media_report_[3];    // [Report ID=2, byte0, byte1]
static uint8_t hid_info_[4] = {0x11, 0x01, 0x00, 0x02};  // bcdHID=0x0111, bCountryCode=0, Flags=RemoteWake(0x02)
static uint8_t protocol_mode_ = 1;  // Report Protocol (0 = Boot, 1 = Report) per HID spec

/* --- UUID constants --- */
static const ble_uuid16_t UUID_HID_SERVICE      = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t UUID_HID_INFORMATION  = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t UUID_HID_CONTROL_PT   = BLE_UUID16_INIT(0x2A4C);
static const ble_uuid16_t UUID_HID_REPORT_MAP   = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t UUID_HID_PROTOCOL     = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t UUID_HID_REPORT       = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t UUID_HID_BOOT_INPUT   = BLE_UUID16_INIT(0x2A22);
static const ble_uuid16_t UUID_HID_BOOT_OUTPUT  = BLE_UUID16_INIT(0x2A32);
static const ble_uuid16_t UUID_REPORT_REF       = BLE_UUID16_INIT(0x2908);
static const ble_uuid16_t UUID_EXT_REPORT_REF   = BLE_UUID16_INIT(0x2907);
static const ble_uuid16_t UUID_BATTERY_SERVICE  = BLE_UUID16_INIT(0x180F);

/* --- Report reference data: {Report ID, Report Type} --- */
static const uint8_t report_ref_keyboard[] = {0x01, 0x01};  // Input
static const uint8_t report_ref_media[]    = {0x02, 0x01};  // Input
static const uint8_t report_ref_output[]   = {0x01, 0x02};  // Output

/* --- shared state --- */
static bool g_connected = false;
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_handle_keyboard = 0;
static uint16_t g_handle_media = 0;
static uint16_t g_handle_boot_input = 0;
static uint16_t g_handle_output = 0;
static uint8_t keyboard_output_report_[1] = {0};
static uint8_t boot_key_report_[8] = {0};
static uint8_t boot_output_leds_ = 0;
static char g_device_name[32] = "BLE Keyboard";
static char g_manufacturer_id[32] = "ESPHome";

/* --- access callback (free function so C GATT tables can reference it) --- */
static int ble_keyboard_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
  uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);
  int rc = 0;

  ESP_LOGI(TAG, "GATT access: uuid=0x%04X op=%d arg=%p handle=%d",
           uuid16, ctxt->op, arg, attr_handle);

  switch (uuid16) {
    case 0x2A4A:
      rc = os_mbuf_append(ctxt->om, hid_info_, sizeof(hid_info_));
      break;
    case 0x2A4C:
      if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t suspend;
        rc = ble_hs_mbuf_to_flat(ctxt->om, &suspend, 1, NULL);
        ESP_LOGD(TAG, "HID Control Point: suspend=%d", suspend);
      }
      break;
    case 0x2A4B:
      rc = os_mbuf_append(ctxt->om, kHidReportMap, sizeof(kHidReportMap));
      break;
    case 0x2A4E:
      if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        rc = os_mbuf_append(ctxt->om, &protocol_mode_, 1);
      } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        rc = ble_hs_mbuf_to_flat(ctxt->om, &protocol_mode_, 1, NULL);
      }
      break;
    case 0x2A22:  // Boot Keyboard Input Report
      if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        rc = os_mbuf_append(ctxt->om, boot_key_report_, sizeof(boot_key_report_));
      }
      break;
    case 0x2A32:  // Boot Keyboard Output Report (LEDs)
      if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        rc = ble_hs_mbuf_to_flat(ctxt->om, &boot_output_leds_, 1, NULL);
        ESP_LOGD(TAG, "Boot Output LED state=0x%02X", boot_output_leds_);
      }
      break;
    case 0x2A4D:  // HID Report
      if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr_handle == g_handle_keyboard) {
          rc = os_mbuf_append(ctxt->om, keyboard_report_, sizeof(keyboard_report_));
          ESP_LOGD(TAG, "Read keyboard report OK");
        } else if (attr_handle == g_handle_media) {
          rc = os_mbuf_append(ctxt->om, media_report_, sizeof(media_report_));
          ESP_LOGD(TAG, "Read media report OK");
        } else if (attr_handle == g_handle_output) {
          rc = os_mbuf_append(ctxt->om, keyboard_output_report_, sizeof(keyboard_output_report_));
          ESP_LOGD(TAG, "Read output report OK");
        } else {
          ESP_LOGW(TAG, "Read HID Report on unknown handle=%d (key=%d med=%d out=%d)",
                   attr_handle, g_handle_keyboard, g_handle_media, g_handle_output);
          rc = BLE_ATT_ERR_ATTR_NOT_FOUND;
        }
      } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr_handle == g_handle_output) {
          rc = ble_hs_mbuf_to_flat(ctxt->om, keyboard_output_report_, sizeof(keyboard_output_report_), NULL);
          ESP_LOGD(TAG, "Keyboard LED output: 0x%02X", keyboard_output_report_[0]);
        } else {
          ESP_LOGW(TAG, "Write HID Report on unknown handle=%d", attr_handle);
          rc = BLE_ATT_ERR_ATTR_NOT_FOUND;
        }
      }
      break;
    default:
      ESP_LOGW(TAG, "GATT access: unhandled uuid=0x%04X", uuid16);
      rc = BLE_ATT_ERR_UNLIKELY;
      break;
  }
  if (rc != 0) {
    ESP_LOGW(TAG, "GATT access returning rc=%d", rc);
  }
  return rc;
}

/* --- GATT service definition --- */
static int report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
  const uint8_t *data = (const uint8_t *)arg;
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
    return os_mbuf_append(ctxt->om, data, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

/* External Report Reference: points to Battery Service (UUID 0x180F) */
static uint8_t ext_report_ref_battery[2] = {0x0F, 0x18};  // UUID 0x180F little-endian
static int ext_report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg) {
  (void)conn_handle;
  (void)attr_handle;
  (void)arg;
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
    return os_mbuf_append(ctxt->om, ext_report_ref_battery, 2) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_dsc_def report_map_dscs[] = {
  {
    .uuid = (const ble_uuid_t *)&UUID_EXT_REPORT_REF,
    .att_flags = BLE_ATT_F_READ,
    .min_key_size = 0,
    .access_cb = ext_report_ref_access,
    .arg = nullptr,
  },
  { 0 },
};

static struct ble_gatt_dsc_def keyboard_report_dscs[] = {
  {
    .uuid = (const ble_uuid_t *)&UUID_REPORT_REF,
    .att_flags = BLE_ATT_F_READ,
    .min_key_size = 0,
    .access_cb = report_ref_access,
    .arg = (void *)report_ref_keyboard,
  },
  { 0 },
};

static struct ble_gatt_dsc_def media_report_dscs[] = {
  {
    .uuid = (const ble_uuid_t *)&UUID_REPORT_REF,
    .att_flags = BLE_ATT_F_READ,
    .min_key_size = 0,
    .access_cb = report_ref_access,
    .arg = (void *)report_ref_media,
  },
  { 0 },
};

static struct ble_gatt_dsc_def output_report_dscs[] = {
  {
    .uuid = (const ble_uuid_t *)&UUID_REPORT_REF,
    .att_flags = BLE_ATT_F_READ,
    .min_key_size = 0,
    .access_cb = report_ref_access,
    .arg = (void *)report_ref_output,
  },
  { 0 },
};

static struct ble_gatt_chr_def hid_chrs[] = {
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_INFORMATION,
    .access_cb =   ble_keyboard_access,
    .arg =         nullptr,
    .descriptors = nullptr,
    .flags =       BLE_GATT_CHR_F_READ,
    .min_key_size = 0,
    .val_handle =  nullptr,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_CONTROL_PT,
    .access_cb =   ble_keyboard_access,
    .arg =         nullptr,
    .descriptors = nullptr,
    .flags =       BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
    .min_key_size = 0,
    .val_handle =  nullptr,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_REPORT_MAP,
    .access_cb =   ble_keyboard_access,
    .arg =         nullptr,
    .descriptors = report_map_dscs,
    .flags =       BLE_GATT_CHR_F_READ,
    .min_key_size = 0,
    .val_handle =  nullptr,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_PROTOCOL,
    .access_cb =   ble_keyboard_access,
    .arg =         nullptr,
    .descriptors = nullptr,
    .flags =       BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    .min_key_size = 0,
    .val_handle =  nullptr,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_BOOT_INPUT,
    .access_cb =   ble_keyboard_access,
    .arg =         nullptr,
    .descriptors = nullptr,
    .flags =       BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    .min_key_size = 0,
    .val_handle =  &g_handle_boot_input,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_BOOT_OUTPUT,
    .access_cb =   ble_keyboard_access,
    .arg =         nullptr,
    .descriptors = nullptr,
    .flags =       BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    .min_key_size = 0,
    .val_handle =  nullptr,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_REPORT,
    .access_cb =   ble_keyboard_access,
    .arg =         (void *)1,
    .descriptors = keyboard_report_dscs,
    .flags =       BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE,
    .min_key_size = 0,
    .val_handle =  &g_handle_keyboard,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_REPORT,
    .access_cb =   ble_keyboard_access,
    .arg =         (void *)2,
    .descriptors = media_report_dscs,
    .flags =       BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_WRITE,
    .min_key_size = 0,
    .val_handle =  &g_handle_media,
  },
  {
    .uuid =        (const ble_uuid_t *)&UUID_HID_REPORT,
    .access_cb =   ble_keyboard_access,
    .arg =         (void *)3,
    .descriptors = output_report_dscs,
    .flags =       BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    .min_key_size = 0,
    .val_handle =  &g_handle_output,
  },
  { 0 },
};

static struct ble_gatt_svc_def gatt_services[] = {
  {
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = (const ble_uuid_t *)&UUID_HID_SERVICE,
    .characteristics = hid_chrs,
  },
  { 0 },
};

/* Forward declaration */
static void start_advertising(const char *name);

/* --- GAP events --- */
static int gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        g_connected = true;
        g_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "Connected (conn_handle=%d)", g_conn_handle);
        // Do NOT call ble_gap_security_initiate() here — Windows initiates
        // pairing on its own, and forcing a parallel security request
        // confuses the SM state machine.
      } else {
        ESP_LOGI(TAG, "Connection failed, status=%d", event->connect.status);
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      g_connected = false;
      g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
      ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
      // Restart advertising so the central can reconnect. Without this,
      // the device stays in 'paired' state in Windows but invisible to
      // new connections, forcing the user to remove and re-pair.
      start_advertising(g_device_name);
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      ESP_LOGI(TAG, "Subscribe cur_notify=%d conn_handle=%d attr_handle=%d",
               event->subscribe.cur_notify, event->subscribe.conn_handle,
               event->subscribe.attr_handle);
      break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      ESP_LOGI(TAG, "Passkey action on conn_handle=%d", event->passkey.conn_handle);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      ESP_LOGI(TAG, "Repeat pairing: conn_handle=%d",
               event->repeat_pairing.conn_handle);
      // Delete the old bond info and accept the new pairing.
      // Returning RETRY causes NimBLE to delete the old LTK and start a
      // fresh pairing. If we don't, Windows keeps retrying with the
      // mismatched LTK and we see encryption failures (status=7).
      struct ble_gap_conn_desc conn;
      if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &conn) == 0) {
        ble_store_util_delete_peer(&conn.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "Encryption change: conn_handle=%d status=%d",
               event->enc_change.conn_handle, event->enc_change.status);
      // If encryption failed it usually means Windows is offering a
      // stale LTK. Log it loudly so the user knows to remove the
      // device from Windows and pair fresh. We do NOT delete the
      // bond here automatically; that just creates a reconnect loop
      // because Windows keeps offering the same stale LTK.
      if (event->enc_change.status != 0) {
        ESP_LOGW(TAG, "Encryption failed (status=%d) - please remove the "
                       "device from Windows Bluetooth settings and pair again",
                       event->enc_change.status);
      }
      break;
    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(TAG, "MTU update: conn_handle=%d mtu=%d",
               event->mtu.conn_handle, event->mtu.value);
      break;
    case BLE_GAP_EVENT_CONN_UPDATE:
      ESP_LOGI(TAG, "Conn param update: conn_handle=%d status=%d",
               event->conn_update.conn_handle, event->conn_update.status);
      break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      ESP_LOGI(TAG, "Conn param update req: conn_handle=%d", event->conn_update_req.conn_handle);
      // Accept the parameter update request
      return 0;
    default:
      break;
  }
  return 0;
}

static void start_advertising(const char *name) {
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields fields;
  memset(&fields, 0, sizeof(fields));

  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (uint8_t *)name;
  fields.name_len = strlen(name);
  fields.name_is_complete = 1;
  fields.appearance = 0x03C1;
  fields.appearance_is_present = 1;
  static const ble_uuid16_t adv_uuid_hid = BLE_UUID16_INIT(0x1812);
  fields.uuids16 = &adv_uuid_hid;
  fields.num_uuids16 = 1;
  fields.uuids16_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
    return;
  }

  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, gap_event, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
  } else {
    ESP_LOGI(TAG, "Advertising started with name='%s'", name);
  }
}

extern "C" void nimble_host_task(void *param) {
  ESP_LOGI(TAG, "NimBLE host task started");
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void ble_on_sync_impl() {
  ESP_LOGI(TAG, "Bluetooth synced");
  // All GATT services were registered in setup() before
  // nimble_port_freertos_init() so they are visible to the central.
  // Here we only need to start advertising.
  start_advertising(g_device_name);
}

static void ble_on_sync_wrapper(void) {
  ble_on_sync_impl();
}

static void ble_on_reset(int reason) {
  ESP_LOGE(TAG, "Bluetooth reset; reason=%d", reason);
}

/* --- class methods --- */

void Esp32BleKeyboard::setup() {
  ESP_LOGI(TAG, "Setting up BLE Keyboard (ESP-IDF NimBLE)");

  strncpy(g_device_name, name_.c_str(), sizeof(g_device_name) - 1);
  g_device_name[sizeof(g_device_name) - 1] = '\0';
  strncpy(g_manufacturer_id, manufacturer_id_.c_str(), sizeof(g_manufacturer_id) - 1);
  g_manufacturer_id[sizeof(g_manufacturer_id) - 1] = '\0';

  ESP_LOGI(TAG, "Calling nimble_port_init()...");
  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed: %d", err);
    return;
  }
  ESP_LOGI(TAG, "nimble_port_init() succeeded");

  // Register NimBLE host callbacks and SM config
  ble_hs_cfg.sync_cb = ble_on_sync_wrapper;
  ble_hs_cfg.reset_cb = ble_on_reset;

  // Use LE Legacy pairing (sm_sc=0) with Just Works. Windows + NimBLE
  // have known issues with sm_sc=1 when sm_mitm=0, where the central
  // rejects the auth request with BLE_SM_ERR_AUTHREQ (status=7). LE
  // Legacy Just Works is the most compatible option for no-IO keyboards.
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  // Only request the encryption key (LTK) from the central. Asking for
  // the Identity key (IRK) on top of LTK sometimes confuses the host
  // and triggers a status=7 reject.
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

  // CRITICAL: register all GATT services BEFORE nimble_port_freertos_init().
  // ble_gatts_start() runs inside ble_hs_start() which runs when the host
  // task starts, and it only processes the ble_gatts_svc_defs that are
  // present at that moment. Adding services later (e.g. from the sync
  // callback) leaves the GATT database empty from the central's POV.
  ESP_LOGI(TAG, "Registering GATT services...");

  // Standard services: each *_init() internally calls ble_gatts_count_cfg()
  // and ble_gatts_add_svcs() for its own service definition.
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_bas_init();
  // Initialize battery level to the component-configured value (default 100)
  // so the central sees something other than 0% on first read.
  ble_svc_bas_battery_level_set(battery_level_);

  int rc = ble_svc_gap_device_name_set(g_device_name);
  if (rc != 0) ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
  rc = ble_svc_gap_device_appearance_set(0x03C1);  // Appearance = Keyboard
  if (rc != 0) ESP_LOGW(TAG, "ble_svc_gap_device_appearance_set failed: %d", rc);

  // Device Information Service
  ble_svc_dis_init();
  ble_svc_dis_manufacturer_name_set(g_manufacturer_id);
  ble_svc_dis_model_number_set("BLE Keyboard");
  // PnP ID: VID Source=0x02 (USB IF), VID=0x1234 (generic), PID=0x0001, Version=0x0001
  static const uint8_t pnp_id[7] = {
    0x02, 0x34, 0x12, 0x01, 0x00, 0x00, 0x01,
  };
  ble_svc_dis_pnp_id_set((const char *)pnp_id);

  // Custom HID service
  rc = ble_gatts_count_cfg(gatt_services);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
    return;
  }
  rc = ble_gatts_add_svcs(gatt_services);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
    return;
  }

  // Do NOT clear all bonds at boot. Wiping the bond store at every boot
  // means Windows can never re-subscribe to notifications after a
  // reboot (it gets stuck in the AUTHREQ failure loop). Bonds persist
  // across reboots on purpose; we only delete them on actual encryption
  // failure to recover from a corrupt LTK.

  ESP_LOGI(TAG, "Starting NimBLE host task...");
  nimble_port_freertos_init(nimble_host_task);
}
void Esp32BleKeyboard::update() {
  if (state_sensor_ != nullptr) {
    state_sensor_->publish_state(g_connected);
  }
}

void Esp32BleKeyboard::set_battery_level(uint8_t level) {
  battery_level_ = level;
  ble_svc_bas_battery_level_set(level);
}

void Esp32BleKeyboard::send_keyboard_report(uint8_t modifiers, uint8_t key1, uint8_t key2,
                                             uint8_t key3, uint8_t key4, uint8_t key5, uint8_t key6) {
  keyboard_report_[0] = 0x01;  // Report ID
  keyboard_report_[1] = modifiers;
  keyboard_report_[2] = 0;
  keyboard_report_[3] = key1;
  keyboard_report_[4] = key2;
  keyboard_report_[5] = key3;
  keyboard_report_[6] = key4;
  keyboard_report_[7] = key5;
  keyboard_report_[8] = key6;

  // Update boot-keyboard report (without Report ID)
  boot_key_report_[0] = modifiers;
  boot_key_report_[1] = 0;
  boot_key_report_[2] = key1;
  boot_key_report_[3] = key2;
  boot_key_report_[4] = key3;
  boot_key_report_[5] = key4;
  boot_key_report_[6] = key5;
  boot_key_report_[7] = key6;

  if (g_handle_keyboard != 0 && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    ESP_LOGI(TAG, "Sending keyboard report: mod=0x%02X k1=0x%02X k2=0x%02X k3=0x%02X k4=0x%02X k5=0x%02X k6=0x%02X",
             modifiers, key1, key2, key3, key4, key5, key6);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(keyboard_report_, sizeof(keyboard_report_));
    if (om != nullptr) {
      // ble_gatts_notify_custom: we are the GATT server (peripheral),
      // pushing a notification to a subscribed client. ble_gattc_* is for
      // the central/client role, which is not what we want here.
      int rc = ble_gatts_notify_custom(g_conn_handle, g_handle_keyboard, om);
      ESP_LOGI(TAG, "  ble_gatts_notify_custom rc=%d handle=%d conn=%d",
               rc, g_handle_keyboard, g_conn_handle);
    } else {
      ESP_LOGE(TAG, "Failed to allocate mbuf for keyboard report");
    }
  } else {
    ESP_LOGW(TAG, "Cannot send keyboard: handle=%d conn=%d", g_handle_keyboard, g_conn_handle);
  }
}

void Esp32BleKeyboard::send_media_report(uint8_t byte0, uint8_t byte1) {
  media_report_[0] = 0x02;  // Report ID
  media_report_[1] = byte0;
  media_report_[2] = byte1;

  if (g_handle_media != 0 && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    struct os_mbuf *om = ble_hs_mbuf_from_flat(media_report_, sizeof(media_report_));
    if (om != nullptr) {
      ble_gatts_notify_custom(g_conn_handle, g_handle_media, om);
    }
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
  if (c == '\x1B') return {0, 0x29}; // Escape
  if (c == '\b') return {0, 0x2A}; // Backspace
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
  return {0, 0}; // unsupported
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
    send_keyboard_report(0, 0); // release
    delay(default_delay_);
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
  send_keyboard_report(0, key);
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
    ble_gap_adv_stop();
    start_advertising(g_device_name);
  }
}

void Esp32BleKeyboard::stop() {
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
