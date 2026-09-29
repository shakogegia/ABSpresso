idf_component_register(SRCS "main.c" "board.c" "wifi.c" "abs_api.c" "player.c" "ui.c" "ui_home.c" "ui_library.c" "ui_player.c" "carousel.c" "cover.c" "lv_mem_psram.c"
                       INCLUDE_DIRS "."
                       # lv_mem_psram.c provides functions LVGL calls; keep them despite link order.
                       WHOLE_ARCHIVE)
