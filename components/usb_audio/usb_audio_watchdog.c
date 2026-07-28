#include "usb_audio_watchdog.h"

#if CONFIG_OUTPUT_WATCHDOG

#include "usb_audio.h"
#include <stddef.h>
#include <stdint.h>

/* Thresholds are in the int16 domain this build actually outputs: AMY's ESP
 * branch drops one bit after soft clipping (amy.c, ESP_PLATFORM), so full
 * scale here is ~16384 and AMY's soft-clip knee FIRST_NONLIN (29491) lands
 * at ~14745. */
#define OWD_SCAN_SAMPLES  2048u   /* ~21 ms of 48 kHz stereo per poll */
#define OWD_CLIP_PEAK     14700   /* peak at/above the soft-clip knee */
#define OWD_LOUD_MEAN     6500    /* mean-abs of a ~-4 dBFS sine: only
                                   * near-full-scale sustained material trips,
                                   * not drones at normal volume */
#define OWD_CLIP_POLLS    10      /* ~0.5 s at synth_ui's 50 ms cadence */
#define OWD_LOUD_POLLS    50      /* ~2.5 s */

/* All state is owned by the single polling task (synth_ui_task). */
static size_t   s_last_write_idx = (size_t)-1;
static uint16_t s_clip_polls;
static uint16_t s_loud_polls;
static output_wd_state_t s_state = OUTPUT_WD_OK;

void output_wd_poll(void)
{
    int32_t peak, mean;
    size_t idx;
    if (!usb_audio_peek_levels(OWD_SCAN_SAMPLES, &peak, &mean, &idx)
        || idx == s_last_write_idx) {
        /* Driver down, or no new audio since the last poll: nothing audible to
         * warn about, and the window under the write index is stale. */
        s_clip_polls = 0;
        s_loud_polls = 0;
        s_state = OUTPUT_WD_OK;
        return;
    }
    s_last_write_idx = idx;

    /* Consecutive-poll hysteresis separates a fault from music: program
     * material has decays and gaps that reset the counters within a bar, a
     * stuck voice does not. Saturate so a tripped state cannot wrap to OK. */
    if (peak >= OWD_CLIP_PEAK) {
        if (s_clip_polls < OWD_CLIP_POLLS) ++s_clip_polls;
    } else {
        s_clip_polls = 0;
    }
    if (mean >= OWD_LOUD_MEAN) {
        if (s_loud_polls < OWD_LOUD_POLLS) ++s_loud_polls;
    } else {
        s_loud_polls = 0;
    }

    if (s_clip_polls >= OWD_CLIP_POLLS)      s_state = OUTPUT_WD_CLIPPING;
    else if (s_loud_polls >= OWD_LOUD_POLLS) s_state = OUTPUT_WD_LOUD;
    else                                     s_state = OUTPUT_WD_OK;
}

output_wd_state_t output_wd_state(void)
{
    return s_state;
}

#endif /* CONFIG_OUTPUT_WATCHDOG */
