#include "amy_profile.h"

#if defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)

#include <stdint.h>
#include "amy.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "amy_profile";

// Coarse mode reads the timer ~8 times per block (4 START/STOP pairs); full mode
// costs 2*sum(tag calls). Both are worth knowing in absolute terms before
// reading a dump, hence the one-off calibration.
void amy_profile_overhead_selftest(void)
{
    const int N = 20000;
    volatile int64_t sink = 0;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; ++i) sink ^= esp_timer_get_time();
    int64_t t1 = esp_timer_get_time();
    (void)sink;
    double ns_per_read = ((double)(t1 - t0) * 1000.0) / (double)N;
    double coarse_us_per_block = (ns_per_read * 8.0) / 1000.0;
    const uint32_t block_us = (uint32_t)(((uint64_t)AMY_BLOCK_SIZE * 1000000ULL)
                                         / (uint64_t)AMY_SAMPLE_RATE);
    ESP_LOGW(TAG,
        "[amy-profile] esp_timer_get_time ~%.1f ns/read | coarse overhead ~%.2f us/block "
        "(~%.3f%% of %u us budget) | full overhead = 2*sum(tag.calls)/block * read_cost",
        ns_per_read, coarse_us_per_block,
        (coarse_us_per_block / (double)block_us) * 100.0, block_us);
}

void amy_profile_report(void)
{
    amy_profiles_print();
}

#endif
