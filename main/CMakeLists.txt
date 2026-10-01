idf_component_register(
    SRCS "main.c"
         "ruwords_app.c"
         "ruwords_logic.c"
         "ruwords_adpcm.c"
         "ruwords_data.c"
         "ruwords_legacy_map.c"
         "../assets/fonts/ruw_font_20.c"
         "../assets/fonts/ruw_font_28.c"
         "../assets/fonts/ruw_font_36.c"
         "../assets/fonts/ruw_font_48.c"
    INCLUDE_DIRS "."
    EMBED_FILES "../assets/music/ruwords_voice.adpcm"
    REQUIRES bsp esp_timer esp_hw_support nvs_flash
)
