/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/ui/terminal.h"
#include "kernel/ui/fb.h"
#include <stdint.h>

static char term_buf[TERMINAL_COLS * TERMINAL_ROWS];
static uint32_t term_fg[TERMINAL_COLS * TERMINAL_ROWS];
static uint32_t cursor_x = 0, cursor_y = 0;
static uint32_t fg_default = TERMINAL_WHITE;
static uint32_t bg_default = TERMINAL_BLACK;
static volatile int terminal_dirty = 1; // start dirty

void terminal_init(void)
{
    for (int i = 0; i < TERMINAL_COLS * TERMINAL_ROWS; i++)
    {
        term_buf[i] = ' ';
        term_fg[i] = fg_default;
    }
    cursor_x = 0;
    cursor_y = 0;
    terminal_dirty = 1;
}

void terminal_putchar(char c, uint32_t attr)
{
    if (c == '\n')
    {
        cursor_x = 0;
        cursor_y++;
    }
    else if (c == '\r')
    {
        cursor_x = 0;
    }
    else if (c == '\b')
    {
        if (cursor_x > 0)
        {
            cursor_x--;
            term_buf[cursor_y * TERMINAL_COLS + cursor_x] = ' ';
        }
    }
    else
    {
        uint32_t idx = cursor_y * TERMINAL_COLS + cursor_x;
        if (idx < TERMINAL_COLS * TERMINAL_ROWS)
        {
            term_buf[idx] = c;
            term_fg[idx] = attr;
        }
        cursor_x++;
        if (cursor_x >= TERMINAL_COLS)
        {
            cursor_x = 0;
            cursor_y++;
        }
    }
    if (cursor_y >= TERMINAL_ROWS)
    {
        // scroll
        for (int y = 1; y < TERMINAL_ROWS; y++)
        {
            for (int x = 0; x < TERMINAL_COLS; x++)
            {
                uint32_t src = y * TERMINAL_COLS + x;
                uint32_t dst = (y - 1) * TERMINAL_COLS + x;
                term_buf[dst] = term_buf[src];
                term_fg[dst] = term_fg[src];
            }
        }
        for (int x = 0; x < TERMINAL_COLS; x++)
        {
            uint32_t last = (TERMINAL_ROWS - 1) * TERMINAL_COLS + x;
            term_buf[last] = ' ';
            term_fg[last] = fg_default;
        }
        cursor_y = TERMINAL_ROWS - 1;
    }
    terminal_dirty = 1;
}

void terminal_puts(const char *s, uint32_t attr)
{
    while (*s)
    {
        terminal_putchar(*s++, attr);
    }
}

void terminal_clear(void)
{
    for (int i = 0; i < TERMINAL_COLS * TERMINAL_ROWS; i++)
    {
        term_buf[i] = ' ';
        term_fg[i] = fg_default;
    }
    cursor_x = 0;
    cursor_y = 0;
    terminal_dirty = 1;
}

void terminal_set_cursor(uint8_t x, uint8_t y)
{
    cursor_x = x;
    cursor_y = y;
}

void terminal_get_cursor(uint8_t *x, uint8_t *y)
{
    *x = cursor_x;
    *y = cursor_y;
}

void terminal_write_at(uint8_t x, uint8_t y, char c, uint32_t attr)
{
    if (x < TERMINAL_COLS && y < TERMINAL_ROWS)
    {
        uint32_t idx = y * TERMINAL_COLS + x;
        term_buf[idx] = c;
        term_fg[idx] = attr;
        terminal_dirty = 1;
    }
}

void terminal_scroll_reset(void)
{
    // no-op - see mouse.h's own note on why: this used to be driven by
    // the mouse wheel via a shell/vga.h that no longer exists in this
    // tree, and there is currently no real scrollback buffer here to
    // reset in the first place.
}

void terminal_render(void)
{
    for (int y = 0; y < TERMINAL_ROWS; y++)
    {
        for (int x = 0; x < TERMINAL_COLS; x++)
        {
            uint32_t idx = y * TERMINAL_COLS + x;
            char c = term_buf[idx];
            uint32_t fg = term_fg[idx];
            fb_draw_char(x * 8, y * 16, c, fg, bg_default);
        }
    }
    // cursor
    fb_fillrect(cursor_x * 8, cursor_y * 16, 8, 16, fg_default);
    fb_swap();
}

void terminal_update(void)
{
    if (terminal_dirty)
    {
        terminal_render();
        terminal_dirty = 0;
    }
}