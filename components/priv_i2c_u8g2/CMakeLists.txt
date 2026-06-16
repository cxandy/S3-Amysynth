idf_component_register(
    SRCS "patch_names.c" "priv_i2c_u8g2.c" "priv_u8g2_seq.c" "graph_popup.c"
         "priv_u8g2_menu.c" "priv_u8g2_arp.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio u8g2
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wstack-usage=1024")