#include "host_actions.h"
#include "coop_input.h"
#include "app_settings.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <Limelight.h>

#include "logging.h"

#define VK_LWIN 0x5B
#define VK_R 0x52
#define VK_RETURN 0x0D

/* Run dialog: how long it takes to appear and take focus before we type into it. */
#define RUN_DIALOG_WAIT_MS 700
#define TEXT_SETTLE_MS 150

/* [0] = our own connection (first PC), [1] = the co-op peer (second PC). */
static atomic_bool g_bp_open[2];

static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long) (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void tap(short vk, char modifiers) {
    LiSendKeyboardEvent(vk, KEY_ACTION_DOWN, modifiers);
    LiSendKeyboardEvent(vk, KEY_ACTION_UP, modifiers);
}

static void *action_thread(void *arg) {
    const int action = (int) (intptr_t) arg;
    const char *url = action == HOST_ACTION_BIGPICTURE_OPEN ? "steam://open/bigpicture"
                                                             : "steam://close/bigpicture";
    LiSendKeyboardEvent(VK_LWIN, KEY_ACTION_DOWN, MODIFIER_META);
    tap(VK_R, MODIFIER_META);
    LiSendKeyboardEvent(VK_LWIN, KEY_ACTION_UP, 0);
    sleep_ms(RUN_DIALOG_WAIT_MS);
    LiSendUtf8TextEvent(url, (unsigned int) strlen(url));
    sleep_ms(TEXT_SETTLE_MS);
    tap(VK_RETURN, 0);
    commons_log_info("HostAction", "sent %s", url);
    return NULL;
}

void host_action_run_local(int action) {
    if (action != HOST_ACTION_BIGPICTURE_OPEN && action != HOST_ACTION_BIGPICTURE_CLOSE) {
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, action_thread, (void *) (intptr_t) action) == 0) {
        pthread_detach(thread);
    }
}

/* Same routing as the controller's virtual mouse (session_virt_mouse.c). */
static bool route_primary(int pad) {
    if (!coop_input_coord_active() || !coop_input_peer_attached()) {
        return true;
    }
    return coop_input_wants_primary(pad);
}

static bool route_peer(int pad) {
    if (!coop_input_coord_active() || !coop_input_peer_attached()) {
        return false;
    }
    return coop_input_route(pad) != COOP_PAD_PRIMARY;
}

bool host_action_bigpicture_is_open(int pad) {
    if (route_primary(pad) && atomic_load(&g_bp_open[0])) {
        return true;
    }
    return route_peer(pad) && atomic_load(&g_bp_open[1]);
}

void host_action_bigpicture_toggle(int pad) {
    /* One decision for every PC the pad reaches, so a pad on BOTH never ends up opening
     * on one and closing on the other. */
    const bool open = !host_action_bigpicture_is_open(pad);
    const int action = open ? HOST_ACTION_BIGPICTURE_OPEN : HOST_ACTION_BIGPICTURE_CLOSE;
    if (route_primary(pad)) {
        atomic_store(&g_bp_open[0], open);
        host_action_run_local(action);
    }
    if (route_peer(pad)) {
        atomic_store(&g_bp_open[1], open);
        coop_input_send_host_action(action);
    }
}
