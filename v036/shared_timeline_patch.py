from pathlib import Path

p = Path('work/source/main.c')
s = p.read_text()

# Add shared timeline state. One offset is used for BOTH audio and video.
s = s.replace(
    'static volatile bool g_need_video_keyframe = true;\nstatic bool g_sd_mounted = false;',
    '''static volatile bool g_need_video_keyframe = true;\nstatic bool g_sd_mounted = false;\n\n/* Shared A/V timeline. Video is authoritative across HOME/system-UI gaps.\n   While video waits for a clean game IDR, audio is discarded too. When the\n   IDR arrives, both streams resume from one common timestamp offset. */\nstatic Mutex g_timeline_lock;\nstatic bool g_timeline_started = false;\nstatic volatile bool g_timeline_rebase_pending = true;\nstatic u64 g_timeline_offset_us = 0;\nstatic u64 g_resume_raw_ts = 0;\nstatic u64 g_last_video_raw_ts = 0;\nstatic u64 g_last_audio_raw_ts = 0;\nstatic u64 g_last_video_out_ts = 0;\nstatic u64 g_last_audio_out_end_ts = 0;\nstatic volatile u64 g_diag_video_timeline_gaps = 0;\nstatic volatile u64 g_diag_audio_timeline_gaps = 0;\nstatic volatile u64 g_diag_timeline_rebases = 0;\nstatic volatile u64 g_diag_timeline_gap_us_removed = 0;\nstatic volatile u64 g_diag_home_placeholder_keyframes = 0;\nstatic volatile u64 g_diag_audio_held_for_video_resync = 0;'''
)

# Reset timeline state with diagnostics.
s = s.replace(
    '    g_diag_audio_max_queue = 0;\n}',
    '''    g_diag_audio_max_queue = 0;\n    g_diag_video_timeline_gaps = 0;\n    g_diag_audio_timeline_gaps = 0;\n    g_diag_timeline_rebases = 0;\n    g_diag_timeline_gap_us_removed = 0;\n    g_diag_home_placeholder_keyframes = 0;\n    g_diag_audio_held_for_video_resync = 0;\n    g_timeline_started = false;\n    g_timeline_rebase_pending = true;\n    g_timeline_offset_us = 0;\n    g_resume_raw_ts = 0;\n    g_last_video_raw_ts = 0;\n    g_last_audio_raw_ts = 0;\n    g_last_video_out_ts = 0;\n    g_last_audio_out_end_ts = 0;\n}'''
)

# Add useful counters to diagnostics.
s = s.replace(
    '    fprintf(f, "audio_max_queue_used=%u/%u\\n", (unsigned)g_diag_audio_max_queue, (unsigned)AUDIO_QUEUE_SLOTS);',
    '''    fprintf(f, "audio_max_queue_used=%u/%u\\n", (unsigned)g_diag_audio_max_queue, (unsigned)AUDIO_QUEUE_SLOTS);\n    fprintf(f, "video_timeline_gaps=%llu\\n", (unsigned long long)g_diag_video_timeline_gaps);\n    fprintf(f, "audio_timeline_gaps=%llu\\n", (unsigned long long)g_diag_audio_timeline_gaps);\n    fprintf(f, "timeline_rebases=%llu\\n", (unsigned long long)g_diag_timeline_rebases);\n    fprintf(f, "timeline_gap_ms_removed=%llu\\n", (unsigned long long)(g_diag_timeline_gap_us_removed / 1000ULL));\n    fprintf(f, "home_placeholder_keyframes=%llu\\n", (unsigned long long)g_diag_home_placeholder_keyframes);\n    fprintf(f, "audio_batches_held_for_video_resync=%llu\\n", (unsigned long long)g_diag_audio_held_for_video_resync);'''
)

# A resync means the next real game IDR will establish a fresh shared A/V base.
s = s.replace(
'''static void enter_video_resync(void) {\n    if (!g_need_video_keyframe) {\n        g_need_video_keyframe = true;\n        ++g_diag_video_resync_events;\n    }\n}''',
'''static void enter_video_resync(void) {\n    if (!g_need_video_keyframe) {\n        g_need_video_keyframe = true;\n        g_timeline_rebase_pending = true;\n        ++g_diag_video_resync_events;\n    }\n}'''
)

start = s.index('static void video_thread(void* arg) {')
end = s.index('static void writer_thread(void* arg) {')
replacement = r'''static u64 audio_duration_us(const AudioFrame* frame) {
    const u64 bytes_per_sec = (u64)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8u);
    return bytes_per_sec ? ((u64)frame->size * 1000000ULL) / bytes_per_sec : 0;
}

static void video_thread(void* arg) {
    (void)arg;
    VideoFrame frame;
    while (g_running) {
        if (captureReadVideo(&frame)) {
            if (!g_capture_enabled) continue;
            ++g_diag_video_captured;

            const u64 raw_ts = frame.timestamp_us;
            bool raw_gap = false;

            mutexLock(&g_timeline_lock);
            if (g_last_video_raw_ts && raw_ts > g_last_video_raw_ts &&
                raw_ts - g_last_video_raw_ts > TIMELINE_GAP_THRESHOLD_US) {
                raw_gap = true;
                ++g_diag_video_timeline_gaps;
            }
            g_last_video_raw_ts = raw_ts;
            mutexUnlock(&g_timeline_lock);

            if (raw_gap) enter_video_resync();

            /* HOME/system UI can emit tiny black IDRs instead of stopping the
               stream entirely. Treat those as a discontinuity, never as video. */
            if (frame.keyframe && frame.size < ACTIVE_KEYFRAME_MIN_BYTES) {
                ++g_diag_home_placeholder_keyframes;
                enter_video_resync();
                ++g_diag_video_resync_drops;
                continue;
            }

            if (g_need_video_keyframe) {
                if (!frame.keyframe || frame.size < ACTIVE_KEYFRAME_MIN_BYTES) {
                    ++g_diag_video_resync_drops;
                    continue;
                }

                mutexLock(&g_timeline_lock);
                u64 target_ts = 0;
                if (!g_timeline_started) {
                    /* First real game frame: recording time starts here. */
                    g_timeline_started = true;
                    target_ts = 0;
                } else {
                    const u64 video_next = g_last_video_out_ts + (1000000ULL / VIDEO_FPS);
                    target_ts = video_next;
                    if (g_last_audio_out_end_ts > target_ts)
                        target_ts = g_last_audio_out_end_ts;

                    /* Measure how much dead HOME/resync time was removed. */
                    if (raw_ts >= g_timeline_offset_us) {
                        const u64 old_projected = raw_ts - g_timeline_offset_us;
                        if (old_projected > target_ts)
                            g_diag_timeline_gap_us_removed += old_projected - target_ts;
                    }
                    ++g_diag_timeline_rebases;
                }

                g_timeline_offset_us = raw_ts >= target_ts ? raw_ts - target_ts : 0;
                g_resume_raw_ts = raw_ts;
                frame.timestamp_us = target_ts;

                const bool ok = enqueue_video(&frame, g_generation);
                if (ok) {
                    g_last_video_out_ts = target_ts;
                    g_need_video_keyframe = false;
                    g_timeline_rebase_pending = false;
                    ++g_diag_video_resync_recoveries;
                }
                mutexUnlock(&g_timeline_lock);

                if (!ok) {
                    ++g_diag_video_queue_overflows;
                    /* Keep waiting for another clean game IDR. */
                }
                continue;
            }

            mutexLock(&g_timeline_lock);
            if (!g_timeline_started) {
                mutexUnlock(&g_timeline_lock);
                continue;
            }

            u64 out_ts = raw_ts >= g_timeline_offset_us ? raw_ts - g_timeline_offset_us
                                                        : g_last_video_out_ts + (1000000ULL / VIDEO_FPS);
            frame.timestamp_us = out_ts;
            const bool ok = enqueue_video(&frame, g_generation);
            if (ok) g_last_video_out_ts = out_ts;
            mutexUnlock(&g_timeline_lock);

            if (!ok) {
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

            const u64 raw_ts = frame.timestamp_us;

            /* Audio gaps are diagnostic only. Video owns discontinuity/resume so
               staggered audio return cannot force a second resync after video. */
            mutexLock(&g_timeline_lock);
            if (g_last_audio_raw_ts && raw_ts > g_last_audio_raw_ts &&
                raw_ts - g_last_audio_raw_ts > TIMELINE_GAP_THRESHOLD_US) {
                ++g_diag_audio_timeline_gaps;
            }
            g_last_audio_raw_ts = raw_ts;

            if (!g_timeline_started || g_need_video_keyframe || raw_ts < g_resume_raw_ts) {
                mutexUnlock(&g_timeline_lock);
                ++g_diag_audio_held_for_video_resync;
                continue;
            }

            u64 out_ts = raw_ts >= g_timeline_offset_us ? raw_ts - g_timeline_offset_us : 0;
            if (out_ts < g_last_audio_out_end_ts)
                out_ts = g_last_audio_out_end_ts;

            frame.timestamp_us = out_ts;
            const bool ok = enqueue_audio(&frame, g_generation);
            if (ok)
                g_last_audio_out_end_ts = out_ts + audio_duration_us(&frame);
            mutexUnlock(&g_timeline_lock);

            if (!ok) ++g_diag_audio_queue_overflows;
        } else {
            if (g_capture_enabled) ++g_diag_audio_capture_failures;
            svcSleepThread(2 * 1000 * 1000LL);
        }
    }
}

'''
s = s[:start] + replacement + s[end:]

# Initialise the shared timeline mutex.
s = s.replace(
    '    mutexInit(&g_video_q_lock);\n    mutexInit(&g_audio_q_lock);',
    '    mutexInit(&g_video_q_lock);\n    mutexInit(&g_audio_q_lock);\n    mutexInit(&g_timeline_lock);'
)

p.write_text(s)

c = Path('work/source/config.h')
cs = c.read_text()
cs = cs.replace('#define SYSDVR_SD_VERSION "0.3.2-recovery"', '#define SYSDVR_SD_VERSION "0.3.6-shared-timeline"')
if 'TIMELINE_GAP_THRESHOLD_US' not in cs:
    cs += '\n#define TIMELINE_GAP_THRESHOLD_US 250000ULL\n'
if 'ACTIVE_KEYFRAME_MIN_BYTES' not in cs:
    cs += '/* HOME placeholder IDRs are only hundreds of bytes; real game IDRs are much larger. */\n#define ACTIVE_KEYFRAME_MIN_BYTES 4096u\n'
c.write_text(cs)
