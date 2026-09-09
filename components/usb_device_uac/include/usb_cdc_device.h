/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LOCAL EDIT (S3-Amysynth): WebSerial song-import CDC device layer.
 *
 * Runs a pump task that frames the CDC bulk stream into two callbacks:
 * complete ASCII lines (for command/response) and raw payload blocks (for a
 * length-prefixed upload frame). Keeps every TinyUSB CDC call on ONE task
 * (the pump), so no lock is needed around tud_cdc_* and the response to an
 * upload is written from where the upload arrived.
 *
 * Protocol (host -> device, LF-terminated commands):
 *   PING\n                       -> "PONG\n"
 *   PUT <slot> <fmt> <bars> <len>\n -> "ACK\n" (or "ERR:...\n"), then the
 *                                    next <len> raw bytes arrive as one
 *                                    payload block; the payload callback
 *                                    is responsible for the eventual
 *                                    "OK:..." / "ERR:..." reply.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*cdc_line_cb_t)(const char *line, size_t len, void *ctx);
typedef void (*cdc_payload_cb_t)(const uint8_t *data, size_t len, void *ctx);

/**
 * @brief Start the CDC pump task.
 *
 * @param line_cb    invoked (on the pump task) once per complete LF-terminated
 *                   line ('\r' stripped). May call usb_cdc_reply() and
 *                   usb_cdc_begin_payload().
 * @param payload_cb invoked (on the pump task) when exactly the requested
 *                   number of raw payload bytes arrived. The buffer is only
 *                   valid for the duration of the call.
 * @param ctx        passed through to both callbacks.
 */
esp_err_t usb_cdc_device_init(cdc_line_cb_t line_cb, cdc_payload_cb_t payload_cb,
                              void *ctx);

/**
 * @brief Switch the pump from line mode to exact-length raw capture.
 *
 * Callable only from within the line callback (the pump owns the token).
 * On success the pump allocates a len+1 PSRAM buffer and delivers it as one
 * payload block; a preceding "ACK\n"/"ERR:...\n" should be sent by the caller
 * so the host knows the frame was accepted.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_STATE outside a line callback /
 *         already capturing, ESP_ERR_NO_MEM on allocation failure (in which
 *         case "ERR:no mem\n" is written before returning to line mode).
 *
 * @param len        exact payload byte count to capture.
 * @param sanitize_nul true for text payloads: NUL bytes are replaced with
 *                     spaces so a WebSerial text read cannot truncate the
 *                     buffer; pass false for raw binary .mid bodies.
 */
esp_err_t usb_cdc_begin_payload(size_t len, bool sanitize_nul);

/**
 * @brief Write a response and flush it. Callable only from the pump task
 *        (i.e. inside a callback handed to usb_cdc_device_init).
 */
esp_err_t usb_cdc_reply(const char *s);

/**
 * @return true while the CDC line state is DTR (host port open under
 *         WebSerial / any terminal).
 */
bool usb_cdc_connected(void);

#ifdef __cplusplus
}
#endif