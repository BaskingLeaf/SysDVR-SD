# v0.3.3 writer-throughput build

Based on v0.3.2 diagnostics showing queue overflow with almost no grc:d capture failures.

Build changes:
- writer thread priority 45 -> 43
- video queue 3 -> 5 slots
- audio queue 16 -> 24 slots
- MKV stdio buffer 64 KiB -> 256 KiB
- periodic fflush threshold 2 MiB -> 32 MiB
- retain v0.3.2 keyframe recovery and diagnostics
