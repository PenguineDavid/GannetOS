/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

// Extract the filename/flag out of a fully qualified path (e.g., "/usr/-r" -> "-r")
static const char *get_filename(const char *path)
{
    const char *last_slash = path;
    while (*path)
    {
        if (*path == '/')
        {
            last_slash = path + 1;
        }
        path++;
    }
    return last_slash;
}

// Custom string equality helper
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

int app_main(int argc, char **argv, app_api_t *api)
{
    int recursive = 0;
    const char *target_path = 0;

    if (argc < 2)
    {
        api->puts("usage: rmdir [-r] <path>\n");
        return 1;
    }

    // Strip shell prefixing out of the first argument to verify flag states
    const char *base1 = get_filename(argv[1]);

    if (str_eq(base1, "-r"))
    {
        if (argc < 3)
        {
            api->puts("usage: rmdir [-r] <path>\n");
            return 1;
        }
        recursive = 1;
        target_path = argv[2];
    }
    else
    {
        // Secondary check case if the user supplied arguments as: rmdir <path> -r
        if (argc >= 3)
        {
            const char *base2 = get_filename(argv[2]);
            if (str_eq(base2, "-r"))
            {
                recursive = 1;
            }
        }
        target_path = argv[1];
    }

    // Call the matching kernel implementation directly
    int result;
    if (recursive)
    {
        result = api->fs_rrmdr(target_path);
    }
    else
    {
        result = api->fs_rmdir(target_path);
    }

    if (result == 0)
    {
        api->puts_col("removed\n", api->col_green);
    }
    else
    {
        api->puts_col(target_path, api->col_red);
        api->puts(": could not remove\n");
        return 1;
    }

    return 0;
}