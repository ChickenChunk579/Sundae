#pragma once
#include <stdint.h>

static inline void WGPUColor_fromGM(uint32_t color, float alpha, float* r, float* g, float* b,
									float* a) {
	*r = (float)(color & 0xFF) / 255.0f;
	*g = (float)((color >> 8) & 0xFF) / 255.0f;
	*b = (float)((color >> 16) & 0xFF) / 255.0f;
	*a = alpha;
}