/*
 * i2s_dac — PCM5102 standalone audio output over I2S (see i2s_dac.h).
 */

#include "i2s_dac.h"
#include "sdkconfig.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "i2s_dac";

static i2s_chan_handle_t s_tx = NULL;
static bool s_ready = false;

esp_err_t i2s_dac_init(uint32_t sample_rate_hz, size_t frames_per_block)
{
    ESP_RETURN_ON_FALSE(s_tx == NULL, ESP_ERR_INVALID_STATE, TAG, "already initialized");

    const bool have_mclk = CONFIG_AMYSYNTH_I2S_DAC_MCLK_GPIO >= 0;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_AMYSYNTH_I2S_DAC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = (int)frames_per_block;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG,
                        "failed to create I2S TX channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = have_mclk ? CONFIG_AMYSYNTH_I2S_DAC_MCLK_GPIO : I2S_GPIO_UNUSED,
            .bclk = CONFIG_AMYSYNTH_I2S_DAC_BCLK_GPIO,  // BCK
            .ws   = CONFIG_AMYSYNTH_I2S_DAC_WS_GPIO,    // LRCK
            .dout = CONFIG_AMYSYNTH_I2S_DAC_DOUT_GPIO,  // DIN
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    /* PCM5102 in auto-clock mode derives SCK from BCK when no MCLK line is
     * routed (modules without an SCK connection). Nothing is output on MCLK
     * unless AMYSYNTH_I2S_DAC_MCLK_GPIO is set to a real pin; if the DAC
     * needs SCK, wire it there and set that option. */
    if (!have_mclk) {
        std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
    }

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG,
                        "failed to init I2S standard mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "failed to enable I2S TX channel");

    s_ready = true;
    ESP_LOGI(TAG, "PCM5102 I2S TX ready: %lu Hz, %lu frames/block, pins "
                  "bclk=%d ws=%d dout=%d mclk=%s",
             (unsigned long)sample_rate_hz, (unsigned long)frames_per_block,
             CONFIG_AMYSYNTH_I2S_DAC_BCLK_GPIO, CONFIG_AMYSYNTH_I2S_DAC_WS_GPIO,
             CONFIG_AMYSYNTH_I2S_DAC_DOUT_GPIO,
             have_mclk ? "configured" : "unused (PCM5102 auto-clock)");
    return ESP_OK;
}

esp_err_t i2s_dac_write_stereo(const int16_t *frames, size_t frame_count)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t bytes = frame_count * 2 * sizeof(int16_t);
    size_t written = 0;
    /* Timeout longer than one block period: the DMA queue drains one block
     * per render tick, so a write only blocks when render has drifted ahead.
     * A full queue drops the block to keep AMY phase-aligned. */
    esp_err_t ret = i2s_channel_write(s_tx, frames, bytes, &written,
                                      pdMS_TO_TICKS(15));
    if (ret != ESP_OK || written != bytes) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool i2s_dac_ready(void)
{
    return s_ready;
}