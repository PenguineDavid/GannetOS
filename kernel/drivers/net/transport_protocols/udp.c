/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/transport_protocols/udp.h"
#include "kernel/drivers/net/internet_layer/ipv4.h"
#include "kernel/drivers/net/include/net_byteorder.h"

#define UDP_MAX_SOCKETS 8

struct udp_socket
{
    int      bound;      /* 0 = free slot */
    uint16_t local_port;
    uint8_t  rx_buf[UDP_MAX_PAYLOAD];
    uint16_t rx_len;      /* 0 = no datagram waiting */
    uint32_t rx_src_ip;
    uint16_t rx_src_port;
};

/* Fixed-size table, no heap - same convention as arp.c's cache and
   task.c's task pool. */
static struct udp_socket sockets[UDP_MAX_SOCKETS];

void udp_init(void)
{
    for (int i = 0; i < UDP_MAX_SOCKETS; i++)
    {
        sockets[i].bound = 0;
    }
}

int udp_socket_open(uint16_t local_port)
{
    int free_slot = -1;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++)
    {
        if (sockets[i].bound && sockets[i].local_port == local_port)
        {
            return -1; /* already bound */
        }
        if (!sockets[i].bound && free_slot == -1)
        {
            free_slot = i;
        }
    }
    if (free_slot == -1)
    {
        return -1;
    }

    sockets[free_slot].bound = 1;
    sockets[free_slot].local_port = local_port;
    sockets[free_slot].rx_len = 0;
    return free_slot;
}

void udp_socket_close(int handle)
{
    if (handle < 0 || handle >= UDP_MAX_SOCKETS)
    {
        return;
    }
    sockets[handle].bound = 0;
}

uint16_t udp_socket_recv(int handle, void *buf, uint16_t buf_len,
                          uint32_t *src_ip_out, uint16_t *src_port_out)
{
    if (handle < 0 || handle >= UDP_MAX_SOCKETS || !sockets[handle].bound)
    {
        return 0;
    }

    struct udp_socket *sock = &sockets[handle];
    if (sock->rx_len == 0)
    {
        return 0;
    }

    uint16_t copy_len = sock->rx_len < buf_len ? sock->rx_len : buf_len;
    for (uint16_t i = 0; i < copy_len; i++)
    {
        ((uint8_t *)buf)[i] = sock->rx_buf[i];
    }
    if (src_ip_out)
    {
        *src_ip_out = sock->rx_src_ip;
    }
    if (src_port_out)
    {
        *src_port_out = sock->rx_src_port;
    }

    sock->rx_len = 0; /* consumed - slot is free for the next datagram */
    return copy_len;
}

static uint8_t tx_buf[UDP_HDR_LEN + UDP_MAX_PAYLOAD];

int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
             const void *data, uint16_t len)
{
    if (len > UDP_MAX_PAYLOAD)
    {
        return 0;
    }

    struct udp_header *hdr = (struct udp_header *)tx_buf;
    hdr->src_port = htons(src_port);
    hdr->dst_port = htons(dest_port);
    hdr->length   = htons((uint16_t)(UDP_HDR_LEN + len));
    hdr->checksum = 0;

    uint8_t *body = tx_buf + UDP_HDR_LEN;
    for (uint16_t i = 0; i < len; i++)
    {
        body[i] = ((const uint8_t *)data)[i];
    }

    uint16_t total = (uint16_t)(UDP_HDR_LEN + len);
    uint16_t checksum = ipv4_checksum_pseudo(ipv4_get_address(), dest_ip, IPV4_PROTO_UDP, tx_buf, total);
    /* RFC 768: a computed checksum of exactly 0 is sent as all-ones,
       since 0 on the wire is reserved to mean "no checksum was
       computed" - the one case where 0 and 0xFFFF are NOT
       interchangeable even though they represent the same value in
       one's-complement arithmetic. */
    hdr->checksum = htons(checksum == 0 ? 0xFFFF : checksum);

    return ipv4_send(dest_ip, IPV4_PROTO_UDP, tx_buf, total);
}

void udp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip)
{
    if (len < UDP_HDR_LEN)
    {
        return;
    }

    const struct udp_header *hdr = (const struct udp_header *)packet;
    uint16_t udp_len = ntohs(hdr->length);
    if (udp_len < UDP_HDR_LEN || udp_len > len)
    {
        return; /* claims more than the datagram actually carried */
    }

    if (hdr->checksum != 0)
    {
        /* A sender is allowed to skip the checksum entirely (leaving
           it 0) - only verify when one was actually provided. */
        if (ipv4_checksum_pseudo(src_ip, ipv4_get_address(), IPV4_PROTO_UDP, packet, udp_len) != 0)
        {
            return;
        }
    }

    uint16_t dst_port = ntohs(hdr->dst_port);
    uint16_t data_len = (uint16_t)(udp_len - UDP_HDR_LEN);
    const uint8_t *data = packet + UDP_HDR_LEN;

    for (int i = 0; i < UDP_MAX_SOCKETS; i++)
    {
        if (sockets[i].bound && sockets[i].local_port == dst_port)
        {
            uint16_t copy_len = data_len < UDP_MAX_PAYLOAD ? data_len : UDP_MAX_PAYLOAD;
            for (uint16_t j = 0; j < copy_len; j++)
            {
                sockets[i].rx_buf[j] = data[j];
            }
            sockets[i].rx_len = copy_len;
            sockets[i].rx_src_ip = src_ip;
            sockets[i].rx_src_port = ntohs(hdr->src_port);
            return; /* only one socket can ever be bound to a given port - see udp_socket_open */
        }
    }
    /* No socket listening on this port - dropped. A real stack would
       answer with an ICMP port-unreachable message; that's still on
       the TODO list alongside the rest of ICMP's error types. */
}