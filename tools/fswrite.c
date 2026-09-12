/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

/*
 * fswrite.c -- host-side tool: formats the GannetOS filesystem if needed,
 *              then injects a file into the image.
 *
 * Usage: fswrite os.img /bin/name file.pexe
 *
 * All constants are derived from the same layout as fs.h.  Previously
 * they were hardcoded independently and drifted out of sync whenever
 * fs.h changed.  Now the struct and constants are defined once here and
 * must be kept in step with fs.h manually -- but since fswrite.c is a
 * host tool compiled separately we can't #include the cross-compiled
 * kernel header directly.
 *
 * Key layout (must match fs.h exactly):
 *   struct fs_inode  = 144 bytes  (name[128] + size + start + count + type + pad[3])
 *   INODES_PER_SECTOR = 512 / 144 = 3
 *   FS_SUPER_SECTOR   = 256
 *   FS_INODE_START    = 257
 *   FS_INODE_SECTORS  = 171   (ceil(512 inodes / 3 per sector))
 *   FS_DATA_START     = 257 + 171 = 428
 *   FS_MAX_INODES     = 512
 *   IMAGE_SECTORS     = 2880
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


/* ---- must match fs.h ---- */
#define FS_NAME_MAX 128
#define FS_SUPER_SECTOR 256
#define FS_INODE_START 257
#define FS_INODE_SECTORS 171
#define FS_DATA_START 428
#define FS_MAX_INODES 512
#define FS_MAGIC 0x504E4753u
#define SECTOR_SIZE 512
#define IMAGE_SECTORS 8192

#define FS_TYPE_FREE 0
#define FS_TYPE_FILE 1
#define FS_TYPE_DIR 2

#pragma pack(push, 1)
struct fs_inode
{
    char name[FS_NAME_MAX]; /* 128 bytes */
    uint32_t size;
    uint32_t start_block;
    uint32_t block_count;
    uint8_t type;
    uint8_t pad[3]; /* total = 144 bytes */
};

struct fs_super
{
    uint32_t magic;
    uint32_t version;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t data_sectors;
    uint8_t pad[492];
};
#pragma pack(pop)

/* Compile-time check: if this errors, the struct size doesn't match the
   layout assumption and INODES_PER_SECTOR will be wrong. */
typedef char _inode_size_check[sizeof(struct fs_inode) == 144 ? 1 : -1];

#define INODES_PER_SECTOR (SECTOR_SIZE / (int)sizeof(struct fs_inode)) /* = 3 */

/* ------------------------------------------------------------------ */
static uint8_t sector_buf[SECTOR_SIZE];

static int read_sector(FILE *f, uint32_t lba, uint8_t *buf)
{
    if (fseek(f, (long)(lba * SECTOR_SIZE), SEEK_SET))
        return -1;
    return (fread(buf, 1, SECTOR_SIZE, f) == SECTOR_SIZE) ? 0 : -1;
}

static int write_sector(FILE *f, uint32_t lba, const uint8_t *buf)
{
    if (fseek(f, (long)(lba * SECTOR_SIZE), SEEK_SET))
        return -1;
    return (fwrite(buf, 1, SECTOR_SIZE, f) == SECTOR_SIZE) ? 0 : -1;
}

static int read_inode(FILE *f, int idx, struct fs_inode *out)
{
    uint32_t lba = (uint32_t)(FS_INODE_START + idx / INODES_PER_SECTOR);
    if (read_sector(f, lba, sector_buf))
        return -1;
    memcpy(out, sector_buf + (idx % INODES_PER_SECTOR) * sizeof(struct fs_inode),
           sizeof(struct fs_inode));
    return 0;
}

static int write_inode(FILE *f, int idx, const struct fs_inode *in)
{
    uint32_t lba = (uint32_t)(FS_INODE_START + idx / INODES_PER_SECTOR);
    if (read_sector(f, lba, sector_buf))
        return -1;
    memcpy(sector_buf + (idx % INODES_PER_SECTOR) * sizeof(struct fs_inode),
           in, sizeof(struct fs_inode));
    return write_sector(f, lba, sector_buf);
}

/* ------------------------------------------------------------------ */
/* Format: write superblock, zero inode table, create root dir inode. */
/* ------------------------------------------------------------------ */
static int format_image(FILE *f)
{
    fprintf(stderr, "fswrite: filesystem not found -- formatting...\n");

    uint8_t zero[SECTOR_SIZE];
    memset(zero, 0, SECTOR_SIZE);

    /* Zero the entire inode region */
    for (int i = 0; i < FS_INODE_SECTORS; i++)
        if (write_sector(f, (uint32_t)(FS_INODE_START + i), zero))
            return -1;

    /* Write superblock */
    struct fs_super sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = FS_MAGIC;
    sb.version = 1;
    sb.inode_count = FS_MAX_INODES;
    sb.data_start = FS_DATA_START;
    sb.data_sectors = IMAGE_SECTORS - FS_DATA_START;
    memset(sector_buf, 0, SECTOR_SIZE);
    memcpy(sector_buf, &sb, sizeof(sb));
    if (write_sector(f, FS_SUPER_SECTOR, sector_buf))
        return -1;

    /* Root directory inode at slot 0 */
    struct fs_inode root;
    memset(&root, 0, sizeof(root));
    strncpy(root.name, "/", FS_NAME_MAX - 1);
    root.type = FS_TYPE_DIR;
    if (write_inode(f, 0, &root))
        return -1;

    /* Create standard directories that loader_init expects */
    const char *dirs[] = {"/bin", "/usr", "/usr/bin", "/home", NULL};
    for (int d = 0; dirs[d]; d++)
    {
        struct fs_inode dir;
        memset(&dir, 0, sizeof(dir));
        strncpy(dir.name, dirs[d], FS_NAME_MAX - 1);
        dir.type = FS_TYPE_DIR;
        if (write_inode(f, d + 1, &dir))
            return -1;
    }

    fprintf(stderr, "fswrite: formatted OK  "
                    "(inode_size=%d, per_sector=%d, data_start=%d)\n",
            (int)sizeof(struct fs_inode), INODES_PER_SECTOR, FS_DATA_START);
    return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: fswrite os.img /dest/path file.pexe\n");
        return 1;
    }

    const char *img_path = argv[1];
    const char *dest_name = argv[2];
    const char *src_path = argv[3];

    if (strlen(dest_name) >= FS_NAME_MAX)
    {
        fprintf(stderr, "destination path too long (max %d chars)\n", FS_NAME_MAX - 1);
        return 1;
    }

    /* Read source file */
    FILE *src = fopen(src_path, "rb");
    if (!src)
    {
        perror(src_path);
        return 1;
    }
    fseek(src, 0, SEEK_END);
    long src_size = ftell(src);
    fseek(src, 0, SEEK_SET);
    uint8_t *src_data = (uint8_t *)malloc((size_t)src_size);
    if (!src_data)
    {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    fread(src_data, 1, (size_t)src_size, src);
    fclose(src);

    FILE *img = fopen(img_path, "r+b");
    if (!img)
    {
        perror(img_path);
        return 1;
    }

    /* Read superblock; format if not present */
    struct fs_super sb;
    if (read_sector(img, FS_SUPER_SECTOR, sector_buf))
    {
        fprintf(stderr, "read superblock failed\n");
        return 1;
    }
    memcpy(&sb, sector_buf, sizeof(sb));
    if (sb.magic != FS_MAGIC)
    {
        if (format_image(img) != 0)
        {
            fprintf(stderr, "format failed\n");
            return 1;
        }
        /* Re-read superblock after format */
        read_sector(img, FS_SUPER_SECTOR, sector_buf);
        memcpy(&sb, sector_buf, sizeof(sb));
    }

    /* Find existing inode for this path, or a free slot */
    int target_idx = -1;
    for (int i = 0; i < FS_MAX_INODES; i++)
    {
        struct fs_inode in;
        if (read_inode(img, i, &in))
            continue;
        if (in.type != FS_TYPE_FREE && strcmp(in.name, dest_name) == 0)
        {
            target_idx = i;
            break;
        }
    }
    if (target_idx < 0)
    {
        for (int i = 0; i < FS_MAX_INODES; i++)
        {
            struct fs_inode in;
            if (read_inode(img, i, &in))
                continue;
            if (in.type == FS_TYPE_FREE)
            {
                target_idx = i;
                break;
            }
        }
    }
    if (target_idx < 0)
    {
        fprintf(stderr, "no free inode slots\n");
        return 1;
    }

    /* Build in-memory bitmap from existing inodes */
    uint32_t data_sectors = sb.data_sectors;
    uint8_t *bitmap = (uint8_t *)calloc(data_sectors, 1);
    if (!bitmap)
    {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    for (int i = 0; i < FS_MAX_INODES; i++)
    {
        if (i == target_idx)
            continue;
        struct fs_inode in;
        if (read_inode(img, i, &in))
            continue;
        if (in.type == FS_TYPE_FREE || in.block_count == 0)
            continue;
        uint32_t rel = in.start_block - FS_DATA_START;
        for (uint32_t j = 0; j < in.block_count; j++)
            if (rel + j < data_sectors)
                bitmap[rel + j] = 1;
    }

    /* Allocate contiguous sectors */
    uint32_t need = ((uint32_t)src_size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (need == 0)
        need = 1;
    uint32_t start_rel = 0;
    int found = 0;
    for (uint32_t i = 0; i + need <= data_sectors; i++)
    {
        int ok = 1;
        for (uint32_t j = 0; j < need; j++)
            if (bitmap[i + j])
            {
                ok = 0;
                break;
            }
        if (ok)
        {
            start_rel = i;
            found = 1;
            break;
        }
    }
    free(bitmap);
    if (!found)
    {
        fprintf(stderr, "no free space\n");
        return 1;
    }

    uint32_t start_lba = FS_DATA_START + start_rel;

    /* Write data sectors */
    uint8_t wbuf[SECTOR_SIZE];
    for (uint32_t s = 0; s < need; s++)
    {
        memset(wbuf, 0, SECTOR_SIZE);
        uint32_t off = s * SECTOR_SIZE;
        uint32_t tocopy = (uint32_t)src_size - off;
        if (tocopy > SECTOR_SIZE)
            tocopy = SECTOR_SIZE;
        memcpy(wbuf, src_data + off, tocopy);
        if (write_sector(img, start_lba + s, wbuf))
        {
            fprintf(stderr, "write data sector failed\n");
            return 1;
        }
    }
    free(src_data);

    /* Write inode */
    struct fs_inode inode;
    memset(&inode, 0, sizeof(inode));
    strncpy(inode.name, dest_name, FS_NAME_MAX - 1);
    inode.size = (uint32_t)src_size;
    inode.start_block = start_lba;
    inode.block_count = need;
    inode.type = FS_TYPE_FILE;
    if (write_inode(img, target_idx, &inode))
    {
        fprintf(stderr, "write inode failed\n");
        return 1;
    }

    fclose(img);
    printf("fswrite: wrote %ld bytes to %s (inode %d, sectors %u-%u)\n",
           src_size, dest_name, target_idx, start_lba, start_lba + need - 1);
    return 0;
}