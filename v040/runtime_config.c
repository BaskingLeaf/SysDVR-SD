#include <switch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "runtime_config.h"
#include "config.h"

typedef struct {
    unsigned segment_minutes;
    unsigned min_free_mib;
    bool audio_enabled;
    unsigned diagnostics_level;
} RuntimeConfig;

static Mutex g_cfg_lock;
static RuntimeConfig g_cfg;

static RuntimeConfig default_config(void) {
    RuntimeConfig c;
    c.segment_minutes = 30u;
    c.min_free_mib = 512u;
    c.audio_enabled = true;
    c.diagnostics_level = 1u;
    return c;
}

static unsigned clamp_u(unsigned v, unsigned lo, unsigned hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void ensure_dir(void) {
    (void)mkdir("sdmc:/switch", 0777);
    (void)mkdir(RECORD_DIR, 0777);
}

static void write_default_file(void) {
    FILE* f = fopen(CONFIG_FILE, "wb");
    if (!f) return;
    fputs("# SysDVR-SD persistent settings\n", f);
    fputs("# segment_minutes: 0 = unlimited, otherwise 5-240\n", f);
    fputs("# diagnostics_level: 0=off, 1=basic, 2=detailed\n", f);
    fputs("segment_minutes=30\n", f);
    fputs("min_free_mib=512\n", f);
    fputs("audio_enabled=1\n", f);
    fputs("diagnostics_level=1\n", f);
    fclose(f);
}

bool runtimeConfigReload(void) {
    RuntimeConfig next = default_config();
    FILE* f = fopen(CONFIG_FILE, "rb");
    if (!f) {
        mutexLock(&g_cfg_lock);
        g_cfg = next;
        mutexUnlock(&g_cfg_lock);
        return false;
    }

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\r' || *p == '\n') continue;

        char key[64] = {0};
        unsigned value = 0;
        if (sscanf(p, "%63[^=]=%u", key, &value) != 2) continue;

        size_t n = strlen(key);
        while (n && (key[n - 1] == ' ' || key[n - 1] == '\t')) key[--n] = '\0';

        if (strcmp(key, "segment_minutes") == 0) {
            next.segment_minutes = value == 0 ? 0u : clamp_u(value, 5u, 240u);
        } else if (strcmp(key, "min_free_mib") == 0) {
            next.min_free_mib = clamp_u(value, 128u, 8192u);
        } else if (strcmp(key, "audio_enabled") == 0) {
            next.audio_enabled = value != 0;
        } else if (strcmp(key, "diagnostics_level") == 0) {
            next.diagnostics_level = clamp_u(value, 0u, 2u);
        }
    }
    fclose(f);

    mutexLock(&g_cfg_lock);
    g_cfg = next;
    mutexUnlock(&g_cfg_lock);
    return true;
}

bool runtimeConfigInit(void) {
    mutexInit(&g_cfg_lock);
    g_cfg = default_config();
    ensure_dir();

    struct stat st;
    if (stat(CONFIG_FILE, &st) != 0) write_default_file();
    return runtimeConfigReload();
}

unsigned runtimeConfigSegmentMinutes(void) {
    unsigned v;
    mutexLock(&g_cfg_lock);
    v = g_cfg.segment_minutes;
    mutexUnlock(&g_cfg_lock);
    return v;
}

unsigned runtimeConfigMinFreeMiB(void) {
    unsigned v;
    mutexLock(&g_cfg_lock);
    v = g_cfg.min_free_mib;
    mutexUnlock(&g_cfg_lock);
    return v;
}

bool runtimeConfigAudioEnabled(void) {
    bool v;
    mutexLock(&g_cfg_lock);
    v = g_cfg.audio_enabled;
    mutexUnlock(&g_cfg_lock);
    return v;
}

unsigned runtimeConfigDiagnosticsLevel(void) {
    unsigned v;
    mutexLock(&g_cfg_lock);
    v = g_cfg.diagnostics_level;
    mutexUnlock(&g_cfg_lock);
    return v;
}
