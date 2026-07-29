# Diagnostics: opt-in instrumentation (render timing, heap-integrity probes)
# shared by the app and every other component.
#
# Dependency direction is one-way and must stay that way: `diagnostics` may be
# required by anything, and requires nothing project-local except `amy` (block
# size / sample rate define the render budget, and the render-block snapshot
# reads two AMY counters). Depending on synth_core, usb_audio or display from
# here would create a cycle, since those are the components that instrument.
idf_component_register(SRCS
    "render_stats.c"
    PRIV_REQUIRES amy esp_hw_support
    INCLUDE_DIRS "include"
)
