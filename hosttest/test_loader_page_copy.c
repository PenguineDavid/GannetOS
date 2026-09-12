/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable check of the page-count/chunk-size arithmetic try_exec()
   (loader.c) added for copying an app's code+data into private frames
   one page at a time: how many pages a given mem_size needs, and how
   many bytes of the LAST page are real content vs left at the frame's
   already-zeroed fill. Off-by-one here would either drop the last
   partial page of an app's BSS/data (too few pages) or copy from past
   the end of exec_buf (too many, or a wrong per-page byte count). */
#include <stdio.h>
#include <stdint.h>

#define PAGE_SIZE 4096u

/* --- exact copy of the two expressions try_exec() uses --- */
static uint32_t pages_needed(uint32_t mem_size)
{
    return (mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
}
static uint32_t chunk_for_page(uint32_t mem_size, uint32_t page_index)
{
    uint32_t remaining = mem_size - page_index * PAGE_SIZE;
    return remaining < PAGE_SIZE ? remaining : PAGE_SIZE;
}
/* --- end copy --- */

static int failures = 0;
static void check(const char *name, int cond)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond)
    {
        failures++;
    }
}

/* Sums chunk_for_page() across every page pages_needed() says is
   needed, and checks it equals mem_size exactly - the real invariant
   try_exec()'s copy loop depends on (every real byte copied exactly
   once, nothing skipped, nothing copied twice). */
static uint32_t total_copied(uint32_t mem_size)
{
    uint32_t pages = pages_needed(mem_size);
    uint32_t total = 0;
    for (uint32_t p = 0; p < pages; p++)
    {
        total += chunk_for_page(mem_size, p);
    }
    return total;
}

int main(void)
{
    check("exactly one page needed for a 1-byte app", pages_needed(1) == 1);
    check("exactly one page needed for exactly PAGE_SIZE bytes", pages_needed(PAGE_SIZE) == 1);
    check("exactly two pages needed for PAGE_SIZE+1 bytes", pages_needed(PAGE_SIZE + 1) == 2);
    check("16 pages needed for a full 64KB app (EXEC_BUF_SIZE)",
          pages_needed(64u * 1024u) == 16);
    check("zero pages needed for a zero mem_size (shouldn't happen in "
          "practice - load_pexe_blob rejects mem_size==0 - but the "
          "arithmetic itself shouldn't underflow if it ever did)",
          pages_needed(0) == 0);

    check("a whole PAGE_SIZE chunk for a full first page of a big app",
          chunk_for_page(64u * 1024u, 0) == PAGE_SIZE);
    check("a partial chunk for the last page of a non-page-aligned app",
          chunk_for_page(PAGE_SIZE + 100, 1) == 100);
    check("a full chunk for the last page of an exactly-page-aligned app",
          chunk_for_page(PAGE_SIZE * 2, 1) == PAGE_SIZE);

    check("every byte of a 1-byte app gets copied exactly once", total_copied(1) == 1);
    check("every byte of an exactly-page-aligned app gets copied exactly once",
          total_copied(PAGE_SIZE * 3) == PAGE_SIZE * 3);
    check("every byte of a non-page-aligned app gets copied exactly once",
          total_copied(PAGE_SIZE * 3 + 777) == PAGE_SIZE * 3 + 777);
    check("every byte of a full 64KB app gets copied exactly once",
          total_copied(64u * 1024u) == 64u * 1024u);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}