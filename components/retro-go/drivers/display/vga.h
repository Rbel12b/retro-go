#include "rg_system.h"
#include "rg_display.h"

#include <stdlib.h>
#include <string.h>

#ifndef LCD_BUFFER_LENGTH
#define LCD_BUFFER_LENGTH (RG_SCREEN_WIDTH * 4) // In pixels
#endif

void lcd_init();
uint16_t *lcd_get_buffer(int);
#define lcd_deinit()
#define lcd_sync()
#define lcd_set_backlight(l)
void lcd_send_buffer(uint16_t* data, size_t len);
void lcd_set_window(int x, int y, int width, int height);
const rg_display_driver_t rg_display_driver_vga = {
    .name = "vga",
};
