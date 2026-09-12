/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/arch/x86/paging.h"
#include "kernel/ui/fb.h"
#include "kernel/ui/terminal.h"
#include "kernel/arch/x86/idt.h"
#include "kernel/mem/pmm.h"
#include "kernel/proc/task.h"
#include <stdint.h>

/* Declared directly in kernel.c (not terminal.h) since it's only meant
   to be called from the main loop after checking terminal_dirty -
   EXCEPT here: every halt-forever diagnostic in this file needs it too,
   since terminal_puts only marks the internal buffer dirty, it doesn't
   actually blit anything to the screen by itself. Skipping this call
   before a halt loop means the diagnostic message is computed and
   sitting in that buffer, then NEVER SHOWN - the exact bug this comment
   exists to stop from recurring (found via the double-fault handler
   below rendering nothing despite clearly having run - see
   df_fault_handler). */
extern void terminal_update(void);

#define PAGE_ENTRIES 1024

/* 64MB: covers the kernel image, task stacks, the app exec buffer
   (0x200000), the fb software backbuffer (0x01000000, ~2.3MB for a
   1024x768 24bpp mode), AND the pmm frame pool (pmm.h's
   PMM_POOL_START..PMM_POOL_END, 32MB-64MB) - pmm.c zeroes a freshly
   allocated frame via its own physical address as a pointer, which only
   works if that address is identity-mapped. Matches the Makefile's
   `-m 64`. See the KNOWN LIMITATION note in paging.h. */
#define PAGING_IDENTITY_LOW_BYTES (64u * 1024u * 1024u)

/* 16 tables for the 64MB low-memory range (each covers 4MB) + a handful
   for the real framebuffer's LFB (misaligned, can span two tables).
   Per-task private pages do NOT come from this pool - see
   MAX_TASK_PRIVATE_TABLES below for why they need their own reclaimable
   one instead. 32 is a comfortable margin for this pool's actual boot-
   time-only users. */
#define MAX_STATIC_PAGE_TABLES 32

typedef uint32_t pde_t;
typedef uint32_t pte_t;

static pde_t page_directory[PAGE_ENTRIES] __attribute__((aligned(4096)));
static pte_t page_tables[MAX_STATIC_PAGE_TABLES][PAGE_ENTRIES] __attribute__((aligned(4096)));
static int next_free_table = 0;
static int g_paging_enabled = 0;

extern void page_fault_isr(void); /* shell/isr_wrappers.s */

static pte_t *alloc_page_table(void)
{
    if (next_free_table >= MAX_STATIC_PAGE_TABLES)
    {
        terminal_puts("paging_init: out of static page tables - raise "
                      "MAX_STATIC_PAGE_TABLES\n", TERMINAL_LIGHT_RED);
        terminal_update();
        for (;;)
        {
            asm volatile("cli; hlt");
        }
    }
    pte_t *pt = page_tables[next_free_table++];
    for (int i = 0; i < PAGE_ENTRIES; i++)
    {
        pt[i] = 0;
    }
    return pt;
}

/* Identity-maps [phys_start, phys_start+length) with the given flags,
   rounding outward to whole pages. Creates page tables on demand. Safe
   to call multiple times / with overlapping ranges (re-mapping a page
   that's already mapped the same way is a no-op in effect). */
static void identity_map_range(uint32_t phys_start, uint32_t length, uint32_t flags)
{
    uint32_t start = phys_start & ~(PAGE_SIZE - 1);
    uint32_t end = (phys_start + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE)
    {
        uint32_t pd_index = addr >> 22;
        uint32_t pt_index = (addr >> 12) & 0x3FF;

        if (!(page_directory[pd_index] & PAGE_PRESENT))
        {
            pte_t *pt = alloc_page_table();
            page_directory[pd_index] = ((uint32_t)pt) | PAGE_PRESENT | PAGE_RW;
        }

        pte_t *pt = (pte_t *)(page_directory[pd_index] & ~0xFFFu);
        pt[pt_index] = addr | flags | PAGE_PRESENT;
    }
}

/* Marks [vaddr, vaddr+length) - which MUST already be identity-mapped,
   e.g. by the main paging_init() identity map - as ring3-accessible, by
   OR-ing PAGE_USER into both the PDE and PTE for every page in range
   (both levels need it; x86 ANDs the two together, same reasoning as
   paging_map_private_page's own comment on this). Unlike
   identity_map_range, this never creates a new page table - it only
   flips a bit on tables that already exist, and does nothing (silently)
   for any page that isn't mapped yet, since there's no sane flags value
   to invent for a mapping that doesn't exist.
   This affects the SHARED identity-map tables every task's directory
   clones (see paging_clone_kernel_directory), so it only needs to run
   ONCE, at boot - not per task. */
void paging_mark_kernel_region_user(uint32_t vaddr, uint32_t length)
{
    uint32_t start = vaddr & ~(PAGE_SIZE - 1);
    uint32_t end = (vaddr + length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint32_t addr = start; addr < end; addr += PAGE_SIZE)
    {
        uint32_t pd_index = addr >> 22;
        uint32_t pt_index = (addr >> 12) & 0x3FF;

        if (!(page_directory[pd_index] & PAGE_PRESENT))
        {
            continue;
        }

        page_directory[pd_index] |= PAGE_USER;

        pte_t *pt = (pte_t *)(page_directory[pd_index] & ~0xFFFu);
        if (pt[pt_index] & PAGE_PRESENT)
        {
            pt[pt_index] |= PAGE_USER;
        }
    }
}

/* Clears PAGE_PRESENT on a single already-mapped page and flushes that
   one translation out of the TLB (invlpg) so the change takes effect
   immediately rather than whenever the next full CR3 reload happens to
   evict it. Used to punch stack guard pages: a deliberately UNMAPPED
   page placed right where a stack would overflow into, so a runaway
   write lands on a page fault (caught by page_fault_handler, or - if
   ESP itself is already past the boundary by then - the df_fault_handler
   task gate in gdt.c/paging.c) instead of silently corrupting whatever
   memory happens to sit there. See task.c's task_stacks layout for
   where this gets used.
   Like paging_mark_kernel_region_user, this operates on the SHARED
   identity-map tables every task's directory clones, so it only needs
   to run once, at boot. Does nothing if the page isn't mapped. */
void paging_unmap_page(uint32_t vaddr)
{
    uint32_t addr = vaddr & ~(PAGE_SIZE - 1);
    uint32_t pd_index = addr >> 22;
    uint32_t pt_index = (addr >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PAGE_PRESENT))
    {
        return;
    }

    pte_t *pt = (pte_t *)(page_directory[pd_index] & ~0xFFFu);
    pt[pt_index] &= ~(uint32_t)PAGE_PRESENT;

    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static void print_hex32(uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    terminal_puts("0x", TERMINAL_WHITE);
    for (int i = 28; i >= 0; i -= 4)
    {
        terminal_putchar(hex[(v >> i) & 0xF], TERMINAL_WHITE);
    }
}

/* Called from page_fault_isr (shell/isr_wrappers.s) with the CPU's error
   code, the faulting address (read from CR2 in the asm stub, since CR2
   only holds the address of the MOST RECENT fault - it must be read
   before anything else that could itself fault, including pushing
   registers onto a stack that turns out to be the problem), and the
   faulting EIP itself (read straight out of the CPU-pushed exception
   frame, no extra bookkeeping needed) - which instruction touched
   fault_addr, not just what address it touched.

   PHASE 1 behaviour: there is no process isolation yet, so any page fault
   at this point means a genuine kernel bug, not a normal user-mode
   violation - there's no safe way to "recover" from that, so this prints
   diagnostics and halts. Once phase 3 adds real ring3 tasks, a fault
   whose error code has the USER bit set will instead kill just that task
   and let the scheduler carry on, the same way a real OS's #PF handler
   works - the asm stub is already written to support resuming through a
   task switch for exactly that later change (see its comments). */
/* Entered via a hardware TASK GATE (see gdt.c's df_handler_init), not a
   normal C call - by the time this executes, the CPU has already loaded
   an entirely fresh, known-good CS/EIP/ESP/SS/EFLAGS/CR3 from the
   dedicated double-fault TSS, discarding whatever was going on in the
   task that triggered this. That's the whole point: a #DF specifically
   means the CPU couldn't even deliver the ORIGINAL exception (typically
   a #PF) normally - most commonly because the faulting task's own ESP
   had already wandered past its stack guard page into unmapped memory,
   so there was nowhere valid left to push an exception frame. Trying to
   handle that on the SAME broken stack is exactly what cascades into a
   triple fault (CPU reset) instead of a clean report - see task.c's
   stack-guard-page comments for the mechanism this is the other half
   of.
   No CPU-pushed error code or fault address is available here the way
   page_fault_handler gets them - a task-gate switch doesn't deliver
   those, by design, since the whole mechanism exists to work even when
   the normal delivery path itself is broken. task_current_id() still
   works (it just reads task.c's own current_task variable, untouched by
   this hardware switch), so that's the most useful diagnostic available.
   Never returns - a double fault this far along has already cost
   whatever kernel state was live in the faulting task; halting is the
   safe choice; matches page_fault_handler's existing kernel-mode-fault
   behaviour. */
void df_fault_handler(void)
{
    terminal_puts("\n*** DOUBLE FAULT ***\n", TERMINAL_LIGHT_RED);
    terminal_puts("task ", TERMINAL_WHITE);
    print_hex32((uint32_t)task_current_id());
    terminal_puts(" likely overran its own stack past the guard page -\n"
                  "the original fault couldn't be delivered on that stack,\n"
                  "so this ran on a separate emergency stack instead.\n",
                  TERMINAL_WHITE);
    terminal_puts("system halted.\n", TERMINAL_LIGHT_RED);
    terminal_update();
    for (;;)
    {
        asm volatile("cli; hlt");
    }
}

void page_fault_handler(uint32_t error_code, uint32_t fault_addr, uint32_t fault_eip)
{
    int from_ring3 = (error_code & 0x4) != 0;

    if (from_ring3)
    {
        /* A ring3 task touching memory it has no business touching -
           this is the isolation boundary actually being enforced, not a
           kernel bug. Print a short diagnostic (useful while porting
           more of the app surface to ring3), then kill only this task
           and let everything else keep running, instead of halting the
           whole machine over one bad app - the entire point of doing
           any of this GDT/TSS/paging work in the first place. Mirrors
           task_exit() exactly: mark ZOMBIE, schedule() away, never
           return here. */
        terminal_puts("\n*** ring3 task killed: page fault at ", TERMINAL_LIGHT_RED);
        print_hex32(fault_addr);
        terminal_puts(error_code & 0x2 ? " (write)" : " (read)", TERMINAL_LIGHT_RED);
        terminal_puts(", faulting instruction at ", TERMINAL_LIGHT_RED);
        print_hex32(fault_eip);
        terminal_puts(" ***\n", TERMINAL_LIGHT_RED);

        task_exit(); /* never returns */
    }

    terminal_puts("\n*** PAGE FAULT ***\n", TERMINAL_LIGHT_RED);

    terminal_puts("fault address: ", TERMINAL_WHITE);
    print_hex32(fault_addr);
    terminal_putchar('\n', TERMINAL_WHITE);

    terminal_puts("faulting eip: ", TERMINAL_WHITE);
    print_hex32(fault_eip);
    terminal_putchar('\n', TERMINAL_WHITE);

    terminal_puts("error code: ", TERMINAL_WHITE);
    print_hex32(error_code);
    terminal_puts("  (", TERMINAL_WHITE);
    terminal_puts(error_code & 0x1 ? "present" : "not-present", TERMINAL_WHITE);
    terminal_puts(error_code & 0x2 ? ", write" : ", read", TERMINAL_WHITE);
    terminal_puts(error_code & 0x4 ? ", user" : ", kernel", TERMINAL_WHITE);
    if (error_code & 0x8)
    {
        terminal_puts(", reserved-bit-violation", TERMINAL_WHITE);
    }
    if (error_code & 0x10)
    {
        terminal_puts(", instruction-fetch", TERMINAL_WHITE);
    }
    terminal_puts(")\n", TERMINAL_WHITE);

    terminal_puts("this is a kernel-mode fault - a genuine kernel bug, halting.\n", TERMINAL_LIGHT_RED);
    terminal_update();
    for (;;)
    {
        asm volatile("cli; hlt");
    }
}

void paging_init(void)
{
    for (int i = 0; i < PAGE_ENTRIES; i++)
    {
        page_directory[i] = 0;
    }
    next_free_table = 0;

    identity_map_range(0, PAGING_IDENTITY_LOW_BYTES, PAGE_RW);

    uint32_t fb_bytes = fb_info.height * fb_info.pitch;
    identity_map_range(fb_info.phys_base, fb_bytes, PAGE_RW);

    /* Override idt_init()'s generic error-code stub for #PF specifically -
       same pattern pit_init()/mouse_init() use for their own vectors. */
    idt_set_gate(14, (uint32_t)page_fault_isr, 0x08, 0x8E);

    asm volatile("mov %0, %%cr3" : : "r"(page_directory) : "memory");

    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u; /* PG */
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    g_paging_enabled = 1;

    pmm_init(); /* pool is now identity-mapped and paging is live */
}

int paging_enabled(void)
{
    return g_paging_enabled;
}

/* ---- Phase 2: per-task page directories ---- */

/* Decoupled from TASK_MAX_TASKS on purpose - paging.c doesn't need to
   know about task.h at all, it just needs a pool at least as big as the
   number of tasks that will ever call paging_clone_kernel_directory(). */
#define MAX_TASK_DIRECTORIES 8

static pde_t task_directories[MAX_TASK_DIRECTORIES][PAGE_ENTRIES] __attribute__((aligned(4096)));
/* Which slots are currently handed out. Was a bump allocator
   (next_free_directory++, never decremented) until this bug: nothing
   ever gave a directory back when its task exited, so after exactly
   MAX_TASK_DIRECTORIES tasks had EVER existed - not "existed at once",
   EVER, cumulative over the system's whole uptime - every further
   task_create*() call would fail here, permanently, even though every
   earlier task was long since reaped. Surfaced as "paging_clone_
   kernel_directory: directory pool exhausted" after exactly 8 app
   launches in a row (TASK_MAX_TASKS/MAX_TASK_DIRECTORIES both happen to
   be 8) - see task_reap()'s call to paging_free_directory(). */
static uint8_t directory_used[MAX_TASK_DIRECTORIES];

uint32_t paging_kernel_directory_phys(void)
{
    return (uint32_t)page_directory;
}

uint32_t paging_clone_kernel_directory(void)
{
    int slot = -1;
    for (int i = 0; i < MAX_TASK_DIRECTORIES; i++)
    {
        if (!directory_used[i])
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        terminal_puts("paging_clone_kernel_directory: directory pool exhausted - "
                      "raise MAX_TASK_DIRECTORIES\n", TERMINAL_LIGHT_RED);
        return 0;
    }
    directory_used[slot] = 1;
    pde_t *dir = task_directories[slot];
    for (int i = 0; i < PAGE_ENTRIES; i++)
    {
        dir[i] = page_directory[i];
    }
    return (uint32_t)dir;
}

/* Returns a directory to the pool so a future paging_clone_kernel_
   directory() call can reuse its slot - see task_reap() for the only
   caller. Does nothing if dir_phys doesn't match any pool slot (e.g.
   called twice, or passed something that was never a cloned directory
   in the first place) - safer to silently ignore than to corrupt an
   unrelated slot from a bad address. */
void paging_free_directory(uint32_t dir_phys)
{
    for (int i = 0; i < MAX_TASK_DIRECTORIES; i++)
    {
        if ((uint32_t)task_directories[i] == dir_phys)
        {
            directory_used[i] = 0;
            return;
        }
    }
}

void paging_switch_directory(uint32_t phys_addr)
{
    asm volatile("mov %0, %%cr3" : : "r"(phys_addr) : "memory");
}

/* ---- Stage 3: actual isolation - per-task private physical memory ---- */

/* Separate, RECLAIMABLE pool for private page tables - deliberately not
   the same alloc_page_table()/MAX_STATIC_PAGE_TABLES pool the boot-time
   identity map uses above. That one is a pure bump allocator with no
   free path, which is fine for boot-time tables (allocated once, live
   forever) but would be a genuine per-app-launch leak here: before this
   pool existed, every paging_map_private_page() call for a NEW PDE slot
   in some task's directory permanently consumed one of
   MAX_STATIC_PAGE_TABLES's 32 slots, and task_reap() had no way to give
   it back - the exact "total tasks that ever existed, not existing at
   once" bug paging_free_directory's own comment already describes for
   the directory pool, just one level deeper.
   Sized for TASK_MAX_TASKS (8) tasks each needing up to 2 tables at
   once: one for their app code+stack (both fit in APP_LOAD_ADDR's
   single 4MB PDE slot - see loader.c) and one for their TASK_PRIVATE_
   VADDR scratch page (a different slot). 20 leaves comfortable margin
   without paging.c needing to know TASK_MAX_TASKS's exact value. */
#define MAX_TASK_PRIVATE_TABLES 20

static pte_t task_private_tables[MAX_TASK_PRIVATE_TABLES][PAGE_ENTRIES] __attribute__((aligned(4096)));
/* 0 = free slot; otherwise the dir_phys of whichever directory's
   paging_map_private_page() call created this table - see
   paging_free_private_pages()'s doc comment on why ownership, not mere
   presence, is what decides what gets freed. */
static uint32_t task_private_table_owner[MAX_TASK_PRIVATE_TABLES];

static pte_t *alloc_task_private_table(uint32_t owner_dir_phys, pde_t existing_pde)
{
    for (int i = 0; i < MAX_TASK_PRIVATE_TABLES; i++)
    {
        if (task_private_table_owner[i] == 0)
        {
            task_private_table_owner[i] = owner_dir_phys;
            pte_t *pt = task_private_tables[i];
            if (existing_pde & PAGE_PRESENT)
            {
                /* This 4MB PDE slot was already mapped - almost always
                   the shared identity map paging_clone_kernel_
                   directory()'s shallow copy left here, covering the
                   WHOLE 4MB region, not just whatever single page is
                   about to be privatized. COPY its existing entries
                   first, rather than starting from an empty table:
                   privatizing ONE page within a slot must not silently
                   unmap every OTHER page that happens to share the same
                   4MB region.
                   This matters most for PDE slot 0 (0-4MB): the kernel
                   ITSELF is linked at 0x8000 (linker.ld), and
                   APP_LOAD_ADDR/.appstack sit at 0x200000/0x210000 -
                   all comfortably inside that same first 4MB. Starting
                   this table empty used to unmap the running kernel's
                   own code/data the instant this directory's CR3 loaded
                   - the very next instruction fetch (still inside
                   task_switch_asm) page-faulted, the fault handler was
                   ALSO now unmapped, and that cascaded through a double
                   fault into a triple fault: an instant CPU reset (the
                   SeaBIOS splash flashing back up) on launching any app
                   at all, not a hang or a clean crash. */
                pte_t *existing_pt = (pte_t *)(existing_pde & ~0xFFFu);
                for (int j = 0; j < PAGE_ENTRIES; j++)
                {
                    pt[j] = existing_pt[j];
                }
            }
            else
            {
                for (int j = 0; j < PAGE_ENTRIES; j++)
                {
                    pt[j] = 0;
                }
            }
            return pt;
        }
    }
    return 0; /* exhausted - caller (paging_map_private_page) reports this */
}

/* Returns owner_dir_phys if pt is a table this pool owns on that
   directory's behalf, 0 otherwise (a boot-time shared table, or a table
   this pool has no record of at all - either way, not this directory's
   to free). */
static uint32_t task_private_table_owner_of(pte_t *pt, uint32_t dir_phys)
{
    for (int i = 0; i < MAX_TASK_PRIVATE_TABLES; i++)
    {
        if (task_private_tables[i] == pt && task_private_table_owner[i] == dir_phys)
        {
            return dir_phys;
        }
    }
    return 0;
}

static void free_task_private_table(pte_t *pt)
{
    for (int i = 0; i < MAX_TASK_PRIVATE_TABLES; i++)
    {
        if (task_private_tables[i] == pt)
        {
            task_private_table_owner[i] = 0;
            return;
        }
    }
}

uint32_t paging_map_private_page(uint32_t dir_phys, uint32_t vaddr)
{
    pde_t *dir = (pde_t *)dir_phys;
    uint32_t pd_index = vaddr >> 22;
    uint32_t pt_index = (vaddr >> 12) & 0x3FF;

    /* If this exact directory already privatized this 4MB PDE slot
       (an earlier paging_map_private_page() call on THIS dir_phys for
       some other vaddr in the same slot - e.g. a second page of the
       same app's code, or its stack sitting in the same slot as its
       code), reuse that table instead of replacing it: replacing it
       would silently discard every page mapped into it so far, since
       each table is 1024 PTEs covering the WHOLE 4MB slot, not just
       the one page this call is for.
       Otherwise (this slot still points at whatever
       paging_clone_kernel_directory()'s shallow copy left there - the
       shared kernel table, or nothing) allocate a genuinely fresh,
       non-shared table - see alloc_task_private_table()'s own comment
       for why that table starts as a COPY of whatever was already
       mapped there rather than empty: this slot is very likely shared
       with things that have nothing to do with this one page (the
       kernel itself, for APP_LOAD_ADDR's slot), and this call must
       only ever change ITS OWN page's mapping, never anyone else's. */
    pte_t *pt;
    pte_t *existing = (pte_t *)(dir[pd_index] & ~0xFFFu);
    if ((dir[pd_index] & PAGE_PRESENT) &&
        task_private_table_owner_of(existing, dir_phys) == dir_phys)
    {
        pt = existing;
    }
    else
    {
        pt = alloc_task_private_table(dir_phys, dir[pd_index]);
        if (!pt)
        {
            return 0; /* private-table pool exhausted */
        }
        dir[pd_index] = ((uint32_t)pt) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }

    uint32_t frame = pmm_alloc_frame();
    if (frame == 0)
    {
        return 0;
    }

    /* PAGE_USER is set on BOTH levels (PDE and PTE) - x86 ANDs the two
       together, so a page is only ring3-accessible if every level of
       its translation says so. This page is meant to be usable by
       ring3 app code directly (see APP_PRIVATE_VADDR's own comment in
       pexe.h), so both need it despite this file itself always running
       at ring0. */
    pt[pt_index] = frame | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    return frame;
}

void paging_free_private_pages(uint32_t dir_phys)
{
    pde_t *dir = (pde_t *)dir_phys;
    for (uint32_t pd_index = 0; pd_index < PAGE_ENTRIES; pd_index++)
    {
        if (!(dir[pd_index] & PAGE_PRESENT))
        {
            continue;
        }
        pte_t *pt = (pte_t *)(dir[pd_index] & ~0xFFFu);
        if (task_private_table_owner_of(pt, dir_phys) != dir_phys)
        {
            continue; /* shared-in via paging_share_pde, or a boot-time
                          shared table - not this directory's to free */
        }

        for (int pt_index = 0; pt_index < PAGE_ENTRIES; pt_index++)
        {
            if (pt[pt_index] & PAGE_PRESENT)
            {
                pmm_free_frame(pt[pt_index] & ~0xFFFu);
            }
        }
        free_task_private_table(pt);
        dir[pd_index] = 0;
    }
}

void paging_share_pde(uint32_t dst_dir_phys, uint32_t src_dir_phys, uint32_t vaddr)
{
    pde_t *dst = (pde_t *)dst_dir_phys;
    pde_t *src = (pde_t *)src_dir_phys;
    uint32_t pd_index = vaddr >> 22;
    dst[pd_index] = src[pd_index];
}