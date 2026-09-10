idf_component_register(SRCS
    "usb_importer.c"
    PRIV_REQUIRES synth_core usb_device_uac soc driver esp_driver_gpio
    INCLUDE_DIRS "include"
)