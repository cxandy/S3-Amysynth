# PCM5102 standalone I2S DAC output. Owned by the render task; the audio block
# rendered by AMY is written here after each amy_update(). Requires
# esp_driver_i2s (IDF driver).
idf_component_register(SRCS
    "i2s_dac.c"
    PRIV_REQUIRES esp_driver_i2s
    INCLUDE_DIRS "include"
)