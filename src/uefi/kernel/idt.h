/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: idt.h
 * 描述: x86_64 中断描述符表 (IDT) 初始化接口
 *       定义中断门描述符结构及初始化函数。
 */

#ifndef DIMENSOS_IDT_H
#define DIMENSOS_IDT_H

#include <stdint.h>

/*
 * x86_64 中断门描述符结构 (16字节)
 *
 * 字节偏移  字段        说明
 *   0-1     offset_low  中断处理函数偏移 [15:0]
 *   2-3     selector    代码段选择子 (GDT_KERNEL_CODE)
 *   4       ist         Interrupt Stack Table (0-7, 0表示不使用)
 *   5       attributes  类型和属性 (0x8E = Present|Ring0|Interrupt Gate)
 *   6-7     offset_mid  中断处理函数偏移 [31:16]
 *   8-11    offset_high 中断处理函数偏移 [63:32]
 *  12-15    reserved    保留 (置零)
 */
typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

/* IDTR 结构 (10字节) - 用于 lidt 指令 */
typedef struct {
    uint16_t limit;          /* IDT 大小 - 1 */
    uint64_t base;           /* IDT 基地址   */
} __attribute__((packed)) idt_ptr_t;

/* IDT 表 (256 条目, 外部可访问用于注册处理函数) */
extern idt_entry_t idt_table[256];

/* 初始化 IDT (所有条目指向默认 handler) */
void idt_init(void);

/* 注册中断/异常处理函数到指定向量
 *
 * vector     - 中断向量号 (0-255)
 * handler    - 处理函数地址
 * selector   - 代码段选择子 (通常 GDT_KERNEL_CODE)
 * attributes - 中断门属性 (0x8E = Present|Ring0|Interrupt Gate)
 * ist        - Interrupt Stack Table (0 = 不使用)
 */
void idt_register_handler(
    uint8_t      vector,
    uint64_t     handler,
    uint16_t     selector,
    uint8_t      attributes,
    uint8_t      ist
);

#endif /* DIMENSOS_IDT_H */
