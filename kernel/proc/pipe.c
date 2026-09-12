/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pipe.h"

static char pipe_buf[PIPE_BUF_SIZE];
static int pipe_pos = 0;
static int pipe_on = 0;

void pipe_begin(void)
{
    pipe_pos = 0;
    pipe_on = 1;
    pipe_buf[0] = '\0';
}

const char *pipe_end(void)
{
    pipe_buf[pipe_pos] = '\0';
    pipe_on = 0;
    return pipe_buf;
}

int pipe_active(void)
{
    return pipe_on;
}

void pipe_putchar(char c)
{
    if (pipe_pos < PIPE_BUF_SIZE - 1)
    {
        pipe_buf[pipe_pos++] = c;
        pipe_buf[pipe_pos] = '\0';
    }
}

static const char *stdin_data = 0;
static int stdin_pos = 0;

void pipe_set_stdin(const char *data)
{
    stdin_data = data;
    stdin_pos = 0;
}

int pipe_has_stdin(void)
{
    return stdin_data != 0;
}

int pipe_read_stdin(char *buf, int maxlen)
{
    if (!stdin_data || maxlen <= 0)
    {
        if (maxlen > 0)
        {
            buf[0] = '\0';
        }
        return 0;
    }

    int n = 0;
    while (stdin_data[stdin_pos] && n < maxlen - 1)
    {
        buf[n++] = stdin_data[stdin_pos++];
    }
    buf[n] = '\0';
    return n;
}