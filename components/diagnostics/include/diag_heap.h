#pragma once
/* Heap-corruption bisect probe.
 *
 * DIAG_HEAP_CHECK("label") scans every heap and logs "HEAP OK"/"HEAP CORRUPT"
 * against the label, so a run of checkpoints through an init chain pins the
 * corruption to the sub-step that caused it. Off by default: without
 * CONFIG_AMYSYNTH_HEAP_CHECK every call compiles to nothing, so call sites
 * need no #if guards.
 *
 * All output carries the "heapchk" tag regardless of the calling module, so a
 * bisect across several components reads as one chronological log stream.
 *
 * The scan is O(heap) and slow under CONFIG_HEAP_POISONING_COMPREHENSIVE -
 * enough back-to-back checks can starve the idle task and trip the task
 * watchdog. Init paths only; never in the render path or an ISR.
 */
#include "sdkconfig.h"

#if CONFIG_AMYSYNTH_HEAP_CHECK

#include "esp_heap_caps.h"
#include "esp_log.h"

#define DIAG_HEAP_CHECK(label) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE("heapchk", "HEAP CORRUPT detected at: %s", (label)); \
    } else { \
        ESP_LOGI("heapchk", "HEAP OK at: %s", (label)); \
    } \
} while (0)

#else

#define DIAG_HEAP_CHECK(label) do { (void)(label); } while (0)

#endif
