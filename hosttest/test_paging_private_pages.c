/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable check of the NEW bookkeeping kernel/arch/x86/paging.c
   added for per-task private memory: paging_map_private_page()'s
   reuse-an-already-owned-table-vs-allocate-fresh decision,
   paging_free_private_pages()'s owner-only walk-and-free, and
   paging_share_pde()'s "the sharer never owns what it shares" contract.

   This is a SIMULATION, not a line-for-line duplicate like the other
   hosttest files - paging.c's real PDE/PTE format is packed 32-bit x86
   page-table entries operated on via raw pointer arithmetic over
   physical memory, which doesn't have a meaningful host-testable
   equivalent (there's no real physical-memory identity map here, and
   the interesting bugs to catch aren't in the bit-packing, which is
   standard/settled - they're in the pool bookkeeping: does a second
   page in the same 4MB slot reuse the first page's table, does two
   directories mapping the same slot number ever alias, does freeing a
   directory return exactly what it owns and nothing it merely shares).
   So this reimplements that bookkeeping - same ownership rule, same
   reuse-vs-fresh decision, same walk-only-what-you-own free - against
   plain host structs standing in for a directory/table/frame, and
   checks those invariants directly rather than bit layout. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define PAGE_ENTRIES 16 /* real paging.c uses 1024; a small number here
                            keeps the test's own bookkeeping arrays
                            readable without changing what's tested */
#define MAX_DIRS 4
#define MAX_TABLES 6
#define MAX_FRAMES 64
/* One extra table slot, index MAX_TABLES, reserved for the "boot-time
   shared" table used in the regression test below - alloc_table()'s own
   free-slot scan only ever looks at indices 0..MAX_TABLES-1, so this
   slot can never be handed out by it, exactly mirroring how the real
   boot-time identity-map tables (kernel/arch/x86/paging.c's OTHER,
   separate static pool) aren't part of task_private_tables[] at all -
   task_private_table_owner_of() correctly treats such a table as
   "not ours" purely because it isn't IN the pool array, not because of
   any ownership bookkeeping. */
#define SHARED_TABLE_ID MAX_TABLES

typedef struct
{
    int present;
    int frame; /* index into fake_frames, or -1 */
} fake_pte_t;

typedef struct
{
    int present;
    int table; /* index into fake_tables, or -1 */
} fake_pde_t;

static fake_pde_t dirs[MAX_DIRS][PAGE_ENTRIES];
static fake_pte_t fake_tables[MAX_TABLES + 1][PAGE_ENTRIES]; /* +1: see SHARED_TABLE_ID */
static int table_owner[MAX_TABLES]; /* -1 = free, else owning dir index */
static int frame_used[MAX_FRAMES];

static void reset_all(void)
{
    memset(dirs, 0, sizeof(dirs));
    memset(fake_tables, 0, sizeof(fake_tables));
    for (int i = 0; i < MAX_TABLES; i++)
    {
        table_owner[i] = -1;
    }
    for (int i = 0; i < MAX_FRAMES; i++)
    {
        frame_used[i] = 0;
    }
}

static int alloc_frame(void)
{
    for (int i = 0; i < MAX_FRAMES; i++)
    {
        if (!frame_used[i])
        {
            frame_used[i] = 1;
            return i;
        }
    }
    return -1;
}
static void free_frame(int f)
{
    if (f >= 0)
    {
        frame_used[f] = 0;
    }
}

static int alloc_table(int owner_dir, int copy_from_table /* -1 = start empty */)
{
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (table_owner[i] == -1)
        {
            table_owner[i] = owner_dir;
            if (copy_from_table >= 0)
            {
                /* Mirrors alloc_task_private_table()'s fix: a slot being
                   privatized for the first time usually already pointed
                   at something (the shared/master table) covering the
                   WHOLE 4MB region, not just the one page about to be
                   privatized - copy its existing entries first so
                   privatizing one page can't silently unmap every other
                   page that happens to share the same slot (in the real
                   code: the kernel itself, sharing APP_LOAD_ADDR's PDE
                   slot - see paging.c's own comment on the reset this
                   caused before this fix existed). */
                memcpy(fake_tables[i], fake_tables[copy_from_table], sizeof(fake_tables[i]));
            }
            else
            {
                memset(fake_tables[i], 0, sizeof(fake_tables[i]));
            }
            return i;
        }
    }
    return -1;
}
static void free_table(int t)
{
    if (t >= 0)
    {
        table_owner[t] = -1;
    }
}

/* --- the actual logic under test, mirroring paging_map_private_page /
   paging_free_private_pages / paging_share_pde's decisions --- */

/* Returns the frame index mapped, or -1 on pool exhaustion. pt_index
   mirrors the real code's (vaddr >> 12) & 0x3FF - the within-table
   slot for this specific page, distinct from pd_index (which page
   TABLE, i.e. which 4MB region, it belongs to). Two pages in the same
   pd_index but different pt_index (e.g. two different 4KB pages of the
   same app's code) must land in different pt_index slots of the SAME
   reused table - never overwrite each other. */
static int map_private_page(int dir, int pd_index, int pt_index)
{
    fake_pde_t *pde = &dirs[dir][pd_index];

    int table;
    if (pde->present && pde->table < MAX_TABLES && table_owner[pde->table] == dir)
    {
        table = pde->table; /* this directory already owns this slot's table - reuse it */
    }
    else
    {
        int copy_from = pde->present ? pde->table : -1;
        table = alloc_table(dir, copy_from);
        if (table < 0)
        {
            return -1;
        }
        pde->present = 1;
        pde->table = table;
    }

    int frame = alloc_frame();
    if (frame < 0)
    {
        return -1;
    }

    fake_tables[table][pt_index].present = 1;
    fake_tables[table][pt_index].frame = frame;
    return frame;
}

static void free_private_pages(int dir)
{
    for (int pd_index = 0; pd_index < PAGE_ENTRIES; pd_index++)
    {
        fake_pde_t *pde = &dirs[dir][pd_index];
        if (!pde->present)
        {
            continue;
        }
        if (pde->table >= MAX_TABLES || table_owner[pde->table] != dir)
        {
            continue; /* shared-in, or a boot-time table, or unowned - not ours */
        }

        for (int i = 0; i < PAGE_ENTRIES; i++)
        {
            if (fake_tables[pde->table][i].present)
            {
                free_frame(fake_tables[pde->table][i].frame);
            }
        }
        free_table(pde->table);
        pde->present = 0;
        pde->table = -1;
    }
}

static void share_pde(int dst_dir, int src_dir, int pd_index)
{
    dirs[dst_dir][pd_index] = dirs[src_dir][pd_index];
}

static int count_used_frames(void)
{
    int n = 0;
    for (int i = 0; i < MAX_FRAMES; i++)
    {
        if (frame_used[i])
        {
            n++;
        }
    }
    return n;
}
static int count_used_tables(void)
{
    int n = 0;
    for (int i = 0; i < MAX_TABLES; i++)
    {
        if (table_owner[i] != -1)
        {
            n++;
        }
    }
    return n;
}

static int failures = 0;
static void check(const char *name, int cond)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond)
    {
        failures++;
    }
}

int main(void)
{
    /* Two pages in the same PDE slot of the same directory reuse one
       table - the bug that motivated this fix in the first place
       (mapping a second page used to silently discard the first). */
    reset_all();
    int f1 = map_private_page(0, 5, 0);
    int f2 = map_private_page(0, 5, 1); /* different page within the same slot */
    check("two pages in the same slot get different frames", f1 != f2 && f1 >= 0 && f2 >= 0);
    check("two pages in the same slot share one table", count_used_tables() == 1);
    check("the second page didn't discard the first (both frames still allocated)",
          frame_used[f1] && frame_used[f2]);

    /* A different PDE slot in the SAME directory gets its own table. */
    reset_all();
    map_private_page(0, 5, 0);
    map_private_page(0, 9, 0);
    check("a different slot in the same directory gets a separate table",
          count_used_tables() == 2);

    /* The SAME PDE slot number in TWO DIFFERENT directories must never
       alias - each directory's own table, own frame. */
    reset_all();
    int fa = map_private_page(0, 5, 0);
    int fb = map_private_page(1, 5, 0);
    check("the same slot number in two different directories never aliases",
          fa != fb);
    check("two directories mapping the same slot number get separate tables",
          count_used_tables() == 2);

    /* Freeing a directory returns exactly its own frame(s) and table(s),
       and the pool becomes reusable afterwards (the leak this whole
       pool replaced: a bump allocator that never gave tables back). */
    reset_all();
    map_private_page(0, 5, 0);
    map_private_page(0, 5, 1); /* second page, same slot - still one table, two frames */
    check("two frames allocated before freeing", count_used_frames() == 2);
    free_private_pages(0);
    check("freeing a directory returns all its frames", count_used_frames() == 0);
    check("freeing a directory returns its table", count_used_tables() == 0);
    int reused = alloc_table(2, -1);
    check("a freed table slot is available for a brand new directory to reuse",
          reused >= 0);

    /* paging_share_pde: the sharer must never own what it shares, and
       freeing the SHARER must never touch the frames the ORIGINAL
       owner still needs. */
    reset_all();
    int parent_frame = map_private_page(0 /* parent dir */, 5, 0);
    share_pde(1 /* child dir */, 0 /* parent dir */, 5);
    check("after sharing, the child's PDE points at the parent's table",
          dirs[1][5].present && dirs[1][5].table == dirs[0][5].table);
    check("the child does NOT own the shared table (parent still does)",
          table_owner[dirs[1][5].table] == 0);

    free_private_pages(1); /* free the CHILD - must be a no-op on the shared slot */
    check("freeing the child leaves the parent's frame alive",
          frame_used[parent_frame]);
    check("freeing the child leaves the shared table alive",
          count_used_tables() == 1);

    free_private_pages(0); /* NOW free the actual owner - this must reclaim it */
    check("freeing the actual owner (parent) afterwards reclaims the frame",
          !frame_used[parent_frame]);
    check("freeing the actual owner (parent) afterwards reclaims the table",
          count_used_tables() == 0);

    /* THE REGRESSION TEST: privatizing one page in a slot that already
       had OTHER, unrelated entries present (a "shared/kernel" table -
       table_owner == -1, exactly like the master's identity-mapped
       low-memory table after paging_clone_kernel_directory()'s shallow
       copy, never allocated through this pool at all) must leave those
       other entries exactly as they were. In the real kernel this slot
       IS PDE 0 (0-4MB): the kernel itself (linked at 0x8000) and
       APP_LOAD_ADDR (0x200000) share it. Before alloc_task_private_
       table() copied the existing table instead of starting empty,
       privatizing an app's own code page silently unmapped the
       kernel's own code/data too - the next instruction fetch, still
       inside the currently-running task-switch code, page-faulted; the
       fault handler was ALSO now unmapped; that cascaded into a triple
       fault and reset the whole machine, on launching any app at all. */
    reset_all();
    /* Seed slot 5 with a pre-existing table at SHARED_TABLE_ID (never
       allocated through alloc_table() - see that macro's own comment)
       that already has entries at pt_index 2 and 9 - standing in for
       "the kernel's own pages, already mapped by the shared identity
       map before this directory ever privatized anything in this
       slot". */
    int kernel_frame_a = alloc_frame(), kernel_frame_b = alloc_frame();
    fake_tables[SHARED_TABLE_ID][2] = (fake_pte_t){.present = 1, .frame = kernel_frame_a};
    fake_tables[SHARED_TABLE_ID][9] = (fake_pte_t){.present = 1, .frame = kernel_frame_b};
    dirs[0][5] = (fake_pde_t){.present = 1, .table = SHARED_TABLE_ID};

    /* Now privatize ONE page in that same slot - e.g. an app's own
       code, at a completely different pt_index. */
    int app_frame = map_private_page(0, 5, 7);

    check("privatizing a page allocates its own frame", app_frame >= 0 && app_frame != kernel_frame_a && app_frame != kernel_frame_b);
    check("the privatized directory now owns a DIFFERENT table than the original shared one",
          dirs[0][5].table < MAX_TABLES && table_owner[dirs[0][5].table] == 0 && dirs[0][5].table != SHARED_TABLE_ID);
    check("THE FIX: the pre-existing entry at pt_index 2 survived privatization",
          fake_tables[dirs[0][5].table][2].present &&
          fake_tables[dirs[0][5].table][2].frame == kernel_frame_a);
    check("THE FIX: the pre-existing entry at pt_index 9 survived privatization",
          fake_tables[dirs[0][5].table][9].present &&
          fake_tables[dirs[0][5].table][9].frame == kernel_frame_b);
    check("the original shared table itself is untouched (other directories/the master still see it correctly)",
          fake_tables[SHARED_TABLE_ID][2].present && fake_tables[SHARED_TABLE_ID][2].frame == kernel_frame_a &&
          fake_tables[SHARED_TABLE_ID][9].present && fake_tables[SHARED_TABLE_ID][9].frame == kernel_frame_b);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}