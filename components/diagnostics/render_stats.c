// Per-block render-time statistics (cycle-count histogram + worst-block
// snapshot). The render budget is one block period: 256 samples @ 48 kHz =
// 5.333 ms; everything here is denominated in CPU cycles of that budget.
//
// Concurrency: the window struct is written only by the render task (Core 1)
// and read only by the idle loop (Core 0), published through a seqlock: the
// writer bumps `version` to odd before mutating and back to even after; the
// reader retries until it sees the same even version on both sides of its
// copy. Window resets are requested by the reader (s_reset_req) but executed
// by the writer at the next block boundary, so only one core ever writes the
// window. No locks, no allocation, nothing blocking on the render side.
#include "render_stats.h"

#if CONFIG_AMYSYNTH_RENDER_STATS

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_cpu.h"
#include "esp_log.h"
#include "amy.h"

static const char *TAG = "render_stats";

// 240 MHz * 256 / 48000 = 1,280,000 cycles per block.
#define BUDGET_CYCLES ((uint32_t)((uint64_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ \
                                  * 1000000ULL * AMY_BLOCK_SIZE / AMY_SAMPLE_RATE))
#define HIST_BUCKETS 16u
// Linear buckets of budget/16 = 6.25% of budget each; the last bucket also
// absorbs everything at or over budget.
#define BUCKET_CYCLES (BUDGET_CYCLES / HIST_BUCKETS)
#define SPIKE_CYCLES ((uint32_t)((uint64_t)BUDGET_CYCLES \
                                 * CONFIG_AMYSYNTH_RENDER_STATS_SPIKE_PCT / 100u))

// Compiler barrier: keeps the window mutations from being reordered across
// the seqlock version bumps. The S3's cores don't reorder stores, so no
// hardware fence is needed.
#define BARRIER() __asm__ __volatile__("" ::: "memory")

typedef struct {
    volatile uint32_t version;   // seqlock: odd while the writer is mid-update
    uint32_t count;
    uint32_t min_cycles;
    uint64_t sum_cycles;
    uint32_t spikes;             // blocks at/above the spike threshold
    uint32_t over_budget;        // blocks that exceeded the full block period
    uint32_t hist[HIST_BUCKETS];
    // Worst block since the last reset (doubles as the window max):
    uint32_t worst_cycles;
    uint32_t worst_block;        // amy_global.total_blocks at capture
    uint16_t worst_qsize;        // delta-queue depth at capture
    uint8_t  worst_tick;         // a sequencer tick ran inside that block
} stats_window_t;

static stats_window_t s_w = { .min_cycles = UINT32_MAX };

static volatile uint32_t s_reset_req = 0;  // reader increments to request
static uint32_t s_reset_ack = 0;           // writer-private
static uint32_t s_block_t0;                // writer-private

void render_stats_block_begin(void)
{
    if (s_reset_ack != s_reset_req) {
        s_reset_ack = s_reset_req;
        uint32_t v = s_w.version + 1;      // -> odd
        s_w.version = v;
        BARRIER();
        memset((char *)&s_w + sizeof(s_w.version), 0,
               sizeof(s_w) - sizeof(s_w.version));
        s_w.min_cycles = UINT32_MAX;
        BARRIER();
        s_w.version = v + 1;               // -> even
    }
    s_block_t0 = esp_cpu_get_cycle_count();
}

void render_stats_block_end(bool seq_tick_coincident)
{
    // Unsigned subtraction stays correct across the ~17.9 s ccount wrap.
    uint32_t cycles = esp_cpu_get_cycle_count() - s_block_t0;

    s_w.version++;                         // -> odd
    BARRIER();
    s_w.count++;
    s_w.sum_cycles += cycles;
    if (cycles < s_w.min_cycles) s_w.min_cycles = cycles;
    uint32_t b = cycles / BUCKET_CYCLES;
    s_w.hist[b >= HIST_BUCKETS ? HIST_BUCKETS - 1u : b]++;
    if (cycles >= SPIKE_CYCLES) s_w.spikes++;
    if (cycles > BUDGET_CYCLES) s_w.over_budget++;
    if (cycles > s_w.worst_cycles) {
        s_w.worst_cycles = cycles;
        s_w.worst_block = amy_global.total_blocks;
        s_w.worst_qsize = amy_global.delta_qsize;
        s_w.worst_tick = seq_tick_coincident ? 1 : 0;
    }
    BARRIER();
    s_w.version++;                         // -> even
}

void render_stats_report(void)
{
    stats_window_t copy = { 0 };
    for (int tries = 0; tries < 8; tries++) {
        uint32_t v1 = s_w.version;
        if (v1 & 1u) continue;
        BARRIER();
        memcpy(&copy, (const void *)&s_w, sizeof(copy));
        BARRIER();
        if (s_w.version == v1) break;      // stable copy; else retry
    }
    if (copy.count == 0) {
        ESP_LOGI(TAG, "no blocks in window");
        return;
    }

    const uint32_t mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t mean = (uint32_t)(copy.sum_cycles / copy.count);

    // p99 from the histogram: first bucket where the cumulative count crosses
    // 99%, reported as that bucket's upper edge.
    uint32_t p99_target = copy.count - copy.count / 100u;
    uint32_t cum = 0, p99_bucket = HIST_BUCKETS - 1u;
    for (uint32_t i = 0; i < HIST_BUCKETS; i++) {
        cum += copy.hist[i];
        if (cum >= p99_target) { p99_bucket = i; break; }
    }
    uint32_t p99_cycles = (p99_bucket + 1u) * BUCKET_CYCLES;

    ESP_LOGI(TAG,
             "blk us min/mean/p99/max=%" PRIu32 "/%" PRIu32 "/<=%" PRIu32 "/%" PRIu32
             " budget-peak=%" PRIu32 "%% spikes>=%d%%=%" PRIu32
             " over=%" PRIu32 " n=%" PRIu32,
             copy.min_cycles / mhz, mean / mhz, p99_cycles / mhz,
             copy.worst_cycles / mhz,
             (uint32_t)((uint64_t)copy.worst_cycles * 100u / BUDGET_CYCLES),
             CONFIG_AMYSYNTH_RENDER_STATS_SPIKE_PCT, copy.spikes,
             copy.over_budget, copy.count);
    ESP_LOGI(TAG,
             "worst: blk=%" PRIu32 " qsize=%u tick=%u",
             copy.worst_block, copy.worst_qsize, copy.worst_tick);

    // One histogram line, 6.25%-of-budget buckets, low to high.
    char hist[HIST_BUCKETS * 11 + 1];
    size_t off = 0;
    for (uint32_t i = 0; i < HIST_BUCKETS; i++) {
        off += (size_t)snprintf(hist + off, sizeof(hist) - off, "%s%" PRIu32,
                                i ? " " : "", copy.hist[i]);
        if (off >= sizeof(hist) - 1) break;
    }
    ESP_LOGI(TAG, "hist(6.25%%/bucket)=[%s]", hist);

    s_reset_req++;   // writer zeroes the window at its next block boundary
}

#endif // CONFIG_AMYSYNTH_RENDER_STATS
