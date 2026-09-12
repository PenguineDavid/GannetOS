/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/task.h"
#include "kernel/arch/x86/paging.h"
#include "kernel/arch/x86/gdt.h"
#include "kernel/proc/loader.h"
#include <stdint.h>

/* Implemented in shell/isr_wrappers.s.
   task_switch_asm(old_esp_ptr, new_esp):
     - pushes ebp, ebx, esi, edi, eflags onto the CURRENT stack
     - stores the resulting esp into *old_esp_ptr
     - loads esp = new_esp
     - pops eflags, edi, esi, ebx, ebp off the NEW stack
     - ret's into whatever return address sits on top of that new stack

   For a task that has run before, that return address is the instruction
   right after its own earlier call to task_switch_asm. For a brand new
   task, task_create() fabricates a stack that makes it land in
   task_trampoline instead, which realigns the stack and calls the task's
   real entry point with its arg. */
extern void task_switch_asm(uint32_t *old_esp_ptr, uint32_t new_esp);

/* Also in isr_wrappers.s. Never called directly from C - only ever reached
   by task_switch_asm's ret the first time a new task runs. */
extern void task_trampoline(void);

/* Also in isr_wrappers.s. Like task_trampoline, but for a ring3 task:
   reads (entry, user_stack_top) off the fabricated stack the same way
   task_trampoline reads (entry, arg), then builds an iret frame and
   drops to CPL3 instead of just calling entry() at CPL0. Never returns
   to its caller in the normal sense - the task's own code takes over
   entirely at ring3 from that point on. */
extern void ring3_entry_trampoline(void);

/* Also in isr_wrappers.s. Like ring3_entry_trampoline, but for
   task_create_ring3_child(): reads (entry, arg, user_stack_top) off the
   fabricated stack instead of just (entry, user_stack_top), and
   delivers arg to entry as a real cdecl call argument via
   user_task_arg_crt0 rather than iret-ing straight to entry. */
extern void ring3_entry_arg_trampoline(void);

static task_t   tasks[TASK_MAX_TASKS];

/* Each task's stack sits in its own PAGE_SIZE-aligned slot, with a
   dedicated, deliberately UNMAPPED guard page at the LOW end (stacks
   grow down, so that's the direction an overflow travels) - see
   task_stacks_init_guards(). Without this, task_stacks was one flat
   array with slots flush against each other: a deep-recursion or
   unbounded-write overflow in task N wouldn't fault at all, it would
   just silently walk into task N-1's stack memory (whatever that task
   happened to have near the top of ITS stack at the time) and corrupt
   it - the exact class of bug a stack-based buffer overflow exploit
   relies on. Now the same overflow hits the guard page immediately and
   takes a page fault instead - and since the fault itself might happen
   with ESP already inside that unmapped page (no room left to push even
   the fault's own exception frame), gdt.c's df_handler_init/paging.c's
   df_fault_handler exist specifically to still report that safely
   instead of triple-faulting, on a completely separate emergency stack.
   Usable stack space per task is unchanged (TASK_STACK_SIZE); only the
   guard page is new, so this is PAGE_SIZE extra bytes of .bss per task,
   not a change to how much stack code actually gets to use. */
static uint8_t  task_stacks[TASK_MAX_TASKS][PAGE_SIZE + TASK_STACK_SIZE] __attribute__((aligned(PAGE_SIZE)));

/* Top of task N's USABLE stack (guard page excluded) - every fabricated
   initial stack in this file should be built from this, never straight
   off task_stacks[n] + TASK_STACK_SIZE, or it'll be sized as if the
   guard page were still usable stack space. */
static inline uint8_t *task_stack_top(int slot)
{
    return task_stacks[slot] + PAGE_SIZE + TASK_STACK_SIZE;
}

/* Unmaps the first page of every task's stack slot - see task_stacks's
   own comment above. Must run after paging_init() (there has to be a
   real mapping there to begin with, or there's nothing to unmap) and
   before any task_create* call (a task must never actually start
   running before its guard page is in place). */
static void task_stacks_init_guards(void)
{
    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        paging_unmap_page((uint32_t)task_stacks[i]);
    }
}

static int      current_task = -1;
static int      next_task_id = 0;
static uint32_t current_cr3 = 0; // what's ACTUALLY loaded right now, to skip redundant reloads

void task_init(void)
{
    task_stacks_init_guards();

    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        tasks[i].state = TASK_UNUSED;
        tasks[i].id = -1;
    }
    tasks[0].state = TASK_RUNNING;
    tasks[0].id = next_task_id++;
    tasks[0].cr3 = paging_kernel_directory_phys(); // already the live CR3 from paging_init()
    tasks[0].parent_id = -1;
    paging_map_private_page(tasks[0].cr3, TASK_PRIVATE_VADDR);
    tasks[0].name[0] = 'm';
    tasks[0].name[1] = 'a';
    tasks[0].name[2] = 'i';
    tasks[0].name[3] = 'n';
    tasks[0].name[4] = '\0';
    current_task = 0;
    current_cr3 = tasks[0].cr3;
}

int task_create(task_entry_t entry, void *arg, const char *name)
{
    int slot = -1;
    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        if (tasks[i].state == TASK_UNUSED)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return -1;
    }

    uint32_t cr3 = paging_clone_kernel_directory();
    if (cr3 == 0)
    {
        return -1; // directory pool exhausted
    }

    if (paging_map_private_page(cr3, TASK_PRIVATE_VADDR) == 0)
    {
        return -1; // frame pool exhausted
    }

    uint8_t *stack_top = task_stack_top(slot);

    /* Fabricate the exact stack layout task_switch_asm expects to restore:
       from low to high address: [eflags][edi][esi][ebx][ebp][ret_addr][entry][arg]
       so that popping eflags/edi/esi/ebx/ebp then `ret` lands in
       task_trampoline with entry and arg sitting where a called function's
       first two arguments would be. task_trampoline reads both, then
       explicitly realigns the stack to 16 bytes before calling entry(arg)
       - see the comment there for why that realignment is necessary
       rather than just relying on a fixed fabricated size. */
    uint32_t *sp = (uint32_t *)stack_top;
    *(--sp) = (uint32_t)arg;             // second "argument" to task_trampoline
    *(--sp) = (uint32_t)entry;           // first "argument"
    *(--sp) = (uint32_t)task_trampoline; // return address
    *(--sp) = 0;                         // ebp
    *(--sp) = 0;                         // ebx
    *(--sp) = 0;                         // esi
    *(--sp) = 0;                         // edi
    *(--sp) = 0x00000202;                // eflags: IF set, reserved bit 1 set

    tasks[slot].esp = (uint32_t)sp;
    tasks[slot].cr3 = cr3;
    tasks[slot].state = TASK_READY;
    tasks[slot].id = next_task_id++;
    tasks[slot].parent_id = -1;

    int i = 0;
    while (name[i] && i < TASK_NAME_MAX - 1)
    {
        tasks[slot].name[i] = name[i];
        i++;
    }
    tasks[slot].name[i] = '\0';

    return tasks[slot].id;
}

int task_create_ring3(uint32_t entry, uint32_t user_stack_top, const char *name, uint32_t *out_private_frame)
{
    int slot = -1;
    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        if (tasks[i].state == TASK_UNUSED)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return -1;
    }

    uint32_t cr3 = paging_clone_kernel_directory();
    if (cr3 == 0)
    {
        return -1;
    }

    uint32_t private_frame = paging_map_private_page(cr3, TASK_PRIVATE_VADDR);
    if (private_frame == 0)
    {
        return -1;
    }
    if (out_private_frame)
    {
        *out_private_frame = private_frame;
    }

    uint8_t *stack_top = task_stack_top(slot);

    /* Same fabricated-stack trick task_create uses, but landing in
       ring3_entry_trampoline instead of task_trampoline, and handing it
       (entry, user_stack_top) instead of (entry, arg) - see that
       function's own comment in isr_wrappers.s for what it does with
       them. This same task_stacks[slot] region then goes on to serve as
       this task's ONGOING kernel-side stack (see kernel_stack_top below)
       for every syscall/interrupt it takes once it's running at ring3 -
       by the time ring3_entry_trampoline's iret executes, nothing here
       is needed anymore, so reusing the space is safe. */
    uint32_t *sp = (uint32_t *)stack_top;
    *(--sp) = user_stack_top;
    *(--sp) = entry;
    *(--sp) = (uint32_t)ring3_entry_trampoline;
    *(--sp) = 0; // ebp
    *(--sp) = 0; // ebx
    *(--sp) = 0; // esi
    *(--sp) = 0; // edi
    *(--sp) = 0x00000202;

    tasks[slot].esp = (uint32_t)sp;
    tasks[slot].cr3 = cr3;
    tasks[slot].state = TASK_READY;
    tasks[slot].id = next_task_id++;
    tasks[slot].is_ring3 = 1;
    tasks[slot].kernel_stack_top = (uint32_t)stack_top;
    tasks[slot].parent_id = -1;

    int i = 0;
    while (name[i] && i < TASK_NAME_MAX - 1)
    {
        tasks[slot].name[i] = name[i];
        i++;
    }
    tasks[slot].name[i] = '\0';

    return tasks[slot].id;
}

extern void app_entry_trampoline(void); // isr_wrappers.s

int task_create_app_ring3(uint32_t dir_phys, uint32_t app_main_addr, int argc, char **argv, void *api, uint32_t user_stack_top, const char *name)
{
    int slot = -1;
    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        if (tasks[i].state == TASK_UNUSED)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return -1;
    }

    /* Unlike the old shared-.appbuf/.appstack design, dir_phys already
       has this app's own private code/data and stack mapped by the
       caller (loader.c's try_exec) - this function's only remaining
       paging responsibility is the same TASK_PRIVATE_VADDR scratch page
       task_create()/task_create_ring3() give their own directories. */
    if (paging_map_private_page(dir_phys, TASK_PRIVATE_VADDR) == 0)
    {
        return -1;
    }

    uint8_t *stack_top = task_stack_top(slot);

    uint32_t *sp = (uint32_t *)stack_top;
    *(--sp) = user_stack_top;
    *(--sp) = (uint32_t)api;
    *(--sp) = (uint32_t)argv;
    *(--sp) = (uint32_t)argc;
    *(--sp) = app_main_addr;
    *(--sp) = (uint32_t)app_entry_trampoline;
    *(--sp) = 0; // ebp
    *(--sp) = 0; // ebx
    *(--sp) = 0; // esi
    *(--sp) = 0; // edi
    *(--sp) = 0x00000202;

    tasks[slot].esp = (uint32_t)sp;
    tasks[slot].cr3 = dir_phys;
    tasks[slot].state = TASK_READY;
    tasks[slot].id = next_task_id++;
    tasks[slot].is_ring3 = 1;
    tasks[slot].kernel_stack_top = (uint32_t)stack_top;
    tasks[slot].parent_id = -1;

    int i = 0;
    while (name[i] && i < TASK_NAME_MAX - 1)
    {
        tasks[slot].name[i] = name[i];
        i++;
    }
    tasks[slot].name[i] = '\0';

    return tasks[slot].id;
}

/* Where a spawned child's own private stack lives - a fixed vaddr in a
   4MB PDE slot distinct from BOTH APP_LOAD_ADDR (0x200000 - shared FROM
   the parent via paging_share_pde, so the child must never privately
   map anything in that same slot, see paging_share_pde's own comment)
   AND TASK_PRIVATE_VADDR (0x40000000 - this task's own scratch page).
   4 pages (16KB, matching TASK_STACK_SIZE) is comfortable for a worker
   function like apps/tests.c's isolation_worker; raise it if a future
   spawned entry point needs a deeper stack than that. */
#define SPAWN_CHILD_STACK_VADDR 0x50000000u
#define SPAWN_CHILD_STACK_PAGES 4

int task_create_ring3_child(uint32_t entry, void *arg, const char *name)
{
    int slot = -1;
    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        if (tasks[i].state == TASK_UNUSED)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        return -1;
    }

    if (current_task < 0 || !tasks[current_task].is_ring3)
    {
        return -1; // nothing sensible to share code/data FROM
    }

    int parent_id = tasks[current_task].id;
    uint32_t parent_dir = tasks[current_task].cr3;

    uint32_t cr3 = paging_clone_kernel_directory();
    if (cr3 == 0)
    {
        return -1;
    }

    /* Share the parent's code/data - see paging_share_pde's own
       comment for the ownership rule this relies on: this directory
       must never privately map anything in APP_LOAD_ADDR's slot after
       this, only the parent (the actual owner) ever frees it. */
    paging_share_pde(cr3, parent_dir, APP_LOAD_ADDR);

    uint32_t user_stack_top = 0;
    for (int p = 0; p < SPAWN_CHILD_STACK_PAGES; p++)
    {
        uint32_t frame = paging_map_private_page(cr3, SPAWN_CHILD_STACK_VADDR + (uint32_t)p * PAGE_SIZE);
        if (frame == 0)
        {
            return -1; // pool exhausted; leaves cr3 to leak, same as every other early-return here today
        }
    }
    user_stack_top = SPAWN_CHILD_STACK_VADDR + SPAWN_CHILD_STACK_PAGES * PAGE_SIZE;

    if (paging_map_private_page(cr3, TASK_PRIVATE_VADDR) == 0)
    {
        return -1;
    }

    uint8_t *stack_top = task_stack_top(slot);

    /* Same fabricated-stack trick task_create_ring3 uses, but landing
       in ring3_entry_arg_trampoline instead, and handing it (entry,
       arg, user_stack_top) instead of just (entry, user_stack_top) -
       see that function's own comment in isr_wrappers.s for what it
       does with them (delivers arg to entry as a real cdecl call
       argument via user_task_arg_crt0, the same "iret into a small
       ring3-reachable stub" pattern app_entry_trampoline/
       user_app_crt0 already use for app_main's arguments). */
    uint32_t *sp = (uint32_t *)stack_top;
    *(--sp) = user_stack_top;
    *(--sp) = (uint32_t)arg;
    *(--sp) = entry;
    *(--sp) = (uint32_t)ring3_entry_arg_trampoline;
    *(--sp) = 0; // ebp
    *(--sp) = 0; // ebx
    *(--sp) = 0; // esi
    *(--sp) = 0; // edi
    *(--sp) = 0x00000202;

    tasks[slot].esp = (uint32_t)sp;
    tasks[slot].cr3 = cr3;
    tasks[slot].state = TASK_READY;
    tasks[slot].id = next_task_id++;
    tasks[slot].is_ring3 = 1;
    tasks[slot].kernel_stack_top = (uint32_t)stack_top;
    tasks[slot].parent_id = parent_id;

    int i = 0;
    while (name[i] && i < TASK_NAME_MAX - 1)
    {
        tasks[slot].name[i] = name[i];
        i++;
    }
    tasks[slot].name[i] = '\0';

    return tasks[slot].id;
}

/* Round-robin: first READY task found after `from`, wrapping around.
   Returns `from` itself if nothing else is READY. */
static int pick_next(int from)
{
    for (int step = 1; step <= TASK_MAX_TASKS; step++)
    {
        int idx = (from + step) % TASK_MAX_TASKS;
        if (tasks[idx].state == TASK_READY)
        {
            return idx;
        }
    }
    return from;
}

static void schedule(void)
{
    if (current_task < 0)
    {
        return;
    }
    int old = current_task;
    int next = pick_next(old);
    if (next == old)
    {
        return; // nothing else ready, keep running
    }

    if (tasks[old].state == TASK_RUNNING)
    {
        tasks[old].state = TASK_READY;
    }
    tasks[next].state = TASK_RUNNING;
    current_task = next;

    if (tasks[next].cr3 != current_cr3)
    {
        paging_switch_directory(tasks[next].cr3);
        current_cr3 = tasks[next].cr3;
    }

    /* Only matters for a ring3 task: the CPU consults TSS.esp0 for the
       kernel-side stack to switch to on its NEXT ring3->ring0 transition
       (any interrupt or syscall it takes while running). Must be updated
       here, every switch, same reasoning as CR3 above - stale esp0 would
       mean the next such transition lands on the PREVIOUS ring3 task's
       kernel stack instead of this one's. Harmless no-op for a ring0
       task, which never takes that transition. */
    if (tasks[next].is_ring3)
    {
        tss_set_kernel_stack(tasks[next].kernel_stack_top);
    }

    task_switch_asm(&tasks[old].esp, tasks[next].esp);
    /* Execution resumes here once something switches back to `old`. */
}

void task_yield(void)
{
    asm volatile("cli");
    schedule();
    asm volatile("sti");
}

void task_exit(void)
{
    asm volatile("cli");
    tasks[current_task].state = TASK_ZOMBIE;
    schedule();
    /* schedule() should never return here since this task is no longer
       READY/RUNNING and thus can never be picked again - but if the whole
       table were somehow left with nothing else runnable, halt rather than
       fall off into garbage. */
    for (;;)
    {
        asm volatile("hlt");
    }
}

void task_tick(void)
{
    schedule();
}

int task_current_id(void)
{
    if (current_task < 0)
    {
        return -1;
    }
    return tasks[current_task].id;
}

uint32_t task_current_cr3(void)
{
    if (current_task < 0)
    {
        return 0;
    }
    return tasks[current_task].cr3;
}

int task_is_zombie(int id)
{
    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        if (tasks[i].id == id)
        {
            return tasks[i].state == TASK_ZOMBIE;
        }
    }
    return 0;
}

void task_reap(int id)
{
    for (int i = 0; i < TASK_MAX_TASKS; i++)
    {
        if (tasks[i].id == id && tasks[i].state == TASK_ZOMBIE)
        {
            /* Reap every child this task ever spawned via SYS_SPAWN_TASK
               (task_create_ring3_child) BEFORE freeing anything of its
               own - see that function's own doc comment in task.h for
               the use-after-free this closes: a child may still be
               sharing THIS task's private code/data (paging_share_pde),
               so freeing that out from under a still-running child
               would corrupt it. A child not yet even a zombie gets
               waited for right here (task_yield() in a loop) - this
               task is already a zombie itself, so yielding just lets
               everything else, including that child, keep running
               until it finishes on its own; it's not spinning against
               nothing happening.
               This also closes a previously-separate gap: nothing was
               reaping a spawned child's own task-table slot at all
               before this existed - task_reap() on the child (via this
               same recursive call) does that the same way it does for
               any other task. */
            for (int j = 0; j < TASK_MAX_TASKS; j++)
            {
                if (tasks[j].parent_id == id && tasks[j].state != TASK_UNUSED)
                {
                    while (tasks[j].state != TASK_ZOMBIE)
                    {
                        task_yield();
                    }
                    task_reap(tasks[j].id);
                }
            }

            /* Same leak class as the task slot itself - paging_clone_
               kernel_directory() has its own separate, equally finite
               pool that was never being given back either. See
               paging_free_directory's own comment for what this fixes.
               Private pages MUST be freed first, while this directory's
               slot is still this task's own and still tells the truth
               about what it owns - paging_free_directory() below hands
               the slot itself back to the pool for potential immediate
               reuse by the very next task_create*() call, and freeing
               private pages afterwards would then be operating on
               someone else's directory. By this point every child has
               already been reaped (above), so nothing still shares
               anything this call is about to free. */
            if (tasks[i].cr3 != 0)
            {
                paging_free_private_pages(tasks[i].cr3);
                paging_free_directory(tasks[i].cr3);
            }
            tasks[i].state = TASK_UNUSED;
            tasks[i].id = -1;
            tasks[i].is_ring3 = 0;
            tasks[i].kernel_stack_top = 0;
            tasks[i].cr3 = 0;
            tasks[i].parent_id = -1;
            return;
        }
    }
}