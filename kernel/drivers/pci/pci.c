/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/pci/pci.h"
#include "asm/io.h"

/* Packs bus/device/function/register into the CONFIG_ADDRESS format:
   bit 31 = enable, bits 23-16 = bus, 15-11 = device, 10-8 = function,
   7-0 = register (offset, low 2 bits forced to 0 - config space is
   accessed in 32-bit-aligned dwords, even when the caller only wants
   16 or 8 bits out of one - see the 16-bit helpers below for how they
   pull their piece out of the dword this returns). */
static uint32_t pci_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (1u << 31)
         | ((uint32_t)bus << 16)
         | ((uint32_t)(device & 0x1F) << 11)
         | ((uint32_t)(function & 0x7) << 8)
         | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value)
{
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t dword = pci_config_read32(bus, device, function, offset & 0xFC);
    /* offset's low 2 bits pick which 16 bits of the dword we actually
       wanted - config space is little-endian, so offset 0 wants the low
       half, offset 2 wants the high half (offsets 1 and 3 aren't valid
       starting points for a 16-bit field and aren't handled specially -
       every caller in this codebase uses even offsets, matching how the
       PCI spec defines its 16-bit registers). */
    return (uint16_t)(dword >> ((offset & 0x2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value)
{
    uint32_t dword = pci_config_read32(bus, device, function, offset & 0xFC);
    uint32_t shift = (offset & 0x2) * 8;
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_config_write32(bus, device, function, offset & 0xFC, dword);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out)
{
    for (uint32_t bus = 0; bus < 256; bus++)
    {
        for (uint32_t device = 0; device < 32; device++)
        {
            /* Function 0 always exists if the slot is populated at
               all - a device with no function 0 is an empty slot, not
               worth probing functions 1-7 of. Multi-function devices
               are rare enough for the hardware this driver targets
               (one NIC) that skipping them entirely here is a
               reasonable simplification, not a real limitation yet. */
            uint16_t vid = pci_config_read16((uint8_t)bus, (uint8_t)device, 0, 0x00);
            if (vid == 0xFFFF)
            {
                continue; /* no device in this slot */
            }

            uint16_t did = pci_config_read16((uint8_t)bus, (uint8_t)device, 0, 0x02);
            if (vid == vendor_id && did == device_id)
            {
                out->bus = (uint8_t)bus;
                out->device = (uint8_t)device;
                out->function = 0;
                out->vendor_id = vid;
                out->device_id = did;
                out->bar0 = pci_config_read32((uint8_t)bus, (uint8_t)device, 0, 0x10);
                out->interrupt_line = (uint8_t)pci_config_read16((uint8_t)bus, (uint8_t)device, 0, 0x3C);
                return 1;
            }
        }
    }
    return 0;
}

void pci_enable_bus_mastering(const pci_device_t *dev)
{
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, 0x04);
    cmd |= (1 << 2); /* bus master enable */
    pci_config_write16(dev->bus, dev->device, dev->function, 0x04, cmd);
}