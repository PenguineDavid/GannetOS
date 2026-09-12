/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef FB_H
#define FB_H

#include <stdint.h>

typedef struct
{
    uint32_t phys_base;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint32_t red_mask_size;
    uint32_t red_field_pos;
    uint32_t green_mask_size;
    uint32_t green_field_pos;
    uint32_t blue_mask_size;
    uint32_t blue_field_pos;
} fb_info_t;

extern fb_info_t fb_info;

void fb_init(void);
void fb_putpixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fillrect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);
void fb_swap(void);

// Draw a character using the embedded 8x16 font
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);

#endif