# PG 内核课程作业

**姓名：孙翌程　学号：2601210437**

本仓库包含两次作业的完整代码：

| 目录 | 对应作业 | 内容 |
|------|----------|------|
| `大作业2_smgr/` | 大作业二 | 实现 `smgr.c` 常用接口，以本地块设备为存储空间 |
| `课1-2_链表搜索/` | 第1-2课开发分析题 | 链表搜索程序 + `perf_event_open()` 统计指令数/周期数/访存次数/L1命中率 |

---

## 一、大作业二：smgr 接口 + 本地块设备存储管理器

### 1. 作业要求

- 实现 `smgr.c` 中最基础的接口，至少包含：`smgr_init`、`smgr_open`、`smgr_create`、`smgr_read`、`smgr_write`、`smgr_close`。
- 以本地块设备为存储空间（不需要分布式）。

### 2. 实现内容

在 `大作业2_smgr/` 下：

- `smgr_blockdevice.h` —— 定义了与 PostgreSQL `f_smgr` 一致的存储管理器接口，以及与 `smgr.c` 同名的顶层接口函数。
- `smgr_blockdevice.c` —— 核心实现，全部接口如下：
  - `smgr_init` / `smgr_shutdown`：打开/关闭块设备，初始化/落盘超级块与空闲位图
  - `smgr_create`：创建关系（在关系目录登记）
  - `smgr_open` / `smgr_close`：打开/关闭关系
  - `smgr_read` / `smgr_write`：按块读取/写入（`pread`/`pwrite`，块对齐）
  - `smgr_extend`：扩展关系、分配新块
  - `smgr_nblocks`、`smgr_exists`、`smgr_unlink`、`smgr_truncate`
- `test_smgr.c` —— 测试程序，演示创建关系→写块→读回校验→截断→删除的全流程。
- `Makefile` —— 一键编译。

### 3. 设备布局

把"块设备"看成一个线性的、按 8KB 块寻址的空间：

```
块 0    : 超级块（魔数 + 设备总块数 + 关系目录）
块 1    : 空闲块位图（每 bit 对应一个块）
块 2 起 : 数据块，供各关系使用
```

所有读写都以块为单位、块对齐地进行，完全绕过文件系统"文件"的概念，模拟直接操作块设备。

### 4. 如何创建本地虚拟块设备

在 Linux 上，可以用 `dd` + `losetup` 创建一个虚拟块设备：

```bash
# 创建一个 64MB 的镜像文件
dd if=/dev/zero of=disk.img bs=1M count=64

# 挂成回环块设备 /dev/loop0（需要 root）
sudo losetup /dev/loop0 disk.img
```

也可以直接用一个普通文件来模拟块设备（本实现既支持真实块设备，也支持普通文件，二者用法相同，都是把路径传给 `smgr_set_device_path`）。

### 5. 编译与运行

```bash
cd 大作业2_smgr
make            # 编译
./test_smgr     # 运行测试（默认设备 ./disk.img）
make clean      # 清理
```

运行输出示例：

```
[1] 创建关系 relfilenode=10001 完成
[2] 写入 3 个块完成，当前块数 nblocks = 3
[3] 读取并校验：全部通过 ✔
[4] smgr_exists = true, nblocks = 3
[5] 截断到 1 个块后 nblocks = 1
[6] 删除关系完成，smgr_exists = false
```

### 6. 如何接入真实 PostgreSQL

在 PostgreSQL 源码中，`src/backend/storage/smgr/smgr.c` 里的顶层函数
（`smgr_init`、`smgropen`、`smgrread`、`smgrwrite` 等）并不直接操作磁盘，
而是通过一张函数指针表 `smgrsw[]`（类型为 `f_smgr`）来"多态"地调用具体实现，
默认实现是 `md.c`（磁盘文件）。

本仓库实现的就是这样一张 `f_smgr` 表（`smgr_blockdevice_sw`），
它和 `md.c` 是"平级"的另一种存储后端。接入 PG 的方式是：

1. 把 `smgr_blockdevice.c` / `.h` 放进 PG 源码树（例如放到
   `src/backend/storage/smgr/`）。
2. 在 `smgr.c` 中注册：把 `smgr_blockdevice_sw` 填进 `smgrsw[]` 数组，
   并让 `smgr_init` 等入口在需要时切换到该实现。
3. 把 `smgr_set_device_path()` 的路径指向你的块设备（如 `/dev/loop0`）。

> 说明：真实 PG 的 `f_smgr` 还有 `ForkNumber`（主 fork / FSM / VM 等）、
> `BlockNumber` 为 32 位、以及 `RelFileNodeBackend` 等参数，本原型做了
> 合理简化以便独立编译演示，但接口语义与 PG 完全一致，可直接对照移植。

---

## 二、课1-2 开发分析题：链表搜索 + perf 统计

### 1. 题目要求

- 让 AI 开发链表搜索程序，不使用 HASH 加速搜索。
- 使用 `perf_event_open()` 统计至少以下指标：指令数、周期数、访存次数、L1 Cache 命中率。

### 2. 实现内容

`课1-2_链表搜索/linked_search_perf.c`：

- 用 C 实现单向链表，结点随机（顺序）取值，纯指针逐个遍历查找，不用哈希/索引。
- 用 `perf_event_open()` 打开 4 个硬件计数器：
  - `PERF_COUNT_HW_INSTRUCTIONS` → 指令数
  - `PERF_COUNT_HW_CPU_CYCLES`   → 周期数
  - L1D 读访问次数 → 访存次数
  - L1D 读缺失次数 → 用于计算 L1 命中率
- L1 命中率 = 1 - 缺失次数 / 访问次数。

### 3. 编译与运行（Linux 环境）

`perf_event_open()` 是 Linux 特有系统调用，请在 Linux 实验机上运行：

```bash
cd 课1-2_链表搜索
make
./linked_search_perf 1000000 5   # 100 万个结点，搜索 5 次
```

> 若提示无权限，可临时执行
> `sudo sysctl kernel.perf_event_paranoid=1`（或 `echo 1 > /proc/sys/kernel/perf_event_paranoid`）。

在非 Linux 系统上，程序会自动退化为"时钟计时"模式，仍可验证链表搜索逻辑正确，但读不到硬件计数器。
