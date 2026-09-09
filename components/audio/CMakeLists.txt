# Only the source list is conditional. The requirements stay outside the
# branch because IDF resolves them before CONFIG_ values are visible here: a
# PRIV_REQUIRES written inside the enabled branch is silently dropped, and the
# component then fails to find driver/i2s_std.h.
if(CONFIG_KERN_AUDIO)
    set(audio_srcs "src/audio.c")
else()
    set(audio_srcs "")
endif()

idf_component_register(
    SRCS ${audio_srcs}
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio
)
