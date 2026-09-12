/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef TCP_H
#define TCP_H

#include <stdint.h>

#define TCP_HDR_LEN 20 /* no options - see the struct tcp_conn note in tcp.c on what that costs */

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

/* RFC 793 header, network byte order, no options (data_offset is
   always 5 words / 20 bytes on send - see tcp.c). packed so
   sizeof(struct tcp_header) is exactly TCP_HDR_LEN. On RECEIVE,
   data_offset must still be read and honoured even though this stack
   never emits options itself: a real peer's SYN almost always carries
   an MSS option (and often more), which pushes its data_offset above
   5 - skip exactly that many bytes to find the payload, don't assume
   TCP_HDR_LEN. */
struct tcp_header
{
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset; /* high nibble = header length in 32-bit words; low nibble reserved, always 0 on send */
    uint8_t  flags;       /* TCP_FLAG_* bits */
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;  /* unused - URG is never set by this stack and is ignored on receive */
} __attribute__((packed));

/* Deliberately small subset of RFC 793's state machine - every state
   a normal connection actually passes through, but CLOSING (the
   simultaneous-close case where both sides send FIN before either
   ACKs the other's) isn't handled as its own state, and TIME_WAIT is
   collapsed straight to CLOSED with no 2MSL wait. See tcp.c's own
   notes at each transition for exactly what that trades away. */
typedef enum
{
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK
} tcp_state_t;

void tcp_init(void);

/* Starts listening on local_port. Returns a handle >= 0, or -1 if
   already listening on that port or the fixed connection table (see
   TCP_MAX_CONNS in tcp.c) is full. Use tcp_accept() to pick up
   connections as they complete their handshake. */
int tcp_listen(uint16_t local_port);

/* Non-blocking: returns a handle to a newly ESTABLISHED connection
   that arrived on listen_handle's port, or -1 if none is waiting.
   Only fully handshaken connections are ever handed back - matching
   accept()'s usual behaviour - so a client that sent a SYN but hasn't
   finished the three-way handshake yet just isn't visible here. */
int tcp_accept(int listen_handle);

/* Actively opens a connection to remote_ip:remote_port. Blocks (via
   task_yield(), pumping ethernet_poll()/tcp_poll() while it waits) up
   to a few seconds for the handshake to finish. Returns a handle on
   success, or -1 on timeout or an explicit refusal (a RST came back). */
int tcp_connect(uint32_t remote_ip, uint16_t remote_port);

/* Sends len bytes, splitting into multiple segments if needed. Blocks
   until each segment is individually acknowledged before sending the
   next - this stack only ever keeps ONE segment in flight per
   connection (see the struct tcp_conn comment in tcp.c for why),
   which is simple and correct but not fast. Returns the number of
   bytes actually sent, which is less than len only if the connection
   died or a send timed out partway through - not a short-write in the
   POSIX sense. */
int tcp_send(int handle, const void *data, uint16_t len);

/* Non-blocking: copies up to buf_len bytes of whatever's arrived (in
   order - out-of-order segments are dropped, not reassembled, see
   tcp.c) into buf and returns how many bytes were copied; 0 means
   nothing is waiting right now. Works even after the peer has sent
   its FIN (state CLOSE_WAIT) - there may still be buffered bytes to
   drain before that side of the connection is really done. */
uint16_t tcp_recv(int handle, void *buf, uint16_t buf_len);

/* Starts closing the connection: sends a FIN and moves to the
   appropriate half-closed state. Does not free the handle immediately
   - poll tcp_state() for TCP_CLOSED to know the close finished (or
   just stop using the handle; tcp_poll() will eventually finish
   tearing it down or give up after too many failed retransmits). */
void tcp_close(int handle);

/* Returns the connection's current state, or TCP_CLOSED for an
   out-of-range handle. Unlike every other function here this
   deliberately still answers for a CLOSED handle, since polling for
   TCP_CLOSED after tcp_close() is the intended way to confirm a close
   finished. */
tcp_state_t tcp_state(int handle);

/* Drives retransmission timers for every connection and should be
   called regularly regardless of whether any traffic is expected -
   this is what actually resends a lost SYN, data segment or FIN after
   a timeout, and what eventually gives up on a connection that never
   gets acknowledged. Called from net_poll() - not normally called
   directly. */
void tcp_poll(void);

/* Handles one TCP segment already stripped of its IPv4 header. Called
   from ipv4_handle_frame() when protocol == IPV4_PROTO_TCP - not
   normally called directly. */
void tcp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip);

#endif