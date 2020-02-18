#pragma GCC optimize ("O3")

#include "odroid_display.h"
#include "odroid_settings.h"
#include "odroid_colors.h"
#include "odroid_image_sdcard.h"
#include "odroid_image_hourglass.h"

#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"

#include <string.h>

const int DUTY_MAX = 0x1fff;

const gpio_num_t SPI_PIN_NUM_MISO = GPIO_NUM_19;
const gpio_num_t SPI_PIN_NUM_MOSI = GPIO_NUM_23;
const gpio_num_t SPI_PIN_NUM_CLK  = GPIO_NUM_18;
const gpio_num_t LCD_PIN_NUM_CS   = GPIO_NUM_5;
const gpio_num_t LCD_PIN_NUM_DC   = GPIO_NUM_21;
const gpio_num_t LCD_PIN_NUM_BCKL = GPIO_NUM_14;

const int LCD_BACKLIGHT_ON_VALUE = 1;
const int LCD_SPI_CLOCK_RATE = 48000000;

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

#define LINE_BUFFERS (2)
#define LINE_COUNT (5)
#define LINE_BUFFER_SIZE (SCREEN_WIDTH*LINE_COUNT)

#define SPI_TRANSACTION_COUNT (4)

// The number of pixels that need to be updated to use interrupt-based updates
// instead of polling.
#define POLLING_PIXEL_THRESHOLD (LINE_BUFFER_SIZE)

// Maximum amount of change (percent) in a frame before we trigger a full transfer
// instead of a partial update (faster). This also allows us to stop the diff early!
#define FULL_UPDATE_THRESHOLD (0.4f)

static uint16_t* line[LINE_BUFFERS];
static SemaphoreHandle_t display_mutex;
static QueueHandle_t spi_queue;
static QueueHandle_t line_buffer_queue;
static SemaphoreHandle_t spi_count_semaphore;
static spi_transaction_t global_transaction;
static spi_transaction_t trans[SPI_TRANSACTION_COUNT];
static spi_device_handle_t spi;
static bool use_polling = false;

static int BacklightLevels[] = {10, 25, 50, 75, 100};
static int BacklightLevel = ODROID_BACKLIGHT_LEVEL2;

/*
 The ILI9341 needs a bunch of command/argument values to be initialized. They are stored in this struct.
*/
typedef struct {
    uint8_t cmd;
    uint8_t data[128];
    uint8_t databytes; //No of data in data; bit 7 = delay after set; 0xFF = end of cmds.
} ili_init_cmd_t;

#define TFT_CMD_SWRESET	0x01
#define TFT_CMD_SLEEP 0x10
#define TFT_CMD_DISPLAY_OFF 0x28

#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_ML  0x10
#define MADCTL_MH 0x04
#define TFT_RGB_BGR 0x08

DRAM_ATTR static const ili_init_cmd_t ili_sleep_cmds[] = {
    {TFT_CMD_SWRESET, {0}, 0x80},
    {TFT_CMD_DISPLAY_OFF, {0}, 0x80},
    {TFT_CMD_SLEEP, {0}, 0x80},
    {0, {0}, 0xff}
};


// 2.4" LCD
DRAM_ATTR static const ili_init_cmd_t ili_init_cmds[] = {
    // VCI=2.8V
    //************* Start Initial Sequence **********//
    {TFT_CMD_SWRESET, {0}, 0x80},
    {0xCF, {0x00, 0xc3, 0x30}, 3},
    {0xED, {0x64, 0x03, 0x12, 0x81}, 4},
    {0xE8, {0x85, 0x00, 0x78}, 3},
    {0xCB, {0x39, 0x2c, 0x00, 0x34, 0x02}, 5},
    {0xF7, {0x20}, 1},
    {0xEA, {0x00, 0x00}, 2},
    {0xC0, {0x1B}, 1},    //Power control   //VRH[5:0]
    {0xC1, {0x12}, 1},    //Power control   //SAP[2:0];BT[3:0]
    {0xC5, {0x32, 0x3C}, 2},    //VCM control
    {0xC7, {0x91}, 1},    //VCM control2
    //{0x36, {(MADCTL_MV | MADCTL_MX | TFT_RGB_BGR)}, 1},    // Memory Access Control
    {0x36, {(MADCTL_MV | MADCTL_MY | TFT_RGB_BGR)}, 1},    // Memory Access Control
    {0x3A, {0x55}, 1},
    {0xB1, {0x00, 0x10}, 2},  // Frame Rate Control (1B=70, 1F=61, 10=119)
    {0xB6, {0x0A, 0xA2}, 2},    // Display Function Control
    {0xF6, {0x01, 0x30}, 2},
    {0xF2, {0x00}, 1},    // 3Gamma Function Disable
    {0x26, {0x01}, 1},     //Gamma curve selected

    //Set Gamma
    {0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}, 15},
    {0XE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15},

/*
    // LUT
    {0x2d, {0x01, 0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d, 0x0f, 0x11, 0x13, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x1f,
            0x21, 0x23, 0x25, 0x27, 0x29, 0x2b, 0x2d, 0x2f, 0x31, 0x33, 0x35, 0x37, 0x39, 0x3b, 0x3d, 0x3f,
            0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
            0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
            0x1d, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x26, 0x27, 0x28, 0x29, 0x2a,
            0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
            0x00, 0x00, 0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x12, 0x14, 0x16, 0x18, 0x1a,
            0x1c, 0x1e, 0x20, 0x22, 0x24, 0x26, 0x26, 0x28, 0x2a, 0x2c, 0x2e, 0x30, 0x32, 0x34, 0x36, 0x38}, 128},
*/

    {0x11, {0}, 0x80},    //Exit Sleep
    {0x29, {0}, 0x80},    //Display on

    {0, {0}, 0xff}
};


static inline uint16_t* line_buffer_get()
{
    uint16_t* buffer;
    if (use_polling) {
        return line[0];
    }

    if (xQueueReceive(line_buffer_queue, &buffer, 1000 / portTICK_RATE_MS) != pdTRUE)
    {
        abort();
    }

    return buffer;
}

static inline void line_buffer_put(uint16_t* buffer)
{
    if (xQueueSend(line_buffer_queue, &buffer, 1000 / portTICK_RATE_MS) != pdTRUE)
    {
        abort();
    }
}

static void spi_task(void *arg)
{
    printf("%s: Entered.\n", __func__);

    while(1)
    {
        // Ensure only LCD transactions are pulled
        if(xSemaphoreTake(spi_count_semaphore, portMAX_DELAY) == pdTRUE )
        {
            spi_transaction_t* t;

            esp_err_t ret = spi_device_get_trans_result(spi, &t, portMAX_DELAY);
            assert(ret==ESP_OK);

            int dc = (int)t->user & 0x80;
            if(dc)
            {
                line_buffer_put(t->tx_buffer);
            }

            if(xQueueSend(spi_queue, &t, portMAX_DELAY) != pdPASS)
            {
                abort();
            }
        }
        else
        {
            printf("%s: xSemaphoreTake failed.\n", __func__);
        }
    }

    printf("%s: Exiting.\n", __func__);
    vTaskDelete(NULL);

    while (1) {}
}

static void spi_initialize()
{
    spi_queue = xQueueCreate(SPI_TRANSACTION_COUNT, sizeof(void*));
    if(!spi_queue) abort();

    line_buffer_queue = xQueueCreate(LINE_BUFFERS, sizeof(void*));
    if(!line_buffer_queue) abort();

    spi_count_semaphore = xSemaphoreCreateCounting(SPI_TRANSACTION_COUNT, 0);
    if (!spi_count_semaphore) abort();

    xTaskCreatePinnedToCore(&spi_task, "spi_task", 1024 + 768, NULL, 5, NULL, 1);
}

static inline spi_transaction_t* spi_get_transaction()
{
    spi_transaction_t* t;

    if (use_polling) {
        t = &global_transaction;
    } else {
        xQueueReceive(spi_queue, &t, portMAX_DELAY);
    }

    memset(t, 0, sizeof(*t));

    return t;
}

static inline void spi_put_transaction(spi_transaction_t* t)
{
    t->rx_buffer = NULL;
    t->rxlength = t->length;

    if (t->flags & SPI_TRANS_USE_TXDATA)
    {
        t->flags |= SPI_TRANS_USE_RXDATA;
    }

    if (use_polling) {
        spi_device_polling_transmit(spi, t);
    } else {
        esp_err_t ret = spi_device_queue_trans(spi, t, portMAX_DELAY);
        assert(ret==ESP_OK);

        xSemaphoreGive(spi_count_semaphore);
    }
}


//Send a command to the ILI9341. Uses spi_device_transmit, which waits until the transfer is complete.
static void ili_cmd(const uint8_t cmd)
{
    spi_transaction_t* t = spi_get_transaction();

    t->length = 8;                     //Command is 8 bits
    t->tx_data[0] = cmd;               //The data is the cmd itself
    t->user = (void*)0;                //D/C needs to be set to 0
    t->flags = SPI_TRANS_USE_TXDATA;

    spi_put_transaction(t);
}

//Send data to the ILI9341. Uses spi_device_transmit, which waits until the transfer is complete.
static void ili_data(const uint8_t *data, int len)
{
    if (len)
    {
        spi_transaction_t* t = spi_get_transaction();

        if (len < 5)
        {
            for (short i = 0; i < len; ++i)
            {
                t->tx_data[i] = data[i];
            }
            t->length = len * 8;               //Len is in bytes, transaction length is in bits.
            t->user = (void*)1;                //D/C needs to be set to 1
            t->flags = SPI_TRANS_USE_TXDATA;
        }
        else
        {
            t->length = len * 8;               //Len is in bytes, transaction length is in bits.
            t->tx_buffer = data;               //Data
            t->user = (void*)1;                //D/C needs to be set to 1
            t->flags = 0;
        }

        spi_put_transaction(t);
    }
}

//This function is called (in irq context!) just before a transmission starts. It will
//set the D/C line to the value indicated in the user field.
static void ili_spi_pre_transfer_callback(spi_transaction_t *t)
{
    gpio_set_level(LCD_PIN_NUM_DC, (int)t->user & 0x01);
}

//Initialize the display
static void ili_init()
{
    short cmd = 0;

    //Initialize non-SPI GPIOs
    gpio_set_direction(LCD_PIN_NUM_DC, GPIO_MODE_OUTPUT);
    //gpio_set_direction(LCD_PIN_NUM_BCKL, GPIO_MODE_OUTPUT);

    //Send all the commands
    while (ili_init_cmds[cmd].databytes != 0xff)
    {
        ili_cmd(ili_init_cmds[cmd].cmd);

        int len = ili_init_cmds[cmd].databytes & 0x7f;
        if (len) ili_data(ili_init_cmds[cmd].data, len);

        // if (ili_init_cmds[cmd].databytes & 0x80)
        // {
        //     vTaskDelay(10 / portTICK_RATE_MS);
        // }

        cmd++;
    }
}

static inline void send_reset_column(short left, short right, int len)
{
    ili_cmd(0x2A);
    const uint8_t data[] = { (left) >> 8, (left) & 0xff, right >> 8, right & 0xff };
    ili_data(data, len);
}

static inline void send_reset_page(short top, short bottom, int len)
{
    ili_cmd(0x2B);
    const uint8_t data[] = { top >> 8, top & 0xff, bottom >> 8, bottom & 0xff };
    ili_data(data, len);
}

static void send_reset_drawing(short left, short top, short width, short height)
{
    static short last_left = -1;
    static short last_right = -1;
    static short last_top = -1;
    static short last_bottom = -1;

    short right = left + width - 1;
    if (height == 1) {
        if (last_right > right) right = last_right;
        else right = SCREEN_WIDTH - 1;
    }
    if (left != last_left || right != last_right) {
        send_reset_column(left, right, (right != last_right) ?  4 : 2);
        last_left = left;
        last_right = right;
    }

    //int bottom = (top + height - 1);
    short bottom = SCREEN_HEIGHT - 1;
    if (top != last_top || bottom != last_bottom) {
        send_reset_page(top, bottom, (bottom != last_bottom) ? 4 : 2);
        last_top = top;
        last_bottom = bottom;
    }

    ili_cmd(0x2C);           //memory write
    if (height > 1) {
        ili_cmd(0x3C);           //memory write continue
    }
}

static void send_continue_line(uint16_t *line, int width, int lineCount)
{
    spi_transaction_t* t = spi_get_transaction();
    t->length = width * 2 * lineCount * 8;
    t->tx_buffer = line;
    t->user = (void*)0x81;
    t->flags = 0;

    spi_put_transaction(t);
}

static void backlight_init()
{
    //configure timer0
    ledc_timer_config_t ledc_timer;
    memset(&ledc_timer, 0, sizeof(ledc_timer));

    ledc_timer.bit_num = LEDC_TIMER_13_BIT; //set timer counter bit number
    ledc_timer.freq_hz = 5000;              //set frequency of pwm
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;   //timer mode,
    ledc_timer.timer_num = LEDC_TIMER_0;    //timer index


    ledc_timer_config(&ledc_timer);


    //set the configuration
    ledc_channel_config_t ledc_channel;
    memset(&ledc_channel, 0, sizeof(ledc_channel));

    //set LEDC channel 0
    ledc_channel.channel = LEDC_CHANNEL_0;
    //set the duty for initialization.(duty range is 0 ~ ((2**bit_num)-1)
    ledc_channel.duty = (LCD_BACKLIGHT_ON_VALUE) ? 0 : DUTY_MAX;
    //GPIO number
    ledc_channel.gpio_num = LCD_PIN_NUM_BCKL;
    //GPIO INTR TYPE, as an example, we enable fade_end interrupt here.
    ledc_channel.intr_type = LEDC_INTR_FADE_END;
    //set LEDC mode, from ledc_mode_t
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    //set LEDC timer source, if different channel use one timer,
    //the frequency and bit_num of these channels should be the same
    ledc_channel.timer_sel = LEDC_TIMER_0;


    ledc_channel_config(&ledc_channel);


    //initialize fade service.
    ledc_fade_func_install(0);

    BacklightLevel = odroid_settings_Backlight_get();
    odroid_display_backlight_set(BacklightLevel);
}

static void backlight_deinit()
{
    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (LCD_BACKLIGHT_ON_VALUE) ? 0 : DUTY_MAX, 100);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_WAIT_DONE);
    ledc_fade_func_uninstall();
}

static void backlight_percentage_set(int value)
{
    int duty = DUTY_MAX * (value * 0.01f);

    ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty, 10);
    ledc_fade_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_FADE_NO_WAIT);
}

int odroid_display_backlight_get()
{
    return BacklightLevel;
}

void odroid_display_backlight_set(int level)
{
    if (level < 0)
    {
        printf("odroid_display_backlight_set: level out of range (%d)\n", level);
        level = 0;
    }
    else if (level >= ODROID_BACKLIGHT_LEVEL_COUNT)
    {
        printf("odroid_display_backlight_set: level out of range (%d)\n", level);
        level = ODROID_BACKLIGHT_LEVEL_COUNT - 1;
    }

    if (level != BacklightLevel) {
        odroid_settings_Backlight_set(level);
    }

    BacklightLevel = level;
    backlight_percentage_set(BacklightLevels[level]);
}

void ili9341_init()
{
    // Return use of backlight pin
    // esp_err_t err = rtc_gpio_deinit(LCD_PIN_NUM_BCKL);
    // if (err != ESP_OK)
    // {
    //     abort();
    // }

    // Init
    spi_initialize();


    // Line buffers
    const size_t lineSize = SCREEN_WIDTH * LINE_COUNT * sizeof(uint16_t);
    for (short x = 0; x < LINE_BUFFERS; x++)
    {
        line[x] = heap_caps_malloc(lineSize, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!line[x]) abort();

        printf("%s: line_buffer_put(%p)\n", __func__, line[x]);
        line_buffer_put(line[x]);
    }

    // Initialize transactions
    for (short x = 0; x < SPI_TRANSACTION_COUNT; x++)
    {
        void* param = &trans[x];
        xQueueSend(spi_queue, &param, portMAX_DELAY);
    }


    // Initialize SPI
    spi_bus_config_t buscfg;
		memset(&buscfg, 0, sizeof(buscfg));

    buscfg.miso_io_num = SPI_PIN_NUM_MISO;
    buscfg.mosi_io_num = SPI_PIN_NUM_MOSI;
    buscfg.sclk_io_num = SPI_PIN_NUM_CLK;
    buscfg.quadwp_io_num=-1;
    buscfg.quadhd_io_num=-1;

    spi_device_interface_config_t devcfg;
		memset(&devcfg, 0, sizeof(devcfg));

    devcfg.clock_speed_hz = LCD_SPI_CLOCK_RATE;
    devcfg.mode = 0;                                //SPI mode 0
    devcfg.spics_io_num = LCD_PIN_NUM_CS;               //CS pin
    devcfg.queue_size = 7;                          //We want to be able to queue 7 transactions at a time
    devcfg.pre_cb = ili_spi_pre_transfer_callback;  //Specify pre-transfer callback to handle D/C line
    devcfg.flags = SPI_DEVICE_NO_DUMMY; //SPI_DEVICE_HALFDUPLEX;

    //Initialize the SPI bus
    spi_bus_initialize(HSPI_HOST, &buscfg, 1);
    //assert(ret==ESP_OK);

    //Attach the LCD to the SPI bus
    spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
    //assert(ret==ESP_OK);

    //Initialize the LCD
	printf("LCD: calling ili_init.\n");
    ili_init();

	printf("LCD: calling backlight_init.\n");
    backlight_init();

    printf("LCD Initialized (%d Hz).\n", LCD_SPI_CLOCK_RATE);
}

void ili9341_poweroff()
{
    // // Drain SPI queue
    // xTaskToNotify = 0;
    //
     esp_err_t err = ESP_OK;

    backlight_deinit();

    // Disable LCD panel
    short cmd = 0;
    while (ili_sleep_cmds[cmd].databytes != 0xff)
    {
        ili_cmd(ili_sleep_cmds[cmd].cmd);
        ili_data(ili_sleep_cmds[cmd].data, ili_sleep_cmds[cmd].databytes & 0x7f);
        cmd++;
    }

    err = rtc_gpio_init(LCD_PIN_NUM_BCKL);
    assert(err == ESP_OK);

    err = rtc_gpio_set_direction(LCD_PIN_NUM_BCKL, RTC_GPIO_MODE_OUTPUT_ONLY);
    assert(err == ESP_OK);

    err = rtc_gpio_set_level(LCD_PIN_NUM_BCKL, LCD_BACKLIGHT_ON_VALUE ? 0 : 1);
    assert(err == ESP_OK);
}


void ili9341_fill_screen(uint16_t color)
{
    // Fill the buffer with the color
    for (short i = 0; i < LINE_BUFFERS; ++i)
    {
        if ((color & 0xFF) == (color >> 8))
        {
            memset(line[i], color & 0xFF, SCREEN_WIDTH * LINE_COUNT * 2);
            continue;
        }
        for (short j = 0; j < SCREEN_WIDTH * LINE_COUNT; ++j)
        {
            line[i][j] = color;
        }
    }

    send_reset_drawing(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    for (short y = 0; y < SCREEN_HEIGHT; y += LINE_COUNT)
    {
        uint16_t* line_buffer = line_buffer_get();
        send_continue_line(line_buffer, SCREEN_WIDTH, LINE_COUNT);
    }
}

void ili9341_blank_screen()
{
    odroid_display_lock();
    ili9341_fill_screen(C_BLACK);
    odroid_display_unlock();
}

static inline void
write_rect(void *buffer, uint16_t *palette,
           short origin_x, short origin_y, short left, short top,
           short width, short height, short stride, short pixel_width,
           uint8_t pixel_mask, short x_inc, short y_inc)
{
    short actual_left = ((SCREEN_WIDTH * left) + (x_inc - 1)) / x_inc;
    short actual_top = ((SCREEN_HEIGHT * top) + (y_inc - 1)) / y_inc;
    short actual_right = ((SCREEN_WIDTH * (left + width)) + (x_inc - 1)) / x_inc;
    short actual_bottom = ((SCREEN_HEIGHT * (top + height)) + (y_inc - 1)) / y_inc;
    short actual_width = actual_right - actual_left;
    short actual_height = actual_bottom - actual_top;
    short ix_acc = (x_inc * actual_left) % SCREEN_WIDTH;
    short iy_acc = (y_inc * actual_top) % SCREEN_HEIGHT;

    if (actual_width == 0 || actual_height == 0) {
        return;
    }

    send_reset_drawing(origin_x + actual_left, origin_y + actual_top,
                       actual_width, actual_height);

    short line_count = LINE_BUFFER_SIZE / actual_width;
    for (short y = 0, y_acc = iy_acc; y < height;)
    {
        uint16_t* line_buffer = line_buffer_get();

        short line_buffer_index = 0;
        short lines_to_copy = 0;

        for (; (lines_to_copy < line_count) && (y < height); ++lines_to_copy)
        {
            for (short x = 0, x_acc = ix_acc; x < width;)
            {
                if (palette == NULL)  {
                    uint16_t sample = ((uint16_t*)buffer)[x];
                    line_buffer[line_buffer_index++] = sample << 8 | sample >> 8;
                } else {
                    line_buffer[line_buffer_index++] = palette[((uint8_t*)buffer)[x] & pixel_mask];
                }

                x_acc += x_inc;
                while (x_acc >= SCREEN_WIDTH) {
                    ++x;
                    x_acc -= SCREEN_WIDTH;
                }
            }

            y_acc += y_inc;
            while (y_acc >= SCREEN_HEIGHT) {
                ++y;
                buffer += stride;
                y_acc -= SCREEN_HEIGHT;
            }
        }

        send_continue_line(line_buffer, actual_width, lines_to_copy);
    }
}

static short x_inc = SCREEN_WIDTH;
static short y_inc = SCREEN_HEIGHT;
static short x_origin = 0;
static short y_origin = 0;
static float x_scale = 1.f;
static float y_scale = 1.f;

void odroid_display_reset_scale(short width, short height)
{
    x_inc = SCREEN_WIDTH;
    y_inc = SCREEN_HEIGHT;
    x_origin = (SCREEN_WIDTH - width) / 2;
    y_origin = (SCREEN_HEIGHT - height) / 2;
    x_scale = y_scale = 1.f;
}

void odroid_display_set_scale(short width, short height, float aspect)
{
    float buffer_aspect = ((width * aspect) / (float)height);
    float screen_aspect = SCREEN_WIDTH / (float)SCREEN_HEIGHT;

    if (buffer_aspect < screen_aspect) {
        y_scale = SCREEN_HEIGHT / (float)height;
        x_scale = y_scale * aspect;
    } else {
        x_scale = SCREEN_WIDTH / (float)width;
        y_scale = x_scale / aspect;
    }

    x_inc = SCREEN_WIDTH / x_scale;
    y_inc = SCREEN_HEIGHT / y_scale;
    x_origin = (SCREEN_WIDTH - (width * x_scale)) / 2.f;
    y_origin = (SCREEN_HEIGHT - (height * y_scale)) / 2.f;

    printf("%dx%d@%.3f x_inc:%d y_inc:%d x_scale:%.3f y_scale:%.3f x_origin:%d y_origin:%d\n",
           width, height, aspect, x_inc, y_inc, x_scale, y_scale, x_origin, y_origin);
}

void ili9341_write_frame_scaled(void* buffer, odroid_scanline *diff,
                                short width, short height, short stride,
                                short pixel_width, uint8_t pixel_mask,
                                uint16_t* palette)
{
    if (!buffer) {
        ili9341_blank_screen();
        return;
    }

    odroid_display_lock();
    spi_device_acquire_bus(spi, portMAX_DELAY);

    // Interrupt/async updates
    odroid_scanline int_updates[SCREEN_HEIGHT/LINE_COUNT];
    odroid_scanline *int_ptr = &int_updates[0];
    odroid_scanline full_update = {0, height, 0, width};

    if (diff) {
        use_polling = true; // Do polling updates first
        for (short y = 0; y < height;)
        {
            odroid_scanline *update = &diff[y];

            if (update->width > 0) {
                int n_pixels = (x_scale * update->width) * (y_scale * update->repeat);
                if (n_pixels < POLLING_PIXEL_THRESHOLD) {
                    write_rect(buffer + (y * stride) + (update->left * pixel_width), palette,
                                x_origin, y_origin, update->left, y, update->width, update->repeat,
                                stride, pixel_width, pixel_mask, x_inc, y_inc);
                } else {
                    (*int_ptr++) = (*update);
                }
            }
            y += update->repeat;
        }
    } else {
        (*int_ptr++) = full_update;
    }

    use_polling = false; // Use interrupt updates for larger areas
    while (--int_ptr >= &int_updates)
    {
        write_rect(buffer + (int_ptr->top * stride) + (int_ptr->left * pixel_width), palette,
                    x_origin, y_origin, int_ptr->left, int_ptr->top, int_ptr->width, int_ptr->repeat,
                    stride, pixel_width, pixel_mask, x_inc, y_inc);
    }

    spi_device_release_bus(spi);
    odroid_display_unlock();
}

void ili9341_write_frame(uint16_t* buffer)
{
    ili9341_write_frame_rectangleLE(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, buffer);
}

void ili9341_write_frame_rectangleLE(short left, short top, short width, short height, uint16_t* buffer)
{
    short x, y, i;

    if (left < 0 || top < 0) abort();
    if (width < 1 || height < 1) abort();

    //xTaskToNotify = xTaskGetCurrentTaskHandle();

    send_reset_drawing(left, top, width, height);

    if (buffer == NULL)
    {
        ili9341_blank_screen();
        return;
    }

    for (y = 0; y < height; y++)
    {
        uint16_t* line_buffer = line_buffer_get();

        for (i = 0; i < width; ++i)
        {
            uint16_t pixel = buffer[y * width + i];
            line_buffer[i] = pixel << 8 | pixel >> 8;
        }

        send_continue_line(line_buffer, width, 1);
    }
}

void odroid_display_drain_spi()
{
    spi_transaction_t *t[SPI_TRANSACTION_COUNT];
    for (short i = 0; i < SPI_TRANSACTION_COUNT; ++i) {
        xQueueReceive(spi_queue, &t[i], portMAX_DELAY);
    }
    for (short i = 0; i < SPI_TRANSACTION_COUNT; ++i) {
        if (xQueueSend(spi_queue, &t[i], portMAX_DELAY) != pdPASS)
        {
            abort();
        }
    }
}

void odroid_display_show_error(int errNum)
{
    switch(errNum)
    {
        case ODROID_SD_ERR_BADFILE:
        case ODROID_SD_ERR_NOCARD:
            ili9341_fill_screen(C_WHITE);
            ili9341_write_frame_rectangleLE((SCREEN_WIDTH / 2) - (image_sdcard_red_48dp.width / 2),
                (SCREEN_HEIGHT / 2) - (image_sdcard_red_48dp.height / 2),
                image_sdcard_red_48dp.width,
                image_sdcard_red_48dp.height,
                image_sdcard_red_48dp.pixel_data);
            break;

        default:
            ili9341_fill_screen(C_RED);
    }

    // Drain SPI queue
    odroid_display_drain_spi();
}

void odroid_display_show_hourglass()
{
    ili9341_write_frame_rectangleLE((SCREEN_WIDTH / 2) - (image_hourglass_empty_black_48dp.width / 2),
        (SCREEN_HEIGHT / 2) - (image_hourglass_empty_black_48dp.height / 2),
        image_hourglass_empty_black_48dp.width,
        image_hourglass_empty_black_48dp.height,
        image_hourglass_empty_black_48dp.pixel_data);
}

void odroid_display_lock()
{
    if (!display_mutex)
    {
        display_mutex = xSemaphoreCreateMutex();
        if (!display_mutex) abort();
    }

    if (xSemaphoreTake(display_mutex, 1000 / portTICK_RATE_MS) != pdTRUE)
    {
        abort();
    }
}

void odroid_display_unlock()
{
    if (!display_mutex) abort();

    odroid_display_drain_spi();
    xSemaphoreGive(display_mutex);
}

static inline bool
pixel_diff(uint8_t *buffer1, uint8_t *buffer2,
           uint16_t *palette1, uint16_t *palette2,
           uint8_t pixel_mask, uint8_t palette_shift_mask,
           int idx)
{
    uint8_t p1 = (buffer1[idx] & pixel_mask);
    uint8_t p2 = (buffer2[idx] & pixel_mask);
    if (!palette1)
        return p1 != p2;

    if (palette_shift_mask) {
        if (buffer1[idx] & palette_shift_mask) p1 += (pixel_mask + 1);
        if (buffer2[idx] & palette_shift_mask) p2 += (pixel_mask + 1);
    }

    return palette1[p1] != palette2[p2];
}

void IRAM_ATTR
odroid_buffer_diff(void *buffer, void *old_buffer,
                   uint16_t *palette, uint16_t *old_palette,
                   short width, short height, short stride, short pixel_width,
                   uint8_t pixel_mask, uint8_t palette_shift_mask,
                   odroid_scanline *out_diff)
{
    if (!old_buffer) {
        goto _full_update;
    }

    // If the palette didn't change we can speed up things by avoiding pixel_diff()
    if (palette == old_palette || memcmp(palette, old_palette, (pixel_mask + 1) * 2) == 0)
    {
        pixel_mask |= palette_shift_mask;
        palette_shift_mask = 0;
        palette = NULL;
    }

    int partial_update_remaining = width * height * FULL_UPDATE_THRESHOLD;

    uint32_t u32_pixel_mask = (pixel_mask << 24)|(pixel_mask << 16)|(pixel_mask << 8)|pixel_mask;
    uint16_t u32_blocks = (width * pixel_width / 4);
    uint16_t u32_pixels = 4 / pixel_width;

    for (int y = 0, i = 0; y < height; ++y, i += stride) {
        out_diff[y].top = y;
        out_diff[y].left = width;
        out_diff[y].width = 0;
        out_diff[y].repeat = 1;

        if (!palette) {
            // This is only accurate to 4 pixels of course, but much faster
            uint32_t *buffer32 = buffer + i;
            uint32_t *old_buffer32 = old_buffer + i;
            for (short x = 0; x < u32_blocks; ++x) {
                if ((buffer32[x] & u32_pixel_mask) != (old_buffer32[x] & u32_pixel_mask))
                {
                    out_diff[y].left = x * u32_pixels;
                    for (x = u32_blocks - 1; x >= 0; --x) {
                        if ((buffer32[x] & u32_pixel_mask) != (old_buffer32[x] & u32_pixel_mask)) {
                            out_diff[y].width = (((x + 1) * u32_pixels) - out_diff[y].left);
                            break;
                        }
                    }
                }
            }
        } else {
            for (int x = 0, idx = i; x < width; ++x, ++idx) {
                if (!pixel_diff(buffer, old_buffer, palette, old_palette,
                                pixel_mask, palette_shift_mask, idx)) {
                    continue;
                }
                out_diff[y].left = x;

                for (x = width - 1, idx = i + (width - 1); x >= 0; --x, --idx)
                {
                    if (!pixel_diff(buffer, old_buffer, palette, old_palette,
                                    pixel_mask, palette_shift_mask, idx)) {
                        continue;
                    }
                    out_diff[y].width = (x - out_diff[y].left) + 1;
                    break;
                }
                break;
            }
        }

        partial_update_remaining -= out_diff[y].width;

        if (partial_update_remaining <= 0) {
            goto _full_update;
        }
    }

    // Combine consecutive lines with similar changes location to optimize the SPI transfer
    odroid_scanline *diff = out_diff;
    for (short y = height - 1; y > 0; --y) {
        short left_diff = abs(diff[y].left - diff[y-1].left);
        if (left_diff > 8) continue;

        short right = diff[y].left + diff[y].width;
        short right_prev = diff[y-1].left + diff[y-1].width;
        short right_diff = abs(right - right_prev);
        if (right_diff > 8) continue;

        if (diff[y].left < diff[y-1].left)
          diff[y-1].left = diff[y].left;
        diff[y-1].width = (right > right_prev) ?
          right - diff[y-1].left : right_prev - diff[y-1].left;
        diff[y-1].repeat = diff[y].repeat + 1;
    }
    return;

_full_update:
    out_diff[0].top = 0;
    out_diff[0].left = 0;
    out_diff[0].width = width;
    out_diff[0].repeat = height;
}
