#pragma once
#include <stdalign.h>
#include <stdint.h>
#include <string.h>
#include <webgpu.h>

typedef struct WGPURender WGPURender;

#define MAX_SPRITES 128

typedef struct {
	float x;
	float y;
} vec2_f32;

typedef struct {
	float r, g, b, a;
} vec4_f32;

typedef struct {
	vec2_f32 position;
	vec2_f32 size;
	vec4_f32 color;
} WGPUSprite;

typedef struct {
	WGPUSprite sprites[128];
	uint32_t count;
	uint32_t width;
	uint32_t height;
	uint32_t pad;
} WGPUUniforms;

void WGPURender_batchDraw(WGPURender* self, float x, float y, float w, float h, float r, float g,
						  float b, float a);
void WGPURender_batchEnd(WGPURender* self);
void WGPURender_batchBegin(WGPURender* self);