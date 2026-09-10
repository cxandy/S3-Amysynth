idf_component_register(SRCS
    "usb_importer.c"
    PRIV_REQUIRES synth_core usb_device_uac soc espressif__tinyusb
    INCLUDE_DIRS "include"
)