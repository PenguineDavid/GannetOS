/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable check of kernel/proc/syscall.c's user_range_ok() and
   user_str_ok() - the bounds checks every syscall handler now runs on
   a ring3 app's pointer arguments before touching them. Duplicates the
   logic against fake stand-in regions (plain host arrays instead of
   the real __appbuf_start/__appstack_start linker symbols) rather than
   #including syscall.c, since that pulls in a pile of freestanding-only
   kernel headers - same reasoning as test_gdt_encoding.c.

   One deliberate deviation from the real code: the real syscall.c uses
   uint32_t for every address, because the actual target is genuine
   32-bit x86 where a pointer IS a uint32_t. This host is 64-bit, and a
   real static array's address here doesn't fit in 32 bits - truncating
   it to uint32_t for the test would produce an address that doesn't
   point at the array at all, and user_str_ok() actually dereferences
   memory, so that's a real out-of-bounds read, not just a numeric
   mismatch. Using uintptr_t here instead keeps every address exact on
   whatever host runs this, while leaving the actual range/overflow/
   NUL-scan logic identical to the real code - that logic is what's
   under test, not the specific integer width x86 happens to use. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Two fake regions standing in for .appbuf and .appstack, with a gap
   between them (and space below/above) so "outside both regions" is
   actually reachable to test against. */
static uint8_t fake_appbuf[256];
static uint8_t gap[64]; /* unmapped as far as the checks are concerned */
static uint8_t fake_appstack[128];

static uintptr_t appbuf_start, appbuf_end;
static uintptr_t appstack_start, appstack_end;

/* --- addr_range_in/user_range_ok/user_str_ok, address width widened
   from syscall.c's uint32_t to uintptr_t per the comment above; the
   range math, overflow check, and NUL-scan logic are otherwise an
   exact copy --- */
static int addr_range_in(uintptr_t ptr, uintptr_t len, uintptr_t start, uintptr_t end)
{
    uintptr_t span_end = ptr + len;
    if (span_end < ptr)
    {
        return 0;
    }
    return ptr >= start && span_end <= end;
}

static int user_range_ok(uintptr_t ptr, uintptr_t len)
{
    return addr_range_in(ptr, len, appbuf_start, appbuf_end) ||
           addr_range_in(ptr, len, appstack_start, appstack_end);
}

static int user_str_ok(uintptr_t ptr)
{
    uintptr_t region_end;
    if (ptr >= appbuf_start && ptr < appbuf_end)
    {
        region_end = appbuf_end;
    }
    else if (ptr >= appstack_start && ptr < appstack_end)
    {
        region_end = appstack_end;
    }
    else
    {
        return 0;
    }
    for (uintptr_t p = ptr; p < region_end; p++)
    {
        if (*(const volatile uint8_t *)p == 0)
        {
            return 1;
        }
    }
    return 0;
}
/* --- end copy --- */

static int failures = 0;
static void check(const char *name, int cond)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond)
    {
        failures++;
    }
}

int main(void)
{
    appbuf_start = (uintptr_t)fake_appbuf;
    appbuf_end = appbuf_start + sizeof(fake_appbuf);
    appstack_start = (uintptr_t)fake_appstack;
    appstack_end = appstack_start + sizeof(fake_appstack);
    (void)gap;

    /* --- user_range_ok --- */
    check("a buffer entirely inside appbuf is accepted",
          user_range_ok(appbuf_start + 10, 20));
    check("a buffer entirely inside appstack is accepted",
          user_range_ok(appstack_start + 5, 5));
    check("a buffer starting before appbuf is rejected",
          !user_range_ok(appbuf_start - 1, 10));
    check("a buffer ending past appbuf is rejected",
          !user_range_ok(appbuf_end - 5, 10));
    check("a buffer entirely outside both regions (the gap) is rejected",
          !user_range_ok(appbuf_end + 1, 4));
    check("a buffer far outside any region is rejected",
          !user_range_ok(appbuf_end + 0x100000, 4));
    check("a buffer exactly filling appbuf, start to end, is accepted",
          user_range_ok(appbuf_start, appbuf_end - appbuf_start));
    check("a zero-length span still needs ptr itself in-range (in-bounds case)",
          user_range_ok(appbuf_start, 0));
    check("a zero-length span at an out-of-range ptr is rejected "
          "(can't smuggle an arbitrary base pointer through with len=0)",
          !user_range_ok(appbuf_end + 0x100000, 0));
    check("ptr+len wrapping past the top of the address space is rejected",
          !user_range_ok((uintptr_t)-16, 0x1000));
    check("null pointer is rejected", !user_range_ok(0, 4));

    /* --- user_str_ok --- */
    memset(fake_appbuf, 'A', sizeof(fake_appbuf));
    fake_appbuf[20] = '\0';
    check("a NUL-terminated string inside appbuf is accepted", user_str_ok(appbuf_start));

    memset(fake_appbuf, 'B', sizeof(fake_appbuf)); /* no NUL anywhere now */
    check("a string with no NUL before the region ends is rejected",
          !user_str_ok(appbuf_start));

    memset(fake_appstack, 'C', sizeof(fake_appstack));
    fake_appstack[3] = '\0';
    check("a NUL-terminated string inside appstack is accepted", user_str_ok(appstack_start));

    check("a string pointer outside both regions is rejected",
          !user_str_ok(appbuf_end + 1));
    check("a string pointer of exactly 0 is rejected", !user_str_ok(0));

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}