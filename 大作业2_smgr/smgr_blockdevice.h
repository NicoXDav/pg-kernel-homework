/*
 * smgr_blockdevice.h
 * ---------------------------------------------------------------------------
 * 大作业二：实现 smgr.c 中常用接口，以本地块设备为存储空间。
 *
 * 本文件定义了一个与 PostgreSQL 的 f_smgr 接口一致的存储管理器接口，
 * 以及对应的顶层接口函数（与 PG 源码 src/backend/storage/smgr/smgr.c 同名）。
 *
 * 作者：孙翌程  学号：2601210437
 * 说明：为了保持接口与 PG 一致，函数签名尽量贴近 PG 原版 f_smgr / smgr.c，
 *       同时做少量简化（例如去掉 ForkNumber，本实现只管理主数据 fork）。
 */
#ifndef SMGR_BLOCKDEVICE_H
#define SMGR_BLOCKDEVICE_H

#include <stdbool.h>
#include <stdint.h>

/* 块大小：与 PostgreSQL 默认一致，8KB */
#define BLCKSZ              8192

/* 设备超级块的魔数 */
#define SMGR_MAGIC          0x534D4752UL   /* "SMGR" */

/* 关系目录最多能容纳的关系数量（超级块里内联存放） */
#define MAX_RELATIONS       128

/* -------------------------------------------------------------------------
 * 关系标识（对应 PG 的 RelFileNode，这里简化为只保留 relfilenode）
 * ----------------------------------------------------------------------- */
typedef uint32_t RelFileNode;

/*
 * 关系目录中的一条记录，描述一个关系在块设备上的位置。
 * 一条记录 16 字节，直接内联存放在超级块（0 号块）中。
 */
typedef struct RelDirEntry
{
    uint32_t    relfilenode;    /* 关系号（OID） */
    uint32_t    base_block;     /* 该关系在设备上的起始数据块号 */
    uint32_t    nblocks;        /* 该关系占用的块数 */
    uint32_t    flags;          /* 保留 */
} RelDirEntry;

/*
 * 关系描述结构（对应 PG 的 SMgrRelationData / SMgrRelation）
 */
typedef struct SMgrRelationData
{
    RelFileNode relnode;        /* 关系文件号 */
    int         fd;             /* 底层块设备文件描述符 */
    uint32_t    base_block;     /* 该关系在设备上的起始数据块号 */
    uint32_t    nblocks;        /* 该关系占用的块数 */
    bool        is_open;        /* 是否已打开 */
} SMgrRelationData;

typedef SMgrRelationData *SMgrRelation;

/*
 * 存储管理器接口表（对应 PG 的 f_smgr）。
 * 每一行都是一个函数指针，smgr.c 通过这张表来“多态”地调用具体的实现。
 */
typedef struct f_smgr
{
    void        (*smgr_init)    (void);                                  /* may be NULL */
    void        (*smgr_shutdown)(void);                                  /* may be NULL */
    void        (*smgr_open)    (SMgrRelation reln);
    void        (*smgr_close)   (SMgrRelation reln);
    void        (*smgr_create)  (SMgrRelation reln, bool isRedo);
    bool        (*smgr_exists)  (SMgrRelation reln);
    void        (*smgr_unlink)  (RelFileNode rnode, bool isRedo);
    void        (*smgr_extend)  (SMgrRelation reln, uint32_t blocknum, char *buffer);
    void        (*smgr_read)    (SMgrRelation reln, uint32_t blocknum, char *buffer);
    void        (*smgr_write)   (SMgrRelation reln, uint32_t blocknum, char *buffer);
    uint32_t    (*smgr_nblocks) (SMgrRelation reln);
    void        (*smgr_truncate)(SMgrRelation reln, uint32_t nblocks);
} f_smgr;

/* -------------------------------------------------------------------------
 * 顶层接口函数（与 PG smgr.c 同名）
 * ----------------------------------------------------------------------- */
extern void smgr_init(void);
extern void smgr_shutdown(void);
extern void smgr_open(SMgrRelation reln);
extern void smgr_close(SMgrRelation reln);
extern void smgr_create(SMgrRelation reln, bool isRedo);
extern bool smgr_exists(SMgrRelation reln);
extern void smgr_unlink(RelFileNode rnode, bool isRedo);
extern void smgr_extend(SMgrRelation reln, uint32_t blocknum, char *buffer);
extern void smgr_read(SMgrRelation reln, uint32_t blocknum, char *buffer);
extern void smgr_write(SMgrRelation reln, uint32_t blocknum, char *buffer);
extern uint32_t smgr_nblocks(SMgrRelation reln);
extern void smgr_truncate(SMgrRelation reln, uint32_t nblocks);

/* 创建一个关系描述对象（对应 PG 的 smgropen） */
extern SMgrRelation smgropen(RelFileNode rnode);

/* 设置底层块设备路径（测试用；PG 中是配置项 data_directory） */
extern void smgr_set_device_path(const char *path);

/* 导出本实现对应的接口表，方便“挂到” smgrsw[] 上 */
extern const f_smgr smgr_blockdevice_sw;

#endif /* SMGR_BLOCKDEVICE_H */
