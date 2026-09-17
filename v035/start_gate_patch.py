from pathlib import Path

p = Path('work/source/main.c')
s = p.read_text()

# v0.3.4 already waits for an IDR after enable/resync. Nintendo grc:d can emit
# tiny black placeholder IDRs while HOME/system UI is visible; accepting one of
# those opens the MKV before real gameplay and lets audio establish the clock.
old = '''            if (g_need_video_keyframe) {\n                if (!frame.keyframe) {\n                    ++g_diag_video_resync_drops;\n                    continue;\n                }'''
new = '''            if (g_need_video_keyframe) {\n                /* HOME/system UI produces tiny black placeholder packets (tens\n                   to a few hundred bytes). Do not open/resume the recording on\n                   those. Wait for a substantial game IDR so video establishes\n                   the timeline first, then audio is allowed through. */\n                if (!frame.keyframe || frame.size < ACTIVE_KEYFRAME_MIN_BYTES) {\n                    ++g_diag_video_resync_drops;\n                    continue;\n                }'''
if old not in s:
    raise SystemExit('keyframe gate marker not found')
s = s.replace(old, new)
p.write_text(s)

c = Path('work/source/config.h')
cs = c.read_text()
cs = cs.replace('#define SYSDVR_SD_VERSION "0.3.4-homegap"', '#define SYSDVR_SD_VERSION "0.3.5-gamegate"')
if 'ACTIVE_KEYFRAME_MIN_BYTES' not in cs:
    cs += '\n/* Reject tiny black HOME-menu placeholder IDRs as recording start points. */\n#define ACTIVE_KEYFRAME_MIN_BYTES 4096u\n'
c.write_text(cs)
