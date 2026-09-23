from pathlib import Path

# --- config.h ---------------------------------------------------------------
c = Path("work/source/config.h")
cs = c.read_text()
cs = cs.replace('#define SYSDVR_SD_VERSION "0.3.6-shared-timeline"',
                '#define SYSDVR_SD_VERSION "0.4.0-config"')
if "CONFIG_FILE" not in cs:
    cs += '''
#define CONFIG_FILE RECORD_DIR "/config.ini"
#define CONFIG_RELOAD_FILE RECORD_DIR "/config.reload"
'''
c.write_text(cs)

# --- main.c ----------------------------------------------------------------
p = Path("work/source/main.c")
s = p.read_text()

s = s.replace('#include "config.h"', '#include "config.h"\n#include "runtime_config.h"')

diag_start = s.index("static void write_diagnostics(void) {")
diag_end = s.index("static void enter_video_resync(void) {")
diag = r'''static void write_diagnostics(void) {
    const unsigned level = runtimeConfigDiagnosticsLevel();
    if (level == 0u) {
        remove(DIAGNOSTICS_FILE);
        return;
    }

    (void)mkdir(RECORD_DIR, 0777);
    FILE* f = fopen(DIAGNOSTICS_FILE, "w");
    if (!f) return;

    fprintf(f, "SysDVR-SD diagnostics\n");
    fprintf(f, "version=%s\n", SYSDVR_SD_VERSION);
    fprintf(f, "diagnostics_level=%u\n", level);
    fprintf(f, "segment_minutes=%u\n", runtimeConfigSegmentMinutes());
    fprintf(f, "min_free_mib=%u\n", runtimeConfigMinFreeMiB());
    fprintf(f, "audio_enabled=%u\n", runtimeConfigAudioEnabled() ? 1u : 0u);

    fprintf(f, "video_capture_failures=%llu\n", (unsigned long long)g_diag_video_capture_failures);
    fprintf(f, "audio_capture_failures=%llu\n", (unsigned long long)g_diag_audio_capture_failures);
    fprintf(f, "video_queue_overflows=%llu\n", (unsigned long long)g_diag_video_queue_overflows);
    fprintf(f, "audio_queue_overflows=%llu\n", (unsigned long long)g_diag_audio_queue_overflows);
    fprintf(f, "video_timeline_gaps=%llu\n", (unsigned long long)g_diag_video_timeline_gaps);
    fprintf(f, "audio_timeline_gaps=%llu\n", (unsigned long long)g_diag_audio_timeline_gaps);
    fprintf(f, "timeline_rebases=%llu\n", (unsigned long long)g_diag_timeline_rebases);
    fprintf(f, "timeline_gap_ms_removed=%llu\n",
            (unsigned long long)(g_diag_timeline_gap_us_removed / 1000ULL));
    fprintf(f, "home_placeholder_keyframes=%llu\n",
            (unsigned long long)g_diag_home_placeholder_keyframes);

    if (level >= 2u) {
        fprintf(f, "video_captured=%llu\n", (unsigned long long)g_diag_video_captured);
        fprintf(f, "audio_batches_captured=%llu\n", (unsigned long long)g_diag_audio_captured);
        fprintf(f, "video_resync_events=%llu\n", (unsigned long long)g_diag_video_resync_events);
        fprintf(f, "video_resync_predictive_frames_dropped=%llu\n",
                (unsigned long long)g_diag_video_resync_drops);
        fprintf(f, "video_resync_recoveries=%llu\n",
                (unsigned long long)g_diag_video_resync_recoveries);
        fprintf(f, "video_max_queue_used=%u/%u\n",
                (unsigned)g_diag_video_max_queue, (unsigned)VIDEO_QUEUE_SLOTS);
        fprintf(f, "audio_max_queue_used=%u/%u\n",
                (unsigned)g_diag_audio_max_queue, (unsigned)AUDIO_QUEUE_SLOTS);
        fprintf(f, "audio_batches_held_for_video_resync=%llu\n",
                (unsigned long long)g_diag_audio_held_for_video_resync);
    }

    fprintf(f, "\nInterpretation:\n");
    fprintf(f, "- capture_failures: Nintendo grc:d did not provide a usable packet.\n");
    fprintf(f, "- queue_overflows: the SD writer could not drain RAM fast enough.\n");
    fprintf(f, "- timeline_gap_ms_removed: HOME/system-UI time removed to keep A/V synchronized.\n");
    fclose(f);
}

'''
s = s[:diag_start] + diag + s[diag_end:]

audio_old = '''            if (!g_capture_enabled) continue;
            ++g_diag_audio_captured;

            const u64 raw_ts = frame.timestamp_us;'''
audio_new = '''            if (!g_capture_enabled) continue;
            if (!runtimeConfigAudioEnabled()) continue;
            ++g_diag_audio_captured;

            const u64 raw_ts = frame.timestamp_us;'''
if audio_old not in s:
    raise SystemExit("audio config insertion marker not found")
s = s.replace(audio_old, audio_new, 1)

control_old = '''    bool first = true;
    bool last = false;
    while (g_running) {
        bool enabled = control_file_exists();'''
control_new = '''    bool first = true;
    bool last = false;
    unsigned config_poll_ticks = 0;
    while (g_running) {
        if (++config_poll_ticks >= 20u) {
            config_poll_ticks = 0;
            struct stat cfgst;
            if (stat(CONFIG_RELOAD_FILE, &cfgst) == 0) {
                remove(CONFIG_RELOAD_FILE);
                (void)runtimeConfigReload();
            }
        }

        bool enabled = control_file_exists();'''
if control_old not in s:
    raise SystemExit("control config reload marker not found")
s = s.replace(control_old, control_new, 1)

init_old = '''    mutexInit(&g_video_q_lock);
    mutexInit(&g_audio_q_lock);
    mutexInit(&g_timeline_lock);
    if (!recorderInitialize()) return 1;'''
init_new = '''    mutexInit(&g_video_q_lock);
    mutexInit(&g_audio_q_lock);
    mutexInit(&g_timeline_lock);
    (void)runtimeConfigInit();
    if (!recorderInitialize()) return 1;'''
if init_old not in s:
    raise SystemExit("runtime config init marker not found")
s = s.replace(init_old, init_new, 1)

p.write_text(s)

# --- package script ---------------------------------------------------------
pkg = Path("work/scripts/package.sh")
ps = pkg.read_text()
ps = ps.replace(
    'mkdir -p "release/switch/.overlays"\n',
    'mkdir -p "release/switch/.overlays"\nmkdir -p "release/switch/SysDVR-SD"\n'
)
ps = ps.replace(
    'cp overlay/SysDVR-SD.ovl "release/switch/.overlays/SysDVR-SD.ovl"\n',
    'cp overlay/SysDVR-SD.ovl "release/switch/.overlays/SysDVR-SD.ovl"\n'
    'cp default_config.ini "release/switch/SysDVR-SD/config.example.ini"\n'
)
pkg.write_text(ps)
