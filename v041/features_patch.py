from pathlib import Path

# v0.4.1 is intentionally a control/UI layer on top of the tested v0.4.0 /
# v0.3.6 recording core. It adds request-based telemetry and bookmarks without
# changing H.264 capture, HOME recovery or queue timing.

# --- config.h ---------------------------------------------------------------
c = Path("work/source/config.h")
cs = c.read_text()
cs = cs.replace('#define SYSDVR_SD_VERSION "0.4.0-config"',
                '#define SYSDVR_SD_VERSION "0.4.1-markers-status"')
if "MARKER_REQUEST_FILE" not in cs:
    cs += '''
#define MARKER_REQUEST_FILE RECORD_DIR "/marker.request"
#define MARKER_RESULT_FILE RECORD_DIR "/marker.result"
#define STATUS_REQUEST_FILE RECORD_DIR "/status.request"
#define STATUS_FILE RECORD_DIR "/status.txt"
#define STATUS_TEMP_FILE RECORD_DIR "/status.tmp"
'''
c.write_text(cs)

# --- recorder.h -------------------------------------------------------------
h = Path("work/source/recorder.h")
hs = h.read_text()
if "recorderAddMarker" not in hs:
    hs += '''
/* v0.4.1 bookmark support. timeline_us is the same shared media timeline fed
   to recorderPushVideo/Audio. The sidecar timestamp is made segment-relative. */
bool recorderAddMarker(uint64_t timeline_us, uint64_t* out_rel_ms, unsigned* out_index);
unsigned recorderMarkerCount(void);
'''
h.write_text(hs)

# --- recorder.c -------------------------------------------------------------
r = Path("work/source/recorder.c")
rs = r.read_text()

rs = rs.replace(
    'static char g_current_path[256];',
    'static char g_current_path[256];\nstatic unsigned g_marker_count = 0;'
)

rs = rs.replace(
    '    g_base_us = base_us;\n    g_last_space_check_ms = 0;\n    g_open = true;',
    '    g_base_us = base_us;\n    g_last_space_check_ms = 0;\n    g_marker_count = 0;\n    g_open = true;'
)

marker_impl = r'''
static void marker_path_locked(char* out, size_t out_size) {
    if (!out || !out_size) return;
    snprintf(out, out_size, "%s", g_current_path);
    size_t len = strlen(out);
    if (len >= 4 && strcmp(out + len - 4, ".mkv") == 0)
        snprintf(out + len - 4, out_size - (len - 4), ".markers.txt");
    else if (len + 12 < out_size)
        strcat(out, ".markers.txt");
}

bool recorderAddMarker(uint64_t timeline_us, uint64_t* out_rel_ms, unsigned* out_index) {
    bool ok = false;
    mutexLock(&g_lock);

    if (g_open && timeline_us >= g_base_us && g_current_path[0]) {
        const uint64_t rel_ms = (timeline_us - g_base_us) / 1000ULL;
        char path[320];
        marker_path_locked(path, sizeof(path));

        const bool new_file = access(path, F_OK) != 0;
        FILE* f = fopen(path, "ab");
        if (f) {
            if (new_file) {
                fprintf(f, "# SysDVR-SD markers\n");
                fprintf(f, "# video=%s\n", g_current_path);
                fprintf(f, "# index,time_ms,time\n");
            }

            const unsigned index = g_marker_count + 1u;
            const uint64_t total_sec = rel_ms / 1000ULL;
            const unsigned ms = (unsigned)(rel_ms % 1000ULL);
            const unsigned sec = (unsigned)(total_sec % 60ULL);
            const unsigned min = (unsigned)((total_sec / 60ULL) % 60ULL);
            const unsigned hour = (unsigned)(total_sec / 3600ULL);

            fprintf(f, "%u,%llu,%02u:%02u:%02u.%03u\n",
                    index,
                    (unsigned long long)rel_ms,
                    hour, min, sec, ms);
            if (fclose(f) == 0) {
                g_marker_count = index;
                if (out_rel_ms) *out_rel_ms = rel_ms;
                if (out_index) *out_index = index;
                ok = true;
            }
        }
    }

    mutexUnlock(&g_lock);
    return ok;
}

unsigned recorderMarkerCount(void) {
    unsigned count;
    mutexLock(&g_lock);
    count = g_marker_count;
    mutexUnlock(&g_lock);
    return count;
}
'''

if "bool recorderAddMarker(" not in rs:
    rs += marker_impl
r.write_text(rs)

# --- main.c ----------------------------------------------------------------
p = Path("work/source/main.c")
s = p.read_text()

s = s.replace(
    'static volatile u64 g_diag_audio_held_for_video_resync = 0;',
    'static volatile u64 g_diag_audio_held_for_video_resync = 0;\n'
    'static volatile u64 g_diag_markers_added = 0;'
)
s = s.replace(
    '    g_diag_audio_held_for_video_resync = 0;\n',
    '    g_diag_audio_held_for_video_resync = 0;\n'
    '    g_diag_markers_added = 0;\n',
    1
)

diag_marker = '''    fprintf(f, "home_placeholder_keyframes=%llu\\n",
            (unsigned long long)g_diag_home_placeholder_keyframes);'''
if diag_marker in s and 'markers_added=' not in s:
    s = s.replace(
        diag_marker,
        diag_marker + '''
    fprintf(f, "markers_added=%llu\\n",
            (unsigned long long)g_diag_markers_added);'''
    )

control_pos = s.index('static void control_thread(void* arg) {')
helpers = r'''
static bool take_request_file(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    remove(path);
    return true;
}

static void write_marker_result(bool ok, uint64_t rel_ms, unsigned index, const char* reason) {
    FILE* f = fopen(MARKER_RESULT_FILE, "wb");
    if (!f) return;
    fprintf(f, "ok=%u\n", ok ? 1u : 0u);
    fprintf(f, "index=%u\n", index);
    fprintf(f, "time_ms=%llu\n", (unsigned long long)rel_ms);
    fprintf(f, "reason=%s\n", reason ? reason : "");
    fclose(f);
}

static void handle_marker_request(void) {
    uint64_t timeline_us = 0;

    mutexLock(&g_timeline_lock);
    const bool can_mark = g_capture_enabled && g_timeline_started &&
                          !g_need_video_keyframe && g_last_video_out_ts > 0;
    if (can_mark) timeline_us = g_last_video_out_ts;
    mutexUnlock(&g_timeline_lock);

    if (!can_mark) {
        write_marker_result(false, 0, 0, "not-recording");
        return;
    }

    uint64_t rel_ms = 0;
    unsigned index = 0;
    if (recorderAddMarker(timeline_us, &rel_ms, &index)) {
        ++g_diag_markers_added;
        write_marker_result(true, rel_ms, index, "ok");
    } else {
        write_marker_result(false, 0, 0, "no-open-file");
    }
}

static void write_status_snapshot(void) {
    u32 video_q = 0;
    u32 audio_q = 0;

    mutexLock(&g_video_q_lock);
    video_q = g_video_q_count;
    mutexUnlock(&g_video_q_lock);

    mutexLock(&g_audio_q_lock);
    audio_q = g_audio_q_count;
    mutexUnlock(&g_audio_q_lock);

    uint64_t media_us = 0;
    bool timeline_started = false;
    bool recovering = false;
    mutexLock(&g_timeline_lock);
    media_us = g_last_video_out_ts;
    timeline_started = g_timeline_started;
    recovering = g_need_video_keyframe;
    mutexUnlock(&g_timeline_lock);

    struct stat active_st;
    const bool active = stat(ACTIVE_FILE, &active_st) == 0;

    FILE* f = fopen(STATUS_TEMP_FILE, "wb");
    if (!f) return;

    fprintf(f, "version=%s\n", SYSDVR_SD_VERSION);
    fprintf(f, "state=%s\n",
            active ? (recovering ? "Recovering" : "Recording")
                   : (g_capture_enabled ? "Armed" : "Off"));
    fprintf(f, "media_ms=%llu\n", (unsigned long long)(media_us / 1000ULL));
    fprintf(f, "timeline_started=%u\n", timeline_started ? 1u : 0u);
    fprintf(f, "video_queue=%u\n", (unsigned)video_q);
    fprintf(f, "video_queue_capacity=%u\n", (unsigned)VIDEO_QUEUE_SLOTS);
    fprintf(f, "audio_queue=%u\n", (unsigned)audio_q);
    fprintf(f, "audio_queue_capacity=%u\n", (unsigned)AUDIO_QUEUE_SLOTS);
    fprintf(f, "video_queue_overflows=%llu\n",
            (unsigned long long)g_diag_video_queue_overflows);
    fprintf(f, "audio_queue_overflows=%llu\n",
            (unsigned long long)g_diag_audio_queue_overflows);
    fprintf(f, "video_capture_failures=%llu\n",
            (unsigned long long)g_diag_video_capture_failures);
    fprintf(f, "audio_capture_failures=%llu\n",
            (unsigned long long)g_diag_audio_capture_failures);
    fprintf(f, "resync_events=%llu\n",
            (unsigned long long)g_diag_video_resync_events);
    fprintf(f, "timeline_rebases=%llu\n",
            (unsigned long long)g_diag_timeline_rebases);
    fprintf(f, "home_placeholder_keyframes=%llu\n",
            (unsigned long long)g_diag_home_placeholder_keyframes);
    fprintf(f, "markers=%llu\n",
            (unsigned long long)g_diag_markers_added);
    fclose(f);

    remove(STATUS_FILE);
    rename(STATUS_TEMP_FILE, STATUS_FILE);
}

'''
s = s[:control_pos] + helpers + s[control_pos:]

loop_marker = '''    while (g_running) {
        if (++config_poll_ticks >= 20u) {'''
loop_repl = '''    while (g_running) {
        if (take_request_file(MARKER_REQUEST_FILE))
            handle_marker_request();

        if (take_request_file(STATUS_REQUEST_FILE))
            write_status_snapshot();

        if (++config_poll_ticks >= 20u) {'''
if loop_marker not in s:
    raise SystemExit("v0.4.1 control loop marker not found")
s = s.replace(loop_marker, loop_repl, 1)

p.write_text(s)
