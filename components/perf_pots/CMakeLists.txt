# Analog performance pots: filter cutoff / master volume / space (echo+reverb
# send), sampled from the ESP32-S3 ADC1 in a low-priority task and pushed into
# the synth. The cutoff knob sweeps every melodic layer AND the live-play slot
# with a resonance ramp; the space knob drives the global FX bus. Each role
# defaults to -1 (disabled) so the upstream devkit is unaffected.
idf_component_register(SRCS
    "perf_pots.c"
    PRIV_REQUIRES esp_adc synth_core amy
    INCLUDE_DIRS "include"
)