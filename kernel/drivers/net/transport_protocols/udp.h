/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef UDP_H
#define UDP_H

#include <stdint.h>

#define UDP_HDR_LEN     8
#define UDP_MAX_PAYLOAD 1472 /* 1500-byte MTU minus 20 (IPv4) minus 8 (UDP) */

/* RFC 768 header, network byte order. packed so sizeof(struct
   udp_header) is exactly UDP_HDR_LEN. */
struct udp_header
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;   /* header + data, network byte order */
    uint16_t checksum; /* 0 means "not computed" - legal per RFC 768 over IPv4, unlike TCP */
} __attribute__((packed));

void udp_init(void);

/* Opens a socket bound to local_port so udp_socket_recv() can pick up
   datagrams addressed to it. Returns a handle >= 0 on success, or -1
   if local_port is already bound or the fixed socket table (see
   UDP_MAX_SOCKETS in udp.c) is full. */
int udp_socket_open(uint16_t local_port);

/* Frees the socket and its port so it can be reused. */
void udp_socket_close(int handle);

/* Non-blocking: if a datagram is waiting, copies up to buf_len bytes
   of it into buf and returns how many bytes were copied; returns 0 if
   nothing is waiting. Only ONE datagram is buffered per socket at a
   time - with no heap there's no real queue, so a second datagram
   arriving before the first is read overwrites it (see
   udp_handle_packet). src_ip_out/src_port_out (either may be NULL)
   receive who it came from. */
uint16_t udp_socket_recv(int handle, void *buf, uint16_t buf_len,
                          uint32_t *src_ip_out, uint16_t *src_port_out);

/* Connectionless send - doesn't require an open socket, matching how
   UDP actually works on the wire (a socket here only matters for
   receiving). Returns 1 on success, 0 if len is too large or the send
   failed further down the stack (see ipv4_send). */
int udp_send(uint32_t dest_ip, uint16_t src_port, uint16_t dest_port,
             const void *data, uint16_t len);

/* Handles one UDP datagram already stripped of its IPv4 header.
   Called from ipv4_handle_frame() when protocol == IPV4_PROTO_UDP -
   not normally called directly. */
void udp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip);

#endif