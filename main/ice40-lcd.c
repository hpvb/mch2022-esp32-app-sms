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
#include <zlib.h>

#include "ili9341.h"
#include "ice40.h"

#undef TAG
#define TAG "ice40-lcd"

extern const uint8_t lcd_controller_start[] asm("_binary_lcd_controller_bin_gz_start");
extern const uint8_t lcd_controller_end[] asm("_binary_lcd_controller_bin_gz_end");
const size_t lcd_controller_size = 104090;

static const uint8_t lcd_init_data[] = {
   0xf2,
   3, 0xef,              0x03, 0x80, 0x02,             // ? (undocumented cmd)
   3, ILI9341_POWERB,    0x00, 0xc1, 0x30,             // Power control B
   4, ILI9341_POWER_SEQ, 0x64, 0x03, 0x12, 0x81,       // Power on sequence control
   3, ILI9341_DTCA,      0x85, 0x00, 0x78,             // Driver timing control A
   5, ILI9341_POWERA,    0x39, 0x2c, 0x00, 0x34, 0x02, // Power control A
   1, ILI9341_PRC,       0x20,                         // Pump ratio control
   2, ILI9341_DTCB,      0x00, 0x00,                   // Driver timing control B
   1, ILI9341_LCMCTRL,   0x23,                         // Power control 1
   1, ILI9341_POWER2,    0x10,                         // Power control 2
   2, ILI9341_VCOM1,     0x3e, 0x28,                   // VCOM Control 1
   1, ILI9341_VCOM2,     0x86,                         // VCOM Control 2
   1, ILI9341_COLMOD,    0x55,                         // Pixel Format: 16b
   3, ILI9341_DFC,       0x08, 0x82, 0x27,             // Display Function Control
   1, ILI9341_3GAMMA_EN, 0x00,                         // 3 Gamma control disable
   1, ILI9341_GAMSET,    0x01,                         // Gamma Set
  15, ILI9341_PVGAMCTRL, 0x0f, 0x31, 0x2b, 0x0c, 0x0e, // Positive Gamma Correction
                         0x08, 0x4e, 0xf1, 0x37, 0x07,
                         0x10, 0x03, 0x0e, 0x09, 0x00,
  15, ILI9341_NVGAMCTRL, 0x00, 0x0e, 0x14, 0x03, 0x11, // Negative Gamma Correction
                         0x07, 0x31, 0xc1, 0x48, 0x08,
                         0x0f, 0x0c, 0x31, 0x36, 0x0f,
   0, ILI9341_SLPOUT,                                  // Sleep Out
   0, ILI9341_DISPON,                                  // Display ON
   1, ILI9341_TEON,      0x00,                         // Tearing Effect Line ON
   1, ILI9341_MADCTL,    0x28,                         // Memory Access Control
   4, ILI9341_CASET,     0x00, 0x00, 0x01, 0x3f,       // Column Address Set
   4, ILI9341_RASET,     0x00, 0x00, 0x00, 0xef,       // Page Address Set
   1, ILI9341_FRMCTR1,   0x10,                         // 63 Hz refresh rate
   0, ILI9341_RAMWR,
};

typedef voidpf (*alloc_func) OF((voidpf opaque, uInt items, uInt size));

void* z_mem_alloc(void *opaque, unsigned count, unsigned size) {
  return heap_caps_malloc(count * size, MALLOC_CAP_SPIRAM);
}

void z_mem_free(void *opaque, void *address) {
  free(address);
}

static void decompress_bitstream(uint8_t* buffer) {
  ESP_LOGI(TAG, "Decompressing ICE40 bitstream from %p, size %i", lcd_controller_start, lcd_controller_end - lcd_controller_start);

  z_stream strm;
  strm.next_in = (Bytef *) lcd_controller_start;
  strm.avail_in = lcd_controller_end - lcd_controller_start;
  strm.total_out = 0;
  strm.zalloc = z_mem_alloc;
  strm.zfree = z_mem_free;
  strm.opaque = Z_NULL;

  int32_t res;
  if (res = inflateInit2(&strm, (16 + MAX_WBITS)) != Z_OK) {
    ESP_LOGE(TAG, "InflateInit2 error: %i", res);
    return;
  }

  while (1) {
    strm.next_out = (Bytef *) (buffer + strm.total_out);
    strm.avail_out = lcd_controller_size - strm.total_out;

    int32_t err = inflate (&strm, Z_SYNC_FLUSH);
    if (err == Z_STREAM_END) break;
    else if (err != Z_OK)  {
      ESP_LOGE(TAG, "Zlib error");
      break;
    }
  }

  if (inflateEnd (&strm) != Z_OK) {
    ESP_LOGE(TAG, "Zlib error");
    return;
  }
}

void ice40_lcd_send_command(ICE40 *ice40, uint8_t cmd, const uint8_t* data, uint8_t size) {
  uint8_t packet[64];
  memset(packet, 0, sizeof(packet));
  packet[0] = 0xf2;
  packet[1] = size;
  packet[2] = cmd;

  for(int i = 0; i < size; ++i) {
    packet[3 + i] = data[i];
  }
  
  ice40_send(ice40, packet, size + 3);
}

void ice40_lcd_set_addr_window(ICE40 *ice40, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint32_t xa = ((uint32_t)x << 16) | (x+w-1);
    uint32_t ya = ((uint32_t)y << 16) | (y+h-1);

    uint8_t packet[] = {
      0xf2,
      0x04, ILI9341_CASET, (xa>>24)&0xFF, (xa>>16)&0xFF, (xa>> 8)&0xFF, xa &0xFF,
      0x04, ILI9341_RASET, (ya>>24)&0xFF, (ya>>16)&0xFF, (ya>> 8)&0xFF, ya &0xFF,
      0x00, ILI9341_RAMWR };

    ice40_send(ice40, packet, sizeof(packet));
}

__attribute__((always_inline)) inline void ice40_lcd_send_turbo(ICE40 *ice40, const uint8_t* data, uint32_t length) {
    spi_transaction_t transaction = {
        .user = (void*) ice40,
        .length = length * 8,
        .tx_buffer = data,
        .rx_buffer = NULL
    };
    spi_device_transmit(ice40->_spi_device_turbo, &transaction);
}

void ice40_lcd_init(ICE40 *ice40, ILI9341 *ili9341) {
  ESP_LOGI(TAG, "Decompressing ICE40 bitstream");
  uint8_t* bitstream = heap_caps_malloc(lcd_controller_size, MALLOC_CAP_SPIRAM);
  decompress_bitstream(bitstream);
  ESP_LOGI(TAG, "Done decompressing ICE40 bitstream");

  ESP_LOGI(TAG, "Loading decompressed ICE40 bitstream");
  ice40_load_bitstream(ice40, bitstream, lcd_controller_size);
  free(bitstream);
  ESP_LOGI(TAG, "ICE40 bitstream loaded");

  ESP_LOGI(TAG, "Switching LCD to ICE40");
  ili9341_deinit(ili9341);
  ESP_LOGI(TAG, "Done LCD to ICE40");

  ESP_LOGI(TAG, "Sending LCD init data");
  ice40_send(ice40, lcd_init_data, sizeof(lcd_init_data));
  ESP_LOGI(TAG, "Done sending LCD init data");
}
