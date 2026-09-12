/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef IPV4_H
#define IPV4_H

#include <stdint.h>

#define IPV4_PROTO_ICMP 1
#define IPV4_PROTO_TCP  6
#define IPV4_PROTO_UDP  17

/* Builds a uint32_t IPv4 address (host byte order) from its four
   dotted octets, most significant octet first - IPV4_ADDR(192,168,1,1)
   reads in the same order it would be typed on a command line. */
#define IPV4_ADDR(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Fixed 20-byte header, no options - this stack never sends options
   and drops any incoming packet whose IHL claims more than 5 (see
   ipv4_handle_frame). Network byte order throughout except where
   noted; packed for the same reason eth_header is. */
struct ipv4_header
{
    uint8_t  version_ihl;  /* high nibble version (always 4), low nibble IHL in 32-bit words (always 5 here) */
    uint8_t  tos;
    uint16_t total_length; /* header + payload, network byte order */
    uint16_t id;
    uint16_t flags_frag;   /* flags + fragment offset - this stack never fragments or reassembles, always sent as 0 (DF/MF clear, offset 0) */
    uint8_t  ttl;
    uint8_t  protocol;     /* one of IPV4_PROTO_* */
    uint16_t checksum;     /* header checksum only, network byte order */
    uint32_t src;          /* network byte order */
    uint32_t dst;          /* network byte order */
} __attribute__((packed));

#define IPV4_HDR_LEN     20
#define IPV4_DEFAULT_TTL 64
#define IPV4_DEFAULT_GATEWAY IPV4_ADDR(10, 0, 2, 2)
#define IPV4_DEFAULT_SUBNET_MASK IPV4_ADDR(255, 255, 255, 0)

/* Sets this machine's own IPv4 address (host byte order, e.g. built
   with IPV4_ADDR()). Must be called before anything sends or replies
   to traffic - arp_handle_frame() checks incoming ARP requests
   against whatever this was last set to. No DHCP client exists yet,
   so this is always a static address chosen by whoever calls
   net_init(). */
void ipv4_init(uint32_t local_ip);

/* Returns whatever ipv4_init() was last called with. */
uint32_t ipv4_get_address(void);

/* Sets the default gateway and subnet mask used for non-local routes. */
void ipv4_set_gateway(uint32_t gateway_ip, uint32_t subnet_mask);

/* Standard Internet checksum (RFC 1071): one's-complement sum of the
   data as big-endian 16-bit words, carries folded back in, then
   complemented. Works identically whether it's being used to COMPUTE
   a checksum (checksum field zeroed first, result stored) or VERIFY
   one (checksum field left as received - summing a correctly
   checksummed region together with its own checksum field always
   yields 0xFFFF, i.e. this function returns 0 on it). len may be odd;
   a trailing single byte is treated as padded with a zero low byte,
   per the RFC. Used directly by ipv4.c (header only) and icmp.c
   (header + payload) - UDP and TCP use ipv4_checksum_pseudo() instead,
   below. */
uint16_t ipv4_checksum(const void *data, uint16_t len);

/* Same RFC 1071 checksum, but over a virtual "pseudo-header" (source
   IP, destination IP, a zero byte, the protocol number, and the
   segment length) immediately followed by the segment itself - this
   is what UDP and TCP checksums actually cover, per RFC 768 / RFC 793,
   even though none of those pseudo-header bytes are ever transmitted
   on the wire. It exists so a corrupted or misdelivered segment
   (wrong destination IP, wrong protocol) fails its checksum even if
   every byte of the segment itself is intact. Computing this as two
   separate accumulation passes (pseudo-header, then segment) rather
   than materializing them into one contiguous buffer first gives the
   identical result, since one's-complement addition doesn't care
   where the "seam" between two summed regions falls. */
uint16_t ipv4_checksum_pseudo(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                               const void *segment, uint16_t segment_len);

/* Wraps payload in an IPv4 header addressed to dest_ip and sends it
   as protocol. Resolves the next hop's MAC via ARP first - the
   destination's own MAC if dest_ip is on the local subnet (per the
   mask set by ipv4_set_gateway()), otherwise the gateway's MAC, with
   dest_ip still named as the ultimate destination in the IP header
   either way. If no gateway has been configured (gateway_ip == 0),
   this always ARPs dest_ip directly regardless of subnet. If the
   chosen next hop isn't already cached, this blocks (via
   task_yield(), not a hardware stall) for up to a few seconds pumping
   ethernet_poll() while waiting for a reply, and gives up (returning
   0) if it never arrives. Returns 1 on success. */
int ipv4_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t payload_len);

/* Handles one IPv4 datagram already stripped of its Ethernet header.
   Called from ethernet_poll() when ethertype == ETH_TYPE_IPV4 - not
   normally called directly. Validates version/IHL/checksum, drops
   anything malformed or addressed to a different IP than
   ipv4_get_address() (no promiscuous or routing behaviour), and
   dispatches the payload to icmp_handle_packet(), udp_handle_packet()
   or tcp_handle_packet() depending on protocol; anything else is
   silently dropped. */
void ipv4_handle_frame(const uint8_t *frame_payload, uint16_t len, const uint8_t src_mac[6]);

#endif