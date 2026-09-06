# Follow an external MIDI Clock from the DIN port / BLE: lock the sequencer
# tempo to the master's 0xF8 stream and follow its song start/stop.
idf_component_register(SRCS
    "midi_clock_slave.c"
    PRIV_REQUIRES synth_core esp_timer
    INCLUDE_DIRS "include"
)