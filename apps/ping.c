/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/proc/pexe.h"

/* ------------------------------------------------------------------ */
/* Tiny helpers - no libc, so print_uint replaces printf("%u") and    */
/* parse_uint replaces atoi() for the optional count argument.        */
/* ------------------------------------------------------------------ */

// Parses an unsigned decimal integer from text. Returns 0 (and leaves
// *out untouched) on an empty string or any non-digit character.
static int parse_uint(const char *text, int *out)
{
    if (!*text)
    {
        return 0;
    }

    int value = 0;
    while (*text)
    {
        if (*text < '0' || *text > '9')
        {
            return 0;
        }
        value = value * 10 + (*text - '0');
        text++;
    }

    *out = value;
    return 1;
}

static void print_uint(app_api_t *api, uint32_t value)
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

static void print_ip(app_api_t *api, uint32_t ip)
{
    print_uint(api, (ip >> 24) & 0xFF);
    api->putchar('.');
    print_uint(api, (ip >> 16) & 0xFF);
    api->putchar('.');
    print_uint(api, (ip >> 8) & 0xFF);
    api->putchar('.');
    print_uint(api, ip & 0xFF);
}

/* ------------------------------------------------------------------ */
/* app_main                                                           */
/* ------------------------------------------------------------------ */
int app_main(int argc, char **argv, app_api_t *api)
{
    if (argc < 2)
    {
        api->puts("usage: ping <host> [count]\n");
        return 1;
    }

    const char *host = argv[1];
    int count = 4; /* matches the classic default most `ping` implementations use */
    if (argc >= 3)
    {
        int parsed_count;
        if (parse_uint(argv[2], &parsed_count) && parsed_count > 0 && parsed_count <= 100)
        {
            count = parsed_count;
        }
    }

    api->puts("Resolving ");
    api->puts(host);
    api->puts("...\n");

    uint32_t ip;
    if (!api->dns_resolve(host, &ip))
    {
        api->puts_col("could not resolve host\n", api->col_red);
        return 1;
    }

    api->puts("PING ");
    api->puts(host);
    api->puts(" [");
    print_ip(api, ip);
    api->puts("]\n");

    int sent = 0, received = 0;
    uint32_t rtt_min = 0, rtt_max = 0, rtt_total = 0;

    for (int seq = 1; seq <= count; seq++)
    {
        sent++;

        uint32_t rtt_ticks = 0;
        if (api->icmp_ping(ip, (uint16_t)seq, &rtt_ticks))
        {
            received++;
            /* pit_init(100) in kernel.c means 100 ticks/second, so
               10ms per tick - see icmp_ping's own doc comment on the
               same assumption. */
            uint32_t rtt_ms = rtt_ticks * 10;
            if (received == 1 || rtt_ms < rtt_min)
            {
                rtt_min = rtt_ms;
            }
            if (rtt_ms > rtt_max)
            {
                rtt_max = rtt_ms;
            }
            rtt_total += rtt_ms;

            api->puts("reply from ");
            print_ip(api, ip);
            api->puts(": seq=");
            print_uint(api, (uint32_t)seq);
            api->puts(" time=");
            print_uint(api, rtt_ms);
            api->puts("ms\n");
        }
        else
        {
            api->puts_col("request timed out\n", api->col_yellow);
        }

        api->task_yield();
    }

    api->puts("\n--- ");
    api->puts(host);
    api->puts(" ping statistics ---\n");
    print_uint(api, (uint32_t)sent);
    api->puts(" sent, ");
    print_uint(api, (uint32_t)received);
    api->puts(" received");

    if (received > 0)
    {
        api->puts(", min/avg/max = ");
        print_uint(api, rtt_min);
        api->puts("/");
        print_uint(api, rtt_total / (uint32_t)received);
        api->puts("/");
        print_uint(api, rtt_max);
        api->puts("ms");
    }
    api->putchar('\n');

    return received > 0 ? 0 : 1;
}