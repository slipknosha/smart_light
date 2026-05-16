#include "ble_service.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

#include "dimmer.h"
#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);

static const char *TAG = "ble_service";

#define BLE_DEVICE_NAME "SmartLight"

static uint32_t s_level;
static uint8_t s_own_addr_type;
static uint16_t s_level_chr_val_handle;
static smart_light_ble_level_cb_t s_level_cb;
static void *s_level_cb_ctx;

static const ble_uuid128_t s_light_service_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0xef, 0xb4, 0x47, 0x8d, 0x57, 0x9a,
                     0x3d, 0x4b, 0x3f, 0x4c, 0x01, 0x00, 0x5e, 0x7b);

static const ble_uuid128_t s_level_chr_uuid =
    BLE_UUID128_INIT(0x01, 0x00, 0xef, 0xb4, 0x47, 0x8d, 0x57, 0x9a,
                     0x3d, 0x4b, 0x3f, 0x4c, 0x02, 0x00, 0x5e, 0x7b);

static int parse_level_write(struct os_mbuf *om, uint32_t *level)
{
    uint8_t data[4] = {0};
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(om, data, sizeof(data), &len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if ((len == 0U) || (len > sizeof(data))) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint32_t value = 0;
    for (uint16_t i = 0; i < len; i++) {
        value |= ((uint32_t)data[i] << (8U * i));
    }

    if (value > DIMMER_MAX_LEVEL) {
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }

    *level = value;
    return 0;
}

static int level_access_cb(uint16_t conn_handle,
                           uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt,
                           void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (attr_handle != s_level_chr_val_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        uint32_t level = s_level;
        if (level > DIMMER_MAX_LEVEL) {
            level = DIMMER_MAX_LEVEL;
        }

        uint8_t data[4] = {
            (uint8_t)(level & 0xffU),
            (uint8_t)((level >> 8) & 0xffU),
            (uint8_t)((level >> 16) & 0xffU),
            (uint8_t)((level >> 24) & 0xffU),
        };
        int rc = os_mbuf_append(ctxt->om, data, sizeof(data));
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint32_t level = 0;
        int rc = parse_level_write(ctxt->om, &level);
        if (rc != 0) {
            ESP_LOGW(TAG, "Rejected BLE level write");
            return rc;
        }

        smart_light_ble_set_level(level);
        if (s_level_cb != NULL) {
            s_level_cb(level, s_level_cb_ctx);
        }

        ESP_LOGI(TAG, "BLE level set to %" PRIu32 "/%u", level, DIMMER_MAX_LEVEL);
        return 0;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_light_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_level_chr_uuid.u,
                .access_cb = level_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                         BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &s_level_chr_val_handle,
            },
            {
                0,
            },
        },
    },
    {
        0,
    },
};

static void gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;

    char uuid_str[BLE_UUID_STR_LEN];
    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        ESP_LOGI(TAG, "Registered service %s handle=%d",
                 ble_uuid_to_str(ctxt->svc.svc_def->uuid, uuid_str), ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        ESP_LOGI(TAG, "Registered characteristic %s val_handle=%d",
                 ble_uuid_to_str(ctxt->chr.chr_def->uuid, uuid_str), ctxt->chr.val_handle);
        break;

    default:
        break;
    }
}

static int gatt_server_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        return rc;
    }

    return ble_gatts_add_svcs(s_gatt_svcs);
}

static void ble_advertise(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    int rc = 0;
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE central connected, handle=%d", event->connect.conn_handle);
            rc = ble_gap_security_initiate(event->connect.conn_handle);
            if (rc != 0) {
                ESP_LOGW(TAG, "Failed to initiate BLE security, rc=%d", rc);
            }
        } else {
            ESP_LOGW(TAG, "BLE connection failed, status=%d", event->connect.status);
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE central disconnected, reason=%d", event->disconnect.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE advertising completed, reason=%d", event->adv_complete.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE subscribe attr=%d notify=%d indicate=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "BLE MTU updated to %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "BLE connection encrypted, handle=%d", event->enc_change.conn_handle);
        } else {
            ESP_LOGE(TAG, "BLE encryption failed, status=%d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to find BLE connection for repeat pairing, rc=%d", rc);
            return rc;
        }

        ble_store_util_delete_peer(&desc.peer_id_addr);
        ESP_LOGI(TAG, "Deleted old bond, retrying pairing");
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
        return 0;
    }
}

static void ble_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set BLE advertising fields, rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    rsp_fields.uuids128 = (ble_uuid128_t[]) {
        s_light_service_uuid,
    };
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set BLE scan response fields, rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start BLE advertising, rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising as %s", BLE_DEVICE_NAME);
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason=%d", reason);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer BLE address type, rc=%d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(s_own_addr_type, addr_val, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
    }

    ble_advertise();
}

static void ble_host_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t smart_light_ble_init(uint32_t initial_level,
                               smart_light_ble_level_cb_t level_cb,
                               void *level_cb_ctx)
{
    smart_light_ble_set_level(initial_level);
    s_level_cb = level_cb;
    s_level_cb_ctx = level_cb_ctx;

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "init nimble");

    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = gatt_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    int rc = gatt_server_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to init GATT server, rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set BLE device name, rc=%d", rc);
        return ESP_FAIL;
    }

    ble_store_config_init();
    nimble_port_freertos_init(ble_host_task);

    return ESP_OK;
}

void smart_light_ble_set_level(uint32_t level)
{
    if (level > DIMMER_MAX_LEVEL) {
        level = DIMMER_MAX_LEVEL;
    }

    s_level = level;

    if (s_level_chr_val_handle != 0U) {
        ble_gatts_chr_updated(s_level_chr_val_handle);
    }
}
