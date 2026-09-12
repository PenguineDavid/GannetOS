/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

// Writes argc-2 space-separated words (argv[2..argc-1]) into out, up to
// max_length bytes (not counting the NUL). Returns the number of bytes
// written. The shell's split_args() breaks "write file hello world" into
// three argv entries, so writing only argv[2] would silently drop every
// word after the first - this rejoins them with single spaces so the
// full text argument round-trips.
static int join_words(char **argv, int argc, char *out, int max_length)
{
    int length = 0;

    for (int arg_index = 2; arg_index < argc; arg_index++)
    {
        if (arg_index > 2 && length < max_length)
        {
            out[length++] = ' ';
        }

        const char *word = argv[arg_index];
        for (int char_index = 0; word[char_index] && length < max_length; char_index++)
        {
            out[length++] = word[char_index];
        }
    }

    return length;
}

int app_main(int argc, char **argv, app_api_t *api)
{
    if (argc < 3)
    {
        api->puts("usage: write <file> <text>\n");
        return 1;
    }

    int file_descriptor = api->fs_open(argv[1], 1);
    if (file_descriptor < 0)
    {
        api->puts_col("write: could not open/create file\n", api->col_red);
        return 1;
    }

    char text_buffer[512];
    int text_length = join_words(argv, argc, text_buffer, (int)sizeof(text_buffer));

    api->fs_write(file_descriptor, text_buffer, (uint32_t)text_length);
    api->fs_close(file_descriptor);
    api->puts_col("written\n", api->col_green);
    return 0;
}