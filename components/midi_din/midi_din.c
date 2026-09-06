/*
 * midi_din: DIN-5 MIDI input on a spare UART.
 *
 * Wiring comes from AMY's UART MIDI config (main.c): when the app sets
 * amy_cfg.midi_in to a real pin, esp_init_midi() (called from amy_start)
 * installs the UART driver and routes that GPIO to the UART's RX input. This
 * component does not touch the UART - it only drains the RX ring, so the
 * pattern is: main configures AMY first, then midi_din_init() starts the read
 * task. With both BLE and DIN feeding midi_core, the parser's spinlock keeps
 * the shared running-status state coherent.
 *
 * No-op when CONFIG_AMYSYNTH_MIDI_IN_GPIO is -1.
 */

#include "midi_din.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "midi_core.h"

static const char *TAG = "midi_din";

#define MIDI_DIN_READ_MS     5
#define MIDI_DIN_TASK_STACK_WORDS 2048
#define MIDI_DIN_TASK_PRIO   5

static void midi_din_task(void *arg)
{
    (void)arg;
    uint8_t buf[32];
    for (;;) {
        int n = uart_read_bytes(UART_NUM_1, buf, (int)sizeof(buf),
                                pdMS_TO_TICKS(MIDI_DIN_READ_MS));
        if (n > 0) {
            midi_core_feed(buf, (size_t)n);
        } else if (n < 0) {
            /* Driver absent (misordered init) or RX closed: don't spin. */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

esp_err_t midi_din_init(void)
{
#if CONFIG_AMYSYNTH_MIDI_IN_GPIO < 0
    ESP_LOGI(TAG, "disabled (pin -1)");
    return ESP_OK;
#else
    if (!uart_is_driver_installed(UART_NUM_1)) {
        ESP_LOGE(TAG, "UART_NUM_1 driver not installed; run after amy_start");
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(midi_din_task, "midi_din",
                                            MIDI_DIN_TASK_STACK_WORDS, NULL,
                                            MIDI_DIN_TASK_PRIO, NULL, 0);
    if (ok != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "DIN MIDI in on GPIO%d -> live_play voice", CONFIG_AMYSYNTH_MIDI_IN_GPIO);
    return ESP_OK;
#endif
}