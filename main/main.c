
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
#include "amy_helpers.h"   /* amy_helpers_set_render_task */
#include "custompatches/sample_rec.h"
#include "usb_audio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "render_clock.h"
#include "esp_compiler.h"
#include "soc/gpio_num.h"
#include "my_buttons.h"
#include "esp_partition.h"
#include "project_fs.h"
#include "project_store.h"
#include "project_snapshot.h"

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

// MY_BUTTON_SHIFT (GPIO47) hold-modifier state, plus a per-button latch so a
// button chorded with SHIFT is swallowed for its whole press (its normal
// hold/click/long behaviour never runs and the release can't leave a latch
// stuck). Touched only from the button-dispatch task, so no volatile needed.
static bool s_shift_held = false;
static bool s_shift_chord_latched[MY_BUTTON_MAX] = { false };

static QueueHandle_t s_button_queue = NULL;

// Every button event is queued to button_task (a press produces up to three
// consumed events: PRESS_DOWN, PRESS_UP, SINGLE_CLICK/LONG_PRESS_START across
// five buttons), so the queue must absorb a short burst of simultaneous
// presses. Overflow drops the event (send with zero timeout).
#define BUTTON_QUEUE_DEPTH 16

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
static void dispatch_button_event(my_button_id_t button_id, button_event_t event);

// Drains the button queue and runs the full per-screen dispatch in task
// context. iot_button delivers events on the esp_timer task — the same task
// that runs the 500 µs sequencer tick callback — so the heavy editor/screen
// branches must not execute there: a long handler would delay sequencer ticks,
// and the esp_timer stack (CONFIG_ESP_TIMER_TASK_STACK_SIZE) is not budgeted
// for them. The callback below only enqueues; all handling lands here.
static void button_handler_task(void *pvParameters)
{
    (void)pvParameters;
    button_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_button_queue, &msg, portMAX_DELAY) == pdTRUE) {
            dispatch_button_event(msg.id, msg.event);
        }
    }
}

// Thin enqueue shim running on the esp_timer task (iot_button's dispatch
// context). Filters to the event types the dispatcher consumes and posts to
// button_task. Inline fallback only when the queue never got created — never
// on a full queue, since handling one event ahead of already-queued ones
// would reorder presses (a drop is less harmful than reordering).
static void main_button_event_cb(my_button_id_t button_id, button_event_t event, void *user_data)
{
    (void)user_data;

    if (event != BUTTON_PRESS_DOWN && event != BUTTON_PRESS_UP &&
        event != BUTTON_SINGLE_CLICK && event != BUTTON_LONG_PRESS_START) {
        return;
    }

    if (s_button_queue != NULL) {
        button_msg_t msg = { .id = button_id, .event = event };
        (void)xQueueSend(s_button_queue, &msg, 0);
    } else {
        dispatch_button_event(button_id, event);
    }
}

// Full button → sequencer/UI action dispatch. Runs on button_task (or inline
// on the esp_timer task only if queue creation failed at startup).
static void dispatch_button_event(my_button_id_t button_id, button_event_t event)
{
    /* MY_BUTTON_SHOULDER (GPIO17, original shoulder): per-view bindings. On the
     * sequencer grid a press toggles the step under the cursor, so one hand
     * rides the encoder while the other enters steps (tracker-style two-handed
     * entry); on the envelope editor it flips the EG1 sweep polarity. Fires on
     * PRESS_DOWN for zero tap latency; all events are consumed here so they
     * never leak into screen logic. */
    if (button_id == MY_BUTTON_SHOULDER) {
        if (event == BUTTON_PRESS_DOWN) {
            ui_view_id_t sv = synth_ui_active_view();
            if (sv == UI_VIEW_SEQ) {
                synth_ui_toggle_step_at_cursor();
            } else if (sv == UI_VIEW_GRAPH) {
                /* Envelope editor: flip the melodic EG1 sweep polarity
                 * (no-op on the EG0 page and for arp/drone targets). */
                synth_ui_graph_flip_eg1_polarity();
            }
        }
        return;
    }

    /* MY_BUTTON_SHIFT (GPIO47, new shoulder): hold-modifier layer. Track the
     * held state here; the chord interception just below turns SHIFT + digit
     * into the "open editor" gestures that used to live on long-presses. A bare
     * SHIFT tap does nothing. */
    if (button_id == MY_BUTTON_SHIFT) {
        if (event == BUTTON_PRESS_DOWN)      s_shift_held = true;
        else if (event == BUTTON_PRESS_UP)   s_shift_held = false;
        return;
    }

    /* SHIFT chords (replace the former open-editor long-presses):
     *   SHIFT + MY_BUTTON_1 -> open the ADSR/graph editor (was encoder long-press)
     *   SHIFT + MY_BUTTON_2 -> open the step probability / trig editor
     *                          (was MY_BUTTON_3 long-press)
     * The chord fires on the digit's PRESS_DOWN and latches that button until
     * its NEXT press-down, swallowing the rest of the chorded press (PRESS_UP
     * plus the trailing SINGLE_CLICK / LONG_PRESS_START) so the button's normal
     * gesture (patch-hold on 1, drum-select on 2, and per-screen single-clicks
     * like the TRACKOPTS layer delete) never runs. SHIFT+1 only opens where the
     * old encoder long-press did (the mode screens); synth_ui_stepedit_open()
     * self-gates to the sequencer screen, so SHIFT+2 elsewhere is a no-op. */
    if (s_shift_chord_latched[button_id]) {
        if (event == BUTTON_PRESS_DOWN) {
            s_shift_chord_latched[button_id] = false;  /* fresh press: re-evaluate */
        } else {
            return;                                    /* swallow the chorded press */
        }
    }
    if (s_shift_held && event == BUTTON_PRESS_DOWN &&
        (button_id == MY_BUTTON_1 || button_id == MY_BUTTON_2)) {
        s_shift_chord_latched[button_id] = true;
        if (button_id == MY_BUTTON_1) {
            ui_view_id_t sv = synth_ui_active_view();
            if (sv == UI_VIEW_SEQ || sv == UI_VIEW_ARP ||
                sv == UI_VIEW_DRONE || sv == UI_VIEW_DRONE_VIS) {
                synth_ui_graph_open_envelope();
            } else if (sv == UI_VIEW_GRAPH || sv == UI_VIEW_LFO) {
                /* Inside the envelope/LFO editor, SHIFT+1 is the apply-to-whole-
                 * layer scope toggle. Moved off the bare button (which now cycles
                 * EG type) so it can't be hit by accident and wipe a layer. */
                synth_ui_toggle_editor_apply_scope();
            }
        } else { /* MY_BUTTON_2 */
            if (!synth_ui_menu_is_active()) synth_ui_stepedit_open();
        }
        return;
    }

    /* Project rename editor: while a project name is being typed, MY_BUTTON_1
     * saves it and MY_BUTTON_2 discards it (MY_BUTTON_3 still closes the whole
     * menu). synth_ui_menu_rename_active() is the authoritative gate (menu open
     * on the projects page mid-rename) and short-circuits instantly otherwise,
     * so this runs before the normal patch-hold / drum-select gestures without
     * disturbing them elsewhere. Fires on PRESS_DOWN and swallows the rest. */
    if ((button_id == MY_BUTTON_1 || button_id == MY_BUTTON_2) &&
        synth_ui_menu_rename_active()) {
        if (event == BUTTON_PRESS_DOWN) {
            if (button_id == MY_BUTTON_1) synth_ui_menu_rename_save();
            else                          synth_ui_menu_rename_discard();
        }
        return;
    }

    // MY_BUTTON_1 is the patch-select hold button, repurposed per editor:
    //   filter editor    → enabled on/off toggle (single press)
    //   envelope editor  → cycle EG curve type (Normal/Linear/DX7/Exp)
    //   LFO editor       → unused (apply-scope moved to SHIFT+1)
    // The apply-to-whole-layer scope toggle that used to live here is now
    // SHIFT+1 (handled in the chord block above) so it can't be pressed by
    // accident and overwrite the whole layer's envelope/filter config.
    if (button_id == MY_BUTTON_1) {
        if (synth_ui_filter_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_filter_toggle_enabled();
            }
            return;
        }
        if (synth_ui_graph_is_active()) {
            if (event == BUTTON_PRESS_DOWN) {
                synth_ui_graph_cycle_eg_type();
            }
            return;
        }
        if (synth_ui_lfo_is_active()) {
            return;   /* scope is now SHIFT+1; bare press is a no-op here */
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

    /* Below the mode-screen isolation guards, the overlay-editor buttons all
     * dispatch on the single precedence resolver instead of re-deriving "which
     * overlay is on top" ad hoc. Resolving once here also removes the former
     * MY_BUTTON_3 stepedit-vs-graph skew (S1): every button now agrees with the
     * draw switch about the active view. */
    ui_view_id_t v = synth_ui_active_view();

    // MY_BUTTON_2 is normally the pitch-edit hold button (transpose the selected
    // track via the encoder). While the graph editor is open it toggles amp-edit
    // mode so the encoder adjusts the target's amplitude trim instead of ADSR
    // points. Time-range is now auto-switched; MY_BUTTON_2 is freed for this use.
    if (button_id == MY_BUTTON_2) {
        switch (v) {
            case UI_VIEW_FILTER:
                /* Suppress drum-select hold so the latch never gets stuck. */
                return;
            case UI_VIEW_GRAPH:
                if (event == BUTTON_PRESS_DOWN) synth_ui_graph_toggle_amp_mode();
                return;
            case UI_VIEW_STEPEDIT:
                /* Suppress drum-select hold while the popup owns the encoder.
                 * (Step Trig editor open/close now lives on MY_BUTTON_3.) */
                return;
            default:
                break;
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
    // cycles to the next editor page (EG0 -> EG1 -> filter -> LFO). The EG1
    // sweep polarity flip lives on MY_BUTTON_SHOULDER (handled above). Step Trig
    // editor: single-click closes it; opening it moved to SHIFT+2 (the former
    // long-press open was replaced). Outside all of that it is the menu toggle.
    if (button_id == MY_BUTTON_3) {
        /* STEPEDIT outranks GRAPH in the canonical order, so resolving via v
         * fixes the former skew where this block checked graph/filter/lfo
         * before stepedit — input now matches what the OLED draws. */
        if (v == UI_VIEW_GRAPH || v == UI_VIEW_FILTER || v == UI_VIEW_LFO) {
            if (event == BUTTON_SINGLE_CLICK) {
                synth_ui_cycle_editor();
            }
            return;
        }
        if (v == UI_VIEW_STEPEDIT) {
            if (event == BUTTON_SINGLE_CLICK) {
                synth_ui_stepedit_close();
            }
            return;
        }
        /* Opening the step-trig editor moved to SHIFT+2 (see the chord block
         * above); MY_BUTTON_3 here is just the menu toggle. */
        if (event == BUTTON_SINGLE_CLICK) {
            synth_ui_menu_toggle();
        }
        return;
    }

    /* graph pop-up: encoder push is the editor's select/adjust toggle only. The
     * commit/close gesture moved off the encoder long-press onto a MY_BUTTON_0
     * short tap (handled below) so finishing an edit is a fast tap, not a ~1.5 s
     * hold. Opening the editor is SHIFT+1 (chord block above).
     * Remove this block to revert the integration. */
    if (button_id == MY_BUTTON_ENC) {
        /* Encoder-push is a clean per-view dispatch — the old cascade was
         * already in canonical order, so this is a direct lift onto v. For the
         * editors the short press toggles select<->adjust (commit/close is now
         * MY_BUTTON_0); the mode screens edit their focused row (opening the
         * bound ADSR editor is SHIFT+1, not the encoder long-press). */
        switch (v) {
            case UI_VIEW_FILTER:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_filter_handle_button(false);
                return;
            case UI_VIEW_LFO:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_lfo_handle_button(false);
                return;
            case UI_VIEW_STEPEDIT:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_stepedit_handle_button();
                return;
            case UI_VIEW_GRAPH:
                if (event == BUTTON_PRESS_DOWN)  synth_ui_graph_handle_button(false);
                return;
            case UI_VIEW_MENU:
                if (event == BUTTON_PRESS_DOWN) synth_ui_menu_handle_button();
                return;
            case UI_VIEW_ARP:
                /* Open moved to SHIFT+1; encoder push edits the focused field. */
                if (event == BUTTON_PRESS_DOWN)  synth_ui_arp_handle_button();
                return;
            case UI_VIEW_DRONE:
            case UI_VIEW_DRONE_VIS:
                /* Open moved to SHIFT+1; encoder push edits the focused row. */
                if (event == BUTTON_PRESS_DOWN)  synth_ui_drone_handle_button();
                return;
            case UI_VIEW_PROG:
                if (event == BUTTON_PRESS_DOWN) synth_ui_prog_handle_button();
                return;
            case UI_VIEW_TRACKOPTS:
                if (event == BUTTON_PRESS_DOWN) synth_ui_trackopts_handle_button();
                return;
#if CONFIG_SYNTH_CUSTOM_FM
            case UI_VIEW_FM:
                if (event == BUTTON_PRESS_DOWN) synth_ui_fm_handle_button();
                return;
#endif
            default:  /* UI_VIEW_SEQ */
                break;
        }
        /* Editor-open moved to SHIFT+1; on SEQ the encoder push falls through to
         * the normal PRESS_DOWN queueing below. */
    }

    /* MY_BUTTON_0 is the open editor's commit/cancel button. Commit moved here
     * off the encoder long-press so finishing an edit is a single fast tap
     * instead of a ~1.5 s hold (the editing-loop bottleneck):
     *   short tap  -> commit & close
     *   long press -> cancel / discard (the deliberate, rarer gesture)
     * STEPEDIT has no discard path, so both gestures just close it (it also
     * still closes on MY_BUTTON_3, per its hint bar). All MY_BUTTON_0 events are
     * consumed while an editor is open, so transport (play/stop) is unavailable
     * here — already the convention for the cancel-on-hold editors. */
    if (button_id == MY_BUTTON_0 &&
        (v == UI_VIEW_GRAPH || v == UI_VIEW_FILTER ||
         v == UI_VIEW_LFO   || v == UI_VIEW_STEPEDIT)) {
        if (event == BUTTON_SINGLE_CLICK) {              /* tap = commit & close */
            if (v == UI_VIEW_FILTER)        synth_ui_filter_close_commit();
            else if (v == UI_VIEW_LFO)      synth_ui_lfo_close_commit();
            else if (v == UI_VIEW_STEPEDIT) synth_ui_stepedit_close();
            else                            synth_ui_graph_close_commit();
        } else if (event == BUTTON_LONG_PRESS_START) {   /* hold = cancel / discard */
            if (v == UI_VIEW_FILTER)        synth_ui_filter_handle_button(true);
            else if (v == UI_VIEW_LFO)      synth_ui_lfo_handle_button(true);
            else if (v == UI_VIEW_STEPEDIT) synth_ui_stepedit_close(); /* no discard path */
            else                            synth_ui_graph_handle_button(true);
        }
        return;
    }

    /* MY_BUTTON_0: short press = cycle active layer, long press = play/stop */
    if (button_id == MY_BUTTON_0) {
        if (event == BUTTON_SINGLE_CLICK) {
            synth_ui_cycle_active_layer();
        } else if (event == BUTTON_LONG_PRESS_START) {
            synth_ui_toggle_playing();
        }
        return;
    }

    /* All other buttons respond to PRESS_DOWN */
    if (event != BUTTON_PRESS_DOWN) return;

    switch (button_id) {
        case MY_BUTTON_ENC:
            synth_ui_handle_button();
            break;
        default:
            break;
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

            /* One precedence resolver drives both input routers and the draw
             * switch, so input and the OLED can never target different views.
             * The five overlays (canonical order FILTER>LFO>STEPEDIT>GRAPH>MENU)
             * capture the encoder outright; below them the mode screens are
             * dispatched by resolved view, with the patch-hold / drum-select
             * modifiers taking over exactly as before. */
            ui_view_id_t v = synth_ui_active_view();
            switch (v) {
                case UI_VIEW_FILTER:   synth_ui_filter_handle_encoder(steps);   goto next_poll;
                case UI_VIEW_LFO:      synth_ui_lfo_handle_encoder(steps);      goto next_poll;
                case UI_VIEW_STEPEDIT: synth_ui_stepedit_handle_encoder(steps); goto next_poll;
                case UI_VIEW_GRAPH:    synth_ui_graph_handle_encoder(steps);    goto next_poll;
                case UI_VIEW_MENU:     synth_ui_menu_handle_encoder(steps);     goto next_poll;
                default:               break;  /* fall through to the mode-tail */
            }

            if (s_patch_held) {
                // Plain patch hold+turn. On the arp screen this cycles the
                // arp's own patch; on the drone screen the drone's PATCH-mode
                // preset. On the sequencer screen it cycles the active layer's
                // patch: the melodic layer's shared patch, or — for a drum
                // layer — the SELECTED drum track's own patch (drums are
                // per-track Juno patches now).
                if (v == UI_VIEW_DRONE || v == UI_VIEW_DRONE_VIS) {
                    synth_ui_drone_cycle_patch((int)steps);
                } else if (v == UI_VIEW_ARP) {
                    synth_ui_arp_cycle_patch((int)steps);
                } else if (sequencer_core_get_layer_type(seq_get_active_layer_idx())
                           == SEQ_LAYER_DRUM) {
                    synth_ui_cycle_drum_patch((int)steps);
                } else {
                    synth_ui_cycle_melodic_patch((int)steps);
                }
            } else if (v == UI_VIEW_DRONE || v == UI_VIEW_DRONE_VIS) {
                // Drone screen: encoder moves the cursor / edits the focused row.
                synth_ui_drone_handle_encoder(steps);
            } else if (s_drum_select_held) {
                // Pitch-edit mode: hold MY_BUTTON_2 + turn encoder edits the
                // selected track's pitch (works for both drum and melodic).
                synth_ui_adjust_track_note((int)steps);
            } else if (v == UI_VIEW_ARP) {
                // Arp screen: encoder moves the cursor / edits the focused field.
                synth_ui_arp_handle_encoder(steps);
            } else if (v == UI_VIEW_PROG) {
                // Prog screen: encoder scrolls cursor / edits the focused entry field.
                synth_ui_prog_handle_encoder((int)steps);
            } else if (v == UI_VIEW_TRACKOPTS) {
                // Track Options screen: encoder scrolls rows / edits focused value.
                synth_ui_trackopts_handle_encoder((int)steps);
#if CONFIG_SYNTH_CUSTOM_FM
            } else if (v == UI_VIEW_FM) {
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
        // 8192: the patch-toggle gesture (MY_BUTTON_1 held + encoder turn)
        // runs the AMY patch-string parse INLINE on this task stack —
        // synth_ui_cycle_*_patch → amy_send_patch → patches_load_patch →
        // parse_patch_string_to_queue, whose amy_event frame (~828 B) plus the
        // nested amy_parse_message buffers drive peak usage to ~3.3-3.6 KB.
        // A 4096 stack could not absorb that inline frame plus an ISR frame
        // landing at the peak, causing intermittent patch-toggle stack
        // overflow (regression from the 8192->4096 trim in 4258556). Restored
        // to 8192 (~8 KB DRAM back, the amount the trim reclaimed). The
        // DRAM-preserving alternative is to port the amy_ingest pump task so
        // the parse runs on its own stack instead of the caller's.
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

#ifdef GAMMA9001
/* Feed AMY the gamma9001 drum-bank blob (PCM presets 256-391) from the
 * 'drums' data partition. Preferred path is a read-only flash mmap of
 * exactly the blob's size, but PSRAM XIP (SPIRAM_FETCH_INSTRUCTIONS +
 * SPIRAM_RODATA) leaves too few data-cache MMU pages for ~3.6 MB on this
 * config, so the fallback copies the blob into PSRAM heap instead (~3.6 MB
 * of the ~7 MB free; same placement class as the FX delay lines). Every
 * failure path is non-fatal: AMY keeps those presets disabled. */
static void gamma9001_pcm_mount(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "drums");
    if (part == NULL) {
        ESP_LOGW(TAG, "gamma9001: no 'drums' partition; PCM presets 256+ unavailable");
        return;
    }

    /* A never-flashed partition reads as erased flash (all 0xFF); don't hand
     * AMY full-scale noise as samples. */
    uint16_t probe[4];
    if (esp_partition_read(part, 0, probe, sizeof(probe)) != ESP_OK ||
        (probe[0] == 0xFFFFu && probe[1] == 0xFFFFu &&
         probe[2] == 0xFFFFu && probe[3] == 0xFFFFu)) {
        ESP_LOGW(TAG, "gamma9001: 'drums' partition is blank (flash drums.bin); "
                      "PCM presets 256+ unavailable");
        return;
    }

    size_t bytes = amy_gamma9001_pcm_bytes();
    if (bytes == 0 || bytes > part->size) {
        ESP_LOGW(TAG, "gamma9001: blob size %u exceeds partition (%u); "
                      "PCM presets 256+ unavailable",
                 (unsigned)bytes, (unsigned)part->size);
        return;
    }

    const void *map = NULL;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(part, 0, bytes, ESP_PARTITION_MMAP_DATA,
                           &map, &handle) == ESP_OK) {
        amy_set_gamma9001_pcm((const int16_t *)map);
        ESP_LOGI(TAG, "gamma9001: drum banks flash-mapped (%u KB)",
                 (unsigned)(bytes / 1024u));
        return;
    }

    /* MMU pages exhausted: stage the blob in PSRAM instead. */
    int16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGW(TAG, "gamma9001: mmap failed and PSRAM alloc of %u KB failed; "
                      "PCM presets 256+ unavailable", (unsigned)(bytes / 1024u));
        return;
    }
    if (esp_partition_read(part, 0, buf, bytes) != ESP_OK) {
        ESP_LOGW(TAG, "gamma9001: partition read failed; PCM presets 256+ unavailable");
        free(buf);
        return;
    }
    amy_set_gamma9001_pcm(buf);
    ESP_LOGI(TAG, "gamma9001: drum banks copied to PSRAM (%u KB; flash mmap unavailable)",
             (unsigned)(bytes / 1024u));
}
#endif /* GAMMA9001 */

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
#ifdef GAMMA9001
    gamma9001_pcm_mount();
#endif

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

    /* Project storage: non-fatal if the partition is absent/corrupt. Mounted
     * before synth_ui_init so the snapshot selftest inside it can run against
     * the boot layers, single-threaded, before the layers-applier task is
     * registered. */
    project_fs_init();
    if (project_fs_ok()) project_store_cleanup_tmp();
#if CONFIG_SYNTH_PROJECT_SELFTEST
    project_store_selftest();
#endif

    /* synth_ui_init adds the boot layers (drum + first melodic) itself,
     * single-threaded on this task's stack, before the UI task starts. */
    synth_ui_init(s_u8g2);
    HEAP_CHECK("after synth_ui_init");
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
    /* Debug builds assert no ingress helper runs on the render body. */
    amy_helpers_set_render_task(amy_render_task_handle);

    ESP_LOGI(TAG, "AMY + USB Audio ready (48 kHz stereo to PC)");

    // Initialize push buttons (GPIO15, GPIO18, GPIO8, GPIO42)
    ESP_LOGI(TAG, "[startup] before my_buttons_init");
    s_button_queue = xQueueCreate(BUTTON_QUEUE_DEPTH, sizeof(button_msg_t));
    if (s_button_queue == NULL) {
        ESP_LOGW(TAG, "Button queue creation failed; callbacks will run inline");
    } else {
        // 8192: carries the full dispatch_button_event chain (synth_ui_* /
        // sequencer_core_* setters), which shares the patch-toggle parse path
        // that runs amy_parse_message's ~3.4 KB frame INLINE on the caller
        // stack. A 4096 stack could not absorb that inline frame plus an ISR
        // frame, giving this task the same intermittent stack-overflow exposure
        // as encoder_task (regression from the 8192->4096 trim in 4258556).
        // Restored to 8192 (~8 KB DRAM back). Longer-term DRAM-preserving fix:
        // port the amy_ingest pump task to offload the parse onto its own stack.
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
