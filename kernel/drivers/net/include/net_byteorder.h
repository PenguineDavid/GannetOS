/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NET_BYTEORDER_H
#define NET_BYTEORDER_H

#include <stdint.h>

/* GannetOS only targets x86 (i386 today, x86_64 on the TODO list) - both
   little-endian - while every protocol header in ethernet.h, arp.h,
   ipv4.h and icmp.h is defined in network byte order (big-endian).
   net_swap16/32 do the same byte swap regardless of direction, since a
   swap is its own inverse; htons/ntohs/htonl/ntohl exist as separate
   names purely so call sites read as "converting to/from wire order"
   rather than "swapping bytes", matching BSD sockets convention even
   though on this architecture htons and ntohs are literally identical
   operations. */

static inline uint16_t net_swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t net_swap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

#define htons(v) net_swap16(v)
#define ntohs(v) net_swap16(v)
#define htonl(v) net_swap32(v)
#define ntohl(v) net_swap32(v)

#endif