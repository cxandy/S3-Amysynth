/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LOCAL EDIT (S3-Amysynth): AMYSONG / .mid import over USB WebSerial.
 *
 * Architecture mirrors wifi_importer.c: the CDC pump task (usb_cdc_device.c)
 * captures the PUT frame and parks it; usb_import_service() - called from the
 * sequencer's single-applier ui task, same task that calls
 * wifi_import_service() - applies it with song_import_apply() and posts the
 * "OK:saved to slot N"/"ERR:..." line back through the pump task.
 *
 * Dependency direction matters: this component PRIV_REQUIRES synth_core and
 * usb_device_uac; synth_core must NOT require usb_importer (cycle). synth_ui/
 * and main only see the small extern surface declared here.
 */

#include "usb_importer.h"
#include "usb_cdc_device.h"
#include "midi_import.h"
#include "song_import.h"
#include "sequencer_core.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#if CONFIG_SYNTH_USB_IMPORT

static const char *TAG = "usb_import";

#define IMP_MAX_BODY        (60 * 1024)      /* AMYSONG text cap      */
#define IMP_MIDI_TEXT_CAP   (2048)           /* on-chip SMF -> text   */
#define IMP_RESULT_WAIT_MS  (12000)
#define IMP_STATUS_LINGER_MS (6000)

/* Firmware build tag (DIAG-N). Query with: GET ver */
#define IMP_VERSION_STR     "DIAG-7"

typedef struct {
    SemaphoreHandle_t done_sem;
    /* request descriptor, written by the CDC pump task on the PUT line    */
    uint8_t          slot;
    uint8_t          mode;          /* 0 txt, 1 mid (loop), 2 arr (whole)   */
    uint8_t          bars;
    /* payload, released-published by the pump after the last byte         */
    uint8_t         *body;
    size_t           len;
    volatile bool    pending;
    /* outcome, written by the ui task, read by the pump                   */
    char             result[256];
    /* status line for the hint strip */
    char             status[64];
    TickType_t       status_tick;
    bool             started;
} cdc_imp_t;

static cdc_imp_t s_imp;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void imp_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_imp.status, sizeof s_imp.status, fmt, ap);
    va_end(ap);
    s_imp.status_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "%s", s_imp.status);
}

/* Runs on the CDC pump task: complete command line. */
static esp_err_t cdc_on_line(const char *line, size_t len, void *ctx)
{
    (void)ctx;
    if (len == 4 && strncmp(line, "PING", 4) == 0) {
        return usb_cdc_reply("PONG\n");
    }

    if (len == 7 && strncmp(line, "GET ver", 7) == 0) {
        char msg[48];
        snprintf(msg, sizeof msg, "OK:S3-Amysynth %s\n", IMP_VERSION_STR);
        return usb_cdc_reply(msg);
    }

    if (len == 8 && strcmp(line, "RST boot") == 0) {
        /* Software reset into the ROM download/flash mode. No physical
         * BOOT+RESET needed.
         *
         * The ROM honors RTC_CNTL_OPTION1_REG / FORCE_DOWNLOAD_BOOT on the
         * next reset. Two things must stay true across that reset:
         *
         * 1. FORCE_DOWNLOAD_BOOT must SURVIVE the reset. DIAG-6 used the
         *    `--after watchdog-reset` config esptool writes (STG0=5 +
         *    WDT_CHIP_RESET_EN): that is a full CHIP reset which wipes the
         *    whole RTC domain, FORCE_DOWNLOAD_BOOT included, so the ROM re-
         *    sampled a clean boot and landed back in the app (screen rebooted,
         *    still woke up as PID 8000). A software/esp_restart() on this
         *    board never completed the reboot at all. So we arm the RTC WDT
         *    with STG0=3 (RWDT_LL_STG_SEL_RESET_SYSTEM): it waits ~1s then
         *    triggers a SYSTEM reset, which resets CPU + digital peripherals
         *    but PRESERVES the RTC domain (RTC_CNTL_OPTION1 keeps its bits).
         *    That is the exact reset class arduino/esp32 and esptool rely on
         *    when they set FORCE_DOWNLOAD_BOOT and reboot into download mode.
         *
         * 2. D+/D- must go back to the native USB-Serial/JTAG (ROM download
         *    enumerates as PID 1001 on COM8) instead of staying routed to the
         *    USB-OTG the app uses. The OTG routing lives in RTC_CNTL_USB_CONF
         *    (set by the app at boot, RTC-domain so it also survives a SYSTEM
         *    reset), so we clear its PHY/PAD select bits before arming the WDT.
         *
         * The WDT unlock key is the literal 0x50D83AA1 - the rtc_cntl_reg.h
         * macro RTC_CNTL_WDT_WKEY is only the 32-bit field mask (0xFFFFFFFF)
         * and MUST NOT be used. */
        usb_cdc_reply("OK:reboot to bootloader\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                            RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL |
                                RTC_CNTL_USB_PAD_ENABLE | RTC_CNTL_USB_PAD_ENABLE_OVERRIDE);
        REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1u);
        REG_WRITE(RTC_CNTL_WDTCONFIG1_REG, 2000); /* stage0 hold */
        REG_WRITE(RTC_CNTL_WDTCONFIG0_REG, (1u << 31) | (3u << 28)); /* WDT_EN + STG0=RESET_SYSTEM */
        REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);
        /* Do not attempt the software reboot; the armed WDT system-resets the
         * chip ~1s later on its own, even if this task stalls. Return now. */
        return ESP_OK;
    }

    if (len == 8 && strncmp(line, "GET song", 8) == 0) {
        /* Diagnostic dump of the live song chain (read-only accessors). */
        uint8_t count = sequencer_core_song_get_count();
        char msg[384];
        int n = snprintf(msg, sizeof msg, "OK:song count=%u enabled=%d loop=%d",
                         (unsigned)count,
                         sequencer_core_song_get_enabled() ? 1 : 0,
                         sequencer_core_song_get_loop() ? 1 : 0);
        for (uint8_t i = 0; i < count && i < 16 && n < (int)sizeof msg - 24; i++) {
            uint8_t bars = 0, mask = 0;
            sequencer_core_song_get_scene(i, &bars, &mask);
            n += snprintf(msg + n, sizeof msg - (size_t)n, " | s%u:%u/%u",
                          (unsigned)i, (unsigned)bars, (unsigned)mask);
        }
        return usb_cdc_reply(msg);
    }

    char fmt[8];
    unsigned slot = 0, bars = 0;
    size_t plen = 0;
    int parsed = sscanf(line, "PUT %u %7s %u %zu", &slot, fmt, &bars, &plen);
    if (parsed != 4) {
        return usb_cdc_reply("ERR:bad cmd\n");
    }
    if (slot < 1 || slot > CONFIG_SYNTH_PROJECT_MAX_SLOTS) {
        char msg[48];
        snprintf(msg, sizeof msg, "ERR:slot 1..%u\n",
                 (unsigned)CONFIG_SYNTH_PROJECT_MAX_SLOTS);
        return usb_cdc_reply(msg);
    }
    uint8_t mode;
    if (strcmp(fmt, "txt") == 0) {
        mode = 0;
    } else if (strcmp(fmt, "mid") == 0) {
        mode = 1;
        if (bars != 1 && bars != 2) return usb_cdc_reply("ERR:bars 1..2\n");
    } else if (strcmp(fmt, "arr") == 0) {
        mode = 2;
        if (bars != 0) return usb_cdc_reply("ERR:arr needs bars 0\n");
    } else {
        return usb_cdc_reply("ERR:fmt txt|mid|arr\n");
    }
    if (plen == 0 || plen > IMP_MAX_BODY) {
        return usb_cdc_reply("ERR:len 1..61440\n");
    }
    if (s_imp.pending) {
        return usb_cdc_reply("ERR:busy\n");
    }

    /* Stash the request; the payload callback finishes the handoff. */
    portENTER_CRITICAL(&s_mux);
    s_imp.slot    = (uint8_t)(slot - 1);
    s_imp.mode    = mode;
    s_imp.bars    = (uint8_t)bars;
    portEXIT_CRITICAL(&s_mux);

    esp_err_t err = usb_cdc_begin_payload(plen, mode == 0);
    if (err != ESP_OK) return ESP_OK;   /* begin_payload already replied ERR */
    return usb_cdc_reply("ACK\n");
}

/* Runs on the CDC pump task, exactly once per accepted PUT: the full body is
 * present. Block here until the ui task applies it, then reply the outcome. */
static void cdc_on_payload(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_mux);
    s_imp.body    = (uint8_t *)data;
    s_imp.len     = len;
    s_imp.pending = true;
    portEXIT_CRITICAL(&s_mux);

    if (s_imp.done_sem &&
        xSemaphoreTake(s_imp.done_sem, pdMS_TO_TICKS(IMP_RESULT_WAIT_MS)) == pdTRUE) {
        usb_cdc_reply(s_imp.result);
    } else {
        usb_cdc_reply("ERR:no response\n");
    }
}

/* ── public ── */

void usb_import_service(void)
{
    uint8_t *body;
    size_t   len;
    uint8_t  slot, mode, bars;

    portENTER_CRITICAL(&s_mux);
    if (!s_imp.pending) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    body    = s_imp.body;
    len     = s_imp.len;
    slot    = s_imp.slot;
    mode    = s_imp.mode;
    bars    = s_imp.bars;
    s_imp.pending = false;
    portEXIT_CRITICAL(&s_mux);

    char out[256];
    out[0] = '\0';
    s_imp.result[0] = '\0';
    bool ok = false;

    if (mode != 0) {
        char *text = heap_caps_malloc(IMP_MIDI_TEXT_CAP, MALLOC_CAP_SPIRAM);
        if (!text) {
            snprintf(s_imp.result, sizeof s_imp.result, "ERR:no mem");
            if (s_imp.done_sem) xSemaphoreGive(s_imp.done_sem);
            return;
        }
        char cvt_err[96];
        char ctext[128];
        ctext[0] = '\0';
        int cvt = (mode == 1)
                  ? midi_amysong_convert(body, len, (int)bars, 256, NULL,
                                         text, IMP_MIDI_TEXT_CAP,
                                         cvt_err, sizeof cvt_err)
                  : midi_amysong_arrange_convert(body, len, 256, NULL,
                                                 text, IMP_MIDI_TEXT_CAP,
                                                 cvt_err, sizeof cvt_err);
        if (cvt != 0) {
            snprintf(s_imp.result, sizeof s_imp.result, "ERR:%s",
                     cvt_err[0] ? cvt_err : "midi parse failed");
        } else {
            ok = song_import_apply(slot, text, NULL, out, sizeof out);
            if (!ok) {
                snprintf(ctext, sizeof ctext, "%.127s", text);
                snprintf(s_imp.result, sizeof s_imp.result,
                         "ERR:apply failed out='%.50s' conv='%.100s'",
                         out, ctext);
            }
        }
        heap_caps_free(text);
    } else {
        ok = song_import_apply(slot, (const char *)body, NULL, out, sizeof out);
    }

    if (ok) {
        snprintf(s_imp.result, sizeof s_imp.result,
                 "OK:saved to slot %u", (unsigned)(slot + 1));
    } else if (out[0] == '\0' || s_imp.result[0] == 'E') {
        /* keep the diagnostic reply (already set) or the generic one */
    } else {
        snprintf(s_imp.result, sizeof s_imp.result, "ERR:%s",
                 out[0] ? out : "import failed");
    }
    imp_set_status("usb import: %s", s_imp.result);
    if (s_imp.done_sem) xSemaphoreGive(s_imp.done_sem);
}

const char *usb_import_status_line(void)
{
    if (s_imp.status[0] &&
        (xTaskGetTickCount() - s_imp.status_tick) <
            pdMS_TO_TICKS(IMP_STATUS_LINGER_MS)) {
        return s_imp.status;
    }
    return NULL;
}

esp_err_t usb_importer_start(void)
{
    if (s_imp.started) return ESP_OK;
    s_imp.done_sem = xSemaphoreCreateBinary();
    if (!s_imp.done_sem) return ESP_ERR_NO_MEM;
    esp_err_t err = usb_cdc_device_init(cdc_on_line, cdc_on_payload, NULL);
    if (err != ESP_OK) return err;
    s_imp.started = true;
    ESP_LOGI(TAG, "WebSerial import ready (USB CDC)");
    return ESP_OK;
}

#endif /* CONFIG_SYNTH_USB_IMPORT */