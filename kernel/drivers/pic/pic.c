/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/pic/pic.h"
#include "asm/io.h"
#include <stdint.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA 0xA1

void pic_remap(uint8_t offset1, uint8_t offset2)
{
    // Save masks (not needed for remap, but we'll just clear them)
    // Start initialization in cascade mode
    outb(PIC1_COMMAND, 0x11);
    outb(PIC2_COMMAND, 0x11);
    // Set new offsets
    outb(PIC1_DATA, offset1);
    outb(PIC2_DATA, offset2);
    // Tell master that there is a slave at IRQ2 (0000 0100)
    outb(PIC1_DATA, 0x04);
    // Tell slave its cascade identity (0000 0010)
    outb(PIC2_DATA, 0x02);
    // Set 8086 mode
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    // Unmask all interrupts (optional, but enables everything)
    outb(PIC1_DATA, 0x00);
    outb(PIC2_DATA, 0x00);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
    {
        outb(PIC2_COMMAND, 0x20);
    }
    outb(PIC1_COMMAND, 0x20);
}