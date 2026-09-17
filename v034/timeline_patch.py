from pathlib import Path

p = Path('work/source/main.c')
s = p.read_text()

s = s.replace(
    'static volatile bool g_need_video_keyframe = true;\nstatic bool g_sd_mounted = false;',
    '''static volatile bool g_need_video_keyframe = true;\nstatic bool g_sd_mounted = false;\n\n/* Nintendo's gameplay capture stream pauses while HOME/system UI is shown.\n   Compact those uncapturable holes out of the output timeline instead of\n   leaving seconds of frozen/black video and silence. */\nstatic u64 g_video_last_raw_ts = 0;\nstatic u64 g_audio_last_raw_ts = 0;\nstatic u64 g_video_ts_offset = 0;\nstatic u64 g_audio_ts_offset = 0;\nstatic volatile u64 g_diag_video_timeline_gaps = 0;\nstatic volatile u64 g_diag_audio_timeline_gaps = 0;\nstatic volatile u64 g_diag_video_gap_us_removed = 0;\nstatic volatile u64 g_diag_audio_gap_us_removed = 0;\nstatic volatile u64 g_diag_audio_held_for_video_resync = 0;'''
)

s = s.replace(
    '    g_diag_audio_max_queue = 0;\n}',
    '''    g_diag_audio_max_queue = 0;\n    g_diag_video_timeline_gaps = 0;\n    g_diag_audio_timeline_gaps = 0;\n    g_diag_video_gap_us_removed = 0;\n    g_diag_audio_gap_us_removed = 0;\n    g_diag_audio_held_for_video_resync = 0;\n    g_video_last_raw_ts = 0;\n    g_audio_last_raw_ts = 0;\n    g_video_ts_offset = 0;\n    g_audio_ts_offset = 0;\n}'''
)

s = s.replace(
    '    fprintf(f, "audio_max_queue_used=%u/%u\\n", (unsigned)g_diag_audio_max_queue, (unsigned)AUDIO_QUEUE_SLOTS);',
    '''    fprintf(f, "audio_max_queue_used=%u/%u\\n", (unsigned)g_diag_audio_max_queue, (unsigned)AUDIO_QUEUE_SLOTS);\n    fprintf(f, "video_timeline_gaps=%llu\\n", (unsigned long long)g_diag_video_timeline_gaps);\n    fprintf(f, "audio_timeline_gaps=%llu\\n", (unsigned long long)g_diag_audio_timeline_gaps);\n    fprintf(f, "video_gap_ms_removed=%llu\\n", (unsigned long long)(g_diag_video_gap_us_removed / 1000ULL));\n    fprintf(f, "audio_gap_ms_removed=%llu\\n", (unsigned long long)(g_diag_audio_gap_us_removed / 1000ULL));\n    fprintf(f, "audio_batches_held_for_video_resync=%llu\\n", (unsigned long long)g_diag_audio_held_for_video_resync);'''
)

video_marker = '''            ++g_diag_video_captured;\n\n            /* After any packet loss, never feed dependent H.264 pictures to the file.'''
video_repl = '''            ++g_diag_video_captured;\n\n            /* Detect HOME/system-overlay suspension or any other long discontinuity.\n               Keep one normal frame interval and remove the uncapturable remainder. */\n            {\n                u64 raw_ts = frame.timestamp_us;\n                if (g_video_last_raw_ts && raw_ts > g_video_last_raw_ts) {\n                    u64 delta = raw_ts - g_video_last_raw_ts;\n                    if (delta > TIMELINE_GAP_THRESHOLD_US) {\n                        const u64 keep_us = 1000000ULL / VIDEO_FPS;\n                        u64 removed = delta > keep_us ? delta - keep_us : 0;\n                        g_video_ts_offset += removed;\n                        ++g_diag_video_timeline_gaps;\n                        g_diag_video_gap_us_removed += removed;\n                        enter_video_resync();\n                    }\n                }\n                g_video_last_raw_ts = raw_ts;\n                frame.timestamp_us = raw_ts - g_video_ts_offset;\n            }\n\n            /* After any packet loss, never feed dependent H.264 pictures to the file.'''
if video_marker not in s:
    raise SystemExit('video marker not found')
s = s.replace(video_marker, video_repl)

audio_marker = '''            ++g_diag_audio_captured;\n            if (!enqueue_audio(&frame, g_generation)) ++g_diag_audio_queue_overflows;'''
audio_repl = '''            ++g_diag_audio_captured;\n\n            {\n                u64 raw_ts = frame.timestamp_us;\n                if (g_audio_last_raw_ts && raw_ts > g_audio_last_raw_ts) {\n                    u64 delta = raw_ts - g_audio_last_raw_ts;\n                    if (delta > TIMELINE_GAP_THRESHOLD_US) {\n                        const u64 bytes_per_sec = (u64)AUDIO_SAMPLE_RATE * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8u);\n                        u64 keep_us = bytes_per_sec ? ((u64)frame.size * 1000000ULL) / bytes_per_sec : 0;\n                        if (!keep_us) keep_us = 1000;\n                        u64 removed = delta > keep_us ? delta - keep_us : 0;\n                        g_audio_ts_offset += removed;\n                        ++g_diag_audio_timeline_gaps;\n                        g_diag_audio_gap_us_removed += removed;\n                        /* Audio may resume a little before video. Force a clean video\n                           recovery point so sound never runs ahead of the picture. */\n                        enter_video_resync();\n                    }\n                }\n                g_audio_last_raw_ts = raw_ts;\n                frame.timestamp_us = raw_ts - g_audio_ts_offset;\n            }\n\n            if (g_need_video_keyframe) {\n                ++g_diag_audio_held_for_video_resync;\n                continue;\n            }\n\n            if (!enqueue_audio(&frame, g_generation)) ++g_diag_audio_queue_overflows;'''
if audio_marker not in s:
    raise SystemExit('audio marker not found')
s = s.replace(audio_marker, audio_repl)

p.write_text(s)

c = Path('work/source/config.h')
cs = c.read_text()
cs = cs.replace('#define SYSDVR_SD_VERSION "0.3.2-recovery"', '#define SYSDVR_SD_VERSION "0.3.4-homegap"')
if 'TIMELINE_GAP_THRESHOLD_US' not in cs:
    cs = cs.replace('#define AUDIO_QUEUE_SLOTS 16u', '#define AUDIO_QUEUE_SLOTS 16u\n\n#define TIMELINE_GAP_THRESHOLD_US 250000ULL')
c.write_text(cs)
