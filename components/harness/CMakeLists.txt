idf_component_register(
    SRCS "harness.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES synth_core diagnostics esp_driver_uart esp_app_format
)
