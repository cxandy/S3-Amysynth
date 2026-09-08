# AMYSONG import over WiFi (SoftAP + minimal HTTP server). Wires in the
# ESP-IDF wifi/netif stack; the parser itself lives in synth_core.
idf_component_register(SRCS
    "wifi_importer.c"
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event synth_core project_store
    INCLUDE_DIRS "include"
    REQUIRES lwip
)