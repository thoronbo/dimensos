/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: gdt.h
 * 描述: x86_64 全局描述符表 (GDT) 初始化接口
 *       提供长模式下的代码段和数据段描述符定义。
 */

#ifndef DIMENSOS_GDT_H
#define DIMENSOS_GDT_H

#include <stdint.h>

/* GDT 段选择子 */
#define GDT_KERNEL_CODE  0x08  /* Ring 0 代码段 (自研 GDT) */
#define GDT_KERNEL_DATA  0x10  /* Ring 0 数据段 (自研 GDT) */

/*
 * UEFI CS 选择子 (0x38)
 *
 * 在 Phase 3 跳过了 gdt_init()，仍使用 UEFI 固件的 GDT。
 * UEFI x64 固件的 64 位代码段选择子为 0x38。
 * 所有 IDT 条目必须使用此值，否则中断触发时 CPU 用错误的选择子
 * 索引 UEFI GDT → #GP。
 */
#define UEFI_CS         0x38

/* 初始化 GDT 并加载到处理器 */
void gdt_init(void);

#endif /* DIMENSOS_GDT_H */
