/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/pit/pit.h"
#include "asm/io.h"
#include "kernel/arch/x86/idt.h"
#include "kernel/proc/task.h"
#include <stdint.h>

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ_HZ  1193182u
#define IRQ0              0x20

static volatile uint32_t ticks = 0;

/* Assembly stub in shell/isr_wrappers.s: sends EOI, calls pit_handler(),
   then irets. See the comment there for why EOI happens before the call. */
extern void pit_isr(void);

void pit_init(uint32_t frequency_hz)
{
    if (frequency_hz == 0)
    {
        frequency_hz = 100;
    }

    uint32_t divisor = PIT_BASE_FREQ_HZ / frequency_hz;
    if (divisor == 0)
    {
        divisor = 1;
    }
    if (divisor > 0xFFFF)
    {
        divisor = 0xFFFF;
    }

    // Channel 0, lobyte/hibyte access, mode 3 (square wave), binary mode
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    idt_set_gate(IRQ0, (uint32_t)pit_isr, 0x08, 0x8E);
}

uint32_t pit_ticks(void)
{
    return ticks;
}

/* Called by pit_isr on every timer interrupt, after EOI has already been
   sent. Advances the tick count and hands off to the scheduler, which may
   context-switch to a different task and not return here until this exact
   task is scheduled again. */
void pit_handler(void)
{
    ticks++;
    task_tick();
}