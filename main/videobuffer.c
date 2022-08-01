#include <stdlib.h>
#include <stdio.h>

#include "videobuffer.h"

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
			printf("Failed to allocate buffer part %i!\n", i);
		buffer->real_parts[i][0] = 0xf3;
		buffer->parts[i] = buffer->real_parts[i] + 1;
	}

	return buffer;

}
