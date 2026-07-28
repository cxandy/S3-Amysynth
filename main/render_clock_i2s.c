#include "sdkconfig.h"

// Alternate render_clock.h backend, active only when
// CONFIG_RENDER_CLOCK_I2S_ENABLE=y (default off; render_clock.c is the default
// GPTimer backend).
#if CONFIG_RENDER_CLOCK_I2S_ENABLE

#include "render_clock.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "render_clock_i2s";

// Stereo 16-bit frames, matching AMY_NCHANS / the USB output format. Nothing
// is wired to the I2S output (no GPIO assigned below) - its only job is to
// drain its TX DMA queue on schedule, so the buffer stays silent.
#define RENDER_CLOCK_I2S_CHANNELS 2
#define RENDER_CLOCK_I2S_BYTES_PER_SAMPLE 2

static i2s_chan_handle_t s_tx_handle = NULL;
static uint8_t *s_silence_block = NULL;
static size_t s_silence_block_bytes = 0;

esp_err_t render_clock_start(uint32_t block_frames, uint32_t sample_rate_hz)
{
    if (s_tx_handle != NULL) {
        return ESP_OK;  // already started
    }

    s_silence_block_bytes =
        (size_t)block_frames * RENDER_CLOCK_I2S_CHANNELS * RENDER_CLOCK_I2S_BYTES_PER_SAMPLE;
    // One-time startup alloc. DMA-capable internal RAM is required by the
    // GDMA-backed I2S TX descriptors; ~1 KiB at the default 256-frame block.
    s_silence_block = heap_caps_calloc(1, s_silence_block_bytes,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_silence_block == NULL) {
        ESP_LOGE(TAG, "silence buffer alloc failed (%u bytes)",
                 (unsigned)s_silence_block_bytes);
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    // Buffer depth = render look-ahead: with the default 2, the render task
    // can run 1 block period ahead of the drain before i2s_channel_write()
    // blocks it, vs. zero slack on the GPTimer path.
    chan_cfg.dma_desc_num = CONFIG_RENDER_CLOCK_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = block_frames;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        goto fail_no_channel;
    }

    // Every GPIO unassigned: no pin mux change, no physical signal. In master
    // role the internal clock divider free-runs and paces the DMA queue
    // regardless of routing - the only property this backend depends on.
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    err = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        goto fail_channel;
    }

    err = i2s_channel_enable(s_tx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        goto fail_channel;
    }

    ESP_LOGI(TAG,
             "render master clock started: I2S TX DMA pacing, %u frames/block @ %u Hz, "
             "%u DMA buffers, no GPIO assigned, on core %d",
             (unsigned)block_frames, (unsigned)sample_rate_hz,
             (unsigned)CONFIG_RENDER_CLOCK_I2S_DMA_DESC_NUM, xPortGetCoreID());
    return ESP_OK;

fail_channel:
    i2s_del_channel(s_tx_handle);
    s_tx_handle = NULL;
fail_no_channel:
    heap_caps_free(s_silence_block);
    s_silence_block = NULL;
    s_silence_block_bytes = 0;
    return err;
}

uint32_t render_clock_wait(void)
{
    // The blocking write is the clock: it returns once the TX DMA queue has
    // room, i.e. a queued block finished transmitting. No accumulated-tick
    // count exists here, so this backend always returns 1 (see render_clock.h).
    size_t written = 0;
    esp_err_t err = i2s_channel_write(s_tx_handle, s_silence_block, s_silence_block_bytes,
                                       &written, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
    }
    return 1;
}

void render_clock_stop(void)
{
    if (s_tx_handle == NULL) {
        return;
    }
    i2s_channel_disable(s_tx_handle);
    i2s_del_channel(s_tx_handle);
    s_tx_handle = NULL;
    heap_caps_free(s_silence_block);
    s_silence_block = NULL;
    s_silence_block_bytes = 0;
}

#endif  // CONFIG_RENDER_CLOCK_I2S_ENABLE
