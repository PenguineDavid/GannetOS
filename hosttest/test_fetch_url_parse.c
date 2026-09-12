/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable check of apps/fetch.c's parse_url() and
   default_filename() - the two pieces of that app with actual logic
   worth getting wrong (splitting host/port/path, and picking a
   filename when the caller doesn't supply one). Duplicates them rather
   than #including fetch.c, since fetch.c pulls in pexe.h's freestanding
   app_api_t - same reasoning as test_gdt_encoding.c. */
#include <stdio.h>
#include <string.h>

#define URL_PART_MAX 128

static int string_length(const char *text)
{
    int length = 0;
    while (text[length])
    {
        length++;
    }
    return length;
}

static int parse_unsigned_int(const char *text, int *value)
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

/* --- exact copy of apps/fetch.c's parse_url() --- */
static int parse_url(const char *url, char *host, int *port, char *path)
{
    const char *cursor = url;
    if (cursor[0] == 'h' && cursor[1] == 't' && cursor[2] == 't' && cursor[3] == 'p' && cursor[4] == ':' &&
        cursor[5] == '/' && cursor[6] == '/')
    {
        cursor += 7;
    }

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
        if (parse_unsigned_int(port_text, &parsed_port) && parsed_port > 0 && parsed_port <= 65535)
        {
            *port = parsed_port;
        }
    }

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

/* --- exact copy of apps/fetch.c's default_filename() --- */
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
/* --- end copy --- */

static int failures = 0;

static void check_url(const char *name, const char *url,
                      const char *expected_host, int expected_port,
                      const char *expected_path)
{
    char actual_host[URL_PART_MAX];
    char actual_path[URL_PART_MAX];
    int actual_port = -1;
    int parse_succeeded = parse_url(url, actual_host, &actual_port, actual_path);
    int test_passed = parse_succeeded && strcmp(actual_host, expected_host) == 0 &&
                      actual_port == expected_port && strcmp(actual_path, expected_path) == 0;
    printf("%s: %s (got host=\"%s\" port=%d path=\"%s\")\n",
           test_passed ? "PASS" : "FAIL", name, actual_host, actual_port, actual_path);
    if (!test_passed)
    {
        failures++;
    }
}

static void check_filename(const char *name, const char *path, const char *expected_filename)
{
    char actual_filename[URL_PART_MAX];
    default_filename(path, actual_filename);
    int test_passed = strcmp(actual_filename, expected_filename) == 0;
    printf("%s: %s (got \"%s\", want \"%s\")\n",
           test_passed ? "PASS" : "FAIL", name, actual_filename, expected_filename);
    if (!test_passed)
    {
        failures++;
    }
}

int main(void)
{
    check_url("scheme + host + path", "http://example.com/foo/bar.txt",
              "example.com", 80, "/foo/bar.txt");
    check_url("no scheme, defaults path to /", "example.com", "example.com", 80, "/");
    check_url("explicit port", "http://example.com:8080/x", "example.com", 8080, "/x");
    check_url("no scheme, explicit port, no path", "example.com:8080",
              "example.com", 8080, "/");
    check_url("root path only", "http://example.com/", "example.com", 80, "/");
    check_url("out-of-range port falls back to 80", "http://example.com:99999/x",
              "example.com", 80, "/x");

    {
        char host[URL_PART_MAX];
        char path[URL_PART_MAX];
        int port;
        int parse_succeeded = parse_url("", host, &port, path);
        printf("%s: empty url is rejected\n", parse_succeeded == 0 ? "PASS" : "FAIL");
        if (parse_succeeded != 0)
        {
            failures++;
        }
    }

    check_filename("last path segment becomes the filename", "/dir/report.pdf", "report.pdf");
    check_filename("root path falls back to index.html", "/", "index.html");
    check_filename("path ending in / falls back to index.html", "/dir/", "index.html");
    check_filename("single top-level segment", "/robots.txt", "robots.txt");

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}