#pragma once
#include <stddef.h>
#include <webgpu.h>

#include "common.h"
#include "renderer.h"
#include "runner.h"
#include "wgpu_batch.h"

#define MAX_TEXTURES 64

struct WGPURender {
	Renderer base;

	WGPUInstance instance;
	WGPUAdapter adapter;
	WGPUDevice device;
	WGPUQueue queue;
	WGPUSurface surface;

	WGPURenderPassEncoder renderPass;
	WGPUCommandEncoder encoder;
	WGPUTextureView targetView;

	bool* textureLoaded;
	int32_t* textureWidths;
	int32_t* textureHeights;
	WGPUTextureView* wgpuTextureViews;
	WGPUTexture* wgpuTextures;

	WGPUShaderModule batchShader;
	WGPUTextureView batchTextureView;
	WGPUBindGroup batchBindGroup;
	WGPURenderPipeline batchPipeline;
	WGPUBuffer batchStorage;

	WGPUColor clearColor;

	WGPUSprite batchSprites[MAX_SPRITES];
	size_t batchSpriteCount;
};

Renderer* WGPURender_create(void);