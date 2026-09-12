/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/mouse/mouse.h"
#include "asm/io.h"
#include "kernel/arch/x86/idt.h"
#include <stdint.h>

#define MOUSE_PORT_CMD 0x64
#define MOUSE_PORT_DATA 0x60
#define IRQ12 0x2C /* IRQ12 is line 4 on the SLAVE PIC: 0x28 (slave offset) + 4 */

static int wheel_enabled = 0;
static int packet_size = 3;
static uint8_t packet[4];
static int packet_index = 0;

static void mouse_wait_write(void)
{
    /* Wait until it's safe to write: controller input buffer empty (bit1==0). */
    int timeout = 100000;
    while (timeout--)
    {
        if (!(inb(MOUSE_PORT_CMD) & 0x02))
        {
            return;
        }
    }
}

static void mouse_wait_read(void)
{
    /* Wait until there's data waiting to be read (bit0==1). */
    int timeout = 100000;
    while (timeout--)
    {
        if (inb(MOUSE_PORT_CMD) & 0x01)
        {
            return;
        }
    }
}

static void mouse_write(uint8_t val)
{
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0xD4); /* next byte written to 0x60 goes to the aux device */
    mouse_wait_write();
    outb(MOUSE_PORT_DATA, val);
}

static uint8_t mouse_read(void)
{
    mouse_wait_read();
    return inb(MOUSE_PORT_DATA);
}

static void mouse_set_sample_rate(uint8_t rate)
{
    mouse_write(0xF3);
    mouse_read(); /* ACK */
    mouse_write(rate);
    mouse_read(); /* ACK */
}

extern void mouse_isr(void);

void mouse_init(void)
{
    /* Enable the auxiliary (mouse) device on the 8042 controller. */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0xA8);

    /* Enable IRQ12 and the mouse clock in the controller's config byte. */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0x20); /* "read command byte" */
    mouse_wait_read();
    uint8_t status = inb(MOUSE_PORT_DATA);
    status |= 0x02;            /* enable IRQ12 */
    status &= (uint8_t)~0x20;  /* enable the mouse clock (clear "disabled") */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0x60); /* "write command byte" */
    mouse_wait_write();
    outb(MOUSE_PORT_DATA, status);

    /* Reset to defaults. */
    mouse_write(0xF6);
    mouse_read(); /* ACK */

    /* Standard magic sequence to negotiate Intellimouse wheel support:
       set the sample rate to 200, then 100, then 80, then ask for the
       device ID. A plain PS/2 mouse ignores this and still reports ID 0;
       one that understands it switches to wheel mode and reports ID 3,
       after which every packet gains a 4th byte carrying wheel delta. */
    mouse_set_sample_rate(200);
    mouse_set_sample_rate(100);
    mouse_set_sample_rate(80);
    mouse_write(0xF2); /* get device ID */
    mouse_read();      /* ACK */
    uint8_t id = mouse_read();
    if (id == 3)
    {
        wheel_enabled = 1;
        packet_size = 4;
    }

    /* Start streaming movement/button/wheel packets. */
    mouse_write(0xF4);
    mouse_read(); /* ACK */

    packet_index = 0;

    idt_set_gate(IRQ12, (uint32_t)mouse_isr, 0x08, 0x8E);
}

void mouse_handler(void)
{
    uint8_t data = inb(MOUSE_PORT_DATA);

    /* Byte 0 of every packet always has bit 3 set; if we see a byte
       without it where we expect byte 0, we're out of sync with the
       stream (e.g. a byte got missed) -- discard until back in sync
       rather than misinterpreting an X/Y/wheel byte as button flags. */
    if (packet_index == 0 && !(data & 0x08))
    {
        return;
    }

    packet[packet_index++] = data;
    if (packet_index < packet_size)
    {
        return;
    }
    packet_index = 0;

    /* NOTE: a complete packet (movement + buttons, and wheel delta in
       packet[3] when wheel_enabled) is sitting in `packet` right here,
       but nothing currently reads it - see the doc comment on
       mouse_handler() in mouse.h. */
}