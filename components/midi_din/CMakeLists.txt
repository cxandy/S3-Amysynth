# DIN-5 MIDI input: drains the UART RX that AMY's UART MIDI config wires to
# GPIO, feeds the shared midi_core parser (same live-play sink as BLE MIDI).
idf_component_register(SRCS
    "midi_din.c"
    REQUIRES amy
    PRIV_REQUIRES esp_driver_uart wireless
    INCLUDE_DIRS "include"
)