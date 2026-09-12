/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>

/* Physical memory manager: a bitmap allocator over a fixed pool of RAM,
   used to hand out distinct physical frames to different tasks. This is
   what phase 2 (per-task page directories) was missing to become real
   isolation - phase 2 gave every task its own directory, but every
   directory pointed at the SAME page tables, so two tasks writing to
   "their own" memory were still writing to the same physical RAM.

   KNOWN LIMITATION: the pool is a hardcoded physical range
   (PMM_POOL_START..PMM_POOL_END below), not derived from a real BIOS
   memory map, for the same reason paging.c's identity-map range is
   hardcoded - boot.asm doesn't fetch one yet. The pool sits comfortably
   above everything paging.c identity-maps for the kernel today (fb
   backbuffer ends well under 32MB; the pool starts at 32MB), so there's
   no collision with existing static structures, but it does assume at
   least PMM_POOL_END bytes of RAM exist - raise -m accordingly if you
   lower the pool end below what you need, or vice versa. */

#define PMM_FRAME_SIZE 4096u
#define PMM_POOL_START 0x02000000u  /* 32MB */
#define PMM_POOL_END   0x04000000u  /* 64MB - matches Makefile's `-m 64` */

/* Call once, before anything tries to allocate a frame. */
void pmm_init(void);

/* Returns the physical address of a free, zeroed 4KB frame, or 0 if the
   pool is exhausted. */
uint32_t pmm_alloc_frame(void);

/* Returns a frame to the pool. Safe to call with 0 (no-op). */
void pmm_free_frame(uint32_t phys_addr);

#endif