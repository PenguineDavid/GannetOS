/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

int app_main(int argc, char **argv, app_api_t *api)
{
    if (argc < 2)
    {
        api->puts("usage: mkdir <path>\n");
        return 1;
    }

    if (api->fs_mkdir(argv[1]) == 0)
    {
        api->puts_col("created\n", api->col_green);
    }
    else
    {
        api->puts_col(argv[1], api->col_red);
        api->puts(": could not create\n");
        return 1;
    }

    return 0;
}