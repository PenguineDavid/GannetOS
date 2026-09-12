/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/*
 * Reads `nm build/kernel.elf`'s output - either from a file given as
 * argv[1], or from stdin if no argument is given - and checks that a
 * fixed set of start/end symbol pairs - linker.ld regions kernel/arch/
 * x86/paging.c's user_range_ok()/user_str_ok() depend on being real,
 * non-empty ranges - actually span more than zero bytes.
 *
 * Takes a FILE rather than reading `nm`'s output through a pipe because
 * a pipe in a Makefile recipe (`nm foo.elf | this_tool`) isn't reliably
 * forwarded through cmd.exe by GNU Make's native Windows port - see the
 * Makefile's own comment on why `check:` redirects nm's output to a
 * file with `>` and passes that file's path here instead.
 *
 * This exists because of a real, silent bug: linker.ld's .appstack
 * section used to have nothing placed in it (the static app_stack[]
 * array that used to carry __attribute__((section(".appstack"))) was
 * removed once apps got real per-task private stacks - see try_exec()
 * in loader.c). With nothing to advance the location counter inside an
 * otherwise-empty `.appstack { __appstack_start = .; ... __appstack_end
 * = .; }` block, both symbols land at the SAME address - a genuinely
 * zero-byte "valid" range. syscall.c's pointer validation then rejected
 * every pointer into an app's own stack (i.e. every local variable an
 * app passed to a syscall) as out of range, and silently no-op'd those
 * syscalls instead of performing them - apps kept running, so nothing
 * crashed; they just got back garbage or unchanged buffers from `ls`,
 * `cat`, `fs_get_parent`, `echo`'s arithmetic, and anything else whose
 * real arguments happened to live on the stack. That combination -
 * runs fine, silently wrong results, only for stack-based arguments -
 * is exactly the kind of thing that's fast to reproduce but slow to
 * diagnose by hand, which is why this exists: it turns "did a section
 * quietly go empty" into a one-line build failure instead.
 *
 * Wired into `make check` (Makefile) as a normal dependency, not just
 * documentation - run it, don't just read it.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define MAX_SYMBOLS 64
#define NAME_MAX 64

typedef struct
{
    char name[NAME_MAX];
    unsigned long addr;
} symbol_t;

static symbol_t symbols[MAX_SYMBOLS];
static int symbol_count;

static int find_symbol(const char *name, unsigned long *out_addr)
{
    for (int i = 0; i < symbol_count; i++)
    {
        if (strcmp(symbols[i].name, name) == 0)
        {
            *out_addr = symbols[i].addr;
            return 1;
        }
    }
    return 0;
}

/* {start symbol, end symbol, human label} - every fixed-vaddr region
   this project's own code depends on actually being wider than zero
   bytes. Add to this list if a future region gets the same treatment
   (a fixed link address whose content moved from "a real static array"
   to "reserved space backed some other way at runtime"). */
static const char *regions[][3] = {
    {"__appbuf_start", "__appbuf_end", "app exec buffer (.appbuf)"},
    {"__appstack_start", "__appstack_end", "app stack (.appstack)"},
    {"__usertramp_start", "__usertramp_end", "syscall trampolines (.usertramp)"},
};
#define REGION_COUNT (int)(sizeof(regions) / sizeof(regions[0]))

int main(int argc, char **argv)
{
    FILE *in = stdin;
    if (argc >= 2)
    {
        in = fopen(argv[1], "r");
        if (!in)
        {
            fprintf(stderr, "check_link_regions: couldn't open %s\n", argv[1]);
            return 1;
        }
    }

    char line[256];
    while (fgets(line, sizeof(line), in))
    {
        unsigned long addr;
        char type;
        char name[NAME_MAX];
        /* nm's normal format: "<hex address> <type char> <name>" */
        if (sscanf(line, "%lx %c %63s", &addr, &type, name) == 3)
        {
            if (symbol_count < MAX_SYMBOLS)
            {
                symbols[symbol_count].addr = addr;
                snprintf(symbols[symbol_count].name, NAME_MAX, "%s", name);
                symbol_count++;
            }
        }
    }

    int failures = 0;
    for (int i = 0; i < REGION_COUNT; i++)
    {
        const char *start_name = regions[i][0];
        const char *end_name = regions[i][1];
        const char *label = regions[i][2];

        unsigned long start_addr, end_addr;
        int have_start = find_symbol(start_name, &start_addr);
        int have_end = find_symbol(end_name, &end_addr);

        if (!have_start || !have_end)
        {
            printf("FAIL: %s - %s and/or %s not found in the symbol table "
                   "(is the kernel still built with symbols? was this "
                   "region renamed?)\n",
                   label, start_name, end_name);
            failures++;
            continue;
        }

        if (end_addr <= start_addr)
        {
            printf("FAIL: %s is %lu bytes wide (%s=0x%lx, %s=0x%lx) - "
                   "nothing is placed in this linker section anymore, so "
                   "every syscall pointer-validation check against it will "
                   "reject every real pointer into it\n",
                   label, end_addr - start_addr, start_name, start_addr,
                   end_name, end_addr);
            failures++;
            continue;
        }

        printf("PASS: %s is %lu bytes wide (0x%lx-0x%lx)\n",
               label, end_addr - start_addr, start_addr, end_addr);
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}