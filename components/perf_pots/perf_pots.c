/*
 * perf_pots — three analog performance knobs (filter cutoff / master volume /
 * FX) sampled from the ESP32-S3 ADC1 and pushed into the synth.
 *
 * Wiring comes from the "Performance Pots" Kconfig menu (this component's
 * Kconfig); each role defaults to -1 (disabled), so the upstream devkit is
 * unaffected. This board wires cutoff / FX / volume to its three spare ADC1
 * pads.
 *
 * A low-priority task scans at 20 Hz, averages + EMA-smooths each channel and
 * pushes a target only when its smoothed value crosses a deadband, so the
 * store and AMY are only touched on real pot movement. On task start it only
 * samples every pot once to seed the baseline; nothing is pushed until the
 * player actually moves a knob, so a power-on demo keeps AMY's default sound
 * regardless of where the knobs happen to sit.
 *
 * Cutoff is a tone knob: it sweeps every melodic layer's low-pass filter AND
 * the live-play slot (so it stays audible however you play - even a drum-only
 * song with a keyboard over DIN), and resonance rides up as the cutoff
 * closes, giving the sweep an unmistakable "wow". Volume drives AMY's global
 * master (unity at 50%). The FX knob is a space knob: global echo + reverb
 * send, audible on every voice the moment you turn it - sequencer, live slot
 * and demos all route through AMY's effect bus.
 */

#include "perf_pots.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdint.h>

#include "sequencer_core.h"
#include "amy.h"            /* amy_event / FILTER_* / COEF_* */
#include "amy_helpers.h"    /* amy_helpers_event_begin/send */
#include "amy_fx.h"
#include "synth_slots.h"    /* LIVE_SYNTH */

static const char *TAG = "perf_pots";

#define POT_AVG_SAMPLES      8u
#define POT_DEADBAND         0.03f
#define POT_SMOOTH_ALPHA     0.25f
#define POT_PUSH_PERIOD_MS   50
#define POT_TASK_STACK_WORDS 3072

enum { P_CUTOFF = 0, P_VOLUME, P_FX, P_COUNT };
#define POT_MV_FULL_SCALE 3300

/* Cutoff sweep range, log-mapped. The seq filter setter clamps to 65..8000 Hz;
 * start at 80 so the closed end stays audible, end at the setter's max. */
#define POT_CUTOFF_MIN_HZ   80.0f
#define POT_CUTOFF_MAX_HZ   8000.0f
/* Resonance rides 0.51 (flat) .. 4.0 (ringing) as the knob closes the
 * cutoff, so the sweep reads as a filter "wow" instead of a mild bright
 * change; capped well under AMY's 8.0 to avoid self-oscillation. */
#define POT_RES_MIN   0.51f
#define POT_RES_SPAN  3.49f

static const int s_pot_pins[P_COUNT] = {
    CONFIG_AMYSYNTH_POT_CUTOFF_GPIO,
    CONFIG_AMYSYNTH_POT_VOLUME_GPIO,
    CONFIG_AMYSYNTH_POT_FX_GPIO,
};

static bool s_pot_enabled[P_COUNT];
static int16_t s_pot_channel[P_COUNT];
static float s_value[P_COUNT];        /* smoothed 0..1 */
static float s_last_pushed[P_COUNT];

static TaskHandle_t s_task = NULL;
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;

static void pot_push(int idx, float v)
{
    switch (idx) {
    case P_CUTOFF: {
        /* Tone knob: sweep every melodic layer's low-pass filter AND the
         * live-play slot, so the knob always audibly tracks the playing
         * content (a boot pass with a drum layer armed, or a keyboard over
         * DIN, no longer leaves it silent). Resonance closes with the filter
         * for an audible filter sweep on any patch. */
        float cutoff = POT_CUTOFF_MIN_HZ *
                       powf(POT_CUTOFF_MAX_HZ / POT_CUTOFF_MIN_HZ, v);
        float res    = POT_RES_MIN + POT_RES_SPAN * powf(v, 1.5f);

        uint8_t n = sequencer_core_get_num_layers();
        for (uint8_t li = 0; li < n; li++) {
            if (sequencer_core_get_layer_type(li) != SEQ_LAYER_MELODIC)
                continue;
            for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
                seq_filter_t f;
                if (!sequencer_core_get_melodic_filter(li, t, &f)) continue;
                f.cutoff_hz = cutoff;
                f.resonance = res;
                f.enabled   = true;   /* first known turn engages the filter */
                sequencer_core_set_melodic_filter(li, t, &f);
            }
        }

        /* Live-play slot (DIN/BLE/USB keyboard): independent of any melodic
         * layer. Pushing FILTER_LPF24 overrides the patch for the sounding
         * voice; the knob re-asserts on every move. */
        amy_event *e = amy_helpers_event_begin();
        e->synth                    = LIVE_SYNTH;
        e->filter_type              = FILTER_LPF24;
        e->filter_freq_coefs[COEF_CONST] = cutoff;
        e->resonance                = res;
        amy_helpers_event_send(e);

        ESP_LOGD(TAG, "tone sweep -> %.0f Hz, res %.2f (layers=%u)", cutoff, res, n);
        break;
    }
    case P_VOLUME:
        amy_fx_set_master_volume(v * 2.0f);   /* unity at v=0.5 */
        ESP_LOGD(TAG, "master volume -> %.2f", v * 2.0f);
        break;
    case P_FX:
        /* Space knob: global echo + reverb send amount on every voice - the
         * sequencer, the live slot and the built-in demos all route through
         * AMY's effect bus, so turning it up is unmistakable regardless of
         * what is playing (unlike a per-voice patch/CC mapping). */
        {
            uint8_t lvl = (uint8_t)(v * 100.0f);
            s_fx.echo_level   = lvl;
            s_fx.reverb_level = lvl;
            fx_push_echo();
            fx_push_reverb();
            ESP_LOGD(TAG, "space send -> %u%%", lvl);
        }
        break;
    }
}

/* Read one pot (averaged), return its calibrated 0..1 position. */
static float pot_read(int idx)
{
    int raw = 0;
    uint32_t acc = 0;
    for (uint32_t k = 0; k < POT_AVG_SAMPLES; k++) {
        if (adc_oneshot_read(s_adc, s_pot_channel[idx], &raw) == ESP_OK)
            acc += (uint32_t)raw;
    }
    raw = (int)(acc / POT_AVG_SAMPLES);

    int mv = raw;   /* fallback: 12-bit raw ≈ linear in V */
    if (s_cali != NULL) {
        int m = 0;
        if (adc_cali_raw_to_voltage(s_cali, raw, &m) == ESP_OK) mv = m;
    }
    float v = (float)mv / POT_MV_FULL_SCALE;
    if (v < 0.0f) v = 0.0f;
    else if (v > 1.0f) v = 1.0f;
    return v;
}

static void pot_task(void *arg)
{
    (void)arg;

    /* Boot pass: sample every pot once so the smoothing state and the
     * deadband baseline sit at the physical knob position, but do NOT push
     * anything. Pushing here would engage the filter / FX send / a master
     * attenuation straight from the knob positions, which dresses the boot
     * demo in whatever the knobs happen to be set to - users read that as
     * "not the original sound". The pots only act once actually moved. */
    for (int idx = 0; idx < P_COUNT; idx++) {
        if (!s_pot_enabled[idx]) continue;
        float v = pot_read(idx);
        s_value[idx]       = v;
        s_last_pushed[idx] = v;
    }

    while (s_task != NULL) {
        for (int idx = 0; idx < P_COUNT; idx++) {
            if (!s_pot_enabled[idx]) continue;

            float v = pot_read(idx);
            s_value[idx] = POT_SMOOTH_ALPHA * v +
                           (1.0f - POT_SMOOTH_ALPHA) * s_value[idx];
            float sm = s_value[idx];

            if (fabsf(sm - s_last_pushed[idx]) > POT_DEADBAND) {
                s_last_pushed[idx] = sm;
                pot_push(idx, sm);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POT_PUSH_PERIOD_MS));
    }
    vTaskDelete(NULL);
}

static void adc_calibration_init(void)
{
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "ADC calibration unavailable; raw scaling fallback");
    }
}

esp_err_t perf_pots_init(void)
{
    int enabled = 0;
    for (int i = 0; i < P_COUNT; i++) {
        s_pot_enabled[i] = false;
        if (s_pot_pins[i] >= 0) {
            if (s_pot_pins[i] < 1 || s_pot_pins[i] > 10) {
                ESP_LOGE(TAG, "pot pin %d out of ADC1 range (GPIO1..10)",
                         s_pot_pins[i]);
                continue;
            }
            s_pot_enabled[i]  = true;
            s_pot_channel[i]  = (int16_t)(s_pot_pins[i] - 1); /* ADC1 ch = pin-1 */
            s_value[i]        = 0.0f;
            s_last_pushed[i]  = -1.0f;   /* force the first push */
            enabled++;
        }
    }
    if (enabled == 0) {
        ESP_LOGI(TAG, "no pots configured - disabled");
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &s_adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to init ADC1: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    for (int i = 0; i < P_COUNT; i++) {
        if (!s_pot_enabled[i]) continue;
        ret = adc_oneshot_config_channel(s_adc, s_pot_channel[i], &chan_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to config ADC1 ch%d: %s",
                     s_pot_channel[i], esp_err_to_name(ret));
            adc_oneshot_del_unit(s_adc);
            s_adc = NULL;
            return ret;
        }
    }

    adc_calibration_init();

    ret = xTaskCreate(pot_task, "perf_pots", POT_TASK_STACK_WORDS, NULL, 1,
                      &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to create pot task");
        if (s_cali != NULL) {
            adc_cali_delete_scheme_curve_fitting(s_cali);
            s_cali = NULL;
        }
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "pots on ADC1: cutoff=GPIO%d volume=GPIO%d fx=GPIO%d",
             s_pot_enabled[P_CUTOFF] ? CONFIG_AMYSYNTH_POT_CUTOFF_GPIO : -1,
             s_pot_enabled[P_VOLUME]  ? CONFIG_AMYSYNTH_POT_VOLUME_GPIO  : -1,
             s_pot_enabled[P_FX]      ? CONFIG_AMYSYNTH_POT_FX_GPIO      : -1);
    return ESP_OK;
}

esp_err_t perf_pots_deinit(void)
{
    if (s_task != NULL) {
        TaskHandle_t t = s_task;
        s_task = NULL;
        vTaskDelete(t);
    }
    if (s_cali != NULL) {
        adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_adc != NULL) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
    return ESP_OK;
}