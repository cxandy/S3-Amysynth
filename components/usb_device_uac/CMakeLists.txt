set(priv_requires esp_timer)

if(${IDF_VERSION_MAJOR} LESS 6)
    list(APPEND priv_requires usb)       # USB PHY is part of usb component in IDF < 6.0
endif()

idf_component_register(SRCS usb_device_uac.c
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES ${priv_requires})

if(NOT CONFIG_USB_DEVICE_UAC_AS_PART)
        # Determine whether tinyusb is fetched from component registry or from local path
    idf_build_get_property(build_components BUILD_COMPONENTS)
    if(tinyusb IN_LIST build_components)
        set(tinyusb_name tinyusb) # Local component
    else()
        set(tinyusb_name espressif__tinyusb) # Managed component
    endif()

    idf_component_get_property(tusb_lib ${tinyusb_name} COMPONENT_LIB)
    target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb" "${COMPONENT_DIR}/tusb_uac")
    # PRIVATE so usb_descriptors.c is compiled only into the tinyusb archive
    # itself. PUBLIC propagates it into every consumer (usb_audio, display,
    # synth_core, harness, the project elfs' own objects...), and the 6
    # descriptor symbols it defines then collide with libsynth_core.a, which is
    # force-linked via WHOLE_ARCHIVE.
    target_sources(${tusb_lib} PRIVATE "${COMPONENT_DIR}/tusb/usb_descriptors.c")
    # WebSerial import CDC pump: needs the tinyusb public API (tusb.h), which
    # the component target itself does not see - only the tusb_lib archive
    # does. Same archive-member resolution as usb_descriptors.c.
    target_sources(${tusb_lib} PRIVATE "${COMPONENT_DIR}/usb_cdc_device.c")
    target_include_directories(${tusb_lib} PRIVATE "${COMPONENT_DIR}/include")
endif()

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
