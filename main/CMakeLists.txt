idf_component_register(SRC_DIRS "." "hardware" "network" "app" "ui" "ui/widgets" "ui/pages"
                       INCLUDE_DIRS "." "hardware" "network" "app" "ui" "ui/widgets"
                       # lv_mem_psram.c provides functions LVGL calls; keep them despite link order.
                       WHOLE_ARCHIVE)
