/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* int 0x80 syscall gate. This is the mechanism that will eventually let
   ring3 apps/WMs talk to the kernel at all, once app execution actually
   flips to CPL3 - see loader.c and pexe.h/wm_api.h for why the CURRENT
   direct-function-pointer api structs cannot survive that flip unchanged
   (calling a raw kernel function pointer from CPL3 code doesn't raise
   privilege - it just runs at CPL3 and #GPs the moment it hits anything
   privileged).

   ABI: eax = syscall number on entry, args in ebx/ecx/edx/esi/edi (up to
   5, unused ones ignored by whichever handler doesn't need them), return
   value in eax on return. ebx/ecx/edx/esi/edi are left exactly as the
   caller set them (the ISR stub in shell/isr_wrappers.s only ever pushes
   copies of them, never assigns back into the registers) - only eax
   changes across the call.

   Gate is installed with DPL=3 (see syscall_init()) so ring3 code is
   actually allowed to trigger it; nothing stops ring0 code from doing
   `int $0x80` too, which is how this gate gets smoke-tested before any
   ring3 task exists to call it for real. */

enum
{
    SYS_PUTS = 0,
    SYS_PUTCHAR,
    SYS_PUTS_COL,
    SYS_GETCHAR,
    SYS_SPAWN_TASK,
    SYS_TASK_YIELD,
    SYS_EXIT_TASK,
    SYS_FS_OPEN,
    SYS_FS_READ,
    SYS_FS_WRITE,
    SYS_FS_CLOSE,
    SYS_FS_STAT,
    SYS_FS_INODE_COUNT,
    SYS_FS_GET_PARENT,
    SYS_FS_DELETE,
    SYS_FS_MKDIR,
    SYS_FS_RMDIR,
    SYS_FS_RRMDR,
    SYS_FS_ISDIR,
    SYS_READ_LINE,
    SYS_GET_TIME,
    SYS_HAS_STDIN,
    SYS_READ_STDIN,
    SYS_DNS_RESOLVE,
    SYS_ICMP_PING,
    SYS_HTTP_GET,
    SYS_COUNT
};

/* Installs the int 0x80 gate. Must run after idt_init() (it overwrites
   idt_init()'s generic default-handler gate for just this one vector,
   same pattern paging_init() already uses for vector 14) and after
   gdt_init() (GDT_KERNEL_CODE_SEL must be a live, correct selector by
   the time this gate is installed, since that's what CS gets reloaded to
   on entry into syscall_isr). */
void syscall_init(void);

/* Called only from syscall_isr (shell/isr_wrappers.s), never directly
   from C. Looks `num` up in the internal dispatch table and calls the
   matching handler with a1..a5, or returns (uint32_t)-1 for an unknown
   syscall number. */
uint32_t syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

#endif