/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

ENTRY(kernel_main)
SECTIONS
{
    . = 0x8000;

    .text : {
        *(.text.kentry)
        build/kernel/kernel.o(.text*)
        *(.text*)
    }

    .rodata : ALIGN(4) {
        *(.rodata*)
    }

    .data : ALIGN(4) {
        *(.data*)
    }

    /* Kernel-authored code AND data that ring3 app code must be able to
       fetch/read/write directly by a plain `call`/load/store - not via
       int 0x80. This holds: the syscall trampoline stubs (kernel/
       user_trampolines.c), the api struct they populate, and the crt0
       landing pad app_entry_trampoline iret's into (isr_wrappers.s). It
       gets PAGE_USER set on it once, at boot (see
       paging_mark_kernel_region_user in kernel.c) - everything else in
       this image stays supervisor-only.
       Placed here, before .bss, deliberately: .bss is NOBITS (no real
       file content, however large it is in memory - the page-table pool
       in kernel/paging.c alone is 128KB) and needs to stay the LAST kept
       section so objcopy -O binary can omit its zero space entirely
       instead of materializing a 100KB+ gap of real zero bytes just to
       reach something with actual content sitting after it. */
    . = ALIGN(0x1000);
    .usertramp : {
        __usertramp_start = .;
        *(.usertramp)
        *(.usertramp.data)
        __usertramp_end = .;
    }

    .bss : ALIGN(4) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    /* App execution buffer -- placed at fixed address 2MB.
       Apps are linked at this address so absolute refs work. This
       section's real content (exec_buf, loader.c) is only ever used as
       a kernel-side STAGING buffer now - try_exec() reads a pexe file
       in here, then copies it into a fresh, private, per-task physical
       frame at this SAME virtual address (see paging_map_private_page)
       before the app ever runs. The address staying fixed is exactly
       what let that become per-task without recompiling apps or
       touching this script otherwise. */
    . = 0x200000;
    .appbuf : {
        __appbuf_start = .;
        *(.appbuf)
        __appbuf_end = .;
    }

    /* Ring3 app stack region - like .appbuf above, genuinely per-task
       now (see try_exec()/paging_map_private_page again), backed by
       private physical frames at runtime, not by any symbol placed
       here at link time. Nothing is placed in this section anymore
       (there is no static app_stack[] array with a matching
       __attribute__((section(".appstack"))) left in the C code, unlike
       .appbuf's exec_buf), so unlike every other section in this
       script, this one is reserved space, not content: the explicit
       ". = . + APP_STACK_SIZE" below is REQUIRED, not decorative - an
       empty `.appstack {}` block here would make __appstack_start ==
       __appstack_end (both just left at 0x210000, since nothing
       advances `.` inside it), collapsing kernel/proc/syscall.c's
       user_range_ok()/user_str_ok() to a ZERO-BYTE valid range for
       every app's own stack. Every syscall whose pointer argument is a
       local/stack variable - which is most of them - would then fail
       that validation and silently no-op, while the app itself keeps
       running (nothing crashes) - exactly the "ls shows nothing,
       fs_get_parent returns garbage, echo prints nothing, but the
       shell/task-switch/scheduler tests all still pass" symptom this
       fixes: those are exactly the syscalls whose real arguments live
       on the app's stack, versus the ones that don't.
       APP_STACK_SIZE here (0x4000 = 16KB) MUST match APP_STACK_SIZE in
       kernel/proc/loader.c - there's no shared header between this
       script and the C sources to enforce that automatically, same
       situation as APP_LOAD_ADDR above already being duplicated
       between here and the Makefile. */
    . = 0x210000;
    .appstack : {
        __appstack_start = .;
        *(.appstack)
        . = . + 0x4000; /* APP_STACK_SIZE - keep in sync with loader.c */
        __appstack_end = .;
    }
}
