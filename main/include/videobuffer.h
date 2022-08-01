#pragma once

typedef struct videobuffer {
    size_t size; // size of the total buffer

    uint8_t **real_parts; // frame buffer parts
    uint8_t **parts; // frame buffer parts
    size_t part_size; // size of individual parts
    short part_numb; // number of parts
    short lines_per_part;
} videobuffer_t;

videobuffer_t* videobuffer_allocate(uint16_t width, uint16_t height, short part_numb);
