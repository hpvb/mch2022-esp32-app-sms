/*
MIT License

Copyright (c) 2022 Hein-Pieter van Braam <hp@tmm.cx>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Sega master system emulator for the MCH2022 badge platform.

#include "main.h"

#include <sms.h>
#include <string.h>

static uint16_t *backbuffer[2];
static uint16_t *framebuffer = NULL;
static ILI9341 *ili9341 = NULL;

extern const uint8_t rom_start[] asm("_binary_rom_sms_start");
extern const uint8_t rom_end[] asm("_binary_rom_sms_end");

uint64_t frames = 0;
uint64_t dropped_frames = 0;

xQueueHandle buttonQueue;
QueueHandle_t vidQueue;

// Apparently we're not supposed to call these directly.
esp_err_t ili9341_send(ILI9341 *device, const uint8_t *data, const int len, const bool dc_level);
esp_err_t ili9341_set_addr_window(ILI9341 *device, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

// Exits the app, returning to the launcher.
void exit_to_launcher() {
  REG_WRITE(RTC_CNTL_STORE0_REG, 0);
  esp_restart();
}

static struct SMS_Core *sms;
#define BSWAP16(__x) (((((__x)&0xFF00) >> 8) | (((__x)&0xFF) << 8)))

static uint32_t core_colour_callback(void *user, uint8_t r, uint8_t g, uint8_t b) {
  r <<= 6;
  g <<= 6;
  b <<= 6;

  uint16_t color = (((r & 0XF8) << 8) + ((g & 0XFC) << 3) + ((b & 0XF8) >> 3));
  return BSWAP16(color);
}

// This avoids some safety checks and a mutex we can't afford
static void write_frame() {
  // ili9341_write_partial(ili9341, framebuffer, 0, 0, SMS_SCREEN_WIDTH,
  // SMS_SCREEN_HEIGHT); ili9341_write(ili9341, framebuffer);
  uint32_t position = 0;
  while (SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2 - position > 0) {
    uint32_t length = ili9341->spi_max_transfer_size;
    
    if (SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2 - position < ili9341->spi_max_transfer_size)
	    length = SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2 - position;

    ili9341_send(ili9341, ((uint8_t *)framebuffer) + position, length, true);
    position += length;
  }
}

static void core_vblank_callback(void *user) {
  if (xQueueSend(vidQueue, &framebuffer, 5) == errQUEUE_FULL) {
    ++dropped_frames;
  }

  ++frames;
}

static void available_ram(const char *context) {
  printf("(%s) Available RAM: %i, Largest block: %i\n", context,
         heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
}

volatile bool videoTaskIsRunning = false;
void videoTask(void *arg) {
  videoTaskIsRunning = true;
  uint16_t *param;
  while (1) {
    xQueuePeek(vidQueue, &param, portMAX_DELAY);
    if (param == (uint16_t *)1)
      break;
    write_frame();
    xQueueReceive(vidQueue, &param, portMAX_DELAY);
  }
  videoTaskIsRunning = false;
  vTaskDelete(NULL);
}

void handle_input() {
  rp2040_input_message_t buttonMessage = {0};
  BaseType_t queueResult;
  do {
    queueResult = xQueueReceive(buttonQueue, &buttonMessage, 0);
    if (queueResult == pdTRUE) {
      uint8_t button = buttonMessage.input;
      bool value = buttonMessage.state;
      switch (button) {
      case RP2040_INPUT_JOYSTICK_DOWN:
        SMS_set_port_a(sms, JOY1_DOWN_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_UP:
        SMS_set_port_a(sms, JOY1_UP_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_LEFT:
        SMS_set_port_a(sms, JOY1_LEFT_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_RIGHT:
        SMS_set_port_a(sms, JOY1_RIGHT_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_ACCEPT:
        SMS_set_port_a(sms, JOY1_A_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_BACK:
        SMS_set_port_a(sms, JOY1_B_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_START:
        SMS_set_port_a(sms, PAUSE_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_SELECT:
        break;
      case RP2040_INPUT_BUTTON_HOME:
        exit_to_launcher();
        break;
      case RP2040_INPUT_BUTTON_MENU:
      default:
        break;
      }
    }
  } while (queueResult == pdTRUE);
}

void app_main() {
  printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
  printf("!!!!!!!!!!!!!! Not A Plumber !!!!!!!!!!!!!!!!!\n");
  printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

  printf("                         _.-*'\"\"`*-._\n");
  printf("                _.-*'                  `*-._\n");
  printf("             .-'                            `-.\n");
  printf("  /`-.    .-'                  _.              `-.\n");
  printf(" :    `..'                  .-'_ .                `.\n");
  printf(" |    .'                 .-'_.' \\ .                 \\\n");
  printf(" |   /                 .' .*     ;               .-'\"\n");
  printf(" :   L                    `.     | ;          .-'\n");
  printf("  \\.' `*.          .-*\"*-.  `.   ; |        .'\n");
  printf("  /      \\        '       `.  `-'  ;      .'\n");
  printf(" : .'\"`.  .       .-*'`*-.  \\     .      (_\n");
  printf(" |              .'        \\  .             `*-.\n");
  printf(" |.     .      /           ;                   `-.\n");
  printf(" :    db      '       d$b  |                      `-.\n");
  printf(" .   :PT;.   '       :P\"T; :                         `.\n");
  printf(" :   :bd;   '        :b_d; :                           \\\n");
  printf(" |   :$$; `'         :$$$; |                            \\\n");
  printf(" |    TP              T$P  '                             ;\n");
  printf(" :                        /.-*'\"`.                       |\n");
  printf(".sdP^T$bs.               /'       \\\n");
  printf("$$$._.$$$$b.--._      _.'   .--.   ;\n");
  printf("`*$$$$$$P*'     `*--*'     '  / \\  :\n");
  printf("   \\                        .'   ; ;\n");
  printf("    `.                  _.-'    ' /\n");
  printf("      `*-.                      .'\n");
  printf("          `*-._            _.-*'\n");
  printf("               `*=--..--=*'\n");

  available_ram("start");

  bsp_init();
  available_ram("bsp_init");

  bsp_rp2040_init();
  available_ram("bsp_rp2040_init");

  ili9341 = get_ili9341();

  buttonQueue = get_rp2040()->queue;
  // nvs_flash_init();

  printf("Clear screen start\n");
  framebuffer = malloc(ILI9341_WIDTH * ILI9341_HEIGHT * 2);
  memset(framebuffer, 0xee, ILI9341_WIDTH * ILI9341_HEIGHT * 2);
  ili9341_write(get_ili9341(), (uint8_t *)framebuffer);
  free(framebuffer);
  printf("Done clearing screen\n");

  // We set this only once, saves us some more SPI bandwidth
  ili9341_set_addr_window(ili9341, (ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2,
                          (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2,
                          SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT);

  backbuffer[0] = malloc(SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2);
  if (!backbuffer[0]) {
    printf("Malloc failed? Tried to allocate %i x %i @ %i (%i bytes)\n",
           SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT, 2,
           (SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2));
  }
  available_ram("backbuffer[0]");

  // Not enough RAM found (yet)
#if 0
    backbuffer[1] = malloc(SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2);
    if (!backbuffer[1]) {
	    printf("Malloc failed? Tried to allocate %i x %i @ %i (%i bytes)\n", SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT, 2, (SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2));
    }
    available_ram("backbuffer[1]");
#endif

  framebuffer = backbuffer[0];

  sms = malloc(sizeof(struct SMS_Core));
  // sms = heap_caps_malloc(sizeof(struct SMS_Core), MALLOC_CAP_SPIRAM);
  if (!sms) {
    printf("Malloc failed? SMS Tried to allocate %i\n",
           sizeof(struct SMS_Core));
  }
  available_ram("sms");

  SMS_init(sms);
  available_ram("SMS_init");

  printf("Starting video thread\n");
  vidQueue = xQueueCreate(1, sizeof(uint16_t *));
  xTaskCreatePinnedToCore(&videoTask, "videoTask", 2048, NULL, 5, NULL, 1);
  available_ram("videoTask");

  SMS_set_colour_callback(sms, core_colour_callback);
  SMS_set_vblank_callback(sms, core_vblank_callback);

  SMS_set_pixels(sms, framebuffer, SMS_SCREEN_WIDTH, 2);
  SMS_loadrom(sms, rom_start, rom_end - rom_start, SMS_System_SMS);

  uint64_t start = esp_timer_get_time();
  uint64_t end;

  while (1) {
    handle_input();
    SMS_run(sms, SMS_CPU_CLOCK / 60);

    end = esp_timer_get_time();
    if ((end - start) >= 1000000) {
      printf("fps: %lli, dropped: %lli, succeeded: %lli\n", frames,
             dropped_frames, frames - dropped_frames);
      frames = 0;
      dropped_frames = 0;
      start = esp_timer_get_time();
    }
  }
}
