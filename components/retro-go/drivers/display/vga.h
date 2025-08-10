#include "rg_system.h"
#include "rg_display.h"

#include <stdlib.h>
#include <string.h>

#ifndef LCD_BUFFER_LENGTH
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 4) // In pixels
#endif

bool lcd_init();
uint16_t *lcd_get_buffer(size_t length);
// #define lcd_deinit()
// #define lcd_sync()
// #define lcd_set_backlight(l)
bool lcd_deinit();
bool lcd_sync();
bool lcd_set_backlight(float percent);
bool lcd_send_buffer(uint16_t* data, size_t len);
bool lcd_set_window(int x, int y, int width, int height);
const rg_display_driver_t rg_display_driver_vga = {
    .name = "vga",
    .init = lcd_init,
    .deinit = lcd_deinit,
    .sync = lcd_sync,
    .set_backlight = lcd_set_backlight,
    .set_window = lcd_set_window,
    .get_buffer = lcd_get_buffer,
    .send_buffer = lcd_send_buffer,
};
