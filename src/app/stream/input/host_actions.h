#pragma once

#include <stdbool.h>

/*
 * Things the radial menu asks a HOST PC to do, beyond plain input.
 *
 * Steam Big Picture is opened/closed by typing the steam:// URL into the Windows Run
 * dialog: Win+R, the URL as UTF-8 text (layout-independent, the same text channel the PIN
 * auto-entry uses), Enter. Needs a Windows host with Steam installed; nothing here can see
 * whether it worked.
 *
 * In co-op the controller's assignment decides which PC gets it (first, second or both),
 * like the controller's virtual mouse. The open/closed state is only what WE last sent, per
 * host, kept for the app's lifetime (Big Picture stays open across a reconnect): closing it
 * on the PC itself is not noticed, the next choice then sends "close" once more.
 */

enum {
    HOST_ACTION_NONE = 0,
    HOST_ACTION_BIGPICTURE_OPEN = 1,
    HOST_ACTION_BIGPICTURE_CLOSE = 2,
};

/** Did we last open Big Picture on the PC(s) controller @p pad is assigned to? */
bool host_action_bigpicture_is_open(int pad);

/** Open Big Picture on that PC, or close it if we opened it before. Never blocks. */
void host_action_bigpicture_toggle(int pad);

/** Run @p action on THIS process's connection (also called by the co-op secondary).
 *  Runs on its own thread because the Run dialog needs a moment to appear. */
void host_action_run_local(int action);
