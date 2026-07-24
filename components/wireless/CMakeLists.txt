# On-demand wireless services (BLE MIDI). Sources compile only when
# CONFIG_SYNTH_WIRELESS is enabled; otherwise this registers as a header-only
# component so dependents can keep an unconditional PRIV_REQUIRES on it.
set(srcs)
if(CONFIG_SYNTH_WIRELESS)
    list(APPEND srcs
        "radio_manager.c"
        "ble_midi.c"
        "midi_core.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt nvs_flash
)
