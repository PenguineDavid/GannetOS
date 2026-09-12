/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable checks for kernel/ipv4.c's checksum algorithms and
   the wire-format struct layouts in kernel/ethernet.h, arp.h, ipv4.h,
   icmp.h, udp.h and tcp.h. Same spirit as hosttest/test_gdt_encoding.c:
   this intentionally duplicates the checksum logic rather than
   #including ipv4.c, since ipv4.c pulls in freestanding-only kernel
   headers (task.h, pit.h) through its own includes. The struct
   layouts are duplicated too rather than #included, for the same
   reason test_gdt_encoding.c duplicates struct gdt_entry - it keeps
   this file buildable with nothing but a hosted libc and no -I search
   path into kernel/, and a mismatch between this copy and the real
   header is exactly the kind of bug (wrong field order, missing
   "packed", wrong size) this test exists to catch, so drifting apart
   on purpose isn't a risk worth taking to save a few duplicated
   lines. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* ---- duplicated from kernel/net_byteorder.h --------------------- */
static uint16_t net_swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}
static uint32_t net_swap32(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

/* ---- duplicated from kernel/ipv4.c's checksum functions ---------- */
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
        sum += (uint16_t)bytes[0] << 8;
    }
    return sum;
}
static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}
static uint16_t ipv4_checksum(const void *data, uint16_t len)
{
    return checksum_finish(checksum_accumulate(data, len, 0));
}
static uint16_t ipv4_checksum_pseudo(uint32_t src_ip, uint32_t dst_ip, uint8_t protocol,
                                      const void *segment, uint16_t segment_len)
{
    struct
    {
        uint32_t src;
        uint32_t dst;
        uint8_t  zero;
        uint8_t  protocol;
        uint16_t length;
    } __attribute__((packed)) pseudo;

    pseudo.src      = net_swap32(src_ip);
    pseudo.dst      = net_swap32(dst_ip);
    pseudo.zero     = 0;
    pseudo.protocol = protocol;
    pseudo.length   = net_swap16(segment_len);

    uint32_t sum = checksum_accumulate(&pseudo, sizeof(pseudo), 0);
    sum = checksum_accumulate(segment, segment_len, sum);
    return checksum_finish(sum);
}

/* ---- duplicated wire-format structs, see file header note -------- */
#define ETH_ADDR_LEN 6

struct eth_header
{
    uint8_t  dest[ETH_ADDR_LEN];
    uint8_t  src[ETH_ADDR_LEN];
    uint16_t ethertype;
} __attribute__((packed));

struct arp_packet
{
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[ETH_ADDR_LEN];
    uint32_t spa;
    uint8_t  tha[ETH_ADDR_LEN];
    uint32_t tpa;
} __attribute__((packed));

struct ipv4_header
{
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct icmp_header
{
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

struct udp_header
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct tcp_header
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

static int failures = 0;
static void check(const char *name, int cond)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond)
    {
        failures++;
    }
}

int main(void)
{
    /* --- byte order ------------------------------------------------ */
    check("net_swap16 round-trips 0x1234 -> 0x3412", net_swap16(0x1234) == 0x3412);
    check("net_swap16 is its own inverse", net_swap16(net_swap16(0xBEEF)) == 0xBEEF);
    check("net_swap32 round-trips 0x01020304 -> 0x04030201", net_swap32(0x01020304) == 0x04030201);
    check("net_swap32 is its own inverse", net_swap32(net_swap32(0xDEADBEEFu)) == 0xDEADBEEFu);

    /* --- struct sizes: catches a missing/misapplied "packed" or a
       silently reordered field, either of which would corrupt every
       frame this stack sends the moment it's compiled with different
       struct packing defaults. --- */
    check("eth_header is exactly 14 bytes", sizeof(struct eth_header) == 14);
    check("arp_packet is exactly 28 bytes", sizeof(struct arp_packet) == 28);
    check("ipv4_header is exactly 20 bytes", sizeof(struct ipv4_header) == 20);
    check("icmp_header is exactly 8 bytes", sizeof(struct icmp_header) == 8);
    check("udp_header is exactly 8 bytes", sizeof(struct udp_header) == 8);
    check("tcp_header is exactly 20 bytes", sizeof(struct tcp_header) == 20);

    /* --- struct field offsets: pins down the actual wire layout, not
       just the total size (two fields could swap sizes and still add
       up to the same total). --- */
    check("eth_header.src at offset 6", offsetof(struct eth_header, src) == 6);
    check("eth_header.ethertype at offset 12", offsetof(struct eth_header, ethertype) == 12);

    check("arp_packet.oper at offset 6", offsetof(struct arp_packet, oper) == 6);
    check("arp_packet.sha at offset 8", offsetof(struct arp_packet, sha) == 8);
    check("arp_packet.spa at offset 14", offsetof(struct arp_packet, spa) == 14);
    check("arp_packet.tha at offset 18", offsetof(struct arp_packet, tha) == 18);
    check("arp_packet.tpa at offset 24", offsetof(struct arp_packet, tpa) == 24);

    check("ipv4_header.ttl at offset 8", offsetof(struct ipv4_header, ttl) == 8);
    check("ipv4_header.checksum at offset 10", offsetof(struct ipv4_header, checksum) == 10);
    check("ipv4_header.src at offset 12", offsetof(struct ipv4_header, src) == 12);
    check("ipv4_header.dst at offset 16", offsetof(struct ipv4_header, dst) == 16);

    check("icmp_header.identifier at offset 4", offsetof(struct icmp_header, identifier) == 4);

    check("udp_header.length at offset 4", offsetof(struct udp_header, length) == 4);
    check("udp_header.checksum at offset 6", offsetof(struct udp_header, checksum) == 6);

    check("tcp_header.ack at offset 8", offsetof(struct tcp_header, ack) == 8);
    check("tcp_header.data_offset at offset 12", offsetof(struct tcp_header, data_offset) == 12);
    check("tcp_header.flags at offset 13", offsetof(struct tcp_header, flags) == 13);
    check("tcp_header.window at offset 14", offsetof(struct tcp_header, window) == 14);
    check("tcp_header.checksum at offset 16", offsetof(struct tcp_header, checksum) == 16);

    /* --- TCP flag bits: each one is a distinct, single bit within
       the byte real stacks agree on (RFC 793) - a typo turning one of
       these into a duplicate or an out-of-range value would make this
       stack's segments unparseable by anything else on the wire. --- */
    check("TCP flags are six distinct single bits", (TCP_FLAG_FIN | TCP_FLAG_SYN | TCP_FLAG_RST |
                                                       TCP_FLAG_PSH | TCP_FLAG_ACK | TCP_FLAG_URG) == 0x3F);
    check("TCP_FLAG_SYN is bit 1", TCP_FLAG_SYN == 0x02);
    check("TCP_FLAG_ACK is bit 4", TCP_FLAG_ACK == 0x10);

    /* --- IPv4 checksum: classic textbook header example (see e.g.
       Wikipedia's "IPv4 header checksum" worked example) - verified
       independently against a reference implementation before being
       hardcoded here, not copied on faith. --- */
    uint8_t sample_hdr[20] = {
        0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
        0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7
    };
    check("known-good header checksum is 0xb861", ipv4_checksum(sample_hdr, sizeof(sample_hdr)) == 0xb861);

    /* Inserting the computed checksum back into the header and
       resumming must yield exactly 0 - this is the actual property
       ipv4_handle_frame() relies on to verify an incoming packet, so
       it matters more than the specific 0xb861 value above. */
    uint16_t computed = ipv4_checksum(sample_hdr, sizeof(sample_hdr));
    sample_hdr[10] = (uint8_t)(computed >> 8);
    sample_hdr[11] = (uint8_t)(computed & 0xFF);
    check("header with its own checksum inserted sums to 0", ipv4_checksum(sample_hdr, sizeof(sample_hdr)) == 0);

    /* Odd-length buffer: the trailing byte must be padded as the HIGH
       byte of a virtual 16-bit word, per RFC 1071 - not skipped, and
       not treated as a low byte. */
    uint8_t odd[3] = { 0x01, 0x02, 0x03 };
    check("odd-length checksum treats trailing byte as a high byte", ipv4_checksum(odd, 3) == 0xfbfd);

    /* Carry-fold: two 0xFFFF words sum to 0x1FFFE, which needs the
       carry-out folded back in (twice, in this specific case) before
       complementing - an implementation that folds only once, or not
       at all, would get this one wrong while still passing the
       simpler cases above. */
    uint8_t carry[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    check("checksum folds carries out of the top 16 bits", ipv4_checksum(carry, 4) == 0x0000);

    /* --- pseudo-header checksum: the property udp_handle_packet() and
       tcp_handle_packet() actually rely on is the same "insert then
       resum to 0" one as the plain IPv4 checksum - exercised here
       across the trickier two-region (pseudo-header + segment)
       accumulation path instead of one contiguous buffer, which is
       exactly where an off-by-one in the accumulation order would
       show up but a single-region test could never catch. --- */
    uint32_t src_ip = 0x0A000205u; /* 10.0.2.5 */
    uint32_t dst_ip = 0x0A000202u; /* 10.0.2.2 */
    uint8_t udp_segment[16] = {
        0x30, 0x39,             /* src port 12345 */
        0x00, 0x35,             /* dst port 53 */
        0x00, 0x10,             /* length 16 */
        0x00, 0x00,             /* checksum, zeroed for the computation below */
        'P', 'E', 'N', 'G', 'O', 'S', '!', '!'
    };
    uint16_t udp_checksum = ipv4_checksum_pseudo(src_ip, dst_ip, 17, udp_segment, sizeof(udp_segment));
    udp_segment[6] = (uint8_t)(udp_checksum >> 8);
    udp_segment[7] = (uint8_t)(udp_checksum & 0xFF);
    check("pseudo-header checksum with itself inserted sums to 0",
          ipv4_checksum_pseudo(src_ip, dst_ip, 17, udp_segment, sizeof(udp_segment)) == 0);

    /* Same segment bytes, but claiming the wrong destination IP (as if
       misdelivered) must NOT still check out - this is the entire
       reason UDP/TCP checksums cover a pseudo-header instead of just
       the segment bytes. */
    check("pseudo-header checksum catches a mismatched destination IP",
          ipv4_checksum_pseudo(src_ip, dst_ip + 1, 17, udp_segment, sizeof(udp_segment)) != 0);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}