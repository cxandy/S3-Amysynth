// Reporter registry. Deliberately allocation-free: a fixed table sized well
// above the number of diagnostics the firmware has, so registration cannot
// fail in practice and cannot fragment the internal heap it is meant to be
// observing.
#include "diag_report.h"
#include "render_stats.h"
#include "rtos_stats.h"
#include "amy_profile.h"
#include "esp_log.h"

static const char *TAG = "diag";

#define DIAG_MAX_REPORTERS 12

typedef struct {
    const char       *name;
    diag_reporter_fn  fn;
} reporter_slot_t;

static reporter_slot_t s_reporters[DIAG_MAX_REPORTERS];
static uint8_t s_num_reporters = 0;

bool diag_register_reporter(const char *name, diag_reporter_fn fn)
{
    if (fn == NULL) return false;
    if (s_num_reporters >= DIAG_MAX_REPORTERS) {
        ESP_LOGW(TAG, "reporter table full (%d), dropping '%s'",
                 DIAG_MAX_REPORTERS, name ? name : "?");
        return false;
    }
    s_reporters[s_num_reporters].name = name ? name : "?";
    s_reporters[s_num_reporters].fn   = fn;
    s_num_reporters++;
    return true;
}

void diag_init(void)
{
    // Each of these compiles to a no-op stub when its Kconfig gate is off, so
    // registering unconditionally costs one table slot and nothing else. The
    // order here is the order the report lines appear in the log.
#if CONFIG_AMYSYNTH_RENDER_STATS
    diag_register_reporter("render", render_stats_report);
#endif
#if CONFIG_AMYSYNTH_RTOS_STATS
    diag_register_reporter("rtos", rtos_stats_report);
#endif
#if defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)
    diag_register_reporter("amy-profile", amy_profile_report);
#endif

    if (s_num_reporters == 0) {
        ESP_LOGD(TAG, "no diagnostics enabled");
        return;
    }
    for (uint8_t i = 0; i < s_num_reporters; i++) {
        ESP_LOGI(TAG, "reporter %u: %s", i, s_reporters[i].name);
    }
}

void diag_report_all(void)
{
    for (uint8_t i = 0; i < s_num_reporters; i++) {
        s_reporters[i].fn();
    }
}

uint32_t diag_report_interval_ms(void)
{
#if CONFIG_AMYSYNTH_RTOS_STATS
    return CONFIG_AMYSYNTH_RTOS_STATS_PERIOD_MS;
#elif defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)
    return CONFIG_AMY_PROFILE_INTERVAL_MS;
#else
    return 5000;
#endif
}
