/*
 * Android co-op without composition: one hardware decoder and one Surface per host.
 *
 * The desktop and TV builds splice both hosts' pictures into ONE bitstream and feed ONE decoder.
 * That needs a decoder that handles the spliced stream exactly, and the Shield's (NVIDIA's OMX
 * HEVC decoder behind MediaCodec) shows garbage for it: one pane green, the other blocky, in both
 * orientations. Here every host keeps its own, completely ordinary stream:
 *
 *   - the primary session's stream goes to the first decoder, straight from the normal video path;
 *   - the secondary session's access units arrive over the co-op socket and go to a SECOND
 *     decoder, fed from the thread in this file;
 *   - each decoder renders into its own SurfaceView and the Activity places the two next to each
 *     other (top/bottom for horizontal co-op, left/right for vertical).
 *
 * Nothing is rewritten, so the picture is exactly what each host encoded, at the full resolution of
 * its half of the screen.
 */
#pragma once

#ifdef __ANDROID__

#include <stdbool.h>

#include "ss4s.h"

/** Open the second decoder (same codec and size as the first) and start feeding it the secondary
 *  host's access units. Returns false if the second decoder cannot be opened. */
bool coop_dual_start(const SS4S_VideoInfo *info);

/** Stop feeding and close the second decoder. Safe when nothing was started. */
void coop_dual_stop(void);

/** True from coop_dual_start() until coop_dual_stop(). */
bool coop_dual_running(void);

/** True once the second decoder has been given its first picture (a keyframe). */
bool coop_dual_peer_started(void);

#endif
