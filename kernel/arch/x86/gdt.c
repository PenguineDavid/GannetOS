/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/arch/x86/gdt.h"
#include "kernel/arch/x86/idt.h"

#define GDT_ENTRIES 7

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdt_p;
static struct tss_entry tss;
static struct tss_entry df_tss;

/* This task NEVER runs anything but df_fault_handler, so it needs only
   enough stack for that one small function's own frame plus whatever
   terminal_puts/print_hex32 use - 4KB is generous headroom, not a tight
   fit. Separate from every other stack in the system on purpose: the
   entire point of a task-gate double fault handler is that it doesn't
   trust ANY existing stack, including task_stacks - see df_handler_init
   and paging.c's df_fault_handler. */
static uint8_t df_stack[4096] __attribute__((aligned(16)));

extern void df_fault_handler(void); // kernel/paging.c

/* Implemented in shell/isr_wrappers.s.
   gdt_flush(ptr): LGDTs from *ptr, reloads DS/ES/FS/GS/SS with the kernel
   data selector, then far-jumps to reload CS with the kernel code
   selector (a plain mov can't reload CS - only a far jump/call/iret can,
   since CS's selector and its cached descriptor are only ever updated
   together via a control transfer).
   tss_flush(): LTRs the TSS selector so the CPU knows which descriptor
   describes the current TSS. */
extern void gdt_flush(uint32_t gdt_ptr_addr);
extern void tss_flush(void);

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran_flags)
{
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran_flags & 0xF0);
    gdt[num].access      = access;
}

static void write_tss(int num)
{
    uint32_t base  = (uint32_t)&tss;
    uint32_t limit = base + sizeof(tss);

    /* 0x89 = present, ring0, type 0x9 (32-bit TSS, "available" not "busy").
       0x00 granularity: TSS limit is a byte count, not page-granular, and
       we don't set the high nibble of the limit here since sizeof(tss)
       comfortably fits in the low 16 bits. */
    gdt_set_gate(num, base, limit, 0x89, 0x00);

    uint8_t *p = (uint8_t *)&tss;
    for (unsigned i = 0; i < sizeof(tss); i++) p[i] = 0;

    /* ss0 is fixed for the life of the OS - every ring0 stack is a normal
       kernel stack addressed via the flat kernel data segment. esp0 is
       NOT fixed; it must point at whichever task is about to run's own
       kernel stack, and gets updated by tss_set_kernel_stack() on every
       switch to a ring3 task. Left at 0 here until the first such switch
       sets a real value - harmless before then since nothing takes a
       ring3->ring0 transition yet. */
    tss.ss0 = GDT_KERNEL_DATA_SEL;
    tss.esp0 = 0;

    /* Everything below only matters if the CPU ever did a hardware task
       switch through this TSS (it doesn't, GannetOS only uses it for
       esp0/ss0) - set to sane flat-kernel values anyway so an accidental
       reference isn't reading zeroed segment selectors. */
    tss.cs = GDT_KERNEL_CODE_SEL;
    tss.ss = tss.ds = tss.es = tss.fs = tss.gs = GDT_KERNEL_DATA_SEL;

    /* No I/O permission bitmap; iomap_base == TSS limit means "bitmap
       starts past the end of the TSS", which the CPU treats as "not
       present" -> every port I/O instruction executed at CPL3 faults
       (#GP), regardless of EFLAGS.IOPL. That's intentional: nothing in
       GannetOS's userland ABI needs raw port access, so ring3 code should
       never be able to do it directly. */
    tss.iomap_base = (uint16_t)sizeof(tss);
}

void gdt_init(void)
{
    gdt_p.limit = (uint16_t)(sizeof(struct gdt_entry) * GDT_ENTRIES - 1);
    gdt_p.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0); /* null descriptor - required, never used */

    /* Flat 4GB segments for both rings, same style as boot.asm's own
       minimal GDT (0xCF = 4KB granularity + 32-bit default operand size,
       ORed with the limit's high nibble inside gdt_set_gate). Access byte
       bit layout: P(1) DPL(2) S(1) Type(4). Kernel entries use DPL=00;
       user entries are identical except DPL=11, which is what actually
       makes them usable from CPL3 at all. */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xC0); /* kernel code, ring0 */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xC0); /* kernel data, ring0 */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xC0); /* user code,   ring3 */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xC0); /* user data,   ring3 */

    write_tss(5);

    gdt_flush((uint32_t)&gdt_p);
    tss_flush();
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss.esp0 = esp0;
}

static void write_df_tss(int num, uint32_t kernel_cr3)
{
    uint32_t base  = (uint32_t)&df_tss;
    uint32_t limit = base + sizeof(df_tss);

    gdt_set_gate(num, base, limit, 0x89, 0x00); /* same shape as the main TSS descriptor */

    uint8_t *p = (uint8_t *)&df_tss;
    for (unsigned i = 0; i < sizeof(df_tss); i++) p[i] = 0;

    /* Unlike the main TSS, EVERY one of these fields matters here - a
       task-gate switch loads all of them unconditionally, which is
       exactly what makes this safe to enter from an otherwise-corrupted
       context: nothing about the faulting task's state is trusted or
       reused, the CPU just discards it and starts fresh from here. */
    df_tss.cr3 = kernel_cr3;
    df_tss.eip = (uint32_t)df_fault_handler;
    df_tss.esp = (uint32_t)(df_stack + sizeof(df_stack));
    df_tss.eflags = 0x2; /* IF=0 - stay non-preemptible while reporting a double fault; bit 1 is the reserved-always-1 bit */
    df_tss.cs = GDT_KERNEL_CODE_SEL;
    df_tss.ss = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = GDT_KERNEL_DATA_SEL;
    df_tss.iomap_base = (uint16_t)sizeof(df_tss);
}

void df_handler_init(uint32_t kernel_cr3)
{
    write_df_tss(6, kernel_cr3);

    /* Task gate, not an interrupt gate: access byte 0x85 = present,
       DPL=0, type 0101 (32-bit task gate). For a task gate the "offset"
       field idt_set_gate normally fills in from its base/selector split
       is meaningless - the CPU never uses an EIP from the IDT entry
       itself for a task gate, only the selector, which must name a TSS
       descriptor (GDT_DF_TSS_SEL here). Passing 0 for the offset is
       conventional and harmless since it's simply never read.
       This OVERWRITES whatever idt_init() installed for vector 8 (a
       normal isr_err_stub interrupt gate, fine for a #DF that never
       actually happens, but exactly the wrong shape for one that does -
       see paging.c's df_fault_handler for why). */
    idt_set_gate(8, 0, GDT_DF_TSS_SEL, 0x85);
}