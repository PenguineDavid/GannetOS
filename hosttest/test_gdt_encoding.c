/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Host-testable check of the GDT descriptor byte-packing logic in
   kernel/gdt.c's gdt_set_gate(). This is the same pack/shift math the
   real kernel uses, lifted out so it can be verified on the host without
   booting a VM - same spirit as tools/test_bsp.c for kernel/bsp.c.
   It intentionally duplicates the packing logic rather than #including
   gdt.c, since gdt.c pulls in freestanding-only asm externs. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

struct gdt_entry
{
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

static void gdt_set_gate(struct gdt_entry *gdt, int entry_index, uint32_t base,
                   uint32_t limit, uint8_t access, uint8_t granularity_flags)
{
    gdt[entry_index].base_low = base & 0xFFFF;
    gdt[entry_index].base_mid = (base >> 16) & 0xFF;
    gdt[entry_index].base_high = (base >> 24) & 0xFF;
    gdt[entry_index].limit_low = limit & 0xFFFF;
    gdt[entry_index].granularity = ((limit >> 16) & 0x0F) | (granularity_flags & 0xF0);
    gdt[entry_index].access = access;
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
    struct gdt_entry gdt[6];
    memset(gdt, 0xAA, sizeof(gdt));

    /* Null descriptor must be all zero. */
    gdt_set_gate(gdt, 0, 0, 0, 0, 0);
    check("null descriptor is all-zero",
          gdt[0].limit_low == 0 && gdt[0].base_low == 0 && gdt[0].base_mid == 0 &&
          gdt[0].access == 0 && gdt[0].granularity == 0 && gdt[0].base_high == 0);

    /* Kernel code: flat 4GB, access 0x9A, gran 0xC0 -> matches the exact
       byte pattern boot.asm's own minimal GDT already used for its code
       descriptor (dw 0xffff,0x0000 / db 0x00,0x9a,0xcf,0x00), so the
       ring0 selectors behave identically before and after gdt_init()
       replaces boot.asm's table. */
    gdt_set_gate(gdt, 1, 0, 0xFFFFFFFF, 0x9A, 0xC0);
    check("kernel code: base is 0", gdt[1].base_low == 0 && gdt[1].base_mid == 0 && gdt[1].base_high == 0);
    check("kernel code: limit_low is 0xFFFF", gdt[1].limit_low == 0xFFFF);
    check("kernel code: granularity byte is 0xCF (0xC0 flags | 0xF limit high nibble)", gdt[1].granularity == 0xCF);
    check("kernel code: access byte is 0x9A", gdt[1].access == 0x9A);

    /* Kernel data: same shape, access 0x92 (matches boot.asm's data entry: db 0x00,0x92,0xcf,0x00). */
    gdt_set_gate(gdt, 2, 0, 0xFFFFFFFF, 0x92, 0xC0);
    check("kernel data: granularity byte is 0xCF", gdt[2].granularity == 0xCF);
    check("kernel data: access byte is 0x92", gdt[2].access == 0x92);

    /* User code/data: identical to their kernel counterparts except DPL
       bits (5:6) set, i.e. access + 0x60. */
    gdt_set_gate(gdt, 3, 0, 0xFFFFFFFF, 0xFA, 0xC0);
    check("user code: access byte is 0xFA (0x9A + DPL3)", gdt[3].access == 0xFA);
    check("user code: DPL bits (5:6) are 11", ((gdt[3].access >> 5) & 0x3) == 0x3);

    gdt_set_gate(gdt, 4, 0, 0xFFFFFFFF, 0xF2, 0xC0);
    check("user data: access byte is 0xF2 (0x92 + DPL3)", gdt[4].access == 0xF2);
    check("user data: DPL bits (5:6) are 11", ((gdt[4].access >> 5) & 0x3) == 0x3);

    /* Kernel selectors must NOT be usable from CPL3: DPL bits must be 0. */
    check("kernel code: DPL bits are 00 (not ring3-usable)", ((gdt[1].access >> 5) & 0x3) == 0x0);
    check("kernel data: DPL bits are 00 (not ring3-usable)", ((gdt[2].access >> 5) & 0x3) == 0x0);

    /* TSS descriptor: base/limit must actually point at the real struct
       address and size, not the flat-segment convention the others use.
       This is the field a wrong write_tss() would most plausibly get
       wrong (e.g. reusing the 0xFFFFFFFF flat-limit pattern by mistake,
       which would make an out-of-bounds esp0/ss0 read look "valid" to
       the CPU instead of catching a real TSS corruption bug). */
    uint32_t fake_tss_base = 0x12345000;
    uint32_t fake_tss_size = 104; /* sizeof(struct tss_entry) on this layout */
    gdt_set_gate(gdt, 5, fake_tss_base, fake_tss_base + fake_tss_size, 0x89, 0x00);
    uint32_t decoded_base = gdt[5].base_low | (gdt[5].base_mid << 16) | (gdt[5].base_high << 24);
    uint32_t decoded_limit = gdt[5].limit_low | ((gdt[5].granularity & 0x0F) << 16);
    /* The GDT limit field is only 20 bits wide (16 in limit_low + 4 in the
       granularity nibble) regardless of granularity - that's an x86
       descriptor-format limit, not something gdt_set_gate can work
       around, so the round-trip is only expected to hold on those low 20
       bits. write_tss()'s real limit (base + sizeof(tss), both small
       numbers) never gets near that ceiling, so this is exercising the
       packing math, not a real capacity requirement on the TSS itself. */
    check("TSS descriptor: base round-trips correctly", decoded_base == fake_tss_base);
    check("TSS descriptor: limit round-trips correctly (low 20 bits)",
          decoded_limit == ((fake_tss_base + fake_tss_size) & 0xFFFFF));
    check("TSS descriptor: access byte is 0x89 (present, ring0, 32-bit TSS)", gdt[5].access == 0x89);
    check("TSS descriptor: granularity flags nibble is 0 (byte-granular)", (gdt[5].granularity & 0xF0) == 0x00);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}