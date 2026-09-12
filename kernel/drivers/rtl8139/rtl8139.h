/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>

/* Realtek RTL8139 - a classic, extremely well-documented 10/100 Mbit PCI
   NIC. Chosen for GannetOS's first network driver over anything fancier
   (e1000, virtio-net) for the same reason ne2000 usually loses out to
   THIS card specifically in OS-dev tutorials: it's DMA-based (so it
   demonstrates giving a real device physical memory to write into,
   which virtio-net would also need but with a much bigger spec to read
   first) while still being small enough to fully understand - a
   handful of I/O port registers, two fixed-size ring buffers, no
   descriptor rings or negotiation protocol to get wrong. QEMU emulates
   it well and it's what most hobby OS's first NIC driver targets.
   PCI IDs: vendor 0x10EC (Realtek), device 0x8139. */
#define RTL8139_VENDOR_ID 0x10EC
#define RTL8139_DEVICE_ID 0x8139

/* True once rtl8139_init() has found the card and brought it up. Every
   other function in this header is only meaningful after this is true -
   check it (or just check rtl8139_init()'s own return value) before
   calling any of them. */
int rtl8139_init(void);

/* Copies the card's 6-byte MAC address into mac_out (caller-owned, must
   be >= 6 bytes). Does nothing if the driver hasn't been initialized. */
void rtl8139_get_mac(uint8_t mac_out[6]);

/* Transmits one raw Ethernet frame (destination MAC + source MAC +
   EtherType/length + payload - this function doesn't build or validate
   any of that, it just hands `len` bytes starting at `data` to the NIC
   as-is). Rotates across the card's 4 hardware TX slots round-robin;
   if all 4 are still busy from earlier sends this blocks (briefly -
   spins on the current slot's own status register) until one frees up,
   rather than dropping the frame or returning an error. len must be
   <= RTL8139_MAX_FRAME. Returns 1 on success, 0 if len is out of range
   or the driver isn't initialized. */
#define RTL8139_MAX_FRAME 1792
int rtl8139_send(const void *data, uint16_t len);

/* Polls for one received frame and, if one is waiting, copies it into
   buf (caller-owned, must be >= RTL8139_MAX_FRAME bytes) and returns
   its length. Returns 0 immediately if nothing has arrived - this is a
   non-blocking poll, not a wait; callers that want to block should loop
   on this themselves (e.g. with task_yield() between calls, so other
   tasks still get scheduled while waiting). This driver is polling-only
   for RX so far even though the card's own IRQ is wired up for TX/RX
   status - see rtl8139.c's own note on why. */
int rtl8139_receive(void *buf);

#endif