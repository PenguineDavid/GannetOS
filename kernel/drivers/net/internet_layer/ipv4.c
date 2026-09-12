/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/internet_layer/ipv4.h"
#include "kernel/drivers/net/link_layer/arp.h"
#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/drivers/net/internet_layer/icmp.h"
#include "kernel/drivers/net/transport_protocols/udp.h"
#include "kernel/drivers/net/transport_protocols/tcp.h"
#include "kernel/drivers/net/include/net_byteorder.h"
#include "kernel/proc/task.h"
#include "kernel/drivers/pit/pit.h"

static uint32_t local_ip = 0;
static uint16_t next_id = 0;
static uint32_t gateway_ip = 0;    /* 0 = unset - see ipv4_set_gateway()'s own comment */
static uint32_t subnet_mask = 0;

/* Give up resolving a destination MAC after this many PIT ticks.
   pit_init(100) in kernel.c means 100 ticks/second, so 300 ticks is
   3 real seconds - long enough for a QEMU usermode NIC's first ARP
   round trip, short enough that a genuinely unreachable host doesn't
   hang the caller forever. */
#define IPV4_ARP_TIMEOUT_TICKS 300

void ipv4_init(uint32_t addr)
{
    local_ip = addr;
}

uint32_t ipv4_get_address(void)
{
    return local_ip;
}

void ipv4_set_gateway(uint32_t gw, uint32_t mask)
{
    gateway_ip = gw;
    subnet_mask = mask;
}

/* Internal: the same one's-complement summation ipv4_checksum() and
   ipv4_checksum_pseudo() both need, but returning the raw (unfolded,
   uncomplemented) accumulator so a caller can feed it several
   discontiguous regions - one call per region, threading sum through
   each - before folding and complementing exactly once at the end.
   Folding and complementing each region separately and then adding
   the results together would NOT give the same answer: folding
   discards information (the carry) that a later region's bytes would
   otherwise have combined with. */
static uint32_t checksum_accumulate(const void *data, uint16_t len, uint32_t sum)
{
    const uint8_t *bytes = (const uint8_t *)data;
    while (len > 1)
    {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        len = (uint16_t)(len - 2);
    }
    if (len == 1)
    {
        /* Odd trailing byte - treated as the high byte of one more
           16-bit word, low byte implicitly zero, per RFC 1071. Only
           valid to do this on the LAST region fed to a given sum, and
           every current caller only ever has a trailing odd byte on
           its final region (a segment length can be odd; the fixed
           12-byte pseudo-header never is), so that's fine here. */
        sum += (uint16_t)bytes[0] << 8;
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    /* Fold any carries out of the top 16 bits back into the low 16 -
       may take more than one pass since folding itself can carry
       (e.g. summing several 0xFFFF words in a row). */
    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

uint16_t ipv4_checksum(const void *data, uint16_t len)
{
    return checksum_finish(checksum_accumulate(data, len, 0));
}

uint16_t ipv4_checksum_pseudo(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                               const void *segment, uint16_t segment_len)
{
    /* RFC 768 / RFC 793 pseudo-header - 12 bytes, never sent on the
       wire, only ever summed. packed so sizeof() is exactly 12 with
       no compiler padding between the two single-byte fields and
       their neighbours. */
    struct
    {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  protocol;
        uint16_t length;
    } __attribute__((packed)) pseudo;

    pseudo.src      = htonl(src_ip);
    pseudo.dst      = htonl(dst_ip);
    pseudo.zero     = 0;
    pseudo.protocol = protocol;
    pseudo.length   = htons(segment_len);

    uint32_t sum = checksum_accumulate(&pseudo, sizeof(pseudo), 0);
    sum = checksum_accumulate(segment, segment_len, sum);
    return checksum_finish(sum);
}

#define IPV4_MTU_PAYLOAD 1500 /* conventional Ethernet MTU */
static uint8_t tx_buf[IPV4_HDR_LEN + IPV4_MTU_PAYLOAD];

int ipv4_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t payload_len)
{
    if ((uint32_t)IPV4_HDR_LEN + payload_len > sizeof(tx_buf))
    {
        return 0;
    }

    /* Which MAC address to ARP for and hand the frame to - the
       destination's own, if it's on the local subnet, or the
       gateway's, if it isn't (a real router hop: the IP header below
       still names dest_ip as the ultimate destination, only the
       Ethernet frame's target changes). Falls back to always ARPing
       dest_ip directly - the original behaviour - if no gateway has
       been configured (gateway_ip == 0, see ipv4_set_gateway()), so
       this is a no-op change for anyone who never calls it. */
    uint32_t next_hop = dest_ip;
    if (gateway_ip != 0 && (dest_ip & subnet_mask) != (local_ip & subnet_mask))
    {
        next_hop = gateway_ip;
    }

    uint8_t dest_mac[ETH_ADDR_LEN];
    if (!arp_resolve(next_hop, dest_mac))
    {
        uint32_t start = pit_ticks();
        while (!arp_resolve(next_hop, dest_mac))
        {
            /* arp_resolve() re-sends the request on every miss, which
               is more retries than strictly necessary, but harmless
               on a local segment and simpler than adding a separate
               "have I already asked" flag here. */
            ethernet_poll();
            task_yield();
            if (pit_ticks() - start > IPV4_ARP_TIMEOUT_TICKS)
            {
                return 0;
            }
        }
    }

    struct ipv4_header *hdr = (struct ipv4_header *)tx_buf;
    hdr->version_ihl  = (4 << 4) | 5;
    hdr->tos          = 0;
    hdr->total_length = htons((uint16_t)(IPV4_HDR_LEN + payload_len));
    hdr->id           = htons(next_id++);
    hdr->flags_frag   = 0;
    hdr->ttl          = IPV4_DEFAULT_TTL;
    hdr->protocol     = protocol;
    hdr->checksum     = 0;
    hdr->src          = htonl(local_ip);
    hdr->dst          = htonl(dest_ip);
    hdr->checksum     = htons(ipv4_checksum(hdr, IPV4_HDR_LEN));

    uint8_t *body = tx_buf + IPV4_HDR_LEN;
    for (uint16_t i = 0; i < payload_len; i++)
    {
        body[i] = ((const uint8_t *)payload)[i];
    }

    return ethernet_send(dest_mac, ETH_TYPE_IPV4, tx_buf, (uint16_t)(IPV4_HDR_LEN + payload_len));
}

void ipv4_handle_frame(const uint8_t *frame_payload, uint16_t len, const uint8_t src_mac[6])
{
    (void)src_mac; /* not needed at this layer - arp.c already learned this pairing straight off the Ethernet header */

    if (len < IPV4_HDR_LEN)
    {
        return;
    }

    const struct ipv4_header *hdr = (const struct ipv4_header *)frame_payload;
    uint8_t version = (uint8_t)(hdr->version_ihl >> 4);
    uint8_t ihl_words = (uint8_t)(hdr->version_ihl & 0x0F);
    if (version != 4 || ihl_words != 5)
    {
        return; /* IPv6, or an options header this stack doesn't parse */
    }

    if (ipv4_checksum(hdr, IPV4_HDR_LEN) != 0)
    {
        return; /* corrupt header - see ipv4_checksum's own note on why a valid header always sums to 0 */
    }

    if (ntohl(hdr->dst) != local_ip)
    {
        return; /* not addressed to us - no routing/forwarding exists */
    }

    uint16_t total_len = ntohs(hdr->total_length);
    if (total_len < IPV4_HDR_LEN || total_len > len)
    {
        return; /* claims more data than the frame actually carried */
    }

    const uint8_t *body = frame_payload + IPV4_HDR_LEN;
    uint16_t body_len = (uint16_t)(total_len - IPV4_HDR_LEN);
    uint32_t src_ip = ntohl(hdr->src);

    switch (hdr->protocol)
    {
        case IPV4_PROTO_ICMP:
            icmp_handle_packet(body, body_len, src_ip);
            break;
        case IPV4_PROTO_UDP:
            udp_handle_packet(body, body_len, src_ip);
            break;
        case IPV4_PROTO_TCP:
            tcp_handle_packet(body, body_len, src_ip);
            break;
        default:
            break; /* nothing above this layer handles anything else */
    }
}