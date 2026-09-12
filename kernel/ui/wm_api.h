/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef WM_API_H
#define WM_API_H

#include <stdint.h>

/* Syscall table for a userland window manager .pexe. This is deliberately
   separate from pexe.h's app_api_t (a normal app's syscalls) - a WM needs
   a different, graphics/windowing-shaped surface, and keeping them apart
   means neither has to carry fields it doesn't need.

   A window is always addressed by a plain integer window_id here - never
   a kernel pointer. See kernel/window.c's win_*_id() wrappers.

   CURRENT LIMITATION (by design, not an oversight): opening a window from
   userland does NOT give it its own independently scheduled task the way
   the kernel-side compositor demo could. GannetOS only supports one resident
   app at a time (a single shared exec buffer), so there's nowhere for a
   second concurrent piece of userland code to live yet - that needs
   per-task memory isolation first. For now, a WM opens windows and is
   itself responsible for drawing into all of them from its own single
   running task, whenever and however often it likes. */
typedef struct
{
    /* ---- compositor lifecycle ---- */
    void (*wm_init)(void);
    int  (*wm_open_window)(const char *name);           /* returns window_id, or -1 */
    int  (*wm_close_window)(int window_id);              /* 0 on success, -1 if not open */
    void (*wm_relayout)(void);                            /* call after opening/closing */
    void (*wm_present)(void);                             /* flips the accumulated frame to the display */

    /* ---- per-window drawing: offset + clipped to that window automatically ---- */
    void (*win_fillrect)(int window_id, int x, int y, int w, int h, uint32_t color);
    void (*win_putpixel)(int window_id, int x, int y, uint32_t color);
    void (*win_clear)(int window_id, uint32_t color);
    void (*win_draw_string)(int window_id, int x, int y, const char *s, uint32_t fg, uint32_t bg);

    /* ---- input ---- */
    int (*getchar_nonblocking)(void); /* -1 if nothing pending, never blocks */

    /* ---- control ----
       F12 is reserved at the kernel level as "stop the running WM and hand
       control back to the shell" - it's tracked regardless of what else is
       reading the keyboard. A WM's main loop should poll should_quit()
       every iteration and, once it returns true, tear down (or just stop
       drawing) and return from its own entry point - then call quit_ack()
       right before returning, so the flag doesn't leak into whatever runs
       next. */
    int  (*should_quit)(void);
    void (*quit_ack)(void);

    /* ---- screen info, filled in before entry() is called ---- */
    uint32_t screen_width;
    uint32_t screen_height;
} wm_api_t;

typedef int (*wm_app_entry_t)(int argc, char **argv, wm_api_t *api);

#endif