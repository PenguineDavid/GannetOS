/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"
#include <stdint.h>

static app_api_t *g_api;
static int pass_count = 0;
static int total_count = 0;

/* ------------------------------------------------------------------ */
/* Tiny string helpers -- no libc in a freestanding app                */
/* ------------------------------------------------------------------ */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a != *b)
        {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n])
    {
        n++;
    }
    return n;
}

static void print_uint(unsigned int v)
{
    char digits[12];
    int n = 0;
    if (v == 0)
    {
        digits[n++] = '0';
    }
    while (v > 0)
    {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0)
    {
        g_api->putchar(digits[--n]);
    }
}

/* ------------------------------------------------------------------ */
/* Reporting -- fixed-width [SUCCESS]/[FAIL] labels so names line up   */
/* regardless of which one printed ("[SUCCESS]" and "[FAIL]   " are    */
/* both exactly 9 characters).                                         */
/* ------------------------------------------------------------------ */
static void report(const char *name, int ok)
{
    total_count++;
    if (ok)
    {
        g_api->puts_col("[SUCCESS]", g_api->col_green);
        pass_count++;
    }
    else
    {
        g_api->puts_col("[FAIL]   ", g_api->col_red);
    }
    g_api->putchar(' ');
    g_api->puts(name);
    g_api->putchar('\n');
}

static void section(const char *title)
{
    g_api->putchar('\n');
    g_api->puts_col("== ", g_api->col_yellow);
    g_api->puts_col(title, g_api->col_yellow);
    g_api->puts_col(" ==\n", g_api->col_yellow);
}

/* ------------------------------------------------------------------ */
/* Section 1: command-level checks -- the same file/dir lifecycle that */
/* write, cat, mkdir, rmdir, and rm actually perform under the hood.   */
/* Uses a dedicated /systest scratch directory, cleaned up as it goes  */
/* so a run never leaves clutter behind (best-effort: later checks     */
/* still run even if an earlier cleanup step unexpectedly fails).      */
/* ------------------------------------------------------------------ */
static void run_command_level_checks(void)
{
    section("Command-level checks");

    report("mkdir /systest", g_api->fs_mkdir("/systest") == 0);
    report("mkdir /systest (duplicate correctly refused)",
           g_api->fs_mkdir("/systest") < 0);

    static const char msg[] = "hello from tests";
    int fd = g_api->fs_open("/systest/a.txt", 1);
    report("write: create /systest/a.txt", fd >= 0);
    if (fd >= 0)
    {
        int n = g_api->fs_write(fd, msg, (uint32_t)str_len(msg));
        report("write: fs_write full message", n == (int)str_len(msg));
        g_api->fs_close(fd);
    }
    else
    {
        report("write: fs_write full message", 0);
    }

    fd = g_api->fs_open("/systest/a.txt", 0);
    report("cat: reopen /systest/a.txt for reading", fd >= 0);
    if (fd >= 0)
    {
        char buf[64];
        int n = g_api->fs_read(fd, buf, sizeof(buf) - 1);
        buf[n < 0 ? 0 : n] = '\0';
        report("cat: readback matches what was written",
               n == (int)str_len(msg) && str_eq(buf, msg));
        g_api->fs_close(fd);
    }
    else
    {
        report("cat: readback matches what was written", 0);
    }

    report("isdir /systest is true", g_api->fs_isdir("/systest"));
    report("isdir /systest/a.txt is false (it's a file)",
           !g_api->fs_isdir("/systest/a.txt"));

    report("rm /systest/a.txt", g_api->fs_delete("/systest/a.txt") == 0);
    report("cat: reopening deleted file correctly fails",
           g_api->fs_open("/systest/a.txt", 0) < 0);

    report("rmdir /systest", g_api->fs_rmdir("/systest") == 0);
    report("rmdir /systest (already gone, correctly refused)",
           g_api->fs_rmdir("/systest") < 0);
}

/* ------------------------------------------------------------------ */
/* Section 2: API-level checks -- lower-level behavior and edge cases  */
/* not necessarily exercised by any single command.                    */
/* ------------------------------------------------------------------ */
static void run_api_level_checks(void)
{
    section("API-level checks");

    report("fs_isdir(\"/\") is true", g_api->fs_isdir("/"));
    report("fs_open on a nonexistent path without create fails",
           g_api->fs_open("/systest_no_such_file.tmp", 0) < 0);
    report("fs_delete on a nonexistent path fails",
           g_api->fs_delete("/systest_no_such_file.tmp") < 0);
    report("fs_mkdir refuses a missing parent directory",
           g_api->fs_mkdir("/systest_missing_parent/child") < 0);
    report("fs_inode_count reports a positive inode table size",
           g_api->fs_inode_count() > 0);

    char parent[64];
    g_api->fs_get_parent("/foo/bar", parent);
    report("fs_get_parent(\"/foo/bar\") == \"/foo\"", str_eq(parent, "/foo"));
    g_api->fs_get_parent("/foo", parent);
    report("fs_get_parent(\"/foo\") == \"/\"", str_eq(parent, "/"));

    /* fs_stat should never crash on an in-range index, whatever it
       currently holds. */
    char name[64];
    uint32_t size;
    uint8_t type;
    int stat_rc = g_api->fs_stat(0, name, &size, &type);
    report("fs_stat(0, ...) returns without crashing", stat_rc == 0 || stat_rc < 0);
}

/* ------------------------------------------------------------------ */
/* Section 3: isolation checks -- Stage 3 of the virtual memory work.  */
/*                                                                      */
/* Every task (this one included) has its own 4KB page mapped at the   */
/* SAME virtual address, APP_PRIVATE_VADDR, but backed by a DIFFERENT  */
/* physical frame per task. The worker below runs as a second,         */
/* independently scheduled task and writes its own pattern there; if   */
/* isolation is actually working, that write is invisible to us - we   */
/* should still see exactly what WE wrote, at the exact same address.  */
/*                                                                      */
/* worker_wrote is an ordinary global, deliberately NOT isolated (only */
/* APP_PRIVATE_VADDR is) - every task sees the same loaded code and     */
/* data for this app, which is what lets a plain flag work as the      */
/* synchronization between the two tasks here.                          */
/* ------------------------------------------------------------------ */
static volatile int worker_wrote = 0;

static void isolation_worker(void *arg)
{
    (void)arg;
    volatile uint32_t *priv = (volatile uint32_t *)APP_PRIVATE_VADDR;
    *priv = 0xCAFEBABEu;
    worker_wrote = 1;
    g_api->exit_task(); /* never returns */
}

static void run_isolation_checks(void)
{
    section("Isolation checks (Stage 3: per-task private memory)");

    volatile uint32_t *priv = (volatile uint32_t *)APP_PRIVATE_VADDR;

    *priv = 0x11111111u;
    report("private page holds what we just wrote", *priv == 0x11111111u);

    worker_wrote = 0;
    int task_id = g_api->spawn_task(isolation_worker, 0);
    report("spawn_task creates a new task", task_id >= 0);

    if (task_id >= 0)
    {
        int spins = 0;
        while (!worker_wrote && spins < 1000000)
        {
            g_api->task_yield();
            spins++;
        }
        report("worker task actually ran and wrote its own pattern", worker_wrote);
        report("our own private page is UNCHANGED by the worker's write "
               "(same virtual address, different physical memory)",
               *priv == 0x11111111u);
    }
    else
    {
        report("worker task actually ran and wrote its own pattern", 0);
        report("our own private page is unchanged by the worker's write", 0);
    }
}

int app_main(int argc, char **argv, app_api_t *api)
{
    (void)argc;
    (void)argv;
    g_api = api;
    pass_count = 0;
    total_count = 0;

    api->puts_col("GannetOS self-test\n", api->col_yellow);

    run_command_level_checks();
    run_api_level_checks();
    run_isolation_checks();

    g_api->putchar('\n');
    g_api->puts_col("Result: ", api->col_yellow);
    print_uint((unsigned int)pass_count);
    g_api->putchar('/');
    print_uint((unsigned int)total_count);
    g_api->puts_col(pass_count == total_count ? " tests passed\n" : " tests passed (see FAIL lines above)\n",
                     pass_count == total_count ? api->col_green : api->col_red);

    return pass_count == total_count ? 0 : 1;
}