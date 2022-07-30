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

#include <string.h>

#include "main.h"
#include "videobuffer.h"
#include "ws2812.h"
#include "sms.h"

extern const uint8_t rom_start[] asm("_binary_rom_sms_start");
extern const uint8_t rom_end[] asm("_binary_rom_sms_end");

static bool current_backbuffer = 0;
videobuffer_t* backbuffer[2];
static uint16_t *framebuffer = NULL;

static uint64_t frames = 0;
static uint64_t cpu_cycles = 0;
static uint64_t dropped_frames = 0;

static xQueueHandle buttonQueue;
static QueueHandle_t videoQueue;
ILI9341 *ili9341 = NULL;

// Apparently we're not supposed to call these directly.
esp_err_t ili9341_send(ILI9341 *device, const uint8_t *data, const int len, const bool dc_level);
esp_err_t ili9341_set_addr_window(ILI9341 *device, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

struct SMS_Core sms;

static bool paused = false;

// Exits the app, returning to the launcher.
void exit_to_launcher() {
  REG_WRITE(RTC_CNTL_STORE0_REG, 0);
  esp_restart();
}

// We get 6 bit RGB values, pack them into a byte swapped RGB565 value
__attribute__((always_inline)) inline uint32_t core_colour_callback(void *user, uint8_t r, uint8_t g, uint8_t b) {
  return __builtin_bswap16(((((r << 6 ) & 0xF8) << 8) + (((g << 6) & 0xFC) << 3) + (((b << 6) & 0xF8) >> 3)));
}

volatile bool currently_drawing;
// This avoids some safety checks and a mutex we can't afford
__attribute__((always_inline)) inline static void write_frame(bool frame) {
  currently_drawing = true;
  ili9341->dc_level = true;

  spi_transaction_t transaction = {
    .length = backbuffer[frame]->part_size * 8,
    .tx_buffer = NULL,
    .user = (void*)ili9341,
  };

  for(int i = 0; i < backbuffer[frame]->part_numb; ++i) {
    transaction.tx_buffer = backbuffer[frame]->parts[i];
    spi_device_polling_transmit(ili9341->spi_device, &transaction);
  }

  currently_drawing = false;
  ++frames;
}

__attribute__((always_inline)) inline void core_vblank_callback(void *user) {
  while (currently_drawing) { }

  if (xQueueSend(videoQueue, &current_backbuffer, 0) == errQUEUE_FULL) {
    ++dropped_frames;
  }

  current_backbuffer = !current_backbuffer;
  sms.pixels = backbuffer[current_backbuffer];
}

volatile bool videoTaskIsRunning = false;
void videoTask(void *arg) {
  videoTaskIsRunning = true;
  bool param;
  while (1) {
    xQueuePeek(videoQueue, &param, portMAX_DELAY);
    write_frame(param);
    xQueueReceive(videoQueue, &param, portMAX_DELAY);
  }
  videoTaskIsRunning = false;
  vTaskDelete(NULL);
}

static bool select_down = false;

static void handle_input() {
  rp2040_input_message_t buttonMessage = {0};
  BaseType_t queueResult;
  do {
    queueResult = xQueueReceive(buttonQueue, &buttonMessage, 0);
    if (queueResult == pdTRUE) {
      uint8_t button = buttonMessage.input;
      bool value = buttonMessage.state;
      switch (button) {
      case RP2040_INPUT_JOYSTICK_DOWN:
        SMS_set_port_a(&sms, JOY1_DOWN_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_UP:
        SMS_set_port_a(&sms, JOY1_UP_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_LEFT:
        SMS_set_port_a(&sms, JOY1_LEFT_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_RIGHT:
        SMS_set_port_a(&sms, JOY1_RIGHT_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_ACCEPT:
        SMS_set_port_a(&sms, JOY1_A_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_BACK:
        SMS_set_port_a(&sms, JOY1_B_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_START:
        SMS_set_port_a(&sms, PAUSE_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_SELECT:
	if (! select_down) {
    	  paused = !paused;
	}

	select_down = !select_down;
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

#define AUDIO_FREQ 11200
#define AUDIO_BLOCK_SIZE 256
static uint16_t audio_buffer[AUDIO_BLOCK_SIZE];
static uint32_t audio_idx = 0;

void audio_init() {
    i2s_config_t i2s_config = {
      .mode                 = I2S_MODE_MASTER | I2S_MODE_TX,
      .sample_rate          = AUDIO_FREQ,
      .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_MSB,
      .dma_buf_count        = 8,
      .dma_buf_len          = AUDIO_BLOCK_SIZE / 4,
      .intr_alloc_flags     = 0,
      .use_apll             = true,
      .tx_desc_auto_clear   = true
   };

    i2s_driver_install(0, &i2s_config, 0, NULL);

    i2s_pin_config_t pin_config = {
      .mck_io_num   = 0,
      .bck_io_num   = 4,
      .ws_io_num    = 12,
      .data_out_num = 13,
      .data_in_num  = I2S_PIN_NO_CHANGE
    };

    i2s_set_pin(0, &pin_config);
    i2s_set_sample_rates(0, AUDIO_FREQ);

    printf("Audio initialized!\n");
}

__attribute__((always_inline)) inline void core_apu_callback(void* user, struct SMS_ApuCallbackData* data) {
  uint16_t sample = (data->tone0 + data->tone1 + data->tone2 + data->noise) * 128;
  audio_buffer[audio_idx++] = sample;
  audio_buffer[audio_idx++] = sample;

  if (audio_idx >= AUDIO_BLOCK_SIZE) {
    size_t count;
    
    i2s_write(0, audio_buffer, sizeof(audio_buffer), &count, portMAX_DELAY);
    audio_idx = 0;
  }

  return;
}

static uint8_t leds[5][3];

static void sonic_leds(uint8_t lives) {
  memset(leds, 0, sizeof(leds));
  switch (lives) {
    case 9: leds[3][1] = 10;
    case 8: leds[1][1] = 10;
    case 7: leds[2][1] = 10;
    case 6: leds[0][1] = 10;
    case 5: leds[4][2] = 10;
    case 4: leds[3][2] = 10;
    case 3: leds[1][2] = 10;
    case 2: leds[2][2] = 10;
    case 1: leds[0][2] = 10;
  }

  ws2812_send_data((uint8_t*)leds, sizeof(leds));
}

static void sonic1_leds() {
  uint8_t lives = sms.system_ram[0xD246 - 0xc000];

  sonic_leds(lives);
}

static void sonic2_leds() {
  uint8_t lives = sms.system_ram[0xD298 - 0xc000];

  sonic_leds(lives);
}

void init_screen_rect() {
  // We set this only once, saves us some more SPI bandwidth
  ili9341_set_addr_window(ili9341, (ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2,
                          (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2,
                          SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT);

}

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
__attribute__((always_inline)) inline void write_screen(uint8_t* buffer, size_t size) {
  for(int i = 0; i < size / ili9341->spi_max_transfer_size + 1; ++i) {
    size_t length = MIN(ili9341->spi_max_transfer_size, size - (i * ili9341->spi_max_transfer_size));
    if (length) ili9341_send(ili9341, buffer, length, true);
  }
}

void set_overscan_border(uint16_t color) {
  while (currently_drawing) {}

  for (uint32_t i = 0; i < ili9341->spi_max_transfer_size / 2; ++i) {
    framebuffer[i] = color;
  }

  // This is not a mistake, we're in 16bpp
  size_t top = ILI9341_WIDTH * (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT);
  size_t side = (ILI9341_WIDTH - SMS_SCREEN_WIDTH) * SMS_SCREEN_HEIGHT; 

  ili9341_set_addr_window(ili9341, 0, 0, ILI9341_WIDTH, (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2);
  write_screen((uint8_t*)framebuffer, top);
  ili9341_set_addr_window(ili9341, 0, (ILI9341_HEIGHT - (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2), ILI9341_WIDTH, (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2);
  write_screen((uint8_t*)framebuffer, top);
  ili9341_set_addr_window(ili9341, 0, (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2, (ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2, ILI9341_HEIGHT - (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT));
  write_screen((uint8_t*)framebuffer, side);
  ili9341_set_addr_window(
    ili9341, ILI9341_WIDTH - ((ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2),
    (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2,
    (ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2,
    ILI9341_HEIGHT - (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT)
  );
  write_screen((uint8_t*)framebuffer, side);

  init_screen_rect();
}

static void available_ram(const char *context) {
  printf("(%s) Available DRAM: %i, Largest block: %i, Available SPIRAM: %i\n",
      context,
      heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
      heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
}

void main_loop() {
  uint64_t start = esp_timer_get_time();
  uint64_t end;

  uint16_t overscan_color = 0;
  uint16_t current_overscan_color = 0;

  while (1) {
    handle_input();
   
    if (paused) {
      //menu_pause();
      //init_screen_rect();
      continue;
    }

    for (size_t i = 0; i < SMS_CPU_CLOCK / 60; i += sms.cpu.cycles) {
      z80_run();
      vdp_run(sms.cpu.cycles);
      psg_run(sms.cpu.cycles);
      cpu_cycles += sms.cpu.cycles;

      if (paused) break;
    }
    
    psg_sync();

    current_overscan_color = sms.vdp.colour[16 + (sms.vdp.registers[0x7] & 0xF)];
    if (overscan_color != current_overscan_color) {
      set_overscan_border(current_overscan_color);
      overscan_color = current_overscan_color;
    }

    switch (sms.crc) {
    case 0xB519E833:
      sonic1_leds();
      break;
    case 0xD6F2BFCA:
      sonic2_leds();
      break;
    }

    end = esp_timer_get_time();
    if ((end - start) >= 1000000) {
      printf("cpu_mhz: %.6f, fps: %lli, dropped: %lli, succeeded: %lli\n",
        cpu_cycles / 1000000.00, frames, dropped_frames, frames - dropped_frames);

      if (cpu_cycles < 3579545)
        set_overscan_border(0x00f0);
      else
        set_overscan_border(current_overscan_color);

      frames = 0;
      cpu_cycles = 0;
      dropped_frames = 0;
      start = esp_timer_get_time();
    }
  }
}

void app_main() {
  printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
  printf("!!!!!!!!!!!!!! Not A Plumber !!!!!!!!!!!!!!!!!\n");
  printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");

  printf("                      _.-*'\"\"`*-._\n");
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

  audio_init();
  available_ram("audio_init");

  buttonQueue = get_rp2040()->queue;
  // nvs_flash_init();

  framebuffer = heap_caps_malloc(ili9341->spi_max_transfer_size, MALLOC_CAP_SPIRAM);
  set_overscan_border(0);

  SMS_init(&sms);
  available_ram("SMS_init");

  printf("Starting video thread\n");
  videoQueue = xQueueCreate(1, sizeof(uint16_t *));
  xTaskCreatePinnedToCore(&videoTask, "videoTask", 2048, NULL, 5, NULL, 0);
  available_ram("videoTask");

  SMS_set_colour_callback(&sms, core_colour_callback);
  SMS_set_vblank_callback(&sms, core_vblank_callback);
  SMS_set_apu_callback(&sms, core_apu_callback, AUDIO_FREQ);

  size_t screen_size = SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2;
  backbuffer[0] = videobuffer_allocate(SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT, screen_size / ili9341->spi_max_transfer_size);
  backbuffer[1] = videobuffer_allocate(SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT, screen_size / ili9341->spi_max_transfer_size);
  available_ram("backbuffer");

  SMS_set_pixels(&sms, backbuffer[0], SMS_SCREEN_WIDTH, 2);

  size_t rom_size = rom_end - rom_start;
  uint8_t* rom = heap_caps_malloc(rom_size, MALLOC_CAP_SPIRAM);
  if (!rom) {
    printf("Allocation of rom in SPIRAM failed. Attempted to allocate %i bytes\n", rom_size);
  }
  memcpy(rom, rom_start, rom_size);
  SMS_loadrom(&sms, rom, rom_size, SMS_System_SMS);
  printf("ROM loaded, crc: 0x%08X\n", sms.crc);

  gpio_set_direction(GPIO_SD_PWR, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SD_PWR, 1);
  ws2812_init(GPIO_LED_DATA);

  available_ram("done initializing");
  main_loop();
}
