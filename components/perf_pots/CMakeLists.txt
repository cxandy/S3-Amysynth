# Analog performance pots: filter cutoff / master volume / FX, sampled
# from the ESP32-S3 ADC1 in a low-priority task and pushed into the synth
# (FX becomes a CC1 mod wheel for the live slot under CONFIG_SYNTH_WIRELESS).
idf_component_register(SRCS
    "perf_pots.c"
    PRIV_REQUIRES esp_adc synth_core
    INCLUDE_DIRS "include"
)