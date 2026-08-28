#pragma once
#include "common.h"
#include "renderer.h"
#include "runner.h"

#include <webgpu.h>

#define MAX_TEXTURES 64

typedef struct {
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
} WGPURender;

Renderer* WGPURender_create(void);