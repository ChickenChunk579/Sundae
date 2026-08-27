#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <pthread.h>
#include <unistd.h>
#include "video.h"
#include "runner.h"

#define THREAD_RETURN void*
#define THREAD_HANDLE pthread_t
typedef pthread_mutex_t Mutex;
typedef pthread_cond_t CondVar;

typedef enum {
    VIDEO_STATUS_CLOSED   = 0,
    VIDEO_STATUS_PREPARING = 1,
    VIDEO_STATUS_PAUSED   = 2,
    VIDEO_STATUS_PLAYING  = 3
} VideoStatus;

#define MAX_QUEUE_SIZE 128
typedef struct {
    AVPacket* packets[MAX_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    Mutex mutex;
    CondVar cond;
} PacketQueue;

typedef struct {
    AVFormatContext* formatCtx;
    AVCodecContext* codecCtx;
    int videoStreamIdx;

    volatile VideoStatus status;
    volatile bool loopEnabled;
    volatile double volume;

    int64_t startTimeUs;
    volatile double currentPosSec;
    double durationSec;

    THREAD_HANDLE demuxThread;
    THREAD_HANDLE decodeThread;
    volatile bool stopThreads;
    PacketQueue pktQueue;
    Runner* runner;
    bool startEventSent;
    bool endEventSent;
    volatile bool demuxEnded;
    Mutex frameMutex;
    uint8_t* latestPixels;
    int32_t latestWidth;
    int32_t latestHeight;
    struct SwsContext* swsCtx;
    bool frameMutexInitialized;
    bool decodedFrameLogged;
    bool videoDrawLogged;
} VideoPlayerState;

static VideoPlayerState g_VideoPlayer = {
    .formatCtx = NULL,
    .codecCtx = NULL,
    .videoStreamIdx = -1,
    .status = VIDEO_STATUS_CLOSED,
    .loopEnabled = false,
    .volume = 1.0
};

// --- Threading Helpers ---
static void mutex_init(Mutex* m) {
#ifdef _WIN32
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}
static void mutex_lock(Mutex* m) {
#ifdef _WIN32
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}
static void mutex_unlock(Mutex* m) {
#ifdef _WIN32
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}
static void mutex_destroy(Mutex* m) {
#ifdef _WIN32
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}
static void cond_init(CondVar* c) {
#ifdef _WIN32
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif
}
static void cond_wait(CondVar* c, Mutex* m) {
#ifdef _WIN32
    SleepConditionVariableCS(c, m, INFINITE);
#else
    pthread_cond_wait(c, m);
#endif
}
static void cond_signal(CondVar* c) {
#ifdef _WIN32
    WakeConditionVariable(c);
#else
    pthread_cond_signal(c);
#endif
}
static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

// --- Queue Functions ---
static void queue_init(PacketQueue* q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    mutex_init(&q->mutex);
    cond_init(&q->cond);
}

static bool queue_push(PacketQueue* q, AVPacket* pkt) {
    mutex_lock(&q->mutex);
    if (q->count >= MAX_QUEUE_SIZE) {
        mutex_unlock(&q->mutex);
        return false;
    }
    q->packets[q->tail] = av_packet_clone(pkt);
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;
    q->count++;
    cond_signal(&q->cond);
    mutex_unlock(&q->mutex);
    return true;
}

static AVPacket* queue_pop(PacketQueue* q) {
    mutex_lock(&q->mutex);
    while (q->count == 0 && !g_VideoPlayer.stopThreads && !g_VideoPlayer.demuxEnded) {
        cond_wait(&q->cond, &q->mutex);
    }
    if (g_VideoPlayer.stopThreads || (q->count == 0 && g_VideoPlayer.demuxEnded)) {
        mutex_unlock(&q->mutex);
        return NULL;
    }
    AVPacket* pkt = q->packets[q->head];
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;
    q->count--;
    mutex_unlock(&q->mutex);
    return pkt;
}

static void queue_clear(PacketQueue* q) {
    mutex_lock(&q->mutex);
    while (q->count > 0) {
        AVPacket* pkt = q->packets[q->head];
        av_packet_free(&pkt);
        q->head = (q->head + 1) % MAX_QUEUE_SIZE;
        q->count--;
    }
    q->head = 0;
    q->tail = 0;
    cond_signal(&q->cond);
    mutex_unlock(&q->mutex);
}

// --- Background Threads ---
static THREAD_RETURN demux_loop(void* arg) {
    AVPacket* packet = av_packet_alloc();
    while (!g_VideoPlayer.stopThreads) {
        if (g_VideoPlayer.status == VIDEO_STATUS_PAUSED) {
            sleep_ms(10);
            continue;
        }

        if (g_VideoPlayer.pktQueue.count >= MAX_QUEUE_SIZE - 2) {
            sleep_ms(10);
            continue;
        }

        int ret = av_read_frame(g_VideoPlayer.formatCtx, packet);
        if (ret >= 0) {
            if (packet->stream_index == g_VideoPlayer.videoStreamIdx) {
                if (!queue_push(&g_VideoPlayer.pktQueue, packet)) {
                    sleep_ms(5);
                }
            }
            av_packet_unref(packet);
        } else {
            g_VideoPlayer.demuxEnded = true;
            cond_signal(&g_VideoPlayer.pktQueue.cond);
            break;
        }
    }
    av_packet_free(&packet);
    return 0;
}

static THREAD_RETURN decode_loop(void* arg) {
    AVFrame* frame = av_frame_alloc();
    while (!g_VideoPlayer.stopThreads) {
        if (g_VideoPlayer.status == VIDEO_STATUS_PAUSED) {
            sleep_ms(10);
            continue;
        }

        AVPacket* pkt = queue_pop(&g_VideoPlayer.pktQueue);
        if (!pkt) {
            if (g_VideoPlayer.stopThreads) break;
            g_VideoPlayer.status = VIDEO_STATUS_CLOSED;
            if (!g_VideoPlayer.endEventSent) {
                g_VideoPlayer.endEventSent = true;
                Runner_queueVideoEvent(g_VideoPlayer.runner, "video_end");
                logInfo("video ended\n");
            }
            break;
        }

        if (avcodec_send_packet(g_VideoPlayer.codecCtx, pkt) >= 0) {
            while (avcodec_receive_frame(g_VideoPlayer.codecCtx, frame) >= 0) {
                double pts = frame->best_effort_timestamp * av_q2d(g_VideoPlayer.formatCtx->streams[g_VideoPlayer.videoStreamIdx]->time_base);
                g_VideoPlayer.currentPosSec = pts;

                int64_t frameTimeUs = g_VideoPlayer.startTimeUs + (int64_t)(pts * 1000000.0);
                int64_t nowUs = av_gettime_relative();
                if (frameTimeUs > nowUs) {
#ifdef _WIN32
                    Sleep((DWORD)((frameTimeUs - nowUs) / 1000));
#else
                    usleep(frameTimeUs - nowUs);
#endif
                }

                int32_t width = g_VideoPlayer.codecCtx->width;
                int32_t height = g_VideoPlayer.codecCtx->height;
                uint8_t* pixels = (uint8_t*)safeMalloc((size_t)width * (size_t)height * 4);
                uint8_t* dstData[4] = {pixels, NULL, NULL, NULL};
                int dstLinesize[4] = {width * 4, 0, 0, 0};
                g_VideoPlayer.swsCtx = sws_getCachedContext(
                    g_VideoPlayer.swsCtx, width, height, g_VideoPlayer.codecCtx->pix_fmt,
                    width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                if (g_VideoPlayer.swsCtx == NULL || sws_scale(g_VideoPlayer.swsCtx,
                        (const uint8_t* const*)frame->data, frame->linesize, 0, height,
                        dstData, dstLinesize) <= 0) {
                    free(pixels);
                    pixels = NULL;
                }
                if (pixels != NULL) {
                    mutex_lock(&g_VideoPlayer.frameMutex);
                    free(g_VideoPlayer.latestPixels);
                    g_VideoPlayer.latestPixels = pixels;
                    g_VideoPlayer.latestWidth = width;
                    g_VideoPlayer.latestHeight = height;
                    mutex_unlock(&g_VideoPlayer.frameMutex);
                    if (!g_VideoPlayer.decodedFrameLogged) {
                        logDebug("Video: first RGBA frame decoded (%dx%d)\n", width, height);
                        g_VideoPlayer.decodedFrameLogged = true;
                    }
                }

                // Decode completed successfully. frame pixel data sits here.
                
                if (!g_VideoPlayer.startEventSent) {
                    g_VideoPlayer.startEventSent = true;
                    g_VideoPlayer.status = VIDEO_STATUS_PLAYING;

                    Runner_queueVideoEvent(
                        g_VideoPlayer.runner,
                        "video_start"
                    );

                    logInfo("video start event sent\n");
                }

                av_frame_unref(frame);
            }
        }
        av_packet_free(&pkt);
    }
    av_frame_free(&frame);
    return 0;
}

static void internal_video_close() {
    g_VideoPlayer.stopThreads = true;
    cond_signal(&g_VideoPlayer.pktQueue.cond); // Unblock pop loops

#ifdef _WIN32
    if (g_VideoPlayer.demuxThread) {
        WaitForSingleObject(g_VideoPlayer.demuxThread, INFINITE);
        CloseHandle(g_VideoPlayer.demuxThread);
        g_VideoPlayer.demuxThread = NULL;
    }
    if (g_VideoPlayer.decodeThread) {
        WaitForSingleObject(g_VideoPlayer.decodeThread, INFINITE);
        CloseHandle(g_VideoPlayer.decodeThread);
        g_VideoPlayer.decodeThread = NULL;
    }
#else
    if (g_VideoPlayer.demuxThread) {
        pthread_join(g_VideoPlayer.demuxThread, NULL);
        g_VideoPlayer.demuxThread = 0;
    }
    if (g_VideoPlayer.decodeThread) {
        pthread_join(g_VideoPlayer.decodeThread, NULL);
        g_VideoPlayer.decodeThread = 0;
    }
#endif

    queue_clear(&g_VideoPlayer.pktQueue);

    if (g_VideoPlayer.codecCtx) avcodec_free_context(&g_VideoPlayer.codecCtx);
    if (g_VideoPlayer.formatCtx) avformat_close_input(&g_VideoPlayer.formatCtx);
    if (g_VideoPlayer.swsCtx) sws_freeContext(g_VideoPlayer.swsCtx);
    g_VideoPlayer.swsCtx = NULL;

    mutex_lock(&g_VideoPlayer.frameMutex);
    free(g_VideoPlayer.latestPixels);
    g_VideoPlayer.latestPixels = NULL;
    g_VideoPlayer.latestWidth = 0;
    g_VideoPlayer.latestHeight = 0;
    mutex_unlock(&g_VideoPlayer.frameMutex);

    g_VideoPlayer.status = VIDEO_STATUS_CLOSED;
    g_VideoPlayer.videoStreamIdx = -1;
}

RValue video_open(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 1) return RValue_makeUndefined();

    g_VideoPlayer.startEventSent = false;
    g_VideoPlayer.endEventSent = false;
    
    g_VideoPlayer.runner = ctx->runner;
    
    char* filename = RValue_toString(args[0]);
    FileSystem* fs = ctx->runner->fileSystem;
    char* resolvedPath = fs->vtable->resolvePath(fs, filename);
    free(filename);

    if (g_VideoPlayer.status != VIDEO_STATUS_CLOSED) {
        internal_video_close();
    }

    g_VideoPlayer.formatCtx = avformat_alloc_context();
    if (avformat_open_input(&g_VideoPlayer.formatCtx, resolvedPath, NULL, NULL) < 0) {
        logError("avformat_open_input(%s) failed\n", resolvedPath);
        free(resolvedPath);
        return RValue_makeUndefined();
    }
    
    free(resolvedPath);

    if (avformat_find_stream_info(g_VideoPlayer.formatCtx, NULL) < 0) {
        internal_video_close();
        logError("avformat_find_stream_info failed\n");
        return RValue_makeUndefined();
    }

    const AVCodec* codec = NULL;
    g_VideoPlayer.videoStreamIdx = av_find_best_stream(g_VideoPlayer.formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (g_VideoPlayer.videoStreamIdx < 0) {
        internal_video_close();
        logError("videoStreamIdx < 0\n");
        return RValue_makeUndefined();
    }

    g_VideoPlayer.codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(g_VideoPlayer.codecCtx, g_VideoPlayer.formatCtx->streams[g_VideoPlayer.videoStreamIdx]->codecpar);

    if (avcodec_open2(g_VideoPlayer.codecCtx, codec, NULL) < 0) {
        internal_video_close();
        logError("avcodec_open2 failed\n");
        return RValue_makeUndefined();
    }

    if (g_VideoPlayer.formatCtx->duration != AV_NOPTS_VALUE) {
        g_VideoPlayer.durationSec = (double)g_VideoPlayer.formatCtx->duration / AV_TIME_BASE;
    }

    queue_init(&g_VideoPlayer.pktQueue);
    if (!g_VideoPlayer.frameMutexInitialized) {
        mutex_init(&g_VideoPlayer.frameMutex);
        g_VideoPlayer.frameMutexInitialized = true;
    }
    g_VideoPlayer.stopThreads = false;
    g_VideoPlayer.demuxEnded = false;
    g_VideoPlayer.startTimeUs = av_gettime_relative();
    g_VideoPlayer.status = VIDEO_STATUS_PLAYING;

#ifdef _WIN32
    g_VideoPlayer.demuxThread = CreateThread(NULL, 0, demux_loop, NULL, 0, NULL);
    g_VideoPlayer.decodeThread = CreateThread(NULL, 0, decode_loop, NULL, 0, NULL);
#else
    pthread_create(&g_VideoPlayer.demuxThread, NULL, demux_loop, NULL);
    pthread_create(&g_VideoPlayer.decodeThread, NULL, decode_loop, NULL);
#endif

    logInfo("video open ok");

    return RValue_makeUndefined();
}

RValue video_close(VMContext* ctx, RValue* args, int32_t argCount) {
    (void)ctx;
    (void)args;
    (void)argCount;
    logInfo("video_close called");
    if (g_VideoPlayer.formatCtx != NULL || g_VideoPlayer.codecCtx != NULL)
        internal_video_close();
    return RValue_makeUndefined();
}

RValue video_resume(VMContext* ctx, RValue* args, int32_t argCount) {
    if (g_VideoPlayer.status == VIDEO_STATUS_PAUSED) {
        double currentPos = g_VideoPlayer.currentPosSec;
        // Shift the anchor time so playback resumes exactly where it left off
        g_VideoPlayer.startTimeUs = av_gettime_relative() - (int64_t)(currentPos * 1000000.0);
        g_VideoPlayer.status = VIDEO_STATUS_PLAYING;
    }
    return RValue_makeUndefined();
}

RValue video_seek_to(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 1 || g_VideoPlayer.status == VIDEO_STATUS_CLOSED) return RValue_makeUndefined();

    double targetMs = RValue_toReal(args[0]);
    double targetSec = targetMs / 1000.0;

    int64_t ts = (int64_t)(targetSec / av_q2d(g_VideoPlayer.formatCtx->streams[g_VideoPlayer.videoStreamIdx]->time_base));

    // Clear old packet streams to prevent rendering outdated frames
    queue_clear(&g_VideoPlayer.pktQueue);
    avcodec_flush_buffers(g_VideoPlayer.codecCtx);

    av_seek_frame(g_VideoPlayer.formatCtx, g_VideoPlayer.videoStreamIdx, ts, AVSEEK_FLAG_BACKWARD);

    // Update timestamps instantly to reflect changes in UI properties before the next frame decodes
    g_VideoPlayer.currentPosSec = targetSec;
    g_VideoPlayer.startTimeUs = av_gettime_relative() - (int64_t)(targetSec * 1000000.0);

    return RValue_makeUndefined();
}

RValue video_draw(VMContext* ctx, RValue* args, int32_t argCount) {
    (void)args;
    (void)argCount;
    static bool videoDrawCallLogged;
    mutex_lock(&g_VideoPlayer.frameMutex);
    bool hasFrame = g_VideoPlayer.latestPixels != NULL;
    int32_t frameWidth = g_VideoPlayer.latestWidth;
    int32_t frameHeight = g_VideoPlayer.latestHeight;
    mutex_unlock(&g_VideoPlayer.frameMutex);
    if (!videoDrawCallLogged) {
        logDebug("Video: video_draw called status=%d frame=%s (%dx%d) uploadHook=%s\n",
                 g_VideoPlayer.status, hasFrame ? "yes" : "no", frameWidth, frameHeight,
                 ctx->runner->renderer != NULL && ctx->runner->renderer->vtable->videoUploadFrame != NULL ? "yes" : "no");
        videoDrawCallLogged = true;
    }
    if (g_VideoPlayer.status == VIDEO_STATUS_CLOSED && !hasFrame) {
        GMLArray* arr = GMLArray_create(ctx->dataWin->gen8.wadVersion, 0);
        GMLArray_add(arr, RValue_makeReal(-2.0));
        return RValue_makeArray(arr);
    }
    GMLArray* arr = GMLArray_create(ctx->dataWin->gen8.wadVersion, 0);
    int32_t surfaceId = -1;
    mutex_lock(&g_VideoPlayer.frameMutex);
    if (g_VideoPlayer.latestPixels != NULL && ctx->runner->renderer != NULL &&
        ctx->runner->renderer->vtable->videoUploadFrame != NULL) {
        surfaceId = ctx->runner->renderer->vtable->videoUploadFrame(
            ctx->runner->renderer, g_VideoPlayer.latestWidth, g_VideoPlayer.latestHeight,
            g_VideoPlayer.latestPixels);
        if (!g_VideoPlayer.videoDrawLogged) {
            logDebug("Video: video_draw uploaded frame (%dx%d) to surface %d\n",
                     g_VideoPlayer.latestWidth, g_VideoPlayer.latestHeight, surfaceId);
            g_VideoPlayer.videoDrawLogged = true;
        }
    }
    mutex_unlock(&g_VideoPlayer.frameMutex);
    GMLArray_add(arr, RValue_makeReal(surfaceId != -1 ? 0.0 : -1.0));
    GMLArray_add(arr, RValue_makeReal((double)surfaceId));
    return RValue_makeArray(arr);
}

RValue video_get_status(VMContext* ctx, RValue* args, int32_t argCount) {
    return RValue_makeInt32((int32_t)g_VideoPlayer.status);
}

RValue video_get_format(VMContext* ctx, RValue* args, int32_t argCount) {
    return RValue_makeInt32(0); // 0 = RGBA color space fallback
}

RValue video_get_duration(VMContext* ctx, RValue* args, int32_t argCount) {
    return RValue_makeReal(g_VideoPlayer.durationSec * 1000.0); // Converted to milliseconds
}

RValue video_get_position(VMContext* ctx, RValue* args, int32_t argCount) {
    return RValue_makeReal(g_VideoPlayer.currentPosSec * 1000.0); // Converted to milliseconds
}

RValue video_get_volume(VMContext* ctx, RValue* args, int32_t argCount) {
    return RValue_makeReal(g_VideoPlayer.volume);
}

RValue video_set_volume(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount >= 1) {
        g_VideoPlayer.volume = RValue_toReal(args[0]);
    }
    return RValue_makeUndefined();
}

RValue video_enable_loop(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount >= 1) {
        g_VideoPlayer.loopEnabled = RValue_toBool(args[0]);
    }
    return RValue_makeUndefined();
}

RValue video_pause(VMContext* ctx, RValue* args, int32_t argCount) {
    if (g_VideoPlayer.status == VIDEO_STATUS_PLAYING) {
        g_VideoPlayer.status = VIDEO_STATUS_PAUSED;
    }

    return RValue_makeUndefined();
}