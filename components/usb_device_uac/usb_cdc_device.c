/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LOCAL EDIT (S3-Amysynth): WebSerial song-import CDC device layer.
 * See usb_cdc_device.h for the contract and protocol.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "usb_cdc_device.h"

static const char *TAG = "usb_cdc";

#define CDC_LINE_CAP      (128)
#define CDC_PUMP_STACK    (4096)
#define CDC_PUMP_PRIO     (3)
#define CDC_PUMP_CORE     (0)

typedef enum {
    CDC_FRAMING_LINE,
    CDC_FRAMING_PAYLOAD,
} cdc_framing_t;

typedef struct {
    cdc_line_cb_t    line_cb;
    cdc_payload_cb_t payload_cb;
    void            *ctx;
    cdc_framing_t    framing;
    size_t           payload_expect;
    size_t           payload_got;
    uint8_t         *payload_buf;
} cdc_device_t;

static cdc_device_t s_cdc;

static void cdc_reply_raw(const char *s)
{
    size_t len = strlen(s);
    size_t sent = 0;
    while (sent < len) {
        uint32_t n = tud_cdc_write(s + sent, (uint32_t)(len - sent));
        if (n == 0 && !tud_cdc_write_available()) {
            tud_cdc_write_flush();
        }
        sent += n;
    }
    tud_cdc_write_flush();
}

esp_err_t usb_cdc_reply(const char *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    cdc_reply_raw(s);
    return ESP_OK;
}

esp_err_t usb_cdc_begin_payload(size_t len)
{
    if (s_cdc.framing != CDC_FRAMING_LINE) return ESP_ERR_INVALID_STATE;
    if (len == 0) return ESP_ERR_INVALID_ARG;

    uint8_t *buf = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "payload alloc failed (%u bytes)", (unsigned)len);
        cdc_reply_raw("ERR:no mem\n");
        return ESP_ERR_NO_MEM;
    }
    s_cdc.payload_buf     = buf;
    s_cdc.payload_expect  = len;
    s_cdc.payload_got     = 0;
    s_cdc.framing         = CDC_FRAMING_PAYLOAD;
    return ESP_OK;
}

static void cdc_abort_payload(void)
{
    heap_caps_free(s_cdc.payload_buf);
    s_cdc.payload_buf    = NULL;
    s_cdc.payload_expect = 0;
    s_cdc.payload_got    = 0;
    s_cdc.framing        = CDC_FRAMING_LINE;
}

static void cdc_pump_task(void *arg)
{
    (void)arg;
    char line[CDC_LINE_CAP];
    size_t line_len = 0;

    for (;;) {
        if (!tud_cdc_connected()) {
            /* Host unplugged / terminal closed: drop any half-received
             * frame so the next connection starts on a clean line. */
            if (s_cdc.framing == CDC_FRAMING_PAYLOAD) cdc_abort_payload();
            line_len = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_cdc.framing == CDC_FRAMING_PAYLOAD) {
            while (tud_cdc_available()) {
                size_t want = s_cdc.payload_expect - s_cdc.payload_got;
                uint32_t n = tud_cdc_read(s_cdc.payload_buf + s_cdc.payload_got,
                                          (uint32_t)want);
                s_cdc.payload_got += n;
                if (s_cdc.payload_got >= s_cdc.payload_expect) {
                    s_cdc.payload_buf[s_cdc.payload_expect] = '\0';
                    for (size_t i = 0; i < s_cdc.payload_expect; i++) {
                        if (s_cdc.payload_buf[i] == '\0') {
                            s_cdc.payload_buf[i] = ' ';
                        }
                    }
                    void *ctx = s_cdc.ctx;
                    cdc_payload_cb_t cb = s_cdc.payload_cb;
                    s_cdc.framing = CDC_FRAMING_LINE;
                    if (cb) cb(s_cdc.payload_buf, s_cdc.payload_expect, ctx);
                    heap_caps_free(s_cdc.payload_buf);
                    s_cdc.payload_buf = NULL;
                    break;
                }
            }
        } else {
            while (tud_cdc_available()) {
                uint8_t b;
                if (tud_cdc_read(&b, 1) != 1) break;
                if (b == '\n') {
                    if (line_len > 0 && s_cdc.line_cb) {
                        line[line_len] = '\0';
                        size_t cb_len = line_len;
                        line_len = 0;
                        s_cdc.line_cb(line, cb_len, s_cdc.ctx);
                    } else {
                        line_len = 0;
                    }
                } else if (b == '\r') {
                    /* drop */
                } else if (line_len + 1 < sizeof line) {
                    line[line_len++] = (char)b;
                } else {
                    line_len = 0;   /* overlong line: drop it */
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool usb_cdc_connected(void)
{
    return tud_cdc_connected();
}

esp_err_t usb_cdc_device_init(cdc_line_cb_t line_cb, cdc_payload_cb_t payload_cb,
                              void *ctx)
{
    ESP_RETURN_ON_FALSE(line_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "line_cb NULL");
    s_cdc.line_cb      = line_cb;
    s_cdc.payload_cb   = payload_cb;
    s_cdc.ctx          = ctx;
    s_cdc.framing      = CDC_FRAMING_LINE;

    BaseType_t ok = xTaskCreatePinnedToCore(cdc_pump_task, "usb_cdc_pump",
                                            CDC_PUMP_STACK, NULL,
                                            CDC_PUMP_PRIO, NULL, CDC_PUMP_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "pump task create failed");
    ESP_LOGI(TAG, "CDC pump up (WebSerial import port ready)");
    return ESP_OK;
}