/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 2: 调度与Syscall
 *
 * 文件: timer.h
 * 描述: PIT (8253/8254) 定时器接口
 *       提供定时器初始化、中断处理及系统节拍功能。
 *
 * PIT 使用 I/O 端口:
 *   0x40 - 通道 0 数据端口
 *   0x43 - 模式/命令寄存器
 *
 * PIC (8259A) I/O 端口:
 *   0x20 - 主片命令
 *   0x21 - 主片数据
 *   0xA0 - 从片命令
 *   0xA1 - 从片数据
 */

#ifndef DIMENSOS_TIMER_H
#define DIMENSOS_TIMER_H

#include <stdint.h>

/* PIT 时钟频率 (Hz) */
#define PIT_BASE_FREQUENCY  1193182ULL

/* 默认节拍率 (100 Hz = 10ms 每节拍) */
#define TIMER_DEFAULT_HZ    100

/* IRQ0 中断向量号 (PIC 主片 IRQ0 映射到此向量) */
#define IRQ0_VECTOR         0x20

/* ---------------------------------------------------------------
 * 初始化 PIT 定时器
 *
 * 配置 PIT 通道 0 为模式 2 (速率发生器),
 * 设置指定频率的周期性中断。
 *
 * hz - 期望的中断频率 (默认 TIMER_DEFAULT_HZ = 100)
 *
 * 注意: 在 UEFI 环境下 PIC 可能已被初始化为 APIC 模式,
 *       如果 PIT+PIC 不可用, 此函数可能无效果。
 *       系统仍可通过 sched_yield 进行协作式调度。
 * --------------------------------------------------------------- */
void timer_init(uint32_t hz);

/* 获取当前系统节拍计数 */
uint64_t timer_get_ticks(void);

#endif /* DIMENSOS_TIMER_H */
