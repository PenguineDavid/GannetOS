/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

/* Special key codes passed to shell_handle_key */
#define SHELL_KEY_LEFT 1
#define SHELL_KEY_RIGHT 2
#define SHELL_KEY_UP 3
#define SHELL_KEY_DOWN 4
#define SHELL_KEY_HOME 5
#define SHELL_KEY_END 6
#define SHELL_KEY_DEL 7

void keyboard_init(void);
void keyboard_handler(void);

/* Raw input mode -- see shell/keyboard.c for details. Used by loader.c
   to give a running app a live, unbuffered-by-the-shell keystroke feed. */
void kb_set_raw_mode(int on);
int kb_raw_getchar(void);
int kb_raw_getchar_nonblocking(void);

/* Reserved "stop the running window manager" hotkey (F12), tracked
   regardless of raw mode or anything else currently reading the keyboard.
   wm_stop_requested is set the instant F12 is pressed; a running WM polls
   it (via wm_api's should_quit()) and calls wm_stop_ack() once it has
   actually stopped, clearing the flag for next time. */
extern volatile int wm_stop_requested;
void wm_stop_ack(void);

#endif