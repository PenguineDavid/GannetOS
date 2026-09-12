/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

int app_main(int argc, char **argv, app_api_t *api)
{
    if (argc < 2)
    {
        api->puts("usage: read <file>\n");
        return 1;
    }

    int file_descriptor = api->fs_open(argv[1], 0);
    if (file_descriptor < 0)
    {
        api->puts_col(argv[1], api->col_red);
        api->puts(": no such file\n");
        return 1;
    }

    char ch;
    while (api->fs_read(file_descriptor, &ch, 1) == 1)
    {
        api->putchar(ch);
    }

    api->putchar('\n');
    api->fs_close(file_descriptor);
    return 0;
}