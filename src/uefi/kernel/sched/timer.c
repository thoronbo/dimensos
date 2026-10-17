/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 2: 调度与Syscall
 *
 * 文件: timer.c
 * 描述: PIT (8253/8254) 定时器初始化与中断处理实现
 *
 * PIT 通道 0 配置为模式 2 (速率发生器):
 *   - 输出引脚在计数到 0 时输出低电平, 然后自动重载
 *   - 产生周期性中断信号到 PIC 主片 IRQ0
 *
 * PIC (8259A) 重新初始化:
 *   - 由于 UEFI 可能已修改 PIC 状态, 我们重新初始化
 *   - IRQ0 映射到中断向量 0x20
 *   - 发送 EOI (0x20) 到主片命令端口以确认中断
 *
 * 在 UEFI 环境下, 如果系统已切换至 APIC 模式,
 * PIT/PIC 操作可能无效果, 系统仍可通过 sched_yield 调度。
 */

#include "timer.h"
#include "sched.h"
#include "../idt.h"
#include "../gdt.h"
#include "../lib/io.h"

/* ---------------------------------------------------------------
 * I/O 端口定义
 * --------------------------------------------------------------- */

/* PIT 端口 */
#define PIT_DATA0       0x40    /* 通道 0 数据端口 */
#define PIT_CMD         0x43    /* 命令寄存器 */

/* PIC 主片端口 */
#define PIC_MASTER_CMD  0x20    /* 主片命令 */
#define PIC_MASTER_DATA 0x21    /* 主片数据 */

/* PIC 从片端口 */
#define PIC_SLAVE_CMD   0xA0    /* 从片命令 */
#define PIC_SLAVE_DATA  0xA1    /* 从片数据 */

/* EOI 命令 */
#define PIC_EOI         0x20

/* ---------------------------------------------------------------
 * 全局节拍计数
 * --------------------------------------------------------------- */
static volatile uint64_t system_ticks = 0;

/* ---------------------------------------------------------------
 * 重新初始化 PIC (8259A)
 *
 * 将 PIC 配置为:
 *   - ICW1: 边沿触发, 级联模式, 需要 ICW4
 *   - ICW2: 主片基址 = 0x20, 从片基址 = 0x28
 *   - ICW3: 主片从片连接在 IRQ2, 从片级联 ID = 2
 *   - ICW4: 8086 模式
 *   - 掩码: 仅允许 IRQ0 (清除位 0)
 *
 * 注意: 如果系统已切换至 APIC, 这些操作可能被忽略。
 * --------------------------------------------------------------- */
static void pic_init(void)
{
    uint8_t mask_master, mask_slave;

    /* 保存当前掩码 */
    mask_master = inb(PIC_MASTER_DATA);
    mask_slave  = inb(PIC_SLAVE_DATA);

    /* ICW1: 初始化命令 (边沿触发, 级联, ICW4) */
    outb(PIC_MASTER_CMD, 0x11);
    outb(PIC_SLAVE_CMD,  0x11);

    /* ICW2: 中断向量基址 */
    outb(PIC_MASTER_DATA, 0x20);   /* IRQ0-7 -> 向量 0x20-0x27 */
    outb(PIC_SLAVE_DATA,  0x28);   /* IRQ8-15 -> 向量 0x28-0x2F */

    /* ICW3: 级联连接 */
    outb(PIC_MASTER_DATA, 0x04);   /* 从片连接在主片 IRQ2 (位2=1) */
    outb(PIC_SLAVE_DATA,  0x02);   /* 从片级联 ID = 2 */

    /* ICW4: 8086 模式 */
    outb(PIC_MASTER_DATA, 0x01);
    outb(PIC_SLAVE_DATA,  0x01);

    /* 恢复掩码, 但确保 IRQ0 未屏蔽 (清除位0) */
    outb(PIC_MASTER_DATA, (uint8_t)(mask_master & ~0x01));
    outb(PIC_SLAVE_DATA,  mask_slave);
}

/* ---------------------------------------------------------------
 * 发送 EOI 到 PIC 主片
 * --------------------------------------------------------------- */
static inline void pic_send_eoi(void)
{
    outb(PIC_MASTER_CMD, PIC_EOI);
}

/* ---------------------------------------------------------------
 * 初始化 PIT 通道 0
 *
 * 配置为模式 2 (速率发生器):
 *   - 命令字节: 0x34
 *     - 位7-6: 00 = 通道 0
 *     - 位5-4: 11 = 先低字节后高字节
 *     - 位3-1: 010 = 模式 2
 *     - 位0:   0 = 16位二进制计数
 *
 * divisor = PIT_BASE_FREQUENCY / hz
 * 对于 100Hz: 1193182 / 100 = 11932 (0x2E9C)
 * --------------------------------------------------------------- */
static void pit_init(uint32_t hz)
{
    uint16_t divisor;

    if (hz == 0) {
        hz = TIMER_DEFAULT_HZ;
    }

    /* 计算分频值 */
    divisor = (uint16_t)(PIT_BASE_FREQUENCY / hz);

    /* 发送命令字节: 通道0, 双字节写入, 模式2, 二进制 */
    outb(PIT_CMD, 0x34);

    /* 写入分频值 (低字节, 然后高字节) */
    outb(PIT_DATA0, (uint8_t)(divisor & 0xFF));
    outb(PIT_DATA0, (uint8_t)((divisor >> 8) & 0xFF));
}

/* ---------------------------------------------------------------
 * PIT 定时器中断处理函数 (IRQ0)
 *
 * naked 内联汇编实现:
 *   1. 保存被调用者保存的寄存器
 *   2. 调用 C 函数 timer_tick_handler()
 *   3. 恢复寄存器
 *   4. 发送 EOI 到 PIC
 *   5. iretq 返回
 * --------------------------------------------------------------- */

/* C 部分: 递增节拍计数并调用 sched_tick */
static void timer_tick_handler(void)
{
    system_ticks++;
    sched_tick();
}

/* ---------------------------------------------------------------
 * 定时器中断入口 (timer_interrupt_entry) - 文件级基本 asm 定义
 *
 * 被 IRQ0 (PIT 定时器) 触发时执行:
 *   1. 保存被调用者保存的寄存器 (r15-r12, rbx, rbp)
 *   2. 调用 timer_tick_handler() C 函数
 *   3. 恢复寄存器
 *   4. 发送 EOI 到 PIC 主片
 *   5. iretq 返回被中断的任务
 * --------------------------------------------------------------- */
__asm__(
    ".text\n"
    ".globl  timer_interrupt_entry\n"
    "timer_interrupt_entry:\n"
    /* 保存被调用者保存的寄存器 */
    "    pushq %r15\n"
    "    pushq %r14\n"
    "    pushq %r13\n"
    "    pushq %r12\n"
    "    pushq %rbx\n"
    "    pushq %rbp\n"

    /* 调用 C 处理函数 */
    "    call  timer_tick_handler\n"

    /* 恢复寄存器 */
    "    popq  %rbp\n"
    "    popq  %rbx\n"
    "    popq  %r12\n"
    "    popq  %r13\n"
    "    popq  %r14\n"
    "    popq  %r15\n"

    /* 发送 EOI 到主片 PIC (端口 0x20) */
    "    movb  $0x20, %al\n"
    "    outb  %al, $0x20\n"

    /* 返回中断 */
    "    iretq\n"
);

/* ---------------------------------------------------------------
 * 初始化 PIT 定时器
 *
 * 1. 重新初始化 PIC (确保 IRQ0 可路由)
 * 2. 配置 PIT 通道 0 (指定频率的模式 2)
 * 3. 注册 IRQ0 中断处理函数到 IDT
 *
 * hz - 期望的中断频率 (Hz), 0 表示使用默认值 (100Hz)
 * --------------------------------------------------------------- */
void timer_init(uint32_t hz)
{
    /* 重新初始化 PIC */
    pic_init();

    /* 初始化 PIT */
    pit_init(hz);

    /* 注册 IRQ0 中断处理函数到 IDT 向量 0x20 */
    extern void timer_interrupt_entry(void);

    idt_register_handler(
        IRQ0_VECTOR,
        (uint64_t)(uintptr_t)timer_interrupt_entry,
        UEFI_CS,
        0x8E,   /* Present | Ring0 | Interrupt Gate */
        0       /* IST = 0 */
    );
}

/* ---------------------------------------------------------------
 * 获取当前系统节拍计数
 * --------------------------------------------------------------- */
uint64_t timer_get_ticks(void)
{
    return system_ticks;
}
