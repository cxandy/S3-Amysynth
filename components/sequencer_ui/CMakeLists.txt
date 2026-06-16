idf_component_register(SRCS "arp_core.c" "sequencer_core.c" "sequencer_ui.c" "quantizer.c" "graph_popup_amy.c"
                       INCLUDE_DIRS "include"
                       REQUIRES u8g2 priv_i2c_u8g2 amy)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wstack-usage=1024")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/sequencer_ui.c"
    PROPERTIES COMPILE_OPTIONS "-Os"
)
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/sequencer_core.c"
    PROPERTIES COMPILE_OPTIONS "-Os"
)