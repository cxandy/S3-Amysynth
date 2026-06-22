idf_component_register(SRCS "arp_core.c" "sequencer_core.c" "synth_ui.c" "quantizer.c" "graph_popup_amy.c"
                            "custompatches/drone_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES u8g2 display amy
                       PRIV_REQUIRES g_clamp)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wstack-usage=1024")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/synth_ui.c"
    PROPERTIES COMPILE_OPTIONS "-Os"
)
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/sequencer_core.c"
    PROPERTIES COMPILE_OPTIONS "-Os"
)
