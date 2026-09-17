#include <switch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "capture.h"
#include "recorder.h"
#include "config.h"

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;
u32 __nx_fsdev_direntry_cache_size = 1;

#define INNER_HEAP_SIZE (384 * 1024)
static char g_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = g_heap;
    fake_heap_end = g_heap + sizeof(g_heap);
}

#define VIDEO_STACK_SIZE 0x6000
#define AUDIO_STACK_SIZE 0x6000
#define WRITER_STACK_SIZE 0x10000
#define CONTROL_STACK_SIZE 0x3000
#define THREAD_PRIORITY 44
#define WRITER_PRIORITY 45
#define THREAD_CPU 3

typedef struct {
    u64 timestamp_us;
    u32 size;
    u32 generation;
    bool keyframe;
    u8 data[VIDEO_BUFFER_SIZE];
} VideoQueueSlot;

typedef struct {
    u64 timestamp_us;
    u32 size;
    u32 generation;
    u8 data[AUDIO_BUFFER_SIZE];
} AudioQueueSlot;

static Thread g_video_thread;
static Thread g_audio_thread;
static Thread g_writer_thread;
static Thread g_control_thread;
static u8 alignas(0x1000) g_video_stack[VIDEO_STACK_SIZE];
static u8 alignas(0x1000) g_audio_stack[AUDIO_STACK_SIZE];
static u8 alignas(0x1000) g_writer_stack[WRITER_STACK_SIZE];
static u8 alignas(0x1000) g_control_stack[CONTROL_STACK_SIZE];

static VideoQueueSlot alignas(0x1000) g_video_queue[VIDEO_QUEUE_SLOTS];
static AudioQueueSlot alignas(0x1000) g_audio_queue[AUDIO_QUEUE_SLOTS];
static Mutex g_video_q_lock;
static Mutex g_audio_q_lock;
static u32 g_video_q_head = 0, g_video_q_tail = 0, g_video_q_count = 0;
static u32 g_audio_q_head = 0, g_audio_q_tail = 0, g_audio_q_count = 0;
static volatile u32 g_generation = 1;
static volatile bool g_capture_enabled = false;
static volatile bool g_running = true;
static volatile bool g_need_video_keyframe = true;
static bool g_sd_mounted = false;

/* Per-recording diagnostics. Each counter has one main writer thread except max queue use. */
static volatile u64 g_diag_video_captured = 0;
static volatile u64 g_diag_audio_captured = 0;
static volatile u64 g_diag_video_capture_failures = 0;
static volatile u64 g_diag_audio_capture_failures = 0;
static volatile u64 g_diag_video_queue_overflows = 0;
static volatile u64 g_diag_audio_queue_overflows = 0;
static volatile u64 g_diag_video_resync_events = 0;
static volatile u64 g_diag_video_resync_drops = 0;
static volatile u64 g_diag_video_resync_recoveries = 0;
static volatile u32 g_diag_video_max_queue = 0;
static volatile u32 g_diag_audio_max_queue = 0;

static bool control_file_exists(void) {
    struct stat st;
    return stat(CONTROL_FILE, &st) == 0;
}

static void reset_diagnostics(void) {
    g_diag_video_captured = 0;
    g_diag_audio_captured = 0;
    g_diag_video_capture_failures = 0;
    g_diag_audio_capture_failures = 0;
    g_diag_video_queue_overflows = 0;
    g_diag_audio_queue_overflows = 0;
    g_diag_video_resync_events = 0;
    g_diag_video_resync_drops = 0;
    g_diag_video_resync_recoveries = 0;
    g_diag_video_max_queue = 0;
    g_diag_audio_max_queue = 0;
}

static void write_diagnostics(void) {
    (void)mkdir(RECORD_DIR, 0777);
    FILE* f = fopen(DIAGNOSTICS_FILE, "w");
    if (!f) return;

    fprintf(f, "SysDVR-SD diagnostics\n");
    fprintf(f, "version=%s\n", SYSDVR_SD_VERSION);
    fprintf(f, "video_captured=%llu\n", (unsigned long long)g_diag_video_captured);
    fprintf(f, "audio_batches_captured=%llu\n", (unsigned long long)g_diag_audio_captured);
    fprintf(f, "video_capture_failures=%llu\n", (unsigned long long)g_diag_video_capture_failures);
    fprintf(f, "audio_capture_failures=%llu\n", (unsigned long long)g_diag_audio_capture_failures);
    fprintf(f, "video_queue_overflows=%llu\n", (unsigned long long)g_diag_video_queue_overflows);
    fprintf(f, "audio_queue_overflows=%llu\n", (unsigned long long)g_diag_audio_queue_overflows);
    fprintf(f, "video_resync_events=%llu\n", (unsigned long long)g_diag_video_resync_events);
    fprintf(f, "video_resync_predictive_frames_dropped=%llu\n", (unsigned long long)g_diag_video_resync_drops);
    fprintf(f, "video_resync_recoveries=%llu\n", (unsigned long long)g_diag_video_resync_recoveries);
    fprintf(f, "video_max_queue_used=%u/%u\n", (unsigned)g_diag_video_max_queue, (unsigned)VIDEO_QUEUE_SLOTS);
    fprintf(f, "audio_max_queue_used=%u/%u\n", (unsigned)g_diag_audio_max_queue, (unsigned)AUDIO_QUEUE_SLOTS);
    fprintf(f, "\nInterpretation:\n");
    fprintf(f, "- capture_failures: Nintendo grc:d did not provide a usable packet.\n");
    fprintf(f, "- queue_overflows: our SD writer could not drain RAM fast enough.\n");
    fprintf(f, "- resync drops: predictive H.264 frames intentionally discarded after a loss until the next IDR/keyframe.\n");
    fclose(f);
}

static void enter_video_resync(void) {
    if (!g_need_video_keyframe) {
        g_need_video_keyframe = true;
        ++g_diag_video_resync_events;
    }
}

static bool enqueue_video(const VideoFrame* frame, u32 generation) {
    bool ok = false;
    mutexLock(&g_video_q_lock);
    if (g_video_q_count < VIDEO_QUEUE_SLOTS) {
        VideoQueueSlot* slot = &g_video_queue[g_video_q_head];
        slot->timestamp_us = frame->timestamp_us;
        slot->size = frame->size;
        slot->generation = generation;
        slot->keyframe = frame->keyframe;
        memcpy(slot->data, frame->data, frame->size);
        g_video_q_head = (g_video_q_head + 1u) % VIDEO_QUEUE_SLOTS;
        ++g_video_q_count;
        if (g_video_q_count > g_diag_video_max_queue) g_diag_video_max_queue = g_video_q_count;
        ok = true;
    }
    mutexUnlock(&g_video_q_lock);
    return ok;
}

static bool enqueue_audio(const AudioFrame* frame, u32 generation) {
    bool ok = false;
    mutexLock(&g_audio_q_lock);
    if (g_audio_q_count < AUDIO_QUEUE_SLOTS) {
        AudioQueueSlot* slot = &g_audio_queue[g_audio_q_head];
        slot->timestamp_us = frame->timestamp_us;
        slot->size = frame->size;
        slot->generation = generation;
        memcpy(slot->data, frame->data, frame->size);
        g_audio_q_head = (g_audio_q_head + 1u) % AUDIO_QUEUE_SLOTS;
        ++g_audio_q_count;
        if (g_audio_q_count > g_diag_audio_max_queue) g_diag_audio_max_queue = g_audio_q_count;
        ok = true;
    }
    mutexUnlock(&g_audio_q_lock);
    return ok;
}

static bool video_tail_info(u64* ts, u32* generation) {
    bool have = false;
    mutexLock(&g_video_q_lock);
    if (g_video_q_count) {
        *ts = g_video_queue[g_video_q_tail].timestamp_us;
        *generation = g_video_queue[g_video_q_tail].generation;
        have = true;
    }
    mutexUnlock(&g_video_q_lock);
    return have;
}

static bool audio_tail_info(u64* ts, u32* generation) {
    bool have = false;
    mutexLock(&g_audio_q_lock);
    if (g_audio_q_count) {
        *ts = g_audio_queue[g_audio_q_tail].timestamp_us;
        *generation = g_audio_queue[g_audio_q_tail].generation;
        have = true;
    }
    mutexUnlock(&g_audio_q_lock);
    return have;
}

static void consume_video(void) {
    VideoQueueSlot* slot;
    mutexLock(&g_video_q_lock);
    if (!g_video_q_count) {
        mutexUnlock(&g_video_q_lock);
        return;
    }
    slot = &g_video_queue[g_video_q_tail];
    mutexUnlock(&g_video_q_lock);

    if (slot->generation == g_generation) {
        VideoFrame frame = {slot->data, slot->size, slot->timestamp_us, slot->keyframe};
        recorderPushVideo(&frame);
    }

    mutexLock(&g_video_q_lock);
    g_video_q_tail = (g_video_q_tail + 1u) % VIDEO_QUEUE_SLOTS;
    --g_video_q_count;
    mutexUnlock(&g_video_q_lock);
}

static void consume_audio(void) {
    AudioQueueSlot* slot;
    mutexLock(&g_audio_q_lock);
    if (!g_audio_q_count) {
        mutexUnlock(&g_audio_q_lock);
        return;
    }
    slot = &g_audio_queue[g_audio_q_tail];
    mutexUnlock(&g_audio_q_lock);

    if (slot->generation == g_generation) {
        AudioFrame frame = {slot->data, slot->size, slot->timestamp_us};
        recorderPushAudio(&frame);
    }

    mutexLock(&g_audio_q_lock);
    g_audio_q_tail = (g_audio_q_tail + 1u) % AUDIO_QUEUE_SLOTS;
    --g_audio_q_count;
    mutexUnlock(&g_audio_q_lock);
}

static void video_thread(void* arg) {
    (void)arg;
    VideoFrame frame;
    while (g_running) {
        if (captureReadVideo(&frame)) {
            if (!g_capture_enabled) continue;
            ++g_diag_video_captured;

            /* After any packet loss, never feed dependent H.264 pictures to the file.
               Wait for an IDR so the decoder resumes from a clean reference picture. */
            if (g_need_video_keyframe) {
                if (!frame.keyframe) {
                    ++g_diag_video_resync_drops;
                    continue;
                }

                if (enqueue_video(&frame, g_generation)) {
                    g_need_video_keyframe = false;
                    ++g_diag_video_resync_recoveries;
                } else {
                    ++g_diag_video_queue_overflows;
                    /* We lost the recovery keyframe too; keep waiting for another. */
                }
                continue;
            }

            if (!enqueue_video(&frame, g_generation)) {
                ++g_diag_video_queue_overflows;
                enter_video_resync();
            }
        } else {
            if (g_capture_enabled) {
                ++g_diag_video_capture_failures;
                enter_video_resync();
            }
            svcSleepThread(2 * 1000 * 1000LL);
        }
    }
}

static void audio_thread(void* arg) {
    (void)arg;
    AudioFrame frame;
    while (g_running) {
        if (captureReadAudio(&frame)) {
            if (!g_capture_enabled) continue;
            ++g_diag_audio_captured;
            if (!enqueue_audio(&frame, g_generation)) ++g_diag_audio_queue_overflows;
        } else {
            if (g_capture_enabled) ++g_diag_audio_capture_failures;
            svcSleepThread(2 * 1000 * 1000LL);
        }
    }
}

static void writer_thread(void* arg) {
    (void)arg;
    while (g_running) {
        u64 vts = 0, ats = 0;
        u32 vgen = 0, agen = 0;
        bool hv = video_tail_info(&vts, &vgen);
        bool ha = audio_tail_info(&ats, &agen);
        (void)vgen;
        (void)agen;

        if (hv && ha) {
            if (vts <= ats) consume_video();
            else consume_audio();
        } else if (hv) {
            consume_video();
        } else if (ha) {
            consume_audio();
        } else {
            svcSleepThread(1 * 1000 * 1000LL);
        }
    }

    for (;;) {
        u64 vts = 0, ats = 0;
        u32 vgen = 0, agen = 0;
        bool hv = video_tail_info(&vts, &vgen);
        bool ha = audio_tail_info(&ats, &agen);
        if (!hv && !ha) break;
        if (hv && (!ha || vts <= ats)) consume_video();
        else consume_audio();
    }
}

static void control_thread(void* arg) {
    (void)arg;
    bool first = true;
    bool last = false;
    while (g_running) {
        bool enabled = control_file_exists();
        if (first || enabled != last) {
            g_capture_enabled = false;
            ++g_generation;

            if (enabled) {
                reset_diagnostics();
                g_need_video_keyframe = true;
                recorderSetEnabled(true);
                g_capture_enabled = true;
            } else {
                recorderSetEnabled(false);
                if (!first && last) write_diagnostics();
                g_need_video_keyframe = true;
            }

            last = enabled;
            first = false;
        }
        svcSleepThread(100LL * 1000 * 1000);
    }
}

void __appInit(void) {
    svcSleepThread(20LL * 1000 * 1000 * 1000);
    Result rc = smInitialize();
    if (R_FAILED(rc)) fatalThrow(rc);
    rc = fsInitialize();
    if (R_FAILED(rc)) fatalThrow(rc);
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) fatalThrow(rc);
    g_sd_mounted = true;
}

void __appExit(void) {
    g_running = false;
    recorderExit();
    captureExit();
    if (g_sd_mounted) fsdevUnmountDevice("sdmc");
    fsExit();
    smExit();
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    mutexInit(&g_video_q_lock);
    mutexInit(&g_audio_q_lock);
    if (!recorderInitialize()) return 1;
    Result rc = captureInitialize();
    if (R_FAILED(rc)) return 2;

    rc = threadCreate(&g_video_thread, video_thread, NULL, g_video_stack, sizeof(g_video_stack), THREAD_PRIORITY, THREAD_CPU);
    if (R_FAILED(rc)) return 3;
    rc = threadCreate(&g_audio_thread, audio_thread, NULL, g_audio_stack, sizeof(g_audio_stack), THREAD_PRIORITY, THREAD_CPU);
    if (R_FAILED(rc)) {
        threadClose(&g_video_thread);
        return 4;
    }
    rc = threadCreate(&g_writer_thread, writer_thread, NULL, g_writer_stack, sizeof(g_writer_stack), WRITER_PRIORITY, THREAD_CPU);
    if (R_FAILED(rc)) {
        threadClose(&g_audio_thread);
        threadClose(&g_video_thread);
        return 5;
    }
    rc = threadCreate(&g_control_thread, control_thread, NULL, g_control_stack, sizeof(g_control_stack), THREAD_PRIORITY, THREAD_CPU);
    if (R_FAILED(rc)) {
        threadClose(&g_writer_thread);
        threadClose(&g_audio_thread);
        threadClose(&g_video_thread);
        return 6;
    }

    threadStart(&g_video_thread);
    threadStart(&g_audio_thread);
    threadStart(&g_writer_thread);
    threadStart(&g_control_thread);

    while (g_running) svcSleepThread(1000LL * 1000 * 1000);

    threadWaitForExit(&g_control_thread);
    threadWaitForExit(&g_video_thread);
    threadWaitForExit(&g_audio_thread);
    threadWaitForExit(&g_writer_thread);
    threadClose(&g_control_thread);
    threadClose(&g_writer_thread);
    threadClose(&g_audio_thread);
    threadClose(&g_video_thread);
    return 0;
}
