/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable check of kernel/syscall.c's dispatch table logic: given
   a syscall number and 5 args, does it call the right handler with the
   right args, and does it reject an out-of-range number instead of
   reading past the table? This reimplements the table-lookup shape of
   syscall_dispatch() with stub handlers instead of #including syscall.c
   (which pulls in freestanding kernel headers) - same reasoning as
   test_gdt_encoding.c. */
#include <stdint.h>
#include <stdio.h>

enum
{
    SYS_PUTS = 0,
    SYS_PUTCHAR,
    SYS_PUTS_COL,
    SYS_GETCHAR,
    SYS_SPAWN_TASK,
    SYS_TASK_YIELD,
    SYS_EXIT_TASK,
    SYS_COUNT
};

typedef uint32_t (*syscall_fn_t)(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);

/* Instrumented stubs: record what they were called with instead of
   touching real terminal/task state. */
static uint32_t last_a1, last_a2;
static int call_count[SYS_COUNT];

static uint32_t stub_puts(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    last_a1 = a1;
    call_count[SYS_PUTS]++;
    return 0;
}
static uint32_t stub_putchar(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a2; (void)a3; (void)a4; (void)a5;
    last_a1 = a1;
    call_count[SYS_PUTCHAR]++;
    return 0;
}
static uint32_t stub_puts_col(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    last_a1 = a1;
    last_a2 = a2;
    call_count[SYS_PUTS_COL]++;
    return 0;
}
static uint32_t stub_getchar(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    call_count[SYS_GETCHAR]++;
    return 42; /* arbitrary sentinel so we can check the return value flows back */
}
static uint32_t stub_spawn_task(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a3; (void)a4; (void)a5;
    last_a1 = a1;
    last_a2 = a2;
    call_count[SYS_SPAWN_TASK]++;
    return 7; /* arbitrary fake task id */
}
static uint32_t stub_task_yield(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    call_count[SYS_TASK_YIELD]++;
    return 0;
}
static uint32_t stub_exit_task(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    call_count[SYS_EXIT_TASK]++;
    return 0;
}

static const syscall_fn_t syscall_table[SYS_COUNT] = {
    [SYS_PUTS]       = stub_puts,
    [SYS_PUTCHAR]    = stub_putchar,
    [SYS_PUTS_COL]   = stub_puts_col,
    [SYS_GETCHAR]    = stub_getchar,
    [SYS_SPAWN_TASK] = stub_spawn_task,
    [SYS_TASK_YIELD] = stub_task_yield,
    [SYS_EXIT_TASK]  = stub_exit_task,
};

static uint32_t syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    if (num >= SYS_COUNT || !syscall_table[num])
    {
        return (uint32_t)-1;
    }
    return syscall_table[num](a1, a2, a3, a4, a5);
}

static int failures = 0;
static void check(const char *name, int cond)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond)
    {
        failures++;
    }
}

int main(void)
{
    uint32_t rc;

    rc = syscall_dispatch(SYS_PUTS, 0xDEADBEEF, 0, 0, 0, 0);
    check("SYS_PUTS reaches stub_puts with the right pointer arg", last_a1 == 0xDEADBEEF && call_count[SYS_PUTS] == 1);
    check("SYS_PUTS returns 0", rc == 0);

    rc = syscall_dispatch(SYS_PUTS_COL, 0x1000, 0xFFEE00, 0, 0, 0);
    check("SYS_PUTS_COL passes both string pointer and colour through", last_a1 == 0x1000 && last_a2 == 0xFFEE00);

    rc = syscall_dispatch(SYS_GETCHAR, 0, 0, 0, 0, 0);
    check("SYS_GETCHAR's return value flows back through dispatch", rc == 42);

    rc = syscall_dispatch(SYS_SPAWN_TASK, 0x2000, 0x3000, 0, 0, 0);
    check("SYS_SPAWN_TASK gets entry and arg in the right slots", last_a1 == 0x2000 && last_a2 == 0x3000);
    check("SYS_SPAWN_TASK's task id return value flows back", rc == 7);

    /* Every syscall number must route to exactly one handler, not
       accidentally alias another (a copy-paste bug in the table would
       make two enum entries point at the same stub silently). */
    check("SYS_PUTS was called exactly once total", call_count[SYS_PUTS] == 1);
    check("SYS_TASK_YIELD untouched by unrelated calls", call_count[SYS_TASK_YIELD] == 0);
    check("SYS_EXIT_TASK untouched by unrelated calls", call_count[SYS_EXIT_TASK] == 0);

    /* Out-of-range and unmapped syscall numbers must be rejected, not
       read past the table or dereference a null entry - this is the
       check that stands in for bounds validation once a ring3 app can
       put an arbitrary attacker-chosen value in eax. */
    rc = syscall_dispatch(SYS_COUNT, 0, 0, 0, 0, 0);
    check("exactly-out-of-range syscall number returns -1", rc == (uint32_t)-1);

    rc = syscall_dispatch(0xFFFFFFFF, 0, 0, 0, 0, 0);
    check("wildly out-of-range syscall number returns -1", rc == (uint32_t)-1);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}