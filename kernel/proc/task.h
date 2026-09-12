/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef TASK_H
#define TASK_H

#include <stdint.h>

// GannetOS has no heap yet, so tasks come from a fixed-size static pool -
// same convention as loader.c's static exec_buf. Raise this if you need
// more concurrent tasks; each one costs TASK_STACK_SIZE bytes of .bss.
#define TASK_MAX_TASKS   8
#define TASK_STACK_SIZE  (16 * 1024)
#define TASK_NAME_MAX    16

// Stage 3 (actual isolation): every task gets one 4KB page of its own,
// mapped at this SAME virtual address in every task's own page
// directory but backed by a DIFFERENT physical frame per task (see
// paging_map_private_page). Chosen well outside the identity-mapped
// low-memory range and the real framebuffer's LFB so mapping it can't
// clobber anything else - see paging_map_private_page's own comment for
// why that matters. Userland apps see the same address as
// APP_PRIVATE_VADDR in pexe.h - the two MUST match.
#define TASK_PRIVATE_VADDR 0x40000000u

typedef enum
{
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

typedef void (*task_entry_t)(void *arg);

/* A ring3 task's entry point takes no C-level parameters - it's not a
   normal call, it's the target of an iret (see task_create_ring3), so
   there's no calling convention delivering arguments the usual way.
   Whatever the entry code needs, it has to already know (e.g. a fixed
   address like APP_PRIVATE_VADDR) or read out of a register the iret
   path left set up for it. */
typedef void (*task_entry_ring3_t)(void);

typedef struct
{
    uint32_t     esp;    // saved stack pointer; only valid while not RUNNING
    uint32_t     cr3;    // this task's own page directory (see paging.h Phase 2)
    task_state_t state;
    char         name[TASK_NAME_MAX];
    int          id;
    uint8_t      is_ring3;         // 1 if this task's own code runs at CPL3
    uint32_t     kernel_stack_top; // TSS.esp0 while this task is RUNNING - only meaningful when is_ring3
    // -1 if this task was NOT created via task_create_ring3_child (every
    // path except SYS_SPAWN_TASK sets this), otherwise the id of the
    // task that spawned it via SYS_SPAWN_TASK - lets task_reap() find
    // and wait for a task's own children before freeing anything they
    // might still be sharing (see task_create_ring3_child's own doc
    // comment on why that ordering matters). Deliberately -1, not 0:
    // task 0 (the boot "main" task - see task_init) is a real, valid
    // id, so 0 can't double as a "no parent" sentinel here.
    int          parent_id;
} task_t;

// Must be called once, before pit_init(). Claims slot 0 for whichever
// context calls this (normally kernel_main's own boot stack) and marks it
// RUNNING, so the very first scheduler tick has something to switch from.
void task_init(void);

// Creates a new READY task running entry(arg) on its own private stack.
// `arg` is handed to entry() as its one and only parameter - this is how a
// task finds out which resource it owns (e.g. which window), without a
// global "who am I" lookup.
// Returns the task id (>= 0) on success, or -1 if the task table is full.
// entry() should not return; if it does, the task is torn down as if it
// had called task_exit().
int task_create(task_entry_t entry, void *arg, const char *name);

/* Like task_create, but the new task's own code begins executing at
   CPL3 (ring3) instead of CPL0, via an iret rather than a plain call -
   see ring3_entry_trampoline in isr_wrappers.s.
   `entry` MUST be an address that this task's own page directory maps
   with PAGE_USER set (see paging_map_private_page) - typically a stub
   copied into that task's private page at APP_PRIVATE_VADDR. A kernel
   .text address is NEVER PAGE_USER, so passing one here would #GP the
   instant the CPU tried to fetch the first instruction at CPL3, before
   this function's caller gets any indication anything is wrong.
   `user_stack_top` has the same requirement (PAGE_USER | PAGE_RW) -
   this is the ring3 code's OWN stack, entirely separate from the
   kernel-side stack this task also gets (used for its syscalls and
   interrupts, never touched by ring3 code directly).
   Returns the task id (>= 0) on success, or -1 if the task table is
   full. */
/* Returns the task id (>= 0) on success, or -1 if the task table is
   full. On success, if out_private_frame is non-NULL, also writes back
   the physical address of this task's private page (see
   paging_map_private_page) - since that frame lives in GannetOS's
   identity-mapped low-memory pool, the CALLER (still running at ring0,
   under its OWN page directory) can dereference it directly as an
   ordinary pointer to place code/data there before the new task ever
   runs, without needing to switch to the new task's own directory
   first. That's how a ring3 task ends up with anything ring3-executable
   to run in the first place - see kernel.c's ring3 smoke test. */
int task_create_ring3(uint32_t entry, uint32_t user_stack_top, const char *name, uint32_t *out_private_frame);

/* Spawns a REAL app at ring3: app_main_addr is called as
   app_main_addr(argc, argv, api) at CPL3, using a real cdecl call (see
   app_entry_trampoline/user_app_crt0 in isr_wrappers.s) rather than the
   raw iret task_create_ring3 does - so, unlike task_create_ring3, this
   is safe to use with an ordinary C entry point that expects real
   arguments, matching pexe.h's app_entry_t signature exactly.
   `dir_phys` is a page directory the CALLER has already obtained from
   paging_clone_kernel_directory() and populated with this app's own
   private code/data (via paging_map_private_page() at APP_LOAD_ADDR)
   and its own private stack (see loader.c's try_exec) - unlike
   task_create_ring3, this function does not clone or populate a
   directory itself, since the caller has to have already written the
   app's actual code/data into it before this task can run at all. This
   function DOES map this directory's TASK_PRIVATE_VADDR scratch page,
   the same as task_create/task_create_ring3 already do for their own
   directories.
   `api` MUST point to a ring3-readable api_t - i.e. loader.c's
   g_ring3_api, whose function pointers are syscall trampolines, never
   g_api, whose function pointers are the real privileged kernel
   functions themselves. Handing a ring3 task &g_api would let it call
   kernel code directly at CPL3 with no syscall gate involved at all -
   exactly the hazard this whole redesign exists to close.
   argv's contents (each string) must also be ring3-readable, for the
   same reason - copy them into the app's own private memory rather
   than pointing at anything kernel-owned.
   Returns the task id (>= 0) on success, or -1 if the task table is
   full or the TASK_PRIVATE_VADDR mapping fails (frame pool exhausted). */
int task_create_app_ring3(uint32_t dir_phys, uint32_t app_main_addr, int argc, char **argv, void *api, uint32_t user_stack_top, const char *name);

/* Spawns a ring3 SIBLING of the CURRENTLY RUNNING task: entry(arg) -
   task_entry_t, the same void(*)(void*) signature task_create() uses -
   runs at CPL3 sharing the current task's own loaded code and data
   (via paging_share_pde() at APP_LOAD_ADDR - see that function's own
   comment on the ownership rules this depends on), with its own
   private stack and its own private TASK_PRIVATE_VADDR scratch page.
   This is what SYS_SPAWN_TASK (syscall.c) is built on: it replaces
   spawning via plain task_create(), which ran the new task at ring0 -
   full kernel privilege - regardless of the CALLER's privilege,
   letting any ring3 app turn an address in its own code into a
   kernel-privileged task on demand. See apps/tests.c's isolation_worker
   for the intended "sibling task, shared code and globals, private
   stack and scratch page" semantics this preserves.
   arg is delivered to entry as an ordinary cdecl call argument via
   ring3_entry_arg_trampoline/user_task_arg_crt0 (isr_wrappers.s) - the
   same "iret into a small ring3-reachable stub that does the actual
   push-and-call" pattern app_entry_trampoline/user_app_crt0 already
   use for app_main's arguments, applied to task_entry_t's one-argument
   signature instead.
   `entry` must be an address the CALLING task's own directory already
   maps with PAGE_USER (validated by syscall.c's sys_spawn_task before
   this is ever called).
   ORDERING HAZARD, not closed by this function alone: the parent's own
   reap (task_reap, once the PARENT itself finishes) frees the parent's
   private frames - including the ones this child only shares, never
   owns (see paging_share_pde/paging_free_private_pages). If the parent
   is reaped while this child is still actually running (not yet a
   zombie itself), the child ends up executing out of frames that have
   already been handed back to pmm and could be reused by some
   unrelated task underneath it - a real use-after-free. See
   task_reap()'s own comment for how this is closed: the parent's reap
   now blocks until every child it ever spawned is itself a zombie
   first, so the ordering this function depends on is actually
   enforced, not just true by coincidence of the one real caller
   happening to wait.
   Returns the task id (>= 0) on success, or -1 if the task table is
   full, the calling task isn't itself a valid ring3 task with a
   private directory to share from, or a mapping fails (pool
   exhausted). */
int task_create_ring3_child(uint32_t entry, void *arg, const char *name);

// Voluntarily gives up the CPU to the next READY task, if any. Safe to
// call with interrupts enabled; disables them only for the duration of
// the actual switch.
void task_yield(void);

// Marks the calling task ZOMBIE and switches away. Never returns.
void task_exit(void);

// Called from the PIT ISR (via pit_handler) on every timer tick. Picks the
// next READY task round-robin and context-switches to it. If nothing else
// is READY, returns immediately without switching.
void task_tick(void);

// Id of the task currently executing.
int task_current_id(void);

// Page directory (see paging.h Phase 2) of the task currently executing
// - i.e. tasks[current_task].cr3. Used by SYS_SPAWN_TASK (syscall.c) to
// find what to share the new child's code/data FROM (see
// task_create_ring3_child). 0 if no task has been set current yet
// (shouldn't happen once task_init() has run).
uint32_t task_current_cr3(void);

/* True once the task with this id has called task_exit() (or its ring3
   equivalent, SYS_EXIT_TASK) and is sitting as a TASK_ZOMBIE - i.e. it's
   done running but its slot hasn't been reclaimed yet. False for an
   unknown/already-reused id too, so a caller polling in a loop (see
   loader.c's try_exec) naturally stops once the task is gone rather
   than spinning forever on an id that no longer means anything. */
int task_is_zombie(int id);

/* Reclaims a TASK_ZOMBIE task's slot so a future task_create*() call can
   reuse it - task_is_zombie() only ever reports that a task finished,
   it never released the slot on its own. Without this, every completed
   task (in particular, every app try_exec ever runs) permanently
   consumes one of the TASK_MAX_TASKS slots forever - after only
   TASK_MAX_TASKS app launches, every task_create*() call starts failing
   with "no free slot", which looks exactly like commands mysteriously
   stopping partway through a loop that runs enough of them. No-op if
   the id is already reaped, reused, or was never a real task.
   BLOCKS (loops on task_yield()) until every child this task ever
   spawned via SYS_SPAWN_TASK (task_create_ring3_child) is itself a
   zombie, then reaps each of those too, before freeing anything of
   THIS task's own - a child may still be sharing this task's private
   code/data (paging_share_pde), so freeing that out from under a
   still-running child would be a real use-after-free. This task being
   reaped is already a zombie by definition, so yielding here just lets
   everything else - including that child - keep running until it
   finishes on its own; it isn't a busy-wait against nothing happening.
   This also closes a previously-separate gap: nothing was reaping a
   spawned child's own task-table slot at all before this existed - it
   isn't reclaimed by the child's own zombie status alone, same as any
   other task. */
void task_reap(int id);

#endif