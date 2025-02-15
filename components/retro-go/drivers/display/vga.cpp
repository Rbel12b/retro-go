extern "C"
{
#include "vga.h"
}
#include "ESP32S3VGA.h"
VGA vga;
struct
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int pos = 0;
} lcd_window;

uint16_t lcd_buffer_vga[LCD_BUFFER_LENGTH + 1];
uint16_t *lcd_get_buffer(int)
{
    return (uint16_t *)lcd_buffer_vga;
}


void lcd_init()
{
    if (!vga.init(PinConfig(21, 39, 40, 41, 42, 16, 15, 7, 6, 5, 4, 10, 9, 8, 18, 17, 47, 48), Mode::MODE_320x240x60,
                  16))
    {
        RG_LOGE("Failed to initialive vga display");
        return;
    }
    vga.start();
}
void lcd_send_buffer(uint16_t *data, size_t len)
{
    uint16_t color = 0;
    for (size_t i = lcd_window.pos; i < (len + lcd_window.pos); i++)
    {
        color = data[i - lcd_window.pos];
        color = (color << 8) | (color >> 8);
        vga.dot((i % lcd_window.width) + lcd_window.x, (i / lcd_window.width) + lcd_window.y, color);
    }
    lcd_window.pos += len;
    vga.show();
}
void lcd_set_window(int x, int y, int width, int height)
{
    lcd_window.x = x;
    lcd_window.y = y;
    lcd_window.width = width;
    lcd_window.height = height;
    lcd_window.pos = 0;
}