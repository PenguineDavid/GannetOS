/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef SHELL_H
#define SHELL_H

void shell_init(void);
void shell_handle_char(char c);
void shell_handle_key(int key); /* special keys from keyboard.h defines */

/* A completed command line is only ever flagged here by shell_handle_char
   (called from the keyboard ISR); the actual dispatch runs from the
   kernel's main loop instead of from inside the ISR. Interrupt gates
   clear IF on entry and don't restore it until iret, so running a
   command synchronously from ISR context would mean any app that itself
   needs to block waiting for further keyboard input (see kernel/loader.c,
   shell/keyboard.c's raw input mode) could never receive it -- no
   interrupt, including the very next keystroke, could ever reach the
   CPU to wake it back up. See kernel/kernel.c's main loop. */
int shell_has_pending_command(void);
void shell_run_pending_command(void);

#endif