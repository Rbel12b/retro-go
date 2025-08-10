#include "rg_system.h"
#include "rg_display.h"

#include <stdlib.h>
#include <string.h>

#ifndef LCD_BUFFER_LENGTH
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 4) // In pixels
#endif

void lcd_init();
uint16_t *lcd_get_buffer(size_t length);
void lcd_deinit();
void lcd_sync();
void lcd_set_backlight(float percent);
void lcd_send_buffer(uint16_t* data, size_t len);
void lcd_set_window(int x, int y, int width, int height);
const rg_display_driver_t rg_display_driver_vga = {
    .name = "vga",
    .init = NULL,
    .deinit = NULL,
    .sync = NULL,
    .set_backlight = NULL,
    .set_window = NULL,
    .get_buffer = NULL,
    .send_buffer = NULL,
};
