
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "iot_button.h"
#include "priv_i2c_u8g2.h"
#include "u8g2.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task.h"
#include "amy.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "rotary_encoder.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "custompatches/sample_rec.h"
#include "usb_audio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "render_clock.h"
#include "esp_compiler.h"
#include "soc/gpio_num.h"
#include "my_buttons.h"

#ifndef CONFIG_AMYSYNTH_INPUT_DIAGNOSTICS
#define CONFIG_AMYSYNTH_INPUT_DIAGNOSTICS 0
#endif

static const char *TAG = "main"; // For ESP_LOG and related logs in this file

/* DEBUG: bisect heap corruption. Gated by CONFIG_AMYSYNTH_HEAP_CHECK; when the
 * option is off (default) this compiles to nothing. Enable it in menuconfig
 * (Heap Diagnostics) only while hunting corruption — the integrity scan is slow
 * under comprehensive poisoning. */
#if CONFIG_AMYSYNTH_HEAP_CHECK
#define HEAP_CHECK(where) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "HEAP CORRUPT detected at: %s", where); \
    } else { \
        ESP_LOGI(TAG, "HEAP OK at: %s", where); \
    } \
} while (0)
#else
#define HEAP_CHECK(where) do { (void)(where); } while (0)
#endif

// Rotary encoder pins
#define ENCODER_PIN_A GPIO_NUM_40
#define ENCODER_PIN_B GPIO_NUM_41
static i2c_u8g2_handle_t s_display;
static u8g2_t *s_u8g2 = NULL;
static volatile uint32_t s_last_seq_tick = 0;
static volatile uint32_t s_seq_tick_hook_count = 0;
static volatile uint32_t s_render_block_count = 0;
// Counts render ticks that arrived while the previous block was still rendering
// (i.e. amy_update() took >1 block period). Pure diagnostic — strict 1:1 pacing
// never renders the backlog. A climbing value means render is falling behind
// realtime (reduce per-block cost), mirroring the reference's "i2s underrun".
static volatile uint32_t s_render_overruns = 0;
// Counts render blocks dropped because the USB ring buffer was full (host slow /
// not draining). Pure diagnostic. In the real-time drop path a full buffer means
// the whole block is discarded (all-or-nothing) to keep AMY phase aligned.
static volatile uint32_t s_usb_drops = 0;
// Set true while the patch-select button (MY_BUTTON_1) is held; encoder turns
// then cycle the patch (melodic layer's patch on the sequencer screen, or the
// arp's own patch on the arp screen) instead of moving the selection. This is
// a plain hold+turn gesture — no timer/latch. BPM lives in the menu overlay.
static volatile bool s_patch_held = false;
// Set true while the pitch-edit button (MY_BUTTON_2) is held; encoder turns
// transpose the selected track's pitch (both drum and melodic layers). Drum
// patch selection is the patch-hold gesture (MY_BUTTON_1 + encoder) instead.
static volatile bool s_drum_select_held = false;

static QueueHandle_t s_button_queue = NULL;

typedef struct {
    my_button_id_t id;
    button_event_t event;
} button_msg_t;

static void main_sequencer_tick_hook(uint32_t tick_count)
{
    s_last_seq_tick = tick_count;
    s_seq_tick_hook_count++;
    /* Per-step probability/ratchet/conditional-trig engine (seq_core_trig.c) —
     * needs to run at the same cadence as the AMY sequencer tick itself, not
     * the 20 Hz UI task, so ratchets (which subdivide a single step) resolve
     * correctly. No-op unless at least one step in the pattern is "decorated". */
    sequencer_core_service_tick();
}

#if CONFIG_USB_AUDIO_DIAGNOSTICS
static void main_log_audio_diagnostics(void)
{
    usb_audio_diag_snapshot_t diag;
    usb_audio_diag_get_snapshot(&diag);
    ESP_LOGI(TAG,
             "audio diag: init=%d fill=%u peak_fill=%u writes=%" PRIu32 " drops=%" PRIu32 " underruns=%" PRIu32 " peak_abs=%d",
             diag.initialized ? 1 : 0,
             (unsigned)diag.fill_samples,
             (unsigned)diag.peak_fill_samples,
             diag.write_calls,
             diag.write_drop_events,
             diag.underrun_events,
              (int)diag.peak_abs_sample);
}
#endif

#if CONFIG_AMYSYNTH_RTOS_STATS
#include "esp_heap_caps.h"
// Periodic RTOS profiling dump. Gated by CONFIG_AMYSYNTH_RTOS_STATS so release
// builds carry zero overhead. Relies on CONFIG_FREERTOS_USE_TRACE_FACILITY,
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS,
// CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID and
// CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER (all enabled in sdkconfig).
//
// This project uses the IDF (non-SMP) dual-core FreeRTOS kernel, so we drive
// everything off uxTaskGetSystemState(): it fills a TaskStatus_t[] with each
// task's name, priority, stack high-water mark, cumulative run-time counter,
// and the core it is pinned to (xCoreID, present because COREID is enabled).
// From that single snapshot we derive both the per-task breakdown and the
// per-core busy %, avoiding the SMP-only helpers that aren't declared here.
static const char *task_state_str(eTaskState st)
{
    switch (st) {
        case eRunning:   return "RUN";
        case eReady:     return "RDY";
        case eBlocked:   return "BLK";
        case eSuspended: return "SUS";
        case eDeleted:   return "DEL";
        default:         return "INV";
    }
}

static void log_rtos_stats(void)
{
    UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = malloc(num_tasks * sizeof(TaskStatus_t));
    if (tasks == NULL) {
        ESP_LOGW(TAG, "rtos stats: malloc(%u tasks) failed", (unsigned)num_tasks);
        return;
    }

    uint32_t total_runtime = 0;
    num_tasks = uxTaskGetSystemState(tasks, num_tasks, &total_runtime);
    if (num_tasks == 0 || total_runtime == 0) {
        ESP_LOGW(TAG, "rtos stats: snapshot unavailable");
        free(tasks);
        return;
    }

    // Per-task table + accumulate cumulative idle time per core for the
    // per-core busy % (computed over the interval since the previous dump).
    uint64_t idle_now[portNUM_PROCESSORS] = {0};
    ESP_LOGI(TAG, "RTOS tasks: name             core prio stack_hwm cpu%%(cumulative)");
    for (UBaseType_t i = 0; i < num_tasks; i++) {
        const TaskStatus_t *t = &tasks[i];
        uint32_t cpu_pct = (uint32_t)(((uint64_t)t->ulRunTimeCounter * 100ULL) / total_runtime);
        int core = (int)t->xCoreID; // tskNO_AFFINITY shows as a large value
        ESP_LOGI(TAG, "  %-16s %4d %4u %9u %3u%%",
                 t->pcTaskName,
                 core,
                 (unsigned)t->uxCurrentPriority,
                 (unsigned)t->usStackHighWaterMark,
                 (unsigned)cpu_pct);
        // The IDLE tasks are named "IDLE0"/"IDLE1" on the IDF kernel.
        if (strncmp(t->pcTaskName, "IDLE", 4) == 0) {
            int c = t->pcTaskName[4] - '0';
            if (c >= 0 && c < portNUM_PROCESSORS) {
                idle_now[c] = (uint64_t)t->ulRunTimeCounter;
            }
        }
    }

    // Per-core busy %, diffing each core's IDLE-task counter against wall time.
    // RUN_TIME_STATS_USING_ESP_TIMER=y => counters are in microseconds and
    // share the esp_timer time base, so the interval == esp_timer delta.
    static uint64_t s_prev_wall_us = 0;
    static uint64_t s_prev_idle_us[portNUM_PROCESSORS] = {0};
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t wall_delta = now_us - s_prev_wall_us;
    for (int core = 0; core < portNUM_PROCESSORS; core++) {
        uint64_t idle_delta = idle_now[core] - s_prev_idle_us[core];
        if (s_prev_wall_us != 0 && wall_delta > 0) {
            uint32_t idle_pct = (uint32_t)((idle_delta * 100ULL) / wall_delta);
            if (idle_pct > 100) idle_pct = 100; // clamp sampling skew
            ESP_LOGI(TAG, "core %d load: busy=%u%% idle=%u%% (interval %u ms)",
                     core, (unsigned)(100 - idle_pct), (unsigned)idle_pct,
                     (unsigned)(wall_delta / 1000ULL));
        } else {
            ESP_LOGI(TAG, "core %d load: (baseline captured)", core);
        }
        s_prev_idle_us[core] = idle_now[core];
    }
    s_prev_wall_us = now_us;

    free(tasks);

    // Heap snapshot: internal vs PSRAM free + largest free block.
    ESP_LOGI(TAG,
             "heap: free=%u min_free=%u | internal free=%u largest=%u | psram free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
#endif


static void amy_usb_render_task(void *arg) {
    (void)arg;

    // Master clock: by default a GPTimer fires every block period (256
    // samples @ 48 kHz = 5333.33 us), tick-rate-independent (the old
    // vTaskDelay(pdMS_TO_TICKS(5)) floored to 0 ticks at FREERTOS_HZ=100 and
    // busy-spun the core). CONFIG_RENDER_CLOCK_I2S_ENABLE (default off) swaps
    // in an I2S-DMA-paced backend instead; see render_clock.h. Started from
    // inside the task so either backend's ISR registers on the render core.
    if (render_clock_start(AMY_BLOCK_SIZE, AMY_SAMPLE_RATE) != ESP_OK) {
        ESP_LOGE(TAG, "render_clock_start failed; render task aborting");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        // Block until the next tick. Returns the accumulated tick count; >1 means
        // the previous block overran its period. STRICT 1:1: we render exactly
        // ONE block regardless, so AMY's total_blocks stays locked to realtime
        // (sequencer tempo, derived from amy_sysclock(), cannot drift). The
        // 170 ms USB ring buffer absorbs transient jitter, not a catch-up queue.
        uint32_t ticks = render_clock_wait();
        if (unlikely(ticks > 1)) {
            s_render_overruns += (ticks - 1);  // diagnostic only
        }

        int16_t *block = amy_update();           // synthesizes everything / advances AMY sample clock
        if (likely(block != NULL)) {
            s_render_block_count++;

            // Runtime PCM sampler (custompatches/sample_rec): alloc-free/non-
            // blocking no-op unless a recording is actually armed/in-progress.
            // Must run after amy_update() so amy_queue_lock is already released
            // (see amy-internals.md's lock-ownership section).
            sample_rec_render_tick(block, AMY_BLOCK_SIZE);

#if CONFIG_USB_AUDIO_BLOCKING_WRITE
            // Resilient path: retry until the host consumes the data, slaving the
            // synth to the PC's consumption rate. NOTE: vTaskDelay(1) waits one
            // whole tick (>=1, never floors to 0 like pdMS_TO_TICKS(1) does at
            // FREERTOS_HZ=100) so this cannot degrade into a busy-spin.
            while (usb_audio_write_stereo(block, AMY_BLOCK_SIZE) == ESP_ERR_NO_MEM) {
                vTaskDelay(1);
            }
#else
            // Real-time path: the write is all-or-nothing. If the ring buffer is
            // full the whole block is dropped to keep AMY's clock phase-aligned;
            // we do NOT delay here (the strict-1:1 GPTimer already paces us, and
            // the previous pdMS_TO_TICKS(1) backoff floored to 0 ticks and did
            // nothing anyway). Just count the drop for diagnostics.
            // Only a real drop if a host is consuming; write_stereo returns
            // ESP_OK (skips the ring) when idle, so this is belt-and-suspenders.
            if (unlikely(usb_audio_write_stereo(block, AMY_BLOCK_SIZE) == ESP_ERR_NO_MEM)
                && usb_audio_consumer_active()) {
                s_usb_drops++;
            }
#endif
        }
    }
}
// Button event callback: routes my_buttons events to sequencer UI actions
static void button_handler_task(void *pvParameters)
{
    (void)pvParameters;
    button_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_button_queue, &msg, portMAX_DELAY) == pdTRUE) {
            switch (msg.id) {
                case MY_BUTTON_0:
                    if (msg.event == BUTTON_SINGLE_CLICK) {
                        synth_ui_cycle_active_layer();
                    } else if (msg.event == BUTTON_LONG_PRESS_START) {
                        synth_ui_toggle_playing();
                    }
                    break;
                case MY_BUTTON_ENC:
                    synth_ui_handle_button();
                    break;
                default:
                    break;
            }
        }
    }
}

static void main_button_event_cb(my_button_id_t button_id, button_event_t event, void *user_data)
{
    (void)user_data;

    // MY_BUTTON_1 is the patch-select hold button, repurposed per editor:
    //   filter editor  → enabled on/off toggle (single press)
    //   ADSR/LFO editor → layer-wide vs track-only commit scope toggle
    if (button_id == MY_BUTTON_1) {
        if (synth_ui_filter_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_filter_toggle_enabled();
            }
            return;
        }
        if (event == BUTTON_PRESS_DOWN) {
            if (synth_ui_toggle_editor_apply_scope()) return;
        }
        /* PROG screen: MY_BUTTON_1 deletes the entry at the cursor (the patch-hold
         * gesture has no meaning here). */
        if (synth_ui_prog_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_prog_delete_entry();
            }
            return;
        }
        if (event == BUTTON_PRESS_DOWN) {
            s_patch_held = true;
            synth_ui_set_patch_select_mode(true);
        } else if (event == BUTTON_PRESS_UP) {
            s_patch_held = false;
            synth_ui_set_patch_select_mode(false);
        }
        return;
    }

    /* Arp screen isolation: while the arp screen is showing (and neither the
     * graph editor nor the menu is up), the sequencer's editing gestures must
     * NOT leak through and mutate sequencer state behind the hidden grid. The
     * arp screen's own input (encoder nav + MY_BUTTON_ENC field edit + the
     * MY_BUTTON_1 patch hold/turn) is handled elsewhere and is unaffected; the
     * menu toggle (MY_BUTTON_3) and global play/pause (MY_BUTTON_0 long-press)
     * also stay live. Everything else is suppressed here. */
    if (synth_ui_arp_is_active()) {
        switch (button_id) {
            case MY_BUTTON_2:
                /* Drum-sound-select hold: pure sequencer state. Block, and make
                 * sure we never leave the latch stuck on if it was held when the
                 * screen switched. */
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                return;
            case MY_BUTTON_0:
                /* Layer cycle (single-click) is sequencer-only; suppress it.
                 * Keep global play/pause on long-press. */
                if (event == BUTTON_LONG_PRESS_START) {
                    synth_ui_toggle_playing();
                }
                return;
            default:
                break;
        }
    }

    /* Prog screen isolation: suppress sequencer-only gestures (MY_BUTTON_2
     * drum-select hold, MY_BUTTON_0 layer-cycle) while the PROG screen is up.
     * MY_BUTTON_2 is repurposed as "+entry"; MY_BUTTON_1 as "del" (handled above). */
    if (synth_ui_prog_is_active()) {
        switch (button_id) {
            case MY_BUTTON_2:
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                if (event == BUTTON_PRESS_DOWN) {
                    synth_ui_prog_add_entry();
                }
                return;
            case MY_BUTTON_0:
                if (event == BUTTON_LONG_PRESS_START) {
                    synth_ui_toggle_playing();
                }
                return;
            default:
                break;
        }
    }

    /* Track Options screen isolation: suppress sequencer-only gestures and
     * repurpose spare buttons for layer management.
     *   MY_BUTTON_1 click  → add a melodic layer (if below MAX_LAYERS)
     *   MY_BUTTON_2 click  → delete the layer currently shown (s_to_layer);
     *                        no-op if it is the drum layer or the last layer
     *   MY_BUTTON_0 long   → play / pause (keep live)
     * MY_BUTTON_1 normally drives patch-hold; suppress that in this context. */
    if (synth_ui_trackopts_is_active()) {
        switch (button_id) {
            case MY_BUTTON_1:
                if (event == BUTTON_SINGLE_CLICK) {
                    synth_ui_request_add_layer();
                }
                return;
            case MY_BUTTON_2:
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                if (event == BUTTON_SINGLE_CLICK) {
                    synth_ui_request_delete_to_layer();
                }
                return;
            case MY_BUTTON_0:
                if (event == BUTTON_LONG_PRESS_START) {
                    synth_ui_toggle_playing();
                }
                return;
            default:
                break;
        }
    }

    /* Drone screen isolation: same rationale as the arp guard above. While the
     * drone screen is showing (no graph/menu), suppress the sequencer's editing
     * gestures so they don't mutate sequencer state behind the hidden grid. The
     * drone's own input (encoder nav + MY_BUTTON_ENC row edit + MY_BUTTON_1
     * patch hold/turn in PATCH mode) is handled elsewhere; the menu toggle
     * (MY_BUTTON_3) and play/pause (MY_BUTTON_0 long-press) stay live. */
    if (synth_ui_drone_is_active()) {
        switch (button_id) {
            case MY_BUTTON_2:
                s_drum_select_held = false;
                synth_ui_set_drum_select_mode(false);
                return;
            case MY_BUTTON_0:
                if (event == BUTTON_LONG_PRESS_START) {
                    synth_ui_toggle_playing();
                }
                return;
            default:
                break;
        }
    }

    // MY_BUTTON_2 is normally the pitch-edit hold button (transpose the selected
    // track via the encoder). While the graph editor is open it toggles amp-edit
    // mode so the encoder adjusts the target's amplitude trim instead of ADSR
    // points. Time-range is now auto-switched; MY_BUTTON_2 is freed for this use.
    if (button_id == MY_BUTTON_2) {
        if (synth_ui_filter_is_active()) {
            /* Suppress drum-select hold so the latch never gets stuck. */
            return;
        }
        if (synth_ui_graph_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_graph_toggle_amp_mode();
            }
            return;
        }
        if (synth_ui_stepedit_is_active()) {
            /* Suppress drum-select hold while the popup owns the encoder.
             * (Step Trig editor open/close now lives on MY_BUTTON_3.) */
            return;
        }
        if (event == BUTTON_PRESS_DOWN) {
            s_drum_select_held = true;
            synth_ui_set_drum_select_mode(true);
        } else if (event == BUTTON_PRESS_UP) {
            s_drum_select_held = false;
            synth_ui_set_drum_select_mode(false);
        }
        return;
    }

    // MY_BUTTON_3: while any editor (ADSR/filter/LFO) is open, single-click
    // cycles to the next editor. While the ADSR graph editor specifically is
    // open, long-press instead switches between its EG0 (amp) and EG1
    // (typically filter) breakpoint sets. Step Trig editor: long-press opens
    // it, single-click closes it (moved off MY_BUTTON_2 so it no longer
    // fires accidentally while holding MY_BUTTON_2 to transpose). Outside
    // all of that it is the menu toggle.
    if (button_id == MY_BUTTON_3) {
        if (synth_ui_graph_is_active() || synth_ui_filter_is_active()
                                       || synth_ui_lfo_is_active()) {
            if (event == BUTTON_SINGLE_CLICK) {
                synth_ui_cycle_editor();
            } else if (event == BUTTON_LONG_PRESS_START && synth_ui_graph_is_active()) {
                synth_ui_graph_toggle_eg_index();
            }
            return;
        }
        if (synth_ui_stepedit_is_active()) {
            if (event == BUTTON_SINGLE_CLICK) {
                synth_ui_stepedit_close();
            }
            return;
        }
        if (event == BUTTON_LONG_PRESS_START) {
            if (!synth_ui_menu_is_active()) {
                synth_ui_stepedit_open();
            }
            return;
        }
        if (event == BUTTON_SINGLE_CLICK) {
            synth_ui_menu_toggle();
        }
        return;
    }

    /* graph pop-up: encoder push is the editor's select/adjust + open trigger.
     * - long press (editor closed) opens the envelope editor
     * - short press (editor open)  toggles select<->adjust, or confirms in VIEW
     * Remove this block to revert the integration. */
    if (button_id == MY_BUTTON_ENC) {
        if (synth_ui_filter_is_active()) {
            /* Long-press commits + closes; short-press cycles cursor. */
            if (event == BUTTON_LONG_PRESS_START) {
                synth_ui_filter_close_commit();
            } else if (event == BUTTON_PRESS_DOWN) {
                synth_ui_filter_handle_button(false);
            }
            return;
        }
        if (synth_ui_lfo_is_active()) {
            if (event == BUTTON_LONG_PRESS_START) {
                synth_ui_lfo_close_commit();
            } else if (event == BUTTON_PRESS_DOWN) {
                synth_ui_lfo_handle_button(false);
            }
            return;
        }
        /* Step Trig editor: short press cycles the focused field, long press
         * closes it (symmetric with the filter/LFO/graph editors above). */
        if (synth_ui_stepedit_is_active()) {
            if (event == BUTTON_LONG_PRESS_START) {
                synth_ui_stepedit_close();
            } else if (event == BUTTON_PRESS_DOWN) {
                synth_ui_stepedit_handle_button();
            }
            return;
        }
        if (synth_ui_graph_is_active()) {
            /* Long-press closes the editor and COMMITS, symmetric with the
             * long-press that opens it. Short-press toggles select<->adjust.
             * (Discard-on-close stays on MY_BUTTON_0 long-press below.) */
            if (event == BUTTON_LONG_PRESS_START) {
                synth_ui_graph_close_commit(); /* long = commit + close */
            } else if (event == BUTTON_PRESS_DOWN) {
                synth_ui_graph_handle_button(false); /* short press */
            }
            return;
        }
        /* Menu overlay captures the encoder push (enter/exit editing, or run an
         * action item). Highest priority below the graph editor. */
        if (synth_ui_menu_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_menu_handle_button();
            }
            return;
        }
        /* Arp screen: short press toggles edit on the focused field/slot; long
         * press opens the ADSR editor bound to the arp envelope. */
        if (synth_ui_arp_is_active()) {
            if (event == BUTTON_LONG_PRESS_START) {
                synth_ui_graph_open_envelope();
            } else if (event == BUTTON_PRESS_DOWN) {
                synth_ui_arp_handle_button();
            }
            return;
        }
        /* Drone screen: short press toggles edit on the focused row; long press
         * opens the ADSR editor bound to the drone envelope. */
        if (synth_ui_drone_is_active()) {
            if (event == BUTTON_LONG_PRESS_START) {
                synth_ui_graph_open_envelope();
            } else if (event == BUTTON_PRESS_DOWN) {
                synth_ui_drone_handle_button();
            }
            return;
        }
        /* Prog screen: encoder-click navigates cursor / confirms edits. */
        if (synth_ui_prog_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_prog_handle_button();
            }
            return;
        }
        /* Track Options screen: encoder-click toggles edit on the focused row. */
        if (synth_ui_trackopts_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_trackopts_handle_button();
            }
            return;
        }
#if CONFIG_SYNTH_CUSTOM_FM
        /* FM screen: encoder-click toggles edit on the focused row. */
        if (synth_ui_fm_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_fm_handle_button();
            }
            return;
        }
#endif
        if (event == BUTTON_LONG_PRESS_START) {
            synth_ui_graph_open_envelope();
            return;
        }
        /* else fall through to the normal PRESS_DOWN queueing below */
    }

    /* MY_BUTTON_0 long press cancels whichever overlay editor is open. */
    if (button_id == MY_BUTTON_0 &&
        (synth_ui_graph_is_active() || synth_ui_filter_is_active()
                                    || synth_ui_lfo_is_active())) {
        if (event == BUTTON_LONG_PRESS_START) {
            if (synth_ui_filter_is_active())
                synth_ui_filter_handle_button(true); /* long = cancel */
            else if (synth_ui_lfo_is_active())
                synth_ui_lfo_handle_button(true);
            else
                synth_ui_graph_handle_button(true);
        }
        return;
    }

    /* MY_BUTTON_0: short press = cycle active layer, long press = play/stop */
    if (button_id == MY_BUTTON_0) {
        if (event == BUTTON_SINGLE_CLICK || event == BUTTON_LONG_PRESS_START) {
            if (s_button_queue != NULL) {
                button_msg_t msg = { .id = button_id, .event = event };
                (void)xQueueSend(s_button_queue, &msg, 0);
            } else {
                if (event == BUTTON_SINGLE_CLICK)      synth_ui_cycle_active_layer();
                else if (event == BUTTON_LONG_PRESS_START) synth_ui_toggle_playing();
            }
        }
        return;
    }

    /* All other buttons respond to PRESS_DOWN */
    if (event != BUTTON_PRESS_DOWN) return;

    if (s_button_queue != NULL) {
        button_msg_t msg = { .id = button_id, .event = event };
        (void)xQueueSend(s_button_queue, &msg, 0);
    } else {
        switch (button_id) {
            case MY_BUTTON_ENC:
                synth_ui_handle_button();
                break;
            default:
                break;
        }
    }
}

//encoder
static void encoder_task(void *pvParameters)
{
    rotary_encoder_handle_t enc = (rotary_encoder_handle_t)pvParameters;
    long prev = rotary_encoder_get_count(enc);
    static long enc_accum = 0; // sub-step accumulator (2 raw ticks = 1 action)
    for (;;) {
        long cur = rotary_encoder_get_count(enc);
        
        if (cur != prev) {
            long delta = cur - prev;
#if CONFIG_AMYSYNTH_INPUT_DIAGNOSTICS
            ESP_LOGI(TAG,
                     "encoder count=%ld (delta=%ld)",
                     (long)cur, (long)(delta));
#endif
            prev = cur;

            enc_accum += delta;
            long steps = enc_accum / 2; // require 2 raw ticks per action
            enc_accum %= 2;
            if (steps == 0) goto next_poll;

            /* filter editor: highest priority encoder consumer. */
            if (synth_ui_filter_handle_encoder(steps)) {
                goto next_poll;
            }

            /* LFO editor: scrolls cursor or adjusts selected field. */
            if (synth_ui_lfo_handle_encoder(steps)) {
                goto next_poll;
            }

            /* Step Trig editor: adjusts the focused field (prob/ratchet/cond/param)
             * for the currently-selected sequencer grid step. */
            if (synth_ui_stepedit_handle_encoder(steps)) {
                goto next_poll;
            }

            /* graph pop-up: when open, the encoder drives the curve editor and
             * normal sequencer routing is skipped. Remove this branch to revert. */
            if (synth_ui_graph_handle_encoder(steps)) {
                goto next_poll;
            }

            // Menu overlay captures the encoder (scroll items, or change the
            // value of the entered item). Highest priority below the graph.
            if (synth_ui_menu_handle_encoder(steps)) {
                goto next_poll;
            }

            if (s_patch_held) {
                // Plain patch hold+turn. On the arp screen this cycles the
                // arp's own patch; on the drone screen the drone's PATCH-mode
                // preset. On the sequencer screen it cycles the active layer's
                // patch: the melodic layer's shared patch, or — for a drum
                // layer — the SELECTED drum track's own patch (drums are
                // per-track Juno patches now).
                if (synth_ui_drone_is_active()) {
                    synth_ui_drone_cycle_patch((int)steps);
                } else if (synth_ui_arp_is_active()) {
                    synth_ui_arp_cycle_patch((int)steps);
                } else if (sequencer_core_get_layer_type(seq_get_active_layer_idx())
                           == SEQ_LAYER_DRUM) {
                    synth_ui_cycle_drum_patch((int)steps);
                } else {
                    synth_ui_cycle_melodic_patch((int)steps);
                }
            } else if (synth_ui_drone_is_active()) {
                // Drone screen: encoder moves the cursor / edits the focused row.
                synth_ui_drone_handle_encoder(steps);
            } else if (s_drum_select_held) {
                // Pitch-edit mode: hold MY_BUTTON_2 + turn encoder edits the
                // selected track's pitch (works for both drum and melodic).
                synth_ui_adjust_track_note((int)steps);
            } else if (synth_ui_arp_is_active()) {
                // Arp screen: encoder moves the cursor / edits the focused field.
                synth_ui_arp_handle_encoder(steps);
            } else if (synth_ui_prog_is_active()) {
                // Prog screen: encoder scrolls cursor / edits the focused entry field.
                synth_ui_prog_handle_encoder((int)steps);
            } else if (synth_ui_trackopts_is_active()) {
                // Track Options screen: encoder scrolls rows / edits focused value.
                synth_ui_trackopts_handle_encoder((int)steps);
#if CONFIG_SYNTH_CUSTOM_FM
            } else if (synth_ui_fm_is_active()) {
                // FM screen: encoder scrolls rows / edits focused value.
                synth_ui_fm_handle_encoder((int)steps);
#endif
            } else {
                synth_ui_handle_encoder(steps);
            }
        }

next_poll:
        vTaskDelay(pdMS_TO_TICKS(20));  // Poll at 50Hz
    }
}

static void encoder_init_task(void *pvParameters)
{
    (void)pvParameters;
    // Delay init to avoid early-boot conflicts (PSRAM, console pins, etc.)
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "[encoder_init] starting after delay");
    // Button GPIO (GPIO16) is now managed by my_buttons/iot_button.

    rotary_encoder_config_t enc_cfg = rotary_encoder_default_config(ENCODER_PIN_A, ENCODER_PIN_B);
    rotary_encoder_handle_t enc = NULL;
    esp_err_t err = rotary_encoder_new_with_config(&enc_cfg, &enc);
    ESP_LOGI(TAG, "[encoder_init] rotary_encoder_new_with_config returned %d", err);
    if (err == ESP_OK && enc) {
        // Increase encoder task stack to avoid stack overflow when handling
        // amy_event-heavy operations (sequencer toggles create several
        // amy_event/delta conversions on the stack).
        // Pin to Core 0 so the encoder poll never migrates onto Core 1 and
        // jitters the audio DSP now running there.
        xTaskCreatePinnedToCore(encoder_task,
             "encoder_task",
             8192,
             enc,
             5,
              NULL,
              0);
    }

    vTaskDelete(NULL);
}

#if defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)
// Measure the AMY profiler's own per-timestamp cost on-target so the per-stage
// numbers can be corrected for instrumentation overhead. AMY's profiler uses
// amy_get_us() == esp_timer_get_time() on ESP; each START/STOP is two reads.
// Coarse mode does ~8 reads/block; full mode does 2*(sum of tag 'calls')/block,
// which the dump itself reports per tag.
static void amy_profiler_overhead_selftest(void)
{
    const int N = 20000;
    volatile int64_t sink = 0;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < N; ++i) sink ^= esp_timer_get_time();
    int64_t t1 = esp_timer_get_time();
    (void)sink;
    // Subtract one read (t1) negligibly; report ns/read averaged over N reads.
    double ns_per_read = ((double)(t1 - t0) * 1000.0) / (double)N;
    double coarse_us_per_block = (ns_per_read * 8.0) / 1000.0;  // 4 START/STOP pairs
    const uint32_t block_us = (uint32_t)(((uint64_t)AMY_BLOCK_SIZE * 1000000ULL)
                                         / (uint64_t)AMY_SAMPLE_RATE);
    ESP_LOGW(TAG,
        "[amy-profile] esp_timer_get_time ~%.1f ns/read | coarse overhead ~%.2f us/block "
        "(~%.3f%% of %u us budget) | full overhead = 2*sum(tag.calls)/block * read_cost",
        ns_per_read, coarse_us_per_block,
        (coarse_us_per_block / (double)block_us) * 100.0, block_us);
}
#endif

void app_main(void)
{
   
    ESP_LOGI(TAG, "Hello world!");
/*
    // I2C recover: Flush Sequence ensure SDA released and toggle SCL 9 times
    printf("[startup] before i2c_recover\n");
    gpio_set_direction(CONFIG_I2C_U8G2_SDA_GPIO, GPIO_MODE_INPUT);

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_I2C_U8G2_SCL_GPIO,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    for (int i = 0; i < 9; i++) {
        gpio_set_level(CONFIG_I2C_U8G2_SCL_GPIO, 0);
        ets_delay_us(5);
        gpio_set_level(CONFIG_I2C_U8G2_SCL_GPIO, 1);
        ets_delay_us(5);
    }
    printf("[startup] after i2c_recover\n");
*/
    ESP_LOGI(TAG, "[startup] before i2c_u8g2_init");
    // Display configuration is now managed through menuconfig (Kconfig)
    i2c_u8g2_config_t display_cfg = i2c_u8g2_config_default();
    ESP_LOGI(TAG, "[startup] display i2c cfg: SDA=%d SCL=%d Freq=%d Addr=0x%02X Timeout=%dms",
             (int)display_cfg.sda_io_num,
             (int)display_cfg.scl_io_num,
             (int)display_cfg.scl_speed_hz,
             (unsigned int)display_cfg.device_address,
             (int)display_cfg.timeout_ms);

    esp_err_t display_err = i2c_u8g2_init(&s_display, &display_cfg);
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "[startup] i2c_u8g2_init failed: %s", esp_err_to_name(display_err));
        return;
    }
    s_u8g2 = i2c_u8g2_get_u8g2(&s_display);
    if (s_u8g2 == NULL) {
        ESP_LOGE(TAG, "[startup] i2c_u8g2_get_u8g2 returned NULL");
        return;
    }
    ESP_LOGI(TAG, "[startup] after i2c_u8g2_init");

  
    // Configure and start AMY
    amy_config_t amy_cfg = amy_default_config();
    amy_cfg.audio = AMY_AUDIO_IS_NONE;
    /* Disable AMY's internal FABT/render tasks: our amy_usb_render_task owns the
    // render loop entirely (AMY_AUDIO_IS_NONE mode). With multithread=1(?? I think 0) (the default),
    // amy_platform_init spawns esp_fill_audio_buffer_task (FABT) and stores app_main's
    // task handle as amy_update_handle. FABT then notifies app_main instead of our
    // render task, causing a permanent deadlock — render_blocks and seq_tick stay 0.*/
    amy_cfg.platform.multicore = 0;
    amy_cfg.platform.multithread = 0;
    amy_cfg.amy_external_sequencer_hook = main_sequencer_tick_hook;
    /* PERF (2026-06): force the render-hot AMY allocations into internal SRAM.
     * amy_default_config() leaves these at MALLOC_CAP_DEFAULT (0), which only
     * lands internal for blocks < CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB);
     * larger blocks (the ~40 KB delta pool, FX delay lines, reverb buffers) fall
     * back to Octal PSRAM. synth[]/msynth[] structs (ram_caps_events) and the
     * delta pool (ram_caps_synth) are dereferenced for every audible oscillator
     * every 256-sample block on Core 1; the echo/reverb/chorus delay lines
     * (ram_caps_delay) are walked sample-by-sample in the FX stage. Pinning them
     * to internal SRAM removes PSRAM access latency from the audio hot path. We
     * have ample internal RAM headroom (heap checks below confirm). */
    amy_cfg.ram_caps_events = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    amy_cfg.ram_caps_synth  = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    amy_cfg.ram_caps_block  = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    amy_cfg.ram_caps_fbl    = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    /* FX delay lines are too large for internal SRAM: the reverb needs ~108 KB
     * across 10 lines and echo a single 256 KB (65536-sample) line, while the
     * internal heap's largest free block is ~32 KB. Pinning them internal made
     * reverb/echo allocation fail (and previously crashed on the resulting NULL
     * deref). Route them to the 8 MB Octal PSRAM, which sits almost entirely
     * free. The per-sample PSRAM latency in the FX stage is acceptable; the
     * lines simply do not fit internally. */
    amy_cfg.ram_caps_delay  = MALLOC_CAP_SPIRAM;
    /* Runtime PCM sampler (custompatches/sample_rec): a 1.5 s mono recording
     * is ~140 KB, comfortably above the SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB)
     * threshold, so pcm_load()'s malloc_caps() call would already land in
     * PSRAM with the default MALLOC_CAP_DEFAULT (0). Set explicitly anyway so
     * that intent doesn't depend on staying above that threshold. */
    amy_cfg.ram_caps_sample = MALLOC_CAP_SPIRAM;
    /* Default is 256, which only covers layer 0 (drum).  Each additional
     * layer needs SEQ_TRACKS * SEQ_MAX_STEPS * 2 extra tags.  With
     * MAX_LAYERS=4, SEQ_TRACKS=4, SEQ_MAX_STEPS=32 the sequencer's highest tag
     * is 1055 (preview slots).  The standalone arp then occupies tags
     * 1056..1119 (SEQ_ARP_TAG_BASE + ARP_MAX_SLOTS*ARP_OCT_MAX*2).  Set to 1200
     * so the table covers the arp range AND stays clear of the off-by-one in
     * sequencer_add_event's `tag > max_sequences` guard (writes sequences[tag]).
     * Keep in sync with SEQ_ARP_TAG_BASE/COUNT in sequencer_core.c.
     *
     * Per-step ratchet trigs (seq_core_trig.c) then claim a further
     * MAX_LAYERS*SEQ_TRACKS*SEQ_MAX_RATCHET*2 = 128 dedicated one-shot tags
     * right above the arp range: 1120..1247 (SEQ_RATCHET_TAG_BASE/MAX in
     * seq_core_config.h). 1200 no longer covers that — raised to 1280 to
     * clear SEQ_RATCHET_TAG_MAX (1247) with the same +2 off-by-one margin. */
    amy_cfg.max_sequencer_tags = 1280;
    /* Raise the instrument table from the default 64 so the standalone drone
     * synth (custompatches/drone_core) can claim dedicated slots above the
     * existing map (drum 6..9, melodic 11..62, arp 63). The drone uses slots
     * DRONE_SYNTH_MAIN=64 and DRONE_SYNTH_SUB=65. instruments_init() sizes the
     * table from this value. AMY's default 250 oscs leave ample headroom for the
     * drone's 2 voices x 2 oscs. Keep in sync with drone_core.c. */
    amy_cfg.max_synths = 66;
    ESP_LOGI(TAG, "Starting AMY synth engine... (audio=%d, Fs=%d)", amy_cfg.audio, AMY_SAMPLE_RATE);
    ESP_LOGI(TAG, "[startup] before amy_start");
    HEAP_CHECK("before amy_start");
    amy_start(amy_cfg);
    HEAP_CHECK("after amy_start");

#if defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)
    // One-shot: characterize profiler timestamp cost so the dumps below can be
    // read net of instrumentation overhead. See docs/dual-core-render-analysis.md.
    amy_profiler_overhead_selftest();
#endif

    // Our USB Audio (must be after TinyUSB init)
    ESP_ERROR_CHECK(usb_audio_init());
    HEAP_CHECK("after usb_audio_init");

    synth_ui_init(s_u8g2);
    HEAP_CHECK("after synth_ui_init");
    /* Add the first melodic layer (DX7, 16 steps). */
    synth_ui_add_layer(SEQ_LAYER_MELODIC, SEQ_STEPS);
    HEAP_CHECK("after add_layer melodic");
    ESP_LOGI(TAG, "[startup] after amy_start");
    
    TaskHandle_t amy_render_task_handle = NULL;
    BaseType_t render_task_ok;
#if CONFIG_FREERTOS_UNICORE
    render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task,
         "amy_render",
         8192,
         NULL,
         23,
         &amy_render_task_handle,
         0);
#else
    // Pin the entire AMY DSP to Core 1. With multicore/multithread=0 AMY spawns
    // no tasks, so all synthesis runs inside amy_update() in this task. Keeping
    // it on Core 1 gives the synth a dedicated core away from the USB UAC task
    // and the esp_timer/sequencer tick, both of which live on Core 0.
    render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task,
         "amy_render",
         8192,
          NULL,
          22,
          &amy_render_task_handle,
          1); 
#endif
    if (render_task_ok != pdPASS) {
        ESP_LOGW(TAG, "amy_render pinned task create failed (%ld), retrying on core 0", (long)render_task_ok);
        render_task_ok = xTaskCreatePinnedToCore(amy_usb_render_task,
             "amy_render",
             8192,
             NULL,
             22,
             &amy_render_task_handle,
             0);
    }
    if (render_task_ok != pdPASS) {
        ESP_LOGE(TAG, "amy_render task create failed (%ld)", (long)render_task_ok);
    }

    ESP_LOGI(TAG, "AMY + USB Audio ready (48 kHz stereo to PC)");

    // Initialize push buttons (GPIO17, GPIO18, GPIO8, GPIO42)
    ESP_LOGI(TAG, "[startup] before my_buttons_init");
    s_button_queue = xQueueCreate(8, sizeof(button_msg_t));
    if (s_button_queue == NULL) {
        ESP_LOGW(TAG, "Button queue creation failed; callbacks will run inline");
    } else {
        if (xTaskCreatePinnedToCore(button_handler_task,
             "button_task",
             8192,
             NULL,
              5,
              NULL,
            0) != pdPASS) {
            ESP_LOGW(TAG, "Button handler task creation failed");
            vQueueDelete(s_button_queue);
            s_button_queue = NULL;
        }
    }
    HEAP_CHECK("before my_buttons_init");
    esp_err_t btn_err = my_buttons_init();
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "my_buttons_init failed: %s", esp_err_to_name(btn_err));
    } else {
        ESP_LOGI(TAG, "[startup] after my_buttons_init");
        my_buttons_register_cb(main_button_event_cb, NULL);
    }

        // Setup rotary encoder
    // Defer rotary encoder initialization to a task to avoid early-boot conflicts
    xTaskCreatePinnedToCore(encoder_init_task,
         "encoder_init_task",
         2048,
         NULL,
         5,
         NULL,
        0);


    ESP_LOGI(TAG, "Main loop started.");
   
    // Idle loop; pot_reader_task handles all pot->synth updates.
#if CONFIG_AMYSYNTH_RTOS_STATS
    const uint32_t idle_loop_ms = CONFIG_AMYSYNTH_RTOS_STATS_PERIOD_MS;
      void heap_caps_get_info( multi_heap_info_t *info, uint32_t caps );
     void heap_caps_print_heap_info( uint32_t caps );
#elif defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)
    const uint32_t idle_loop_ms = CONFIG_AMY_PROFILE_INTERVAL_MS;
#else
    const uint32_t idle_loop_ms = 5000;
#endif
    while (1) {
        ESP_LOGI(TAG,
                 "Main loop idle... seq_tick=%" PRIu32 " tick_hook_calls=%" PRIu32 " render_blocks=%" PRIu32 " render_overruns=%" PRIu32 " usb_drops=%" PRIu32 " render_sysclock_ms=%" PRIu32,
                 s_last_seq_tick, s_seq_tick_hook_count, s_render_block_count, s_render_overruns, s_usb_drops,
                 /* computed on demand here, not per render block */ amy_sysclock());
#if CONFIG_USB_AUDIO_DIAGNOSTICS
        main_log_audio_diagnostics();
#endif
#if CONFIG_AMYSYNTH_RTOS_STATS
        log_rtos_stats();
#endif
#if defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)
        // Dump and reset the AMY profile accumulated over this window. The
        // "% render" column in COARSE mode is the parallelizable fraction:
        // AMY_RENDER us / (AMY_RENDER + AMY_FILL_BUFFER + AMY_EXECUTE_DELTAS).
        amy_profiles_print();
#endif
        vTaskDelay(pdMS_TO_TICKS(idle_loop_ms));
    }
}
