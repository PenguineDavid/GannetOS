/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/drivers/net/link_layer/arp.h"
#include "kernel/drivers/net/internet_layer/ipv4.h"
#include "kernel/drivers/rtl8139/rtl8139.h"
#include "kernel/drivers/net/include/net_byteorder.h"

const uint8_t eth_broadcast_mac[ETH_ADDR_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* One shared scratch buffer per direction. Safe because this module
   is only ever called from ordinary task context, never re-entered
   from an ISR - rtl8139's own IRQ handler just acks the card and
   sends EOI (see rtl8139.c), it never touches these buffers. That's
   also why rtl8139's tx_buffers[] needed 4 separate slots and these
   don't: the card DMAs concurrently out of each of ITS slots, but
   nothing here runs concurrently with itself. */
static uint8_t tx_scratch[RTL8139_MAX_FRAME];
static uint8_t rx_scratch[RTL8139_MAX_FRAME];

void ethernet_init(void)
{
    /* Nothing to do yet - see the header comment. */
}

void ethernet_get_mac(uint8_t mac_out[ETH_ADDR_LEN])
{
    rtl8139_get_mac(mac_out);
}

int ethernet_send(const uint8_t dest_mac[ETH_ADDR_LEN], uint16_t ethertype,
                   const void *payload, uint16_t payload_len)
{
    if ((uint32_t)payload_len + ETH_HDR_LEN > RTL8139_MAX_FRAME)
    {
        return 0;
    }

    struct eth_header *hdr = (struct eth_header *)tx_scratch;
    for (int i = 0; i < ETH_ADDR_LEN; i++)
    {
        hdr->dest[i] = dest_mac[i];
    }
    rtl8139_get_mac(hdr->src);
    hdr->ethertype = htons(ethertype);

    uint8_t *body = tx_scratch + ETH_HDR_LEN;
    for (uint16_t i = 0; i < payload_len; i++)
    {
        body[i] = ((const uint8_t *)payload)[i];
    }

    return rtl8139_send(tx_scratch, (uint16_t)(ETH_HDR_LEN + payload_len));
}

void ethernet_poll(void)
{
    int len = rtl8139_receive(rx_scratch);
    if (len < ETH_HDR_LEN)
    {
        return; /* nothing waiting, or a runt frame too short to even hold a header */
    }

    struct eth_header *hdr = (struct eth_header *)rx_scratch;
    uint16_t ethertype = ntohs(hdr->ethertype);
    const uint8_t *body = rx_scratch + ETH_HDR_LEN;
    uint16_t body_len = (uint16_t)(len - ETH_HDR_LEN);

    switch (ethertype)
    {
        case ETH_TYPE_ARP:
            arp_handle_frame(body, body_len, hdr->src);
            break;
        case ETH_TYPE_IPV4:
            ipv4_handle_frame(body, body_len, hdr->src);
            break;
        default:
            /* Unknown ethertype (IPv6, etc.) - dropped, not an error.
               Nothing above this layer wants it yet. */
            break;
    }
}