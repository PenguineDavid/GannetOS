/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable checks for kernel/dns.c's QNAME encoder and skip_name()
   - the two pieces of DNS parsing most likely to have an off-by-one
   (label-length framing and RFC 1035 message-compression pointers are
   exactly the kind of bit-fiddly logic that's easy to get subtly
   wrong and hard to notice by eye). Same duplication approach as
   test_net_wire.c: dns.c pulls in freestanding-only kernel headers
   (task.h, pit.h, ethernet.h) through its own includes, so this
   re-derives just the two functions under test rather than
   #including dns.c directly. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- duplicated from kernel/dns.c --------------------------------- */
static uint16_t encode_qname(const char *hostname, uint8_t *output, uint16_t output_capacity)
{
    uint16_t output_position = 0;
    const char *label_start = hostname;

    for (;;)
    {
        const char *label_end = label_start;
        while (*label_end && *label_end != '.')
        {
            label_end++;
        }

        uint32_t label_length = (uint32_t)(label_end - label_start);
        if (label_length == 0 || label_length > 63)
        {
            return 0;
        }
        if ((uint32_t)output_position + label_length + 1 >= output_capacity)
        {
            return 0;
        }

        output[output_position++] = (uint8_t)label_length;
        for (uint32_t index = 0; index < label_length; index++)
        {
            output[output_position++] = (uint8_t)label_start[index];
        }

        if (*label_end == '\0')
        {
            break;
        }
        label_start = label_end + 1;
    }

    if ((uint32_t)output_position + 1 > output_capacity)
    {
        return 0;
    }
    output[output_position++] = 0;
    return output_position;
}

static uint16_t skip_name(const uint8_t *packet, uint16_t packet_length, uint16_t offset)
{
    while (offset < packet_length)
    {
        uint8_t length_byte = packet[offset];
        /* A compression pointer always occupies exactly two bytes. */
        if ((length_byte & 0xC0) == 0xC0)
        {
            return (uint16_t)(offset + 2);
        }
        if (length_byte == 0)
        {
            return (uint16_t)(offset + 1);
        }
        offset = (uint16_t)(offset + 1 + length_byte);
    }
    return packet_length;
}

static int failures = 0;

static void check(const char *name, int condition)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
    {
        failures++;
    }
}

int main(void)
{
    /* --- encode_qname --------------------------------------------- */
    uint8_t buf[300];

    uint16_t len = encode_qname("google.com", buf, sizeof(buf));
    uint8_t expected[] = {6, 'g', 'o', 'o', 'g', 'l', 'e', 3, 'c', 'o', 'm', 0};
    check("encode_qname length for \"google.com\" is 12", len == sizeof(expected));
    check("encode_qname bytes for \"google.com\" match 6google3com0",
          len == sizeof(expected) && memcmp(buf, expected, sizeof(expected)) == 0);

    uint16_t single = encode_qname("localhost", buf, sizeof(buf));
    uint8_t expected_single[] = {9, 'l', 'o', 'c', 'a', 'l', 'h', 'o', 's', 't', 0};
    check("encode_qname handles a single label (no dots)",
          single == sizeof(expected_single) && memcmp(buf, expected_single, sizeof(expected_single)) == 0);

    check("encode_qname rejects an empty hostname", encode_qname("", buf, sizeof(buf)) == 0);
    check("encode_qname rejects a leading dot (empty first label)", encode_qname(".com", buf, sizeof(buf)) == 0);
    check("encode_qname rejects a trailing dot (empty last label)", encode_qname("google.", buf, sizeof(buf)) == 0);
    check("encode_qname rejects a double dot (empty middle label)", encode_qname("a..b", buf, sizeof(buf)) == 0);

    char label64[65];
    memset(label64, 'a', 64);
    label64[64] = '\0';
    check("encode_qname rejects a 64-byte label (max is 63)", encode_qname(label64, buf, sizeof(buf)) == 0);

    char label63[64];
    memset(label63, 'a', 63);
    label63[63] = '\0';
    check("encode_qname accepts a 63-byte label (the actual max)", encode_qname(label63, buf, sizeof(buf)) != 0);

    check("encode_qname refuses to overflow an undersized buffer",
          encode_qname("google.com", buf, 5) == 0);

    /* --- skip_name --------------------------------------------------
       Packet layout for this test: a plain name "a.b" at offset 0
       (2 + 2 + 1 = 5 bytes: 1a 1b 00), then at offset 5 a compression
       pointer (0xC0 0x00, pointing back at offset 0). --- */
    uint8_t packet[] = {1, 'a', 1, 'b', 0, 0xC0, 0x00};
    check("skip_name over a plain two-label name consumes 5 bytes",
          skip_name(packet, sizeof(packet), 0) == 5);
    check("skip_name over a compression pointer consumes exactly 2 bytes",
          skip_name(packet, sizeof(packet), 5) == 7);

    uint8_t root_only[] = {0};
    check("skip_name over just the root label consumes 1 byte",
          skip_name(root_only, sizeof(root_only), 0) == 1);

    uint8_t truncated[] = {5, 'h', 'e'}; /* claims a 5-byte label but only 2 bytes follow */
    check("skip_name on a truncated name stops at packet_len rather than reading past it",
          skip_name(truncated, sizeof(truncated), 0) == sizeof(truncated));

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}