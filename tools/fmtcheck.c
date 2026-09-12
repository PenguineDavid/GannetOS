/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/*
 * Check whether a disk image already contains a valid GannetOS superblock.
 * The formatter uses this tool to avoid overwriting an existing filesystem.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Must match filesys/fs.h and tools/fswrite.c - this tool doesn't include
   fs.h since it operates on a raw disk image, not through the kernel's
   own fs driver, so the on-disk layout constants are duplicated here. */
#define FS_SUPER_SECTOR 256
#define FS_MAGIC 0x504E4753u

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        return 1;
    }

    FILE *image_file = fopen(argv[1], "rb");
    if (!image_file)
    {
        return 1;
    }

    /* The superblock lives at a fixed sector in the on-disk format. */
    if (fseek(image_file, FS_SUPER_SECTOR * 512, SEEK_SET) != 0)
    {
        fclose(image_file);
        return 1;
    }

    uint32_t filesystem_magic = 0;
    if (fread(&filesystem_magic, sizeof(filesystem_magic), 1, image_file) != 1)
    {
        fclose(image_file);
        return 1;
    }
    fclose(image_file);

    if (filesystem_magic == FS_MAGIC)
    {
        printf("formatted\n");
        return 0;
    }
    printf("not formatted\n");
    return 1;
}