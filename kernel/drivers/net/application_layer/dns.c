/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/application_layer/dns.h"
#include "kernel/drivers/net/transport_protocols/udp.h"
#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/drivers/net/include/net_byteorder.h"
#include "kernel/proc/task.h"
#include "kernel/drivers/pit/pit.h"

#define DNS_HDR_LEN        12
#define DNS_PORT           53
#define DNS_MAX_PACKET     512 /* classic UDP DNS message size limit, no EDNS0 support */
#define DNS_TIMEOUT_TICKS  300 /* 3s at pit_init(100) - see kernel.c */

/* RFC 1035 header, network byte order. packed so sizeof() is exactly
   DNS_HDR_LEN. */
struct dns_header
{
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed));

/* The fixed part of a resource record that follows its NAME field -
   TYPE, CLASS, TTL, RDLENGTH - with RDATA (rdlength bytes) right
   after. packed so sizeof() is exactly 10. */
struct dns_rr_fixed
{
    uint16_t type;
    uint16_t rr_class;
    uint32_t ttl;
    uint16_t rdlength;
} __attribute__((packed));

static uint32_t resolver_ip;

void dns_init(uint32_t r)
{
    resolver_ip = r;
}

void dns_set_resolver(uint32_t r)
{
    resolver_ip = r;
}

/* Parses a dotted-decimal IPv4 literal like "10.0.2.2" - exactly four
   decimal octets 0-255 separated by dots, nothing else (no
   whitespace, no leading '+', no IPv6). Returns 1 and fills *ip_out
   on success, 0 if s isn't a literal at all (the normal case - most
   hostnames aren't, and this just means "go do a real lookup"). */
static int parse_ipv4_literal(const char *s, uint32_t *ip_out)
{
    uint32_t octets[4];

    for (int oct = 0; oct < 4; oct++)
    {
        if (*s < '0' || *s > '9')
        {
            return 0;
        }

        int val = 0, digits = 0;
        while (*s >= '0' && *s <= '9')
        {
            val = val * 10 + (*s - '0');
            s++;
            digits++;
            if (digits > 3 || val > 255)
            {
                return 0;
            }
        }
        octets[oct] = (uint32_t)val;

        if (oct < 3)
        {
            if (*s != '.')
            {
                return 0;
            }
            s++;
        }
    }
    if (*s != '\0')
    {
        return 0;
    }

    *ip_out = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 1;
}

/* Writes hostname into out as a sequence of length-prefixed labels
   terminated by a zero-length root label (e.g. "google.com" becomes
   6google3com0), the QNAME wire format every DNS query section needs.
   Returns the number of bytes written, or 0 if hostname is empty, has
   an empty or over-63-byte label (both illegal per RFC 1035), or
   wouldn't fit in out_max bytes. */
static uint16_t encode_qname(const char *hostname, uint8_t *out, uint16_t out_max)
{
    uint16_t pos = 0;
    const char *label_start = hostname;

    for (;;)
    {
        const char *p = label_start;
        while (*p && *p != '.')
        {
            p++;
        }

        uint32_t label_len = (uint32_t)(p - label_start);
        if (label_len == 0 || label_len > 63)
        {
            return 0;
        }
        if ((uint32_t)pos + label_len + 1 >= out_max)
        {
            return 0; /* +1 for this label's own length byte, leaving room for at least the root label after it */
        }

        out[pos++] = (uint8_t)label_len;
        for (uint32_t i = 0; i < label_len; i++)
        {
            out[pos++] = (uint8_t)label_start[i];
        }

        if (*p == '\0')
        {
            break;
        }
        label_start = p + 1;
    }

    if ((uint32_t)pos + 1 > out_max)
    {
        return 0;
    }
    out[pos++] = 0; /* root label */
    return pos;
}

/* Returns how many bytes the NAME field starting at offset occupies,
   without needing to decode it - a run of length-prefixed labels
   ending in a zero byte, OR (per RFC 1035 message compression) a
   2-byte pointer that can appear instead of a label at any point.
   Only ever used to skip past a NAME whose contents this code doesn't
   need (the answer records' own names, trusted to correspond to the
   question asked rather than independently verified against it - a
   simplification most minimal DNS clients make, and harmless against
   the trusted local resolver this stack talks to). */
static uint16_t skip_name(const uint8_t *packet, uint16_t packet_len, uint16_t offset)
{
    while (offset < packet_len)
    {
        uint8_t len_byte = packet[offset];
        if ((len_byte & 0xC0) == 0xC0)
        {
            return (uint16_t)(offset + 2); /* compression pointer - always exactly 2 bytes here, regardless of what it points to */
        }
        if (len_byte == 0)
        {
            return (uint16_t)(offset + 1); /* root label - end of name */
        }
        offset = (uint16_t)(offset + 1 + len_byte);
    }
    return packet_len; /* ran off the end - malformed; the caller's own length checks reject whatever this points past */
}

static uint16_t pick_ephemeral_port(void)
{
    static uint16_t next_port = 50000;
    uint16_t port = next_port++;
    if (next_port == 0)
    {
        next_port = 50000; /* wrapped past 65535 */
    }
    return port;
}

int dns_resolve(const char *hostname, uint32_t *ip_out)
{
    if (parse_ipv4_literal(hostname, ip_out))
    {
        return 1;
    }

    uint16_t local_port = pick_ephemeral_port();
    int sock = udp_socket_open(local_port);
    if (sock < 0)
    {
        return 0;
    }

    uint8_t query[DNS_MAX_PACKET];
    struct dns_header *qh = (struct dns_header *)query;
    uint16_t txid = (uint16_t)(pit_ticks() & 0xFFFF);
    qh->id      = htons(txid);
    qh->flags   = htons(0x0100); /* standard query, recursion desired, everything else 0 */
    qh->qdcount = htons(1);
    qh->ancount = 0;
    qh->nscount = 0;
    qh->arcount = 0;

    uint16_t pos = DNS_HDR_LEN;
    uint16_t qname_len = encode_qname(hostname, query + pos, (uint16_t)(sizeof(query) - pos - 4));
    if (qname_len == 0)
    {
        udp_socket_close(sock);
        return 0; /* hostname malformed or too long to fit a query */
    }
    pos = (uint16_t)(pos + qname_len);
    query[pos++] = 0x00;
    query[pos++] = 0x01; /* QTYPE  = A */
    query[pos++] = 0x00;
    query[pos++] = 0x01; /* QCLASS = IN */

    if (!udp_send(resolver_ip, local_port, DNS_PORT, query, pos))
    {
        udp_socket_close(sock);
        return 0;
    }

    uint8_t response[DNS_MAX_PACKET];
    uint16_t resp_len = 0;
    uint32_t start = pit_ticks();
    while (resp_len == 0)
    {
        ethernet_poll();
        uint32_t from_ip;
        uint16_t from_port;
        resp_len = udp_socket_recv(sock, response, sizeof(response), &from_ip, &from_port);
        if (resp_len == 0)
        {
            task_yield();
            if (pit_ticks() - start > DNS_TIMEOUT_TICKS)
            {
                udp_socket_close(sock);
                return 0;
            }
        }
    }
    udp_socket_close(sock);

    if (resp_len < DNS_HDR_LEN)
    {
        return 0;
    }
    const struct dns_header *rh = (const struct dns_header *)response;
    if (ntohs(rh->id) != txid)
    {
        return 0; /* stray or duplicate packet - not our query */
    }
    uint16_t flags = ntohs(rh->flags);
    if (!(flags & 0x8000))
    {
        return 0; /* QR bit clear - not actually a response */
    }
    if ((flags & 0x000F) != 0)
    {
        return 0; /* RCODE nonzero - NXDOMAIN or another server-side error */
    }

    uint16_t ancount = ntohs(rh->ancount);
    if (ancount == 0)
    {
        return 0;
    }

    /* Skip the question section: one NAME, then QTYPE(2)+QCLASS(2). */
    uint16_t offset = skip_name(response, resp_len, DNS_HDR_LEN);
    offset = (uint16_t)(offset + 4);

    for (uint16_t i = 0; i < ancount && offset < resp_len; i++)
    {
        offset = skip_name(response, resp_len, offset);
        if ((uint32_t)offset + sizeof(struct dns_rr_fixed) > resp_len)
        {
            break; /* not enough left for TYPE/CLASS/TTL/RDLENGTH */
        }

        const struct dns_rr_fixed *rr = (const struct dns_rr_fixed *)(response + offset);
        uint16_t rtype  = ntohs(rr->type);
        uint16_t rclass = ntohs(rr->rr_class);
        uint16_t rdlen  = ntohs(rr->rdlength);
        uint16_t rdata_offset = (uint16_t)(offset + sizeof(struct dns_rr_fixed));

        if ((uint32_t)rdata_offset + rdlen > resp_len)
        {
            break; /* RDLENGTH claims more than the packet actually has */
        }

        if (rtype == 1 && rclass == 1 && rdlen == 4) /* TYPE A, CLASS IN */
        {
            *ip_out = ((uint32_t)response[rdata_offset]     << 24) |
                      ((uint32_t)response[rdata_offset + 1] << 16) |
                      ((uint32_t)response[rdata_offset + 2] << 8)  |
                      (uint32_t)response[rdata_offset + 3];
            return 1;
        }

        offset = (uint16_t)(rdata_offset + rdlen); /* not an A record (CNAME, AAAA, etc.) - skip its RDATA and keep looking */
    }

    return 0; /* no A record among the answers */
}