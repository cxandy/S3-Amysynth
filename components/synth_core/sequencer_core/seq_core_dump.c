/* ── Sequencer state dump (DEV menu one-shot) ──
 * Prints every layer's track config and step exceptions to the console so
 * runtime-tuned values (drum pitches/presets above all) can be read back in
 * one block instead of scraped from per-edit log lines. Deliberately scoped
 * to sequencer layer/track/step state - global FX, arp and drone have their
 * own screens and are not this tool's job.
 *
 * Output discipline: plain printf (no ESP_LOG prefixes to strip when copying
 * from the monitor), BEGIN/END markers carrying the firmware version and a
 * printed-line count so a truncated copy is detectable. Track lines lead
 * with note= and the drum preset so the tuning harvest reads column-wise;
 * step state prints lazily - a pattern string per track, plus one line per
 * step only where something differs from the plain-step neutral values
 * (prob=100, ratchet=1, every<=1, note==track base, all else 0). */

#include "seq_core_internal.h"
#include "sequencer_core.h"
#include "esp_app_desc.h"
#include <stdio.h>

/* One printed line per call site, counted for the END marker. */
#define DP(...) do { printf(__VA_ARGS__); printf("\n"); n_lines++; } while (0)

static const char *const PCM_MODE_NAMES[] =
    { "DFLT", "PLAY", "LOOP", "LOOPST", "FRVR" };
static const char *const XFORM_NAMES[] =
    { "NONE", "RAND", "RAMPUP", "RAMPDN" };

static const char *pcm_mode_name(uint8_t m)
{
    return (m < sizeof PCM_MODE_NAMES / sizeof *PCM_MODE_NAMES)
               ? PCM_MODE_NAMES[m] : "?";
}

static const char *xform_name(uint8_t x)
{
    return (x < sizeof XFORM_NAMES / sizeof *XFORM_NAMES)
               ? XFORM_NAMES[x] : "?";
}

void sequencer_core_dump_state(void)
{
    unsigned n_lines = 0;

    DP("==== SEQDUMP BEGIN fw=%s layers=%u engine=%s ====",
       esp_app_get_description()->version, (unsigned)s_num_layers,
       sequencer_core_get_drum_engine() == SEQ_DRUM_PCM ? "PCM" : "SYNTH");

    for (uint8_t li = 0; li < s_num_layers; li++) {
        const seq_layer_t *L = &s_layers[li];
        bool drum = (L->type == SEQ_LAYER_DRUM);

        if (drum) {
            DP("L%u DRUM steps=%u swing=%u groove=%u",
               li + 1u, L->num_steps, L->swing_pct, L->groove_pct);
        } else {
            DP("L%u MELODIC steps=%u patch=%u voices=%u swing=%u gate=%u"
               " porta=%u groove=%u chord=%s root=%u type=%u",
               li + 1u, L->num_steps, L->patch, L->num_voices, L->swing_pct,
               L->gate_pct, L->portamento_ms, L->groove_pct,
               L->chord_mode ? "on" : "off", L->chord_root,
               (unsigned)L->chord_type);
        }

        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            /* Track line: pitch and timbre first (the harvest columns). */
            if (drum) {
                DP("  T%u note=%u patch=%u pcm=%u mode=%u(%s) rep=%u"
                   " mute=%u solo=%u",
                   t + 1u, L->track_base_note[t], L->track_patch[t],
                   L->track_pcm_preset[t], L->track_pcm_mode[t],
                   pcm_mode_name(L->track_pcm_mode[t]), L->repeat_rate[t],
                   (unsigned)L->mute[t], (unsigned)L->solo[t]);
            } else {
                DP("  T%u note=%u rep=%u mute=%u solo=%u",
                   t + 1u, L->track_base_note[t], L->repeat_rate[t],
                   (unsigned)L->mute[t], (unsigned)L->solo[t]);
            }

            char grid[SEQ_MAX_STEPS + 1];
            for (uint8_t s = 0; s < L->num_steps; s++)
                grid[s] = L->grid[t][s] ? 'X' : '.';
            grid[L->num_steps] = '\0';
            DP("  T%u |%s|", t + 1u, grid);

            /* Step exceptions: only active steps, only non-neutral fields. */
            for (uint8_t s = 0; s < L->num_steps; s++) {
                if (!L->grid[t][s]) continue;
                bool off_note = (L->step_note[t][s] != L->track_base_note[t]);
                bool dec = off_note ||
                           L->step_prob[t][s] != 100 ||
                           L->step_ratchet[t][s] != 1 ||
                           L->step_every[t][s] > 1 ||
                           L->step_prev[t][s] ||
                           L->step_transform[t][s] ||
                           L->step_quant_bypass[t][s] ||
                           L->step_nudge[t][s] ||
                           L->step_velocity_adj[t][s] ||
                           L->step_ratchet_taper[t][s];
                if (!dec) continue;

                char line[128];
                int n = snprintf(line, sizeof line, "  T%u s%u",
                                 t + 1u, s + 1u);
                #define AP(...) \
                    n += snprintf(line + n, sizeof line - (size_t)n, __VA_ARGS__)
                if (off_note)                    AP(" note=%u", L->step_note[t][s]);
                if (L->step_prob[t][s] != 100)   AP(" prob=%u", L->step_prob[t][s]);
                if (L->step_ratchet[t][s] != 1)  AP(" rat=%u", L->step_ratchet[t][s]);
                if (L->step_ratchet_taper[t][s]) AP(" taper=%d", L->step_ratchet_taper[t][s]);
                if (L->step_every[t][s] > 1)     AP(" every=%u", L->step_every[t][s]);
                if (L->step_prev[t][s])          AP(" prev");
                if (L->step_transform[t][s])     AP(" xform=%s", xform_name(L->step_transform[t][s]));
                if (L->step_quant_bypass[t][s])  AP(" qbyp");
                if (L->step_nudge[t][s])         AP(" nudge=%d", L->step_nudge[t][s]);
                if (L->step_velocity_adj[t][s])  AP(" vel=%d", L->step_velocity_adj[t][s]);
                #undef AP
                DP("%s", line);
            }
        }
    }

    DP("==== SEQDUMP END layers=%u lines=%u ====",
       (unsigned)s_num_layers, n_lines + 1u);
    fflush(stdout);
}
