/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/mem/pmm.h"

#define POOL_FRAMES ((PMM_POOL_END - PMM_POOL_START) / PMM_FRAME_SIZE)
#define BITMAP_WORDS ((POOL_FRAMES + 31) / 32)

static uint32_t bitmap[BITMAP_WORDS];

static void bitmap_set(uint32_t index)
{
    bitmap[index / 32] |= (1u << (index % 32));
}

static void bitmap_clear(uint32_t index)
{
    bitmap[index / 32] &= ~(1u << (index % 32));
}

static int bitmap_test(uint32_t index)
{
    return (int)((bitmap[index / 32] >> (index % 32)) & 1u);
}

void pmm_init(void)
{
    for (uint32_t i = 0; i < BITMAP_WORDS; i++)
    {
        bitmap[i] = 0;
    }
}

uint32_t pmm_alloc_frame(void)
{
    for (uint32_t i = 0; i < POOL_FRAMES; i++)
    {
        if (!bitmap_test(i))
        {
            bitmap_set(i);
            uint32_t physical_address = PMM_POOL_START + i * PMM_FRAME_SIZE;
            /* Zeroing via the frame's own physical address as a pointer
               relies on this whole pool being identity-mapped by
               paging.c - see PAGING_IDENTITY_LOW_BYTES there, which
               covers the entire pool for exactly this reason. */
            uint8_t *frame_bytes = (uint8_t *)physical_address;
            for (uint32_t b = 0; b < PMM_FRAME_SIZE; b++)
            {
                frame_bytes[b] = 0;
            }
            return physical_address;
        }
    }
    return 0;
}

void pmm_free_frame(uint32_t phys_addr)
{
    if (phys_addr < PMM_POOL_START || phys_addr >= PMM_POOL_END)
    {
        return;
    }
    uint32_t index = (phys_addr - PMM_POOL_START) / PMM_FRAME_SIZE;
    bitmap_clear(index);
}