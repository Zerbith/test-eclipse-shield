#ifdef __ANDROID__

#include "coop_dual_android.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "coop_coord.h"
#include "logging.h"

#define DUAL_BUF_CAP (8u * 1024u * 1024u)
#define DUAL_WAIT_MS 50
#define DUAL_STATS_MS 5000ul

static SS4S_Player *dual_player = NULL;
static pthread_t dual_thread;
static bool dual_thread_started = false;
static pthread_mutex_t dual_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dual_cond = PTHREAD_COND_INITIALIZER;
static bool dual_kick = false;
static atomic_bool dual_stop_req = false;
static atomic_bool dual_running = false;
static atomic_bool dual_started = false;

static unsigned char *dual_buf = NULL;
static bool dual_synced = false;      /* the decoder holds an unbroken run ending at dual_expect - 1 */
static uint32_t dual_expect = 0;
static unsigned dual_fed, dual_unsynced, dual_holes, dual_rejects, dual_idr_req;

static unsigned long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long) ts.tv_sec * 1000ul + (unsigned long) (ts.tv_nsec / 1000000ul);
}

/* Called from the coordinator's drain thread whenever a pane arrives. */
static void dual_wake(void) {
    pthread_mutex_lock(&dual_lock);
    dual_kick = true;
    pthread_cond_signal(&dual_cond);
    pthread_mutex_unlock(&dual_lock);
}

static void dual_ask_for_keyframe(void) {
    /* The coordinator rate-limits these (about 8 a second at most). */
    coop_coord_request_peer_idr_keep();
    dual_idr_req++;
}

static void dual_handle(size_t len, uint32_t frame, bool idr) {
    if (!dual_synced) {
        if (!idr) {
            /* Every inter picture predicts from the one before it. Without a keyframe to start
             * from there is nothing valid to show. */
            dual_unsynced++;
            dual_ask_for_keyframe();
            return;
        }
    } else if (!idr && frame != dual_expect) {
        /* A picture went missing between the host and here. The ones after it reference it. */
        dual_holes++;
        dual_synced = false;
        dual_ask_for_keyframe();
        return;
    }
    SS4S_VideoFeedFlags flags = SS4S_VIDEO_FEED_DATA_FRAME_START | SS4S_VIDEO_FEED_DATA_FRAME_END;
    if (idr) {
        flags |= SS4S_VIDEO_FEED_DATA_KEYFRAME;
    }
    const SS4S_VideoFeedResult r = SS4S_PlayerVideoFeed(dual_player, dual_buf, len, flags);
    if (r == SS4S_VIDEO_FEED_OK) {
        dual_synced = true;
        dual_expect = frame + 1u;
        dual_fed++;
        if (!atomic_exchange(&dual_started, true)) {
            commons_log_info("CoopDual", "second decoder received its first picture (frame %u, %zu bytes, %s)",
                             (unsigned) frame, len, idr ? "keyframe" : "inter");
        }
    } else {
        /* The decoder dropped it (busy) or refused it; the chain is broken from here. */
        dual_rejects++;
        dual_synced = false;
        dual_ask_for_keyframe();
    }
}

static void dual_drain(void) {
    for (int i = 0; i < 32 && !atomic_load(&dual_stop_req); i++) {
        uint32_t frame = 0;
        unsigned skipped = 0;
        const size_t len = coop_coord_take_next(dual_buf, DUAL_BUF_CAP, 0, &frame, &skipped);
        if (len == 0) {
            break;
        }
        coop_ipc_hdr_t meta;
        memset(&meta, 0, sizeof(meta));
        coop_coord_last_taken_meta(&meta, NULL);
        dual_handle(len, frame, meta.is_idr != 0);
    }
}

static void *dual_main(void *arg) {
    (void) arg;
    unsigned long last_stats = mono_ms();
    while (!atomic_load(&dual_stop_req)) {
        pthread_mutex_lock(&dual_lock);
        if (!dual_kick && !atomic_load(&dual_stop_req)) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += (long) DUAL_WAIT_MS * 1000000L;
            while (until.tv_nsec >= 1000000000L) {
                until.tv_nsec -= 1000000000L;
                until.tv_sec++;
            }
            (void) pthread_cond_timedwait(&dual_cond, &dual_lock, &until);
        }
        dual_kick = false;
        pthread_mutex_unlock(&dual_lock);
        if (atomic_load(&dual_stop_req)) {
            break;
        }
        dual_drain();
        const unsigned long now = mono_ms();
        if (now - last_stats >= DUAL_STATS_MS) {
            last_stats = now;
            commons_log_info("CoopDual",
                             "peer: fed=%u waitingForKeyframe=%u holes=%u rejects=%u keyframeRequests=%u queued=%u "
                             "received=%u evicted=%u synced=%d",
                             dual_fed, dual_unsynced, dual_holes, dual_rejects, dual_idr_req, coop_coord_queued(),
                             coop_coord_received(), coop_coord_replaced(), (int) dual_synced);
        }
    }
    return NULL;
}

bool coop_dual_start(const SS4S_VideoInfo *info) {
    if (atomic_load(&dual_running)) {
        return true;
    }
    dual_player = SS4S_PlayerOpen();
    if (dual_player == NULL) {
        commons_log_error("CoopDual", "cannot create the second player");
        return false;
    }
    const SS4S_VideoOpenResult r = SS4S_PlayerVideoOpen(dual_player, info);
    if (r != SS4S_VIDEO_OPEN_OK) {
        commons_log_error("CoopDual", "cannot open the second decoder (result %d)", (int) r);
        SS4S_PlayerClose(dual_player);
        dual_player = NULL;
        return false;
    }
    dual_buf = malloc(DUAL_BUF_CAP);
    if (dual_buf == NULL) {
        SS4S_PlayerVideoClose(dual_player);
        SS4S_PlayerClose(dual_player);
        dual_player = NULL;
        return false;
    }
    dual_synced = false;
    dual_expect = 0;
    dual_fed = dual_unsynced = dual_holes = dual_rejects = dual_idr_req = 0;
    atomic_store(&dual_started, false);
    atomic_store(&dual_stop_req, false);
    dual_kick = false;

    /* Anything queued so far predates the decoder. Start from a keyframe that arrives now. */
    coop_coord_flush();
    coop_coord_request_peer_idr();
    coop_coord_set_wakeup(dual_wake);

    atomic_store(&dual_running, true);
    if (pthread_create(&dual_thread, NULL, dual_main, NULL) != 0) {
        atomic_store(&dual_running, false);
        coop_coord_set_wakeup(NULL);
        SS4S_PlayerVideoClose(dual_player);
        SS4S_PlayerClose(dual_player);
        dual_player = NULL;
        free(dual_buf);
        dual_buf = NULL;
        return false;
    }
    dual_thread_started = true;
    commons_log_info("CoopDual", "second decoder open (%dx%d); each host has its own decoder and Surface",
                     info->width, info->height);
    return true;
}

void coop_dual_stop(void) {
    if (!atomic_load(&dual_running)) {
        return;
    }
    coop_coord_set_wakeup(NULL);
    atomic_store(&dual_stop_req, true);
    pthread_mutex_lock(&dual_lock);
    dual_kick = true;
    pthread_cond_signal(&dual_cond);
    pthread_mutex_unlock(&dual_lock);
    if (dual_thread_started) {
        pthread_join(dual_thread, NULL);
        dual_thread_started = false;
    }
    if (dual_player != NULL) {
        SS4S_PlayerVideoClose(dual_player);
        SS4S_PlayerClose(dual_player);
        dual_player = NULL;
    }
    free(dual_buf);
    dual_buf = NULL;
    atomic_store(&dual_started, false);
    atomic_store(&dual_running, false);
    commons_log_info("CoopDual", "second decoder closed (fed=%u holes=%u rejects=%u)", dual_fed, dual_holes,
                     dual_rejects);
}

bool coop_dual_running(void) {
    return atomic_load(&dual_running);
}

bool coop_dual_peer_started(void) {
    return atomic_load(&dual_started);
}

#endif /* __ANDROID__ */
