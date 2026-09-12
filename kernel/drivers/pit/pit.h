/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PIT_H
#define PIT_H

#include <stdint.h>

// Programs PIT channel 0 to fire IRQ0 at approximately frequency_hz and
// installs its IDT gate. Also enables the scheduler tick: from this point
// on, every IRQ0 will call task_tick() (see task.h) after sending EOI.
void pit_init(uint32_t frequency_hz);

// Number of timer ticks since pit_init() was called. Wraps at UINT32_MAX.
uint32_t pit_ticks(void);

#endif