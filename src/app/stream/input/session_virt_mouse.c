#include "session_virt_mouse.h"
#include "session_input.h"
#include "coop_input.h"
#include "app_settings.h"

#include <SDL_stdinc.h>

static short calc_mouse_movement(short axis);

static Uint32 vmouse_timer_callback(Uint32 interval, void *param);

/* ---- where a pad's mouse goes ---------------------------------------------------------
 * Outside co-op, or while no second PC is attached, everything goes to our own connection
 * exactly as before. In co-op the pad's assignment decides: the first PC, the second PC
 * (over the co-op input link), or both. */
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

static void send_move(int pad, short dx, short dy) {
    if (route_primary(pad)) {
        LiSendMouseMoveEvent(dx, dy);
    }
    if (route_peer(pad)) {
        coop_input_send_mouse_move(dx, dy);
    }
}

static void send_scroll(int pad, short amount) {
    if (route_primary(pad)) {
        LiSendHighResScrollEvent(amount);
    }
    if (route_peer(pad)) {
        coop_input_send_mouse_scroll_hr(amount);
    }
}

static void send_button(int pad, int action, int button) {
    if (route_primary(pad)) {
        LiSendMouseButtonEvent((char) action, button);
    }
    if (route_peer(pad)) {
        coop_input_send_mouse_button(action, button);
    }
}

static session_input_vmouse_pad_t *pad_state(session_input_vmouse_t *vmouse, int pad) {
    if (pad < 0 || pad >= VMOUSE_PADS) {
        pad = 0;
    }
    session_input_vmouse_pad_t *p = &vmouse->pads[pad];
    p->owner = vmouse;
    p->pad = pad;
    return p;
}

void session_input_set_vmouse_active(session_input_vmouse_t *vmouse, bool active) {
    vmouse->active = active;
    if (!active) {
        for (int i = 0; i < VMOUSE_PADS; ++i) {
            vmouse_set_vector(vmouse, i, 0, 0);
            vmouse_set_trigger(vmouse, i, 0, 0);
        }
    }
}

bool session_input_is_vmouse_active(session_input_vmouse_t *vmouse) {
    return vmouse->active;
}

void vmouse_set_vector(session_input_vmouse_t *vmouse, int pad, short x, short y) {
    session_input_vmouse_pad_t *p = pad_state(vmouse, pad);
    p->x = calc_mouse_movement(x);
    p->y = calc_mouse_movement((short) -SDL_max(y, -32767));
    if (p->x || p->y) {
        if (!p->timer_id) {
            p->timer_id = SDL_AddTimer(0, vmouse_timer_callback, p);
        }
    } else if (p->timer_id) {
        SDL_RemoveTimer(p->timer_id);
        p->timer_id = 0;
    }
}

void vmouse_set_trigger(session_input_vmouse_t *vmouse, int pad, char l, char r) {
    session_input_vmouse_pad_t *p = pad_state(vmouse, pad);
    const char trigger_threshold = 64;
    bool ldown = l > trigger_threshold, rdown = r > trigger_threshold;
    if (p->l != ldown) {
        send_button(p->pad, ldown ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    }
    if (p->r != rdown) {
        send_button(p->pad, rdown ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    }
    p->l = ldown;
    p->r = rdown;
}

void vmouse_set_modifier(session_input_vmouse_t *vmouse, int pad, bool v) {
    pad_state(vmouse, pad)->modifier = v;
}

static Uint32 vmouse_timer_callback(Uint32 interval, void *param) {
    (void) interval;
    session_input_vmouse_pad_t *p = param;
    if (!p->owner->active || (!p->x && !p->y)) {
        return 0;
    }
    short speed = 4;
    double speed_divider = 32 - SDL_max(0, SDL_min(speed, 16));
    double x = p->x / speed_divider;
    double y = p->y / speed_divider;
    double abs_x = SDL_fabs(x), abs_y = SDL_fabs(y);
    if (p->modifier) {
        abs_y /= 20.0;
        send_scroll(p->pad, (short) (abs_y > 1 ? -y : -y / abs_y));
    } else {
        send_move(p->pad, (short) (abs_x > 1 ? x : x / abs_x), (short) (abs_y > 1 ? y : y / abs_y));
    }
    return SDL_max(5, SDL_min(5 / SDL_max(abs_x, abs_y), 20));
}

static short calc_mouse_movement(short axis) {
    short abs_axis = (short) (axis > 0 ? axis : -axis);
    short threshold = 4096;
    if (abs_axis < threshold) { return 0; }
    return (short) (SDL_sqrt(abs_axis - threshold) * (axis > 0 ? 1 : -1));
}
