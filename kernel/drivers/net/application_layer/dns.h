/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef DNS_H
#define DNS_H

#include <stdint.h>
#include "kernel/drivers/net/internet_layer/ipv4.h"

/* QEMU's usermode ("SLIRP") networking provides a built-in DNS proxy
   at the third address of its default 10.0.2.0/24 subnet - see
   net_init()'s IPV4_ADDR(10,0,2,15) for the client address this
   convention pairs with. net_init() sets this as the default resolver
   automatically; call dns_set_resolver() afterwards to point at a
   different one (a real router's DNS server, a public resolver
   reachable through one, etc). */
#define DNS_DEFAULT_RESOLVER IPV4_ADDR(10, 0, 2, 3)

void dns_init(uint32_t resolver_ip);
void dns_set_resolver(uint32_t resolver_ip);

/* Resolves hostname to an IPv4 address (host byte order) in *ip_out.
   If hostname is already a dotted-decimal literal (e.g. "8.8.8.8") it
   is parsed directly with no network traffic at all; otherwise this
   sends one recursive A-record query to the configured resolver and
   blocks (via task_yield(), pumping ethernet_poll() while it waits)
   for up to a few seconds. Returns 1 on success, 0 on a malformed
   hostname, a timeout, or an NXDOMAIN/error response. Does not cache
   - every call is a fresh lookup (or a fresh literal-parse). */
int dns_resolve(const char *hostname, uint32_t *ip_out);

#endif