#include "rotary_encoder.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "rotary_encoder";

typedef struct rotary_encoder_s {
    pcnt_unit_handle_t pcnt_unit;
    pcnt_channel_handle_t chan_a;
    pcnt_channel_handle_t chan_b;
    bool pcnt_enabled;
    bool pcnt_started;
} rotary_encoder_t;

static esp_err_t rotary_encoder_create_unit(pcnt_unit_handle_t *unit, int32_t low_limit, int32_t high_limit)
{
    ESP_RETURN_ON_FALSE(unit != NULL, ESP_ERR_INVALID_ARG, TAG, "unit pointer is null");

    pcnt_unit_config_t unit_config = {
        .low_limit = low_limit,
        .high_limit = high_limit,
    };

    return pcnt_new_unit(&unit_config, unit);
}

static esp_err_t rotary_encoder_create_channels(pcnt_unit_handle_t unit, gpio_num_t pin_a, gpio_num_t pin_b,
                                                 pcnt_channel_handle_t *out_chan_a, pcnt_channel_handle_t *out_chan_b)
{
    pcnt_channel_handle_t chan_a = NULL;
    pcnt_channel_handle_t chan_b = NULL;

    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = pin_a,
        .level_gpio_num = pin_b,
    };

    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = pin_b,
        .level_gpio_num = pin_a,
    };

    esp_err_t ret = pcnt_new_channel(unit, &chan_a_config, &chan_a);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel A failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = pcnt_new_channel(unit, &chan_b_config, &chan_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_channel B failed: %s", esp_err_to_name(ret));
        pcnt_del_channel(chan_a);
        return ret;
    }

    ret = pcnt_channel_set_edge_action(chan_a,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_channel_set_edge_action A failed: %s", esp_err_to_name(ret));
        goto err_cleanup;
    }

    ret = pcnt_channel_set_level_action(chan_a,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_channel_set_level_action A failed: %s", esp_err_to_name(ret));
        goto err_cleanup;
    }

    ret = pcnt_channel_set_edge_action(chan_b,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_channel_set_edge_action B failed: %s", esp_err_to_name(ret));
        goto err_cleanup;
    }

    ret = pcnt_channel_set_level_action(chan_b,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_channel_set_level_action B failed: %s", esp_err_to_name(ret));
        goto err_cleanup;
    }

    *out_chan_a = chan_a;
    *out_chan_b = chan_b;
    return ESP_OK;

err_cleanup:
    pcnt_del_channel(chan_a);
    pcnt_del_channel(chan_b);
    return ret;
}

rotary_encoder_config_t rotary_encoder_default_config(gpio_num_t pin_a, gpio_num_t pin_b)
{
    rotary_encoder_config_t config = {
        .pin_a = pin_a,
        .pin_b = pin_b,
        .high_limit = ROTARY_ENCODER_DEFAULT_HIGH_LIMIT,
        .low_limit = ROTARY_ENCODER_DEFAULT_LOW_LIMIT,
        .glitch_filter_ns = ROTARY_ENCODER_DEFAULT_GLITCH_NS,
    };
    return config;
}

esp_err_t rotary_encoder_new_with_config(const rotary_encoder_config_t *config, rotary_encoder_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid config");
    ESP_RETURN_ON_FALSE(out_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid handle pointer");

    rotary_encoder_t *encoder = calloc(1, sizeof(rotary_encoder_t));
    ESP_RETURN_ON_FALSE(encoder, ESP_ERR_NO_MEM, TAG, "no memory for encoder");

    esp_err_t ret = ESP_FAIL;

    /* validate pins */
    if (!GPIO_IS_VALID_GPIO(config->pin_a) || !GPIO_IS_VALID_GPIO(config->pin_b)) {
        ESP_LOGE(TAG, "invalid GPIO pins: A=%d B=%d", config->pin_a, config->pin_b);
        ret = ESP_ERR_INVALID_ARG;
        goto err;
    }

    ret = rotary_encoder_create_unit(&encoder->pcnt_unit, config->low_limit, config->high_limit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcnt unit create failed: %s", esp_err_to_name(ret));
        goto err;
    }

    if (config->glitch_filter_ns > 0) {
        pcnt_glitch_filter_config_t filter_config = {
            .max_glitch_ns = config->glitch_filter_ns,
        };
        ret = pcnt_unit_set_glitch_filter(encoder->pcnt_unit, &filter_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "pcnt set glitch filter failed: %s", esp_err_to_name(ret));
            goto err;
        }
    }

    ret = rotary_encoder_create_channels(encoder->pcnt_unit, config->pin_a, config->pin_b,
                                         &encoder->chan_a, &encoder->chan_b);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "create_channels failed: %s", esp_err_to_name(ret));
        goto err;
    }

    ret = pcnt_unit_enable(encoder->pcnt_unit);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "pcnt_unit_enable failed: %s", esp_err_to_name(ret)); goto err; }
    encoder->pcnt_enabled = true;
    ret = pcnt_unit_clear_count(encoder->pcnt_unit);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "pcnt_unit_clear_count failed: %s", esp_err_to_name(ret)); goto err; }
    ret = pcnt_unit_start(encoder->pcnt_unit);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "pcnt_unit_start failed: %s", esp_err_to_name(ret)); goto err; }
    encoder->pcnt_started = true;

    *out_handle = encoder;
    return ESP_OK;

err:
    rotary_encoder_delete(encoder);
    return ret;
}

esp_err_t rotary_encoder_new(gpio_num_t pin_a, gpio_num_t pin_b, rotary_encoder_handle_t *out_handle)
{
    rotary_encoder_config_t config = rotary_encoder_default_config(pin_a, pin_b);
    return rotary_encoder_new_with_config(&config, out_handle);
}

int32_t rotary_encoder_get_count(rotary_encoder_handle_t handle)
{
    if (!handle) return 0;
    int count = 0;
    pcnt_unit_get_count(handle->pcnt_unit, &count);
    return count;
}

esp_err_t rotary_encoder_reset(rotary_encoder_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return pcnt_unit_clear_count(handle->pcnt_unit);
}

esp_err_t rotary_encoder_delete(rotary_encoder_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    if (handle->pcnt_unit) {
        if (handle->pcnt_started) {
            pcnt_unit_stop(handle->pcnt_unit);
            handle->pcnt_started = false;
        }
        if (handle->pcnt_enabled) {
            pcnt_unit_disable(handle->pcnt_unit);
            handle->pcnt_enabled = false;
        }
    }
    if (handle->chan_a) {
        pcnt_del_channel(handle->chan_a);
    }
    if (handle->chan_b) {
        pcnt_del_channel(handle->chan_b);
    }
    if (handle->pcnt_unit) {
        pcnt_del_unit(handle->pcnt_unit);
    }
    free(handle);
    return ESP_OK;
}
