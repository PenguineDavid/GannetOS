/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/syscall.h"
#include "kernel/arch/x86/idt.h"
#include "kernel/arch/x86/gdt.h"
#include "kernel/ui/terminal.h"
#include "kernel/drivers/keyboard/keyboard.h"
#include "kernel/proc/task.h"
#include "kernel/proc/pipe.h"
#include "filesys/fs.h"
#include "kernel/drivers/rtc/rtc.h"
#include "kernel/drivers/net/application_layer/dns.h"
#include "kernel/drivers/net/internet_layer/icmp.h"
#include "kernel/drivers/net/application_layer/http.h"

/* Implemented in shell/isr_wrappers.s. */
extern void syscall_isr(void);

typedef uint32_t (*syscall_fn_t)(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

/* ------------------------------------------------------------------ */
/* Ring3 user-memory validation                                        */
/* ------------------------------------------------------------------ */
/* A ring3 app's ENTIRE legitimate memory today is these two fixed
   regions from linker.ld: .appbuf (its own loaded code + data + bss,
   see loader.c's exec_buf) and .appstack (its stack, plus the argv
   copy loader.c places at the bottom of it). Both are still single,
   shared regions rather than per-task - see loader.c's own comments on
   why - but that doesn't change what counts as valid here: whichever
   one app is currently running, this is the whole of what it owns.
   Anything else a syscall's pointer arguments could name is kernel
   memory, another task's memory, or garbage - none of which a ring3
   caller has any legitimate reason to hand the kernel a pointer into,
   whether by bug or on purpose. */
extern uint8_t __appbuf_start[], __appbuf_end[];
extern uint8_t __appstack_start[], __appstack_end[];

static int addr_range_in(uint32_t ptr, uint32_t len, uint32_t start, uint32_t end)
{
    uint32_t span_end = ptr + len;
    if (span_end < ptr)
    {
        return 0; /* ptr+len wrapped past 0xFFFFFFFF */
    }
    return ptr >= start && span_end <= end;
}

/* True if the whole span [ptr, ptr+len) lies inside the app's own
   memory. len == 0 still requires ptr itself to be in-range, so a
   zero-length call can't be used to smuggle an arbitrary base pointer
   through some later computation. Every handler below that takes a
   fixed-size out-pointer or a buffer+length pair calls this before
   touching it. */
static int user_range_ok(uint32_t ptr, uint32_t len)
{
    return addr_range_in(ptr, len, (uint32_t)__appbuf_start, (uint32_t)__appbuf_end) ||
           addr_range_in(ptr, len, (uint32_t)__appstack_start, (uint32_t)__appstack_end);
}

/* Same idea for a NUL-terminated string argument: ptr must be
   in-range, and a NUL byte must turn up before the containing region
   ends. The scan itself therefore never runs past the app's own
   memory looking for a terminator that isn't there - an unterminated
   "string" just fails validation instead of the search wandering into
   kernel memory. */
static int user_str_ok(uint32_t ptr)
{
    uint32_t appbuf_start = (uint32_t)__appbuf_start, appbuf_end = (uint32_t)__appbuf_end;
    uint32_t appstack_start = (uint32_t)__appstack_start, appstack_end = (uint32_t)__appstack_end;
    uint32_t region_end;
    if (ptr >= appbuf_start && ptr < appbuf_end)
    {
        region_end = appbuf_end;
    }
    else if (ptr >= appstack_start && ptr < appstack_end)
    {
        region_end = appstack_end;
    }
    else
    {
        return 0;
    }
    for (uint32_t p = ptr; p < region_end; p++)
    {
        if (*(const volatile uint8_t *)p == 0)
        {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */
/* Every handler below that takes a pointer argument validates it with
   user_range_ok()/user_str_ok() before dereferencing - see those
   functions' own comments for what "valid" means. A validation failure
   returns (uint32_t)-1, the same sentinel syscall_dispatch already
   uses for an unknown syscall number, without touching the pointer at
   all. This used to be a documented gap (every caller was ring0 back
   when this note was written) - it isn't anymore: try_exec() has run
   apps as real ring3 tasks via task_create_app_ring3() for a while
   now, so an unvalidated pointer argument here has been a live
   kernel-memory read/write primitive reachable from any ring3 app,
   not just a theoretical one. */

static uint32_t sys_puts(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    const char *s = (const char *)a1;
    /* Matches the pipe-redirection behaviour loader.c's api_puts had
       before ring3 apps went through this syscall instead of calling
       that function directly - a piped app (e.g. `ls | grep x`) needs
       its output to keep landing in the pipe, not the screen. */
    if (pipe_active())
    {
        while (*s)
        {
            pipe_putchar(*s++);
        }
    }
    else
    {
        terminal_puts(s, TERMINAL_WHITE);
    }
    return 0;
}

static uint32_t sys_putchar(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (pipe_active())
    {
        pipe_putchar((char)a1);
    }
    else
    {
        terminal_putchar((char)a1, TERMINAL_WHITE);
    }
    return 0;
}

static uint32_t sys_puts_col(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    const char *s = (const char *)a1;
    if (pipe_active())
    {
        while (*s)
        {
            pipe_putchar(*s++);
        }
    }
    else
    {
        terminal_puts(s, (uint32_t)a2);
    }
    return 0;
}

static uint32_t sys_getchar(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint32_t)kb_raw_getchar();
}

static uint32_t sys_task_yield(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    task_yield();
    return 0;
}

static uint32_t sys_exit_task(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    task_exit(); /* never returns */
    return 0;
}

static uint32_t sys_spawn_task(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    /* entry (a1) must at least point into the app's own loaded code -
       rejects the obviously-wrong case (a kernel address, garbage, 0)
       outright. task_create_ring3_child() (task.c) runs it as a real
       ring3 sibling task sharing the caller's own code/data, instead
       of the plain task_create() this used to call - which ran the new
       task at ring0, full kernel privilege, regardless of the CALLER's
       privilege level: a ring3 app could turn any address in its own
       code into a kernel-privileged task on demand. a2 (arg) is
       delivered to entry as a real cdecl argument via
       ring3_entry_arg_trampoline/user_task_arg_crt0 (isr_wrappers.s) -
       opaque to the kernel, task_create_ring3_child() only ever passes
       it through, never dereferences it - so it needs no validation of
       its own here, same as it needed none when task_create() handled
       it before. See task_create_ring3_child's own doc comment in
       task.h for the ordering guarantee task_reap() now enforces
       around a spawned child outliving its parent. */
    if (!addr_range_in(a1, 1, (uint32_t)__appbuf_start, (uint32_t)__appbuf_end))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)task_create_ring3_child(a1, (void *)a2, "syscall_task");
}

/* ------------------------------------------------------------------ */
/* Filesystem handlers.                                                */
/* ------------------------------------------------------------------ */

static uint32_t sys_fs_open(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_open((const char *)a1, (int)a2);
}

static uint32_t sys_fs_read(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a4; (void)a5;
    if (!user_range_ok(a2, a3))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_read((int)a1, (void *)a2, a3);
}

static uint32_t sys_fs_write(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a4; (void)a5;
    if (!user_range_ok(a2, a3))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_write((int)a1, (const void *)a2, a3);
}

static uint32_t sys_fs_close(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    fs_close((int)a1);
    return 0;
}

static uint32_t sys_fs_stat(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a5;
    /* name_out is always a FS_NAME_MAX-byte buffer by convention (every
       real caller declares char name[FS_NAME_MAX] or bigger - see
       fs.h), size_out a uint32_t*, type_out a uint8_t* - fs_stat()
       itself takes those sizes on faith from its caller, so this is
       the one place they get pinned down and checked. */
    if (!user_range_ok(a2, FS_NAME_MAX) || !user_range_ok(a3, sizeof(uint32_t)) ||
        !user_range_ok(a4, sizeof(uint8_t)))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_stat((int)a1, (char *)a2, (uint32_t *)a3, (uint8_t *)a4);
}

static uint32_t sys_fs_inode_count(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint32_t)fs_inode_count();
}

static uint32_t sys_fs_get_parent(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    /* parent_out is a FS_NAME_MAX-byte buffer by the same convention as
       fs_stat's name_out above - every real caller (fs.c's own
       parent_exists, apps' path handling) declares it that size. */
    if (!user_str_ok(a1) || !user_range_ok(a2, FS_NAME_MAX))
    {
        return (uint32_t)-1;
    }
    fs_get_parent((const char *)a1, (char *)a2);
    return 0;
}

static uint32_t sys_fs_delete(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_delete((const char *)a1);
}

static uint32_t sys_fs_mkdir(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_mkdir((const char *)a1);
}

static uint32_t sys_fs_rmdir(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_rmdir((const char *)a1);
}

static uint32_t sys_fs_rrmdr(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_rrmdr((const char *)a1);
}

static uint32_t sys_fs_isdir(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)fs_isdir((const char *)a1);
}

static uint32_t sys_read_line(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if ((int)a2 <= 0 || !user_range_ok(a1, a2))
    {
        return (uint32_t)-1;
    }
    char *buf = (char *)a1;
    int maxlen = (int)a2;
    int len = 0;
    for (;;)
    {
        int c = kb_raw_getchar();
        if (c == '\n' || c == '\r')
        {
            if (pipe_active())
            {
                pipe_putchar('\n');
            }
            else
            {
                terminal_putchar('\n', TERMINAL_WHITE);
            }
            break;
        }
        else if (c == '\b')
        {
            if (len > 0)
            {
                len--;
                if (pipe_active())
                {
                    pipe_putchar('\b');
                }
                else
                {
                    terminal_putchar('\b', TERMINAL_WHITE);
                }
            }
        }
        else if (len < maxlen - 1)
        {
            buf[len++] = (char)c;
            if (pipe_active())
            {
                pipe_putchar((char)c);
            }
            else
            {
                terminal_putchar((char)c, TERMINAL_WHITE);
            }
        }
    }
    buf[len] = '\0';
    return (uint32_t)len;
}

static uint32_t sys_get_time(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    /* rtc_format_time() always writes exactly "HH:MM:SS\0" - 9 bytes. */
    if (!user_range_ok(a1, 9))
    {
        return (uint32_t)-1;
    }
    rtc_format_time((char *)a1);
    return 0;
}

static uint32_t sys_has_stdin(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return (uint32_t)pipe_has_stdin();
}

static uint32_t sys_read_stdin(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if ((int)a2 <= 0 || !user_range_ok(a1, a2))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)pipe_read_stdin((char *)a1, (int)a2);
}

/* ------------------------------------------------------------------ */
/* Networking handlers.                                                */
/* ------------------------------------------------------------------ */

static uint32_t sys_dns_resolve(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if (!user_str_ok(a1) || !user_range_ok(a2, sizeof(uint32_t)))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)dns_resolve((const char *)a1, (uint32_t *)a2);
}

static uint32_t sys_icmp_ping(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a4; (void)a5;
    if (!user_range_ok(a3, sizeof(uint32_t)))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)icmp_ping(a1, (uint16_t)a2, (uint32_t *)a3);
}

static uint32_t sys_http_get(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    if (!user_str_ok(a1) || !user_str_ok(a3))
    {
        return (uint32_t)-1;
    }
    /* http_get()'s own contract: body_buf may legitimately be NULL/0 to
       discard the body and just get the status code back - only
       validate the buffer range when the caller actually supplied one. */
    if (a4 != 0 && !user_range_ok(a4, a5))
    {
        return (uint32_t)-1;
    }
    return (uint32_t)http_get((const char *)a1, (uint16_t)a2, (const char *)a3,
                               (char *)a4, a5);
}

static const syscall_fn_t syscall_table[SYS_COUNT] = {
    [SYS_PUTS]           = sys_puts,
    [SYS_PUTCHAR]        = sys_putchar,
    [SYS_PUTS_COL]       = sys_puts_col,
    [SYS_GETCHAR]        = sys_getchar,
    [SYS_SPAWN_TASK]     = sys_spawn_task,
    [SYS_TASK_YIELD]     = sys_task_yield,
    [SYS_EXIT_TASK]      = sys_exit_task,
    [SYS_FS_OPEN]        = sys_fs_open,
    [SYS_FS_READ]        = sys_fs_read,
    [SYS_FS_WRITE]       = sys_fs_write,
    [SYS_FS_CLOSE]       = sys_fs_close,
    [SYS_FS_STAT]        = sys_fs_stat,
    [SYS_FS_INODE_COUNT] = sys_fs_inode_count,
    [SYS_FS_GET_PARENT]  = sys_fs_get_parent,
    [SYS_FS_DELETE]      = sys_fs_delete,
    [SYS_FS_MKDIR]       = sys_fs_mkdir,
    [SYS_FS_RMDIR]       = sys_fs_rmdir,
    [SYS_FS_RRMDR]       = sys_fs_rrmdr,
    [SYS_FS_ISDIR]       = sys_fs_isdir,
    [SYS_READ_LINE]      = sys_read_line,
    [SYS_GET_TIME]       = sys_get_time,
    [SYS_HAS_STDIN]      = sys_has_stdin,
    [SYS_READ_STDIN]     = sys_read_stdin,
    [SYS_DNS_RESOLVE]    = sys_dns_resolve,
    [SYS_ICMP_PING]      = sys_icmp_ping,
    [SYS_HTTP_GET]       = sys_http_get,
};

uint32_t syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    if (num >= SYS_COUNT || !syscall_table[num])
    {
        return (uint32_t)-1;
    }
    return syscall_table[num](a1, a2, a3, a4, a5);
}

void syscall_init(void)
{
    /* 0xEF = present, DPL=3, 32-bit TRAP gate (P=1 DPL=11 S=0 type=1111).
       This USED to be 0xEE (an interrupt gate, type=1110) - the only
       difference is that an interrupt gate clears EFLAGS.IF on entry and
       only restores it via the eventual iret, while a trap gate leaves IF
       untouched. That distinction didn't matter back when every syscall
       just did its work and returned immediately. It matters now:
       icmp_ping()/dns_resolve()/tcp_connect() all call task_yield()
       *from inside* this syscall, to block waiting on the network stack.
       With an interrupt gate, the IF=0 state at the moment of that yield
       gets saved as part of THIS task's suspended context - so every
       time this task is scheduled back (still mid-syscall, waiting on a
       reply), it runs with interrupts disabled, which starves the very
       PIT tick every timeout in that blocking code depends on, and
       starves normal keyboard/terminal servicing right along with it.
       A trap gate leaves IF exactly as the caller had it (normally 1),
       so a syscall is free to yield internally without silently turning
       off the clock for itself. DPL=3 is unchanged and still the whole
       point - it's the difference between this gate and every other one
       idt_init() installs (all 0x8E, DPL=0), and it's what lets a CPL3
       `int $0x80` through without a general-protection fault. */
    idt_set_gate(0x80, (uint32_t)syscall_isr, GDT_KERNEL_CODE_SEL, 0xEF);
}