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

#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"

#include "videobuffer.h"

#undef TAG
#define TAG "videobuffer"

videobuffer_t* videobuffer_allocate(uint16_t width, uint16_t height, short part_numb) {
    videobuffer_t* buffer = malloc(sizeof(videobuffer_t));
    buffer->size = width * height * 2;

    buffer->part_size = buffer->size / part_numb;
    buffer->lines_per_part = buffer->part_size / (width * 2);
    buffer->part_numb = part_numb;
    buffer->real_parts = malloc(part_numb * sizeof(void*));
    buffer->parts = malloc(part_numb * sizeof(void*));

    for (int i = 0; i < part_numb; ++i) {
        buffer->real_parts[i] = calloc(buffer->part_size + 1, 1);
        if (!buffer->real_parts[i])
           ESP_LOGE(TAG, "Failed to allocate buffer part %i!\n", i);
        buffer->real_parts[i][0] = 0xf3;
        buffer->parts[i] = buffer->real_parts[i] + 1;
    }

    return buffer;
}
