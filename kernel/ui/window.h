/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include "kernel/bsp.h"

/* One window per task, so this is capped the same as the task table -
   no point allowing more windows than could ever have a task behind them. */
#define WM_MAX_WINDOWS 8

typedef struct
{
    int in_use;
    int window_id;      // == this struct's own index in the window pool
    int task_id;
    bsp_rect_t rect;      // current on-screen rect; kept in sync by wm_relayout()
    char name[16];
} window_t;

/* Called with a pointer to its own window_t. Draw with win_* below (never
   fb_* directly) so drawing is automatically offset/clipped to whatever
   rect this window currently has - which can change under it at any time
   as other windows open/close/resize. Should never return (loop forever);
   if it does, the task is torn down same as calling task_exit(). */
typedef void (*wm_entry_t)(void *win);

/* Call once at startup, before opening any windows. */
void wm_init(void);

/* Opens a new window, tiled into the layout by auto-splitting the
   currently focused window (or filling the whole screen if this is the
   first window). The new window becomes focused. Returns the window_id,
   or -1 if the window pool is full.
   `entry`, if non-NULL, is run as the window's own independently
   scheduled task (see wm_entry_t above) - or -1 if that failed (window
   pool wasn't the problem, task creation was).
   Pass entry = NULL to open a window with no dedicated task: the caller
   (typically a userland WM running as a single task) is responsible for
   drawing into it themselves, via the win_* calls below, whenever they
   like. This is how userland window managers open windows today - giving
   each window its own independently scheduled task from userland is a
   later step that needs per-task memory isolation first.
   Calls wm_relayout() itself before returning, so the new layout is
   already computed and chrome drawn - only wm_present() still needs
   calling to actually flip it to the display. */
int wm_open_window(wm_entry_t entry, const char *name);

/* Removes a window from the layout and recomputes the rest.
   NOTE: GannetOS cannot forcibly kill another task yet, so this only tears
   down the WINDOW (bsp/layout/bookkeeping side) - the window's own task
   must call task_exit() itself, ideally before this is called for it.
   Returns 0 on success, -1 if window_id isn't currently open. */
int wm_close_window(int window_id);

/* Recomputes every open window's on-screen rect from the current BSP tree
   shape and (re)draws each window's border. Call after anything that
   changes the tree shape. Does NOT present to the display - call
   wm_present() (or let the next scheduled one happen) to actually see it. */
void wm_relayout(void);

/* Flips the accumulated framebuffer to the display. Decoupled from
   drawing on purpose: window tasks draw into the shared backbuffer
   whenever they get scheduled, and something in the main loop calls this
   periodically to present - same pattern as terminal_update()'s own
   dirty-triggered fb_swap(), just on a steady cadence instead of a dirty
   flag, since multiple independent tasks are drawing at once. */
void wm_present(void);

/* ---- window-local drawing: offset + clipped to `win`'s current rect ---- */
void win_fillrect(window_t *win, int x, int y, int w, int h, uint32_t color);
void win_putpixel(window_t *win, int x, int y, uint32_t color);
void win_clear(window_t *win, uint32_t color);
void win_draw_string(window_t *win, int x, int y, const char *s, uint32_t fg, uint32_t bg);

/* Same as above, but addressed by window_id instead of a raw window_t*.
   This is what userland WM syscalls are built on - a plain integer handle
   is all a userland process should ever hold, never a kernel pointer.
   Silently do nothing if window_id isn't currently open. */
void win_fillrect_id(int window_id, int x, int y, int w, int h, uint32_t color);
void win_putpixel_id(int window_id, int x, int y, uint32_t color);
void win_clear_id(int window_id, uint32_t color);
void win_draw_string_id(int window_id, int x, int y, const char *s, uint32_t fg, uint32_t bg);

#endif