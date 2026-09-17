#include <switch.h>
#include <stdbool.h>
#include <stddef.h>
#include "capture.h"
#include "grcd.h"
#include "config.h"

#define GRCD_NOT_INITIALIZED 0x3E8D4

static u8 alignas(0x1000) g_video[VIDEO_BUFFER_SIZE];
static u8 alignas(0x1000) g_audio[AUDIO_BUFFER_SIZE];
static Service g_video_service;
static Service g_audio_service;
static Mutex g_begin_mutex;
static bool g_begin_attempted = false;

static bool ensure_begin(Result* rc) {
    if (g_begin_attempted) return false;
    mutexLock(&g_begin_mutex);
    if (g_begin_attempted) {
        mutexUnlock(&g_begin_mutex);
        return false;
    }
    g_begin_attempted = true;
    *rc = sdgrcdBegin(&g_video_service);
    mutexUnlock(&g_begin_mutex);
    return true;
}

static size_t start_code_len(const u8* p, size_t n) {
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    return 0;
}

static bool contains_idr(const u8* data, size_t size) {
    size_t i = 0;
    bool saw_start_code = false;
    while (i < size) {
        size_t sc = start_code_len(data + i, size - i);
        if (sc) {
            saw_start_code = true;
            i += sc;
            if (i < size && (data[i] & 0x1f) == 5) return true;
            continue;
        }
        ++i;
    }
    return !saw_start_code && size > 0 && ((data[0] & 0x1f) == 5);
}

Result captureInitialize(void) {
    mutexInit(&g_begin_mutex);
    Result rc = sdgrcdOpen(&g_video_service);
    if (R_FAILED(rc)) return rc;
    rc = sdgrcdOpen(&g_audio_service);
    if (R_FAILED(rc)) {
        sdgrcdClose(&g_video_service);
        return rc;
    }
    return 0;
}

void captureExit(void) {
    sdgrcdClose(&g_audio_service);
    sdgrcdClose(&g_video_service);
}

bool captureReadVideo(VideoFrame* out) {
    if (!out) return false;
    u32 data_size = 0;
    u64 ts = 0;
    Result rc = sdgrcdTransfer(&g_video_service, GrcStream_Video,
                             g_video, sizeof(g_video), NULL, &data_size, &ts);
    if (rc == GRCD_NOT_INITIALIZED && ensure_begin(&rc)) {
        if (R_FAILED(rc)) return false;
        return captureReadVideo(out);
    }
    if (R_FAILED(rc) || data_size == 0 || data_size > sizeof(g_video)) return false;
    out->data = g_video;
    out->size = data_size;
    out->timestamp_us = ts;
    out->keyframe = contains_idr(g_video, data_size);
    return true;
}

bool captureReadAudio(AudioFrame* out) {
    if (!out) return false;
    u32 data_size = 0;
    u64 ts = 0;
    Result rc = sdgrcdTransfer(&g_audio_service, GrcStream_Audio,
                             g_audio, AUDIO_CHUNK_SIZE, NULL, &data_size, &ts);
    if (rc == GRCD_NOT_INITIALIZED && ensure_begin(&rc)) {
        if (R_FAILED(rc)) return false;
        return captureReadAudio(out);
    }
    if (R_FAILED(rc) || data_size == 0 || data_size > AUDIO_CHUNK_SIZE) return false;

    u32 total = data_size;
    for (u32 i = 1; i < AUDIO_BATCH_CHUNKS; ++i) {
        u32 chunk_size = 0;
        rc = sdgrcdTransfer(&g_audio_service, GrcStream_Audio,
                            g_audio + (i * AUDIO_CHUNK_SIZE), AUDIO_CHUNK_SIZE,
                            NULL, &chunk_size, NULL);
        if (R_FAILED(rc) || chunk_size == 0 || chunk_size > AUDIO_CHUNK_SIZE) break;
        total += chunk_size;
    }

    out->data = g_audio;
    out->size = total;
    out->timestamp_us = ts;
    return true;
}
