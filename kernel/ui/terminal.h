/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

#define TERMINAL_COLS 80
#define TERMINAL_ROWS 40

// Colour values (24-bit RGB)
#define TERMINAL_BLACK       0x000000
#define TERMINAL_WHITE       0xFFFFFF
#define TERMINAL_LIGHT_GREY  0xC0C0C0
#define TERMINAL_LIGHT_RED   0xFF6666
#define TERMINAL_LIGHT_GREEN 0x66FF66
#define TERMINAL_LIGHT_BLUE  0x6666FF
#define TERMINAL_LIGHT_CYAN  0x66FFFF
#define TERMINAL_YELLOW      0xFFFF66
#define TERMINAL_BLUE        0x0000FF
#define TERMINAL_GREEN       0x00FF00
#define TERMINAL_RED         0xFF0000
#define TERMINAL_CYAN        0x00FFFF
#define TERMINAL_MAGENTA     0xFF00FF
#define TERMINAL_BROWN       0xAA5500
#define TERMINAL_DARK_GREY   0x808080

// For compatibility with old VGA attribute style, we define a macro
// that just returns the foreground colour (ignore background for now).
// We can later extend to support background.
#define TERMINAL_ATTR(fg, bg) ((uint32_t)(fg))

void terminal_init(void);
void terminal_putchar(char c, uint32_t attr);
void terminal_puts(const char *s, uint32_t attr);
void terminal_clear(void);
void terminal_set_cursor(uint8_t x, uint8_t y);
void terminal_get_cursor(uint8_t *x, uint8_t *y);
void terminal_write_at(uint8_t x, uint8_t y, char c, uint32_t attr);
void terminal_scroll_reset(void);   // no-op for now
void terminal_render(void);        // redraw the whole terminal to framebuffer
void terminal_update(void);        // redraw only when the terminal is dirty

#endif