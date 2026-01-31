set(src_dirs
            "LED"
            "SERVO"
            "BUTTON"
            "WIFI"
            "WAKE_WORD"
            "VOICE_CONTROL"
            "MP3_PLAYER"
            "CLOUD_TTS"
            "CLOUD_CHAT"
            "VOICE_DIALOG"
            "STATE_MACHINE"
            "OTA"
            "WEBSOCKET_CHAT"
            "DISPLAY"
)
set(include_dirs
            "LED"
            "SERVO"
            "BUTTON"
            "WIFI"
            "WAKE_WORD"
            "VOICE_CONTROL"
            "MP3_PLAYER"
            "CLOUD_TTS"
            "CLOUD_CHAT"
            "VOICE_DIALOG"
            "STATE_MACHINE"
            "OTA"
            "WEBSOCKET_CHAT"
            "DISPLAY"
)
set(requires
            driver
            espressif__servo
            esp_wifi
            esp_event
            esp_netif
            esp_http_server
            esp_http_client
            nvs_flash
            espressif__esp-sr
            chmorgan__esp-audio-player
            esp_https_ota
            espressif__esp_websocket_client
            app_update
            esp_lcd
            json)

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires}
                       EMBED_FILES "MP3_PLAYER/dinosaur-roar.mp3"
)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Who-format -Wno-error=stringop-overflow)
