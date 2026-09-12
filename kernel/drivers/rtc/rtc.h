/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef RTC_H
#define RTC_H

/* Formats the current CMOS RTC time as "HH:MM:SS\0" (24-hour) into buf,
   which must be at least 9 bytes. Reads the RTC directly on demand --
   no timer interrupt or tick counter involved, the chip already keeps
   wall-clock time on its own. */
void rtc_format_time(char *buf);

#endif