/*
 * test_smgr.c
 * ---------------------------------------------------------------------------
 * 大作业二的测试程序：演示如何使用 smgr 接口在“本地块设备”上创建关系、
 * 扩展块、写入、读取、校验，最后删除关系。
 *
 * 编译：make
 * 运行：./test_smgr [设备路径]      （默认 ./disk.img）
 *
 * 作者：孙翌程  学号：2601210437
 */
#include "smgr_blockdevice.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 测试用的关系号 */
#define TEST_RELNODE    10001

static char g_page[BLCKSZ];

int
main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "disk.img";
    smgr_set_device_path(path);

    printf("=== 测试开始：设备 = %s ===\n", path);

    /* 1. 初始化存储管理器（打开块设备，读/格式化超级块与位图） */
    smgr_init();

    /* 2. 创建一个关系 */
    SMgrRelation reln = smgropen(TEST_RELNODE);
    smgr_create(reln, false);
    printf("[1] 创建关系 relfilenode=%u 完成\n", TEST_RELNODE);

    /* 3. 写 3 个块（每个块 8KB），块内容用可校验的模式填充 */
    for (uint32_t b = 0; b < 3; b++)
    {
        memset(g_page, 0, BLCKSZ);
        /* 每个字节填 (blocknum * 13 + i % 256)，方便校验 */
        for (int i = 0; i < BLCKSZ; i++)
            g_page[i] = (char)((b * 13 + i) & 0xff);
        smgr_extend(reln, b, g_page);
    }
    printf("[2] 写入 3 个块完成，当前块数 nblocks = %u\n", smgr_nblocks(reln));

    /* 4. 读回来并校验 */
    int ok = 1;
    for (uint32_t b = 0; b < 3; b++)
    {
        memset(g_page, 0, BLCKSZ);
        smgr_read(reln, b, g_page);
        for (int i = 0; i < BLCKSZ; i++)
        {
            char expect = (char)((b * 13 + i) & 0xff);
            if (g_page[i] != expect)
            {
                ok = 0;
                printf("  !!! 块 %u 偏移 %d 校验失败：期望 %d 实际 %d\n",
                       b, i, (int)(unsigned char)expect, (int)(unsigned char)g_page[i]);
                break;
            }
        }
    }
    printf("[3] 读取并校验：%s\n", ok ? "全部通过 ✔" : "存在错误 ✘");

    /* 5. 检查 exists / nblocks */
    printf("[4] smgr_exists = %s, nblocks = %u\n",
           smgr_exists(reln) ? "true" : "false", smgr_nblocks(reln));

    /* 6. 截断到 1 个块，再验证 nblocks */
    smgr_truncate(reln, 1);
    printf("[5] 截断到 1 个块后 nblocks = %u\n", smgr_nblocks(reln));

    /* 7. 关闭、删除关系 */
    smgr_close(reln);
    smgr_unlink(TEST_RELNODE, false);
    printf("[6] 删除关系完成，smgr_exists = %s\n",
           smgr_exists(reln) ? "true" : "false");

    free(reln);

    /* 8. 关闭存储管理器（刷盘） */
    smgr_shutdown();
    printf("=== 测试结束 ===\n");
    return 0;
}
