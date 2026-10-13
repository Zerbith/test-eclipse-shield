#pragma once

#include <stdbool.h>

#ifdef __ANDROID__
#ifdef __cplusplus
extern "C" {
#endif

/** Start the co-op secondary in its own Android process. Returns its process id, or -1 if it did
 *  not come up within ten seconds. */
int eclipse_android_coop_start(const char *socket, const char *host, int app_id, int width,
                               int height);

/** Ask Android to stop the service (which ends its process). */
void eclipse_android_coop_stop(void);

/** The secondary writes its process id to a file; 0 when there is none. */
int eclipse_android_coop_pid(void);

/** Is that process still running? The secondary is not our child, so waitpid() cannot tell. */
bool eclipse_android_coop_alive(int pid);

#ifdef __cplusplus
}
#endif
#endif
