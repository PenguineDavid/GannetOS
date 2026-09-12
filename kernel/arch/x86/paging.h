/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_SIZE 4096

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_USER    0x4

/* PHASE 1 of virtual memory bring-up: enables standard 2-level x86 paging
   with a single, fully identity-mapped address space (every virtual
   address equals its physical address). This does NOT give any task its
   own address space and does NOT achieve any process isolation yet - it
   only proves paging itself works correctly (page directory/tables set
   up right, CR3/CR0.PG toggled correctly, page faults land in a real
   handler instead of silently looping) before building anything on top
   of it. Per-task address spaces and a real ring3/kernel split are
   later, separate phases.

   Must be called AFTER fb_init() (it reads fb_info.phys_base to map the
   real hardware framebuffer, which fb_init() is what populates) and
   after idt_init() (it installs its own gate for vector 14, overriding
   idt_init()'s generic error-code stub for that one vector).

   KNOWN LIMITATION: identity-maps a HARDCODED low-memory range
   (see PAGING_IDENTITY_LOW_BYTES in paging.c) rather than parsing a real
   BIOS memory map, since boot.asm doesn't retrieve one yet. If total
   kernel-side memory usage (kernel + task stacks + app buffer + fb
   backbuffer) ever grows past that, or -m is set below it, raise the
   constant. A proper fix is teaching boot.asm to fetch an INT 15h/E820
   map and pass it to the kernel - not done yet. */
void paging_init(void);

/* True once paging_init() has run and CR0.PG is actually set. */
int paging_enabled(void);

/* ---- Phase 2: per-task page directories ----
   Each task gets its OWN page directory object, so CR3 genuinely differs
   per task and the scheduler can switch it on every task switch. For now
   every cloned directory is a SHALLOW copy of the master one - same
   underlying page tables, so the actual mappings are still identical
   everywhere. This does NOT add isolation by itself (two tasks still see
   the exact same memory); it's the plumbing a later phase (giving each
   task its own distinct physical frames for a private region) builds
   real isolation on top of. What THIS phase proves is that switching CR3
   on every task switch works correctly and doesn't destabilize anything
   - a real prerequisite regardless of what those per-task mappings
   eventually diverge into. */

/* Physical address of the master (kernel) page directory - what task 0
   (kernel_main's own context) uses; already loaded into CR3 by
   paging_init(). */
uint32_t paging_kernel_directory_phys(void);

/* Returns the physical address of a new page directory, shallow-copied
   from the master one (see above). Returns 0 if the directory pool is
   exhausted. */
uint32_t paging_clone_kernel_directory(void);

/* Returns a directory obtained from paging_clone_kernel_directory() back
   to the pool - see paging.c for why this matters (without it, the pool
   silently exhausts after MAX_TASK_DIRECTORIES tasks have EVER existed,
   not MAX_TASK_DIRECTORIES existing at once). Must be called exactly
   once per successful paging_clone_kernel_directory() call, once that
   task is fully done (see task_reap()) - never call this while the
   directory might still be in use (e.g. still loaded in CR3). */
void paging_free_directory(uint32_t dir_phys);

/* Loads phys_addr into CR3. Cheap-ish but not free (flushes the TLB) -
   callers that switch frequently should skip the call entirely when
   phys_addr already equals what's currently loaded, as task.c's
   scheduler does. */
void paging_switch_directory(uint32_t phys_addr);

/* ---- Stage 3: actual isolation ----
   Maps ONE 4KB page at `vaddr` within the page directory at `dir_phys`
   to a freshly allocated, distinct physical frame (see pmm.h). This is
   what turns Phase 2's "every task has its own directory, but they all
   point at the same tables" into genuine isolation: two tasks that each
   call this with the SAME vaddr get DIFFERENT physical memory backing
   it, so writes through one are invisible to the other.
   The first call for a given 4MB PDE slot in a given directory creates
   a fresh, non-shared page table for that slot - it can never alias
   with any other directory's table for the same slot, which is the
   entire point of "private" here - but if that slot was already mapped
   to something (almost always true: paging_clone_kernel_directory()'s
   shallow copy leaves every slot pointing at the shared identity map),
   the new table starts as a COPY of whatever was there, with only this
   call's own page then redirected to the fresh private frame. Every
   OTHER page already mapped in that same 4MB region keeps working
   exactly as before.
   This matters even for `vaddr`s that look unrelated to anything: PDE
   slots cover 4MB, so e.g. APP_LOAD_ADDR (0x200000, loader.h) shares
   its slot with everything else in the first 0-4MB, including the
   KERNEL ITSELF (linked at 0x8000 - see linker.ld). An earlier version
   of this function started every new table empty instead of copying -
   privatizing an app's own code silently unmapped the running kernel
   the instant that directory's CR3 loaded, which triple-faulted the
   whole machine back to a reset on launching any app at all. A second
   call for a page already in a slot THIS SAME dir_phys already
   privatized (e.g. a second page of the same app's code) reuses that
   table rather than re-deriving it, so it doesn't matter which order
   pages within one slot get mapped in.
   Returns the mapped frame's physical address, or 0 if the frame pool is
   exhausted. */
uint32_t paging_map_private_page(uint32_t dir_phys, uint32_t vaddr);

/* Marks an already-identity-mapped range as ring3-accessible (OR's
   PAGE_USER into both PDE and PTE) - see paging.c for the full
   reasoning. Used once at boot for the fixed regions ring3 app code
   needs to reach directly (not via a syscall): the app exec buffer,
   its stack, and the kernel-authored syscall trampolines/crt0. Does
   nothing for any page that isn't mapped yet. */
void paging_mark_kernel_region_user(uint32_t vaddr, uint32_t length);

/* Frees every private page this directory OWNS - not merely sees. Each
   private table (see paging.c's task-private-table pool) records which
   directory's paging_map_private_page() call created it; this walks
   dir_phys's PDEs and, for each one whose table this directory itself
   owns, returns its frame(s) to pmm (pmm_free_frame) and the table
   itself to the pool. A PDE slot this directory only SHARES via
   paging_share_pde() - owned by some other directory - is left
   untouched, so freeing a child doesn't rip the parent's still-live
   code/data out from under it, and freeing the parent later doesn't
   double-free a frame the child's share never separately allocated.
   Call this once per task, in task_reap(), BEFORE
   paging_free_directory() - the directory's own slot in the directory
   pool gets reused the moment that runs, so anything not reclaimed
   here first is reclaimed nowhere. */
void paging_free_private_pages(uint32_t dir_phys);

/* Copies ONE page-directory entry (the whole 4MB PDE slot containing
   vaddr) from src_dir_phys into dst_dir_phys, so dst's directory now
   points at the exact SAME page table - and therefore the exact same
   physical frames - as src does for that slot. Used by
   task_create_ring3_child() (task.c) to give a spawned child task
   access to its parent's already-private code/data (SYS_SPAWN_TASK's
   whole point is a sibling task that shares its parent's loaded code
   and globals, not an independent copy - see apps/tests.c's
   isolation_worker for the intended semantics).
   OWNERSHIP: this copies an entry, not a table - dst never becomes
   that table's owner, so dst_dir_phys must never call
   paging_map_private_page() for any vaddr in this same 4MB slot
   afterwards (there's nothing for it to safely reuse or safely
   replace - src still owns it). paging_free_private_pages() already
   leaves an unowned slot alone on its own, so a shared slot only ever
   gets freed once, when the ORIGINAL owner (src) is reaped - never
   when a directory that merely shares it via this call is. */
void paging_share_pde(uint32_t dst_dir_phys, uint32_t src_dir_phys, uint32_t vaddr);

/* Clears PAGE_PRESENT on one already-mapped page (and flushes the TLB
   for it) - see paging.c for the full reasoning. Used to punch stack
   guard pages so a stack overflow page-faults instead of silently
   corrupting whatever's mapped next. Does nothing if the page isn't
   mapped. */
void paging_unmap_page(uint32_t vaddr);

#endif