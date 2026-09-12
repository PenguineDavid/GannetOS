/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/rtc/rtc.h"
#include "asm/io.h"
#include <stdint.h>

#define CMOS_INDEX 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int rtc_update_in_progress(void)
{
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v)
{
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static void rtc_read_raw(uint8_t *hour, uint8_t *min, uint8_t *sec)
{
    uint8_t s, m, h, last_s, last_m, last_h;

    /* The RTC can be mid-update when read, which would return a garbled
       in-flux value. Standard technique: wait for any in-progress update
       to finish, read all three fields, then read them again -- if they
       don't match, an update happened in between and we retry. */
    do
    {
        while (rtc_update_in_progress())
        {
        }
        s = cmos_read(0x00);
        m = cmos_read(0x02);
        h = cmos_read(0x04);

        while (rtc_update_in_progress())
        {
        }
        last_s = cmos_read(0x00);
        last_m = cmos_read(0x02);
        last_h = cmos_read(0x04);
    } while (s != last_s || m != last_m || h != last_h);

    uint8_t status_b = cmos_read(0x0B);
    if (!(status_b & 0x04)) /* bit clear = values are in BCD, not binary */
    {
        s = bcd_to_bin(s);
        m = bcd_to_bin(m);
        h = (uint8_t)(bcd_to_bin(h & 0x7F) | (h & 0x80));
    }
    if (!(status_b & 0x02) && (h & 0x80)) /* 12-hour mode, PM flag set */
    {
        h = (uint8_t)(((h & 0x7F) + 12) % 24);
    }

    *hour = h;
    *min = m;
    *sec = s;
}

void rtc_format_time(char *buf)
{
    uint8_t h, m, s;
    rtc_read_raw(&h, &m, &s);
    buf[0] = (char)('0' + h / 10);
    buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + m / 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + s / 10);
    buf[7] = (char)('0' + s % 10);
    buf[8] = '\0';
}