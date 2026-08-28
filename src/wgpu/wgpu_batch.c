#include "wgpu_batch.h"
#include "wgpu_renderer.h"
#include <webgpu.h>

const char* shaderCode = R"(
struct Sprite {
    position: vec2<f32>,
    size: vec2<f32>,
    color: vec4<f32>,
};

struct Uniforms {
    sprites: array<Sprite, 128>,
    count: u32,
    width: u32,
    height: u32,
    pad: u32,
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec4<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) in_vertex_index: u32) -> VertexOutput {
    let sprite_index = in_vertex_index / 6u;
    let corner_index = in_vertex_index % 6u;

    if (sprite_index >= uniforms.count) {
        return VertexOutput(vec4f(0.0), vec4f(0.0));
    }

    let sprite = uniforms.sprites[sprite_index];

    var local_pos = vec2f(0.0, 0.0);
    switch (corner_index) {
        case 0u: { local_pos = vec2f(0.0, 0.0); } // Top-Left
        case 1u: { local_pos = vec2f(0.0, 1.0); } // Bottom-Left
        case 2u: { local_pos = vec2f(1.0, 1.0); } // Bottom-Right
        case 3u: { local_pos = vec2f(0.0, 0.0); } // Top-Left
        case 4u: { local_pos = vec2f(1.0, 1.0); } // Bottom-Right
        case 5u: { local_pos = vec2f(1.0, 0.0); } // Top-Right
        default: {}
    }

    let screen_w = f32(uniforms.width);
    let screen_h = f32(uniforms.height);
    
    let pixel_x = sprite.position.x + local_pos.x * sprite.size.x;
    let pixel_y = sprite.position.y + local_pos.y * sprite.size.y;

    let ndc_x = (pixel_x / screen_w) * 2.0 - 1.0;
    let ndc_y = 1.0 - (pixel_y / screen_h) * 2.0;

    var output: VertexOutput;
    output.position = vec4f(ndc_x, ndc_y, 0.0, 1.0);
    output.color = sprite.color;
    
    return output;
}


@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    return in.color;
}

)";

void WGPURender_batchBegin(WGPURender* self) {
    if (self->batchPipeline) {
        self->batchSpriteCount = 0;
        return;
    }

    WGPUShaderModuleDescriptor shaderDesc = {};

    WGPUShaderSourceWGSL shaderCodeDesc = {};
    shaderCodeDesc.chain.next = nullptr;
    shaderCodeDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
    shaderCodeDesc.code = (WGPUStringView){ .data = shaderCode, .length = strlen(shaderCode) };

    shaderDesc.nextInChain = &shaderCodeDesc.chain;
    self->batchShader = wgpuDeviceCreateShaderModule(self->device, &shaderDesc);


    self->batchSpriteCount = 0;

    WGPUBufferDescriptor bufferDesc = {};
    bufferDesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    bufferDesc.size = sizeof(WGPUUniforms);
    bufferDesc.mappedAtCreation = false;

    self->batchUniforms = wgpuDeviceCreateBuffer(self->device, &bufferDesc);

    WGPUBindGroupLayoutEntry layoutEntry = {};
    layoutEntry.binding = 0;
    layoutEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    layoutEntry.buffer.type = WGPUBufferBindingType_Uniform;
    layoutEntry.buffer.hasDynamicOffset = false;
    layoutEntry.buffer.minBindingSize = sizeof(WGPUUniforms);

    WGPUBindGroupLayoutDescriptor layoutDesc = {};
    layoutDesc.entryCount = 1;
    layoutDesc.entries = &layoutEntry;

    WGPUBindGroupLayout bindGroupLayout = wgpuDeviceCreateBindGroupLayout(self->device, &layoutDesc);


    WGPURenderPipelineDescriptor pipelineDesc = {};

    pipelineDesc.vertex.bufferCount = 0;
    pipelineDesc.vertex.buffers = NULL;

    pipelineDesc.vertex.module = self->batchShader;
    pipelineDesc.vertex.entryPoint = (WGPUStringView){ .data = "vs_main", .length = 7 };
    pipelineDesc.vertex.constantCount = 0;
    pipelineDesc.vertex.constants = NULL;

    pipelineDesc.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipelineDesc.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
    pipelineDesc.primitive.frontFace = WGPUFrontFace_CCW;
    pipelineDesc.primitive.cullMode = WGPUCullMode_None;

    WGPUFragmentState fragmentState = {};
    fragmentState.module = self->batchShader;
    fragmentState.entryPoint = (WGPUStringView){ .data = "fs_main", .length = 7 };
    fragmentState.constantCount = 0;
    fragmentState.constants = NULL;
    
    WGPUBlendState blendState = {};
    blendState.color.srcFactor = WGPUBlendFactor_SrcAlpha;
    blendState.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blendState.color.operation = WGPUBlendOperation_Add;
    blendState.alpha.srcFactor = WGPUBlendFactor_Zero;
    blendState.alpha.dstFactor = WGPUBlendFactor_One;
    blendState.alpha.operation = WGPUBlendOperation_Add;

    WGPUColorTargetState colorTarget = {};
    colorTarget.format = WGPUTextureFormat_BGRA8Unorm;
    colorTarget.blend = &blendState;
    colorTarget.writeMask = WGPUColorWriteMask_All;
    
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTarget;
    pipelineDesc.fragment = &fragmentState;

    pipelineDesc.depthStencil = NULL;

    
	pipelineDesc.multisample.count = 1;
    pipelineDesc.multisample.mask = ~0u;
    pipelineDesc.multisample.alphaToCoverageEnabled = false;
    
    WGPUPipelineLayoutDescriptor pipelineLayoutDesc = {};
    pipelineLayoutDesc.bindGroupLayoutCount = 1;
    pipelineLayoutDesc.bindGroupLayouts = &bindGroupLayout;
    
    WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(self->device, &pipelineLayoutDesc);

    pipelineDesc.layout = pipelineLayout;

    WGPUBindGroupEntry bindEntry = {};
    bindEntry.binding = 0;
    bindEntry.buffer = self->batchUniforms;
    bindEntry.offset = 0;
    bindEntry.size = sizeof(WGPUUniforms);

    WGPUBindGroupDescriptor bindGroupDesc = {};
    bindGroupDesc.layout = bindGroupLayout;
    bindGroupDesc.entryCount = 1;
    bindGroupDesc.entries = &bindEntry;

    self->batchBindGroup = wgpuDeviceCreateBindGroup(self->device, &bindGroupDesc);

    self->batchPipeline = wgpuDeviceCreateRenderPipeline(
        self->device,
        &pipelineDesc
    );

    wgpuShaderModuleRelease(self->batchShader);
}

void WGPURender_batchDraw(
    WGPURender* self,
    float x,
    float y,
    float w,
    float h,
    float r,
    float g,
    float b,
    float a
) {
    self->batchSprites[self->batchSpriteCount].position.x = x;
    self->batchSprites[self->batchSpriteCount].position.y = y;
    self->batchSprites[self->batchSpriteCount].size.x = w;
    self->batchSprites[self->batchSpriteCount].size.y = h;

    self->batchSprites[self->batchSpriteCount].color.r = r;
    self->batchSprites[self->batchSpriteCount].color.g = g;
    self->batchSprites[self->batchSpriteCount].color.b = b;
    self->batchSprites[self->batchSpriteCount].color.a = a;
    self->batchSpriteCount++;
}

void WGPURender_batchEnd(WGPURender* self) {
    WGPUUniforms uniforms = {};
    memcpy(uniforms.sprites,
        self->batchSprites,
        sizeof(self->batchSprites[0]) * self->batchSpriteCount);

    uniforms.count = self->batchSpriteCount;

    uniforms.width = 640;
    uniforms.height = 480;

    wgpuQueueWriteBuffer(
        self->queue,
        self->batchUniforms,
        0,
        &uniforms,
        sizeof(uniforms)
    );

    wgpuRenderPassEncoderSetPipeline(self->renderPass, self->batchPipeline);
    wgpuRenderPassEncoderSetBindGroup(self->renderPass, 0, self->batchBindGroup, 0, NULL);
    wgpuRenderPassEncoderDraw(
        self->renderPass,
        6 * self->batchSpriteCount,
        1,
        0,
        0
    );
}