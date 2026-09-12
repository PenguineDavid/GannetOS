/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Selector layout. Kernel code/data KEEP the same values boot.asm's own
   minimal GDT already used (0x08/0x10) so nothing else in the codebase -
   idt.c's gate installs, isr_wrappers.s, task.c - needs to change when
   gdt_init() replaces that minimal GDT with this real one. User code/data
   and the TSS are new. The `| 3` on the user selectors sets RPL=3 in the
   selector itself, which is what actually matters for the privilege
   check on a far jump/iret into ring3 - the descriptor's own DPL=3
   (baked into the access byte in gdt.c) is what makes the segment
   USABLE from ring3 at all, but the selector's RPL is what the CPU
   compares against CS.RPL/CPL at the point of use. */
#define GDT_KERNEL_CODE_SEL 0x08
#define GDT_KERNEL_DATA_SEL 0x10
#define GDT_USER_CODE_SEL   (0x18 | 3)
#define GDT_USER_DATA_SEL   (0x20 | 3)
#define GDT_TSS_SEL         0x28
#define GDT_DF_TSS_SEL      0x30

struct gdt_entry
{
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity; /* high nibble = flags (G, D/B, ...), low nibble = limit bits 16-19 */
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr
{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Standard 32-bit TSS. GannetOS never does a hardware task-switch (no far
   jump/call through the TSS descriptor), so almost every field here is
   dead weight the CPU never reads - the ONE field that matters is esp0/
   ss0: on any ring3 -> ring0 transition (interrupt or exception while a
   user-mode task is running), the CPU loads esp from tss.esp0 and ss from
   tss.ss0 itself, before pushing the interrupt frame. Without a valid
   esp0 pointing at a real kernel stack, that push happens on whatever
   ring3's ESP currently is - which is untrusted, possibly unmapped for
   ring0, and the first stray byte of kernel state landing in ring3-
   readable memory (or a fault on an already-faulting path) triple-faults
   the machine. esp0 must be updated on every task switch, same spirit as
   task.c already re-pointing CR3 - see tss_set_kernel_stack(). */
struct tss_entry
{
    uint32_t prev_tss;
    uint32_t esp0;
    uint32_t ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* Builds the real GDT (null, kernel code/data, user code/data, TSS),
   loads it with LGDT, reloads every segment register (far jump for CS,
   direct loads for the rest) and loads the task register with LTR so the
   CPU knows where the TSS lives. Must run after nothing that depends on
   the OLD (boot.asm) GDT still being current - in practice, call this
   first thing in kernel_main, before idt_init(). */
void gdt_init(void);

/* Updates the TSS's esp0 field - the kernel-mode stack the CPU will
   switch to the next time THIS task takes a ring3->ring0 transition.
   Called from the scheduler on every switch to a ring3 task, same as
   paging_switch_directory() is called for CR3. Safe to call even before
   any ring3 task exists (harmless, just unused). */
void tss_set_kernel_stack(uint32_t esp0);

/* Sets up a SECOND, completely independent TSS and installs it as a
   hardware TASK GATE for vector 8 (#DF, double fault) in the IDT -
   see gdt.c for the full reasoning. Unlike the main TSS above, a
   task-gate switch reloads EVERY field from this TSS unconditionally
   (CS, EIP, ESP, SS, the other segments, EFLAGS, CR3) - it doesn't
   trust anything about whatever context was interrupted, which is
   exactly what's needed here: a double fault is specifically what
   happens when the CPU couldn't even push a normal exception frame on
   the current stack (e.g. a stack overflow whose ESP has already
   wandered into an unmapped guard page), so "the current stack" can't
   be trusted at all by the time this matters.
   Must run after gdt_init() (extends the same GDT) and after
   paging_init() (needs a real, valid page directory to put in the new
   TSS's cr3 field - pass paging_kernel_directory_phys()). */
void df_handler_init(uint32_t kernel_cr3);

#endif