/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>

#define ETH_ADDR_LEN 6
#define ETH_HDR_LEN  14 /* dest(6) + src(6) + ethertype(2), no 802.1Q tagging support */

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806

/* Wire format, network byte order throughout - do not read ethertype
   directly as a plain uint16_t without ntohs()'ing it first. packed
   so sizeof(struct eth_header) is exactly ETH_HDR_LEN with no compiler
   padding, since this is laid straight over the raw bytes rtl8139
   hands back. */
struct eth_header
{
    uint8_t  dest[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t ethertype;
} __attribute__((packed));

extern const uint8_t eth_broadcast_mac[ETH_ADDR_LEN];

/* Call once, after rtl8139_init() has already found the card. This
   layer has no state of its own beyond what the driver already
   tracks - init exists mainly for symmetry with arp_init()/ipv4_init()
   in net_init(), and as a place to hang future state without having
   to change every caller. */
void ethernet_init(void);

/* Copies this machine's own MAC address into mac_out. Thin wrapper
   around rtl8139_get_mac so callers above this layer (arp.c, ipv4.c)
   never need to reach past ethernet.h down into the driver header
   directly. */
void ethernet_get_mac(uint8_t mac_out[ETH_ADDR_LEN]);

/* Wraps payload in an Ethernet header addressed to dest_mac and sends
   it. ethertype is given in HOST byte order (e.g. plain ETH_TYPE_IPV4)
   - this function does the htons() itself, callers should never
   pre-swap it. payload_len must leave room for ETH_HDR_LEN within
   RTL8139_MAX_FRAME. Returns 1 on success, 0 on a bad length or if the
   driver isn't initialized. */
int ethernet_send(const uint8_t dest_mac[ETH_ADDR_LEN], uint16_t ethertype,
                   const void *payload, uint16_t payload_len);

/* Polls the driver for one waiting frame (see rtl8139_receive - this
   is a non-blocking poll, not a wait) and, if one arrived, strips the
   Ethernet header and dispatches the payload to arp_handle_frame() or
   ipv4_handle_frame() based on ethertype. Anything else (802.1Q
   tagged frames, IPv6, etc.) is silently dropped - no protocol above
   this layer exists yet to hand it to. Call this often (e.g. once per
   kernel_main loop iteration, or from a dedicated polling task) or
   incoming ARP replies and IP datagrams just sit in the driver's ring
   buffer until the next call finally catches up to them. */
void ethernet_poll(void);

#endif