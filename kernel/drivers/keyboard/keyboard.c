/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/keyboard/keyboard.h"
#include "shell/shell.h"
#include "kernel/arch/x86/idt.h"
#include "kernel/drivers/pic/pic.h"
#include "asm/io.h"
#include <stdint.h>

#define IRQ1 0x21

static const char scancode_ascii[] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0};

static const char scancode_ascii_shift[] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0};

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LSHIFT_REL 0xAA
#define SC_RSHIFT_REL 0xB6
#define SC_CAPS 0x3A
#define SC_EXTENDED 0xE0
#define SC_EXT_UP 0x48
#define SC_EXT_DOWN 0x50
#define SC_EXT_LEFT 0x4B
#define SC_EXT_RIGHT 0x4D
#define SC_EXT_HOME 0x47
#define SC_EXT_END 0x4F
#define SC_EXT_DEL 0x53

static uint8_t shift_held = 0;
static uint8_t caps_on = 0;
static uint8_t extended = 0;

/* Raw input mode */
#define KB_RAW_BUF_SIZE 256
static volatile uint8_t raw_mode = 0;
static char raw_buf[KB_RAW_BUF_SIZE];
static volatile int raw_head = 0;
static volatile int raw_tail = 0;

static void raw_push(char c)
{
    int next = (raw_tail + 1) % KB_RAW_BUF_SIZE;
    if (next != raw_head)
    {
        raw_buf[raw_tail] = c;
        raw_tail = next;
    }
}

void kb_set_raw_mode(int on)
{
    raw_mode = on ? 1 : 0;
    raw_head = raw_tail = 0;
}

int kb_raw_getchar(void)
{
    while (raw_head == raw_tail)
    {
        asm volatile("hlt");
    }
    char c = raw_buf[raw_head];
    raw_head = (raw_head + 1) % KB_RAW_BUF_SIZE;
    return (int)(unsigned char)c;
}

/* Same as kb_raw_getchar but never blocks: returns -1 immediately if
   nothing is pending. Needed by anything that has other work to do every
   loop iteration (like presenting a frame) and can't afford to sit in
   kb_raw_getchar's hlt-loop until a key shows up. */
int kb_raw_getchar_nonblocking(void)
{
    if (raw_head == raw_tail)
    {
        return -1;
    }
    char c = raw_buf[raw_head];
    raw_head = (raw_head + 1) % KB_RAW_BUF_SIZE;
    return (int)(unsigned char)c;
}

#define SC_F12 0x58

/* Set the instant F12 is pressed, regardless of raw_mode or anything else -
   this is the reserved "reclaim control from whatever's running" hotkey.
   A userland WM polls this via its wm_api's should_quit() to know when to
   stop and hand control back to the shell; nothing else in the kernel
   clears it, so the WM (or whoever's using kb_raw mode) is responsible for
   checking it and calling wm_stop_ack() once handled. */
volatile int wm_stop_requested = 0;

void wm_stop_ack(void)
{
    wm_stop_requested = 0;
}

void keyboard_handler(void)
{
    uint8_t scancode = inb(0x60);

    if (scancode == SC_F12)
    {
        wm_stop_requested = 1;
        pic_send_eoi(1);
        return;
    }

    if (scancode == SC_EXTENDED)
    {
        extended = 1;
        pic_send_eoi(1);
        return;
    }

    if (extended)
    {
        extended = 0;
        if (!(scancode & 0x80) && !raw_mode)
        {
            switch (scancode)
            {
                case SC_EXT_LEFT:
                    shell_handle_key(SHELL_KEY_LEFT);
                    break;
                case SC_EXT_RIGHT:
                    shell_handle_key(SHELL_KEY_RIGHT);
                    break;
                case SC_EXT_UP:
                    shell_handle_key(SHELL_KEY_UP);
                    break;
                case SC_EXT_DOWN:
                    shell_handle_key(SHELL_KEY_DOWN);
                    break;
                case SC_EXT_HOME:
                    shell_handle_key(SHELL_KEY_HOME);
                    break;
                case SC_EXT_END:
                    shell_handle_key(SHELL_KEY_END);
                    break;
                case SC_EXT_DEL:
                    shell_handle_key(SHELL_KEY_DEL);
                    break;
                default:
                    break;
            }
        }
        pic_send_eoi(1);
        return;
    }

    if (scancode == SC_LSHIFT || scancode == SC_RSHIFT)
    {
        shift_held = 1;
        pic_send_eoi(1);
        return;
    }
    if (scancode == SC_LSHIFT_REL || scancode == SC_RSHIFT_REL)
    {
        shift_held = 0;
        pic_send_eoi(1);
        return;
    }
    if (scancode == SC_CAPS)
    {
        caps_on = !caps_on;
        pic_send_eoi(1);
        return;
    }

    if (scancode & 0x80)
    {
        pic_send_eoi(1);
        return;
    }

    if (scancode < sizeof(scancode_ascii))
    {
        int use_shift = shift_held;
        if (caps_on && scancode >= 0x10 && scancode <= 0x32)
        {
            char base = scancode_ascii[scancode];
            if (base >= 'a' && base <= 'z')
            {
                use_shift = !use_shift;
            }
        }
        char ascii = use_shift ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
        if (ascii)
        {
            if (raw_mode)
            {
                raw_push(ascii);
            }
            else
            {
                shell_handle_char(ascii);
            }
        }
    }

    pic_send_eoi(1);
}

extern void keyboard_isr(void);

void keyboard_init(void)
{
    idt_set_gate(IRQ1, (uint32_t)keyboard_isr, 0x08, 0x8E);
}