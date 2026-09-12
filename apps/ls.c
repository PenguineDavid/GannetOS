/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

int app_main(int argc, char **argv, app_api_t *api)
{
    const char *path = (argc > 1) ? argv[1] : "/";
    int found = 0;

    for (int i = 0; i < api->fs_inode_count(); i++)
    {
        char name[128]; /* must match FS_NAME_MAX */
        uint32_t size;
        uint8_t type;
        if (api->fs_stat(i, name, &size, &type) != 0)
        {
            continue;
        }

        /* Skip the directory inode itself -- show only its children. */
        {
            const char *name_cursor = name, *path_cursor = path;
            int is_same = 1;
            while (*name_cursor && *path_cursor)
            {
                if (*name_cursor++ != *path_cursor++)
                {
                    is_same = 0;
                    break;
                }
            }
            if (is_same && *name_cursor == *path_cursor)
            {
                continue;
            }
        }

        /* Get parent of this inode */
        char parent[128]; /* must match FS_NAME_MAX */
        api->fs_get_parent(name, parent);

        /* Match parent against path */
        const char *parent_cursor = parent, *path_cursor = path;
        int is_match = 1;
        while (*parent_cursor && *path_cursor)
        {
            if (*parent_cursor++ != *path_cursor++)
            {
                is_match = 0;
                break;
            }
        }
        if (is_match && *parent_cursor != *path_cursor)
        {
            is_match = 0;
        }
        if (!is_match)
        {
            continue;
        }

        /* Basename */
        const char *base_name = name;
        for (int char_index = 0; name[char_index]; char_index++)
        {
            if (name[char_index] == '/')
            {
                base_name = name + char_index + 1;
            }
        }

        if (type == 2)
        {
            api->puts_col(base_name, api->col_blue);
        }
        else
        {
            api->puts_col(base_name, api->col_white);
        }

        if (type == 1)
        {
            // Render the file size as decimal digits, least significant
            // digit first into a scratch buffer, then flip it into the
            // output buffer in the correct order.
            char size_buffer[12];
            int size_digit_count = 0;
            uint32_t remaining_size = size;
            if (!remaining_size)
            {
                size_buffer[size_digit_count++] = '0';
            }
            else
            {
                char reversed_digits[12];
                int reversed_count = 0;
                while (remaining_size)
                {
                    reversed_digits[reversed_count++] = (char)('0' + remaining_size % 10);
                    remaining_size /= 10;
                }
                for (int digit_index = reversed_count - 1; digit_index >= 0; digit_index--)
                {
                    size_buffer[size_digit_count++] = reversed_digits[digit_index];
                }
            }
            size_buffer[size_digit_count] = '\0';

            api->putchar(' ');
            api->puts_col(size_buffer, api->col_grey);
            api->puts_col(" bytes", api->col_grey);
        }
        else if (type == 2)
        {
            api->puts_col("/", api->col_blue);
        }

        api->putchar('\n');
        found++;
    }

    if (!found)
    {
        api->puts_col("(empty)\n", api->col_grey);
    }

    return 0;
}