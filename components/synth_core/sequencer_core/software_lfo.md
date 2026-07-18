 Runs on a timer at ~20 Hz (every 50 ms), from a normal low-priority
 task -- NOT inside the audio render callback.

 One instance of this state per LFO'd synth:
   phase   : float 0..1     (free-running, persists between ticks)
   held    : float -1..1    (sample-and-hold value, RANDOM wave only)
 Config per synth: wave, rate_hz, depth (0..1), targets, and the
 known base filter cutoff.

every 50 ms, for each synth with an LFO enabled:

1. advance the oscillator by hand

```
    phase += rate_hz * 0.050
    if phase >= 1.0:
        phase -= 1.0
        if wave == RANDOM: held = new_random(-1..1)
```
2. evaluate the waveform at this phase -> value in -1..+1
   
```
    value = sine / triangle / saw_up / saw_down / square (phase)
            or `held` for RANDOM

    d = depth   # 0..1
```

3. inject: one amy.send() per tick, rewriting the CONSTANT term
        of each modulated parameter's ControlCoefficients.
        (Only the named coefficient changes; note/vel/env/mod
        coefficients the patch set up are left alone.)
```
    amy.send(synth=s,
        filter_freq = base_cutoff * 2**(3 * d * value),  # +/-3 octave log sweep
        amp         = 1.0 - d * (0.5 - 0.5 * value),     # dips 1.0 -> 1.0-d, never boosts
        freq        = 2**(d * value),                    # +/-1 octave pitch multiplier
        pan         = 0.5 + 0.5 * d * value)             # around center
```
*(only the parameters the user actually targeted are included)*

> this deliberately does not use AMY's mod_source LFO mechanism. A mod_source needs a free oscillator, and inside a loaded patch voice (Juno, DX7) there are none - every osc is the output, the patch's own LFO, or an FM operator/chained layer, and naming one as a mod_source mutes it (fatal for DX7 carriers). So instead, the modulation is done from outside: a control-rate task recomputes each target parameter's constant term and re-sends it as a normal parameter update, exactly as if a very fast hand were turning the knob 20 times a second. The patch's internal structure is untouched; the trade-offs are 50 ms zipper-stepping (audible on square/random-to-amp without slew) and that the patch's original constants on the modulated parameters are overwritten rather than restored when the LFO turns off.