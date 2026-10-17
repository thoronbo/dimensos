/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 2: 调度与Syscall
 *
 * 文件: sched.h
 * 描述: 进程控制块 (PCB) 结构、调度器接口、Syscall编号定义。
 *       提供任务创建、上下文切换、调度控制及系统调用功能。
 */

#ifndef DIMENSOS_SCHED_H
#define DIMENSOS_SCHED_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------
 * 常量定义
 * --------------------------------------------------------------- */

/* 最大任务数 */
#define MAX_TASKS       8

/* 每个任务的内核栈大小 (4KB) */
#define STACK_SIZE      4096

/* 默认时间片 (10ms) */
#define TIME_SLICE_MS   10

/* Syscall 软中断向量号 */
#define SYSCALL_VECTOR  0x80

/* ---------------------------------------------------------------
 * 任务状态枚举
 * --------------------------------------------------------------- */
typedef enum {
    TASK_READY       = 0,
    TASK_RUNNING     = 1,
    TASK_BLOCKED     = 2,
    TASK_TERMINATED  = 3
} task_state_t;

/* ---------------------------------------------------------------
 * 寄存器上下文结构
 *
 * 保存顺序与 switch_context 汇编中的压栈/出栈顺序一致：
 *   push: r15, r14, r13, r12, rbx, rbp, rflags
 *   pop:  rflags, rbp, rbx, r12, r13, r14, r15
 *   ret:  RIP
 *
 * 栈布局 (从低地址到高地址):
 *   [rflags] <- context.rsp 指向这里 (第一个被弹出)
 *   [rbp]
 *   [rbx]
 *   [r12]
 *   [r13]
 *   [r14]
 *   [r15]
 *   [RIP]   <- ret 弹出此地址并跳转
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rflags;
    uint64_t rip, rsp;     /* rip=入口/恢复地址, rsp=内核栈指针 */
} context_t;

/* ---------------------------------------------------------------
 * 进程控制块 (PCB)
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t    pid;                 /* 进程ID */
    task_state_t state;              /* 任务状态 */
    context_t   context;             /* 保存的寄存器上下文 */
    uint8_t     stack[STACK_SIZE];   /* 内核栈 (4KB) */
    char        name[16];           /* 任务名称 */
} task_t;

/* ---------------------------------------------------------------
 * Syscall 编号
 * --------------------------------------------------------------- */
#define SYS_YIELD   0   /* 主动让出CPU */
#define SYS_WRITE   1   /* 输出字符串 */
#define SYS_READ    2   /* 读取键盘输入 */

/* ---------------------------------------------------------------
 * 调度器接口
 * --------------------------------------------------------------- */

/* 初始化调度器 (清零任务表) */
void sched_init(void);

/* 创建一个新任务
 *   name  - 任务名称
 *   entry - 入口函数指针
 * 返回: 任务 PID, 失败返回 -1 */
int sched_create_task(const char *name, void (*entry)(void));

/* 主动让出CPU (协作式调度) */
void sched_yield(void);

/* 时钟中断处理 (标记需要重新调度, 在中断上下文中调用) */
void sched_tick(void);

/* 启动调度器 (进入调度循环, 不再返回) */
void sched_run(void);

/* ---------------------------------------------------------------
 * 调度器查询与控制接口 (供 Shell ps/kill 使用)
 * --------------------------------------------------------------- */

/* 获取任务总数 */
int sched_get_task_count(void);

/* 获取指定索引的任务信息
 *   index - 0..MAX_TASKS-1
 *   pid   - 输出: 进程ID (0=空槽)
 *   name  - 输出: 任务名称
 *   state - 输出: 任务状态值
 * 返回: 0=成功, -1=无效索引 */
int sched_get_task_info(int index, uint64_t *pid, char *name, int *state);

/* 终止指定 PID 的任务
 *   返回: 0=成功, -1=未找到或不可终止 */
int sched_kill_task(uint64_t pid);

/* ---------------------------------------------------------------
 * Syscall 处理函数
 *
 * 被 syscall_entry (int 0x80 入口) 调用
 *   num  - 系统调用号 (来自 RAX)
 *   arg1 - 参数1 (来自 RDI)
 *   arg2 - 参数2 (来自 RSI)
 *   arg3 - 参数3 (来自 RDX)
 * --------------------------------------------------------------- */
__attribute__((sysv_abi)) void syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3);

/* ---------------------------------------------------------------
 * 上下文切换 (汇编实现, naked函数)
 *
 *   old_sp - 输出参数, 保存当前 RSP 到此地址
 *   new_sp - 输入参数, 加载此值为新 RSP 并恢复上下文
 *
 * 当切换到新任务时, 此函数不返回 (RSP已改变);
 * 当被切换回时, 它会从之前的保存点继续执行。
 * --------------------------------------------------------------- */
__attribute__((sysv_abi)) void switch_context(uint64_t *old_sp, uint64_t new_sp);

/* ---------------------------------------------------------------
 * Syscall 注册
 * --------------------------------------------------------------- */

/* 注册 syscall 入口到 IDT 向量 0x80
 * 在 sched_run 之前调用 */
void sched_register_syscall(void);

/* ---------------------------------------------------------------
 * UEFI 控制台输出函数指针
 *
 * 由 main.c 在初始化时设置, 供 syscall SYS_WRITE 使用。
 * 参数: null 结尾的 UTF-16 宽字符串
 * --------------------------------------------------------------- */
extern void (* volatile g_sched_puts)(const uint16_t *str);

/* ---------------------------------------------------------------
 * Syscall 内联包装函数
 *
 * 提供类型安全的接口供任务代码调用系统调用。
 * 使用 int $0x80 触发系统调用, 参数通过寄存器传递:
 *   EAX = 系统调用号
 *   RDI = arg1, RSI = arg2, RDX = arg3
 * --------------------------------------------------------------- */

/* SYS_YIELD: 主动让出 CPU */
static inline void sys_yield(void)
{
    /* 可能触发上下文切换, 所有寄存器都可能被改变 */
    __asm__ volatile(
        "movl   %[num], %%eax\n"
        "int    $0x80\n"
        :
        : [num] "i"(SYS_YIELD)
        : "eax", "rdi", "rsi", "rdx", "rcx", "rbx",
          "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
          "memory"
    );
}

/* SYS_WRITE: 输出 UTF-16 字符串到控制台
 * str - null 结尾的宽字符串
 *
 * 注意: sys_write 不触发上下文切换, syscall_entry 会保存/恢复
 * 所有通用寄存器。此内联汇编仅直接修改 rax 和 rdi, 无需全量 clobber。
 */
static inline void sys_write(const uint16_t *str)
{
    __asm__ volatile(
        "movq   %[str], %%rdi\n"
        "movl   %[num], %%eax\n"
        "int    $0x80\n"
        :
        : [num] "i"(SYS_WRITE),
          [str] "r"((uint64_t)(uintptr_t)str)
        : "rax", "rdi", "memory"
    );
}

#endif /* DIMENSOS_SCHED_H */
