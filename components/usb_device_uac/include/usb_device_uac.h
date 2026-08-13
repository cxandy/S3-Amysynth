/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*uac_output_cb_t)(uint8_t *buf, size_t len, void *cb_ctx);
typedef esp_err_t (*uac_input_cb_t)(uint8_t *buf, size_t len, size_t *bytes_read, void *cb_ctx);
typedef void (*uac_set_mute_cb_t)(uint32_t mute, void *cb_ctx);
typedef void (*uac_set_volume_cb_t)(uint32_t volume, void *cb_ctx);

/**
 * @brief USB UAC Device Config
 *
 */
typedef struct  {
    bool skip_tinyusb_init;                      /*!< if true, the Tinyusb and usb phy will not be initialized */
    uac_output_cb_t output_cb;                   /*!< callback function for UAC data output, if NULL, output will be disabled */
    uac_input_cb_t input_cb;                     /*!< callback function for UAC data input, if NULL, input will be disabled */
    uac_set_mute_cb_t set_mute_cb;               /*!< callback function for set mute, if NULL, the set mute request will be ignored */
    uac_set_volume_cb_t set_volume_cb;           /*!< callback function for set volume, if NULL, the set volume request will be ignored */
    void *cb_ctx;                                /*!< callback context, for user specific usage */
#if CONFIG_USB_DEVICE_UAC_AS_PART
    int spk_itf_num;                             /*!< If CONFIG_USB_DEVICE_UAC_AS_PART is enabled, you need to provide the speaker interface number */
    int mic_itf_num;                             /*!< If CONFIG_USB_DEVICE_UAC_AS_PART is enabled, you need to provide the mic interface number */
#endif
} uac_device_config_t;

/**
 * @brief Initialize the USB Audio Class (UAC) device.
 *
 * @param config Pointer to the UAC device configuration structure.
 * @return
 *       - ESP_OK on success
 *       - ESP_FAIL on failure
 */
esp_err_t uac_device_init(uac_device_config_t *config);

/**
 * @brief LOCAL EDIT (S3-Amysynth): mic pull-cadence diagnostics.
 *
 * Snapshot of the EP-IN supply path counters, for attributing a consumption
 * deficit to its layer (frames that never reached the pre-load callback vs
 * FIFO-room gate skips vs executed pulls).
 *
 * Obligations: callable from any task; out and t_us non-NULL.
 * Guarantees: with CONFIG_AMYSYNTH_DROPOUT_TS compiled in, fills
 * out[0..3] = {pre-load callback invocations, FIFO-room gate skips,
 * executed pulls, bytes pulled} cumulative since boot, *t_us = esp_timer
 * microseconds at the read, and returns ESP_OK. Counters have a single
 * writer (TinyUSB task); aligned 32-bit reads never tear, so rates
 * computed between two snapshots are exact to +-1 event. Without the
 * config flag: returns ESP_ERR_NOT_SUPPORTED and writes nothing.
 */
esp_err_t uac_device_get_pull_stats(uint32_t out[4], int64_t *t_us);

#ifdef __cplusplus
}
#endif
