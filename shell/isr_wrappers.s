# Copyright (C) 2026 David S
# SPDX-License-Identifier: GPL-3.0-only

# shell/isr_wrappers.s

.section .text
.code32

# Common tail: send EOI to both PICs in case this was a hardware IRQ,
# restore registers, and return. Entered with a clean pushal-only frame
# sitting under the CPU's own EIP/CS/EFLAGS push -- no leftover error code.
.global isr_default_common
.type isr_default_common, @function
isr_default_common:
    pushal
    movb $0x20, %al
    outb %al, $0xA0
    outb %al, $0x20
    popal
    iretl

# Default handler for vectors that do NOT push a CPU error code
# (hardware IRQs and most exceptions). Falls straight into the common tail.
.global isr_default
.type isr_default, @function
isr_default:
    jmp isr_default_common

# Stub for exceptions that DO push a CPU error code: #DF(8) #TS(10) #NP(11)
# #SS(12) #GP(13) #PF(14) #AC(17). The error code sits between the CPU's
# EIP/CS/EFLAGS push and where our handler starts, so it must be discarded
# before iret runs -- otherwise iret pops the error code as EIP, the real
# EIP as CS, and the real EFLAGS gets a bogus CS loaded into it, which
# faults again and cascades into a double fault then a triple fault.
.global isr_err_stub
.type isr_err_stub, @function
isr_err_stub:
    add $4, %esp
    jmp isr_default_common

# Page fault (#PF, vector 14). Installed by paging_init(), overriding
# isr_err_stub for just this one vector once real paging is up.
#
# CR2 (the faulting linear address) is read FIRST, before pushal or
# anything else that touches memory - CR2 only holds the most recent
# fault's address, so anything that itself faulted in between (extremely
# unlikely here, but the ordering costs nothing) would clobber it before
# we get to read it.
#
# No PIC EOI: #PF is a CPU exception, not a PIC-routed hardware IRQ - it
# doesn't go through the PIC at all, so there's nothing to acknowledge
# there.
#
# The popal/iret tail is real, not dead code, even though
# page_fault_handler() never returns yet in phase 1 (it halts): once a
# later phase makes it selectively kill just the faulting task instead,
# execution needs to resume correctly through here for whichever task the
# scheduler picks next - same resumable-tail pattern as pit_isr.
#
# Also passes the CPU-pushed EIP through as a third argument - the
# faulting instruction itself, not just the address it was trying to
# read/write. Sits one dword above the error code in the same exception
# frame CR2/the error code already come from, so no extra work to fetch
# it, just one more push. Lets page_fault_handler's diagnostics name the
# exact instruction that faulted instead of just where it faulted.
.global page_fault_isr
.type page_fault_isr, @function
page_fault_isr:
    movl %cr2, %ecx
    pushal
    movl 32(%esp), %eax    # CPU-pushed error code, sits right above our 8 pushed GPRs (8*4=32 bytes)
    movl 36(%esp), %edx    # CPU-pushed EIP, sits right above the error code - the actual faulting instruction
    push %edx               # faulting EIP (arg 3)
    push %ecx               # fault address (arg 2)
    push %eax               # error code   (arg 1)
    call page_fault_handler
    add $12, %esp
    popal
    add $4, %esp             # discard the CPU's error code before iret
    iretl

# gdt_flush(uint32_t gdt_ptr_addr): LGDTs from the gdt_ptr struct at the
# given address, then reloads every segment register so none of them are
# still running on cached descriptor state from the OLD (boot.asm) GDT.
# DS/ES/FS/GS/SS can be reloaded with a plain mov once the new selector
# value is known - CS cannot, a mov into CS is not a valid instruction,
# so CS is reloaded the only way x86 allows outside of an interrupt/call/
# ret: a far jump, which loads both the selector AND fetches a fresh
# descriptor for it in one step.
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    movl 4(%esp), %eax
    lgdt (%eax)
    movw $0x10, %ax     # GDT_KERNEL_DATA_SEL
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
    ljmp $0x08, $.Lgdt_flush_cs_reloaded   # GDT_KERNEL_CODE_SEL
.Lgdt_flush_cs_reloaded:
    ret

# tss_flush(): loads the task register with the TSS selector, so the CPU
# knows which GDT entry describes the current TSS (and therefore where to
# find esp0/ss0 on a ring3->ring0 transition).
.global tss_flush
.type tss_flush, @function
tss_flush:
    movw $0x28, %ax     # GDT_TSS_SEL, plain index/table bits, RPL=0
    ltr %ax
    ret

# Syscall gate (int 0x80, DPL=3 -- see syscall_init()). ABI: eax = syscall
# number, ebx/ecx/edx/esi/edi = up to 5 arguments, return value comes back
# in eax. A software interrupt via the `int` instruction never pushes a
# CPU error code (that only happens for the handful of CPU exceptions
# listed in idt_init()), so this follows isr_default's frame shape, not
# isr_err_stub's.
#
# Each argument register is PUSHED (not moved out of), and nothing but
# eax is ever written back afterward, so ebx/ecx/edx/esi/edi come out the
# other side holding exactly what the caller put in them - only eax
# changes, to the dispatch handler's return value. That happens to match
# the ABI note in syscall.h without any extra work: push doesn't clobber
# its source register.
.global syscall_isr
.type syscall_isr, @function
syscall_isr:
    push %edi
    push %esi
    push %edx
    push %ecx
    push %ebx
    push %eax
    call syscall_dispatch
    add $24, %esp
    iretl

# Keyboard IRQ1 handler
.global keyboard_isr
.type keyboard_isr, @function
keyboard_isr:
    pushal
    call keyboard_handler
    # Send EOI to master PIC. Without this the PIC never re-arms IRQ1,
    # so every keypress after the very first one is silently dropped.
    movb $0x20, %al
    outb %al, $0x20
    popal
    iretl

# Mouse IRQ12 handler -- IRQ12 lives on the SLAVE PIC (line 4 there), so
# both PICs need an EOI: slave first, then master, same as isr_default_common.
.global mouse_isr
.type mouse_isr, @function
mouse_isr:
    pushal
    call mouse_handler
    movb $0x20, %al
    outb %al, $0xA0
    outb %al, $0x20
    popal
    iretl

# RTL8139 NIC IRQ handler -- installed at a PCI-assigned vector, not a
# fixed one (see rtl8139_init), so unlike the two above this can't
# hardcode which PIC(s) need the EOI at assemble time. rtl8139_irq_
# handler (kernel/rtl8139.c) does that itself via pic_send_eoi(), which
# already knows the master-vs-slave rule (see kernel/pic.c) - so this
# stub does NOT also send one, which would double-EOI and desync the
# PIC's own notion of what's still pending.
.global rtl8139_isr
.type rtl8139_isr, @function
rtl8139_isr:
    pushal
    call rtl8139_irq_handler
    popal
    iretl

# Timer IRQ0 handler -- drives the preemptive scheduler.
#
# EOI is sent BEFORE calling pit_handler, not after, on purpose:
# pit_handler() may call into the scheduler and context-switch to a
# different task, in which case execution will not return to this exact
# point until this same task is picked again by task_tick - which could be
# an arbitrarily long time later. The PIC will not re-arm IRQ0 until it
# gets its EOI, so sending it late would stall the timer (and therefore
# the whole scheduler) until whatever task got switched to some other task
# happens to switch back here.
.global pit_isr
.type pit_isr, @function
pit_isr:
    pushal
    movb $0x20, %al
    outb %al, $0x20
    call pit_handler
    popal
    iretl

# task_switch_asm(uint32_t *old_esp_ptr, uint32_t new_esp)
# cdecl: [esp+4] = old_esp_ptr, [esp+8] = new_esp (before this function's
# own pushes below shift those offsets by 20 bytes).
#
# Saves the four callee-saved GPRs plus eflags onto the CURRENT stack,
# stashes the resulting esp through old_esp_ptr, switches esp to new_esp,
# then restores the same five values from the NEW stack and `ret`s into
# whatever return address sits on top of it. For a task that has run
# before, that's the instruction after its own earlier call to this same
# function. For a brand new task, task_create() fabricates a stack that
# makes this land in task_trampoline instead (see below).
.global task_switch_asm
.type task_switch_asm, @function
task_switch_asm:
    push %ebp
    push %ebx
    push %esi
    push %edi
    pushfl

    movl 24(%esp), %eax    # old_esp_ptr
    movl %esp, (%eax)

    movl 28(%esp), %eax    # new_esp
    movl %eax, %esp

    popfl
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret

# Landing pad for a task's very first run. task_create() arranges for
# task_switch_asm's `ret` to jump here with the task's entry function and
# its arg sitting where a called function's first two arguments would be
# (i.e. entry at [esp], arg at [esp+4]).
#
# Before calling entry(arg), the stack is explicitly realigned to a 16-byte
# boundary (`andl $-16, %esp`) rather than relying on the fabricated stack
# in task_create() having exactly the right byte count to land there on its
# own. Both would work today, but hand-counting bytes across two files (the
# C fabrication and this asm) is exactly the kind of thing that silently
# breaks the next time either one changes - GCC assumes a 16-byte aligned
# stack at every call site (for SSE, which boot.asm enables), so a
# misaligned entry() call here is a real, not theoretical, source of a
# hard-to-diagnose fault the first time entry() does anything the compiler
# vectorizes.
.global task_trampoline
.type task_trampoline, @function
task_trampoline:
    movl (%esp), %eax      # entry
    movl 4(%esp), %ecx     # arg
    andl $-16, %esp        # force 16-byte alignment, unconditionally
    subl $12, %esp         # reserve so that after pushing 1 arg (4 bytes),
                            # esp lands back on a 16-byte boundary at the call
    pushl %ecx              # arg
    sti
    call *%eax               # entry(arg)
    addl $16, %esp           # clean up: 4 (arg) + 12 (reserved above)
    call task_exit
.Ltask_trampoline_hang:
    # task_exit() never returns, but if it somehow did, halting beats
    # running off into whatever garbage follows in memory.
    hlt
    jmp .Ltask_trampoline_hang

# Landing pad for a ring3 task's very first run - task_create_ring3()
# arranges for task_switch_asm's `ret` to jump here the same way it does
# for task_trampoline above, with (entry, user_stack_top) sitting where
# task_trampoline finds (entry, arg): entry at [esp], user_stack_top at
# [esp+4].
#
# Unlike task_trampoline, this does NOT call entry directly - a plain
# `call` doesn't change privilege level, so the task's own code would
# just start executing at CPL0 instead of CPL3, defeating the entire
# point. The only way to actually drop to ring3 is an iret (or a far
# ret/call through a conforming gate, which GannetOS doesn't use), so this
# builds the 5-dword frame iret expects - SS, ESP, EFLAGS, CS, EIP, in
# that order from high address to low, i.e. pushed in reverse - using
# the user data/code selectors from gdt.h, then executes it.
#
# EFLAGS = 0x202 (IF=1, reserved bit 1 set) - same as a ring0 task gets
# in task_create(). iret is also what re-enables interrupts here (ring0
# tasks get an explicit `sti` in task_trampoline instead, since a plain
# call doesn't touch EFLAGS - iret restores whatever EFLAGS value is on
# the frame, so it doesn't need a separate sti next to it).
.global ring3_entry_trampoline
.type ring3_entry_trampoline, @function
ring3_entry_trampoline:
    movl (%esp), %eax      # entry
    movl 4(%esp), %ecx     # user_stack_top

    pushl $0x23            # SS  = GDT_USER_DATA_SEL (0x20 | RPL 3)
    pushl %ecx              # ESP = user_stack_top
    pushl $0x202            # EFLAGS
    pushl $0x1B             # CS  = GDT_USER_CODE_SEL (0x18 | RPL 3)
    pushl %eax               # EIP = entry
    iretl

# Landing pad for task_create_ring3_child() (task.c/SYS_SPAWN_TASK) -
# same idea as ring3_entry_trampoline above, but for task_entry_t's
# actual C signature, void entry(void *arg), instead of a bare no-
# argument entry point. A plain iret can't push a call argument for
# entry the way a normal `call` would - it just starts fetching at EIP
# with the CPU already at ring3 - so, same as app_entry_trampoline/
# user_app_crt0 do for app_main's THREE arguments below, this hands off
# to a small ring3-reachable stub (user_task_arg_crt0) that does the
# actual cdecl push-and-call once execution is already at CPL3.
# Reads three words off the fabricated stack - entry, arg,
# user_stack_top - task_create_ring3_child() builds it in that order.
.global ring3_entry_arg_trampoline
.type ring3_entry_arg_trampoline, @function
ring3_entry_arg_trampoline:
    movl 0(%esp), %eax      # entry
    movl 4(%esp), %ebx      # arg
    movl 8(%esp), %esi      # user_stack_top

    pushl $0x23              # SS
    pushl %esi                 # ESP = user_stack_top
    pushl $0x202                # EFLAGS
    pushl $0x1B                  # CS
    pushl $user_task_arg_crt0     # EIP
    iretl

# Runs at ring3 (ring3_entry_arg_trampoline above already dropped
# privilege before jumping here) - lives in .usertramp (see linker.ld),
# not the normal .text, for the same reason user_app_crt0 does: it has
# to actually be fetchable at CPL3. Turns eax=entry/ebx=arg into a real
# cdecl call to entry(arg) - task_entry_t's actual signature - then
# traps into SYS_EXIT_TASK the same way user_app_crt0 does once entry
# returns, in case it returns without calling exit_task itself (every
# real entry point today - apps/tests.c's isolation_worker - always
# does call it directly and never returns, but this stays consistent
# and defensive the same way user_app_crt0 is, rather than silently
# running off the end into whatever follows in memory). */
.section .usertramp
.global user_task_arg_crt0
.type user_task_arg_crt0, @function
user_task_arg_crt0:
    pushl %ebx                 # arg
    call *%eax                  # entry(arg)
    addl $4, %esp
    movl $6, %eax               # SYS_EXIT_TASK
    int $0x80
.Luser_task_arg_crt0_hang:
    hlt
    jmp .Luser_task_arg_crt0_hang
.text

# Landing pad for a REAL app (task_create_app_ring3) rather than the raw
# ring3_entry_trampoline above - the difference is this passes through
# user_app_crt0 (below) instead of iret-ing straight to the app's own
# code, because a real app_main(argc, argv, api) needs its arguments set
# up as an ordinary cdecl call would (pushed on the stack) - something
# no amount of register juggling in THIS function can do by itself,
# since iret doesn't call anything, it just starts fetching at EIP with
# nothing pushed. So this hands off five words instead of two: the real
# app_main address, argc, argv, api, and user_stack_top - all read off
# the fabricated stack the same way ring3_entry_trampoline reads its
# two - into registers, then iret's into user_app_crt0 (at ring3) to do
# the actual pushing and calling.
.global app_entry_trampoline
.type app_entry_trampoline, @function
app_entry_trampoline:
    movl 0(%esp), %eax      # app_main address
    movl 4(%esp), %ebx      # argc
    movl 8(%esp), %ecx      # argv
    movl 12(%esp), %edx     # api
    movl 16(%esp), %esi     # user_stack_top

    pushl $0x23              # SS
    pushl %esi                # ESP = user_stack_top
    pushl $0x202               # EFLAGS
    pushl $0x1B                 # CS
    pushl $user_app_crt0         # EIP
    iretl

# Runs at ring3 (app_entry_trampoline above already dropped privilege
# before jumping here) - lives in .usertramp (see linker.ld), not the
# normal .text, so it's actually fetchable at CPL3. Turns the four
# registers app_entry_trampoline set up (eax=app_main, ebx=argc,
# ecx=argv, edx=api) into a real cdecl call, then traps into
# SYS_EXIT_TASK with whatever app_main returned - this is what makes
# "return rc;" from an app's app_main actually end the app, the same as
# it does today when try_exec calls entry() directly at CPL0.
.section .usertramp
.global user_app_crt0
.type user_app_crt0, @function
user_app_crt0:
    pushl %edx                 # api
    pushl %ecx                 # argv
    pushl %ebx                 # argc
    call *%eax
    addl $12, %esp
    # eax now holds app_main's return value. SYS_EXIT_TASK doesn't take
    # an exit-code argument yet (see syscall.c's TODO on that) - the
    # value is dropped here, not silently wrong, but a real exit-code
    # path is follow-up work same as the rest of the unported api
    # surface in kernel/user_trampolines.c.
    movl $6, %eax               # SYS_EXIT_TASK
    int $0x80
.Luser_app_crt0_hang:
    # SYS_EXIT_TASK never returns, but if it somehow did, halting beats
    # running off into whatever garbage follows in this page.
    hlt
    jmp .Luser_app_crt0_hang
.text
