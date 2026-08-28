#include "wgpu_renderer.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <webgpu.h>

#include "data_win.h"
#include "image_decoder.h"
#include "platformdefs.h"
#include "renderer.h"
#include "wgpu_batch.h"

static RendererVtable wgpuVtable;

void onWGPUError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message,
				 void* userdata1, void* userdata2) {
	(void)device;
	(void)userdata1;
	(void)userdata2;

	fprintf(stderr, "WGPU ERROR type=%d: %.*s\n", (int)type, (int)message.length, message.data);
}

void onAdapterRequestEnded(WGPURequestAdapterStatus status, WGPUAdapter adapter,
						   WGPUStringView message, WGPU_NULLABLE void* userdata1,
						   WGPU_NULLABLE void* userdata2) {
	if (status != WGPURequestAdapterStatus_Success) {
		logError("wgpu: adapter request failed: %d\n", status);
		logError("wgpu: %s", message.data);
		exit(1);
	}
	*(bool*)(userdata1) = true;
	*(WGPUAdapter*)(userdata2) = adapter;
}

void onDeviceRequestEnded(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
						  WGPU_NULLABLE void* userdata1, WGPU_NULLABLE void* userdata2) {
	if (status != WGPURequestDeviceStatus_Success) {
		logError("wgpu: device request failed: %d\n", status);
		logError("wgpu: %s", message.data);
		exit(1);
	}
	*(bool*)(userdata1) = true;
	*(WGPUDevice*)(userdata2) = device;
}

bool WGPURender_ensureTextureLoaded(Renderer* renderer, uint32_t pageId) {
	WGPURender* self = (WGPURender*)renderer;
	if (self->textureLoaded[pageId]) return (self->textureWidths[pageId] != 0);

	self->textureLoaded[pageId] = true;

	DataWin* dw = self->base.dataWin;
	Texture* txtr = &dw->txtr.textures[pageId];

	if (!txtr) {
		logWarn("wgpu: attempted to load non extistant texture page %d\n", txtr);
		return false;
	}

	DataWin_loadTxtrIfNeeded(dw, pageId);

	int w, h;
	bool gm2022_5 = DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0);
	uint8_t* pixels =
		ImageDecoder_decodeToRgba(txtr->blobData, (size_t)txtr->blobSize, gm2022_5, &w, &h);
	if (pixels == nullptr) {
		logWarn("wgpu: Failed to decode TXTR page %u\n", pageId);
		return false;
	}
	if (!txtr->mapped) {
		free(txtr->blobData);
		txtr->blobData = nullptr;
		logWarn("wgpu: texture not mapped\n");
		return false;
	}

	logDebug("wgpu: decoded texture successfully\n");

	self->textureWidths[pageId] = w;
	self->textureHeights[pageId] = h;

	WGPUTextureDescriptor textureDesc = {};
	textureDesc.nextInChain = nullptr;
	textureDesc.dimension = WGPUTextureDimension_2D;
	textureDesc.format = WGPUTextureFormat_RGBA8Unorm;
	textureDesc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
	textureDesc.viewFormats = NULL;
	textureDesc.viewFormatCount = 0;
	textureDesc.sampleCount = 1;
	textureDesc.mipLevelCount = 1;

	WGPUExtent3D textureSize = {};
	textureSize.width = w;
	textureSize.height = h;
	textureSize.depthOrArrayLayers = 1;

	textureDesc.size = textureSize;

	WGPUTexture tex = wgpuDeviceCreateTexture(self->device, &textureDesc);

	logDebug("wgpu: created texture\n");

	WGPUTexelCopyTextureInfo copyInfo = {};
	copyInfo.texture = tex;
	copyInfo.aspect = WGPUTextureAspect_All;
	copyInfo.mipLevel = 0;

	WGPUTexelCopyBufferLayout copyBufferLayout = {};
	copyBufferLayout.bytesPerRow = w * 4;
	copyBufferLayout.offset = 0;
	copyBufferLayout.rowsPerImage = h;

	wgpuQueueWriteTexture(self->queue, &copyInfo, pixels, (size_t)((w * h) * 4), &copyBufferLayout,
						  &textureSize);

	logDebug("wgpu: wrote texture\n");

	logDebug("wgpu: loaded texture page %d into texture %x\n", tex);

	free(pixels);

	return true;
}

void WGPURender_init(Renderer* renderer, DataWin* dataWin) {
	WGPURender* self = (WGPURender*)renderer;

	renderer->dataWin = dataWin;

	Matrix4f world;
	Matrix4f_identity(&world);
	renderer->gmlMatrices[MATRIX_WORLD] = world;

	WGPUInstanceDescriptor desc = {};
	desc.nextInChain = nullptr;

	WGPUInstance instance = wgpuCreateInstance(&desc);
	if (!instance) {
		logError("wgpu: failed to create instance.");
		exit(1);
	}

	logDebug("wgpu: instance: %x\n", instance);

	self->instance = instance;

	WGPURequestAdapterOptions adapterOpts = {};
	adapterOpts.nextInChain = nullptr;

	bool requestEnded = false;

	WGPURequestAdapterCallbackInfo adapterCallbackInfo = {};
	adapterCallbackInfo.mode = WGPUCallbackMode_WaitAnyOnly;
	adapterCallbackInfo.callback = onAdapterRequestEnded;
	adapterCallbackInfo.userdata1 = &requestEnded;
	adapterCallbackInfo.userdata2 = &self->adapter;

	wgpuInstanceRequestAdapter(self->instance, &adapterOpts, adapterCallbackInfo);

	while (!requestEnded) {
		logDebug("wgpu: waiting for adapter\n");
		usleep(1000000);
	}

	logDebug("wgpu: adapter: %x\n", self->adapter);

	WGPUDeviceDescriptor deviceDesc = {};
	deviceDesc.nextInChain = nullptr;
	deviceDesc.requiredFeatureCount = 0;
	deviceDesc.requiredLimits = nullptr;
	deviceDesc.defaultQueue.nextInChain = nullptr;
	deviceDesc.nextInChain = nullptr;
	deviceDesc.uncapturedErrorCallbackInfo.nextInChain = nullptr;
	deviceDesc.uncapturedErrorCallbackInfo.callback = onWGPUError;
	deviceDesc.uncapturedErrorCallbackInfo.userdata1 = NULL;
	deviceDesc.uncapturedErrorCallbackInfo.userdata2 = NULL;
	requestEnded = false;

	WGPURequestDeviceCallbackInfo deviceCallbackInfo = {};
	deviceCallbackInfo.mode = WGPUCallbackMode_WaitAnyOnly;
	deviceCallbackInfo.callback = onDeviceRequestEnded;
	deviceCallbackInfo.userdata1 = &requestEnded;
	deviceCallbackInfo.userdata2 = &self->device;

	wgpuAdapterRequestDevice(self->adapter, &deviceDesc, deviceCallbackInfo);

	while (!requestEnded) {
		logDebug("wgpu: waiting for device\n");
		usleep(1000000);
	}

	logDebug("wgpu: device: %x\n", self->device);

	self->queue = wgpuDeviceGetQueue(self->device);
	logDebug("wgpu: queue: %x\n", self->queue);

	self->surface = platformCreateWgpuSurface(instance);

	logDebug("wgpu: surface: %x\n", self->surface);

	int width = 0;
	int height = 0;
	platformGetWindowSize(&width, &height);

	WGPUSurfaceConfiguration config = {};
	config.width = width;
	config.height = height;
	config.usage = WGPUTextureUsage_RenderAttachment;
	WGPUTextureFormat surfaceFormat = WGPUTextureFormat_BGRA8Unorm;
	config.format = surfaceFormat;

	config.viewFormatCount = 0;
	config.viewFormats = NULL;
	config.device = self->device;
	config.presentMode = WGPUPresentMode_Fifo;
	config.alphaMode = WGPUCompositeAlphaMode_Auto;

	wgpuSurfaceConfigure(self->surface, &config);
}

void WGPURender_destroy(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;

	wgpuQueueRelease(self->queue);
	wgpuSurfaceRelease(self->surface);
	wgpuDeviceRelease(self->device);
	wgpuAdapterRelease(self->adapter);
	wgpuInstanceRelease(self->instance);
}

WGPUTextureView WGPURender_getNextSurfaceTextureView(WGPURender* self) {
	WGPUSurfaceTexture surfaceTexture;
	wgpuSurfaceGetCurrentTexture(self->surface, &surfaceTexture);
	if (surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
		surfaceTexture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
		return nullptr;
	}

	WGPUTextureViewDescriptor viewDescriptor = {};
	viewDescriptor.nextInChain = nullptr;
	viewDescriptor.format = wgpuTextureGetFormat(surfaceTexture.texture);
	viewDescriptor.dimension = WGPUTextureViewDimension_2D;
	viewDescriptor.baseMipLevel = 0;
	viewDescriptor.mipLevelCount = 1;
	viewDescriptor.baseArrayLayer = 0;
	viewDescriptor.arrayLayerCount = 1;
	viewDescriptor.aspect = WGPUTextureAspect_All;
	WGPUTextureView targetView = wgpuTextureCreateView(surfaceTexture.texture, &viewDescriptor);

	return targetView;
}

void WGPURender_beginFrame(Renderer* renderer, int32_t gameW, int32_t gameH, int32_t windowW,
						   int32_t windowH) {
	WGPURender* self = (WGPURender*)renderer;

	self->targetView = WGPURender_getNextSurfaceTextureView(self);
	if (!self->targetView) return;

	WGPUCommandEncoderDescriptor encoderDesc = {};
	encoderDesc.nextInChain = NULL;
	self->encoder = wgpuDeviceCreateCommandEncoder(self->device, &encoderDesc);

	WGPURenderPassDescriptor renderPassDesc = {};
	renderPassDesc.nextInChain = nullptr;

	WGPURenderPassColorAttachment renderPassColorAttachment = {};
	renderPassColorAttachment.view = self->targetView;
	renderPassColorAttachment.resolveTarget = nullptr;
	renderPassColorAttachment.loadOp = WGPULoadOp_Clear;
	renderPassColorAttachment.storeOp = WGPUStoreOp_Store;
	static float t = 0.0f;
	t += 0.01f;

	renderPassColorAttachment.clearValue = (WGPUColor){(sin(t) + 1.0) * 0.5, 0.0, 0.0, 1.0};
	renderPassColorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

	renderPassDesc.colorAttachmentCount = 1;
	renderPassDesc.colorAttachments = &renderPassColorAttachment;
	renderPassDesc.depthStencilAttachment = nullptr;
	renderPassDesc.timestampWrites = nullptr;

	self->renderPass = wgpuCommandEncoderBeginRenderPass(self->encoder, &renderPassDesc);

	static int frames = 0;
	static time_t last = 0;

	time_t now = time(NULL);

	frames++;

	if (now != last) {
		printf("FPS: %d\n", frames);
		frames = 0;
		last = now;
	}

	WGPURender_batchBegin(self);

	for (int i = 0; i < 100; i++) {
		float x = (float)(rand() % 640);
		float y = (float)(rand() % 480);

		float w = 25.0f + (float)(rand() % 76);
		float h = 25.0f + (float)(rand() % 76);

		float r = (float)rand() / (float)RAND_MAX;
		float g = (float)rand() / (float)RAND_MAX;
		float b = (float)rand() / (float)RAND_MAX;
		float a = 1.0f;

		WGPURender_batchDraw(self, x, y, w, h, r, g, b, a);
	}

	WGPURender_batchEnd(self);
}

WGPUBool wgpuDevicePoll(WGPUDevice device, WGPUBool wait,
						WGPU_NULLABLE void const* submissionIndex);

void WGPURender_endFrameInit(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;

	wgpuRenderPassEncoderEnd(self->renderPass);
	wgpuRenderPassEncoderRelease(self->renderPass);

	WGPUCommandBufferDescriptor cmdBufferDescriptor = {};
	cmdBufferDescriptor.nextInChain = nullptr;
	WGPUCommandBuffer command = wgpuCommandEncoderFinish(self->encoder, &cmdBufferDescriptor);
	wgpuCommandEncoderRelease(self->encoder);

	wgpuQueueSubmit(self->queue, 1, &command);
	wgpuCommandBufferRelease(command);

	wgpuTextureViewRelease(self->targetView);

	wgpuDevicePoll(self->device, false, NULL);

	wgpuSurfacePresent(self->surface);
}

void WGPURender_endFrameEnd(Renderer* renderer) { WGPURender* self = (WGPURender*)renderer; }

void WGPURender_beginView(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW,
						  int32_t viewH, int32_t portX, int32_t portY, int32_t portW, int32_t portH,
						  float viewAngle) {
	WGPURender* self = (WGPURender*)renderer;
}

void WGPURenderer_endView(Renderer* renderer) { WGPURender* self = (WGPURender*)renderer; }

void WGPURender_beginGUI(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portX,
						 int32_t portY, int32_t portW, int32_t portH, int32_t targetSurfaceId) {
	WGPURender* self = (WGPURender*)renderer;
}

void WGPURender_endGUI(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
}

void WGPURender_setGUIProjection(Renderer* renderer, int32_t guiW, int32_t guiH, int32_t portW,
								 int32_t portH, bool renderingToUserSurface) {
	(void)guiW;
	(void)guiH;
	(void)portW;
	(void)portH;
	(void)renderingToUserSurface;
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
}

void WGPURender_applyProjection(Renderer* renderer, const Matrix4f* viewMatrix,
								const Matrix4f* projectionMatrix) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)viewMatrix;
	(void)projectionMatrix;
}

void WGPURender_drawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX,
						   float originY, float xscale, float yscale, float angleDeg,
						   uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tpagIndex;
	(void)x;
	(void)y;
	(void)originX;
	(void)originY;
	(void)xscale;
	(void)yscale;
	(void)angleDeg;
	(void)color;
	(void)alpha;
}

void WGPURender_drawSpritePart(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX,
							   int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y,
							   float xscale, float yscale, float angleDeg, float pivotX,
							   float pivotY, uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tpagIndex;
	(void)srcOffX;
	(void)srcOffY;
	(void)srcW;
	(void)srcH;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)angleDeg;
	(void)pivotX;
	(void)pivotY;
	(void)color;
	(void)alpha;
}

void WGPURender_drawSpritePartColor(Renderer* renderer, int32_t tpagIndex, int32_t srcOffX,
									int32_t srcOffY, int32_t srcW, int32_t srcH, float x, float y,
									float xscale, float yscale, float angleDeg, float pivotX,
									float pivotY, uint32_t color1, uint32_t color2, uint32_t color3,
									uint32_t color4, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tpagIndex;
	(void)srcOffX;
	(void)srcOffY;
	(void)srcW;
	(void)srcH;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)angleDeg;
	(void)pivotX;
	(void)pivotY;
	(void)color1;
	(void)color2;
	(void)color3;
	(void)color4;
	(void)alpha;
}

void WGPURender_drawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2,
							  float y2, float x3, float y3, float x4, float y4, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tpagIndex;
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
	(void)x3;
	(void)y3;
	(void)x4;
	(void)y4;
	(void)alpha;
}

void WGPURender_drawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2,
							  uint32_t color, float alpha, bool outline) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
	(void)color;
	(void)alpha;
	(void)outline;
}

void WGPURender_drawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2,
								   uint32_t color1, uint32_t color2, uint32_t color3,
								   uint32_t color4, float alpha, bool outline) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
	(void)color1;
	(void)color2;
	(void)color3;
	(void)color4;
	(void)alpha;
	(void)outline;
}

void WGPURender_drawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width,
						 uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
	(void)width;
	(void)color;
	(void)alpha;
}

void WGPURender_drawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3,
							 float y3, uint32_t color1, uint32_t color2, uint32_t color3,
							 float alpha, bool outline) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
	(void)x3;
	(void)y3;
	(void)color1;
	(void)color2;
	(void)color3;
	(void)alpha;
	(void)outline;
}

void WGPURender_drawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2,
							  float width, uint32_t color1, uint32_t color2, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)x1;
	(void)y1;
	(void)x2;
	(void)y2;
	(void)width;
	(void)color1;
	(void)color2;
	(void)alpha;
}

void WGPURender_drawText(Renderer* renderer, const char* text, float x, float y, float xscale,
						 float yscale, float angleDeg, float lineSeparation) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)text;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)angleDeg;
	(void)lineSeparation;
}

void WGPURender_drawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale,
							  float yscale, float angleDeg, int32_t c1, int32_t c2, int32_t c3,
							  int32_t c4, float alpha, float lineSeparation) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)text;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)angleDeg;
	(void)c1;
	(void)c2;
	(void)c3;
	(void)c4;
	(void)alpha;
	(void)lineSeparation;
}

void WGPURender_flush(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
}

void WGPURender_clearScreen(Renderer* renderer, uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)color;
	(void)alpha;
}

int32_t WGPURender_createSpriteFromSurface(Renderer* renderer, int32_t surfaceID, int32_t x,
										   int32_t y, int32_t w, int32_t h, bool removeback,
										   bool smooth, int32_t xorig, int32_t yorig) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	(void)removeback;
	(void)smooth;
	(void)xorig;
	(void)yorig;
	return 0;
}

void WGPURender_deleteSprite(Renderer* renderer, int32_t spriteIndex) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)spriteIndex;
}

BlendFactors WGPURender_gpuGetBlendFactors(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;

	BlendFactors factors = {0};
	return factors;
}

int32_t WGPURender_gpuGetBlendMode(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	return 0;
}

void WGPURender_gpuSetBlendMode(Renderer* renderer, int32_t mode) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)mode;
}

void WGPURender_gpuSetBlendModeExt(Renderer* renderer, int32_t sfactor, int32_t dfactor,
								   int32_t sfactor_alpha, int32_t dfactor_alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)sfactor;
	(void)dfactor;
	(void)sfactor_alpha;
	(void)dfactor_alpha;
}

void WGPURender_gpuSetBlendEnable(Renderer* renderer, bool enable) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)enable;
}

void WGPURender_gpuSetAlphaTestEnable(Renderer* renderer, bool enable) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)enable;
}

bool WGPURender_gpuGetAlphaTestEnable(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	return false;
}

void WGPURender_gpuSetAlphaTestRef(Renderer* renderer, uint8_t ref) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)ref;
}

void WGPURender_gpuSetColorWriteEnable(Renderer* renderer, bool red, bool green, bool blue,
									   bool alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
}

void WGPURender_gpuGetColorWriteEnable(Renderer* renderer, bool* red, bool* green, bool* blue,
									   bool* alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)red;
	(void)green;
	(void)blue;
	(void)alpha;
}

bool WGPURender_gpuGetBlendEnable(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	return false;
}

void WGPURender_gpuSetFog(Renderer* renderer, bool enable, uint32_t color) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)enable;
	(void)color;
}

void WGPURender_drawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tile;
	(void)offsetX;
	(void)offsetY;
}

void WGPURender_drawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY,
								float x, float y, float xscale, float yscale, bool tileX,
								bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tpagIndex;
	(void)originX;
	(void)originY;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)tileX;
	(void)tileY;
	(void)roomW;
	(void)roomH;
	(void)color;
	(void)alpha;
}

int32_t WGPURender_createSurface(Renderer* renderer, int32_t width, int32_t height) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)width;
	(void)height;
	return 0;
}

bool WGPURender_surfaceExists(Renderer* renderer, int32_t surfaceID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	return false;
}

bool WGPURender_setRenderTarget(Renderer* renderer, int32_t surfaceID,
								bool implicitApplicationSurface) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	(void)implicitApplicationSurface;
	return false;
}

int32_t WGPURender_ensureApplicationSurface(Renderer* renderer, int32_t width, int32_t height) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)width;
	(void)height;
	return 0;
}

float WGPURender_getSurfaceWidth(Renderer* renderer, int32_t surfaceID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	return 0.0f;
}

float WGPURender_getSurfaceHeight(Renderer* renderer, int32_t surfaceID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	return 0.0f;
}

void WGPURender_drawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop,
							int32_t srcWidth, int32_t srcHeight, float x, float y, float xscale,
							float yscale, float angleDeg, uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	(void)srcLeft;
	(void)srcTop;
	(void)srcWidth;
	(void)srcHeight;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)angleDeg;
	(void)color;
	(void)alpha;
}

void WGPURender_drawSurfaceColor(Renderer* renderer, int32_t surfaceID, int32_t srcLeft,
								 int32_t srcTop, int32_t srcWidth, int32_t srcHeight, float x,
								 float y, float xscale, float yscale, float angleDeg,
								 uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4,
								 float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	(void)srcLeft;
	(void)srcTop;
	(void)srcWidth;
	(void)srcHeight;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)angleDeg;
	(void)color1;
	(void)color2;
	(void)color3;
	(void)color4;
	(void)alpha;
}

void WGPURender_drawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y,
								 float xscale, float yscale, float roomW, float roomH,
								 uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	(void)x;
	(void)y;
	(void)xscale;
	(void)yscale;
	(void)roomW;
	(void)roomH;
	(void)color;
	(void)alpha;
}

void WGPURender_surfaceResize(Renderer* renderer, int32_t surfaceID, int32_t width,
							  int32_t height) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	(void)width;
	(void)height;
}

void WGPURender_surfaceFree(Renderer* renderer, int32_t surfaceID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
}

void WGPURender_surfaceCopy(Renderer* renderer, int32_t destSurfaceID, int32_t destX, int32_t destY,
							int32_t srcSurfaceID, int32_t srcX, int32_t srcY, int32_t srcW,
							int32_t srcH, bool part) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)destSurfaceID;
	(void)destX;
	(void)destY;
	(void)srcSurfaceID;
	(void)srcX;
	(void)srcY;
	(void)srcW;
	(void)srcH;
	(void)part;
}

bool WGPURender_surfaceGetPixels(Renderer* renderer, int32_t surfaceID, uint8_t* outRGBA) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	(void)outRGBA;
	return false;
}

void WGPURender_drawTiledPart(Renderer* renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY,
							  int32_t srcW, int32_t srcH, float dstX, float dstY, float dstW,
							  float dstH, uint32_t color, float alpha) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tpagIndex;
	(void)srcX;
	(void)srcY;
	(void)srcW;
	(void)srcH;
	(void)dstX;
	(void)dstY;
	(void)dstW;
	(void)dstH;
	(void)color;
	(void)alpha;
}

void WGPURender_gpuSetShader(Renderer* renderer, int32_t shaderIndex) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)shaderIndex;
}

void WGPURender_gpuResetShader(Renderer* renderer) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
}

int32_t WGPURender_shaderGetUniform(Renderer* renderer, int32_t shaderIndex, char* uniform) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)shaderIndex;
	(void)uniform;
	return 0;
}

int32_t WGPURender_shaderGetSamplerIndex(Renderer* renderer, int32_t shaderIndex, char* uniform) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)shaderIndex;
	(void)uniform;
	return 0;
}

void WGPURender_shaderSetUniformF(Renderer* renderer, int32_t handle, int32_t count, float value1,
								  float value2, float value3, float value4) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)handle;
	(void)count;
	(void)value1;
	(void)value2;
	(void)value3;
	(void)value4;
}

void WGPURender_shaderSetUniformFArray(Renderer* renderer, int32_t handle, float* values,
									   uint32_t count) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)handle;
	(void)values;
	(void)count;
}

void WGPURender_shaderSetUniformI(Renderer* renderer, int32_t handle, int32_t count, int32_t value1,
								  int32_t value2, int32_t value3, int32_t value4) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)handle;
	(void)count;
	(void)value1;
	(void)value2;
	(void)value3;
	(void)value4;
}

uint32_t WGPURender_spriteGetTexture(Renderer* renderer, int32_t tpagIndex) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)tpagIndex;
	return 0;
}

uint32_t WGPURender_surfaceGetTexture(Renderer* renderer, int32_t surfaceID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)surfaceID;
	return 0;
}

float WGPURender_textureGetTexelWidth(Renderer* renderer, uint32_t texID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)texID;
	return 0.0f;
}

float WGPURender_textureGetTexelHeight(Renderer* renderer, uint32_t texID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)texID;
	return 0.0f;
}

bool WGPURender_textureGetUVs(Renderer* renderer, uint32_t texID, float* outUVs) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)texID;
	(void)outUVs;
	return false;
}

void WGPURender_textureSetStage(Renderer* renderer, int32_t slot, uint32_t texID) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)slot;
	(void)texID;
}

bool WGPURender_shaderIsCompiled(Renderer* renderer, int32_t shader) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)shader;
	return false;
}

bool WGPURender_shadersSupported(void) { return false; }

void WGPURender_setMatrix(Renderer* renderer, int32_t matrixType, Matrix4f matrix) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)matrixType;
	(void)matrix;
}

int32_t WGPURender_videoUploadFrame(Renderer* renderer, int32_t width, int32_t height,
									const uint8_t* rgba) {
	WGPURender* self = (WGPURender*)renderer;
	(void)self;
	(void)width;
	(void)height;
	(void)rgba;
	return 0;
}

Renderer* WGPURender_create(void) {
	WGPURender* wgpu = (WGPURender*)safeCalloc(1, sizeof(WGPURender));

	wgpu->base.vtable = &wgpuVtable;

	wgpuVtable.init = WGPURender_init;
	wgpuVtable.destroy = WGPURender_destroy;

	wgpuVtable.beginFrame = WGPURender_beginFrame;
	wgpuVtable.endFrameInit = WGPURender_endFrameInit;
	wgpuVtable.endFrameEnd = WGPURender_endFrameEnd;

	wgpuVtable.beginView = WGPURender_beginView;
	wgpuVtable.endView = WGPURenderer_endView;
	wgpuVtable.applyProjection = WGPURender_applyProjection;

	wgpuVtable.beginGUI = WGPURender_beginGUI;
	wgpuVtable.setGuiProjection = WGPURender_setGUIProjection;
	wgpuVtable.endGUI = WGPURender_endGUI;

	wgpuVtable.drawSprite = WGPURender_drawSprite;
	wgpuVtable.drawSpritePart = WGPURender_drawSpritePart;
	wgpuVtable.drawSpritePartColor = WGPURender_drawSpritePartColor;
	wgpuVtable.drawSpritePos = WGPURender_drawSpritePos;

	wgpuVtable.drawRectangle = WGPURender_drawRectangle;
	wgpuVtable.drawRectangleColor = WGPURender_drawRectangleColor;

	wgpuVtable.drawLine = WGPURender_drawLine;
	wgpuVtable.drawTriangle = WGPURender_drawTriangle;
	wgpuVtable.drawLineColor = WGPURender_drawLineColor;

	wgpuVtable.drawText = WGPURender_drawText;
	wgpuVtable.drawTextColor = WGPURender_drawTextColor;

	wgpuVtable.flush = WGPURender_flush;
	wgpuVtable.clearScreen = WGPURender_clearScreen;

	wgpuVtable.createSpriteFromSurface = WGPURender_createSpriteFromSurface;
	wgpuVtable.deleteSprite = WGPURender_deleteSprite;

	wgpuVtable.gpuGetBlendFactors = WGPURender_gpuGetBlendFactors;
	wgpuVtable.gpuGetBlendMode = WGPURender_gpuGetBlendMode;
	wgpuVtable.gpuSetBlendMode = WGPURender_gpuSetBlendMode;
	wgpuVtable.gpuSetBlendModeExt = WGPURender_gpuSetBlendModeExt;
	wgpuVtable.gpuSetBlendEnable = WGPURender_gpuSetBlendEnable;

	wgpuVtable.gpuSetAlphaTestEnable = WGPURender_gpuSetAlphaTestEnable;
	wgpuVtable.gpuGetAlphaTestEnable = WGPURender_gpuGetAlphaTestEnable;
	wgpuVtable.gpuSetAlphaTestRef = WGPURender_gpuSetAlphaTestRef;

	wgpuVtable.gpuSetColorWriteEnable = WGPURender_gpuSetColorWriteEnable;
	wgpuVtable.gpuGetColorWriteEnable = WGPURender_gpuGetColorWriteEnable;
	wgpuVtable.gpuGetBlendEnable = WGPURender_gpuGetBlendEnable;

	wgpuVtable.gpuSetFog = WGPURender_gpuSetFog;

	wgpuVtable.drawTile = WGPURender_drawTile;
	wgpuVtable.drawSpriteTiled = WGPURender_drawSpriteTiled;

	wgpuVtable.createSurface = WGPURender_createSurface;
	wgpuVtable.surfaceExists = WGPURender_surfaceExists;
	wgpuVtable.setRenderTarget = WGPURender_setRenderTarget;
	wgpuVtable.ensureApplicationSurface = WGPURender_ensureApplicationSurface;

	wgpuVtable.getSurfaceWidth = WGPURender_getSurfaceWidth;
	wgpuVtable.getSurfaceHeight = WGPURender_getSurfaceHeight;

	wgpuVtable.drawSurface = WGPURender_drawSurface;
	wgpuVtable.drawSurfaceColor = WGPURender_drawSurfaceColor;
	wgpuVtable.drawSurfaceTiled = WGPURender_drawSurfaceTiled;

	wgpuVtable.surfaceResize = WGPURender_surfaceResize;
	wgpuVtable.surfaceFree = WGPURender_surfaceFree;
	wgpuVtable.surfaceCopy = WGPURender_surfaceCopy;
	wgpuVtable.surfaceGetPixels = WGPURender_surfaceGetPixels;

	wgpuVtable.drawTiledPart = WGPURender_drawTiledPart;

	wgpuVtable.gpuSetShader = WGPURender_gpuSetShader;
	wgpuVtable.gpuResetShader = WGPURender_gpuResetShader;
	wgpuVtable.shaderGetUniform = WGPURender_shaderGetUniform;
	wgpuVtable.shaderGetSamplerIndex = WGPURender_shaderGetSamplerIndex;

	wgpuVtable.shaderSetUniformF = WGPURender_shaderSetUniformF;
	wgpuVtable.shaderSetUniformFArray = WGPURender_shaderSetUniformFArray;
	wgpuVtable.shaderSetUniformI = WGPURender_shaderSetUniformI;

	wgpuVtable.spriteGetTexture = WGPURender_spriteGetTexture;
	wgpuVtable.surfaceGetTexture = WGPURender_surfaceGetTexture;

	wgpuVtable.textureGetTexelWidth = WGPURender_textureGetTexelWidth;
	wgpuVtable.textureGetTexelHeight = WGPURender_textureGetTexelHeight;
	wgpuVtable.textureGetUVs = WGPURender_textureGetUVs;
	wgpuVtable.textureSetStage = WGPURender_textureSetStage;

	wgpuVtable.shaderIsCompiled = WGPURender_shaderIsCompiled;
	wgpuVtable.shadersSupported = WGPURender_shadersSupported;

	wgpuVtable.setMatrix = WGPURender_setMatrix;
	wgpuVtable.videoUploadFrame = WGPURender_videoUploadFrame;

	wgpu->base.drawColor = 0xFFFFFF;
	wgpu->base.drawAlpha = 1.0f;
	wgpu->base.drawFont = -1;
	wgpu->base.drawHalign = 0;
	wgpu->base.drawValign = 0;
	wgpu->base.circlePrecision = 24;
	wgpu->base.currentShader = -1;
	wgpu->base.cameraCurrent = 0;

	wgpu->wgpuTextures = (WGPUTexture*)safeCalloc(MAX_TEXTURES, sizeof(WGPUTexture));
	wgpu->wgpuTextureViews = (WGPUTextureView*)safeCalloc(MAX_TEXTURES, sizeof(WGPUTextureView));
	wgpu->textureLoaded = (bool*)safeCalloc(MAX_TEXTURES, sizeof(bool));
	wgpu->textureWidths = (int32_t*)safeCalloc(MAX_TEXTURES, sizeof(int32_t));
	wgpu->textureHeights = (int32_t*)safeCalloc(MAX_TEXTURES, sizeof(int32_t));

	wgpu->batchSpriteCount = 0;

	return (Renderer*)wgpu;
}