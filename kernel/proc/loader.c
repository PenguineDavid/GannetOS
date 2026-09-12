/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/loader.h"
#include "kernel/proc/pexe.h"
#include "kernel/ui/wm_api.h"
#include "filesys/fs.h"
#include "kernel/ui/terminal.h"
#include "kernel/proc/pipe.h"
#include "kernel/drivers/keyboard/keyboard.h"
#include "kernel/drivers/rtc/rtc.h"
#include "kernel/ui/fb.h"
#include "kernel/ui/window.h"
#include "kernel/proc/task.h"
#include "kernel/arch/x86/paging.h"
#include <stdint.h>

#define EXEC_BUF_SIZE (64 * 1024)

static uint8_t exec_buf[EXEC_BUF_SIZE] __attribute__((section(".appbuf")));

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */
static void join_path(char *out, int max, const char *dir, const char *name)
{
    int i = 0;
    while (dir[i] && i < max - 1)
    {
        out[i] = dir[i];
        i++;
    }
    if (i < max - 1 && (i == 0 || out[i - 1] != '/'))
    {
        out[i++] = '/';
    }
    int j = 0;
    while (name[j] && i < max - 1)
    {
        out[i++] = name[j++];
    }
    out[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Debug                                                               */
/* ------------------------------------------------------------------ */
void loader_debug_inodes(void)
{
    terminal_puts("--- inodes ---\n", TERMINAL_YELLOW);
    for (int i = 0; i < fs_inode_count(); i++)
    {
        char name[128];
        uint32_t size;
        uint8_t type;
        if (fs_stat(i, name, &size, &type) != 0)
        {
            continue;
        }
        terminal_puts(name, TERMINAL_WHITE);
        terminal_putchar('\n', TERMINAL_WHITE);
    }
    terminal_puts("--- end ---\n", TERMINAL_YELLOW);
}

/* ------------------------------------------------------------------ */
/* load_pexe_blob: shared mechanics for both try_exec and wm_try_exec -- */
/* opens path, verifies the pexe header, reads the blob into exec_buf,  */
/* zero-fills any BSS past what was on disk, and returns a pointer to   */
/* the entry point. Returns NULL on any failure. Callers cast the      */
/* returned pointer to whichever entry function type they expect       */
/* (app_entry_t or wm_app_entry_t) and call it with their own api      */
/* struct - the pexe file format itself doesn't know or care which.    */
/* If out_mem_size is non-NULL, also writes back hdr.mem_size - used by */
/* try_exec to know how many private pages the app actually needs;      */
/* wm_try_exec (still ring0, still running straight out of exec_buf     */
/* today) has no use for it and passes NULL.                            */
/* ------------------------------------------------------------------ */
static void *load_pexe_blob(const char *path, uint32_t *out_mem_size)
{
    int fd = fs_open(path, 0);
    if (fd < 0)
    {
        return 0;
    }

    struct pexe_header hdr;
    if (fs_read(fd, &hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        hdr.magic != PEXE_MAGIC)
    {
        fs_close(fd);
        return 0;
    }

    int n = fs_read(fd, exec_buf, EXEC_BUF_SIZE);
    fs_close(fd);
    if (n <= 0)
    {
        return 0;
    }

    if (hdr.mem_size == 0 || hdr.mem_size > EXEC_BUF_SIZE ||
        (uint32_t)n > hdr.mem_size || hdr.entry_offset >= hdr.mem_size)
    {
        terminal_puts("app too large or corrupt to load\n", TERMINAL_LIGHT_RED);
        return 0;
    }
    for (uint32_t i = (uint32_t)n; i < hdr.mem_size; i++)
    {
        exec_buf[i] = 0;
    }

    if (out_mem_size)
    {
        *out_mem_size = hdr.mem_size;
    }
    return exec_buf + hdr.entry_offset;
}

static void enable_fpu_sse(void)
{
    uint16_t fpu_cw = 0x037F;
    asm volatile("fldcw %0" : : "m"(fpu_cw));
    asm volatile("fnclex");

    uint32_t sse_mxcsr = 0x1F80;
    asm volatile("ldmxcsr %0" : : "m"(sse_mxcsr));
}

/* ------------------------------------------------------------------ */
/* try_exec: load and run one absolute path as a normal app.          */
/* ------------------------------------------------------------------ */
extern app_api_t g_ring3_api; // kernel/user_trampolines.c

/* Ring3 app stack, sized in pages so it can be mapped into a new
   task's OWN private directory one paging_map_private_page() call per
   page (see try_exec below) - genuinely per-task now, unlike the
   comment this replaces ("Fixed, shared... Matches .appbuf's existing
   single-app-at-a-time assumption") used to say. The vaddr itself
   (APP_STACK_VADDR) is unchanged from before - still wherever
   linker.ld's .appstack section sits - only what backs it per-task is
   new; nothing about the Makefile, linker script, or compiled .pexe
   files had to change for that. */
#define APP_STACK_SIZE (16 * 1024)
#define APP_STACK_VADDR 0x210000u
#define APP_STACK_PAGES (APP_STACK_SIZE / PAGE_SIZE)

/* argv (the pointer array AND every string it points to) must be
   ring3-readable too, for the same reason g_ring3_api has to replace
   g_api - whatever the shell built argv out of lives in ordinary kernel
   memory, never PAGE_USER. Staged here first (plain kernel .bss, never
   itself ring3-accessible) with the exact layout it'll get once copied
   into the app's own private bottom-of-stack page: the strings
   themselves in the first APP_ARGV_AREA_SIZE bytes, followed by the
   argv_ptrs pointer array - see try_exec for the copy and the vaddr
   fixup (a pointer VALUE stored for the app to dereference has to be
   the vaddr it'll see under its OWN directory, never this staging
   buffer's own kernel address, and never the frame's physical address
   either). Truncates silently rather than failing outright - good
   enough for now, real bounds reporting is follow-up work like the
   rest of the not-yet-ported api surface. */
#define APP_ARGV_MAX 16
#define APP_ARGV_AREA_SIZE 2048
static char argv_stage_area[APP_ARGV_AREA_SIZE];
static uint32_t argv_stage_off[APP_ARGV_MAX]; /* byte offset of arg i within argv_stage_area */
static uint32_t argv_stage_len[APP_ARGV_MAX];

static int stage_argv(int argc, char **argv)
{
    if (argc > APP_ARGV_MAX)
    {
        argc = APP_ARGV_MAX;
    }
    int off = 0;
    for (int i = 0; i < argc; i++)
    {
        int len = 0;
        while (argv[i][len] && off + len + 1 < APP_ARGV_AREA_SIZE)
        {
            len++;
        }
        for (int j = 0; j < len; j++)
        {
            argv_stage_area[off + j] = argv[i][j];
        }
        argv_stage_area[off + len] = '\0';
        argv_stage_off[i] = (uint32_t)off;
        argv_stage_len[i] = (uint32_t)len;
        off += len + 1;
    }
    return argc;
}

static int try_exec(const char *path, int argc, char **argv)
{
    uint32_t mem_size = 0;
    void *entry_ptr = load_pexe_blob(path, &mem_size);
    if (!entry_ptr)
    {
        return -1;
    }

    /* From here on, exec_buf/argv are just kernel-side STAGING - the
       app's own private directory (a fresh paging_clone_kernel_
       directory(), populated below) is what it actually runs out of.
       Stage argv before anything can fail and bail early, same reason
       as reading the whole file before validating it above: keep the
       "things that can still fail" window as small and as early as
       possible. */
    int staged_argc = stage_argv(argc, argv);

    uint32_t dir_phys = paging_clone_kernel_directory();
    if (dir_phys == 0)
    {
        terminal_puts("try_exec: directory pool exhausted\n", TERMINAL_LIGHT_RED);
        return -1;
    }

    /* Copy the app's code+data into fresh private frames at
       APP_LOAD_ADDR, one page at a time - paging_map_private_page()
       correctly REUSES the same table across all of these calls since
       they land in the same 4MB PDE slot (APP_LOAD_ADDR is 0x200000;
       even the largest app, EXEC_BUF_SIZE=64KB, stays well inside that
       one slot). Each returned frame is a plain physical address,
       identity-mapped under the CALLING task's own (still-active)
       directory - see paging_map_private_page's own comment - so it's
       safe to memcpy into directly, before this new task ever runs and
       without switching CR3 to reach it.
       Only copy the bytes load_pexe_blob actually populated
       (min(PAGE_SIZE, remaining)) rather than a flat 4096 every time -
       pmm_alloc_frame() already zeroed the frame, so leaving the tail
       of the LAST page at that zero fill is both correct (matches what
       BSS past the file's on-disk content should be) and avoids
       leaking whatever stale bytes happened to be sitting in exec_buf
       past mem_size from a PREVIOUS app's load. */
    uint32_t code_pages = (mem_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t p = 0; p < code_pages; p++)
    {
        uint32_t frame = paging_map_private_page(dir_phys, APP_LOAD_ADDR + p * PAGE_SIZE);
        if (frame == 0)
        {
            terminal_puts("try_exec: out of memory loading app code\n", TERMINAL_LIGHT_RED);
            paging_free_private_pages(dir_phys);
            paging_free_directory(dir_phys);
            return -1;
        }
        uint32_t remaining = mem_size - p * PAGE_SIZE;
        uint32_t chunk = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;
        uint8_t *src = exec_buf + p * PAGE_SIZE;
        uint8_t *dst = (uint8_t *)frame;
        for (uint32_t i = 0; i < chunk; i++)
        {
            dst[i] = src[i];
        }
    }

    /* Same idea for the stack: private frames at APP_STACK_VADDR.
       Pages 1..APP_STACK_PAGES-1 need nothing written - a fresh,
       zeroed frame IS a valid blank stack page. Page 0 (the LOWEST
       address - stacks grow down from the top, so this is the
       farthest-away, last-to-be-touched end) also carries the staged
       argv content, copied in below with its pointer values fixed up
       to APP_STACK_VADDR-relative vaddrs - the same "bottom of the
       stack" placement the old shared design used, just now backed by
       a private frame instead of a shared static array. */
    uint32_t stack_frame0 = 0;
    for (uint32_t p = 0; p < APP_STACK_PAGES; p++)
    {
        uint32_t frame = paging_map_private_page(dir_phys, APP_STACK_VADDR + p * PAGE_SIZE);
        if (frame == 0)
        {
            terminal_puts("try_exec: out of memory allocating app stack\n", TERMINAL_LIGHT_RED);
            paging_free_private_pages(dir_phys);
            paging_free_directory(dir_phys);
            return -1;
        }
        if (p == 0)
        {
            stack_frame0 = frame;
        }
    }

    /* Copy the staged strings verbatim into the first APP_ARGV_AREA_SIZE
       bytes of that bottom stack page, then build the argv_ptrs array
       right after them - each entry gets the VADDR (APP_STACK_VADDR +
       offset) the app will see under its OWN directory, never this
       frame's physical address (only meaningful to code running under
       the CALLER's directory, which the app isn't) and never
       argv_stage_area's own kernel address (never ring3-readable at
       all). argv itself (the pointer the app receives) is therefore
       also a vaddr: APP_STACK_VADDR + APP_ARGV_AREA_SIZE. */
    char *strings_dst = (char *)stack_frame0;
    uint32_t *argv_ptrs_dst = (uint32_t *)(stack_frame0 + APP_ARGV_AREA_SIZE);
    for (int i = 0; i < staged_argc; i++)
    {
        uint32_t off = argv_stage_off[i], len = argv_stage_len[i];
        for (uint32_t j = 0; j <= len; j++) /* include the NUL */
        {
            strings_dst[off + j] = argv_stage_area[off + j];
        }
        argv_ptrs_dst[i] = APP_STACK_VADDR + off;
    }
    char **ring3_argv = (char **)(APP_STACK_VADDR + APP_ARGV_AREA_SIZE);

    kb_set_raw_mode(1);
    enable_fpu_sse();

    /* Runs the app as a genuine ring3 task instead of calling entry()
       directly at CPL0 - see task_create_app_ring3's own comment on
       why it gets g_ring3_api (syscall trampolines) here, never g_api
       (the real kernel functions g_ring3_api's trampolines actually
       trap into). The calling task (the shell) just yields in a loop
       until the app reaches TASK_ZOMBIE, exactly mirroring the
       synchronous, blocking behaviour entry(...) had before - nothing
       about the CALLING side's control flow changes, only what
       privilege level the app's own code executes at, and now, what
       physical memory backs it. */
    uint32_t user_stack_top = APP_STACK_VADDR + APP_STACK_SIZE;

    int app_id = task_create_app_ring3(dir_phys, (uint32_t)entry_ptr, staged_argc, ring3_argv,
                                        &g_ring3_api, user_stack_top, "app");
    int rc = -1;
    if (app_id >= 0)
    {
        while (!task_is_zombie(app_id))
        {
            task_yield();
        }
        /* MUST reap here, not just observe zombie status - see
           task_reap's own comment on why leaving this out silently
           exhausts TASK_MAX_TASKS after that many app launches (and
           now also leaks this app's private frames/tables - task_reap
           is what returns those too, via paging_free_private_pages). */
        task_reap(app_id);
        /* Exit code isn't plumbed back from SYS_EXIT_TASK yet (see
           user_app_crt0's own comment in isr_wrappers.s) - 0 stands in
           for "ran and returned" until that's wired up. */
        rc = 0;
    }
    else
    {
        /* Task table was full, or TASK_PRIVATE_VADDR mapping failed
           inside task_create_app_ring3 - either way dir_phys was never
           handed off to a task that'll eventually get reaped, so
           nothing will ever free it otherwise. */
        paging_free_private_pages(dir_phys);
        paging_free_directory(dir_phys);
    }

    kb_set_raw_mode(0);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Window manager loader: same pexe format, different syscall table.  */
/* ------------------------------------------------------------------ */
static int wm_api_should_quit(void)
{
    return wm_stop_requested;
}

static int wm_api_open_window(const char *name)
{
    /* entry = 0 (NULL): no dedicated task for this window - see the
       CURRENT LIMITATION note in wm_api.h for why. */
    return wm_open_window(0, name);
}

static wm_api_t g_wm_api = {
    .wm_init = wm_init,
    .wm_open_window = wm_api_open_window,
    .wm_close_window = wm_close_window,
    .wm_relayout = wm_relayout,
    .wm_present = wm_present,
    .win_fillrect = win_fillrect_id,
    .win_putpixel = win_putpixel_id,
    .win_clear = win_clear_id,
    .win_draw_string = win_draw_string_id,
    .getchar_nonblocking = kb_raw_getchar_nonblocking,
    .should_quit = wm_api_should_quit,
    .quit_ack = wm_stop_ack,
    .screen_width = 0,  // filled in per-call in wm_try_exec, once fb_info is known good
    .screen_height = 0,
};

int wm_try_exec(const char *path, int argc, char **argv)
{
    void *entry_ptr = load_pexe_blob(path, 0);
    if (!entry_ptr)
    {
        return -1;
    }

    g_wm_api.screen_width = fb_info.width;
    g_wm_api.screen_height = fb_info.height;

    wm_app_entry_t entry = (wm_app_entry_t)entry_ptr;
    kb_set_raw_mode(1);
    enable_fpu_sse();
    int rc = entry(argc, argv, &g_wm_api);
    kb_set_raw_mode(0);
    wm_stop_ack(); // clear any pending stop request so it doesn't leak into next time
    return rc;
}

/* ------------------------------------------------------------------ */
/* loader_exec                                                         */
/* ------------------------------------------------------------------ */
int loader_exec(const char *name, int argc, char **argv,
                const char *path_env, const char *cwd)
{
    int is_explicit = 0;
    for (int i = 0; name[i]; i++)
    {
        if (name[i] == '/')
        {
            is_explicit = 1;
            break;
        }
    }

    if (is_explicit)
    {
        return try_exec(name, argc, argv);
    }

    char path[128];

    if (cwd)
    {
        int cwd_in_path = 0;
        if (path_env)
        {
            const char *pp = path_env;
            int cwdlen = 0;
            while (cwd[cwdlen])
            {
                cwdlen++;
            }
            while (*pp)
            {
                int match = 1, ti = 0;
                while (pp[ti] && pp[ti] != ':')
                {
                    if (ti >= cwdlen || pp[ti] != cwd[ti])
                    {
                        match = 0;
                        break;
                    }
                    ti++;
                }
                if (match && ti == cwdlen && (pp[ti] == ':' || pp[ti] == '\0'))
                {
                    cwd_in_path = 1;
                    break;
                }
                while (*pp && *pp != ':')
                {
                    pp++;
                }
                if (*pp == ':')
                {
                    pp++;
                }
            }
        }
        if (!cwd_in_path)
        {
            join_path(path, 128, cwd, name);
            int r = try_exec(path, argc, argv);
            if (r != -1)
            {
                return r;
            }
        }
    }

    if (!path_env)
    {
        return -1;
    }

    const char *p = path_env;
    while (*p)
    {
        char dir[128];
        int di = 0;
        while (*p && *p != ':' && di < 127)
        {
            dir[di++] = *p++;
        }
        dir[di] = '\0';
        if (*p == ':')
        {
            p++;
        }

        if (!di)
        {
            continue;
        }

        join_path(path, 128, dir, name);
        int r = try_exec(path, argc, argv);
        if (r != -1)
        {
            return r;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* loader_init                                                         */
/* ------------------------------------------------------------------ */
void loader_init(void)
{
    if (!fs_isdir("/bin"))
    {
        fs_mkdir("/bin");
    }
    if (!fs_isdir("/usr"))
    {
        fs_mkdir("/usr");
    }
    if (!fs_isdir("/usr/bin"))
    {
        fs_mkdir("/usr/bin");
    }
    if (!fs_isdir("/home"))
    {
        fs_mkdir("/home");
    }
}