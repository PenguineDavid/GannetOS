/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "kernel/drivers/net/link_layer/ethernet.h"

#define ARP_HTYPE_ETHERNET 1
#define ARP_PTYPE_IPV4      0x0800
#define ARP_HLEN_ETHERNET   ETH_ADDR_LEN
#define ARP_PLEN_IPV4       4

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

/* RFC 826 wire format, network byte order throughout. htype/ptype/
   hlen/plen are real fields (rather than hardcoded on the wire) since
   that's what makes this a general ARP packet and not just an
   Ethernet/IPv4-specific one, but this stack only ever checks them
   against ARP_HLEN_ETHERNET/ARP_PLEN_IPV4 on receive - a packet
   advertising different address sizes is dropped rather than parsed
   generically (see arp_handle_frame). packed so sizeof(struct
   arp_packet) is exactly 28 bytes with no compiler-inserted padding. */
struct arp_packet
{
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[ETH_ADDR_LEN]; /* sender hardware address */
    uint32_t spa;               /* sender protocol address, network byte order */
    uint8_t  tha[ETH_ADDR_LEN]; /* target hardware address - all-zero in a request */
    uint32_t tpa;               /* target protocol address, network byte order */
} __attribute__((packed));

/* Call once, before anything sends or receives IP traffic. Clears the
   resolution cache. */
void arp_init(void);

/* Handles one ARP packet already stripped of its Ethernet header.
   src_mac is the sender's MAC as read off that Ethernet header - used
   instead of trusting the ARP payload's own sha field, same as a real
   stack would. Called from ethernet_poll() when ethertype ==
   ETH_TYPE_ARP - not normally called directly. Learns the sender's
   IP/MAC pairing into the cache regardless of opcode (the standard
   "learn from any ARP you see" behaviour, not specific to replies),
   and additionally sends a reply if this is a request for our own
   configured IPv4 address (see ipv4_get_address()). */
void arp_handle_frame(const uint8_t *payload, uint16_t len, const uint8_t src_mac[ETH_ADDR_LEN]);

/* Looks up ip in the resolution cache. On a hit, copies the MAC into
   mac_out and returns 1. On a miss, broadcasts an ARP request for ip
   as a side effect and returns 0 immediately - this function never
   blocks. A typical caller (see ipv4_send()) loops calling this
   alongside ethernet_poll()/task_yield() until it returns 1 or a
   timeout is reached, since a reply only makes it into the cache via
   arp_handle_frame() being reached through ethernet_poll(). */
int arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ADDR_LEN]);

#endif