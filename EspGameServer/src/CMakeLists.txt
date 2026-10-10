idf_component_register(
    SRCS "main.c" "game_client.c" "esp_bsp.c" "lv_port.c"
         "esp_lcd_axs15231b.c" "esp_lcd_touch.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer esp_wifi esp_event esp_netif nvs_flash
             json esp_websocket_client)
