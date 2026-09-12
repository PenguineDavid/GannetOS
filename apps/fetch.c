/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

/* fetch: downloads one file over plain HTTP (no TLS support in the
   network stack yet - see kernel/drivers/net/application_layer/http.h)
   and saves the response body to the filesystem.

   usage: fetch <url> [output_file]

   <url> is "[http://]host[:port][/path]" - the scheme is optional and
   only "http://" is accepted (there's nothing to strip a "https://"
   down to, so it's left as part of the hostname and will just fail DNS
   resolution, which is an honest failure mode for a stack with no TLS).
   Port defaults to 80, path defaults to "/". If [output_file] is
   omitted, the last path segment is used (falling back to
   "index.html" if the path is empty or ends in "/"). */

#define URL_PART_MAX 128
#define BODY_BUF_SIZE 8192 /* matches HTTP_MAX_RESPONSE in http.c - no
                               point asking for more than the kernel
                               will ever hand back in one call */

static int string_length(const char *text)
{
    int length = 0;
    while (text[length])
    {
        length++;
    }
    return length;
}

static int parse_unsigned_integer(const char *text, int *value)
{
    if (!*text)
    {
        return 0;
    }

    int parsed_value = 0;
    while (*text)
    {
        if (*text < '0' || *text > '9')
        {
            return 0;
        }
        parsed_value = parsed_value * 10 + (*text - '0');
        text++;
    }
    *value = parsed_value;
    return 1;
}

static void print_unsigned_integer(app_api_t *api, uint32_t value)
{
    if (value == 0)
    {
        api->putchar('0');
        return;
    }
    char digits[10];
    int digit_count = 0;
    while (value > 0)
    {
        digits[digit_count++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (int digit_index = digit_count - 1; digit_index >= 0; digit_index--)
    {
        api->putchar(digits[digit_index]);
    }
}

/* Splits url into host, port and path, each caller-supplied buffers of
   at least URL_PART_MAX bytes. Returns 0 on a malformed url (empty
   host), 1 on success. */
static int parse_url(const char *url, char *host, int *port, char *path)
{
    const char *cursor = url;
    if (cursor[0] == 'h' && cursor[1] == 't' && cursor[2] == 't' && cursor[3] == 'p' && cursor[4] == ':' &&
        cursor[5] == '/' && cursor[6] == '/')
    {
        cursor += 7;
    }

    /* host[:port] runs up to the first '/', or the end of the string. */
    int host_length = 0;
    char port_text[16];
    int port_length = 0;
    int reading_port = 0;
    while (*cursor && *cursor != '/')
    {
        if (*cursor == ':')
        {
            reading_port = 1;
        }
        else if (reading_port)
        {
            if (port_length < (int)sizeof(port_text) - 1)
            {
                port_text[port_length++] = *cursor;
            }
        }
        else if (host_length < URL_PART_MAX - 1)
        {
            host[host_length++] = *cursor;
        }
        cursor++;
    }

    host[host_length] = '\0';
    port_text[port_length] = '\0';
    if (host_length == 0)
    {
        return 0;
    }

    *port = 80;
    if (port_length > 0)
    {
        int parsed_port;
        if (parse_unsigned_integer(port_text, &parsed_port) && parsed_port > 0 && parsed_port <= 65535)
        {
            *port = parsed_port;
        }
    }

    /* Whatever's left (starting at '/', or nothing) is the path. */
    if (*cursor == '/')
    {
        int path_length = 0;
        while (*cursor && path_length < URL_PART_MAX - 1)
        {
            path[path_length++] = *cursor++;
        }
        path[path_length] = '\0';
    }
    else
    {
        path[0] = '/';
        path[1] = '\0';
    }
    return 1;
}

/* Derives a filename from the last "/"-separated segment of path,
   falling back to "index.html" if that segment is empty (path is "/"
   or ends in "/"). */
static void default_filename(const char *path, char *output)
{
    int path_length = string_length(path);
    int last_slash_index = -1;
    for (int index = 0; index < path_length; index++)
    {
        if (path[index] == '/')
        {
            last_slash_index = index;
        }
    }

    const char *filename = path + last_slash_index + 1;
    if (*filename == '\0')
    {
        const char *fallback_name = "index.html";
        int output_length = 0;
        while (fallback_name[output_length])
        {
            output[output_length] = fallback_name[output_length];
            output_length++;
        }
        output[output_length] = '\0';
        return;
    }

    int output_length = 0;
    while (filename[output_length] && output_length < URL_PART_MAX - 1)
    {
        output[output_length] = filename[output_length];
        output_length++;
    }
    output[output_length] = '\0';
}

static char body_buf[BODY_BUF_SIZE];

int app_main(int argc, char **argv, app_api_t *api)
{
    if (argc < 2)
    {
        api->puts("usage: fetch <url> [output_file]\n");
        return 1;
    }

    char host[URL_PART_MAX];
    char path[URL_PART_MAX];
    int port;
    if (!parse_url(argv[1], host, &port, path))
    {
        api->puts_col("could not parse url\n", api->col_red);
        return 1;
    }

    char out_name_buf[URL_PART_MAX];
    const char *out_name;
    if (argc >= 3)
    {
        out_name = argv[2];
    }
    else
    {
        default_filename(path, out_name_buf);
        out_name = out_name_buf;
    }

    api->puts("Fetching http://");
    api->puts(host);
    if (port != 80)
    {
        api->putchar(':');
        print_unsigned_integer(api, (uint32_t)port);
    }
    api->puts(path);
    api->puts(" ...\n");

    int status = api->http_get(host, (uint16_t)port, path, body_buf, sizeof(body_buf));

    if (status < 0)
    {
        switch (status)
        {
            case -1:
                api->puts_col("could not resolve host\n", api->col_red);
                break;
            case -2:
                api->puts_col("connection failed or was refused\n", api->col_red);
                break;
            default:
                api->puts_col("no valid response received (timed out)\n", api->col_red);
                break;
        }
        return 1;
    }

    api->puts("status: ");
    print_unsigned_integer(api, (uint32_t)status);
    api->putchar('\n');

    uint32_t body_length = (uint32_t)string_length(body_buf);
    if (body_length == 0)
    {
        api->puts_col("no body received, nothing saved\n", api->col_yellow);
        return status >= 200 && status < 300 ? 0 : 1;
    }

    api->fs_delete(out_name); /* fs_open(..., 1) may not truncate an existing file */
    int file_descriptor = api->fs_open(out_name, 1);
    if (file_descriptor < 0)
    {
        api->puts_col("could not open output file\n", api->col_red);
        return 1;
    }
    api->fs_write(file_descriptor, body_buf, body_length);
    api->fs_close(file_descriptor);

    if (status >= 200 && status < 300)
    {
        api->puts_col("saved ", api->col_green);
    }
    else
    {
        api->puts_col("saved (non-2xx status) ", api->col_yellow);
    }
    print_unsigned_integer(api, body_length);
    api->puts(" bytes to ");
    api->puts(out_name);
    api->putchar('\n');

    return status >= 200 && status < 300 ? 0 : 1;
}