/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/rtl8139/rtl8139.h"
#include "kernel/drivers/pci/pci.h"
#include "kernel/drivers/pic/pic.h"
#include "kernel/arch/x86/idt.h"
#include "asm/io.h"
#include "kernel/ui/terminal.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Registers, all offsets from the card's I/O base (BAR0).             */
/* ------------------------------------------------------------------ */
#define REG_IDR0     0x00 /* MAC address, 6 bytes */
#define REG_TSD0     0x10 /* Transmit Status Descriptor 0-3, 4 bytes apart */
#define REG_TSAD0    0x20 /* Transmit Start Address of Descriptor 0-3 */
#define REG_RBSTART  0x30 /* Receive Buffer Start Address */
#define REG_CAPR     0x38 /* Current Address of Packet Read (RX ring read pointer) */
#define REG_CMD      0x37
#define REG_IMR      0x3C /* Interrupt Mask Register */
#define REG_ISR      0x3E /* Interrupt Status Register */
#define REG_TCR      0x40
#define REG_RCR      0x44
#define REG_CONFIG1  0x52

#define CMD_BUFE 0x01 /* RX buffer empty (read-only status bit) */
#define CMD_TE   0x04 /* transmit enable */
#define CMD_RE   0x08 /* receive enable */
#define CMD_RST  0x10 /* reset (self-clears when done) */

#define ISR_ROK 0x0001 /* receive OK */
#define ISR_TOK 0x0004 /* transmit OK */

/* RX ring: 8192 bytes of real ring + 16 bytes + 1500 bytes of overflow
   headroom. The overflow headroom exists so a packet arriving right at
   the end of the ring can be written out in one contiguous DMA burst
   instead of split across the wrap point - the RCR_WRAP bit below tells
   the card it's allowed to do that (i.e. that this extra space exists
   and is safe to write into), which is what makes RX parsing below not
   have to special-case a packet spanning the wraparound. 9709 rounded
   up to 9728 for tidy alignment; the exact padding beyond what the card
   needs doesn't matter. */
#define RX_BUF_SIZE (8192 + 16 + 1500)
static uint8_t rx_buffer[RX_BUF_SIZE] __attribute__((aligned(4)));
static uint32_t rx_read_offset = 0;

/* 4 hardware TX slots, round-robin. Each needs its OWN backing buffer -
   the card DMAs directly out of whatever physical address TSAD[slot]
   names, so that memory has to still be valid and unchanged for as long
   as a send might be in flight, which rules out reusing a single shared
   scratch buffer across slots. */
#define TX_SLOTS 4
static uint8_t tx_buffers[TX_SLOTS][RTL8139_MAX_FRAME] __attribute__((aligned(4)));
static int tx_next_slot = 0;

static uint16_t io_base = 0;
static uint8_t mac_addr[6];
static int initialized = 0;

extern void rtl8139_isr(void); /* isr_wrappers.s */
static uint8_t irq_number = 0;

/* Called from rtl8139_isr (isr_wrappers.s) on every IRQ this card
   raises. Doesn't actually move any data - rtl8139_receive() does that
   by polling, which keeps this driver's very first cut simple (no
   producer/consumer queue needed between an ISR and whatever's asking
   for packets). This still has a real job though: the card won't raise
   further interrupts until its OWN ISR register bits are acknowledged
   (written back to clear them, NOT just read - RTL8139's ISR is
   write-1-to-clear), and the PIC needs its EOI regardless of whether
   anything upstream cares about this specific interrupt. Skipping
   either one would eventually wedge the card or the PIC. */
void rtl8139_irq_handler(void)
{
    uint16_t status = inw(io_base + REG_ISR);
    outw(io_base + REG_ISR, status); /* write-1-to-clear */
    pic_send_eoi(irq_number);
}

int rtl8139_init(void)
{
    pci_device_t dev;
    if (!pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, &dev))
    {
        terminal_puts("rtl8139: no card found\n", TERMINAL_LIGHT_RED);
        return 0;
    }
    if (!PCI_BAR_IS_IO(dev.bar0))
    {
        /* Every real RTL8139 exposes an I/O BAR at BAR0 - this would
           only trip if something very unusual is emulating the device
           ID without matching the real card's BAR layout. */
        terminal_puts("rtl8139: BAR0 is not an I/O BAR, unsupported\n", TERMINAL_LIGHT_RED);
        return 0;
    }
    io_base = PCI_BAR_IO_ADDR(dev.bar0);
    irq_number = dev.interrupt_line;

    pci_enable_bus_mastering(&dev);

    /* Power on (clears any sleep/power-down state left over from
       whatever the BIOS or a previous boot left it in) - writing 0 here
       is the documented wake-up sequence, not a real "config" value. */
    outb(io_base + REG_CONFIG1, 0x00);

    /* Software reset - self-clearing bit; the card is done resetting
       once it clears CMD_RST on its own. No timeout here: on real
       hardware this could hang forever if the card never comes back,
       but for a first cut targeting QEMU (which always completes this
       essentially instantly) that risk is accepted rather than adding
       timeout/retry plumbing this early. */
    outb(io_base + REG_CMD, CMD_RST);
    while (inb(io_base + REG_CMD) & CMD_RST)
    {
    }

    /* MAC address - 6 consecutive bytes at IDR0. */
    for (int i = 0; i < 6; i++)
    {
        mac_addr[i] = inb(io_base + REG_IDR0 + i);
    }

    /* RX buffer: give the card the physical address to DMA into. Since
       paging identity-maps this whole region (rx_buffer is an ordinary
       static array within the kernel's own identity-mapped memory),
       its virtual address IS its physical address - no translation
       needed here, unlike a userspace/ring3 buffer would. */
    for (uint32_t i = 0; i < RX_BUF_SIZE; i++)
    {
        rx_buffer[i] = 0;
    }
    rx_read_offset = 0;
    outl(io_base + REG_RBSTART, (uint32_t)rx_buffer);

    /* IMR: which conditions actually raise an IRQ. ROK/TOK cover the
       normal cases this driver cares about (a frame arrived / a send
       finished) - not enabling the error bits (RXOVW, TER, etc.) yet is
       a real gap (a card-side error condition would go unnoticed rather
       than logged), acceptable for a first cut, worth revisiting once
       there's a real protocol stack depending on this being reliable. */
    outw(io_base + REG_IMR, ISR_ROK | ISR_TOK);

    /* RCR: accept broadcast (AB, bit 3) + multicast (AM, bit 2) +
       physical-match (APM, bit 1) frames, and set WRAP (bit 7) so the
       card uses the RX_BUF_SIZE overflow headroom instead of splitting
       a packet across the ring's wrap point - see RX_BUF_SIZE's own
       comment. Deliberately NOT setting AAP (bit 0, accept-all/
       promiscuous) - no reason for this card to see traffic that isn't
       broadcast, multicast, or addressed to its own MAC yet. */
#define RCR_APM  (1 << 1)
#define RCR_AM   (1 << 2)
#define RCR_AB   (1 << 3)
#define RCR_WRAP (1 << 7)
    outl(io_base + REG_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);

    /* Enable RX and TX. */
    outb(io_base + REG_CMD, CMD_RE | CMD_TE);

    /* Install and unmask the IRQ - PCI-assigned interrupt_line, never
       hardcoded, since it depends on the chipset/emulator/slot (see
       pci.h's own note on this). idt_set_gate's flags (0x8E) match
       every other hardware ISR in this codebase (keyboard, mouse, PIT):
       present, ring0, 32-bit interrupt gate. */
    idt_set_gate(0x20 + irq_number, (uint32_t)rtl8139_isr, 0x08, 0x8E);
    /* Unmask this line on whichever PIC it lives on - master for
       irq_number < 8, slave (and the master's cascade line, IRQ2, which
       pic_remap already leaves unmasked) otherwise. */
    if (irq_number < 8)
    {
        uint8_t mask = inb(0x21);
        outb(0x21, mask & ~(uint8_t)(1 << irq_number));
    }
    else
    {
        uint8_t mask = inb(0xA1);
        outb(0xA1, mask & ~(uint8_t)(1 << (irq_number - 8)));
    }

    initialized = 1;

    terminal_puts("rtl8139: found, MAC ", TERMINAL_LIGHT_GREEN);
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++)
    {
        char buf[3] = { hex[mac_addr[i] >> 4], hex[mac_addr[i] & 0xF], 0 };
        terminal_puts(buf, TERMINAL_LIGHT_GREEN);
        if (i < 5)
        {
            terminal_puts(":", TERMINAL_LIGHT_GREEN);
        }
    }
    terminal_puts("\n", TERMINAL_LIGHT_GREEN);

    return 1;
}

void rtl8139_get_mac(uint8_t mac_out[6])
{
    if (!initialized)
    {
        return;
    }
    for (int i = 0; i < 6; i++)
    {
        mac_out[i] = mac_addr[i];
    }
}

int rtl8139_send(const void *data, uint16_t len)
{
    if (!initialized || len == 0 || len > RTL8139_MAX_FRAME)
    {
        return 0;
    }

    int slot = tx_next_slot;
    tx_next_slot = (tx_next_slot + 1) % TX_SLOTS;

    /* TSD's own status bit (OWN, bit 13) is 1 when the slot is free for
       the CPU to hand off a new frame, and gets cleared by the card
       while it owns the buffer, then set again once the send completes
       - so this spins until whatever was PREVIOUSLY in this slot has
       actually finished transmitting, before overwriting its buffer
       with a new frame. With only 4 slots and sends this infrequent
       (nothing above this driver yet generates sustained traffic),
       this essentially never actually spins in practice today - it's
       here for correctness as soon as something does. */
    while (!(inl(io_base + REG_TSD0 + slot * 4) & (1 << 13)))
    {
    }

    uint8_t *tx_buf = tx_buffers[slot];
    for (uint16_t i = 0; i < len; i++)
    {
        tx_buf[i] = ((const uint8_t *)data)[i];
    }

    outl(io_base + REG_TSAD0 + slot * 4, (uint32_t)tx_buf);
    /* Writing the length to TSD is what actually triggers the send -
       everything above just got the buffer ready. Ethernet has a 60-
       byte minimum frame size (before the 4-byte FCS the card appends
       itself); the card handles padding a shorter frame up to that on
       its own, so len itself doesn't need to be pre-padded here. */
    outl(io_base + REG_TSD0 + slot * 4, len);

    return 1;
}

int rtl8139_receive(void *buf)
{
    if (!initialized)
    {
        return 0;
    }

    /* CMD_BUFE is set exactly when the card considers the ring empty -
       cheaper and simpler than comparing CAPR/CBR by hand, and it's
       what the card itself is telling us, not a value this driver has
       to keep in sync independently. */
    if (inb(io_base + REG_CMD) & CMD_BUFE)
    {
        return 0;
    }

    /* Packet layout at rx_read_offset: a 4-byte header (status uint16,
       length uint16, both little-endian, length INCLUDES the 4-byte
       FCS the card appended on the wire) immediately followed by the
       frame data itself. */
    uint16_t status = rx_buffer[rx_read_offset] | (rx_buffer[rx_read_offset + 1] << 8);
    uint16_t length = rx_buffer[rx_read_offset + 2] | (rx_buffer[rx_read_offset + 3] << 8);
    (void)status; /* not checked yet - ROK-vs-error discrimination is the same gap as IMR's error bits above */

    uint16_t frame_len = length >= 4 ? (uint16_t)(length - 4) : 0; /* drop the FCS */
    uint32_t data_start = rx_read_offset + 4;

    for (uint16_t i = 0; i < frame_len && i < RTL8139_MAX_FRAME; i++)
    {
        ((uint8_t *)buf)[i] = rx_buffer[(data_start + i) % RX_BUF_SIZE];
    }

    /* Advance past this packet (header + length, rounded up to a
       4-byte boundary - the card always starts the next packet's
       header on one), wrapping back to the start of the real ring
       (RX_BUF_SIZE's overflow headroom is only ever a landing spot for
       one packet's tail, never itself a new packet's start). */
    uint32_t next = (data_start + length + 3) & ~3u;
    rx_read_offset = next % RX_BUF_SIZE;

    /* CAPR is documented as "read pointer minus 16" - a real quirk of
       this specific card, not a typo; every reference driver applies
       this same offset. */
    outw(io_base + REG_CAPR, (uint16_t)(rx_read_offset - 16));

    return frame_len;
}