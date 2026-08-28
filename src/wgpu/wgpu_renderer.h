#pragma once
#include "common.h"
#include "renderer.h"
#include "runner.h"

#include <webgpu.h>

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
} WGPURender;

Renderer* WGPURender_create(void);