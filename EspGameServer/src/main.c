#include "display.h"
#include "esp_bsp.h"
#include "lv_port.h"
#include "game_client.h"
#include "esp_check.h"

void app_main(void)
{
    /* Full-screen buffer with 90-degree landscape rotation. */
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
        .rotate = LV_DISP_ROT_90,
    };
    ESP_ERROR_CHECK(bsp_display_start_with_config(&cfg) ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    bsp_display_lock(0);
    game_client_init();
    bsp_display_unlock();
}
