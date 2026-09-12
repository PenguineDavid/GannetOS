/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MOUSE_H
#define MOUSE_H

/* Initializes the PS/2 auxiliary device, enables IRQ12, and attempts to
   negotiate Intellimouse wheel reporting (4-byte packets instead of 3).
   Must be called after idt_init/pic_remap. */
void mouse_init(void);

/* Called from the IRQ12 ISR (see shell/isr_wrappers.s). Reads one byte
   from the controller and accumulates it into a 3-byte packet (4 bytes
   if wheel reporting was negotiated in mouse_init()), resyncing on any
   byte that doesn't look like packet start.
   NOTE: this used to drive the terminal scrollback view on wheel
   movement, referencing a shell/vga.h that no longer exists in this
   tree - kernel/ui/terminal.c has no scrollback buffer today, just a
   fixed grid that scrolls history away, and terminal_scroll_reset() is
   a no-op. The completed packet is currently discarded: X/Y motion,
   button state, and wheel delta are all parsed off the wire (to stay
   in sync with the stream) but nothing reads them afterward. Wire this
   up to whatever the real scrollback/cursor story ends up being before
   relying on mouse input for anything. */
void mouse_handler(void);

#endif