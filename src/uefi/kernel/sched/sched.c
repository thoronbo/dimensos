/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 2: 调度与Syscall
 *
 * 文件: sched.c
 * 描述: PCB管理、上下文切换（内联汇编）、协作式调度器、Syscall处理。
 *
 * 调度策略: 简单的轮转 (Round-Robin) 调度。
 * 上下文切换: 使用 switch_context 汇编函数保存/恢复寄存器。
 * Syscall入口: int 0x80 -> syscall_entry (汇编) -> syscall_handler
 *
 * 注意: switch_context 和 syscall_entry 使用文件级基本 asm 定义,
 *       以避免 GCC 在 naked 函数中的扩展 asm 处理问题。
 */

#include "sched.h"
#include "../idt.h"
#include "../gdt.h"
#include "../lib/io.h"
#include <stddef.h>

/* ---------------------------------------------------------------
 * 全局状态
 * --------------------------------------------------------------- */

/* 任务表 */
static task_t  tasks[MAX_TASKS];
static int     num_tasks = 0;

/* 当前运行任务指针 */
static task_t *current_task = NULL;

/* IDLE 循环的 RSP 保存点 (sched_run 中的 while 循环) */
static uint64_t idle_rsp = 0;

/* 是否需要重新调度 (由 sched_tick 设置) */
static volatile bool need_resched = false;

/* 下一个可用的 PID */
static uint64_t next_pid = 1;

/* ---------------------------------------------------------------
 * UEFI 控制台输出函数指针
 *
 * 由 main.c 在 sched_run 之前设置, 供 syscall SYS_WRITE 使用。
 * main.c 应将此指针设置为一个包装函数, 内部调用
 * SystemTable->ConOut->OutputString。
 * --------------------------------------------------------------- */
void (* volatile g_sched_puts)(const uint16_t *str) = NULL;

/* ---------------------------------------------------------------
 * 任务退出处理
 *
 * 当任务入口函数返回时:
 *   1. 标记任务为 TERMINATED
 *   2. 进入无限停机循环
 * --------------------------------------------------------------- */
__attribute__((noreturn)) static void task_exit_handler(void)
{
    if (current_task) {
        current_task->state = TASK_TERMINATED;
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}

/* ---------------------------------------------------------------
 * 上下文切换 (switch_context) - 文件级基本 asm 定义
 *
 * 函数原型 (在 C 中声明):
 *   void switch_context(uint64_t *old_sp, uint64_t new_sp);
 *
 * 参数传递 (x86_64 System V ABI):
 *   RDI = old_sp (输出: 保存当前 RSP 到此地址)
 *   RSI = new_sp (输入: 从此地址恢复上下文)
 *
 * 保存顺序:
 *   1. push r15, r14, r13, r12, rbx, rbp, rflags 到当前栈
 *   2. *old_sp = RSP
 * 恢复顺序:
 *   1. RSP = new_sp
 *   2. pop rflags, rbp, rbx, r12, r13, r14, r15
 *   3. ret (弹出 RIP 并跳转)
 * --------------------------------------------------------------- */
__asm__(
    ".text\n"
    ".globl  switch_context\n"
    "switch_context:\n"
    /* 保存当前任务上下文 */
    "    pushq %r15\n"
    "    pushq %r14\n"
    "    pushq %r13\n"
    "    pushq %r12\n"
    "    pushq %rbx\n"
    "    pushq %rbp\n"
    "    pushfq\n"
    "    movq  %rsp, (%rdi)\n"    /* *old_sp = RSP */

    /* 恢复新任务上下文 */
    "    movq  %rsi, %rsp\n"      /* RSP = new_sp */
    "    popfq\n"
    "    popq  %rbp\n"
    "    popq  %rbx\n"
    "    popq  %r12\n"
    "    popq  %r13\n"
    "    popq  %r14\n"
    "    popq  %r15\n"
    "    ret\n"
);

/* ---------------------------------------------------------------
 * Syscall 入口 (int 0x80 处理函数) - 文件级基本 asm 定义
 *
 * int 0x80 调用约定:
 *   RAX = 系统调用号
 *   RDI = arg1, RSI = arg2, RDX = arg3
 *
 * CPU 自动压入 (同特权级, 无栈切换):
 *   RIP, CS, RFLAGS (24 字节)
 *
 * 处理流程:
 *   1. 保存所有通用寄存器到栈
 *   2. 调用 syscall_handler(num, arg1, arg2, arg3)
 *   3. 恢复通用寄存器
 *   4. iretq 返回
 * --------------------------------------------------------------- */
__asm__(
    ".text\n"
    ".globl  syscall_entry\n"
    "syscall_entry:\n"
    /* === 保存所有通用寄存器 === */
    "    pushq %r15\n"
    "    pushq %r14\n"
    "    pushq %r13\n"
    "    pushq %r12\n"
    "    pushq %r11\n"
    "    pushq %r10\n"
    "    pushq %r9\n"
    "    pushq %r8\n"
    "    pushq %rbx\n"
    "    pushq %rcx\n"
    "    pushq %rdx\n"        /* RSP+16: orig arg3 */
    "    pushq %rsi\n"        /* RSP+8:  orig arg2 */
    "    pushq %rdi\n"        /* RSP+0:  orig arg1 */

    /* 栈布局:
     *   RSP+0:   rdi (orig arg1)
     *   RSP+8:   rsi (orig arg2)
     *   RSP+16:  rdx (orig arg3)
     *   RSP+24:  rcx
     *   ...
     *   RSP+96:  r15
     *   RSP+104: RIP  (来自 int 0x80)
     *   RSP+112: CS
     *   RSP+120: RFLAGS
     */

    /* === 调用 syscall_handler(num, arg1, arg2, arg3) === */
    "    movq  %rax, %rdi\n"      /* 1st arg: syscall num (from RAX) */
    "    movq  0(%rsp), %rsi\n"   /* 2nd arg: orig RDI (arg1) */
    "    movq  8(%rsp), %rdx\n"   /* 3rd arg: orig RSI (arg2) */
    "    movq  16(%rsp), %rcx\n"  /* 4th arg: orig RDX (arg3) */
    "    call  syscall_handler\n"

    /* === 恢复所有通用寄存器 (反向顺序) === */
    "    popq  %rdi\n"
    "    popq  %rsi\n"
    "    popq  %rdx\n"
    "    popq  %rcx\n"
    "    popq  %rbx\n"
    "    popq  %r8\n"
    "    popq  %r9\n"
    "    popq  %r10\n"
    "    popq  %r11\n"
    "    popq  %r12\n"
    "    popq  %r13\n"
    "    popq  %r14\n"
    "    popq  %r15\n"

    /* === 返回任务 === */
    "    iretq\n"
);

/* ---------------------------------------------------------------
 * 选取下一个就绪任务 (Round-Robin)
 *
 * 从当前任务的下一个位置开始搜索第一个 READY 任务。
 * 返回: 就绪任务指针, 或 NULL
 * --------------------------------------------------------------- */
static task_t *pick_next_task(void)
{
    int start;
    int i;

    if (num_tasks == 0) {
        return NULL;
    }

    if (current_task) {
        start = (int)(current_task - tasks) + 1;
    } else {
        start = 0;
    }

    /* 从 start 到末尾 */
    for (i = start; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_READY) {
            return &tasks[i];
        }
    }

    /* 从头到 start */
    for (i = 0; i < start; i++) {
        if (tasks[i].state == TASK_READY) {
            return &tasks[i];
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------
 * 初始化调度器
 * --------------------------------------------------------------- */
void sched_init(void)
{
    int i;

    for (i = 0; i < MAX_TASKS; i++) {
        tasks[i].pid     = 0;
        tasks[i].state   = TASK_TERMINATED;
        tasks[i].name[0] = '\0';
    }

    num_tasks    = 0;
    current_task = NULL;
    idle_rsp     = 0;
    need_resched = false;
    next_pid     = 1;
}

/* ---------------------------------------------------------------
 * 创建新任务
 *
 * 在任务表中分配空闲槽位, 初始化 PCB 并设置初始内核栈。
 * 初始栈布局 (从低到高地址):
 *   [rflags = 0x202]  <- context.rsp
 *   [rbp = 0]
 *   [rbx = 0]
 *   [r12 = 0]
 *   [r13 = 0]
 *   [r14 = 0]
 *   [r15 = 0]
 *   [RIP = entry]     <- ret 弹出并跳转
 *   [RIP = task_exit]  <- entry 返回后跳转
 *
 * name   - 任务名称 (最多15字符)
 * entry  - 入口函数
 * 返回: PID (>0), 失败返回 -1
 * --------------------------------------------------------------- */
int sched_create_task(const char *name, void (*entry)(void))
{
    int      i;
    task_t  *task = NULL;
    uint64_t *sp;
    size_t   name_len;

    for (i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_TERMINATED && tasks[i].pid == 0) {
            task = &tasks[i];
            break;
        }
    }

    if (!task) {
        return -1;
    }

    task->pid   = next_pid++;
    task->state = TASK_READY;

    for (name_len = 0; name[name_len] != '\0' && name_len < 15; name_len++) {
        task->name[name_len] = name[name_len];
    }
    task->name[name_len] = '\0';

    /* 设置初始栈 */
    sp = (uint64_t *)(task->stack + STACK_SIZE);

    *(--sp) = (uint64_t)task_exit_handler;   /* entry 返回后 */
    *(--sp) = (uint64_t)entry;                /* 入口函数地址 */
    *(--sp) = 0;                              /* r15 */
    *(--sp) = 0;                              /* r14 */
    *(--sp) = 0;                              /* r13 */
    *(--sp) = 0;                              /* r12 */
    *(--sp) = 0;                              /* rbx */
    *(--sp) = 0;                              /* rbp */
    *(--sp) = 0x202;                          /* rflags (IF=1) */

    task->context.rsp = (uint64_t)sp;

    num_tasks++;

    return (int)task->pid;
}

/* ---------------------------------------------------------------
 * 主动让出 CPU (协作式调度)
 *
 * 将当前任务放回就绪队列, 选取下一个就绪任务并切换。
 * 若无其他就绪任务, 直接返回。
 * --------------------------------------------------------------- */
void sched_yield(void)
{
    task_t *next;

    if (!current_task) {
        return;
    }

    next = pick_next_task();
    if (!next || next == current_task) {
        return;
    }

    current_task->state = TASK_READY;

    next->state = TASK_RUNNING;
    task_t *old = current_task;
    current_task = next;

    /* 上下文切换: 保存旧任务 RSP, 加载新任务 RSP */
    switch_context(&old->context.rsp, next->context.rsp);

    /* --- 被重新调度后从此继续 --- */
}

/* ---------------------------------------------------------------
 * 时钟中断处理
 *
 * 在 PIT 定时器中断上下文中被调用。
 * 设置 need_resched 标志供后续调度使用。
 * --------------------------------------------------------------- */
void sched_tick(void)
{
    need_resched = true;
}

/* ---------------------------------------------------------------
 * 启动调度器 (调度循环, 永不返回)
 *
 * 无限循环: 选取就绪任务 -> switch_context 切换到任务 ->
 * 任务 yield 后回到循环 -> 重复。
 * 无就绪任务时 HLT 省电。
 * --------------------------------------------------------------- */
__attribute__((noreturn)) void sched_run(void)
{
    task_t *next;

    uart_puts("SCH:run_start\n");

    while (1) {
        next = pick_next_task();
        if (next) {
            uart_puts("SCH:task=");
            uart_puts(next->name);
            uart_puts("\n");

            current_task = next;
            next->state  = TASK_RUNNING;

            /* 切换到任务 */
            switch_context(&idle_rsp, next->context.rsp);

            /* 任务 yield 后回到这里 */
            uart_puts("SCH:back=");
            uart_puts(next->name);
            uart_puts("\n");
            next->state = TASK_READY;
        } else {
            __asm__ volatile("hlt");
        }
    }
}

/* ---------------------------------------------------------------
 * 注册 Syscall 入口到 IDT
 *
 * 将 syscall_entry 函数注册到 IDT 向量 0x80。
 * 中断门属性: 0x8E (Present, Ring 0, Interrupt Gate)
 *
 * syscall_entry 在文件级 asm 中定义, 符号全局可见。
 * --------------------------------------------------------------- */
void sched_register_syscall(void)
{
    extern void syscall_entry(void);

    uart_puts("  SR_A\n");
    idt_register_handler(
        SYSCALL_VECTOR,
        (uint64_t)(uintptr_t)syscall_entry,
        UEFI_CS,
        0x8E,   /* Present | Ring0 | Interrupt Gate */
        0       /* IST = 0 */
    );
    uart_puts("  SR_B\n");
}

/* ---------------------------------------------------------------
 * 调度器查询与控制接口实现 (供 Shell 使用)
 * --------------------------------------------------------------- */

int sched_get_task_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid != 0 && tasks[i].state != TASK_TERMINATED)
            count++;
    }
    return count;
}

int sched_get_task_info(int index, uint64_t *pid, char *name, int *state)
{
    if (index < 0 || index >= MAX_TASKS || !pid || !name || !state)
        return -1;
    *pid   = tasks[index].pid;
    *state = tasks[index].state;
    int i = 0;
    while (tasks[index].name[i] && i < 15) {
        name[i] = tasks[index].name[i];
        i++;
    }
    name[i] = '\0';
    return 0;
}

int sched_kill_task(uint64_t pid)
{
    if (pid == 0) return -1;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].pid == pid) {
            if (tasks[i].state == TASK_TERMINATED)
                return -1;
            tasks[i].state = TASK_TERMINATED;
            return 0;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------
 * Syscall 处理函数 (C 语言实现)
 *
 * 被 syscall_entry 汇编入口调用, 根据 syscall 编号分发。
 *   num  - 系统调用号 (RAX)
 *   arg1 - 参数1 (RDI)
 *   arg2 - 参数2 (RSI)
 *   arg3 - 参数3 (RDX)
 *
 * 支持的 Syscall:
 *   SYS_YIELD (0) - 主动让出 CPU
 *   SYS_WRITE (1) - 输出字符串
 *   SYS_READ  (2) - 保留, 暂未实现
 * --------------------------------------------------------------- */
__attribute__((sysv_abi)) void syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg3;  /* 保留, 暂未使用 */

    switch (num) {
    case SYS_YIELD:
        sched_yield();
        break;

    case SYS_WRITE:
        if (g_sched_puts && arg1 != 0) {
            g_sched_puts((const uint16_t *)arg1);
        }
        break;

    case SYS_READ:
        /* 读取键盘输入 - 暂未实现 */
        break;

    default:
        /* 未知系统调用, 忽略 */
        uart_puts("SYS:bad=");
        {
            /* 简单输出数字 */
            char hex_buf[3];
            hex_buf[0] = "0123456789ABCDEF"[(num >> 4) & 0xF];
            hex_buf[1] = "0123456789ABCDEF"[num & 0xF];
            hex_buf[2] = '\0';
            uart_puts(hex_buf);
        }
        uart_puts("\n");
        break;
    }
}
