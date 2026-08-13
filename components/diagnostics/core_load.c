// Per-core busy % from idle-hook invocation counting (contract and the
// history of why this must not use uxTaskGetSystemState(): core_load.h).
#include "core_load.h"

#include "freertos/FreeRTOS.h"
#include "esp_freertos_hooks.h"
#include "esp_timer.h"

// One counter per core, bumped by that core's IDLE task via the registered
// hook. Single writer per counter (the owning core's idle task); aligned
// 32-bit reads from the sampler never tear. Wraps are handled by modular
// u32 deltas.
static volatile uint32_t s_idle_ticks[CORE_LOAD_NUM_CORES];

static bool idle_hook_core0(void)
{
    s_idle_ticks[0]++;
    return true; // no pending work - never veto the idle task's WAITI
}

#if CORE_LOAD_NUM_CORES > 1
static bool idle_hook_core1(void)
{
    s_idle_ticks[1]++;
    return true;
}
#endif

// Last successful sample, republished for lock-free readers (contract in
// core_load.h). Per-byte stores are atomic on Xtensa; a reader racing the
// writer sees at worst one core's value from the previous sample.
static volatile uint8_t s_last_pct[CORE_LOAD_NUM_CORES];
static volatile bool    s_last_valid;

bool core_load_last(uint8_t busy_pct[CORE_LOAD_NUM_CORES])
{
    if (!s_last_valid) return false;
    for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
        busy_pct[core] = s_last_pct[core];
    }
    return true;
}

bool core_load_sample(uint8_t busy_pct[CORE_LOAD_NUM_CORES])
{
    static bool     s_hooks_ok;
    static uint32_t s_prev_ticks[CORE_LOAD_NUM_CORES];
    static int64_t  s_prev_wall_us;
    static uint64_t s_max_rate[CORE_LOAD_NUM_CORES]; // ticks/s, best observed
    static bool     s_have_baseline;

    if (!s_hooks_ok) {
        if (esp_register_freertos_idle_hook_for_cpu(idle_hook_core0, 0) != ESP_OK) {
            return false;
        }
#if CORE_LOAD_NUM_CORES > 1
        if (esp_register_freertos_idle_hook_for_cpu(idle_hook_core1, 1) != ESP_OK) {
            esp_deregister_freertos_idle_hook_for_cpu(idle_hook_core0, 0);
            return false;
        }
#endif
        s_hooks_ok = true;
    }

    uint32_t ticks_now[CORE_LOAD_NUM_CORES];
    for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
        ticks_now[core] = s_idle_ticks[core];
    }
    int64_t now_us = esp_timer_get_time();

    bool have_result = false;
    int64_t wall_delta = now_us - s_prev_wall_us;
    if (s_have_baseline && wall_delta > 0) {
        for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
            uint32_t dticks = ticks_now[core] - s_prev_ticks[core]; // modular
            uint64_t rate = ((uint64_t)dticks * 1000000u) / (uint64_t)wall_delta;
            if (rate > s_max_rate[core]) {
                s_max_rate[core] = rate;
            }
            if (s_max_rate[core] == 0) {
                busy_pct[core] = 100; // core has never idled at all
            } else {
                uint64_t idle_pct = (rate * 100u) / s_max_rate[core];
                busy_pct[core] = (uint8_t)(100u - (idle_pct > 100u ? 100u : idle_pct));
            }
        }
        have_result = true;
        for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
            s_last_pct[core] = busy_pct[core];
        }
        s_last_valid = true;
    }

    for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
        s_prev_ticks[core] = ticks_now[core];
    }
    s_prev_wall_us = now_us;
    s_have_baseline = true;
    return have_result;
}
