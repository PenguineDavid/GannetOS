/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <stddef.h>
#include "kernel/drivers/net/transport_protocols/tcp.h"
#include "kernel/drivers/net/internet_layer/ipv4.h"
#include "kernel/drivers/net/link_layer/ethernet.h"
#include "kernel/drivers/net/include/net_byteorder.h"
#include "kernel/proc/task.h"
#include "kernel/drivers/pit/pit.h"

#define TCP_MAX_CONNS     4
#define TCP_RX_BUF_SIZE   2048
#define TCP_MAX_SEGMENT   536  /* the classic default MSS for a connection that never negotiated one */

#define TCP_RETRANSMIT_TIMEOUT_TICKS 200  /* 2s at pit_init(100) - see kernel.c */
#define TCP_MAX_RETRANSMITS          5
#define TCP_CONNECT_TIMEOUT_TICKS    500  /* 5s */
#define TCP_SEND_TIMEOUT_TICKS       500  /* 5s per segment, on top of its own retransmits */

/* One connection control block per active or listening connection.
   Fixed-size table, no heap - same convention as arp.c's cache. Two
   simplifications shape this whole file:

   1. Stop-and-wait, not a sliding window: at most ONE segment (data,
      or a bare SYN/FIN) is ever unacknowledged at a time per
      connection. tcp_send() only queues the next chunk once the
      previous one is fully acked. This means every sequence-number
      comparison this file needs to make is an exact EQUALITY check
      (== send_next, == recv_next) rather than a "is this before or
      after" ordering check - which is what real TCP stacks need
      modular-arithmetic-aware comparisons for, to handle sequence
      numbers wrapping past 2^32. Equality is exact even across a
      wrap, so this implementation is correct through a wraparound
      "for free", without needing that extra comparison logic at all.
      The cost is throughput: no data flows to fill idle round-trip
      time the way a real window would.

   2. No out-of-order reassembly: a segment that doesn't land exactly
      at recv_next is dropped, not buffered - the sender's own
      retransmission timer is relied on to eventually resend it in
      the right order. Correct, not efficient, and fine for a hobby
      OS's own traffic patterns (a lost segment on a local QEMU link
      is rare to begin with). */
struct tcp_conn
{
    tcp_state_t state;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    uint32_t send_una; /* oldest byte we've sent that isn't acked yet */
    uint32_t send_next; /* next sequence number we'll use - equals send_una whenever nothing is in flight */
    uint32_t recv_next; /* next sequence number we expect from the peer */

    uint8_t  unacked_data[TCP_MAX_SEGMENT]; /* saved copy of whatever's currently in flight, for retransmission */
    uint16_t unacked_len;
    uint8_t  unacked_flags;
    uint32_t last_send_tick;
    uint8_t  retransmit_count;

    uint8_t  rx_ring[TCP_RX_BUF_SIZE]; /* in-order bytes waiting for tcp_recv() */
    uint16_t rx_head;
    uint16_t rx_tail;
    uint16_t rx_count;

    int peer_fin_received; /* set the moment the peer's FIN is processed, even if our own side isn't done closing yet */
    int pending_accept;    /* 1 once handshake completes on a connection spawned from a LISTEN - cleared by tcp_accept() */
};

static struct tcp_conn conns[TCP_MAX_CONNS];

void tcp_init(void)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++)
    {
        conns[i].state = TCP_CLOSED;
    }
}

static struct tcp_conn *find_connection(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++)
    {
        struct tcp_conn *c = &conns[i];
        if (c->state != TCP_CLOSED && c->state != TCP_LISTEN &&
            c->local_port == local_port && c->remote_ip == remote_ip && c->remote_port == remote_port)
        {
            return c;
        }
    }
    return NULL;
}

static struct tcp_conn *find_listener(uint16_t local_port)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (conns[i].state == TCP_LISTEN && conns[i].local_port == local_port)
        {
            return &conns[i];
        }
    }
    return NULL;
}

static struct tcp_conn *alloc_slot(void)
{
    for (int i = 0; i < TCP_MAX_CONNS; i++)
    {
        if (conns[i].state == TCP_CLOSED)
        {
            struct tcp_conn *c = &conns[i];
            /* Zero everything - a slot that previously held a
               different, now-closed connection must not leak its old
               rx_ring bytes, sequence numbers or flags into whatever
               reuses this slot next. */
            uint8_t *raw = (uint8_t *)c;
            for (uint32_t j = 0; j < sizeof(*c); j++)
            {
                raw[j] = 0;
            }
            return c;
        }
    }
    return NULL;
}

static struct tcp_conn *get_conn(int handle)
{
    if (handle < 0 || handle >= TCP_MAX_CONNS)
    {
        return NULL;
    }
    if (conns[handle].state == TCP_CLOSED)
    {
        return NULL;
    }
    return &conns[handle];
}

static int handle_of(const struct tcp_conn *c)
{
    return (int)(c - conns);
}

static uint32_t initial_seq(void)
{
    /* NOT a cryptographically meaningful ISN generator - just spreads
       successive connections' starting sequence numbers apart so two
       opened moments apart don't collide. A hobby OS talking over a
       private QEMU link has no realistic off-path attacker to defend
       against here; a production stack would need RFC 6528's
       unpredictable ISN instead. */
    static uint32_t counter = 0;
    counter += 64000;
    return pit_ticks() ^ counter;
}

static uint16_t pick_ephemeral_port(void)
{
    static uint16_t next_port = 49152; /* start of the IANA dynamic/private port range */
    uint16_t port = next_port++;
    if (next_port == 0)
    {
        next_port = 49152; /* wrapped past 65535 */
    }
    return port;
}

/* Builds and sends one TCP segment with an explicit seq/flags/data -
   used both for fresh sends (send_segment, below) and for resending
   an unmodified copy on retransmit (tcp_poll). Does NOT touch any of
   the connection's sequence-number or retransmission bookkeeping
   itself - callers own that. */
static void build_and_send(struct tcp_conn *c, uint32_t seq, uint8_t flags,
                            const void *data, uint16_t data_len)
{
    uint8_t buf[TCP_HDR_LEN + TCP_MAX_SEGMENT];
    struct tcp_header *hdr = (struct tcp_header *)buf;

    hdr->src_port    = htons(c->local_port);
    hdr->dst_port    = htons(c->remote_port);
    hdr->seq         = htonl(seq);
    hdr->ack         = htonl((flags & TCP_FLAG_ACK) ? c->recv_next : 0);
    hdr->data_offset = (uint8_t)((TCP_HDR_LEN / 4) << 4);
    hdr->flags       = flags;
    hdr->window      = htons((uint16_t)(TCP_RX_BUF_SIZE - c->rx_count));
    hdr->checksum    = 0;
    hdr->urgent_ptr  = 0;

    uint8_t *body = buf + TCP_HDR_LEN;
    for (uint16_t i = 0; i < data_len; i++)
    {
        body[i] = ((const uint8_t *)data)[i];
    }

    uint16_t total = (uint16_t)(TCP_HDR_LEN + data_len);
    hdr->checksum = htons(ipv4_checksum_pseudo(ipv4_get_address(), c->remote_ip, IPV4_PROTO_TCP, buf, total));

    ipv4_send(c->remote_ip, IPV4_PROTO_TCP, buf, total);
}

/* Sends flags+data as a brand-new outgoing segment at c->send_next,
   saves a copy for retransmission, and advances send_next by however
   many sequence numbers this segment consumes - a SYN or a FIN each
   count as one, exactly like a data byte, per RFC 793. Because this
   connection only ever keeps ONE segment in flight (see the struct
   tcp_conn comment above), send_una must already equal send_next
   before calling this - i.e. wait for the previous segment to be
   acked before sending the next. */
static void send_segment(struct tcp_conn *c, uint8_t flags, const void *data, uint16_t data_len)
{
    build_and_send(c, c->send_next, flags, data, data_len);

    for (uint16_t i = 0; i < data_len; i++)
    {
        c->unacked_data[i] = ((const uint8_t *)data)[i];
    }
    c->unacked_len = data_len;
    c->unacked_flags = flags;
    c->last_send_tick = pit_ticks();
    c->retransmit_count = 0;

    uint32_t seq_len = data_len;
    if (flags & TCP_FLAG_SYN)
    {
        seq_len++;
    }
    if (flags & TCP_FLAG_FIN)
    {
        seq_len++;
    }
    c->send_next += seq_len;
}

/* A bare ACK carries no new information of its own - if it's lost,
   the connection's normal retransmission of whatever it WAS
   acknowledging (the peer's data or FIN) will eventually prompt
   another one - so unlike send_segment() this is never itself
   retransmitted, and doesn't touch send_next. */
static void send_ack(struct tcp_conn *c)
{
    build_and_send(c, c->send_next, TCP_FLAG_ACK, NULL, 0);
}

static void send_reset(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port,
                        uint32_t peer_seq, uint32_t peer_ack, uint16_t peer_data_len, uint8_t peer_flags)
{
    uint8_t buf[TCP_HDR_LEN];
    struct tcp_header *hdr = (struct tcp_header *)buf;

    hdr->src_port    = htons(local_port);
    hdr->dst_port    = htons(remote_port);
    hdr->data_offset = (uint8_t)((TCP_HDR_LEN / 4) << 4);
    hdr->window      = 0;
    hdr->urgent_ptr  = 0;

    /* RFC 793's rule for a reset to an unwanted segment: if that
       segment had ACK set, our reset uses ITS ack value as our
       sequence number and carries no ACK of its own. Otherwise our
       reset acknowledges past their sequence number - plus however
       much sequence space their segment consumed (its data, and/or a
       SYN or FIN, each worth one) - and sets ACK. */
    if (peer_flags & TCP_FLAG_ACK)
    {
        hdr->seq   = htonl(peer_ack);
        hdr->ack   = 0;
        hdr->flags = TCP_FLAG_RST;
    }
    else
    {
        uint32_t seg_len = peer_data_len;
        if (peer_flags & TCP_FLAG_SYN)
        {
            seg_len++;
        }
        if (peer_flags & TCP_FLAG_FIN)
        {
            seg_len++;
        }
        hdr->seq   = 0;
        hdr->ack   = htonl(peer_seq + seg_len);
        hdr->flags = TCP_FLAG_RST | TCP_FLAG_ACK;
    }
    hdr->checksum = 0;
    hdr->checksum = htons(ipv4_checksum_pseudo(ipv4_get_address(), remote_ip, IPV4_PROTO_TCP, buf, TCP_HDR_LEN));

    ipv4_send(remote_ip, IPV4_PROTO_TCP, buf, TCP_HDR_LEN);
}

int tcp_listen(uint16_t local_port)
{
    if (find_listener(local_port))
    {
        return -1; /* already listening on this port */
    }

    struct tcp_conn *c = alloc_slot();
    if (!c)
    {
        return -1;
    }
    c->local_port = local_port;
    c->state = TCP_LISTEN;
    return handle_of(c);
}

int tcp_accept(int listen_handle)
{
    if (listen_handle < 0 || listen_handle >= TCP_MAX_CONNS)
    {
        return -1;
    }
    struct tcp_conn *listener = &conns[listen_handle];
    if (listener->state != TCP_LISTEN)
    {
        return -1;
    }

    for (int i = 0; i < TCP_MAX_CONNS; i++)
    {
        struct tcp_conn *c = &conns[i];
        if (c->state == TCP_ESTABLISHED && c->pending_accept && c->local_port == listener->local_port)
        {
            c->pending_accept = 0;
            return i;
        }
    }
    return -1;
}

int tcp_connect(uint32_t remote_ip, uint16_t remote_port)
{
    struct tcp_conn *c = alloc_slot();
    if (!c)
    {
        return -1;
    }

    c->local_port  = pick_ephemeral_port();
    c->remote_ip   = remote_ip;
    c->remote_port = remote_port;
    c->send_una = c->send_next = initial_seq();
    c->state = TCP_SYN_SENT;
    send_segment(c, TCP_FLAG_SYN, NULL, 0);

    uint32_t start = pit_ticks();
    while (c->state == TCP_SYN_SENT)
    {
        ethernet_poll();
        tcp_poll(); /* drives the SYN's own retransmit timer */
        task_yield();
        if (pit_ticks() - start > TCP_CONNECT_TIMEOUT_TICKS)
        {
            c->state = TCP_CLOSED;
            return -1;
        }
    }
    return c->state == TCP_ESTABLISHED ? handle_of(c) : -1; /* -1 covers a RST (refused) or the timeout above */
}

int tcp_send(int handle, const void *data, uint16_t len)
{
    struct tcp_conn *c = get_conn(handle);
    if (!c || c->state != TCP_ESTABLISHED)
    {
        return -1;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    uint16_t sent = 0;

    while (sent < len)
    {
        uint16_t chunk = (uint16_t)(len - sent);
        if (chunk > TCP_MAX_SEGMENT)
        {
            chunk = TCP_MAX_SEGMENT;
        }

        send_segment(c, TCP_FLAG_ACK | TCP_FLAG_PSH, bytes + sent, chunk);

        uint32_t start = pit_ticks();
        while (c->send_una != c->send_next)
        {
            ethernet_poll();
            tcp_poll();
            task_yield();
            if (c->state == TCP_CLOSED)
            {
                return sent; /* aborted (RST, or gave up retransmitting - see tcp_poll) */
            }
            if (pit_ticks() - start > TCP_SEND_TIMEOUT_TICKS)
            {
                return sent; /* tcp_poll() already retried several times by now */
            }
        }
        sent = (uint16_t)(sent + chunk);
    }
    return sent;
}

uint16_t tcp_recv(int handle, void *buf, uint16_t buf_len)
{
    struct tcp_conn *c = get_conn(handle);
    if (!c)
    {
        return 0;
    }

    uint16_t n = c->rx_count < buf_len ? c->rx_count : buf_len;
    for (uint16_t i = 0; i < n; i++)
    {
        ((uint8_t *)buf)[i] = c->rx_ring[c->rx_head];
        c->rx_head = (uint16_t)((c->rx_head + 1) % TCP_RX_BUF_SIZE);
    }
    c->rx_count = (uint16_t)(c->rx_count - n);
    return n;
}

void tcp_close(int handle)
{
    struct tcp_conn *c = get_conn(handle);
    if (!c)
    {
        return;
    }

    switch (c->state)
    {
        case TCP_LISTEN:
        case TCP_SYN_SENT:
            c->state = TCP_CLOSED; /* nothing was ever established on the wire - nothing to tear down */
            break;
        case TCP_ESTABLISHED:
            send_segment(c, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            c->state = TCP_FIN_WAIT_1;
            break;
        case TCP_CLOSE_WAIT:
            send_segment(c, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
            c->state = TCP_LAST_ACK;
            break;
        default:
            break; /* already closing or closed - nothing more to do */
    }
}

tcp_state_t tcp_state(int handle)
{
    if (handle < 0 || handle >= TCP_MAX_CONNS)
    {
        return TCP_CLOSED;
    }
    return conns[handle].state;
}

void tcp_poll(void)
{
    uint32_t now = pit_ticks();
    for (int i = 0; i < TCP_MAX_CONNS; i++)
    {
        struct tcp_conn *c = &conns[i];
        if (c->state == TCP_CLOSED || c->state == TCP_LISTEN)
        {
            continue;
        }
        if (c->send_una == c->send_next)
        {
            continue; /* nothing outstanding to retransmit */
        }

        if (now - c->last_send_tick > TCP_RETRANSMIT_TIMEOUT_TICKS)
        {
            if (c->retransmit_count >= TCP_MAX_RETRANSMITS)
            {
                c->state = TCP_CLOSED; /* peer unreachable or gone - give up rather than retry forever */
                continue;
            }
            build_and_send(c, c->send_una, c->unacked_flags, c->unacked_data, c->unacked_len);
            c->last_send_tick = now;
            c->retransmit_count++;
        }
    }
}

void tcp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip)
{
    if (len < TCP_HDR_LEN)
    {
        return;
    }

    const struct tcp_header *hdr = (const struct tcp_header *)packet;
    uint16_t hdr_len = (uint16_t)(((hdr->data_offset >> 4) & 0x0F) * 4);
    if (hdr_len < TCP_HDR_LEN || hdr_len > len)
    {
        return; /* malformed, or a header claiming to be longer than the segment */
    }

    if (ipv4_checksum_pseudo(src_ip, ipv4_get_address(), IPV4_PROTO_TCP, packet, len) != 0)
    {
        return;
    }

    uint16_t src_port = ntohs(hdr->src_port);
    uint16_t dst_port = ntohs(hdr->dst_port);
    uint32_t seq      = ntohl(hdr->seq);
    uint32_t ack_num  = ntohl(hdr->ack);
    uint8_t  flags    = hdr->flags;
    const uint8_t *data = packet + hdr_len; /* skips any options - see struct tcp_header's own note on why this can't just be packet + TCP_HDR_LEN */
    uint16_t data_len = (uint16_t)(len - hdr_len);

    struct tcp_conn *c = find_connection(dst_port, src_ip, src_port);
    struct tcp_conn *listener = c ? NULL : find_listener(dst_port);

    if (!c && !listener)
    {
        if (!(flags & TCP_FLAG_RST))
        {
            send_reset(dst_port, src_ip, src_port, seq, ack_num, data_len, flags);
        }
        return;
    }

    if (listener)
    {
        if ((flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK))
        {
            struct tcp_conn *child = alloc_slot();
            if (!child)
            {
                return; /* connection table full - drop, the peer's own SYN retransmit will try again */
            }

            child->local_port  = dst_port;
            child->remote_ip   = src_ip;
            child->remote_port = src_port;
            child->recv_next   = seq + 1;
            child->send_una = child->send_next = initial_seq();
            child->state = TCP_SYN_RECEIVED;
            send_segment(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
        }
        /* Anything else aimed at a listening port with no connection
           yet - a bare ACK, data, a SYN retransmit once the child
           already exists - is simply dropped rather than reset;
           the child (if any) has its own retransmit timer already
           running and doesn't need prompting here. */
        return;
    }

    if (flags & TCP_FLAG_RST)
    {
        c->state = TCP_CLOSED;
        return;
    }

    if (c->state == TCP_SYN_SENT)
    {
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && ack_num == c->send_next)
        {
            c->send_una  = c->send_next;
            c->recv_next = seq + 1;
            c->state = TCP_ESTABLISHED;
            send_ack(c);
        }
        return;
    }

    /* Every state from here on shares the same ACK/data/FIN
       processing - only the state TRANSITIONS at the end of each
       piece differ by current state. */

    if ((flags & TCP_FLAG_ACK) && c->send_una != c->send_next && ack_num == c->send_next)
    {
        int fin_was_acked = (c->unacked_flags & TCP_FLAG_FIN) != 0;
        c->send_una = c->send_next;

        if (c->state == TCP_SYN_RECEIVED)
        {
            c->state = TCP_ESTABLISHED;
            c->pending_accept = 1;
        }
        else if (c->state == TCP_FIN_WAIT_1 && fin_was_acked)
        {
            c->state = c->peer_fin_received ? TCP_CLOSED : TCP_FIN_WAIT_2;
        }
        else if (c->state == TCP_LAST_ACK && fin_was_acked)
        {
            c->state = TCP_CLOSED;
        }
    }

    if (c->state == TCP_CLOSED)
    {
        return; /* just finished closing above - nothing left here wants this segment */
    }

    if (data_len > 0 && seq == c->recv_next &&
        (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2))
    {
        uint16_t space = (uint16_t)(TCP_RX_BUF_SIZE - c->rx_count);
        uint16_t accept_len = data_len < space ? data_len : space;
        for (uint16_t i = 0; i < accept_len; i++)
        {
            c->rx_ring[c->rx_tail] = data[i];
            c->rx_tail = (uint16_t)((c->rx_tail + 1) % TCP_RX_BUF_SIZE);
        }
        c->rx_count = (uint16_t)(c->rx_count + accept_len);
        c->recv_next += accept_len; /* only ever advances by what we actually buffered - this IS the flow control, see the window field in build_and_send */
        send_ack(c);
    }
    else if (data_len > 0)
    {
        /* Out of order, or arrived with nowhere to put it - dropped,
           no reassembly (see the struct tcp_conn comment). Re-ACK our
           current recv_next anyway so a peer doing fast retransmit
           has a chance to notice sooner than its own timeout. */
        send_ack(c);
    }

    if ((flags & TCP_FLAG_FIN) && seq + data_len == c->recv_next)
    {
        c->recv_next += 1;
        c->peer_fin_received = 1;
        send_ack(c);

        switch (c->state)
        {
            case TCP_ESTABLISHED:
                c->state = TCP_CLOSE_WAIT;
                break;
            case TCP_FIN_WAIT_2:
                c->state = TCP_CLOSED; /* no TIME_WAIT - see tcp.h's note on the state machine */
                break;
            default:
                break; /* FIN_WAIT_1: simultaneous close, resolved once our own FIN is acked above; CLOSE_WAIT/LAST_ACK: already knew */
        }
    }
}