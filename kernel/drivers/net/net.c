/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "kernel/drivers/net/net.h"
#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/drivers/net/link_layer/arp.h"
#include "kernel/drivers/net/internet_layer/ipv4.h"
#include "kernel/drivers/net/transport_protocols/udp.h"
#include "kernel/drivers/net/transport_protocols/tcp.h"
#include "kernel/drivers/net/application_layer/dns.h"

void net_init(uint32_t local_ip)
{
    ethernet_init();
    arp_init();
    ipv4_init(local_ip);
    ipv4_set_gateway(IPV4_DEFAULT_GATEWAY, IPV4_DEFAULT_SUBNET_MASK);
    udp_init();
    tcp_init();
    dns_init(DNS_DEFAULT_RESOLVER);
}

void net_poll(void)
{
    ethernet_poll();
    tcp_poll();
}