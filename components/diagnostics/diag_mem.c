#include "diag_mem.h"

#include <inttypes.h>
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "diag_mem";

#define DIAG_MEM_MAX_ENTRIES 16

typedef struct {
    const char *name;
    const void *ptr;
    size_t      bytes;
} mem_entry_t;

static mem_entry_t s_entries[DIAG_MEM_MAX_ENTRIES];
static uint8_t s_num_entries = 0;

const char *diag_mem_region(const void *p)
{
    if (p == NULL)                        return "(null)";
    // Order matters: the data-RAM tests are the cheap common case, and
    // esp_ptr_executable() must come last because it is the broadest - it
    // accepts IRAM and RTC IRAM as well as the flash text window.
    if (esp_ptr_in_dram(p))               return "DRAM";
    if (esp_ptr_external_ram(p))          return "PSRAM";
    if (esp_ptr_in_iram(p))               return "IRAM";
    if (esp_ptr_in_drom(p))               return "DROM/flash";
    if (esp_ptr_in_rtc_dram_fast(p))      return "RTC-DRAM";
    if (esp_ptr_in_rtc_iram_fast(p))      return "RTC-IRAM";
    if (esp_ptr_in_rtc_slow(p))           return "RTC-slow";
    if (esp_ptr_executable(p))            return "text/flash-XIP";
    return "?";
}

bool diag_mem_track(const char *name, const void *p, size_t bytes)
{
    if (s_num_entries >= DIAG_MEM_MAX_ENTRIES) {
        ESP_LOGW(TAG, "table full (%d), not tracking '%s'",
                 DIAG_MEM_MAX_ENTRIES, name ? name : "?");
        return false;
    }
    s_entries[s_num_entries].name  = name ? name : "?";
    s_entries[s_num_entries].ptr   = p;
    s_entries[s_num_entries].bytes = bytes;
    s_num_entries++;
    return true;
}

void diag_mem_report(void)
{
    ESP_LOGI(TAG, "placement map: name              addr       region        size");
    for (uint8_t i = 0; i < s_num_entries; i++) {
        const mem_entry_t *e = &s_entries[i];
        if (e->ptr == NULL) {
            // A NULL entry means the allocation failed and the owner degraded
            // gracefully - worth a warning, since the feature is silently off.
            ESP_LOGW(TAG, "  %-16s (failed - allocation returned NULL)", e->name);
            continue;
        }
        ESP_LOGI(TAG, "  %-16s %p %-13s %6u KB",
                 e->name, e->ptr, diag_mem_region(e->ptr),
                 (unsigned)(e->bytes / 1024u));
    }

    // Aggregate free space per cap, for budgeting against actual steady state
    // rather than the boot-time pool sizes.
    ESP_LOGI(TAG,
             "placement heap: internal free=%u largest=%u | psram free=%u largest=%u"
             " | dma free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}
