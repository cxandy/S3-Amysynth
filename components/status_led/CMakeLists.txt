# Onboard WS2812 status LED: render-load color bands + alloc-failure blink.
# The led_strip managed component (RMT backend) is pulled in via
# idf_component.yml; diagnostics provides the core_load sampler.
idf_component_register(SRCS
    "status_led.c"
    PRIV_REQUIRES diagnostics heap esp_timer
    INCLUDE_DIRS "include"
)
