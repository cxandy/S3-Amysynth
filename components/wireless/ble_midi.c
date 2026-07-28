#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS

#include "ble_midi.h"
#include "midi_core.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_log.h"

static const char *TAG = "ble_midi";

#define BLE_MIDI_DEVICE_NAME "Amysynth"

/* NVS-backed bond store (NimBLE store/config module). Not declared in any
 * public header, so declare it here as the IDF examples do. */
void ble_store_config_init(void);

/* BLE-MIDI service/characteristic UUIDs (MIDI over BLE spec), little-endian
 * byte order as BLE_UUID128_INIT expects.
 * Service:        03B80E5A-EDE8-4B33-A751-6CE34EC4C700
 * Characteristic: 7772E5DB-3868-4112-A1A9-F2669D106BF3 */
static const ble_uuid128_t midi_svc_uuid =
    BLE_UUID128_INIT(0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
                     0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t midi_chr_uuid =
    BLE_UUID128_INIT(0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
                     0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static volatile bool s_connected = false;
static void (*s_disconnect_cb)(void) = 0;
static uint8_t s_own_addr_type = 0;

/* Flatten target for incoming writes, sized for the largest ATT payload we
 * negotiate. Static: only ever touched by the NimBLE host task. */
static uint8_t s_rx_buf[512];

static void ble_midi_advertise(void);

/* Strip BLE-MIDI packet framing and feed the plain MIDI bytes to midi_core.
 * Packet = header byte, then interleaved timestamp bytes and MIDI messages.
 * A high-bit byte after a data byte (or at message boundary) is a timestamp;
 * two consecutive high-bit bytes are timestamp-then-status. Running-status
 * data bytes arrive with no preceding timestamp and pass straight through. */
static void ble_midi_ingest_packet(const uint8_t *data, uint16_t len)
{
    if (len < 2) return;                       /* header only: nothing to do */
    bool last_was_ts = false;
    for (uint16_t i = 1; i < len; i++) {
        uint8_t b = data[i];
        if (b & 0x80u) {
            if (!last_was_ts) {                /* timestamp byte: swallow */
                last_was_ts = true;
                continue;
            }
            last_was_ts = false;               /* status byte */
            midi_core_feed(&b, 1);
        } else {
            last_was_ts = false;               /* data byte */
            midi_core_feed(&b, 1);
        }
    }
}

static int midi_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            /* BLE-MIDI spec: reads return no payload. */
            return 0;
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint16_t out_len = 0;
            int rc = ble_hs_mbuf_to_flat(ctxt->om, s_rx_buf,
                                         sizeof(s_rx_buf), &out_len);
            if (rc != 0) return BLE_ATT_ERR_INSUFFICIENT_RES;
#if CONFIG_SYNTH_BLE_MIDI_LOG
            ESP_LOGI(TAG, "rx %u bytes", (unsigned)out_len);
#endif
            ble_midi_ingest_packet(s_rx_buf, out_len);
            return 0;
        }
        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

/* One service, one characteristic. NOTIFY is declared so the CCCD exists for
 * hosts that probe it, though nothing notifies yet; the _ENC flags make the
 * stack require pairing/encryption per the BLE-MIDI spec. */
static const struct ble_gatt_svc_def midi_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &midi_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &midi_chr_uuid.u,
                .access_cb = midi_chr_access,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                             BLE_GATT_CHR_F_WRITE_NO_RSP |
                             BLE_GATT_CHR_F_WRITE_ENC |
                             BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }
        },
    },
    { 0 }
};

static int ble_midi_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_connected = true;
                midi_core_reset();
                ESP_LOGI(TAG, "central connected");
            } else {
                ESP_LOGW(TAG, "connect failed (%d), re-advertising",
                         event->connect.status);
                ble_midi_advertise();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            ESP_LOGI(TAG, "central disconnected (reason %d)",
                     event->disconnect.reason);
            if (s_disconnect_cb) s_disconnect_cb();
            ble_midi_advertise();
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ble_midi_advertise();
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* Peer lost its bond: delete ours so it can pair again. Without
             * this, re-pairing after the host "forgets" us fails. */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

        default:
            return 0;
    }
}

static void ble_midi_advertise(void)
{
    /* 31-byte legacy ADV payload: flags (3) + 128-bit service UUID (18).
     * The device name does not fit alongside them, so it goes in the scan
     * response - standard practice for BLE-MIDI peripherals. */
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&midi_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)BLE_MIDI_DEVICE_NAME;
    rsp.name_len = sizeof(BLE_MIDI_DEVICE_NAME) - 1;
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_midi_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

static void ble_midi_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return;
    }
    ble_midi_advertise();
    ESP_LOGI(TAG, "advertising as \"%s\"", BLE_MIDI_DEVICE_NAME);
}

static void ble_midi_on_reset(int reason)
{
    s_connected = false;
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

int ble_midi_setup(void)
{
    s_connected = false;

    ble_hs_cfg.sync_cb  = ble_midi_on_sync;
    ble_hs_cfg.reset_cb = ble_midi_on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* Just Works pairing with bonding: no IO capability, no MITM, LE Secure
     * Connections, LTKs distributed both ways so the bond survives. */
    ble_hs_cfg.sm_io_cap  = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm    = 0;
    ble_hs_cfg.sm_sc      = 1;
    ble_hs_cfg.sm_our_key_dist   |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(midi_gatt_svcs);
    if (rc != 0) return rc;
    rc = ble_gatts_add_svcs(midi_gatt_svcs);
    if (rc != 0) return rc;
    rc = ble_svc_gap_device_name_set(BLE_MIDI_DEVICE_NAME);
    if (rc != 0) return rc;

    ble_store_config_init();
    return 0;
}

void ble_midi_host_task(void *param)
{
    (void)param;
    nimble_port_run();              /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();  /* self-deletes this task */
}

bool ble_midi_connected(void)
{
    return s_connected;
}

void ble_midi_set_disconnect_cb(void (*cb)(void))
{
    s_disconnect_cb = cb;
}

#endif /* CONFIG_SYNTH_WIRELESS */
