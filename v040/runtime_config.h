#pragma once
#include <stdbool.h>

bool runtimeConfigInit(void);
bool runtimeConfigReload(void);

unsigned runtimeConfigSegmentMinutes(void);
unsigned runtimeConfigMinFreeMiB(void);
bool runtimeConfigAudioEnabled(void);
unsigned runtimeConfigDiagnosticsLevel(void);
