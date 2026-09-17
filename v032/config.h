#pragma once

#define SYSDVR_SD_VERSION "0.3.2-recovery"
#define RECORD_DIR "sdmc:/switch/SysDVR-SD"
#define CONTROL_FILE RECORD_DIR "/recording.enabled"
#define ACTIVE_FILE RECORD_DIR "/recording.active"
#define DIAGNOSTICS_FILE RECORD_DIR "/diagnostics.txt"
#define SEGMENT_MINUTES 30u
#define MIN_FREE_MIB 512u
#define FLUSH_EVERY_BYTES (2u * 1024u * 1024u)

#define VIDEO_BUFFER_SIZE 0x54000u
#define AUDIO_CHUNK_SIZE 0x1000u
#define AUDIO_BATCH_CHUNKS 4u
#define AUDIO_BUFFER_SIZE (AUDIO_CHUNK_SIZE * AUDIO_BATCH_CHUNKS)

/* Keep the lite v0.3.1 footprint: about 1.3 MiB of packet queue storage. */
#define VIDEO_QUEUE_SLOTS 3u
#define AUDIO_QUEUE_SLOTS 16u

#define VIDEO_WIDTH 1280u
#define VIDEO_HEIGHT 720u
#define VIDEO_FPS 30u
#define AUDIO_SAMPLE_RATE 48000u
#define AUDIO_CHANNELS 2u
#define AUDIO_BITS_PER_SAMPLE 16u
