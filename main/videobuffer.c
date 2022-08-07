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
#include "esp_heap_caps.h"

#include "videobuffer.h"

#undef TAG
#define TAG "videobuffer"

videobuffer_t* videobuffer_allocate(uint16_t width, uint16_t height, uint16_t part_numb, uint16_t extra_parts) {
    videobuffer_t* buffer = malloc(sizeof(videobuffer_t));
    buffer->size = width * height * 2;

    buffer->part_size = buffer->size / part_numb;
    buffer->lines_per_part = buffer->part_size / (width * 2);
    buffer->part_numb = part_numb + extra_parts;
    buffer->parts_per_frame = part_numb;
    buffer->writer_offset = 0;

    buffer->real_parts = heap_caps_malloc(buffer->part_numb * sizeof(void*), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    buffer->parts = heap_caps_malloc(buffer->part_numb * sizeof(void*), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

    for (int i = 0; i < buffer->part_numb; ++i) {
        buffer->real_parts[i] = heap_caps_calloc(buffer->part_size + 1, 1, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!buffer->real_parts[i])
           ESP_LOGE(TAG, "Failed to allocate buffer part %i!\n", i);
        buffer->real_parts[i][0] = 0xf3;
        buffer->parts[i] = buffer->real_parts[i] + 1;
    }

    return buffer;
}

void videobuffer_deallocate(videobuffer_t* buffer) {
    for (int i = 0; i < buffer->part_numb; ++i) {
        free(buffer->real_parts[i]);
    }

    free(buffer->real_parts);
    free(buffer->parts);
    free(buffer);
}
