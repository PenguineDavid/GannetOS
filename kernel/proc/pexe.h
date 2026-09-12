/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PEXE_H
#define PEXE_H

#include <stdint.h>

/* Custom bool definition for bare-metal C */
#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1
typedef _Bool bool;
#define true  1
#define false 0
#endif

#define PEXE_MAGIC 0x45584550u

/* Stage 3 (actual isolation): every task - including whichever task is
   running the currently-loaded app - has its own 4KB page mapped at this
   SAME virtual address, backed by a DIFFERENT physical frame per task.
   Just dereference it directly, e.g.
     volatile uint32_t *p = (volatile uint32_t *)APP_PRIVATE_VADDR;
   No syscall needed to use it - the mapping is already live for whichever
   task is currently executing. MUST match TASK_PRIVATE_VADDR in
   kernel/task.h. */
#define APP_PRIVATE_VADDR 0x40000000u

struct pexe_header {
    uint32_t magic;
    uint32_t entry_offset;
    uint32_t mem_size;
    uint32_t reserved1;
} __attribute__((packed));

typedef struct
{
    void (*puts)(const char *s);
    void (*putchar)(char c);
    void (*puts_col)(const char *s, uint32_t attr);   // now uint32_t

    int (*fs_open)(const char *path, int create);
    int (*fs_read)(int fd, void *buf, uint32_t len);
    int (*fs_write)(int fd, const void *buf, uint32_t len);
    void (*fs_close)(int fd);
    int (*fs_stat)(int idx, char *name, uint32_t *size, uint8_t *type);
    int (*fs_inode_count)(void);
    void (*fs_get_parent)(const char *path, char *parent);
    int (*fs_delete)(const char *path);
    int (*fs_mkdir)(const char *path);
    int (*fs_rmdir)(const char *path);
    int (*fs_rrmdr)(const char *path);
    int (*fs_isdir)(const char *path);

    int (*getchar)(void);
    int (*read_line)(char *buf, int maxlen);
    void (*get_time)(char *buf);

    // ---- Piping ----
    // True if this app is running as a non-first stage of a shell
    // pipeline (e.g. "producer | thisapp") and has piped-in input
    // waiting - check this before read_stdin() if the app wants to
    // behave differently depending on whether anything was piped in at
    // all (versus just getting an empty read right away).
    int (*has_stdin)(void);
    // Reads up to maxlen-1 bytes of the piped-in data into buf,
    // null-terminated, continuing from wherever the last call to this
    // left off (so repeated calls drain it incrementally, not re-read
    // from the start). Returns the number of bytes written - 0 once
    // the piped input is exhausted, or immediately if has_stdin() is
    // false. This is the previous pipeline stage's own output, captured
    // via the SAME pipe_active()/pipe_putchar() mechanism that
    // puts/putchar/puts_col already check - see kernel/pipe.c.
    int (*read_stdin)(char *buf, int maxlen);

    // Colors are now 32-bit RGB values
    uint32_t col_white;
    uint32_t col_green;
    uint32_t col_red;
    uint32_t col_blue;
    uint32_t col_grey;
    uint32_t col_yellow;

    // ---- Stage 3: actual isolation ----
    // Creates a new task running entry(arg), same as the kernel's own
    // task_create - see APP_PRIVATE_VADDR above for how to actually
    // observe it having its own private memory. Returns the task id
    // (>= 0), or -1 if the task/directory/frame pool is exhausted.
    int (*spawn_task)(void (*entry)(void *arg), void *arg);
    // Voluntarily gives up the CPU to another READY task, if any.
    void (*task_yield)(void);
    // Ends the CALLING task. Never returns. Call this from a spawned
    // task's entry function once it's done - not from the main app
    // (returning from app_main ends the app normally instead).
    void (*exit_task)(void);

    // ---- Networking ----
    // Resolves hostname (a dotted-decimal IPv4 literal, e.g.
    // "10.0.2.2", or a real DNS name, e.g. "google.com") to an IPv4
    // address in *ip_out (host byte order - e.g. print it as
    // (ip>>24)&0xFF . (ip>>16)&0xFF . (ip>>8)&0xFF . ip&0xFF). Blocks
    // for up to a few seconds if an actual DNS query is needed.
    // Returns 1 on success, 0 on failure/timeout/NXDOMAIN.
    int (*dns_resolve)(const char *hostname, uint32_t *ip_out);
    // Sends one ICMP echo request to dest_ip and blocks for up to a
    // couple of seconds for a matching reply. seq distinguishes
    // successive pings in the same session (see apps/ping.c). On
    // success, rtt_ticks_out (if non-NULL) receives the round-trip
    // time in PIT ticks - kernel.c's pit_init() call sets ticks per
    // second (100 as of this writing, so multiply by 10 for ms).
    // Returns 1 on a reply, 0 on timeout.
    int (*icmp_ping)(uint32_t dest_ip, uint16_t seq, uint32_t *rtt_ticks_out);
    // Performs one blocking HTTP/1.1 GET request for path on host (a
    // hostname or a dotted-decimal literal - resolved via dns_resolve())
    // at port, and copies up to body_buf_len-1 bytes of the RESPONSE
    // BODY (headers stripped) into body_buf, null-terminated (pass
    // NULL/0 to discard the body and just get the status code). See
    // kernel/drivers/net/application_layer/http.h's own doc comment for
    // the full behaviour (Connection: close, no chunked-encoding
    // decoding, etc). Returns the HTTP status code (e.g. 200, 404) on
    // success. Negative on failure before any status line was received:
    // -1 DNS resolution failed, -2 the TCP connection failed or was
    // refused, -3 no valid HTTP response arrived before the connection
    // closed or the overall timeout was reached.
    int (*http_get)(const char *host, uint16_t port, const char *path,
                     char *body_buf, uint32_t body_buf_len);
} app_api_t;

typedef int (*app_entry_t)(int argc, char **argv, app_api_t *api);

#endif