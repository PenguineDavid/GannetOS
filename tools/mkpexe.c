/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/*
 * mkpexe.c  --  host-side build tool
 * Usage: mkpexe input.elf output.pexe
 *
 * Reads a 32-bit LE ELF produced by the app linker, extracts e_entry to
 * compute the correct entry_offset for the PEXE header, then flattens all
 * PT_LOAD segments into a raw binary and prepends the 16-byte header.
 *
 * The linker MUST be invoked with --entry=app_main so that e_entry points
 * at app_main rather than the default (_start / first symbol).
 *
 * entry_offset = e_entry - first_PT_LOAD.p_vaddr
 *
 * For apps linked with -Ttext 0x200000 this is e_entry - 0x200000, which
 * gives the byte offset from the start of the binary at which app_main sits.
 * The kernel loader jumps to exec_buf + entry_offset, which is exact.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define PEXE_MAGIC 0x45584550u
#define PT_LOAD 1
#define ELFCLASS32 1
#define ELFDATA2LSB 1

typedef struct
{
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum;
    uint16_t e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct
{
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

static int read_phdr(FILE *f, const Elf32_Ehdr *eh, int i, Elf32_Phdr *out)
{
    if (fseek(f, (long)(eh->e_phoff + (unsigned)i * eh->e_phentsize), SEEK_SET))
        return -1;
    return fread(out, sizeof(*out), 1, f) == 1 ? 0 : -1;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: mkpexe input.elf output.pexe\n");
        return 1;
    }

    FILE *elf = fopen(argv[1], "rb");
    if (!elf)
    {
        perror(argv[1]);
        return 1;
    }

    Elf32_Ehdr eh;
    if (fread(&eh, sizeof(eh), 1, elf) != 1 ||
        memcmp(eh.e_ident, "\x7f"
                           "ELF",
               4) != 0 ||
        eh.e_ident[4] != ELFCLASS32 ||
        eh.e_ident[5] != ELFDATA2LSB)
    {
        fprintf(stderr, "mkpexe: %s: not a valid 32-bit LE ELF\n", argv[1]);
        fclose(elf);
        return 1;
    }

    /* Locate the PT_LOAD segment that actually CONTAINS the entry point,
       and use ITS p_vaddr as load_base -- not just the first PT_LOAD
       found. ld routinely inserts a small extra LOAD segment before the
       real .text segment purely to cover the ELF header itself; using
       that as load_base silently shifts every offset computed below by
       the gap between the two segments, corrupting every absolute
       address the app's own code relies on. */
    uint32_t load_base = 0;
    int found_load = 0;
    for (int i = 0; i < eh.e_phnum; i++)
    {
        Elf32_Phdr ph;
        if (read_phdr(elf, &eh, i, &ph))
            continue;
        if (ph.p_type != PT_LOAD)
            continue;
        if (eh.e_entry >= ph.p_vaddr && eh.e_entry < ph.p_vaddr + ph.p_memsz)
        {
            load_base = ph.p_vaddr;
            found_load = 1;
            break;
        }
    }
    if (!found_load)
    {
        fprintf(stderr, "mkpexe: %s: no PT_LOAD segment contains the entry point\n", argv[1]);
        fclose(elf);
        return 1;
    }

    uint32_t entry_offset = eh.e_entry - load_base;

    /* Compute flat binary extent across all PT_LOAD segments at or above
       load_base -- file-backed content only (.text/.rodata/.data).
       Segments entirely below load_base (the header-only one noted
       above) are intentionally skipped: the app's code never references
       its own ELF header at runtime, so there's nothing there worth
       keeping, and including it would require a negative offset. */
    uint32_t bin_end = load_base;
    /* Compute the FULL memory extent including .bss (p_memsz, not just
       p_filesz). The kernel loader needs this to zero-fill .bss itself,
       since that space has no file content to read in the first place. */
    uint32_t mem_end = load_base;
    for (int i = 0; i < eh.e_phnum; i++)
    {
        Elf32_Phdr ph;
        if (read_phdr(elf, &eh, i, &ph))
            continue;
        if (ph.p_type != PT_LOAD)
            continue;
        if (ph.p_vaddr < load_base)
            continue;
        uint32_t fend = ph.p_vaddr + ph.p_filesz;
        if (fend > bin_end)
            bin_end = fend;
        uint32_t mend = ph.p_vaddr + ph.p_memsz;
        if (mend > mem_end)
            mem_end = mend;
    }
    uint32_t bin_size = bin_end - load_base;
    uint32_t mem_size = mem_end - load_base;

    uint8_t *body = (uint8_t *)calloc(1, bin_size ? bin_size : 1);
    if (!body)
    {
        fprintf(stderr, "mkpexe: out of memory\n");
        fclose(elf);
        return 1;
    }

    /* Copy each PT_LOAD's file content into the flat buffer, again
       skipping anything below load_base for the same reason as above. */
    for (int i = 0; i < eh.e_phnum; i++)
    {
        Elf32_Phdr ph;
        if (read_phdr(elf, &eh, i, &ph))
            continue;
        if (ph.p_type != PT_LOAD || ph.p_filesz == 0)
            continue;
        if (ph.p_vaddr < load_base)
            continue;
        if (fseek(elf, (long)ph.p_offset, SEEK_SET))
            continue;
        fread(body + (ph.p_vaddr - load_base), 1, ph.p_filesz, elf);
    }
    fclose(elf);

    FILE *out = fopen(argv[2], "wb");
    if (!out)
    {
        perror(argv[2]);
        free(body);
        return 1;
    }

    uint32_t hdr[4] = {PEXE_MAGIC, entry_offset, mem_size, 0};
    fwrite(hdr, 4, 4, out);
    fwrite(body, 1, bin_size, out);
    fclose(out);
    free(body);

    printf("mkpexe: %s -> %s  entry_offset=0x%x  body=%u bytes  mem_size=%u bytes\n",
           argv[1], argv[2], entry_offset, bin_size, mem_size);
    return 0;
}