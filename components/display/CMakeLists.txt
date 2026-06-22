idf_component_register(
    SRCS "patch_names.c" "priv_i2c_u8g2.c" "display_seq.c" "graph_popup.c"
         "display_menu.c" "display_arp.c" "display_drone.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio u8g2
    PRIV_REQUIRES g_clamp
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wstack-usage=1024")