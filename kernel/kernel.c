/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/arch/x86/gdt.h"
#include "kernel/arch/x86/idt.h"
#include "kernel/proc/syscall.h"
#include "kernel/drivers/pic/pic.h"
#include "kernel/drivers/keyboard/keyboard.h"
#include "kernel/drivers/mouse/mouse.h"
#include "filesys/fs.h"
#include "kernel/proc/loader.h"
#include "kernel/ui/fb.h"
#include "kernel/ui/terminal.h"
#include "shell/shell.h"
#include "kernel/proc/task.h"
#include "kernel/drivers/pit/pit.h"
#include "kernel/ui/window.h"
#include "kernel/arch/x86/paging.h"
#include "kernel/proc/pexe.h"
#include "kernel/drivers/rtl8139/rtl8139.h"
#include "kernel/drivers/net/net.h"
#include "kernel/drivers/net/internet_layer/ipv4.h"
#include <stdint.h>

extern uint32_t __bss_start;
extern uint32_t __bss_end;

void terminal_update(void);   // render if dirty, called from main loop

/* boot.asm jumps to the raw address KERNEL_OFFSET (0x8000) after loading
   the kernel binary - it has no symbol table, no ELF entry point, it just
   executes whatever machine code physically starts there. This pins
   kernel_main's own machine code to a dedicated section that linker.ld
   places first in the image, so it is ALWAYS what's at 0x8000 no matter
   what the compiler does with everything else in this file (or any other
   file added to the build). Without this, adding so much as one static
   helper function above kernel_main in this file is enough for the
   compiler to legally place that function's code first instead - which is
   exactly what happened with an earlier demo: the CPU booted straight
   into a demo task's infinite loop and never ran kernel_main at all, so
   fb_init/terminal_init never happened - hence a plain black screen with
   no crash, no reset, nothing. */

__attribute__((section(".text.kentry")))
void kernel_main(void) {
    uint32_t *p = &__bss_start;
    while (p < &__bss_end) *p++ = 0;

    /* Replaces boot.asm's minimal 2-entry GDT with the real one (kernel
       code/data at the same selector values, plus user code/data and a
       TSS) - see gdt.c. Must run before idt_init() installs any gates
       referencing GDT_KERNEL_CODE_SEL. */
    gdt_init();
    idt_init();
    /* Installs the int 0x80 syscall gate at DPL=3. try_exec() (loader.c)
       runs every app as a real ring3 task via task_create_app_ring3(),
       so this gate's callers are genuine untrusted ring3 code, not a
       hypothetical - see syscall.c's user_range_ok()/user_str_ok() for
       how pointer arguments coming through it get validated. */
    syscall_init();
    pic_remap(0x20, 0x28);
    keyboard_init();
    mouse_init();
    fs_init();

    fb_init();
    terminal_init();        // no arguments

    /* Needs idt_init()/pic_remap() (installs its own IRQ gate and
       unmasks the right PIC line) already done above, and terminal_init
       just above THIS for its own "found the card" diagnostic. Doesn't
       need paging_init() first - paging isn't even turned on yet at
       this point in boot, so rx_buffer/tx_buffers' addresses are just
       plain physical addresses as far as the card's DMA is concerned,
       exactly as true before paging_init() runs as after (once it does,
       they stay valid too, since the identity map keeps virtual ==
       physical for ordinary kernel statics like these). */
    rtl8139_init();

    /* Brings up the whole networking stack: Ethernet/ARP/IPv4/UDP/TCP/
       DNS - see net.h. 10.0.2.15 matches QEMU's default usermode NIC
       subnet (`-nic user,model=rtl8139`), where 10.0.2.2 is the virtual
       gateway and 10.0.2.3 is the built-in DNS proxy net_init() points
       DNS at by default. Without this call, ipv4_get_address() stays 0
       and every outgoing packet claims to be FROM 0.0.0.0 - which is
       exactly the kind of source address a real remote host has no way
       to send a reply back to, so ICMP echo requests would leave the
       NIC but any reply would have nowhere valid to go. Must run after
       rtl8139_init() (net_init() doesn't touch the driver itself, but
       every layer above it assumes the card is already up by the time
       anything tries to send). */
    net_init(IPV4_ADDR(10, 0, 2, 15));

    paging_init();          // phase 1 of virtual memory: identity-mapped, no isolation yet

    /* Ring3 app code/data needs these fixed regions to be genuinely
       user-accessible - see linker.ld's own comments on each. Runs once
       here since paging_clone_kernel_directory() shares these same
       underlying tables across every task (see paging.c's Phase 2
       notes), so there's no per-task work to redo later. Uses the
       linker's own start/end symbols rather than guessed sizes - each
       section's real size depends on exactly what got linked into it,
       which a hardcoded byte count would silently drift out of sync
       with the moment anything in kernel/user_trampolines.c or
       loader.c's app_stack/argv area changes size. */
    extern uint8_t __appbuf_start, __appbuf_end;
    extern uint8_t __appstack_start, __appstack_end;
    extern uint8_t __usertramp_start, __usertramp_end;
    paging_mark_kernel_region_user((uint32_t)&__appbuf_start, (uint32_t)(&__appbuf_end - &__appbuf_start));
    paging_mark_kernel_region_user((uint32_t)&__appstack_start, (uint32_t)(&__appstack_end - &__appstack_start));
    paging_mark_kernel_region_user((uint32_t)&__usertramp_start, (uint32_t)(&__usertramp_end - &__usertramp_start));

    /* Must run after paging_init() (needs a real CR3 to give the new
       TSS) and before task_init()'s guard pages could ever actually be
       hit - see gdt.c's df_handler_init and paging.c's df_fault_handler
       for what this sets up (a hardware task gate for #DF, so a stack
       overflow that outruns even its own exception frame still gets
       reported instead of triple-faulting the machine). */
    df_handler_init(paging_kernel_directory_phys());

    loader_init();
    shell_init();           // does NOT call terminal_init() anymore

    wm_init();               // compositor state, ready for a userland WM later
    task_init();             // claims the current (kernel_main) context as task 0

    pit_init(100);           // 100 Hz preemption tick

    asm volatile("sti");

    while (1) {
        if (shell_has_pending_command())
            shell_run_pending_command();
        terminal_update();
        net_poll();          // pumps the driver + drives TCP retransmit timers - see net.h
        asm volatile("hlt");
    }
}