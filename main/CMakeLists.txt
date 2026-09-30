idf_component_register(SRCS "main.c" "board.c" "wifi.c" "config.c" "portal.c" "abs_api.c" "player.c" "catalog.c" "cover.c" "storage.c" "download.c" "battery.c" "power.c" "carousel.c" "switcher.c" "ui.c" "ui_home.c" "ui_library.c" "ui_player.c" "ui_sheet.c" "ui_episodes.c" "ui_settings.c" "ui_setup.c" "ui_status.c" "ui_capture.c" "lv_mem_psram.c"
                       INCLUDE_DIRS "."
                       # lv_mem_psram.c provides functions LVGL calls; keep them despite link order.
                       WHOLE_ARCHIVE)
