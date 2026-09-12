/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* tiled - reference userland window manager for GannetOS.
 *
 * Demonstrates the wm_api_t syscall surface (see kernel/wm_api.h): opens
 * a couple of windows via the kernel's BSP compositor, draws into them
 * itself every loop iteration (there's no per-window task yet - see the
 * CURRENT LIMITATION note in wm_api.h), and quits cleanly when the
 * reserved F12 hotkey is pressed.
 *
 * This is meant as a working example for anyone writing their own WM,
 * not the only way to write one - wm_api_t is the whole contract; do
 * whatever you want with it.
 */
#include "kernel/ui/wm_api.h"

static void uint_to_str(unsigned value, char *out)
{
    char digits[12];
    int digit_count = 0;
    if (value == 0)
    {
        digits[digit_count++] = '0';
    }
    while (value > 0 && digit_count < 11)
    {
        digits[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    }

    int out_index = 0;
    while (digit_count > 0)
    {
        out[out_index++] = digits[--digit_count];
    }
    out[out_index] = '\0';
}

static void draw_window_frame(wm_api_t *api, int window, uint32_t bg, const char *name, unsigned counter)
{
    api->win_fillrect(window, 8, 28, 96, 16, bg); /* only the counter line needs re-clearing each frame */
    char counter_text[12];
    uint_to_str(counter, counter_text);
    api->win_draw_string(window, 8, 28, counter_text, 0xCCCCCC, bg);
    (void)name;
}

int app_main(int argc, char **argv, wm_api_t *api)
{
    (void)argc;
    (void)argv;

    api->wm_init();

    int window_a = api->wm_open_window("win0");
    int window_b = api->wm_open_window("win1");

    const uint32_t bg_a = 0x203050;
    const uint32_t bg_b = 0x502030;

    if (window_a >= 0)
    {
        api->win_clear(window_a, bg_a);
        api->win_draw_string(window_a, 8, 8, "win0", 0xFFFFFF, bg_a);
    }
    if (window_b >= 0)
    {
        api->win_clear(window_b, bg_b);
        api->win_draw_string(window_b, 8, 8, "win1", 0xFFFFFF, bg_b);
    }
    api->wm_present();

    unsigned counter = 0;
    while (!api->should_quit())
    {
        if (window_a >= 0)
        {
            draw_window_frame(api, window_a, bg_a, "win0", counter);
        }
        if (window_b >= 0)
        {
            draw_window_frame(api, window_b, bg_b, "win1", counter);
        }
        api->wm_present();

        counter++;

        /* Pace it so the counter is readable instead of spinning as fast
           as the CPU allows. */
        for (volatile int i = 0; i < 400000; i++)
        {
        }
    }

    api->quit_ack();
    return 0;
}