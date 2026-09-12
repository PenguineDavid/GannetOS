/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef FS_H
#define FS_H

#include <stdint.h>

#define FS_SUPER_SECTOR 256
#define FS_INODE_START 257
#define FS_INODE_SECTORS 171
#define FS_DATA_START 428
#define FS_MAX_INODES 512
#define FS_MAGIC 0x504E4753u /* "PNGS" */

#define FS_NAME_MAX 128 /* max absolute path length stored in an inode */

struct fs_inode
{
    char name[FS_NAME_MAX]; /* full absolute path, e.g. /home/file.txt */
    uint32_t size;
    uint32_t start_block;
    uint32_t block_count;
    uint8_t type; /* FS_TYPE_FREE / FILE / DIR */
    uint8_t pad[3];
} __attribute__((packed));

#define FS_TYPE_FREE 0
#define FS_TYPE_FILE 1
#define FS_TYPE_DIR 2

struct fs_super
{
    uint32_t magic;
    uint32_t version;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t data_sectors;
    uint8_t pad[492];
} __attribute__((packed));

#define FS_MAX_OPEN 8

struct fs_fd
{
    int inode_idx;
    uint32_t offset;
};

/* Core API */
int fs_init(void);
int fs_open(const char *path, int create);
int fs_read(int fd, void *buf, uint32_t len);
int fs_write(int fd, const void *buf, uint32_t len);
void fs_close(int fd);
int fs_find(const char *path);
int fs_delete(const char *path);

/* Directory API */
int fs_mkdir(const char *path);
int fs_rmdir(const char *path);
int fs_rrmdr(const char *path); /* recursive remove directory */
int fs_isdir(const char *path);

/* fs_stat now also returns type */
int fs_stat(int idx, char *name_out, uint32_t *size_out, uint8_t *type_out);

/* Returns the parent directory of a full path into parent_out */
void fs_get_parent(const char *path, char *parent_out);

int fs_inode_count(void);

#endif