#pragma once

#include <stdbool.h>

/*
 * Controller radial menu: hold Start for a few seconds during a stream and a small
 * four-way menu appears while the button stays down. The left stick picks an entry, letting
 * go of Start confirms it; letting go with the stick centred cancels. Nothing is sent to the
 * host while it is open.
 *
 * Threads: the SELECTION and the OPEN FLAG are atomics written by the input thread
 * (session_gamepad.c); everything that touches LVGL runs on the UI thread, driven by the
 * USER_RADIAL_* bus events handled in streaming.controller.c.
 */

enum {
    RADIAL_NONE = -1,
    RADIAL_MOUSE = 0,      /* up */
    RADIAL_KEYBOARD = 1,   /* right */
    RADIAL_END_STREAM = 2, /* down */
    RADIAL_BIGPICTURE = 3, /* left: Steam Big Picture open/close (stream/input/host_actions.h) */
};

/** Input thread: is the menu open (so the pad that opened it must not reach the host)? */
bool radial_menu_is_open(void);
int radial_menu_owner_pad(void);

/** Input thread: mark open/closed and set the highlighted entry. Closing keeps the owner
 *  pad readable (radial_menu_last_pad) so the UI thread knows whose choice it was. */
void radial_menu_set_open(bool open, int pad);
int radial_menu_last_pad(void);
void radial_menu_set_selection(int sector);
int radial_menu_selection(void);

/** UI thread. */
void radial_menu_show(bool mouse_on, bool bigpicture_open);
void radial_menu_refresh(void);
void radial_menu_hide(void);
