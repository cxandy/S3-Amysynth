#ifndef _DELAY_H

// How many bits used for fractional part of delay line index.
#define DELAY_INDEX_FRAC_BITS 15
// The number of bits used to hold the delay line index.
#define DELAY_INDEX_BITS (31 - DELAY_INDEX_FRAC_BITS)

#include "amy.h"

delay_line_t *new_delay_line(int len, int fixed_delay, int ram_type /* e.g. MALLOC_CAP_INTERNAL */);
void free_delay_line(delay_line_t *d);

void apply_variable_delay(SAMPLE *block, delay_line_t *delay_line, SAMPLE *delay_samples, SAMPLE mod_scale, SAMPLE mix_level, SAMPLE feedback_level);
void apply_fixed_delay(SAMPLE *block, delay_line_t *delay_line, uint32_t delay_samples, SAMPLE mix_level, SAMPLE feedback, SAMPLE filter_coef);

void config_stereo_reverb(float a_liveness, float crossover_hz, float damping);
// LOCAL EDIT (2026-06-19): init_stereo_reverb() return type void->bool and new
// stereo_reverb_ready() for reverb OOM crash-safety. See AMY-EDITS.md.
// Returns true if all reverb delay lines were allocated, false on OOM
// (in which case all delay lines are freed and reverb must stay disabled).
bool init_stereo_reverb(void);
// True when the reverb delay lines are allocated and stereo_reverb() is safe to call.
bool stereo_reverb_ready(void);
void stereo_reverb(SAMPLE *r_in, SAMPLE *l_in, SAMPLE *r_out, SAMPLE *l_out, int n_samples, SAMPLE level);

#endif // !_DELAY_H
