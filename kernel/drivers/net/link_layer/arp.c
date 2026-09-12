/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/link_layer/arp.h"
#include "kernel/drivers/net/internet_layer/ipv4.h"
#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/drivers/net/include/net_byteorder.h"
#include "kernel/drivers/pit/pit.h"

#define ARP_CACHE_SIZE 16

struct arp_entry
{
    uint32_t ip;   /* host byte order, 0 means unused slot */
    uint8_t  mac[ETH_ADDR_LEN];
    uint32_t learned_tick;
};

/* Fixed-size cache, no heap - same convention as task.c's static task
   pool (see task.h's own note on why GannetOS does this everywhere).
   Eviction below is oldest-wins by learned_tick rather than a real
   LRU list, which is the cheapest thing that's still better than
   "stop learning once full" for a cache this small serving a single
   machine's local-subnet traffic. */
static struct arp_entry cache[ARP_CACHE_SIZE];

void arp_init(void)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
    {
        cache[i].ip = 0;
        cache[i].learned_tick = 0;
    }
}

static void cache_learn(uint32_t ip, const uint8_t mac[ETH_ADDR_LEN])
{
    if (ip == 0)
    {
        return; /* 0.0.0.0 shows up in ARP probes - never worth caching */
    }

    int slot = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (cache[i].ip == ip)
        {
            slot = i; /* refresh an existing entry */
            break;
        }
    }
    if (slot == -1)
    {
        for (int i = 0; i < ARP_CACHE_SIZE; i++)
        {
            if (cache[i].ip == 0)
            {
                slot = i; /* first free slot */
                break;
            }
        }
    }
    if (slot == -1)
    {
        /* Cache full and this is a genuinely new IP - evict whichever
           entry was learned longest ago. */
        uint32_t oldest_tick = cache[0].learned_tick;
        slot = 0;
        for (int i = 1; i < ARP_CACHE_SIZE; i++)
        {
            if (cache[i].learned_tick < oldest_tick)
            {
                oldest_tick = cache[i].learned_tick;
                slot = i;
            }
        }
    }

    cache[slot].ip = ip;
    for (int i = 0; i < ETH_ADDR_LEN; i++)
    {
        cache[slot].mac[i] = mac[i];
    }
    cache[slot].learned_tick = pit_ticks();
}

static int cache_lookup(uint32_t ip, uint8_t mac_out[ETH_ADDR_LEN])
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (cache[i].ip == ip)
        {
            for (int j = 0; j < ETH_ADDR_LEN; j++)
            {
                mac_out[j] = cache[i].mac[j];
            }
            return 1;
        }
    }
    return 0;
}

static void send_arp(uint16_t oper, const uint8_t tha[ETH_ADDR_LEN], uint32_t tpa,
                      const uint8_t dest_mac[ETH_ADDR_LEN])
{
    struct arp_packet pkt;
    pkt.htype = htons(ARP_HTYPE_ETHERNET);
    pkt.ptype = htons(ARP_PTYPE_IPV4);
    pkt.hlen  = ARP_HLEN_ETHERNET;
    pkt.plen  = ARP_PLEN_IPV4;
    pkt.oper  = htons(oper);
    ethernet_get_mac(pkt.sha);
    pkt.spa = htonl(ipv4_get_address());
    for (int i = 0; i < ETH_ADDR_LEN; i++)
    {
        pkt.tha[i] = tha[i];
    }
    pkt.tpa = htonl(tpa);

    ethernet_send(dest_mac, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

void arp_handle_frame(const uint8_t *payload, uint16_t len, const uint8_t src_mac[ETH_ADDR_LEN])
{
    if (len < sizeof(struct arp_packet))
    {
        return; /* truncated - can't be a real ARP packet */
    }

    const struct arp_packet *pkt = (const struct arp_packet *)payload;
    if (ntohs(pkt->htype) != ARP_HTYPE_ETHERNET || ntohs(pkt->ptype) != ARP_PTYPE_IPV4 ||
        pkt->hlen != ARP_HLEN_ETHERNET || pkt->plen != ARP_PLEN_IPV4)
    {
        return; /* not an Ethernet/IPv4 ARP packet - nothing else is supported */
    }

    uint32_t sender_ip = ntohl(pkt->spa);
    cache_learn(sender_ip, src_mac);

    uint16_t oper = ntohs(pkt->oper);
    if (oper == ARP_OP_REQUEST && ntohl(pkt->tpa) == ipv4_get_address())
    {
        send_arp(ARP_OP_REPLY, pkt->sha, sender_ip, src_mac);
    }
}

int arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ADDR_LEN])
{
    if (cache_lookup(ip, mac_out))
    {
        return 1;
    }

    /* Miss - fire off a request and let the caller keep polling. The
       all-zero tha is meaningless for a request but every real ARP
       request on the wire zeroes it anyway rather than leaving it
       uninitialized, so this matches that convention. */
    static const uint8_t zero_mac[ETH_ADDR_LEN] = { 0, 0, 0, 0, 0, 0 };
    send_arp(ARP_OP_REQUEST, zero_mac, ip, eth_broadcast_mac);
    return 0;
}