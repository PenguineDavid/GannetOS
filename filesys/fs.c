/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "filesys/fs.h"
#include "filesys/ata.h"
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */
static int str_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (*a++ != *b++)
        {
            return 0;
        }
    }
    return *a == *b;
}

static int str_len(const char *s)
{
    int n = 0;
    while (*s++)
    {
        n++;
    }
    return n;
}

static void mem_zero(void *p, int n)
{
    uint8_t *b = (uint8_t *)p;
    while (n--)
    {
        *b++ = 0;
    }
}

static void mem_copy(void *d, const void *s, int n)
{
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--)
    {
        *dd++ = *ss++;
    }
}

static void str_copy(char *d, const char *s, int max)
{
    int i = 0;
    while (i < max - 1 && s[i])
    {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* Inode I/O (8 inodes per 512-byte sector)                           */
/* ------------------------------------------------------------------ */
#define INODES_PER_SECTOR (512 / sizeof(struct fs_inode))

static int inode_read(int idx, struct fs_inode *out)
{
    if (idx < 0 || idx >= FS_MAX_INODES)
    {
        return -1;
    }
    uint8_t sb[512];
    if (ata_read_sector((uint32_t)(FS_INODE_START + idx / INODES_PER_SECTOR), sb))
    {
        return -1;
    }
    mem_copy(out, sb + (idx % INODES_PER_SECTOR) * sizeof(struct fs_inode), sizeof(struct fs_inode));
    return 0;
}

static int inode_write(int idx, const struct fs_inode *in)
{
    if (idx < 0 || idx >= FS_MAX_INODES)
    {
        return -1;
    }
    uint8_t sb[512];
    if (ata_read_sector((uint32_t)(FS_INODE_START + idx / INODES_PER_SECTOR), sb))
    {
        return -1;
    }
    mem_copy(sb + (idx % INODES_PER_SECTOR) * sizeof(struct fs_inode), in, sizeof(struct fs_inode));
    return ata_write_sector((uint32_t)(FS_INODE_START + idx / INODES_PER_SECTOR), sb);
}

/* ------------------------------------------------------------------ */
/* Allocation bitmap (in-memory)                                       */
/* ------------------------------------------------------------------ */
/* Must cover every data sector fs_init() can ever hand out. FS_TOTAL_SECTORS
   matches the disk image size the Makefile actually creates (`dd ... count=8192`),
   the same value fs_init() uses below to compute data_sector_count - both MUST
   stay in sync, since data_sector_count is what bm_alloc()/bm_set()/bm_clr()
   actually index against at runtime.
   BUG FIX: this used to be a bare #define BITMAP_WORDS 87 (covering only the
   first 2784 of the real 7764 data sectors) - bm_set()/bm_clr() on any sector
   index at or past 2784 indexed straight past the end of alloc_bitmap[] and
   corrupted whatever static memory happened to sit after it. Silent, and only
   triggered once roughly 1.4MB of data had been allocated somewhere on disk -
   the kind of bug that looks like unrelated memory corruption far from its
   actual cause. */
#define FS_TOTAL_SECTORS 8192
#define BITMAP_WORDS (((FS_TOTAL_SECTORS - FS_DATA_START) + 31) / 32)
static uint32_t alloc_bitmap[BITMAP_WORDS];
static uint32_t data_sector_count = 0;

static void bm_set(uint32_t b)
{
    alloc_bitmap[b / 32] |= (1u << (b % 32));
}
static void bm_clr(uint32_t b)
{
    alloc_bitmap[b / 32] &= ~(1u << (b % 32));
}
static int bm_get(uint32_t b)
{
    return (alloc_bitmap[b / 32] >> (b % 32)) & 1;
}

static int bm_alloc(uint32_t count)
{
    for (uint32_t i = 0; i + count <= data_sector_count; i++)
    {
        int ok = 1;
        for (uint32_t j = 0; j < count; j++)
        {
            if (bm_get(i + j))
            {
                ok = 0;
                break;
            }
        }
        if (ok)
        {
            for (uint32_t j = 0; j < count; j++)
            {
                bm_set(i + j);
            }
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Open fd table                                                        */
/* ------------------------------------------------------------------ */
static struct fs_fd fds[FS_MAX_OPEN];

/* ------------------------------------------------------------------ */
/* fs_init                                                             */
/* ------------------------------------------------------------------ */
int fs_init(void)
{
    for (int i = 0; i < FS_MAX_OPEN; i++)
    {
        fds[i].inode_idx = -1;
    }

    uint8_t buf[512];
    if (ata_read_sector(FS_SUPER_SECTOR, buf))
    {
        return -1;
    }
    struct fs_super *sb = (struct fs_super *)buf;

    if (sb->magic != FS_MAGIC)
    {
        /* Format */
        mem_zero(buf, 512);
        sb->magic = FS_MAGIC;
        sb->version = 1;
        sb->inode_count = FS_MAX_INODES;
        sb->data_start = FS_DATA_START;
        sb->data_sectors = FS_TOTAL_SECTORS - FS_DATA_START;
        if (ata_write_sector(FS_SUPER_SECTOR, buf))
        {
            return -1;
        }
        uint8_t zero[512];
        mem_zero(zero, 512);
        for (int i = 0; i < FS_INODE_SECTORS; i++)
        {
            if (ata_write_sector((uint32_t)(FS_INODE_START + i), zero))
            {
                return -1;
            }
        }
        data_sector_count = FS_TOTAL_SECTORS - FS_DATA_START;
        mem_zero(alloc_bitmap, sizeof(alloc_bitmap));

        /* Create root directory inode */
        struct fs_inode root;
        mem_zero(&root, sizeof(root));
        str_copy(root.name, "/", FS_NAME_MAX);
        root.type = FS_TYPE_DIR;
        inode_write(0, &root);
        return 0;
    }

    data_sector_count = sb->data_sectors;
    mem_zero(alloc_bitmap, sizeof(alloc_bitmap));
    for (int i = 0; i < FS_MAX_INODES; i++)
    {
        struct fs_inode inode;
        if (inode_read(i, &inode))
        {
            continue;
        }
        if (inode.type == FS_TYPE_FREE || inode.block_count == 0)
        {
            continue;
        }
        uint32_t rel = inode.start_block - FS_DATA_START;
        for (uint32_t j = 0; j < inode.block_count; j++)
        {
            bm_set(rel + j);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */
void fs_get_parent(const char *path, char *parent_out)
{
    int len = str_len(path);
    int last = 0;
    for (int i = 0; i < len; i++)
    {
        if (path[i] == '/')
        {
            last = i;
        }
    }
    if (last == 0)
    {
        parent_out[0] = '/';
        parent_out[1] = '\0';
        return;
    }
    str_copy(parent_out, path, last + 1);
    parent_out[last] = '\0';
}

/*
 * Returns 1 if 'path' names an existing directory (including "/").
 * Used internally to validate parent directories before creating
 * files or subdirectories.
 */
static int parent_exists(const char *path)
{
    char parent[FS_NAME_MAX];
    fs_get_parent(path, parent);
    return fs_isdir(parent);
}

/* ------------------------------------------------------------------ */
/* fs_find                                                             */
/* ------------------------------------------------------------------ */
int fs_find(const char *path)
{
    for (int i = 0; i < FS_MAX_INODES; i++)
    {
        struct fs_inode inode;
        if (inode_read(i, &inode))
        {
            continue;
        }
        if (inode.type == FS_TYPE_FREE)
        {
            continue;
        }
        if (str_eq(inode.name, path))
        {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Directory API                                                       */
/* ------------------------------------------------------------------ */
int fs_isdir(const char *path)
{
    if (str_eq(path, "/"))
    {
        return 1;
    }
    int idx = fs_find(path);
    if (idx < 0)
    {
        return 0;
    }
    struct fs_inode inode;
    if (inode_read(idx, &inode))
    {
        return 0;
    }
    return inode.type == FS_TYPE_DIR;
}

int fs_mkdir(const char *path)
{
    if (fs_find(path) >= 0)
    {
        return -1; /* already exists */
    }

    /* Bug fix: refuse to create a directory whose parent does not exist. */
    if (!parent_exists(path))
    {
        return -1;
    }

    int idx = -1;
    for (int i = 0; i < FS_MAX_INODES; i++)
    {
        struct fs_inode tmp;
        if (inode_read(i, &tmp))
        {
            continue;
        }
        if (tmp.type == FS_TYPE_FREE)
        {
            idx = i;
            break;
        }
    }
    if (idx < 0)
    {
        return -1;
    }

    struct fs_inode inode;
    mem_zero(&inode, sizeof(inode));
    str_copy(inode.name, path, FS_NAME_MAX);
    inode.type = FS_TYPE_DIR;
    return inode_write(idx, &inode);
}

int fs_rmdir(const char *path)
{
    if (str_eq(path, "/"))
    {
        return -1;
    }

    int idx = fs_find(path);
    if (idx < 0)
    {
        return -1;
    }

    struct fs_inode inode;
    if (inode_read(idx, &inode))
    {
        return -1;
    }
    if (inode.type != FS_TYPE_DIR)
    {
        return -1;
    }

    /* Refuse if any inode has this dir as parent */
    char parent[FS_NAME_MAX];
    for (int i = 0; i < FS_MAX_INODES; i++)
    {
        if (i == idx)
        {
            continue;
        }
        struct fs_inode child;
        if (inode_read(i, &child))
        {
            continue;
        }
        if (child.type == FS_TYPE_FREE)
        {
            continue;
        }
        fs_get_parent(child.name, parent);
        if (str_eq(parent, path))
        {
            return -1;
        }
    }

    mem_zero(&inode, sizeof(inode));
    return inode_write(idx, &inode);
}

/*
 * fs_rrmdr: recursive remove directory.
 *
 * Bug fix: the previous version unconditionally called fs_rrmdr() on
 * every child of the directory being removed, whether that child was a
 * file or a subdirectory. fs_rrmdr() (like fs_rmdir()) only ever accepts
 * an actual directory -- it type-checks and returns -1 for anything
 * else -- so every FILE child's "removal" silently failed and the file
 * was never actually deleted. Worse, that failure was never checked:
 * the function fell through and zeroed the parent directory's own inode
 * regardless, so the top-level call reported success (0) even though
 * files were left behind on disk. That's exactly "it says it removed
 * them, it doesn't".
 *
 * Fixed by dispatching each child to the correct removal function based
 * on its actual type (fs_delete for files, fs_rrmdr for subdirectories),
 * and by checking every child removal's return value -- if any child
 * fails to be removed, the whole call now fails and stops rather than
 * silently deleting the parent directory out from under remaining
 * children.
 */
int fs_rrmdr(const char *path)
{
    if (str_eq(path, "/"))
    {
        return -1;
    }

    int idx = fs_find(path);
    if (idx < 0)
    {
        return -1;
    }

    struct fs_inode inode;
    if (inode_read(idx, &inode))
    {
        return -1;
    }
    if (inode.type != FS_TYPE_DIR)
    {
        return -1;
    }

    char parent[FS_NAME_MAX];
    for (int i = 0; i < FS_MAX_INODES; i++)
    {
        if (i == idx)
        {
            continue;
        }
        struct fs_inode child;
        if (inode_read(i, &child))
        {
            continue;
        }
        if (child.type == FS_TYPE_FREE)
        {
            continue;
        }
        fs_get_parent(child.name, parent);
        if (!str_eq(parent, path))
        {
            continue;
        }

        int rc = (child.type == FS_TYPE_DIR) ? fs_rrmdr(child.name)
                                              : fs_delete(child.name);
        if (rc != 0)
        {
            return -1; /* stop -- don't remove the parent while a child
                           of it still exists on disk */
        }
    }

    mem_zero(&inode, sizeof(inode));
    return inode_write(idx, &inode);
}

/* ------------------------------------------------------------------ */
/* fs_open                                                             */
/* ------------------------------------------------------------------ */
int fs_open(const char *path, int create)
{
    int fd = -1;
    for (int i = 0; i < FS_MAX_OPEN; i++)
    {
        if (fds[i].inode_idx == -1)
        {
            fd = i;
            break;
        }
    }
    if (fd < 0)
    {
        return -1;
    }

    int idx = fs_find(path);
    if (idx < 0)
    {
        if (!create)
        {
            return -1;
        }

        /* Bug fix: refuse to create a file in a non-existent directory. */
        if (!parent_exists(path))
        {
            return -1;
        }

        for (idx = 0; idx < FS_MAX_INODES; idx++)
        {
            struct fs_inode tmp;
            if (inode_read(idx, &tmp))
            {
                continue;
            }
            if (tmp.type == FS_TYPE_FREE)
            {
                break;
            }
        }
        if (idx == FS_MAX_INODES)
        {
            return -1;
        }

        struct fs_inode inode;
        mem_zero(&inode, sizeof(inode));
        str_copy(inode.name, path, FS_NAME_MAX);
        inode.type = FS_TYPE_FILE;
        if (inode_write(idx, &inode))
        {
            return -1;
        }
    }
    else
    {
        struct fs_inode tmp;
        if (inode_read(idx, &tmp))
        {
            return -1;
        }
        if (tmp.type == FS_TYPE_DIR)
        {
            return -1;
        }
    }

    fds[fd].inode_idx = idx;
    fds[fd].offset = 0;
    return fd;
}

void fs_close(int fd)
{
    if (fd >= 0 && fd < FS_MAX_OPEN)
    {
        fds[fd].inode_idx = -1;
    }
}

/* ------------------------------------------------------------------ */
/* fs_read                                                             */
/* ------------------------------------------------------------------ */
int fs_read(int fd, void *buf, uint32_t len)
{
    if (fd < 0 || fd >= FS_MAX_OPEN || fds[fd].inode_idx < 0)
    {
        return -1;
    }
    struct fs_inode inode;
    if (inode_read(fds[fd].inode_idx, &inode))
    {
        return -1;
    }

    /*
     * Bug fix: if the fd offset has somehow advanced past EOF (e.g. after a
     * truncated write), inode.size - fds[fd].offset would underflow as an
     * unsigned subtraction and produce a huge positive remainder.  Guard it
     * explicitly.
     */
    if (fds[fd].offset >= inode.size)
    {
        return 0;
    }
    uint32_t rem = inode.size - fds[fd].offset;
    if (len > rem)
    {
        len = rem;
    }
    if (!len)
    {
        return 0;
    }

    uint8_t sb[512];
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < len)
    {
        uint32_t si = (fds[fd].offset + done) / 512;
        uint32_t so = (fds[fd].offset + done) % 512;
        uint32_t tc = 512 - so;
        if (tc > len - done)
        {
            tc = len - done;
        }
        if (ata_read_sector(inode.start_block + si, sb))
        {
            return -1;
        }
        mem_copy(out + done, sb + so, (int)tc);
        done += tc;
    }
    fds[fd].offset += done;
    return (int)done;
}

/* ------------------------------------------------------------------ */
/* fs_write                                                            */
/* ------------------------------------------------------------------ */
int fs_write(int fd, const void *buf, uint32_t len)
{
    if (fd < 0 || fd >= FS_MAX_OPEN || fds[fd].inode_idx < 0)
    {
        return -1;
    }
    if (!len)
    {
        return 0;
    }
    struct fs_inode inode;
    if (inode_read(fds[fd].inode_idx, &inode))
    {
        return -1;
    }

    uint32_t new_end = fds[fd].offset + len;
    uint32_t need_sects = (new_end + 511) / 512;

    if (need_sects > inode.block_count)
    {
        if (inode.block_count == 0)
        {
            /* First allocation for this file. */
            int rel = bm_alloc(need_sects);
            if (rel < 0)
            {
                return -1;
            }
            inode.start_block = (uint32_t)(FS_DATA_START + rel);
            inode.block_count = need_sects;
        }
        else
        {
            /*
             * Bug fix: grow an existing file by allocating a fresh contiguous
             * run of the required size, copying existing data, then freeing
             * the old blocks.  This replaces the hard return -1 that made any
             * write past the initial allocation silently fail.
             */
            int new_rel = bm_alloc(need_sects);
            if (new_rel < 0)
            {
                return -1;
            }

            /* Copy existing sectors to the new location. */
            uint8_t sector_buf[512];
            for (uint32_t s = 0; s < inode.block_count; s++)
            {
                if (ata_read_sector(inode.start_block + s, sector_buf))
                {
                    /* Free the newly-allocated blocks and bail. */
                    for (uint32_t k = 0; k < need_sects; k++)
                    {
                        bm_clr((uint32_t)new_rel + k);
                    }
                    return -1;
                }
                if (ata_write_sector((uint32_t)(FS_DATA_START + new_rel) + s, sector_buf))
                {
                    for (uint32_t k = 0; k < need_sects; k++)
                    {
                        bm_clr((uint32_t)new_rel + k);
                    }
                    return -1;
                }
            }

            /* Release old blocks. */
            uint32_t old_rel = inode.start_block - FS_DATA_START;
            for (uint32_t j = 0; j < inode.block_count; j++)
            {
                bm_clr(old_rel + j);
            }

            inode.start_block = (uint32_t)(FS_DATA_START + new_rel);
            inode.block_count = need_sects;
        }

        /*
         * Bug fix: write the updated inode (with new block allocation) back
         * to disk before writing data.  Previously, if inode_write failed
         * here the fd stayed open but the inode on disk had no block
         * allocation -- leaving the fd in an inconsistent state.  We now
         * close the fd and return an error if the inode write fails.
         */
        if (inode_write(fds[fd].inode_idx, &inode))
        {
            fds[fd].inode_idx = -1; /* force-close the broken fd */
            return -1;
        }
    }

    uint8_t sb[512];
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < len)
    {
        uint32_t si = (fds[fd].offset + done) / 512;
        uint32_t so = (fds[fd].offset + done) % 512;
        uint32_t tc = 512 - so;
        if (tc > len - done)
        {
            tc = len - done;
        }
        uint32_t lba = inode.start_block + si;
        if (so != 0 || tc != 512)
        {
            if (ata_read_sector(lba, sb))
            {
                return -1;
            }
        }
        mem_copy(sb + so, in + done, (int)tc);
        if (ata_write_sector(lba, sb))
        {
            return -1;
        }
        done += tc;
    }
    fds[fd].offset += done;
    if (fds[fd].offset > inode.size)
    {
        inode.size = fds[fd].offset;
    }
    inode_write(fds[fd].inode_idx, &inode);
    return (int)done;
}

/* ------------------------------------------------------------------ */
/* fs_delete                                                           */
/* ------------------------------------------------------------------ */
int fs_delete(const char *path)
{
    int idx = fs_find(path);
    if (idx < 0)
    {
        return -1;
    }
    struct fs_inode inode;
    if (inode_read(idx, &inode))
    {
        return -1;
    }
    if (inode.type == FS_TYPE_DIR)
    {
        return -1;
    }
    if (inode.block_count)
    {
        uint32_t rel = inode.start_block - FS_DATA_START;
        for (uint32_t j = 0; j < inode.block_count; j++)
        {
            bm_clr(rel + j);
        }
    }
    mem_zero(&inode, sizeof(inode));
    return inode_write(idx, &inode);
}

/* ------------------------------------------------------------------ */
/* fs_stat / fs_inode_count                                           */
/* ------------------------------------------------------------------ */
int fs_stat(int idx, char *name_out, uint32_t *size_out, uint8_t *type_out)
{
    struct fs_inode inode;
    if (inode_read(idx, &inode))
    {
        return -1;
    }
    if (inode.type == FS_TYPE_FREE)
    {
        return -1;
    }
    str_copy(name_out, inode.name, FS_NAME_MAX);
    *size_out = inode.size;
    *type_out = inode.type;
    return 0;
}

int fs_inode_count(void)
{
    return FS_MAX_INODES;
}