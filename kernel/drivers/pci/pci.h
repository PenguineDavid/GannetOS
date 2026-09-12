/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* Legacy PCI configuration mechanism #1 - the two-port interface every
   x86 chipset still supports, PCI Express host bridges included. Not
   the fastest or most modern way to talk to PCI configuration space
   (MMCONFIG/ECAM supersedes it), but it's the one that works
   unconditionally, needs no prior discovery step to find its own
   address, and is exactly what every OS-dev PCI tutorial - and QEMU's
   own emulated chipset - expects.
   How it works: write a "config address" (bus/device/function/register,
   packed into a 32-bit value with the enable bit set) to CONFIG_ADDRESS,
   then read or write the corresponding 32-bit value at CONFIG_DATA -
   the chipset decodes the address you wrote and routes the CONFIG_DATA
   access to the right device's configuration space. */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* One matched device, filled in by pci_find_device(). bus/device/
   function identify WHERE it lives on the bus (needed to address it
   again later, e.g. to enable bus mastering); bar0 and interrupt_line
   are the two fields almost every simple PCI device driver needs
   immediately: bar0 for where its registers actually are (see the
   PCI_BAR_IS_IO note below), interrupt_line for which IRQ to expect it
   on (BIOS/chipset-assigned, read out of config space - never guessed
   or hardcoded, since it varies by machine/emulator/PCI slot). */
typedef struct
{
    uint8_t  bus, device, function;
    uint16_t vendor_id, device_id;
    uint32_t bar0;
    uint8_t  interrupt_line;
} pci_device_t;

/* PCI_BAR_IS_IO(bar0): a BAR's low bit tells you whether it's an I/O
   space address (bit 0 = 1 - the low 2 bits are then reserved/flags,
   mask them off with ~0x3) or a memory space address (bit 0 = 0, low 4
   bits are flags, mask with ~0xF). RTL8139 exposes both an I/O BAR
   (BAR0) and a memory BAR (BAR1); this driver uses the I/O one since
   plain in/out instructions are simpler than setting up an extra
   mapping for MMIO this early - same reasoning as everything else so
   far preferring simplicity over performance. */
#define PCI_BAR_IS_IO(bar0)     ((bar0) & 0x1)
#define PCI_BAR_IO_ADDR(bar0)   ((uint16_t)((bar0) & ~0x3u))
#define PCI_BAR_MEM_ADDR(bar0)  ((bar0) & ~0xFu)

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);

/* Walks every bus/device/function (a brute-force scan of all 256x32x8
   possible slots - slow in the abstract, but this only ever runs once
   at boot and even a full scan is a few thousand port I/O round trips,
   negligible next to everything else boot already does) looking for a
   vendor_id/device_id match. Fills *out and returns 1 on success, 0 if
   nothing matching was found (e.g. no NIC attached to this QEMU
   instance, or a different model than the caller asked for). */
int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);

/* Sets the bus-mastering bit in a device's PCI command register (offset
   0x04, bit 2) - required before a device can DMA on its own (write to
   host memory without the CPU driving every transfer), which RTL8139's
   TX/RX ring buffers both depend on. Without this, the card silently
   fails to actually move any data even though every register write to
   it appears to succeed. */
void pci_enable_bus_mastering(const pci_device_t *dev);

#endif