/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/internet_layer/icmp.h"
#include "kernel/drivers/net/internet_layer/ipv4.h"
#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/drivers/net/include/net_byteorder.h"
#include "kernel/proc/task.h"
#include "kernel/drivers/pit/pit.h"

#define ICMP_MAX_DATA 1400 /* comfortably under the 1500-byte MTU minus the IPv4 + ICMP headers */

static uint8_t reply_buf[ICMP_HDR_LEN + ICMP_MAX_DATA];

/* ---- client-side ping state --------------------------------------
   One outstanding ping at a time, same stop-and-wait philosophy as
   arp.c/tcp.c elsewhere in this stack - see icmp_ping()'s own header
   comment. identifier is fixed rather than randomised per call: with
   only one ping ever in flight, there's nothing for it to
   disambiguate against, unlike a real OS's ping where many processes
   might be pinging concurrently and need their own identifier to tell
   their replies apart from each other's. */
#define ICMP_PING_IDENTIFIER 0xC0DE
#define ICMP_PING_TIMEOUT_TICKS 200 /* 2s at pit_init(100) - see kernel.c */

struct ping_wait
{
    int      active;
    uint16_t seq;
    int      got_reply;
    uint32_t sent_tick;
    uint32_t rtt_ticks;
};
static struct ping_wait ping;

void icmp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip)
{
    if (len < ICMP_HDR_LEN)
    {
        return;
    }

    const struct icmp_header *hdr = (const struct icmp_header *)packet;

    if (hdr->type == ICMP_TYPE_ECHO_REPLY)
    {
        if (!ping.active)
        {
            return; /* nobody's waiting - not an error, just not for us */
        }
        if (ntohs(hdr->identifier) != ICMP_PING_IDENTIFIER || ntohs(hdr->sequence) != ping.seq)
        {
            return; /* somebody else's reply, or a stale one for an earlier seq */
        }
        if (ipv4_checksum(packet, len) != 0)
        {
            return; /* corrupt - see ipv4_checksum's own note on why a valid checksummed region always sums to 0 */
        }

        (void)src_ip; /* not checked - a reply is matched by identifier+sequence, same as a real ping does, not by source address */
        ping.got_reply = 1;
        ping.rtt_ticks = pit_ticks() - ping.sent_tick;
        return;
    }

    if (hdr->type != ICMP_TYPE_ECHO_REQUEST)
    {
        return; /* only echo request/reply are handled - everything else silently dropped */
    }

    if (ipv4_checksum(packet, len) != 0)
    {
        return;
    }

    uint16_t data_len = (uint16_t)(len - ICMP_HDR_LEN);
    if (data_len > ICMP_MAX_DATA)
    {
        return; /* larger than this stack is willing to echo back - drop rather than truncate a reply that would then fail its own checksum */
    }

    struct icmp_header *reply = (struct icmp_header *)reply_buf;
    reply->type       = ICMP_TYPE_ECHO_REPLY;
    reply->code       = 0;
    reply->checksum   = 0;
    reply->identifier = hdr->identifier; /* already network byte order, copied through unchanged */
    reply->sequence   = hdr->sequence;

    uint8_t *reply_data = reply_buf + ICMP_HDR_LEN;
    const uint8_t *req_data = packet + ICMP_HDR_LEN;
    for (uint16_t i = 0; i < data_len; i++)
    {
        reply_data[i] = req_data[i];
    }

    reply->checksum = htons(ipv4_checksum(reply_buf, (uint16_t)(ICMP_HDR_LEN + data_len)));

    ipv4_send(src_ip, IPV4_PROTO_ICMP, reply_buf, (uint16_t)(ICMP_HDR_LEN + data_len));
}

int icmp_ping(uint32_t dest_ip, uint16_t seq, uint32_t *rtt_ticks_out)
{
    uint8_t buf[ICMP_HDR_LEN];
    struct icmp_header *hdr = (struct icmp_header *)buf;
    hdr->type       = ICMP_TYPE_ECHO_REQUEST;
    hdr->code       = 0;
    hdr->checksum   = 0;
    hdr->identifier = htons(ICMP_PING_IDENTIFIER);
    hdr->sequence   = htons(seq);
    hdr->checksum   = htons(ipv4_checksum(buf, sizeof(buf)));

    ping.active     = 1;
    ping.seq        = seq;
    ping.got_reply  = 0;
    ping.sent_tick  = pit_ticks();

    if (!ipv4_send(dest_ip, IPV4_PROTO_ICMP, buf, sizeof(buf)))
    {
        ping.active = 0;
        return 0;
    }

    while (!ping.got_reply)
    {
        ethernet_poll();
        task_yield();
        if (pit_ticks() - ping.sent_tick > ICMP_PING_TIMEOUT_TICKS)
        {
            ping.active = 0;
            return 0;
        }
    }

    ping.active = 0;
    if (rtt_ticks_out)
    {
        *rtt_ticks_out = ping.rtt_ticks;
    }
    return 1;
}