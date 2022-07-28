#include <stdlib.h>
#include <stdio.h>

#include "videobuffer.h"

videobuffer_t* videobuffer_allocate(uint16_t width, uint16_t height, short part_numb) {
	videobuffer_t* buffer = malloc(sizeof(videobuffer_t));
	buffer->size = width * height * 2;

	buffer->part_size = buffer->size / part_numb;
	buffer->lines_per_part = buffer->part_size / (width * 2);
	buffer->part_numb = part_numb;
	buffer->parts = malloc(part_numb * sizeof(void*));
	buffer->current_part = 0;

	for (int i = 0; i < part_numb; ++i) {
		buffer->parts[i] = malloc(buffer->part_size);
	}

	return buffer;
}
