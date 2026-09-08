/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LOCAL EDIT (S3-Amysynth): AMYSONG / .mid import over USB WebSerial.
 *
 * Wire protocol (LF-terminated commands, then raw payload bytes):
 *   PING\n                    -> PONG\n
 *   GET song\n                -> OK:song count=.. enabled=.. loop=.. | s<idx>:<bars>/<mask>...
 *                              (diagnostic dump of the live song chain)
 *   PUT <slot> <fmt> <bars> <len>\n
 *                             fmt = "txt" (amysong text), "mid" (SMF loop) or
 *                                   "arr" (whole SMF arranged by section markers)
 *                             bars = 1|2 for "mid", 0 for "arr", ignored for "txt"
 *                             -> ACK\n (or ERR:<reason>\n)
 *                             then exactly <len> raw bytes,
 *                             -> OK:saved to slot <n>\n (or ERR:<reason>\n)
 *
 * The request is parsed and the payload captured on the CDC pump task; the
 * actual song_import_apply / midi_amysong_convert / midi_amysong_arrange_convert
 * runs on the sequencer's single-applier ui task via usb_import_service(),
 * exactly like the WiFi importer. Call usb_import_service() from the same
 * task that calls wifi_import_service().
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the CDC pump task and register the import protocol.
 *        Must run after the USB audio stack is up. Non-fatal and idempotent:
 *        if the config gate is off, enables nothing and returns ESP_OK.
 */
esp_err_t usb_importer_start(void);

/**
 * @brief Apply a pending USB upload. Same single-applier contract as
 *        wifi_import_service(): call it periodically from the synth_ui task.
 */
void usb_import_service(void);

/**
 * @brief Transient status line for the hint strip, or NULL when idle.
 */
const char *usb_import_status_line(void);

#ifdef __cplusplus
}
#endif