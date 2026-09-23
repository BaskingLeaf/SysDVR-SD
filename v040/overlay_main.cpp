#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <string>

static constexpr const char* kRecordDir = "sdmc:/switch/SysDVR-SD";
static constexpr const char* kControlFile = "sdmc:/switch/SysDVR-SD/recording.enabled";
static constexpr const char* kActiveFile = "sdmc:/switch/SysDVR-SD/recording.active";
static constexpr const char* kConfigFile = "sdmc:/switch/SysDVR-SD/config.ini";
static constexpr const char* kConfigTemp = "sdmc:/switch/SysDVR-SD/config.tmp";
static constexpr const char* kReloadFile = "sdmc:/switch/SysDVR-SD/config.reload";

struct AppConfig {
    unsigned segmentMinutes = 30;
    unsigned minFreeMiB = 512;
    bool audioEnabled = true;
    unsigned diagnosticsLevel = 1;
};

static bool fileExists(const char* path) {
    struct stat st{};
    return stat(path, &st) == 0;
}

static void ensureDirs() {
    mkdir("sdmc:/switch", 0777);
    mkdir(kRecordDir, 0777);
}

static AppConfig loadConfig() {
    AppConfig c;
    FILE* f = fopen(kConfigFile, "rb");
    if (!f) return c;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char key[64]{};
        unsigned value = 0;
        if (sscanf(line, " %63[^=]=%u", key, &value) != 2) continue;

        size_t n = strlen(key);
        while (n && (key[n - 1] == ' ' || key[n - 1] == '\t')) key[--n] = '\0';

        if (strcmp(key, "segment_minutes") == 0) c.segmentMinutes = value;
        else if (strcmp(key, "min_free_mib") == 0) c.minFreeMiB = value;
        else if (strcmp(key, "audio_enabled") == 0) c.audioEnabled = value != 0;
        else if (strcmp(key, "diagnostics_level") == 0) c.diagnosticsLevel = value > 2 ? 2 : value;
    }
    fclose(f);
    return c;
}

static bool saveConfig(const AppConfig& c) {
    ensureDirs();

    FILE* f = fopen(kConfigTemp, "wb");
    if (!f) return false;

    fputs("# SysDVR-SD persistent settings\n", f);
    fputs("# segment_minutes: 0 = unlimited\n", f);
    fputs("# diagnostics_level: 0=off, 1=basic, 2=detailed\n", f);
    fprintf(f, "segment_minutes=%u\n", c.segmentMinutes);
    fprintf(f, "min_free_mib=%u\n", c.minFreeMiB);
    fprintf(f, "audio_enabled=%u\n", c.audioEnabled ? 1u : 0u);
    fprintf(f, "diagnostics_level=%u\n", c.diagnosticsLevel);
    fclose(f);

    remove(kConfigFile);
    if (rename(kConfigTemp, kConfigFile) != 0) {
        remove(kConfigTemp);
        return false;
    }

    FILE* r = fopen(kReloadFile, "wb");
    if (r) {
        fputs("reload\n", r);
        fclose(r);
    }
    return true;
}

static bool setRecordingEnabled(bool enabled) {
    ensureDirs();
    if (enabled) {
        FILE* f = fopen(kControlFile, "wb");
        if (!f) return false;
        fputs("enabled\n", f);
        fclose(f);
        return true;
    }

    if (remove(kControlFile) == 0) return true;
    return !fileExists(kControlFile);
}

static std::string statusText() {
    if (fileExists(kActiveFile)) return "Recording";
    if (fileExists(kControlFile)) return "Armed";
    return "Off";
}

struct ActiveInfo {
    std::string path;
    u64 startTick = 0;
};

static ActiveInfo readActiveInfo() {
    ActiveInfo info;
    FILE* f = fopen(kActiveFile, "rb");
    if (!f) return info;

    char line[384];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "path=", 5) == 0) {
            char* p = line + 5;
            p[strcspn(p, "\r\n")] = '\0';
            info.path = p;
        } else if (strncmp(line, "start_tick=", 11) == 0) {
            info.startTick = strtoull(line + 11, nullptr, 10);
        }
    }
    fclose(f);
    return info;
}

static std::string formatElapsed(u64 startTick) {
    if (!startTick) return "--:--";
    const u64 now = armGetSystemTick();
    if (now < startTick) return "--:--";
    const u64 sec = armTicksToNs(now - startTick) / 1000000000ULL;
    char out[32];
    if (sec >= 3600ULL)
        snprintf(out, sizeof(out), "%llu:%02llu:%02llu",
                 (unsigned long long)(sec / 3600ULL),
                 (unsigned long long)((sec / 60ULL) % 60ULL),
                 (unsigned long long)(sec % 60ULL));
    else
        snprintf(out, sizeof(out), "%02llu:%02llu",
                 (unsigned long long)(sec / 60ULL),
                 (unsigned long long)(sec % 60ULL));
    return out;
}

static std::string formatFileSize(const std::string& path) {
    if (path.empty()) return "--";
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return "--";

    const double mib = static_cast<double>(st.st_size) / (1024.0 * 1024.0);
    char out[32];
    if (mib >= 1024.0)
        snprintf(out, sizeof(out), "%.2f GB", mib / 1024.0);
    else
        snprintf(out, sizeof(out), "%.0f MB", mib);
    return out;
}

static std::string formatFreeSpace() {
    struct statvfs vfs{};
    if (statvfs("sdmc:/", &vfs) != 0) return "--";
    const double gib = static_cast<double>(vfs.f_bavail) * static_cast<double>(vfs.f_frsize) /
                       (1024.0 * 1024.0 * 1024.0);
    char out[32];
    snprintf(out, sizeof(out), "%.1f GB", gib);
    return out;
}

static std::string segmentText(unsigned m) {
    if (m == 0) return "Unlimited";
    char out[32];
    snprintf(out, sizeof(out), "%u min", m);
    return out;
}

static std::string minFreeText(unsigned mib) {
    char out[32];
    if (mib >= 1024 && mib % 1024 == 0)
        snprintf(out, sizeof(out), "%u GB", mib / 1024);
    else
        snprintf(out, sizeof(out), "%u MB", mib);
    return out;
}

static const char* diagText(unsigned level) {
    switch (level) {
        case 0: return "Off";
        case 2: return "Detailed";
        default: return "Basic";
    }
}

class SettingsGui : public tsl::Gui {
public:
    AppConfig cfg{};
    tsl::elm::ListItem* segment = nullptr;
    tsl::elm::ListItem* minFree = nullptr;
    tsl::elm::ToggleListItem* audio = nullptr;
    tsl::elm::ListItem* diagnostics = nullptr;
    tsl::elm::ListItem* result = nullptr;

    tsl::elm::Element* createUI() override {
        cfg = loadConfig();

        auto* frame = new tsl::elm::OverlayFrame("SysDVR-SD Settings", "v0.4.0");
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Recording"));

        segment = new tsl::elm::ListItem("Segment length", segmentText(cfg.segmentMinutes));
        segment->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            static const unsigned values[] = {10u, 30u, 60u, 120u, 0u};
            size_t at = 0;
            for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
                if (values[i] == cfg.segmentMinutes) at = i;
            cfg.segmentMinutes = values[(at + 1) % (sizeof(values) / sizeof(values[0]))];
            segment->setValue(segmentText(cfg.segmentMinutes));
            result->setValue(saveConfig(cfg) ? "Saved" : "Save failed", !fileExists(kConfigFile));
            return true;
        });
        list->addItem(segment);

        minFree = new tsl::elm::ListItem("Minimum SD free", minFreeText(cfg.minFreeMiB));
        minFree->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            static const unsigned values[] = {256u, 512u, 1024u, 2048u, 4096u};
            size_t at = 0;
            for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
                if (values[i] == cfg.minFreeMiB) at = i;
            cfg.minFreeMiB = values[(at + 1) % (sizeof(values) / sizeof(values[0]))];
            minFree->setValue(minFreeText(cfg.minFreeMiB));
            result->setValue(saveConfig(cfg) ? "Saved" : "Save failed", !fileExists(kConfigFile));
            return true;
        });
        list->addItem(minFree);

        audio = new tsl::elm::ToggleListItem("Game audio", cfg.audioEnabled, "ON", "OFF");
        audio->setStateChangedListener([this](bool state) {
            cfg.audioEnabled = state;
            if (!saveConfig(cfg)) {
                cfg.audioEnabled = !state;
                audio->setState(!state);
                result->setValue("Save failed", true);
            } else {
                result->setValue("Saved", false);
            }
        });
        list->addItem(audio);

        diagnostics = new tsl::elm::ListItem("Diagnostics", diagText(cfg.diagnosticsLevel));
        diagnostics->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            cfg.diagnosticsLevel = (cfg.diagnosticsLevel + 1u) % 3u;
            diagnostics->setValue(diagText(cfg.diagnosticsLevel));
            result->setValue(saveConfig(cfg) ? "Saved" : "Save failed", !fileExists(kConfigFile));
            return true;
        });
        list->addItem(diagnostics);

        list->addItem(new tsl::elm::CategoryHeader("Capture behaviour"));
        list->addItem(new tsl::elm::ListItem("HOME handling", "Cut + re-sync"));
        list->addItem(new tsl::elm::ListItem("Game start", "Wait for real IDR"));
        list->addItem(new tsl::elm::ListItem("Video", "720p / 30 fps H.264"));

        result = new tsl::elm::ListItem("Config", "Ready");
        list->addItem(result);

        frame->setContent(list);
        return frame;
    }
};

class RecorderGui : public tsl::Gui {
public:
    tsl::elm::ToggleListItem* toggle = nullptr;
    tsl::elm::ListItem* status = nullptr;
    tsl::elm::ListItem* elapsed = nullptr;
    tsl::elm::ListItem* fileSize = nullptr;
    tsl::elm::ListItem* sdFree = nullptr;
    tsl::elm::ListItem* result = nullptr;
    u64 lastRefreshTick = 0;

    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("SysDVR-SD", "v0.4.0");
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Recorder"));

        const bool enabled = fileExists(kControlFile);
        toggle = new tsl::elm::ToggleListItem("Recording Enabled", enabled, "ON", "OFF");
        toggle->setStateChangedListener([this](bool state) {
            if (!setRecordingEnabled(state)) {
                toggle->setState(!state);
                result->setValue("SD write failed", true);
            } else {
                result->setValue(state ? "Start requested" : "Stop requested", false);
            }
            refreshStatus();
        });
        list->addItem(toggle);

        status = new tsl::elm::ListItem("State", statusText());
        list->addItem(status);
        elapsed = new tsl::elm::ListItem("Elapsed", "--:--");
        list->addItem(elapsed);
        fileSize = new tsl::elm::ListItem("Current file", "--");
        list->addItem(fileSize);
        sdFree = new tsl::elm::ListItem("SD free", formatFreeSpace());
        list->addItem(sdFree);

        auto* settings = new tsl::elm::ListItem("Settings", ">");
        settings->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<SettingsGui>();
                return true;
            }
            return false;
        });
        list->addItem(settings);

        result = new tsl::elm::ListItem("Command", "Ready");
        list->addItem(result);

        list->addItem(new tsl::elm::CategoryHeader("Current behaviour"));
        list->addItem(new tsl::elm::ListItem("HOME menu", "Cut + re-sync"));
        list->addItem(new tsl::elm::ListItem("Folder", "/switch/SysDVR-SD"));

        frame->setContent(list);
        refreshStatus();
        lastRefreshTick = armGetSystemTick();
        return frame;
    }

    void refreshStatus() {
        if (!status || !toggle) return;

        const bool enabled = fileExists(kControlFile);
        const bool active = fileExists(kActiveFile);
        if (toggle->getState() != enabled) toggle->setState(enabled);
        status->setValue(statusText(), enabled && !active);

        if (active) {
            const ActiveInfo info = readActiveInfo();
            elapsed->setValue(formatElapsed(info.startTick));
            fileSize->setValue(formatFileSize(info.path));
        } else {
            elapsed->setValue("--:--");
            fileSize->setValue("--");
        }
        sdFree->setValue(formatFreeSpace());
    }

    void update() override {
        const u64 now = armGetSystemTick();
        if (armTicksToNs(now - lastRefreshTick) >= 500000000ULL) {
            lastRefreshTick = now;
            refreshStatus();
        }
    }
};

class RecorderOverlay : public tsl::Overlay {
public:
    Result sdMountResult = 0;

    void initServices() override {
        sdMountResult = fsdevMountSdmc();
        if (R_SUCCEEDED(sdMountResult)) ensureDirs();
    }

    void exitServices() override {
        if (R_SUCCEEDED(sdMountResult)) fsdevUnmountDevice("sdmc");
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<RecorderGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<RecorderOverlay>(argc, argv);
}
