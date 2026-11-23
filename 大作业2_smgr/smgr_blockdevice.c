/*
 * smgr_blockdevice.c
 * ---------------------------------------------------------------------------
 * 大作业二：实现 smgr.c 中常用接口，以本地块设备为存储空间。
 *
 * 设计思路：
 *   把“本地块设备”（例如 /dev/loop0，或用 dd 出来的镜像文件模拟）看成
 *   一个线性的、按块寻址的存储空间，每块 8KB（BLCKSZ）。
 *
 *   设备布局：
 *     块 0  : 超级块（魔数 + 设备总块数 + 关系目录）
 *     块 1  : 空闲块位图（每个 bit 对应一个块，1 表示已占用）
 *     块 2..: 数据块，供各关系使用
 *
 *   关系（表）在设备上占一段“连续”的块区间，通过超级块里的关系目录
 *   （relfilenode -> base_block + nblocks）来记录位置。
 *
 *   所有读写都通过 pread/pwrite 以块为单位、块对齐地进行，完全绕过
 *   操作系统文件系统的“文件”概念，模拟直接操作块设备。
 *
 * 作者：孙翌程  学号：2601210437
 *
 * 说明：本实现是一个独立、可编译、可运行的演示/原型，用于展示 smgr
 *       接口的语义和块设备存储的实现方式。要接入真实 PG 时，只需按
 *       README 中“如何接入 PostgreSQL”一节，把本文件的实现注册到
 *       smgrsw[] 分发表即可。
 */
#include "smgr_blockdevice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * 全局状态
 * ----------------------------------------------------------------------- */
static int      g_dev_fd = -1;             /* 块设备文件描述符 */
static uint32_t g_dev_nblocks = 0;         /* 设备总块数 */
static uint8_t *g_bitmap = NULL;           /* 空闲块位图（内存镜像） */
static uint32_t g_bitmap_nblocks = 0;      /* 位图占用的块数 */
static char     g_device_path[512] = "disk.img"; /* 默认设备路径 */

/* 内存中的关系目录（relfilenode -> 位置），持久化在超级块中 */
RelDirEntry g_dir[MAX_RELATIONS];

/* 超级块在块 0 中的布局（偏移量，字节） */
#define SB_OFF_MAGIC        0
#define SB_OFF_NBLOCKS      4
#define SB_OFF_NRELATIONS   8
#define SB_OFF_RESERVED     12
#define SB_OFF_RELDIR       16

/* 空闲块位图固定放在块 1 */
#define BITMAP_BLOCK        1

#define SUPER_BLOCK         0

/* -------------------------------------------------------------------------
 * 小工具
 * ----------------------------------------------------------------------- */
static void
ereport_fatal(const char *msg)
{
    fprintf(stderr, "FATAL: %s (errno=%d: %s)\n", msg, errno, strerror(errno));
    exit(1);
}

/* 计算位图一共需要多少块（每块可表示 BLCKSZ*8 个块） */
static uint32_t
bitmap_nblocks_for(uint32_t total_blocks)
{
    uint64_t bits = (uint64_t) total_blocks;
    uint64_t bytes = (bits + 7) / 8;
    return (uint32_t) ((bytes + BLCKSZ - 1) / BLCKSZ);
}

static void
bitmap_set(uint32_t blockno, int used)
{
    uint32_t byte = blockno / 8;
    uint32_t bit  = blockno % 8;
    if (used)
        g_bitmap[byte] |= (uint8_t)(1u << bit);
    else
        g_bitmap[byte] &= (uint8_t)~(1u << bit);
}

static int
bitmap_get(uint32_t blockno)
{
    uint32_t byte = blockno / 8;
    uint32_t bit  = blockno % 8;
    return (g_bitmap[byte] >> bit) & 1u;
}

/* 分配一个空闲数据块，返回块号；没有空闲块返回 0 并报错 */
static uint32_t
alloc_block(void)
{
    uint32_t b;
    /* 块 0（超级块）和块 1..1+n（位图）为保留区，数据块从位图之后开始 */
    uint32_t first_data = BITMAP_BLOCK + g_bitmap_nblocks;
    for (b = first_data; b < g_dev_nblocks; b++)
    {
        if (!bitmap_get(b))
        {
            bitmap_set(b, 1);
            return b;
        }
    }
    ereport_fatal("alloc_block: 设备空间已用完");
    return 0; /* 不可达 */
}

static void
free_block(uint32_t blockno)
{
    bitmap_set(blockno, 0);
}

/* 把内存中的位图刷回设备（块 1 开始） */
static void
flush_bitmap(void)
{
    ssize_t n = pwrite(g_dev_fd, g_bitmap, (size_t) g_bitmap_nblocks * BLCKSZ,
                       (off_t) BITMAP_BLOCK * BLCKSZ);
    if (n != (ssize_t) g_bitmap_nblocks * BLCKSZ)
        ereport_fatal("flush_bitmap: 写位图失败");
}

/* 把超级块刷回设备（块 0） */
static void
flush_superblock(void)
{
    char sb[BLCKSZ];
    memset(sb, 0, sizeof(sb));

    *(uint32_t *)(sb + SB_OFF_MAGIC)      = SMGR_MAGIC;
    *(uint32_t *)(sb + SB_OFF_NBLOCKS)    = g_dev_nblocks;

    /* 统计当前关系数（目录中 relfilenode != 0 的条目） */
    uint32_t nrels = 0;
    for (int i = 0; i < MAX_RELATIONS; i++)
        if (g_dir[i].relfilenode != 0)
            nrels++;
    *(uint32_t *)(sb + SB_OFF_NRELATIONS) = nrels;
    memcpy(sb + SB_OFF_RELDIR, g_dir, sizeof(g_dir));

    ssize_t n = pwrite(g_dev_fd, sb, BLCKSZ, 0);
    if (n != BLCKSZ)
        ereport_fatal("flush_superblock: 写超级块失败");
}

/* -------------------------------------------------------------------------
 * 接口实现
 * ----------------------------------------------------------------------- */

/* 打开块设备，读取/初始化超级块与位图 */
static void
blockdev_init(void)
{
    g_dev_fd = open(g_device_path, O_RDWR | O_CREAT, 0644);
    if (g_dev_fd < 0)
        ereport_fatal("open 设备失败");

    /* 读取超级块，判断是否已经格式化 */
    char sb[BLCKSZ];
    ssize_t n = pread(g_dev_fd, sb, BLCKSZ, 0);
    bool formatted = (n == BLCKSZ && *(uint32_t *)(sb + SB_OFF_MAGIC) == SMGR_MAGIC);

    if (formatted)
    {
        g_dev_nblocks = *(uint32_t *)(sb + SB_OFF_NBLOCKS);
        memcpy(g_dir, sb + SB_OFF_RELDIR, sizeof(g_dir));
    }
    else
    {
        /* 第一次使用：把设备当作 64MB 的空间（8192 个块）格式化 */
        g_dev_nblocks = 8192;
        memset(g_dir, 0, sizeof(g_dir));
        flush_superblock();
    }

    g_bitmap_nblocks = bitmap_nblocks_for(g_dev_nblocks);
    g_bitmap = calloc((size_t) g_bitmap_nblocks, BLCKSZ);
    if (!g_bitmap)
        ereport_fatal("calloc 位图失败");

    n = pread(g_dev_fd, g_bitmap, (size_t) g_bitmap_nblocks * BLCKSZ,
              (off_t) BITMAP_BLOCK * BLCKSZ);
    if (n <= 0)
    {
        /* 位图尚未写过：全新初始化，并把保留块标记为已占用 */
        memset(g_bitmap, 0, (size_t) g_bitmap_nblocks * BLCKSZ);
        for (uint32_t b = 0; b < BITMAP_BLOCK + g_bitmap_nblocks && b < g_dev_nblocks; b++)
            bitmap_set(b, 1);
        flush_bitmap();
    }
}

/* 关闭块设备，刷盘 */
static void
blockdev_shutdown(void)
{
    if (g_dev_fd >= 0)
    {
        flush_superblock();
        flush_bitmap();
        fsync(g_dev_fd);
        close(g_dev_fd);
        g_dev_fd = -1;
    }
    free(g_bitmap);
    g_bitmap = NULL;
}

/* 在关系目录中查找关系，返回下标，找不到返回 -1 */
static int
find_relation(RelFileNode rnode)
{
    for (int i = 0; i < MAX_RELATIONS; i++)
        if (g_dir[i].relfilenode == rnode)
            return i;
    return -1;
}

/* ---- 顶层接口 ---- */

void
smgr_init(void)
{
    blockdev_init();
}

void
smgr_shutdown(void)
{
    blockdev_shutdown();
}

SMgrRelation
smgropen(RelFileNode rnode)
{
    SMgrRelation reln = calloc(1, sizeof(SMgrRelationData));
    if (!reln)
        ereport_fatal("smgropen: calloc 失败");
    reln->relnode = rnode;
    reln->fd = -1;
    reln->is_open = false;
    return reln;
}

void
smgr_open(SMgrRelation reln)
{
    int idx = find_relation(reln->relnode);
    if (idx < 0)
        ereport_fatal("smgr_open: 关系不存在");

    reln->fd = g_dev_fd;
    reln->base_block = g_dir[idx].base_block;
    reln->nblocks = g_dir[idx].nblocks;
    reln->is_open = true;
}

void
smgr_close(SMgrRelation reln)
{
    /* 设备描述符是共享的，这里只做逻辑关闭 */
    reln->fd = -1;
    reln->is_open = false;
}

void
smgr_create(SMgrRelation reln, bool isRedo)
{
    (void) isRedo;

    if (find_relation(reln->relnode) >= 0)
        ereport_fatal("smgr_create: 关系已存在");

    /* 找空闲目录项 */
    int slot = -1;
    for (int i = 0; i < MAX_RELATIONS; i++)
    {
        if (g_dir[i].relfilenode == 0)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        ereport_fatal("smgr_create: 关系目录已满");

    g_dir[slot].relfilenode = reln->relnode;
    g_dir[slot].base_block = 0;   /* 尚未分配数据块，extend 时再分配 */
    g_dir[slot].nblocks = 0;
    g_dir[slot].flags = 0;

    reln->fd = g_dev_fd;
    reln->base_block = 0;
    reln->nblocks = 0;
    reln->is_open = true;

    flush_superblock();
}

bool
smgr_exists(SMgrRelation reln)
{
    return find_relation(reln->relnode) >= 0;
}

void
smgr_unlink(RelFileNode rnode, bool isRedo)
{
    (void) isRedo;
    int idx = find_relation(rnode);
    if (idx < 0)
        return;

    /* 释放该关系占用的所有块 */
    for (uint32_t k = 0; k < g_dir[idx].nblocks; k++)
        free_block(g_dir[idx].base_block + k);

    memset(&g_dir[idx], 0, sizeof(RelDirEntry));
    flush_superblock();
    flush_bitmap();
}

void
smgr_extend(SMgrRelation reln, uint32_t blocknum, char *buffer)
{
    int idx = find_relation(reln->relnode);
    if (idx < 0)
        ereport_fatal("smgr_extend: 关系不存在");

    /* 扩展关系，直到拥有 blocknum+1 个块 */
    while (reln->nblocks < blocknum + 1)
    {
        uint32_t nb = alloc_block();
        if (reln->nblocks == 0)
            g_dir[idx].base_block = nb;   /* 第一次分配时确定起始块 */
        g_dir[idx].nblocks = reln->nblocks + 1;
        reln->base_block = g_dir[idx].base_block;
        reln->nblocks = g_dir[idx].nblocks;
    }

    /* 把 buffer 里的内容写进新块 blocknum */
    smgr_write(reln, blocknum, buffer);
    flush_superblock();
    flush_bitmap();
}

void
smgr_read(SMgrRelation reln, uint32_t blocknum, char *buffer)
{
    if (!reln->is_open)
        ereport_fatal("smgr_read: 关系未打开");
    if (blocknum >= reln->nblocks)
        ereport_fatal("smgr_read: 块号越界");

    off_t off = (off_t)(reln->base_block + blocknum) * BLCKSZ;
    ssize_t n = pread(g_dev_fd, buffer, BLCKSZ, off);
    if (n != BLCKSZ)
        ereport_fatal("smgr_read: 读块失败");
}

void
smgr_write(SMgrRelation reln, uint32_t blocknum, char *buffer)
{
    if (!reln->is_open)
        ereport_fatal("smgr_write: 关系未打开");
    if (blocknum >= reln->nblocks)
        ereport_fatal("smgr_write: 块号越界");

    off_t off = (off_t)(reln->base_block + blocknum) * BLCKSZ;
    ssize_t n = pwrite(g_dev_fd, buffer, BLCKSZ, off);
    if (n != BLCKSZ)
        ereport_fatal("smgr_write: 写块失败");
}

uint32_t
smgr_nblocks(SMgrRelation reln)
{
    return reln->nblocks;
}

void
smgr_truncate(SMgrRelation reln, uint32_t nblocks)
{
    int idx = find_relation(reln->relnode);
    if (idx < 0)
        ereport_fatal("smgr_truncate: 关系不存在");

    if (nblocks >= reln->nblocks)
        return;

    for (uint32_t k = nblocks; k < reln->nblocks; k++)
        free_block(g_dir[idx].base_block + k);

    g_dir[idx].nblocks = nblocks;
    reln->nblocks = nblocks;
    flush_superblock();
    flush_bitmap();
}

void
smgr_set_device_path(const char *path)
{
    strncpy(g_device_path, path, sizeof(g_device_path) - 1);
    g_device_path[sizeof(g_device_path) - 1] = '\0';
}

/* -------------------------------------------------------------------------
 * 接口表（对应 PG 的 f_smgr 实例，可注册到 smgrsw[]）
 * ----------------------------------------------------------------------- */
const f_smgr smgr_blockdevice_sw = {
    .smgr_init     = smgr_init,
    .smgr_shutdown = smgr_shutdown,
    .smgr_open     = smgr_open,
    .smgr_close    = smgr_close,
    .smgr_create   = smgr_create,
    .smgr_exists   = smgr_exists,
    .smgr_unlink   = smgr_unlink,
    .smgr_extend   = smgr_extend,
    .smgr_read     = smgr_read,
    .smgr_write    = smgr_write,
    .smgr_nblocks  = smgr_nblocks,
    .smgr_truncate = smgr_truncate,
};
