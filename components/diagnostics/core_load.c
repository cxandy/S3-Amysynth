// Per-core busy % from the IDLE task run-time counters (contract in
// core_load.h). Same mechanism as the rtos_stats dump, minus the per-task
// table: one uxTaskGetSystemState() snapshot, idle-delta vs esp_timer wall
// time. Keying idle tasks off their "IDLE0"/"IDLE1" names (IDF kernel) avoids
// needing TaskStatus_t.xCoreID and its extra config symbol.
#include "core_load.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Snapshot capacity: the firmware runs ~15 tasks; headroom for growth.
// uxTaskGetSystemState() returns 0 when the array is too small, so an
// overflow degrades to "no data this call", never a truncated snapshot.
#define CORE_LOAD_MAX_TASKS 24

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
    static TaskStatus_t s_tasks[CORE_LOAD_MAX_TASKS];
    static uint32_t s_prev_idle_us[CORE_LOAD_NUM_CORES];
    static int64_t s_prev_wall_us;
    static bool s_have_baseline;

    UBaseType_t n = uxTaskGetSystemState(s_tasks, CORE_LOAD_MAX_TASKS, NULL);
    int64_t now_us = esp_timer_get_time();
    if (n == 0) {
        return false;
    }

    uint32_t idle_now[CORE_LOAD_NUM_CORES] = {0};
    unsigned found = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        const char *name = s_tasks[i].pcTaskName;
        if (strncmp(name, "IDLE", 4) == 0 &&
            name[4] >= '0' && name[4] < '0' + CORE_LOAD_NUM_CORES &&
            name[5] == '\0') {
            // Truncate to u32 regardless of configRUN_TIME_COUNTER_TYPE: the
            // delta below is modular u32 arithmetic, correct across one wrap.
            idle_now[name[4] - '0'] = (uint32_t)s_tasks[i].ulRunTimeCounter;
            found++;
        }
    }
    if (found != CORE_LOAD_NUM_CORES) {
        return false;
    }

    bool have_result = false;
    int64_t wall_delta = now_us - s_prev_wall_us;
    if (s_have_baseline && wall_delta > 0) {
        for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
            uint64_t idle_us = (uint32_t)(idle_now[core] - s_prev_idle_us[core]);
            if (idle_us >= (uint64_t)wall_delta) {
                busy_pct[core] = 0; // clamp sampling skew
            } else {
                busy_pct[core] =
                    (uint8_t)(100u - (unsigned)((idle_us * 100u) / (uint64_t)wall_delta));
            }
        }
        have_result = true;
        for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
            s_last_pct[core] = busy_pct[core];
        }
        s_last_valid = true;
    }

    for (int core = 0; core < CORE_LOAD_NUM_CORES; core++) {
        s_prev_idle_us[core] = idle_now[core];
    }
    s_prev_wall_us = now_us;
    s_have_baseline = true;
    return have_result;
}

#endif // CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
