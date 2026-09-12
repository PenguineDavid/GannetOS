/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "filesys/ata.h"
#include "asm/io.h"
#include <stdint.h>

/* Primary ATA bus, master drive */
#define ATA_DATA 0x1F0
#define ATA_ERROR 0x1F1
#define ATA_SECT_COUNT 0x1F2
#define ATA_LBA_LO 0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI 0x1F5
#define ATA_DRIVE_HEAD 0x1F6
#define ATA_STATUS 0x1F7
#define ATA_COMMAND 0x1F7

#define ATA_CMD_READ 0x20
#define ATA_CMD_WRITE 0x30
#define ATA_CMD_FLUSH 0xE7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01
#define ATA_SR_DF 0x20

/*
 * Wait for BSY to clear.  Used before issuing a command and after a
 * write flush -- does NOT check DRQ.
 */
static int ata_wait_not_busy(void)
{
    int timeout = 0x100000;
    while (inb(ATA_STATUS) & ATA_SR_BSY)
    {
        if (--timeout == 0)
        {
            return -1;
        }
    }
    return 0;
}

/*
 * Wait for BSY to clear AND DRQ to set (data ready to transfer).
 * Also returns -1 on ERR or Device Fault.
 * Called after issuing a READ command.
 */
static int ata_wait_drq(void)
{
    int timeout = 0x100000;
    uint8_t status;
    for (;;)
    {
        status = inb(ATA_STATUS);
        if (status & (ATA_SR_ERR | ATA_SR_DF))
        {
            return -1;
        }
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ))
        {
            return 0;
        }
        if (--timeout == 0)
        {
            return -1;
        }
    }
}

static void ata_setup_lba(uint32_t lba, uint8_t command)
{
    outb(ATA_DRIVE_HEAD, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_ERROR, 0x00);
    outb(ATA_SECT_COUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, command);
}

int ata_read_sector(uint32_t lba, uint8_t *buf)
{
    /* Wait for drive to be idle before sending the command. */
    if (ata_wait_not_busy() != 0)
    {
        return -1;
    }

    ata_setup_lba(lba, ATA_CMD_READ);

    /*
     * After issuing READ, wait for DRQ (data ready), not just BSY-clear.
     * The drive asserts BSY briefly after the command, then clears BSY
     * and asserts DRQ when the sector buffer is filled.  Reading before
     * DRQ is set pulls garbage off the bus.
     */
    if (ata_wait_drq() != 0)
    {
        return -1;
    }

    uint16_t *words = (uint16_t *)buf;
    for (int i = 0; i < 256; i++)
    {
        uint16_t val;
        asm volatile("inw %1, %0" : "=a"(val) : "Nd"((uint16_t)ATA_DATA));
        words[i] = val;
    }
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t *buf)
{
    if (ata_wait_not_busy() != 0)
    {
        return -1;
    }

    ata_setup_lba(lba, ATA_CMD_WRITE);

    /* For writes, DRQ signals the drive is ready to accept data. */
    if (ata_wait_drq() != 0)
    {
        return -1;
    }

    const uint16_t *words = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++)
    {
        uint16_t val = words[i];
        asm volatile("outw %0, %1" : : "a"(val), "Nd"((uint16_t)ATA_DATA));
    }

    /* Flush write cache and wait for completion. */
    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    ata_wait_not_busy();
    return 0;
}