/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/ui/window.h"
#include "kernel/ui/fb.h"
#include "kernel/proc/task.h"

static window_t windows[WM_MAX_WINDOWS];
static bsp_tree_t wm_tree;
static int focused_window_id = -1;

#define WM_BORDER_PX        2
#define WM_BORDER_FOCUSED   0xFFFFFFu
#define WM_BORDER_UNFOCUSED 0x444444u

void wm_init(void)
{
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
    {
        windows[i].in_use = 0;
    }
    bsp_tree_init(&wm_tree);
    focused_window_id = -1;
}

int wm_open_window(wm_entry_t entry, const char *name)
{
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
    {
        if (!windows[i].in_use)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return -1;
    }

    int window_id = slot; /* window_id doubles as this window's own pool index */

    bsp_rect_t full_screen = {0, 0, (int)fb_info.width, (int)fb_info.height};
    const bsp_rect_t *split_rect = &full_screen;
    int target_leaf = -1;

    if (focused_window_id != -1 && windows[focused_window_id].in_use)
    {
        target_leaf = bsp_find(&wm_tree, focused_window_id);
        split_rect = &windows[focused_window_id].rect;
    }

    int leaf = bsp_insert_auto(&wm_tree, target_leaf, split_rect, window_id);
    if (leaf < 0)
    {
        return -1;
    }

    windows[slot].in_use = 1;
    windows[slot].window_id = window_id;
    windows[slot].task_id = -1;
    int i = 0;
    while (name[i] && i < 15)
    {
        windows[slot].name[i] = name[i];
        i++;
    }
    windows[slot].name[i] = '\0';

    /* wm_entry_t and task_entry_t are both void(*)(void*) - no cast needed,
       they're structurally identical, just named for their own contexts. */
    if (entry != 0)
    {
        int task_id = task_create(entry, &windows[slot], name);
        if (task_id < 0)
        {
            bsp_remove(&wm_tree, leaf);
            windows[slot].in_use = 0;
            return -1;
        }
        windows[slot].task_id = task_id;
    }
    focused_window_id = window_id;

    wm_relayout();
    return window_id;
}

int wm_close_window(int window_id)
{
    if (window_id < 0 || window_id >= WM_MAX_WINDOWS || !windows[window_id].in_use)
    {
        return -1;
    }

    int leaf = bsp_find(&wm_tree, window_id);
    if (leaf != -1)
    {
        bsp_remove(&wm_tree, leaf);
    }
    windows[window_id].in_use = 0;

    if (focused_window_id == window_id)
    {
        focused_window_id = -1;
        for (int i = 0; i < WM_MAX_WINDOWS; i++)
        {
            if (windows[i].in_use)
            {
                focused_window_id = windows[i].window_id;
                break;
            }
        }
    }

    wm_relayout();
    return 0;
}

static void relayout_visit(void *user_data, int leaf, int window_id, const bsp_rect_t *rect)
{
    (void)user_data;
    (void)leaf;
    if (window_id < 0 || window_id >= WM_MAX_WINDOWS || !windows[window_id].in_use)
    {
        return;
    }
    windows[window_id].rect = *rect;
}

void wm_relayout(void)
{
    bsp_rect_t screen = {0, 0, (int)fb_info.width, (int)fb_info.height};
    bsp_layout(&wm_tree, &screen, relayout_visit, 0);

    /* Known limitation: if a window task is preempted mid-draw using its
       OLD rect at the exact moment this function moves rects around (open
       /close/resize happening concurrently with another window's own
       drawing), that task can briefly paint into what is now a different
       window's territory - a one-frame visual glitch, not memory
       corruption, since every window's writes still go through win_* and
       stay inside SOME valid rect at all times. Worth revisiting once
       there's a reason to (e.g. real interactive resize), not needed for
       this first working version. */
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
    {
        if (!windows[i].in_use)
        {
            continue;
        }
        uint32_t c = (windows[i].window_id == focused_window_id)
                         ? WM_BORDER_FOCUSED
                         : WM_BORDER_UNFOCUSED;
        bsp_rect_t r = windows[i].rect;
        fb_fillrect(r.x, r.y, r.w, WM_BORDER_PX, c);                          /* top */
        fb_fillrect(r.x, r.y + r.h - WM_BORDER_PX, r.w, WM_BORDER_PX, c);     /* bottom */
        fb_fillrect(r.x, r.y, WM_BORDER_PX, r.h, c);                          /* left */
        fb_fillrect(r.x + r.w - WM_BORDER_PX, r.y, WM_BORDER_PX, r.h, c);     /* right */
    }
}

void wm_present(void)
{
    fb_swap();
}

/* ---- window-local drawing: offset + clipped to win's current rect ---- */

void win_fillrect(window_t *win, int x, int y, int w, int h, uint32_t color)
{
    if (!win)
    {
        return;
    }
    int x0 = win->rect.x + x;
    int y0 = win->rect.y + y;
    int x1 = x0 + w;
    int y1 = y0 + h;
    int cx0 = win->rect.x, cy0 = win->rect.y;
    int cx1 = win->rect.x + win->rect.w, cy1 = win->rect.y + win->rect.h;

    if (x0 < cx0)
    {
        x0 = cx0;
    }
    if (y0 < cy0)
    {
        y0 = cy0;
    }
    if (x1 > cx1)
    {
        x1 = cx1;
    }
    if (y1 > cy1)
    {
        y1 = cy1;
    }
    if (x1 <= x0 || y1 <= y0)
    {
        return;
    }

    fb_fillrect((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), color);
}

void win_putpixel(window_t *win, int x, int y, uint32_t color)
{
    if (!win)
    {
        return;
    }
    int sx = win->rect.x + x;
    int sy = win->rect.y + y;
    if (sx < win->rect.x || sy < win->rect.y ||
        sx >= win->rect.x + win->rect.w || sy >= win->rect.y + win->rect.h)
    {
        return;
    }
    fb_putpixel((uint32_t)sx, (uint32_t)sy, color);
}

void win_clear(window_t *win, uint32_t color)
{
    if (!win)
    {
        return;
    }
    win_fillrect(win, 0, 0, win->rect.w, win->rect.h, color);
}

void win_draw_string(window_t *win, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    if (!win)
    {
        return;
    }
    int px = win->rect.x + x;
    int py = win->rect.y + y;
    int cx0 = win->rect.x, cy0 = win->rect.y;
    int cx1 = win->rect.x + win->rect.w, cy1 = win->rect.y + win->rect.h;

    while (*s)
    {
        /* Skip (don't partially draw) any glyph that wouldn't fully fit -
           simple and good enough here; partial-glyph clipping isn't worth
           the complexity for a first version. */
        if (px >= cx0 && py >= cy0 && px + 8 <= cx1 && py + 16 <= cy1)
        {
            fb_draw_char((uint32_t)px, (uint32_t)py, *s, fg, bg);
        }
        px += 8;
        s++;
    }
}

/* ---- window_id-based wrappers, for userland syscalls ----
   A userland process should only ever hold a plain integer handle, never
   a kernel pointer - these look the window up by id (window_id doubles as
   its own pool index, see wm_open_window) and no-op safely if it isn't
   currently open, rather than trusting whatever the caller passed in. */

static window_t *window_by_id(int window_id)
{
    if (window_id < 0 || window_id >= WM_MAX_WINDOWS || !windows[window_id].in_use)
    {
        return 0;
    }
    return &windows[window_id];
}

void win_fillrect_id(int window_id, int x, int y, int w, int h, uint32_t color)
{
    win_fillrect(window_by_id(window_id), x, y, w, h, color);
}

void win_putpixel_id(int window_id, int x, int y, uint32_t color)
{
    win_putpixel(window_by_id(window_id), x, y, color);
}

void win_clear_id(int window_id, uint32_t color)
{
    win_clear(window_by_id(window_id), color);
}

void win_draw_string_id(int window_id, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    win_draw_string(window_by_id(window_id), x, y, s, fg, bg);
}