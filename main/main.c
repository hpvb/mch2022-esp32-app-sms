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

#include "ice40-lcd.h"
#include "main.h"
#include "videobuffer.h"
#include "ws2812.h"
#include "sms.h"

#undef TAG
#define TAG "SMS"

extern const uint8_t rom_start[] asm("_binary_rom_sms_start");
extern const uint8_t rom_end[] asm("_binary_rom_sms_end");

static bool current_video_backbuffer = 0;
videobuffer_t* backbuffer[2];

#define OVERSCAN_BUFFER_SIZE 1024
static uint8_t *overscan_buffer = NULL;

static uint64_t frames = 0;
static uint64_t cpu_cycles = 0;
static uint64_t dropped_frames = 0;
static uint64_t frame_time = 0;
static uint64_t cpu_time = 0;
static uint64_t vdp_time = 0;

extern uint64_t vdp_display_enabled;
extern uint64_t vdp_display_disabled;
extern uint64_t cpal_updates;

static xQueueHandle button_queue;
static QueueHandle_t video_queue;
static SemaphoreHandle_t video_mutex;

ILI9341 *ili9341 = NULL;
ICE40 *ice40 = NULL;

struct SMS_Core sms;

static bool paused = false;

// Exits the app, returning to the launcher.
void exit_to_launcher() {
  REG_WRITE(RTC_CNTL_STORE0_REG, 0);
  esp_restart();
}

// We get 6 bit RGB values, pack them into a byte swapped RGB565 value
__attribute__((always_inline)) inline uint32_t core_colour_callback(void *user, uint8_t r, uint8_t g, uint8_t b) {
  if (SMS_is_system_type_gg()) {
    r <<= 4;
    g <<= 4;
    b <<= 4;
  } else {
    r <<= 6;
    g <<= 6;
    b <<= 6;
  }
  return __builtin_bswap16((((r & 0xF8) << 8) + ((g & 0xFC) << 3) + ((b & 0xF8) >> 3)));
}

__attribute__((always_inline)) inline static void write_frame(bool frame) {
  xSemaphoreTake(video_mutex, portMAX_DELAY);

  uint64_t start = esp_timer_get_time();
  uint64_t end;

  for(int i = 0; i < backbuffer[frame]->part_numb; ++i) {
    ice40_lcd_send_turbo(ice40, ((uint8_t*)backbuffer[frame]->parts[i]) - 1, backbuffer[frame]->part_size + 1);
  }

  xSemaphoreGive(video_mutex);
  ++frames;

  end = esp_timer_get_time();

  frame_time += end - start;
}

__attribute__((always_inline)) inline void core_vblank_callback(void *user) {
  if (xQueueSend(video_queue, &current_video_backbuffer, portMAX_DELAY) == errQUEUE_FULL) {
    ++dropped_frames;
  }

  current_video_backbuffer = !current_video_backbuffer;
  sms.pixels = backbuffer[current_video_backbuffer];
}

void video_task(void *arg) {
  bool param;
  while (1) {
    xQueuePeek(video_queue, &param, portMAX_DELAY);
    write_frame(param);
    xQueueReceive(video_queue, &param, portMAX_DELAY);
  }
  vTaskDelete(NULL);
}

static bool select_down = false;

static void handle_input() {
  rp2040_input_message_t buttonMessage = {0};
  BaseType_t queueResult;
  do {
    queueResult = xQueueReceive(button_queue, &buttonMessage, 0);
    if (queueResult == pdTRUE) {
      uint8_t button = buttonMessage.input;
      bool value = buttonMessage.state;
      switch (button) {
      case RP2040_INPUT_JOYSTICK_DOWN:
        SMS_set_port_a(JOY1_DOWN_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_UP:
        SMS_set_port_a(JOY1_UP_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_LEFT:
        SMS_set_port_a(JOY1_LEFT_BUTTON, value);
        break;
      case RP2040_INPUT_JOYSTICK_RIGHT:
        SMS_set_port_a(JOY1_RIGHT_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_ACCEPT:
        SMS_set_port_a(JOY1_A_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_BACK:
        SMS_set_port_a(JOY1_B_BUTTON, value);
        break;
      case RP2040_INPUT_BUTTON_START:
        SMS_set_port_a(PAUSE_BUTTON, value);
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

#define AUDIO_FREQ 22000
#define AUDIO_BLOCK_SIZE 256
static uint16_t* audio_buffer;
static uint32_t audio_idx = 0;

void audio_init() {
    i2s_config_t i2s_config = {
      .mode                 = I2S_MODE_MASTER | I2S_MODE_TX,
      .sample_rate          = AUDIO_FREQ,
      .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_MSB,
      .dma_buf_count        = 4,
      .dma_buf_len          = AUDIO_BLOCK_SIZE / 4,
      .intr_alloc_flags     = 0,
      .use_apll             = true,
      .tx_desc_auto_clear   = false, 
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

    audio_buffer = heap_caps_malloc(AUDIO_BLOCK_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!audio_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
    }
    ESP_LOGI(TAG, "Audio initialized!");
}

__attribute__((always_inline)) inline void core_apu_callback(void* user, struct SMS_ApuCallbackData* data) {
  size_t count;
  uint16_t sample = (data->tone0 + data->tone1 + data->tone2 + data->noise) * 128;
  audio_buffer[audio_idx++] = sample;
  audio_buffer[audio_idx++] = sample;

  if (audio_idx >= AUDIO_BLOCK_SIZE) {
    i2s_write(0, audio_buffer, AUDIO_BLOCK_SIZE * 2, &count, portMAX_DELAY);
    audio_idx = 0;
  }
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
  ice40_lcd_set_addr_window(ice40,
      ((ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2) - 4,
      (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2,
      SMS_SCREEN_WIDTH,
      SMS_SCREEN_HEIGHT);
}

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
__attribute__((always_inline)) inline void write_screen(uint8_t* buffer, size_t size) {
  while(1) {
    if (!size) break;

    size_t length = MIN(OVERSCAN_BUFFER_SIZE, size);
    ice40_lcd_send_turbo(ice40, buffer, length + 1);
    size -= length;
  }
}

void set_overscan_border(uint16_t color) {
  // Don't try to interleave this with regular frame data, bad things will happen

  xSemaphoreTake(video_mutex, portMAX_DELAY);

  overscan_buffer[0] = 0xf3;
  for (uint32_t i = 1; i < OVERSCAN_BUFFER_SIZE + 1; i += 2) {
    *(overscan_buffer + i) = color;
    *(overscan_buffer + i + 1) = color >> 8;
  }

  // This is not a mistake, we're in 16bpp
  size_t top = ILI9341_WIDTH * (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT);
  size_t bottom = top;
  size_t leftside = (ILI9341_WIDTH - SMS_SCREEN_WIDTH - 8) * SMS_SCREEN_HEIGHT;
  size_t rightside = (ILI9341_WIDTH - SMS_SCREEN_WIDTH + 8) * SMS_SCREEN_HEIGHT;

  ice40_lcd_set_addr_window(ice40,
      0,
      0,
      ILI9341_WIDTH,
      (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2);
  write_screen(overscan_buffer, top);

  ice40_lcd_set_addr_window(ice40,
      0,
      (ILI9341_HEIGHT - (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2),
      ILI9341_WIDTH,
      (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2);
  write_screen(overscan_buffer, bottom);

  ice40_lcd_set_addr_window(ice40,
      0,
      (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2,
      ((ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2) - 4,
      ILI9341_HEIGHT - (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT));
  write_screen(overscan_buffer, leftside);

  ice40_lcd_set_addr_window(ice40,
      (ILI9341_WIDTH - ((ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2)) - 4,
      (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT) / 2,
      ((ILI9341_WIDTH - SMS_SCREEN_WIDTH) / 2) + 4,
      ILI9341_HEIGHT - (ILI9341_HEIGHT - SMS_SCREEN_HEIGHT));
  write_screen(overscan_buffer, rightside);

  init_screen_rect();
  xSemaphoreGive(video_mutex);
}

static void available_ram(const char *context) {
  ESP_LOGI(TAG, "(%s) Available DRAM: %i, Largest block: %i, Available SPIRAM: %i",
      context,
      heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DMA),
      heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
}

void main_loop() {
  TickType_t xLastWakeTime = xTaskGetTickCount ();

  uint64_t start = esp_timer_get_time();
  uint64_t end;

  uint16_t overscan_color = 0;
  uint16_t current_overscan_color = 0;

  while (1) {
    handle_input();
   
    if (paused) {
      //menu_pause();
      //init_screen_rect();
      i2s_zero_dma_buffer(0);
      continue;
    }

    for (size_t i = 0; i < SMS_CPU_CLOCK / 60; i += sms.cpu.cycles) {
      //uint64_t cpu_start = esp_timer_get_time();
      z80_run();
      //uint64_t cpu_end = esp_timer_get_time();
      //cpu_time += cpu_end - cpu_start;

      //uint64_t vdp_start = esp_timer_get_time();
      vdp_run(sms.cpu.cycles);
      //uint64_t vdp_end = esp_timer_get_time();
      //vdp_time += vdp_end - vdp_start;
      
      psg_run(sms.cpu.cycles);
      cpu_cycles += sms.cpu.cycles;

      if (paused) break;
    }
    
    psg_sync();
    vTaskDelayUntil(&xLastWakeTime, 2);

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
    double elapsed = end - start;
    if (elapsed >= 1000000) {

      double cpu_mhz = cpu_cycles / elapsed;
      if (cpu_mhz < 3.579545) {
        set_overscan_border(0x00f0);

        ESP_LOGW(TAG, "cpu_mhz: %.6f, fps: %lli, dropped: %lli, avg_frametime: %lli, avg_cputime: %lli, avg_vdptime: %lli, vdp_enabled: %lli, vdp_disabled: %lli, cpal_updates: %lli",
          cpu_mhz, frames, dropped_frames, frame_time / frames, cpu_time / frames, vdp_time / frames, vdp_display_enabled, vdp_display_disabled, cpal_updates);
      } else {
        set_overscan_border(current_overscan_color);

        ESP_LOGI(TAG, "cpu_mhz: %.6f, fps: %lli, dropped: %lli, avg_frametime: %lli, avg_cputime: %lli, avg_vdptime: %lli, vdp_enabled: %lli, vdp_disabled: %lli, cpal_updates: %lli",
          cpu_mhz, frames, dropped_frames, frame_time / frames, cpu_time / frames, vdp_time / frames, vdp_display_enabled, vdp_display_disabled, cpal_updates);
      }

      frames = 0;
      cpu_cycles = 0;
      dropped_frames = 0;
      frame_time = 0;
      cpu_time = 0;
      vdp_time = 0;

      vdp_display_enabled = 0;
      vdp_display_disabled = 0;
      cpal_updates = 0;

      start = esp_timer_get_time();
    }
  }
}

void ping_task() {
  TickType_t xLastWakeTime = xTaskGetTickCount ();
  while(1) {
    xTaskDelayUntil(&xLastWakeTime, 120);
    ESP_LOGI(TAG, "Alive and well");
  }
}

void app_main() {
  ESP_LOGI(TAG, "\n"
  "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"
  "!!!!!!!!!!!!!! Not A Plumber !!!!!!!!!!!!!!!!!\n"
  "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"

  "                      _.-*'\"\"`*-._\n"
  "                _.-*'                  `*-._\n"
  "             .-'                            `-.\n"
  "  /`-.    .-'                  _.              `-.\n"
  " :    `..'                  .-'_ .                `.\n"
  " |    .'                 .-'_.' \\ .                 \\\n"
  " |   /                 .' .*     ;               .-'\"\n"
  " :   L                    `.     | ;          .-'\n"
  "  \\.' `*.          .-*\"*-.  `.   ; |        .'\n"
  "  /      \\        '       `.  `-'  ;      .'\n"
  " : .'\"`.  .       .-*'`*-.  \\     .      (_\n"
  " |              .'        \\  .             `*-.\n"
  " |.     .      /           ;                   `-.\n"
  " :    db      '       d$b  |                      `-.\n"
  " .   :PT;.   '       :P\"T; :                         `.\n"
  " :   :bd;   '        :b_d; :                           \\\n"
  " |   :$$; `'         :$$$; |                            \\\n"
  " |    TP              T$P  '                             ;\n"
  " :                        /.-*'\"`.                       |\n"
  ".sdP^T$bs.               /'       \\\n"
  "$$$._.$$$$b.--._      _.'   .--.   ;\n"
  "`*$$$$$$P*'     `*--*'     '  / \\  :\n"
  "   \\                        .'   ; ;\n"
  "    `.                  _.-'    ' /\n"
  "      `*-.                      .'\n"
  "          `*-._            _.-*'\n"
  "               `*=--..--=*'\n");

  available_ram("start");

  bsp_init();
  available_ram("bsp_init");

  bsp_rp2040_init();
  available_ram("bsp_rp2040_init");

  bsp_ice40_init();
  available_ram("bsp_ice40_init");

  ili9341 = get_ili9341();
  ice40 = get_ice40();

  ice40_lcd_init(ice40, ili9341);
  available_ram("ice40_lcd_init");

  audio_init();
  available_ram("audio_init");

  gpio_set_direction(GPIO_SD_PWR, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SD_PWR, 1);
  ws2812_init(GPIO_LED_DATA);
  available_ram("ws2812_init");

  button_queue = get_rp2040()->queue;
  // nvs_flash_init();

  SMS_init();
  available_ram("SMS_init");

  ESP_LOGI(TAG, "Starting video thread");
  video_queue = xQueueCreate(1, sizeof(uint16_t *));
  video_mutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(&video_task, "video_task", 2048, NULL, 5, NULL, 0);
  available_ram("video_task");

  //xTaskCreatePinnedToCore(&ping_task, "ping_task", 2048, NULL, 5, NULL, 0);

  SMS_set_colour_callback(core_colour_callback);
  SMS_set_vblank_callback(core_vblank_callback);
  SMS_set_apu_callback(core_apu_callback, AUDIO_FREQ);

  size_t screen_size = SMS_SCREEN_WIDTH * SMS_SCREEN_HEIGHT * 2;
  backbuffer[0] = videobuffer_allocate(SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT, screen_size / 4096);
  backbuffer[1] = videobuffer_allocate(SMS_SCREEN_WIDTH, SMS_SCREEN_HEIGHT, screen_size / 4096);
  available_ram("backbuffer");

  SMS_set_pixels(backbuffer[0], SMS_SCREEN_WIDTH, 2);

  size_t rom_size = rom_end - rom_start;
  uint8_t* rom = heap_caps_malloc(rom_size, MALLOC_CAP_SPIRAM);
  if (!rom) {
    ESP_LOGE(TAG, "Allocation of rom in SPIRAM failed. Attempted to allocate %i bytes", rom_size);
  }
  memcpy(rom, rom_start, rom_size);
  SMS_loadrom(rom, rom_size, SMS_System_SMS);
  ESP_LOGI(TAG, "ROM loaded, crc: 0x%08X", sms.crc);

  overscan_buffer = heap_caps_malloc(OVERSCAN_BUFFER_SIZE + 1, MALLOC_CAP_SPIRAM);
  available_ram("overscan_buffer");

  // Also sets screen rect
  set_overscan_border(0);

  available_ram("done initializing");
  main_loop();
}
