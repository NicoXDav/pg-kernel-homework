/*
 * linked_search_perf.c
 * ---------------------------------------------------------------------------
 * 课1-2 开发分析题：让 AI 开发链表搜索程序，并使用 perf_event_open()
 * 统计“链表搜索”的指标：指令数、周期数、访存次数、L1 Cache 命中率。
 *
 * 要求：不使用 HASH 加速搜索（只用指针逐个遍历链表）。
 *
 * 作者：孙翌程  学号：2601210437
 *
 * 编译（Linux）：
 *     gcc -O2 -o linked_search_perf linked_search_perf.c
 * 运行：
 *     ./linked_search_perf [结点数N] [搜索次数K]
 *
 * 说明：
 *   perf_event_open() 是 Linux 特有的系统调用，需要在 Linux 环境
 *   （课程使用的 PG 内核实验机）下运行才能读到真实的硬件计数器。
 *   在 macOS / 其它系统上编译时，会自动退化为只用时钟计时的版本，
 *   仍能验证链表搜索逻辑正确，但读不到硬件计数器。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

static long
perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}
#endif

/* ---- 单向链表结点 ---- */
typedef struct Node
{
    int             val;
    struct Node    *next;
} Node;

/*
 * 创建包含 n 个结点的单向链表，结点值依次为 0,1,...,n-1，
 * 返回头结点指针。链表结点用 malloc 逐个分配（真实指针链表）。
 */
static Node *
build_list(int n)
{
    Node *head = NULL;
    for (int i = n - 1; i >= 0; i--)
    {
        Node *p = malloc(sizeof(Node));
        if (!p)
        {
            perror("malloc");
            exit(1);
        }
        p->val = i;
        p->next = head;
        head = p;
    }
    return head;
}

static void
free_list(Node *head)
{
    while (head)
    {
        Node *t = head;
        head = head->next;
        free(t);
    }
}

/*
 * 在链表中线性查找目标值，返回查找步数（比较次数）。
 * 不使用 HASH、索引等任何加速手段，纯指针遍历。
 */
static long
search_list(Node *head, int target)
{
    long steps = 0;
    for (Node *p = head; p; p = p->next)
    {
        steps++;
        if (p->val == target)
            return steps;
    }
    return steps;
}

int
main(int argc, char **argv)
{
    int N = (argc > 1) ? atoi(argv[1]) : 1000000;   /* 结点数 */
    int K = (argc > 2) ? atoi(argv[2]) : 1;         /* 搜索次数 */
    int target = N;                                  /* 不存在的值 => 最坏情况全遍历 */

    printf("=== 链表搜索 + perf_event_open 统计 ===\n");
    printf("结点数 N = %d，搜索次数 K = %d，目标值 = %d（不存在，保证全遍历）\n",
           N, K, target);

    Node *head = build_list(N);

#ifdef __linux__
    /* ---- 打开 4 个硬件性能计数器 ---- */
    struct perf_event_attr pe;
    long fd_instructions, fd_cycles, fd_l1_access, fd_l1_miss;

    /* 1. 指令数 */
    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(pe);
    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    pe.disabled = 1;
    fd_instructions = perf_event_open(&pe, 0, -1, -1, 0);

    /* 2. 周期数 */
    pe.config = PERF_COUNT_HW_CPU_CYCLES;
    fd_cycles = perf_event_open(&pe, 0, -1, -1, 0);

    /* 3. L1D 数据缓存访问次数（访存次数） */
    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_HW_CACHE;
    pe.size = sizeof(pe);
    pe.config = PERF_COUNT_HW_CACHE_L1D |
                (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16);
    pe.disabled = 1;
    fd_l1_access = perf_event_open(&pe, 0, -1, -1, 0);

    /* 4. L1D 数据缓存缺失次数 */
    pe.config = PERF_COUNT_HW_CACHE_L1D |
                (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    fd_l1_miss = perf_event_open(&pe, 0, -1, -1, 0);

    if (fd_instructions < 0 || fd_cycles < 0 || fd_l1_access < 0 || fd_l1_miss < 0)
    {
        fprintf(stderr, "打开 perf 计数器失败（可能需要 root 权限或降低 perf_event_paranoid）\n");
        free_list(head);
        return 1;
    }

    ioctl(fd_instructions, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_cycles,       PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_l1_access,    PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_l1_miss,      PERF_EVENT_IOC_RESET, 0);
    ioctl(fd_instructions, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(fd_cycles,       PERF_EVENT_IOC_ENABLE, 0);
    ioctl(fd_l1_access,    PERF_EVENT_IOC_ENABLE, 0);
    ioctl(fd_l1_miss,      PERF_EVENT_IOC_ENABLE, 0);

    /* ---- 执行 K 次链表搜索 ---- */
    long total_steps = 0;
    for (int i = 0; i < K; i++)
        total_steps += search_list(head, target);

    ioctl(fd_instructions, PERF_EVENT_IOC_DISABLE, 0);
    ioctl(fd_cycles,       PERF_EVENT_IOC_DISABLE, 0);
    ioctl(fd_l1_access,    PERF_EVENT_IOC_DISABLE, 0);
    ioctl(fd_l1_miss,      PERF_EVENT_IOC_DISABLE, 0);

    uint64_t instructions, cycles, l1_access, l1_miss;
    read(fd_instructions, &instructions, sizeof(instructions));
    read(fd_cycles,       &cycles,       sizeof(cycles));
    read(fd_l1_access,    &l1_access,    sizeof(l1_access));
    read(fd_l1_miss,      &l1_miss,      sizeof(l1_miss));

    double hit_rate = (l1_access > 0)
                    ? (1.0 - (double) l1_miss / (double) l1_access) * 100.0
                    : 0.0;

    printf("\n--- 统计结果（K=%d 次搜索合计） ---\n", K);
    printf("指令数       : %llu\n", (unsigned long long) instructions);
    printf("周期数       : %llu\n", (unsigned long long) cycles);
    printf("访存次数     : %llu  (L1D 读访问次数)\n", (unsigned long long) l1_access);
    printf("L1 缺失次数  : %llu\n", (unsigned long long) l1_miss);
    printf("L1 Cache 命中率: %.2f%%\n", hit_rate);
    printf("比较步数合计 : %ld\n", total_steps);

    close(fd_instructions);
    close(fd_cycles);
    close(fd_l1_access);
    close(fd_l1_miss);
#else
    /* ---- 非 Linux：只用时钟计时，验证逻辑正确 ---- */
    printf("\n（非 Linux 环境，跳过 perf_event_open，改用时钟计时验证）\n");
    clock_t t0 = clock();
    long total_steps = 0;
    for (int i = 0; i < K; i++)
        total_steps += search_list(head, target);
    clock_t t1 = clock();
    double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
    printf("比较步数合计 : %ld\n", total_steps);
    printf("耗时         : %.3f 秒\n", secs);
#endif

    free_list(head);
    return 0;
}
