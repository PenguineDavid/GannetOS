/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// Interrupt gate descriptor (64-bit)
struct idt_entry
{
    uint16_t base_low;
    uint16_t selector; // kernel code segment
    uint8_t zero;      // unused, set to 0
    uint8_t flags;     // type and attributes (present, ring0, gate type)
    uint16_t base_high;
} __attribute__((packed));

// Pointer structure for LIDT instruction
struct idt_ptr
{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

// Set an entry in the IDT
void idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags);

// Called once to install the IDT and load it with LIDT
void idt_init();

#endif