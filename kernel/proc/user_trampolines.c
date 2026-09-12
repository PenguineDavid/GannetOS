/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Ring3-callable syscall trampolines. Everything in this file is placed
   in the .usertramp section (see linker.ld) and marked PAGE_USER at
   boot (kernel.c calls paging_mark_kernel_region_user over it) - it is
   the ONLY kernel-authored code a ring3 app is allowed to reach with a
   plain `call` instead of `int $0x80`, and it exists purely to turn
   that plain call into the real trap. Nothing here runs any differently
   whether it happens to execute at CPL0 or CPL3 - `int $0x80` works
   from either - so there is no ring3-specific logic to get wrong here,
   just argument marshalling from cdecl into the syscall register ABI
   (see kernel/syscall.h).

   This is the ring3 app's ENTIRE view of the kernel: it never sees a
   real kernel function pointer, only these stubs - see loader.c's
   g_ring3_api, which is this file's other job. */

#include "kernel/proc/pexe.h"
#include "kernel/proc/syscall.h"
#include "kernel/ui/terminal.h"
#include <stdint.h>

#define USERTRAMP __attribute__((section(".usertramp")))

USERTRAMP static void t_puts(const char *s)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_PUTS), "b"((uint32_t)s) : "memory");
}

USERTRAMP static void t_putchar(char c)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_PUTCHAR), "b"((uint32_t)(uint8_t)c) : "memory");
}

USERTRAMP static void t_puts_col(const char *s, uint32_t attr)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_PUTS_COL), "b"((uint32_t)s), "c"(attr) : "memory");
}

USERTRAMP static int t_getchar(void)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_GETCHAR) : "memory");
    return (int)rc;
}

USERTRAMP static void t_task_yield(void)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_TASK_YIELD) : "memory");
}

USERTRAMP static void t_exit_task(void)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_EXIT_TASK) : "memory");
    for (;;)
    {
    } /* SYS_EXIT_TASK never returns; this is unreachable */
}

USERTRAMP static int t_spawn_task(void (*entry)(void *arg), void *arg)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_SPAWN_TASK), "b"((uint32_t)entry), "c"((uint32_t)arg) : "memory");
    return (int)rc;
}

/* ------------------------------------------------------------------ */
/* Filesystem/misc trampolines. Real syscalls now (kernel/syscall.c),   */
/* not stubs - each one just marshals cdecl args into the register ABI  */
/* and traps, same shape as the six above. */
/* ------------------------------------------------------------------ */

USERTRAMP static int t_fs_open(const char *path, int create)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_OPEN), "b"((uint32_t)path), "c"((uint32_t)create) : "memory");
    return (int)rc;
}

USERTRAMP static int t_fs_read(int fd, void *buf, uint32_t len)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_READ), "b"((uint32_t)fd), "c"((uint32_t)buf), "d"(len) : "memory");
    return (int)rc;
}

USERTRAMP static int t_fs_write(int fd, const void *buf, uint32_t len)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_WRITE), "b"((uint32_t)fd), "c"((uint32_t)buf), "d"(len) : "memory");
    return (int)rc;
}

USERTRAMP static void t_fs_close(int fd)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_FS_CLOSE), "b"((uint32_t)fd) : "memory");
}

USERTRAMP static int t_fs_stat(int idx, char *name, uint32_t *size, uint8_t *type)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_STAT), "b"((uint32_t)idx), "c"((uint32_t)name), "d"((uint32_t)size), "S"((uint32_t)type) : "memory");
    return (int)rc;
}

USERTRAMP static int t_fs_inode_count(void)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_INODE_COUNT) : "memory");
    return (int)rc;
}

USERTRAMP static void t_fs_get_parent(const char *path, char *parent)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_FS_GET_PARENT), "b"((uint32_t)path), "c"((uint32_t)parent) : "memory");
}

USERTRAMP static int t_fs_delete(const char *path)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_DELETE), "b"((uint32_t)path) : "memory");
    return (int)rc;
}

USERTRAMP static int t_fs_mkdir(const char *path)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_MKDIR), "b"((uint32_t)path) : "memory");
    return (int)rc;
}

USERTRAMP static int t_fs_rmdir(const char *path)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_RMDIR), "b"((uint32_t)path) : "memory");
    return (int)rc;
}

USERTRAMP static int t_fs_rrmdr(const char *path)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_RRMDR), "b"((uint32_t)path) : "memory");
    return (int)rc;
}

USERTRAMP static int t_fs_isdir(const char *path)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_FS_ISDIR), "b"((uint32_t)path) : "memory");
    return (int)rc;
}

USERTRAMP static int t_read_line(char *buf, int maxlen)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_READ_LINE), "b"((uint32_t)buf), "c"((uint32_t)maxlen) : "memory");
    return (int)rc;
}

USERTRAMP static void t_get_time(char *buf)
{
    asm volatile("int $0x80" : : "a"((uint32_t)SYS_GET_TIME), "b"((uint32_t)buf) : "memory");
}

USERTRAMP static int t_has_stdin(void)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_HAS_STDIN) : "memory");
    return (int)rc;
}

USERTRAMP static int t_read_stdin(char *buf, int maxlen)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_READ_STDIN), "b"((uint32_t)buf), "c"((uint32_t)maxlen) : "memory");
    return (int)rc;
}

/* ------------------------------------------------------------------ */
/* Networking trampolines - same shape as everything above.            */
/* ------------------------------------------------------------------ */

USERTRAMP static int t_dns_resolve(const char *hostname, uint32_t *ip_out)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_DNS_RESOLVE), "b"((uint32_t)hostname), "c"((uint32_t)ip_out) : "memory");
    return (int)rc;
}

USERTRAMP static int t_icmp_ping(uint32_t dest_ip, uint16_t seq, uint32_t *rtt_ticks_out)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_ICMP_PING), "b"(dest_ip), "c"((uint32_t)seq), "d"((uint32_t)rtt_ticks_out) : "memory");
    return (int)rc;
}

USERTRAMP static int t_http_get(const char *host, uint16_t port, const char *path,
                                 char *body_buf, uint32_t body_buf_len)
{
    uint32_t rc;
    asm volatile("int $0x80" : "=a"(rc) : "a"((uint32_t)SYS_HTTP_GET), "b"((uint32_t)host),
                 "c"((uint32_t)port), "d"((uint32_t)path), "S"((uint32_t)body_buf),
                 "D"(body_buf_len) : "memory");
    return (int)rc;
}

/* The api struct itself is DATA a ring3 app reads (api->puts, etc.), not
   code it fetches - still needs PAGE_USER (data reads are gated by the
   same U/S bit as instruction fetches), so it lives in .usertramp too,
   right alongside the code above. loader.c hands out &g_ring3_api to
   every ring3 app instead of &g_api (which still points at the real,
   privileged kernel functions and must never be handed to ring3 code -
   see task_create_app_ring3's own comment on why). */
__attribute__((section(".usertramp.data"))) app_api_t g_ring3_api = {
    .puts = t_puts,
    .putchar = t_putchar,
    .puts_col = t_puts_col,
    .fs_open = t_fs_open,
    .fs_read = t_fs_read,
    .fs_write = t_fs_write,
    .fs_close = t_fs_close,
    .fs_stat = t_fs_stat,
    .fs_inode_count = t_fs_inode_count,
    .fs_get_parent = t_fs_get_parent,
    .fs_delete = t_fs_delete,
    .fs_mkdir = t_fs_mkdir,
    .fs_rmdir = t_fs_rmdir,
    .fs_rrmdr = t_fs_rrmdr,
    .fs_isdir = t_fs_isdir,
    .getchar = t_getchar,
    .read_line = t_read_line,
    .get_time = t_get_time,
    .has_stdin = t_has_stdin,
    .read_stdin = t_read_stdin,
    .col_white = TERMINAL_WHITE,
    .col_green = TERMINAL_LIGHT_GREEN,
    .col_red = TERMINAL_LIGHT_RED,
    .col_blue = TERMINAL_LIGHT_BLUE,
    .col_grey = TERMINAL_LIGHT_GREY,
    .col_yellow = TERMINAL_YELLOW,
    .spawn_task = t_spawn_task,
    .task_yield = t_task_yield,
    .exit_task = t_exit_task,
    .dns_resolve = t_dns_resolve,
    .icmp_ping = t_icmp_ping,
    .http_get = t_http_get,
};