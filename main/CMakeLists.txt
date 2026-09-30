idf_component_register(SRCS "power.c" "ui_status.c" "battery.c" "power.c" "ui_episodes.c" "catalog.c" "main.c" "board.c" "wifi.c" "abs_api.c" "player.c" "ui.c" "ui_home.c" "ui_library.c" "ui_player.c" "ui_sheet.c" "ui_status.c" "battery.c" "power.c" "ui_episodes.c" "catalog.c" "ui_settings.c" "switcher.c" "carousel.c" "cover.c" "lv_mem_psram.c" "storage.c" "download.c" "ui_capture.c"
                       INCLUDE_DIRS "."
                       # lv_mem_psram.c provides functions LVGL calls; keep them despite link order.
                       WHOLE_ARCHIVE)
