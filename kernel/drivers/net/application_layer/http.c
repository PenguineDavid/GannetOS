/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/application_layer/http.h"
#include "kernel/drivers/net/application_layer/dns.h"
#include "kernel/drivers/net/transport_protocols/tcp.h"
#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/proc/task.h"
#include "kernel/drivers/pit/pit.h"

#define HTTP_MAX_REQUEST         512
#define HTTP_MAX_RESPONSE        8192 /* headers + as much body as this call will buffer at once - anything beyond this is read and discarded, not streamed to the caller */
#define HTTP_DISCARD_CHUNK       512
#define HTTP_TOTAL_TIMEOUT_TICKS 1000 /* 10s at pit_init(100) - see kernel.c, covers connect + send + the whole response */

static uint8_t recv_buf[HTTP_MAX_RESPONSE];

static int append_str(char *buf, uint32_t buf_len, uint32_t *pos, const char *s)
{
    while (*s)
    {
        if (*pos + 1 >= buf_len)
        {
            return 0; /* would overflow the request buffer - caller aborts the whole request rather than send a truncated one */
        }
        buf[(*pos)++] = *s++;
    }
    return 1;
}

int http_get(const char *host, uint16_t port, const char *path,
             char *body_buf, uint32_t body_buf_len)
{
    uint32_t ip;
    if (!dns_resolve(host, &ip))
    {
        return -1;
    }

    int handle = tcp_connect(ip, port);
    if (handle < 0)
    {
        return -2;
    }

    char request[HTTP_MAX_REQUEST];
    uint32_t pos = 0;
    int ok = 1;
    ok &= append_str(request, sizeof(request), &pos, "GET ");
    ok &= append_str(request, sizeof(request), &pos, path);
    ok &= append_str(request, sizeof(request), &pos, " HTTP/1.1\r\nHost: ");
    ok &= append_str(request, sizeof(request), &pos, host);
    ok &= append_str(request, sizeof(request), &pos, "\r\nConnection: close\r\n\r\n");
    if (!ok)
    {
        tcp_close(handle);
        return -3;
    }

    tcp_send(handle, request, (uint16_t)pos);

    uint32_t recv_len = 0;
    uint32_t start = pit_ticks();
    for (;;)
    {
        if (recv_len < sizeof(recv_buf))
        {
            recv_len += tcp_recv(handle, recv_buf + recv_len, (uint16_t)(sizeof(recv_buf) - recv_len));
        }
        else
        {
            /* Buffer's full - keep draining and discarding anyway so
               the connection's own flow control doesn't stall waiting
               for a reader that will never come; nothing further can
               be kept regardless. */
            char discard[HTTP_DISCARD_CHUNK];
            tcp_recv(handle, discard, sizeof(discard));
        }

        tcp_state_t state = tcp_state(handle);
        if (state == TCP_CLOSE_WAIT || state == TCP_CLOSED)
        {
            /* Peer is done sending (FIN seen) or the connection died
               (RST, or gave up retransmitting) - one more drain in
               case a final chunk arrived in the same segment as the
               FIN, then stop either way. */
            if (recv_len < sizeof(recv_buf))
            {
                recv_len += tcp_recv(handle, recv_buf + recv_len, (uint16_t)(sizeof(recv_buf) - recv_len));
            }
            break;
        }

        ethernet_poll();
        task_yield();
        if (pit_ticks() - start > HTTP_TOTAL_TIMEOUT_TICKS)
        {
            break; /* gave up - work with whatever arrived so far */
        }
    }

    tcp_close(handle); /* no-op if the peer already closed things down to CLOSE_WAIT/CLOSED */

    if (recv_len < 12)
    {
        return -3; /* not even enough for "HTTP/1.x NNN" */
    }

    /* Status line: "HTTP/1.x SSS <reason>\r\n" - the code is always
       exactly 3 digits starting right after the first space. */
    uint32_t i = 0;
    while (i < recv_len && recv_buf[i] != ' ')
    {
        i++;
    }
    if (i + 4 > recv_len)
    {
        return -3;
    }
    i++; /* skip the space */
    int status = 0;
    for (int d = 0; d < 3; d++)
    {
        uint8_t c = recv_buf[i + (uint32_t)d];
        if (c < '0' || c > '9')
        {
            return -3;
        }
        status = status * 10 + (c - '0');
    }

    /* The body starts right after the blank line ("\r\n\r\n") that
       ends the headers. */
    uint32_t body_start = 0;
    int found = 0;
    for (uint32_t j = 0; j + 3 < recv_len; j++)
    {
        if (recv_buf[j] == '\r' && recv_buf[j + 1] == '\n' && recv_buf[j + 2] == '\r' && recv_buf[j + 3] == '\n')
        {
            body_start = j + 4;
            found = 1;
            break;
        }
    }

    if (body_buf && body_buf_len > 0)
    {
        uint32_t n = 0;
        if (found && recv_len > body_start)
        {
            n = recv_len - body_start;
            if (n > body_buf_len - 1)
            {
                n = body_buf_len - 1;
            }
            for (uint32_t k = 0; k < n; k++)
            {
                body_buf[k] = (char)recv_buf[body_start + k];
            }
        }
        body_buf[n] = '\0';
    }

    return status;
}