/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

/* RFC 792 echo header, network byte order. The variable-length data
   that follows an echo request/reply (whatever the pinger put there,
   just echoed back verbatim) isn't part of this struct - callers
   handle it as a separate byte range immediately after. packed so
   sizeof(struct icmp_header) is exactly ICMP_HDR_LEN. */
struct icmp_header
{
    uint8_t  type;
    uint8_t  code;   /* always 0 for echo request/reply */
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

#define ICMP_HDR_LEN 8

/* Handles one ICMP packet already stripped of its IPv4 header. Called
   from ipv4_handle_frame() when protocol == IPV4_PROTO_ICMP - not
   normally called directly. Answers an echo request the same way it
   always has, and additionally checks an echo REPLY against whatever
   icmp_ping() (below) is currently waiting on. Everything else
   (destination unreachable, time exceeded, etc.) is silently
   dropped. */
void icmp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip);

/* Client-side ping: sends one ICMP echo request to dest_ip and blocks
   (via task_yield(), pumping ethernet_poll() while it waits) for up
   to a couple of seconds for a matching reply. seq lets a caller doing
   several pings in a row (see apps/ping.c) tell them apart; the
   identifier used on the wire is fixed internally, since this stack
   only ever has ONE ping outstanding at a time (same stop-and-wait
   philosophy as arp.c/tcp.c elsewhere) - don't call this again before
   a previous call has returned, and don't call it concurrently from
   two tasks. On success, rtt_ticks_out (if non-NULL) receives the
   round-trip time in PIT ticks - see kernel.c's pit_init() call for
   ticks-per-second. Returns 1 on a reply, 0 on timeout or a send
   failure. */
int icmp_ping(uint32_t dest_ip, uint16_t seq, uint32_t *rtt_ticks_out);

#endif