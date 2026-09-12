/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/arch/x86/idt.h"
#include <stdint.h>

#define IDT_ENTRIES 256

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idt_p = {0, 0};

/* Default handler for any unhandled interrupt -- just irets */
extern void isr_default();
/* For the handful of exceptions that push a CPU error code -- discards
   it before returning, see shell/isr_wrappers.s for why this matters. */
extern void isr_err_stub();

void idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags)
{
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].selector = selector;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

void idt_init(void)
{
    /* Zero the IDT explicitly */
    uint8_t *idt_bytes = (uint8_t *)idt;
    for (int i = 0; i < (int)(sizeof(struct idt_entry) * IDT_ENTRIES); i++)
    {
        idt_bytes[i] = 0;
    }

    idt_p.limit = (uint16_t)(sizeof(struct idt_entry) * IDT_ENTRIES - 1);
    idt_p.base = (uint32_t)&idt[0];

    /* Install a safe default handler on every vector so an unexpected
       interrupt doesn't hit a not-present gate and triple fault */
    for (int i = 0; i < IDT_ENTRIES; i++)
    {
        idt_set_gate((uint8_t)i, (uint32_t)isr_default, 0x08, 0x8E);
    }

    /* #DF #TS #NP #SS #GP #PF #AC push a CPU error code onto the stack
       before entry. isr_default does not account for that extra 4 bytes,
       which desyncs iret's return frame and cascades into a double fault
       then a triple fault the moment any one of these fires. Route them
       to the stub that discards the error code first. */
    static const uint8_t error_code_vectors[] = {8, 10, 11, 12, 13, 14, 17};
    for (unsigned i = 0; i < sizeof(error_code_vectors); i++)
    {
        idt_set_gate(error_code_vectors[i], (uint32_t)isr_err_stub, 0x08, 0x8E);
    }

    asm volatile("lidt %0" : : "m"(idt_p));
}