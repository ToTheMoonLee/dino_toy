set(src_dirs
            "LED"
            "SERVO"
            "BUTTON"
            "WIFI"
            "WAKE_WORD"
            "VOICE_CONTROL"
            "MP3_PLAYER"
)
set(include_dirs
            "LED"
            "SERVO"
            "BUTTON"
            "WIFI"
            "WAKE_WORD"
            "VOICE_CONTROL"
            "MP3_PLAYER"
)
set(requires
            driver
            espressif__servo
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            espressif__esp-sr
            chmorgan__esp-audio-player)

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires}
                       EMBED_FILES "MP3_PLAYER/dinosaur-roar.mp3"
)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Who-format -Wno-error=stringop-overflow)