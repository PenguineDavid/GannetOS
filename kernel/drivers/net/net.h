/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Brings up the whole networking stack in order: Ethernet layer, ARP
   cache, IPv4 with the given static address, a default gateway/subnet
   pointed at IPV4_DEFAULT_GATEWAY/IPV4_DEFAULT_SUBNET_MASK (see ipv4.h
   - without this, anything outside the local /24 is unreachable, since
   there'd be nothing to ARP for it), UDP, TCP, and finally DNS pointed
   at DNS_DEFAULT_RESOLVER (see dns.h). All three defaults match QEMU's
   usermode NIC subnet convention. Build the local address with
   IPV4_ADDR() from ipv4.h, e.g. IPV4_ADDR(10,0,2,15) for that same
   convention - see kernel.c's `-nic user,model=rtl8139`. Call this once
   from kernel_main, after rtl8139_init() has already brought the card
   up. There is no DHCP client yet, so the address is always whatever
   the caller hardcodes here. If the network isn't QEMU's default
   usermode setup, call ipv4_set_gateway()/dns_set_resolver() afterwards
   to point at the right addresses. */
void net_init(uint32_t local_ip);

/* Pumps the whole stack once: polls the driver for a waiting frame and
   runs it all the way up through Ethernet, ARP, IPv4, ICMP, UDP and
   TCP if one arrived, then drives TCP's retransmission timers
   (tcp_poll()) regardless of whether anything arrived - a lost
   segment only ever gets resent because something keeps calling this.
   Call this every kernel_main loop iteration (same spirit as the
   existing shell_has_pending_command()/terminal_update() calls
   there). */
void net_poll(void);

#endif