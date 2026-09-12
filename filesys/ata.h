/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* Read/write single 512-byte sectors using ATA PIO mode.
   lba  : 28-bit logical block address
   buf  : must be exactly 512 bytes
   Returns 0 on success, -1 on error. */
int ata_read_sector(uint32_t lba, uint8_t *buf);
int ata_write_sector(uint32_t lba, const uint8_t *buf);

#endif