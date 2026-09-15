/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "shell/shell.h"
#include "kernel/drivers/keyboard/keyboard.h"
#include "filesys/fs.h"
#include "kernel/proc/loader.h"
#include "kernel/proc/pipe.h"
#include "kernel/drivers/rtc/rtc.h"
#include "kernel/ui/terminal.h"
#include "kernel/ui/fb.h"
#include <stdint.h>

#define CMD_BUF_SIZE 256
#define HOSTNAME "GannetOS"
#define PATH_MAX 128
#define MAX_ARGS 16
#define MAX_SEGMENTS 8

/* ------------------------------------------------------------------ */
/* Environment variable table                                          */
/* ------------------------------------------------------------------ */
#define ENV_MAX_VARS 32
#define ENV_NAME_MAX 32
#define ENV_VAL_MAX 128
#define OS_VERSION "GannetOS 0.0.1"

typedef struct
{
    char name[ENV_NAME_MAX];
    char val[ENV_VAL_MAX];
    int used;
} env_entry_t;

static env_entry_t env_table[ENV_MAX_VARS];

static const char *env_get(const char *name)
{
    for (int i = 0; i < ENV_MAX_VARS; i++)
    {
        if (env_table[i].used)
        {
            const char *a = env_table[i].name, *b = name;
            while (*a && *b && *a == *b)
            {
                a++;
                b++;
            }
            if (*a == '\0' && *b == '\0')
            {
                return env_table[i].val;
            }
        }
    }
    return 0;
}

static void env_set(const char *name, const char *val)
{
    for (int i = 0; i < ENV_MAX_VARS; i++)
    {
        if (env_table[i].used)
        {
            const char *a = env_table[i].name, *b = name;
            while (*a && *b && *a == *b)
            {
                a++;
                b++;
            }
            if (*a == '\0' && *b == '\0')
            {
                int j = 0;
                while (val[j] && j < ENV_VAL_MAX - 1)
                {
                    env_table[i].val[j] = val[j];
                    j++;
                }
                env_table[i].val[j] = '\0';
                return;
            }
        }
    }
    for (int i = 0; i < ENV_MAX_VARS; i++)
    {
        if (!env_table[i].used)
        {
            int j = 0;
            while (name[j] && j < ENV_NAME_MAX - 1)
            {
                env_table[i].name[j] = name[j];
                j++;
            }
            env_table[i].name[j] = '\0';
            j = 0;
            while (val[j] && j < ENV_VAL_MAX - 1)
            {
                env_table[i].val[j] = val[j];
                j++;
            }
            env_table[i].val[j] = '\0';
            env_table[i].used = 1;
            return;
        }
    }
}

static void env_unset(const char *name)
{
    for (int i = 0; i < ENV_MAX_VARS; i++)
    {
        if (env_table[i].used)
        {
            const char *a = env_table[i].name, *b = name;
            while (*a && *b && *a == *b)
            {
                a++;
                b++;
            }
            if (*a == '\0' && *b == '\0')
            {
                env_table[i].used = 0;
                env_table[i].name[0] = '\0';
                env_table[i].val[0] = '\0';
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Shell state                                                         */
/* ------------------------------------------------------------------ */
static char buf[CMD_BUF_SIZE];
static int buf_len = 0;
static int buf_cur = 0;
static volatile int cmd_pending = 0;

#define HISTORY_SIZE 16
static char history[HISTORY_SIZE][CMD_BUF_SIZE];
static int history_count = 0;
static int history_next = 0;
static int history_browse = -1;
static char history_saved_line[CMD_BUF_SIZE];
static uint8_t prompt_x = 0;
static uint8_t prompt_y = 0;

static char cwd[PATH_MAX] = "/";
static void sync_cwd_env(void)
{
    env_set("CWD", cwd);
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */
static void itoa_s(int val, char *out);

static int streq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a++ != *b++)
        {
            return 0;
        }
    }
    return *a == *b;
}
static int strpfx(const char *s, const char *p)
{
    while (*p)
    {
        if (*s++ != *p++)
        {
            return 0;
        }
    }
    return 1;
}
static int strlen_s(const char *s)
{
    int n = 0;
    while (*s++)
    {
        n++;
    }
    return n;
}
static void strcpy_s(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i])
    {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* $VAR expansion                                                      */
/* ------------------------------------------------------------------ */
static int is_var_start(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_var_body(char c)
{
    return is_var_start(c) || (c >= '0' && c <= '9');
}

static const char *resolve_var(const char *vname, char *time_buf_out /* char[9] */)
{
    if (streq(vname, "TIME"))
    {
        rtc_format_time(time_buf_out);
        return time_buf_out;
    }
    return env_get(vname);
}

static void expand_vars(const char *src, char *dst, int dst_max)
{
    int di = 0;
    const char *p = src;
    while (*p && di < dst_max - 1)
    {
        if (*p == '$' && is_var_start(p[1]))
        {
            p++;
            char vname[ENV_NAME_MAX];
            int vi = 0;
            while (is_var_body(*p) && vi < ENV_NAME_MAX - 1)
            {
                vname[vi++] = *p++;
            }
            vname[vi] = '\0';

            char time_buf[9];
            const char *val = resolve_var(vname, time_buf);
            if (val)
            {
                while (*val && di < dst_max - 1)
                {
                    dst[di++] = *val++;
                }
            }
        }
        else
        {
            dst[di++] = *p++;
        }
    }
    dst[di] = '\0';
}

/* ------------------------------------------------------------------ */
/* Path resolution                                                     */
/* ------------------------------------------------------------------ */
static void resolve_path(const char *input, char *abs_out)
{
    char tmp[PATH_MAX];
    if (input[0] == '/')
    {
        strcpy_s(tmp, input, PATH_MAX);
    }
    else if (input[0] == '~')
    {
        tmp[0] = '/', tmp[1] = 'h', tmp[2] = 'o', tmp[3] = 'm', tmp[4] = 'e', tmp[5] = '\0';
        if (input[1])
        {
            int i = 5;
            const char *p = input + 1;
            if (*p != '/')
            {
                tmp[i++] = '/';
            }
            while (*p && i < PATH_MAX - 1)
            {
                tmp[i++] = *p++;
            }
            tmp[i] = '\0';
        }
    }
    else
    {
        strcpy_s(tmp, cwd, PATH_MAX);
        int tlen = strlen_s(tmp);
        if (tlen < PATH_MAX - 2 && tmp[tlen - 1] != '/')
        {
            tmp[tlen++] = '/';
        }
        const char *p = input;
        while (*p && tlen < PATH_MAX - 1)
        {
            tmp[tlen++] = *p++;
        }
        tmp[tlen] = '\0';
    }

    char result[PATH_MAX];
    int ri = 0;
    result[ri++] = '/';
    result[ri] = '\0';
    const char *p = tmp;
    if (*p == '/')
    {
        p++;
    }
    while (*p)
    {
        char comp[64];
        int ci = 0;
        while (*p && *p != '/')
        {
            if (ci < 63)
            {
                comp[ci++] = *p;
            }
            p++;
        }
        comp[ci] = '\0';
        if (*p == '/')
        {
            p++;
        }
        if (!ci)
        {
            continue;
        }
        if (comp[0] == '.' && ci == 1)
        {
            continue;
        }
        if (comp[0] == '.' && comp[1] == '.' && ci == 2)
        {
            if (ri <= 1)
            {
                ri = 1;
                result[ri] = '\0';
            }
            else
            {
                if (result[ri - 1] == '/')
                {
                    ri--;
                }
                while (ri > 1 && result[ri - 1] != '/')
                {
                    ri--;
                }
                result[ri] = '\0';
            }
            continue;
        }
        if (ri > 1 && result[ri - 1] != '/')
        {
            result[ri++] = '/';
        }
        for (int j = 0; j < ci && ri < PATH_MAX - 1; j++)
        {
            result[ri++] = comp[j];
        }
        result[ri] = '\0';
    }
    if (ri > 1 && result[ri - 1] == '/')
    {
        result[--ri] = '\0';
    }
    strcpy_s(abs_out, result, PATH_MAX);
}

/* ------------------------------------------------------------------ */
/* cmd_takes_path                                                      */
/* ------------------------------------------------------------------ */
static int cmd_takes_path(const char *cmd)
{
    static const char *path_cmds[] = {"ls", "cat", "rm", "mkdir", "rmdir", "write", 0};
    for (int i = 0; path_cmds[i]; i++)
    {
        if (streq(cmd, path_cmds[i]))
        {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Argument splitter                                                   */
/* ------------------------------------------------------------------ */
static char *argv_store[MAX_ARGS];

static int split_args(char *seg, char **argv, int max_args)
{
    int argc = 0;
    char *p = seg;
    while (*p && argc < max_args)
    {
        while (*p == ' ')
        {
            p++;
        }
        if (!*p)
        {
            break;
        }

        if (*p == '"')
        {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"')
            {
                p++;
            }
            if (*p)
            {
                *p++ = '\0';
            }
        }
        else
        {
            argv[argc++] = p;
            while (*p && *p != ' ')
            {
                p++;
            }
            if (*p)
            {
                *p++ = '\0';
            }
        }
    }
    return argc;
}

/* ------------------------------------------------------------------ */
/* wm: manage userland window managers                                 */
/* ------------------------------------------------------------------ */
#define WM_DIR "/wm"
#define WM_DEFAULT_FILE "/etc/wm_default"

static void wm_puts(const char *s, uint32_t color)
{
    if (pipe_active())
    {
        while (*s)
        {
            pipe_putchar(*s++);
        }
    }
    else
    {
        terminal_puts(s, color);
    }
}

/* Reads /etc/wm_default into out (size max), empty string if unset. */
static void wm_read_default(char *out, int max)
{
    out[0] = '\0';
    int fd = fs_open(WM_DEFAULT_FILE, 0);
    if (fd < 0)
    {
        return;
    }
    int n = fs_read(fd, out, max - 1);
    fs_close(fd);
    if (n < 0)
    {
        n = 0;
    }
    out[n] = '\0';
    /* strip a trailing newline if present, so it matches typed names */
    if (n > 0 && out[n - 1] == '\n')
    {
        out[n - 1] = '\0';
    }
}

static void wm_write_default(const char *name)
{
    if (!fs_isdir("/etc"))
    {
        fs_mkdir("/etc");
    }
    fs_delete(WM_DEFAULT_FILE); /* fs_open(..., 1) may not truncate an existing file */
    int fd = fs_open(WM_DEFAULT_FILE, 1);
    if (fd < 0)
    {
        return;
    }
    int i = 0;
    while (name[i])
    {
        i++;
    }
    fs_write(fd, name, (uint32_t)i);
    fs_close(fd);
}

static void wm_command(int argc, char **argv)
{
    if (!fs_isdir(WM_DIR))
    {
        fs_mkdir(WM_DIR);
    }

    if (argc < 2)
    {
        wm_puts("usage: wm list | wm start [NAME] | wm stop | wm default NAME\n", TERMINAL_WHITE);
        return;
    }
    const char *sub = argv[1];

    if (streq(sub, "list"))
    {
        int found = 0;
        for (int i = 0; i < fs_inode_count(); i++)
        {
            char name[128];
            uint32_t size;
            uint8_t type;
            if (fs_stat(i, name, &size, &type) != 0 || type != FS_TYPE_FILE)
            {
                continue;
            }

            char parent[128];
            fs_get_parent(name, parent);
            if (!streq(parent, WM_DIR))
            {
                continue;
            }

            /* basename, minus a trailing .pexe if present */
            const char *base = name;
            for (int j = 0; name[j]; j++)
            {
                if (name[j] == '/')
                {
                    base = name + j + 1;
                }
            }
            char display[128];
            int bi = 0;
            while (base[bi])
            {
                display[bi] = base[bi];
                bi++;
            }
            if (bi > 5 && streq(display + bi - 5, ".pexe"))
            {
                bi -= 5;
            }
            display[bi] = '\0';

            wm_puts(display, TERMINAL_WHITE);
            wm_puts("\n", TERMINAL_WHITE);
            found++;
        }
        if (!found)
        {
            wm_puts("(no window managers installed in /wm)\n", TERMINAL_LIGHT_GREY);
        }
        return;
    }

    if (streq(sub, "start"))
    {
        char name[64];
        if (argc >= 3)
        {
            int i = 0;
            while (argv[2][i] && i < 63)
            {
                name[i] = argv[2][i];
                i++;
            }
            name[i] = '\0';
        }
        else
        {
            wm_read_default(name, sizeof(name));
            if (name[0] == '\0')
            {
                wm_puts("no WM name given and no default set - try 'wm list', ", TERMINAL_LIGHT_RED);
                wm_puts("or 'wm default NAME' to set one\n", TERMINAL_LIGHT_RED);
                return;
            }
        }

        char path[128];
        int i = 0, j = 0;
        const char *prefix = WM_DIR "/";
        while (prefix[i])
        {
            path[j++] = prefix[i++];
        }
        i = 0;
        while (name[i])
        {
            path[j++] = name[i++];
        }
        const char *suffix = ".pexe";
        i = 0;
        while (suffix[i])
        {
            path[j++] = suffix[i++];
        }
        path[j] = '\0';

        int rc = wm_try_exec(path, 1, (char *[]){(char *)name});
        /* The WM drew straight to the framebuffer the whole time it ran,
           potentially over the ENTIRE screen - but terminal_render() only
           redraws its own TERMINAL_COLS x TERMINAL_ROWS character grid
           (640x640 pixels here, not the full 1024x768 display), so it
           alone would leave whatever the WM drew outside that grid on
           screen forever. Blank the whole physical screen first, then let
           terminal_render() draw the actual prompt on top of that clean
           backdrop - fb_clear() only touches the backbuffer, so this adds
           no extra swap of its own; terminal_render()'s one swap at the
           end is what actually presents the fully-restored text mode. */
        fb_clear(0x000000);
        terminal_render();
        if (rc == -1)
        {
            wm_puts("wm: couldn't load '", TERMINAL_LIGHT_RED);
            wm_puts(name, TERMINAL_LIGHT_RED);
            wm_puts("' (not found in /wm, or corrupt)\n", TERMINAL_LIGHT_RED);
        }
        return;
    }

    if (streq(sub, "stop"))
    {
        /* A running WM has this same shell task's own CPU time for as long
           as it runs (GannetOS has one resident app at a time) - so there's
           no shell prompt to type this AT while a WM has control. This is
           only reachable once you're already back at the prompt, meaning
           nothing is running - the real way to stop a WM is F12, which is
           recognized at the keyboard level regardless of what's running. */
        wm_puts("no window manager is currently running (press F12 to stop one)\n", TERMINAL_LIGHT_GREY);
        return;
    }

    if (streq(sub, "default"))
    {
        if (argc < 3)
        {
            char name[64];
            wm_read_default(name, sizeof(name));
            if (name[0])
            {
                wm_puts(name, TERMINAL_WHITE);
                wm_puts("\n", TERMINAL_WHITE);
            }
            else
            {
                wm_puts("(none set)\n", TERMINAL_LIGHT_GREY);
            }
            return;
        }
        wm_write_default(argv[2]);
        wm_puts("default WM set to '", TERMINAL_WHITE);
        wm_puts(argv[2], TERMINAL_WHITE);
        wm_puts("' (used by 'wm start' with no name; boot still starts in text mode)\n", TERMINAL_WHITE);
        return;
    }

    wm_puts("wm: unknown subcommand - try list, start, stop, or default\n", TERMINAL_LIGHT_RED);
}

/* ------------------------------------------------------------------ */
/* Run one pipeline segment                                            */
/* ------------------------------------------------------------------ */
static char resolved_arg[PATH_MAX];
static char resolved_arg2[PATH_MAX];

static int run_segment(char *seg, const char *stdin_data)
{
    while (*seg == ' ')
    {
        seg++;
    }

    int argc = split_args(seg, argv_store, MAX_ARGS);
    if (!argc)
    {
        return 0;
    }
    const char *cmd = argv_store[0];

    if (argc == 1 && streq(cmd, "ls") && argc < MAX_ARGS)
    {
        argv_store[argc++] = cwd;
    }

    if (argc > 1 && cmd_takes_path(cmd))
    {
        if (streq(cmd, "rmdir") && streq(argv_store[1], "-r") && argc > 2)
        {
            resolve_path(argv_store[2], resolved_arg2);
            argv_store[2] = resolved_arg2;
        }
        else
        {
            resolve_path(argv_store[1], resolved_arg);
            argv_store[1] = resolved_arg;
        }
    }

    if (stdin_data && argc < MAX_ARGS)
    {
        argv_store[argc++] = (char *)stdin_data;
    }

    /* ---- Builtins ---- */
    if (streq(cmd, "clear"))
    {
        terminal_clear();
        return -2;
    }

    if (streq(cmd, "ver"))
    {
        const char *v = env_get("VER");
        if (!v)
        {
            v = OS_VERSION;
        }
        if (pipe_active())
        {
            while (*v)
            {
                pipe_putchar(*v++);
            }
            pipe_putchar('\n');
        }
        else
        {
            terminal_puts(v, TERMINAL_LIGHT_CYAN);
            terminal_putchar('\n', TERMINAL_WHITE);
        }
        return -2;
    }

    if (streq(cmd, "pwd"))
    {
        if (pipe_active())
        {
            const char *p = cwd;
            while (*p)
            {
                pipe_putchar(*p++);
            }
            pipe_putchar('\n');
        }
        else
        {
            terminal_puts(cwd, TERMINAL_WHITE);
            terminal_putchar('\n', TERMINAL_WHITE);
        }
        return -2;
    }

    if (streq(cmd, "cd"))
    {
        char cdpath[PATH_MAX];
        if (argc > 1)
        {
            resolve_path(argv_store[1], cdpath);
        }
        else
        {
            cdpath[0] = '/';
            cdpath[1] = '\0';
        }
        if (fs_isdir(cdpath))
        {
            strcpy_s(cwd, cdpath, PATH_MAX);
            sync_cwd_env();
        }
        else
        {
            terminal_puts(argc > 1 ? argv_store[1] : "~", TERMINAL_LIGHT_RED);
            terminal_puts(": no such directory\n", TERMINAL_LIGHT_RED);
        }
        return -2;
    }

    if (streq(cmd, "setenv"))
    {
        if (argc < 2)
        {
            terminal_puts("usage: setenv NAME [VALUE]\n", TERMINAL_YELLOW);
            return -2;
        }
        const char *varname = argv_store[1];
        if (streq(varname, "CWD") || streq(varname, "VER"))
        {
            terminal_puts(varname, TERMINAL_LIGHT_RED);
            terminal_puts(" is read-only\n", TERMINAL_LIGHT_RED);
            return -2;
        }
        if (argc >= 3)
        {
            env_set(varname, argv_store[2]);
        }
        else
        {
            env_unset(varname);
        }
        return -2;
    }

    if (streq(cmd, "getenv"))
    {
        if (argc < 2)
        {
            terminal_puts("usage: getenv NAME\n", TERMINAL_YELLOW);
            return -2;
        }
        const char *val = env_get(argv_store[1]);
        if (!val)
        {
            val = "";
        }
        if (pipe_active())
        {
            while (*val)
            {
                pipe_putchar(*val++);
            }
            pipe_putchar('\n');
        }
        else
        {
            terminal_puts(val, TERMINAL_WHITE);
            terminal_putchar('\n', TERMINAL_WHITE);
        }
        return -2;
    }

    if (streq(cmd, "printenv"))
    {
        for (int i = 0; i < ENV_MAX_VARS; i++)
        {
            if (!env_table[i].used)
            {
                continue;
            }
            if (pipe_active())
            {
                const char *n = env_table[i].name;
                while (*n)
                {
                    pipe_putchar(*n++);
                }
                pipe_putchar('=');
                const char *v = env_table[i].val;
                while (*v)
                {
                    pipe_putchar(*v++);
                }
                pipe_putchar('\n');
            }
            else
            {
                terminal_puts(env_table[i].name, TERMINAL_YELLOW);
                terminal_putchar('=', TERMINAL_WHITE);
                terminal_puts(env_table[i].val, TERMINAL_WHITE);
                terminal_putchar('\n', TERMINAL_WHITE);
            }
        }
        return -2;
    }

    if (streq(cmd, "wm"))
    {
        wm_command(argc, argv_store);
        return -2;
    }

    if (streq(cmd, "inodes"))
    {
        extern void loader_debug_inodes(void);
        loader_debug_inodes();
        return -2;
    }

    if (streq(cmd, "help"))
    {
        terminal_puts("Shell builtins:\n", TERMINAL_YELLOW);
        terminal_puts("  clear                 clear the screen\n", TERMINAL_WHITE);
        terminal_puts("  pwd                    print the current working directory\n", TERMINAL_WHITE);
        terminal_puts("  cd DIR                 change the current working directory\n", TERMINAL_WHITE);
        terminal_puts("  ver                    print the GannetOS version\n", TERMINAL_WHITE);
        terminal_puts("  setenv NAME [VALUE]    set NAME=VALUE, or unset NAME if VALUE is omitted\n", TERMINAL_WHITE);
        terminal_puts("  getenv NAME            print the value of ONE variable (empty if unset)\n", TERMINAL_WHITE);
        terminal_puts("  printenv               print EVERY set variable, one NAME=VALUE per line\n", TERMINAL_WHITE);
        terminal_puts("  $TIME                  live wall-clock time, HH:MM:SS (from the RTC,\n", TERMINAL_WHITE);
        terminal_puts("                         not a normal setenv'd variable)\n", TERMINAL_WHITE);
        terminal_puts("  for ((i=A; i OP B; i++)) do CMD done\n", TERMINAL_WHITE);
        terminal_puts("                         run CMD once per loop, with $i set each time.\n", TERMINAL_WHITE);
        terminal_puts("                         OP is one of < > <= >= == !=, step is ++ or --.\n", TERMINAL_WHITE);
        terminal_puts("  inodes                 dump raw filesystem inode table (debugging)\n", TERMINAL_WHITE);
        terminal_puts("  wm list                list window managers installed in /wm\n", TERMINAL_WHITE);
        terminal_puts("  wm start [NAME]        switch to graphics mode and run that WM (or the\n", TERMINAL_WHITE);
        terminal_puts("                         default one, see 'wm default', if NAME is omitted)\n", TERMINAL_WHITE);
        terminal_puts("  wm default NAME        set which WM 'wm start' runs with no name given\n", TERMINAL_WHITE);
        terminal_puts("                         (boot always starts in text mode regardless)\n", TERMINAL_WHITE);
        terminal_puts("  wm stop                (info only - press F12 to actually stop a running WM)\n", TERMINAL_WHITE);
        terminal_puts("  help                   this text\n", TERMINAL_WHITE);
        terminal_putchar('\n', TERMINAL_WHITE);
        terminal_puts("Everything else you type is looked for as a program: first in the\n", TERMINAL_WHITE);
        terminal_puts("current directory, then in PATH (default /bin:/usr/bin). Run 'ls /bin'\n", TERMINAL_WHITE);
        terminal_puts("to see what's actually installed -- things like echo, ls, cat, write,\n", TERMINAL_WHITE);
        terminal_puts("rm, mkdir, rmdir, and forth all live there as ordinary programs, not\n", TERMINAL_WHITE);
        terminal_puts("builtins, and each has its own usage message if you run it wrong.\n", TERMINAL_WHITE);
        terminal_putchar('\n', TERMINAL_WHITE);
        return -2;
    }

    /* ---- External executable ---- */
    const char *path_env = env_get("PATH");
    int r = loader_exec(cmd, argc, argv_store, path_env, cwd);
    if (r == -1)
    {
        terminal_puts(cmd, TERMINAL_LIGHT_RED);
        terminal_puts(": command not found\n", TERMINAL_LIGHT_RED);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Dispatch -- splits on | and runs pipeline                          */
/* ------------------------------------------------------------------ */
static char seg_store[MAX_SEGMENTS][CMD_BUF_SIZE];

/* ------------------------------------------------------------------ */
/* for loop builtin                                                    */
/* ------------------------------------------------------------------ */
static int atoi_s(const char *s)
{
    int neg = 0;
    if (*s == '-')
    {
        neg = 1;
        s++;
    }
    else if (*s == '+')
    {
        s++;
    }
    int v = 0;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (*s++ - '0');
    }
    return neg ? -v : v;
}

static void itoa_s(int val, char *out)
{
    char tmp[12];
    int i = 0;
    int neg = val < 0;
    unsigned int u = neg ? (unsigned int)(-val) : (unsigned int)val;
    do
    {
        tmp[i++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (neg)
    {
        tmp[i++] = '-';
    }
    int j = 0;
    while (i > 0)
    {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';
}

static char *trim(char *s)
{
    while (*s == ' ')
    {
        s++;
    }
    int len = strlen_s(s);
    while (len > 0 && s[len - 1] == ' ')
    {
        s[--len] = '\0';
    }
    return s;
}

static void for_err(const char *msg)
{
    terminal_puts("for: ", TERMINAL_LIGHT_RED);
    terminal_puts(msg, TERMINAL_LIGHT_RED);
    terminal_putchar('\n', TERMINAL_WHITE);
}

static void run_for_loop(char *raw)
{
    char *p = raw + 3;
    while (*p == ' ')
    {
        p++;
    }

    if (p[0] != '(' || p[1] != '(')
    {
        return for_err("expected (( after 'for'");
    }
    p += 2;
    char *header = p;
    char *hend = 0;
    for (char *q = p; q[0] && q[1]; q++)
    {
        if (q[0] == ')' && q[1] == ')')
        {
            hend = q;
            break;
        }
    }
    if (!hend)
    {
        return for_err("missing ))");
    }
    *hend = '\0';
    p = hend + 2;

    while (*p == ' ')
    {
        p++;
    }
    if (!(p[0] == 'd' && p[1] == 'o' && (p[2] == ' ' || p[2] == '\0')))
    {
        return for_err("expected 'do' after ((...))");
    }
    p += 2;
    while (*p == ' ')
    {
        p++;
    }
    char *body = p;

    int e = strlen_s(body);
    while (e > 0 && body[e - 1] == ' ')
    {
        e--;
    }
    if (e >= 4 && body[e - 4] == 'd' && body[e - 3] == 'o' &&
        body[e - 2] == 'n' && body[e - 1] == 'e' &&
        (e == 4 || body[e - 5] == ' '))
    {
        e -= 4;
        while (e > 0 && body[e - 1] == ' ')
        {
            e--;
        }
        body[e] = '\0';
    }
    else
    {
        return for_err("missing 'done'");
    }

    if (strlen_s(body) == 0)
    {
        return for_err("empty loop body");
    }

    char *parts[3];
    int np = 0;
    char *hp = header;
    parts[np++] = hp;
    while (*hp && np < 3)
    {
        if (*hp == ';')
        {
            *hp = '\0';
            hp++;
            if (np < 3)
            {
                parts[np++] = hp;
            }
        }
        else
        {
            hp++;
        }
    }
    if (np != 3)
    {
        return for_err("expected VAR=START; COND; VAR++ inside ((...))");
    }

    char *init_s = trim(parts[0]);
    char *cond_s = trim(parts[1]);
    char *incr_s = trim(parts[2]);

    char *eq = init_s;
    while (*eq && *eq != '=')
    {
        eq++;
    }
    if (!*eq)
    {
        return for_err("expected VAR=START");
    }
    *eq = '\0';
    char *varname = trim(init_s);
    int start_val = atoi_s(trim(eq + 1));

    int step;
    int ilen = strlen_s(incr_s);
    if (ilen >= 2 && incr_s[ilen - 2] == '+' && incr_s[ilen - 1] == '+')
    {
        step = 1;
    }
    else if (ilen >= 2 && incr_s[ilen - 2] == '-' && incr_s[ilen - 1] == '-')
    {
        step = -1;
    }
    else
    {
        return for_err("only VAR++ or VAR-- increments are supported");
    }

    char *op = 0;
    int oplen = 0;
    for (char *q = cond_s; *q; q++)
    {
        if ((q[0] == '<' || q[0] == '>' || q[0] == '=' || q[0] == '!') && q[1] == '=')
        {
            op = q;
            oplen = 2;
            break;
        }
        if (q[0] == '<' || q[0] == '>')
        {
            op = q;
            oplen = 1;
            break;
        }
    }
    if (!op)
    {
        return for_err("expected a comparison in the condition");
    }
    char opbuf[3];
    opbuf[0] = op[0];
    opbuf[1] = (oplen == 2) ? op[1] : '\0';
    opbuf[2] = '\0';
    *op = '\0';
    int end_val = atoi_s(trim(op + oplen));

    int val = start_val;
    int guard = 0;
    while (guard++ < 100000)
    {
        int cont;
        if (streq(opbuf, "<"))
        {
            cont = val < end_val;
        }
        else if (streq(opbuf, ">"))
        {
            cont = val > end_val;
        }
        else if (streq(opbuf, "<="))
        {
            cont = val <= end_val;
        }
        else if (streq(opbuf, ">="))
        {
            cont = val >= end_val;
        }
        else if (streq(opbuf, "=="))
        {
            cont = val == end_val;
        }
        else if (streq(opbuf, "!="))
        {
            cont = val != end_val;
        }
        else
        {
            cont = 0;
        }
        if (!cont)
        {
            break;
        }

        char valstr[12];
        itoa_s(val, valstr);
        env_set(varname, valstr);

        char expanded_body[CMD_BUF_SIZE];
        expand_vars(body, expanded_body, CMD_BUF_SIZE);
        run_segment(expanded_body, 0);

        val += step;
    }
}

static void dispatch(void)
{
    buf[buf_len] = '\0';
    if (!buf_len)
    {
        return;
    }

    {
        char *fp = buf;
        while (*fp == ' ')
        {
            fp++;
        }
        if (fp[0] == 'f' && fp[1] == 'o' && fp[2] == 'r' &&
            (fp[3] == ' ' || fp[3] == '\0'))
        {
            run_for_loop(fp);
            return;
        }
    }

    {
        // A line that's nothing but "$VARNAME" (whitespace aside) isn't a
        // real command - it's a request to see the variable's value. Handle
        // it directly here, before expand_vars/run_segment ever see it,
        // otherwise e.g. "$TIME" expands to something like "12:55:52" and
        // THAT gets treated as the command name, producing the nonsensical
        // "12:55:52: command not found".
        char *fp = buf;
        while (*fp == ' ')
        {
            fp++;
        }
        if (fp[0] == '$' && is_var_start(fp[1]))
        {
            char *vp = fp + 1;
            char vname[ENV_NAME_MAX];
            int vi = 0;
            while (is_var_body(*vp) && vi < ENV_NAME_MAX - 1)
            {
                vname[vi++] = *vp++;
            }
            vname[vi] = '\0';
            while (*vp == ' ')
            {
                vp++;
            }
            if (*vp == '\0')
            {
                char time_buf[9];
                const char *val = resolve_var(vname, time_buf);
                terminal_puts(val ? val : "", TERMINAL_WHITE);
                terminal_putchar('\n', TERMINAL_WHITE);
                return;
            }
        }
    }

    {
        char expanded[CMD_BUF_SIZE];
        expand_vars(buf, expanded, CMD_BUF_SIZE);
        strcpy_s(buf, expanded, CMD_BUF_SIZE);
        buf_len = strlen_s(buf);
        if (!buf_len)
        {
            return;
        }
    }

    int nseg = 0;
    char *p = buf;
    while (*p && nseg < MAX_SEGMENTS)
    {
        char *start = p;
        int depth = 0;
        while (*p)
        {
            if (p[0] == '(' && p[1] == '(')
            {
                depth++;
                p += 2;
                continue;
            }
            if (p[0] == ')' && p[1] == ')' && depth > 0)
            {
                depth--;
                p += 2;
                continue;
            }
            if (*p == '|' && depth == 0)
            {
                break;
            }
            p++;
        }
        int slen = (int)(p - start);
        if (slen >= CMD_BUF_SIZE)
        {
            slen = CMD_BUF_SIZE - 1;
        }
        for (int i = 0; i < slen; i++)
        {
            seg_store[nseg][i] = start[i];
        }
        seg_store[nseg][slen] = '\0';
        nseg++;
        if (*p == '|')
        {
            p++;
        }
    }

    if (nseg == 1)
    {
        pipe_set_stdin(0); /* clear any stale state from a PREVIOUS piped command */
        run_segment(seg_store[0], 0);
        return;
    }

    const char *stdin_data = 0;
    for (int i = 0; i < nseg; i++)
    {
        pipe_set_stdin(stdin_data); /* NULL on the first stage, previous stage's captured output after that */
        if (i < nseg - 1)
        {
            pipe_begin();
            run_segment(seg_store[i], stdin_data);
            stdin_data = pipe_end();
        }
        else
        {
            run_segment(seg_store[i], stdin_data);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Prompt                                                              */
/* ------------------------------------------------------------------ */
static void print_prompt(void)
{
    char display[PATH_MAX];
    if (strpfx(cwd, "/home"))
    {
        display[0] = '~';
        strcpy_s(display + 1, cwd + 5, PATH_MAX - 1);
    }
    else
    {
        strcpy_s(display, cwd, PATH_MAX);
    }
    if (!display[0])
    {
        display[0] = '/';
        display[1] = '\0';
    }

    terminal_puts(HOSTNAME, TERMINAL_LIGHT_GREEN);
    terminal_putchar(':', TERMINAL_WHITE);
    terminal_puts(display, TERMINAL_LIGHT_BLUE);
    terminal_puts("$ ", TERMINAL_WHITE);
    terminal_get_cursor(&prompt_x, &prompt_y);
}

/* ------------------------------------------------------------------ */
/* Redraw                                                              */
/* ------------------------------------------------------------------ */
static void redraw_line(void)
{
    terminal_set_cursor(prompt_x, prompt_y);
    for (int i = 0; i < buf_len; i++)
    {
        terminal_putchar(buf[i], TERMINAL_WHITE);
    }
    terminal_write_at(
        (uint8_t)((prompt_x + buf_len) % TERMINAL_COLS),
        (uint8_t)(prompt_y + (prompt_x + buf_len) / TERMINAL_COLS),
        ' ', TERMINAL_WHITE);
    terminal_set_cursor(
        (uint8_t)((prompt_x + buf_cur) % TERMINAL_COLS),
        (uint8_t)(prompt_y + (prompt_x + buf_cur) / TERMINAL_COLS));
}

static void history_push(const char *line)
{
    if (!line[0])
    {
        return;
    }
    if (history_count > 0)
    {
        int last = (history_next - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        if (streq(history[last], line))
        {
            return;
        }
    }
    strcpy_s(history[history_next], line, CMD_BUF_SIZE);
    history_next = (history_next + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE)
    {
        history_count++;
    }
}

static void set_line_from_history(const char *s)
{
    int old_len = buf_len;
    int new_len = 0;
    while (s[new_len] && new_len < CMD_BUF_SIZE - 1)
    {
        buf[new_len] = s[new_len];
        new_len++;
    }
    buf_len = new_len;
    buf_cur = new_len;

    terminal_set_cursor(prompt_x, prompt_y);
    for (int i = 0; i < buf_len; i++)
    {
        terminal_putchar(buf[i], TERMINAL_WHITE);
    }
    int clear_to = old_len > buf_len ? old_len : buf_len;
    for (int i = buf_len; i <= clear_to; i++)
    {
        terminal_write_at(
            (uint8_t)((prompt_x + i) % TERMINAL_COLS),
            (uint8_t)(prompt_y + (prompt_x + i) / TERMINAL_COLS),
            ' ', TERMINAL_WHITE);
    }
    terminal_set_cursor(
        (uint8_t)((prompt_x + buf_cur) % TERMINAL_COLS),
        (uint8_t)(prompt_y + (prompt_x + buf_cur) / TERMINAL_COLS));
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
void shell_init(void)
{
    // terminal_init() is called from kernel.c - do NOT call it again
    buf_len = 0;
    buf_cur = 0;

    for (int i = 0; i < ENV_MAX_VARS; i++)
    {
        env_table[i].used = 0;
        env_table[i].name[0] = '\0';
        env_table[i].val[0] = '\0';
    }
    env_set("PATH", "/bin:/usr/bin");
    env_set("VER", OS_VERSION);
    sync_cwd_env();

    if (!fs_isdir("/"))
    {
        fs_mkdir("/");
    }

    print_prompt();
}

void shell_handle_char(char c)
{
    terminal_scroll_reset();
    if (c == '\n')
    {
        buf[buf_len] = '\0';
        history_push(buf);
        history_browse = -1;
        terminal_set_cursor(
            (uint8_t)((prompt_x + buf_len) % TERMINAL_COLS),
            (uint8_t)(prompt_y + (prompt_x + buf_len) / TERMINAL_COLS));
        terminal_putchar('\n', TERMINAL_WHITE);
        cmd_pending = 1;
        return;
    }
    if (c == '\b')
    {
        if (buf_cur > 0)
        {
            for (int i = buf_cur - 1; i < buf_len - 1; i++)
            {
                buf[i] = buf[i + 1];
            }
            buf_len--;
            buf_cur--;
            redraw_line();
        }
        return;
    }
    if (buf_len < CMD_BUF_SIZE - 1)
    {
        for (int i = buf_len; i > buf_cur; i--)
        {
            buf[i] = buf[i - 1];
        }
        buf[buf_cur++] = c;
        buf_len++;
        redraw_line();
    }
}

int shell_has_pending_command(void)
{
    return cmd_pending;
}

void shell_run_pending_command(void)
{
    dispatch();
    buf_len = 0;
    buf_cur = 0;
    print_prompt();
    cmd_pending = 0;
}

void shell_handle_key(int key)
{
    terminal_scroll_reset();
    switch (key)
    {
    case SHELL_KEY_LEFT:
        if (buf_cur > 0)
        {
            buf_cur--;
            redraw_line();
        }
        break;
    case SHELL_KEY_RIGHT:
        if (buf_cur < buf_len)
        {
            buf_cur++;
            redraw_line();
        }
        break;
    case SHELL_KEY_HOME:
        buf_cur = 0;
        redraw_line();
        break;
    case SHELL_KEY_END:
        buf_cur = buf_len;
        redraw_line();
        break;
    case SHELL_KEY_UP:
        if (history_count == 0)
        {
            break;
        }
        if (history_browse == -1)
        {
            buf[buf_len] = '\0';
            strcpy_s(history_saved_line, buf, CMD_BUF_SIZE);
            history_browse = 0;
        }
        else if (history_browse < history_count - 1)
        {
            history_browse++;
        }
        {
            int idx = (history_next - 1 - history_browse + HISTORY_SIZE) % HISTORY_SIZE;
            set_line_from_history(history[idx]);
        }
        break;
    case SHELL_KEY_DOWN:
        if (history_browse == -1)
        {
            break;
        }
        if (history_browse == 0)
        {
            history_browse = -1;
            set_line_from_history(history_saved_line);
        }
        else
        {
            history_browse--;
            int idx = (history_next - 1 - history_browse + HISTORY_SIZE) % HISTORY_SIZE;
            set_line_from_history(history[idx]);
        }
        break;
    case SHELL_KEY_DEL:
        if (buf_cur < buf_len)
        {
            for (int i = buf_cur; i < buf_len - 1; i++)
            {
                buf[i] = buf[i + 1];
            }
            buf_len--;
            redraw_line();
        }
        break;
    default:
        break;
    }
}
