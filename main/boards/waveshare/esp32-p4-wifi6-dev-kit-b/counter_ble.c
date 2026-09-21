#include "counter_ble.h"
#include "sdkconfig.h"

#if CONFIG_COUNTER_BLE_LINK
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char* TAG = "CounterBle";
// 7b8e0001-6c4d-4b21-9f30-58a6d2e41000 (NimBLE stores UUIDs little endian).
#define COUNTER_UUID(n)                                                                         \
    BLE_UUID128_INIT(0x00, 0x10, 0xe4, 0xd2, 0xa6, 0x58, 0x30, 0x9f, 0x21, 0x4b, 0x4d, 0x6c, n, \
                     0x00, 0x8e, 0x7b)
static const ble_uuid128_t service_uuid = COUNTER_UUID(1);
static const ble_uuid128_t info_uuid = COUNTER_UUID(2);
static const ble_uuid128_t command_uuid = COUNTER_UUID(3);
static const ble_uuid128_t reply_uuid = COUNTER_UUID(4);
static uint8_t own_addr_type;
static uint8_t device_mac[6];
static char device_name[24];
static uint16_t reply_handle;
static uint16_t active_connection = BLE_HS_CONN_HANDLE_NONE;
static bool subscribed;
// 0: no request, 1: pending local approval, 2: approved for this connection only.
static atomic_int approval_state;
int CounterBleApprovalState(void) { return atomic_load(&approval_state); }
void CounterBleApprove(void) {
    int pending = 1;
    atomic_compare_exchange_strong(&approval_state, &pending, 2);
}
static int gap_event(struct ble_gap_event* event, void* arg);

static int access_characteristic(uint16_t connection, uint16_t handle,
                                 struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)handle;
    (void)arg;
    if (ble_uuid_cmp(ctxt->chr->uuid, &info_uuid.u) == 0 &&
        ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t packet[14] = {0x58, 0x54, 1, 0x80};
        memcpy(packet + 4, device_mac, sizeof(device_mac));
        uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
        for (int i = 0; i < 4; ++i)
            packet[10 + i] = (uint8_t)(uptime >> (8 * i));
        return os_mbuf_append(ctxt->om, packet, sizeof(packet)) == 0 ? 0
                                                                     : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ble_uuid_cmp(ctxt->chr->uuid, &command_uuid.u) != 0 ||
        ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    // Link probe only: no display, recording, credential or account mutations.
    uint8_t packet[8];
    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(packet))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(ctxt->om, 0, sizeof(packet), packet) != 0)
        return BLE_ATT_ERR_UNLIKELY;
    if (packet[0] != 0x58 || packet[1] != 0x54 || packet[2] != 1 ||
        (packet[3] != 1 && packet[3] != 2 && packet[3] != 3))
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    if (connection != active_connection || !subscribed)
        return BLE_ATT_ERR_UNLIKELY;
    uint8_t response[9];
    memcpy(response, packet, sizeof(packet));
    response[3] |= 0x80;
    if (packet[3] == 2) {
        int empty = 0;
        atomic_compare_exchange_strong(&approval_state, &empty, 1);
    }
    response[8] = CounterBleApprovalState();
    struct os_mbuf* reply = ble_hs_mbuf_from_flat(response, packet[3] == 1 ? 8 : 9);
    if (!reply)
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    // notify_custom consumes the mbuf, including on failure.
    return ble_gatts_notify_custom(connection, reply_handle, reply) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def characteristics[] = {
    {.uuid = &info_uuid.u, .access_cb = access_characteristic, .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &command_uuid.u, .access_cb = access_characteristic, .flags = BLE_GATT_CHR_F_WRITE},
    {.uuid = &reply_uuid.u,
     .access_cb = access_characteristic,
     .flags = BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &reply_handle},
    {0},
};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY,
     .uuid = &service_uuid.u,
     .characteristics = characteristics},
    {0},
};

static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t*)&service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    struct ble_hs_adv_fields response = {0};
    response.name = (uint8_t*)device_name;
    response.name_len = strlen(device_name);
    response.name_is_complete = 1;
    if (!rc)
        rc = ble_gap_adv_rsp_set_fields(&response);
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 320;
    params.itvl_max = 480;
    if (!rc)
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc)
        ESP_LOGE(TAG, "Advertising failed rc=%d", rc);
    else
        ESP_LOGI(TAG, "Advertising %s", device_name);
}

static int gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                active_connection = event->connect.conn_handle;
                subscribed = false;
                atomic_store(&approval_state, 0);
                ESP_LOGI(TAG, "Connected handle=%u", active_connection);
            } else
                advertise();
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            active_connection = BLE_HS_CONN_HANDLE_NONE;
            subscribed = false;
            atomic_store(&approval_state, 0);
            ESP_LOGI(TAG, "Disconnected reason=%d", event->disconnect.reason);
            advertise();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.conn_handle == active_connection &&
                event->subscribe.attr_handle == reply_handle)
                subscribed = event->subscribe.cur_notify;
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
        default:
            break;
    }
    return 0;
}

static void on_reset(int reason) {
    atomic_store(&approval_state, 0);
    active_connection = BLE_HS_CONN_HANDLE_NONE;
    subscribed = false;
    ESP_LOGW(TAG, "Host reset reason=%d", reason);
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc)
        rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc)
        ESP_LOGE(TAG, "BLE address unavailable rc=%d", rc);
    else
        advertise();
}

static void host_task(void* arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void initialize(void* arg) {
    (void)arg;
    esp_err_t err = esp_read_mac(device_mac, ESP_MAC_BASE);
    if (err != ESP_OK)
        goto done;
    snprintf(device_name, sizeof(device_name), "XingTun-P4-%02X%02X%02X", device_mac[3],
             device_mac[4], device_mac[5]);
    esp_hosted_connect_to_slave();
    err = esp_hosted_bt_controller_init();
    if (err != ESP_OK)
        goto done;
    err = esp_hosted_bt_controller_enable();
    if (err != ESP_OK)
        goto done;
    err = nimble_port_init();
    if (err != ESP_OK)
        goto done;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(device_name);
    if (!rc)
        rc = ble_gatts_count_cfg(services);
    if (!rc)
        rc = ble_gatts_add_svcs(services);
    if (rc) {
        ESP_LOGE(TAG, "Service registration failed rc=%d", rc);
        nimble_port_deinit();
        err = ESP_FAIL;
        goto done;
    }
    nimble_port_freertos_init(host_task);
done:
    if (err != ESP_OK)
        ESP_LOGE(TAG, "BLE initialization failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

void StartCounterBle(void) {
    static bool started;
    if (started)
        return;
    started = xTaskCreate(initialize, "counter_ble_init", 4096, NULL, 3, NULL) == pdPASS;
    if (!started)
        ESP_LOGE(TAG, "Cannot allocate BLE initialization task");
}
#else
void StartCounterBle(void) {}
int CounterBleApprovalState(void) { return 0; }
void CounterBleApprove(void) {}
#endif
