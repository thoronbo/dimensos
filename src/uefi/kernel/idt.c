/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: idt.c
 * 描述: x86_64 中断描述符表 (IDT) 实现
 *       创建 256 个中断门描述符，全部指向同一个默认中断
 *       处理函数。当未处理的中断触发时，进入死循环停机。
 */

#include "idt.h"
#include "gdt.h"
#include "lib/io.h"
#include <stddef.h>

/* IDT 条目数 (x86_64 固定 256) */
#define IDT_ENTRIES  256

/* IDT 表 (非 static, 供 sched.c 等外部模块访问) */
idt_entry_t idt_table[IDT_ENTRIES];

/* ---------------------------------------------------------------
 * 默认中断处理函数 (用于无错误码的中断/异常/IRQ)
 *
 * 对于未注册具体处理函数的中断/异常:
 *   1. 发送 EOI 到主从 PIC (避免中断阻塞)
 *   2. iretq 返回
 *
 * 注意: 此处理函数仅适用于 CPU 不推送错误码的向量,
 *       包括所有硬件 IRQ 和部分异常。
 * --------------------------------------------------------------- */
__asm__(
    ".text\n"
    ".globl  isr_default_handler\n"
    "isr_default_handler:\n"
    "    pushq %rax\n"
    "    movb  $0x20, %al\n"
    "    outb  %al, $0x20\n"    /* EOI 主 PIC */
    "    outb  %al, $0xA0\n"    /* EOI 从 PIC */
    "    popq  %rax\n"
    "    iretq\n"
);

/* ---------------------------------------------------------------
 * 默认异常处理函数 (用于有错误码的异常)
 *
 * x86_64 架构中, 以下异常 CPU 会推送错误码到栈上:
 *   向量  8  (#DF)  双故障
 *   向量 10  (#TS)  无效 TSS
 *   向量 11  (#NP)  段不存在
 *   向量 12  (#SS)  栈段故障
 *   向量 13  (#GP)  通用保护
 *   向量 14  (#PF)  页故障
 *   向量 17  (#AC)  对齐检查
 *
 * 差异: CPU 在推送错误码后才会触发 handler, 所以栈布局为:
 *   SS, RSP(if ring change), RFLAGS, CS, RIP, ErrorCode
 *
 * 此 handler 发送 EOI 后弹出错误码, 然后 iretq 返回。
 * --------------------------------------------------------------- */
__asm__(
    ".text\n"
    ".globl  isr_default_handler_err\n"
    "isr_default_handler_err:\n"
    "    pushq %rax\n"
    "    movb  $0x20, %al\n"
    "    outb  %al, $0x20\n"    /* EOI 主 PIC */
    "    outb  %al, $0xA0\n"    /* EOI 从 PIC */
    "    popq  %rax\n"
    "    addq  $8, %rsp\n"      /* 弹出错误码 */
    "    iretq\n"
);

/* ---------------------------------------------------------------
 * 设置单个 IDT 条目
 *
 * entry     - IDT 条目指针
 * handler   - 中断处理函数地址
 * selector  - 代码段选择子 (GDT_KERNEL_CODE)
 * attributes - 中断门属性 (0x8E)
 * ist       - Interrupt Stack Table (0 = 不使用)
 * --------------------------------------------------------------- */
static void idt_set_entry(
    idt_entry_t *entry,
    uint64_t     handler,
    uint16_t     selector,
    uint8_t      attributes,
    uint8_t      ist
    )
{
    entry->offset_low  = handler & 0xFFFF;
    entry->selector    = selector;
    entry->ist         = ist & 0x07;
    entry->attributes  = attributes;
    entry->offset_mid  = (handler >> 16) & 0xFFFF;
    entry->offset_high = (handler >> 32) & 0xFFFFFFFF;
    entry->reserved    = 0;
}

/* ---------------------------------------------------------------
 * IDT 初始化
 *
 * 1. 将所有 256 个 IDT 条目指向对应默认 handler
 * 2. 有错误码的异常使用 isr_default_handler_err
 * 3. 使用 lidt 加载 IDTR
 *
 * 注意: 所有条目 IST=0, 不配置 TSS。
 *       如需 IST 需在 gdt_init 中设置 TSS 描述符和 IST 栈。
 * --------------------------------------------------------------- */
void idt_init(void)
{
    idt_ptr_t  idt_ptr;
    uint64_t   handler_addr, handler_addr_err;
    size_t     i;

    /* 获取默认处理函数的地址 (文件级 asm, 直接 extern) */
    extern void isr_default_handler(void);
    extern void isr_default_handler_err(void);
    handler_addr     = (uint64_t)(uintptr_t)isr_default_handler;
    handler_addr_err = (uint64_t)(uintptr_t)isr_default_handler_err;

    /* 有错误码的异常向量（#DF, #TS, #NP, #SS, #GP, #PF, #AC） */
    static const uint8_t err_code_vectors[] = { 8, 10, 11, 12, 13, 14, 17 };

    /* 设置所有 256 个中断门描述符 */
    for (i = 0; i < IDT_ENTRIES; i++) {
        /* 选择正确的 handler: 有错误码的异常用 err 版本 */
        uint64_t addr = handler_addr;
        for (size_t ei = 0; ei < sizeof(err_code_vectors); ei++) {
            if (i == err_code_vectors[ei]) {
                addr = handler_addr_err;
                break;
            }
        }

        idt_set_entry(
            &idt_table[i],
            addr,
            UEFI_CS,            /* 选择子 (UEFI GDT 的 CS=0x38) */
            0x8E,               /* attributes: Present|Ring0|Interrupt Gate */
            0                   /* IST = 0 (未配置 TSS, 不使用 IST) */
            );
    }

    /* 填充 IDTR */
    idt_ptr.limit = sizeof(idt_table) - 1;
    idt_ptr.base  = (uint64_t)(uintptr_t)idt_table;

    /* 加载 IDT */
    __asm__ volatile("lidt %[ptr]" : : [ptr] "m"(idt_ptr) : "memory");
}



/* ---------------------------------------------------------------
 * 注册中断/异常处理函数到指定向量
 *
 * 覆盖指定向量号对应的 IDT 条目, 指向自定义处理函数。
 * 用于注册 syscall 入口 (向量 0x80) 和 IRQ 处理函数。
 * --------------------------------------------------------------- */
void idt_register_handler(
    uint8_t      vector,
    uint64_t     handler,
    uint16_t     selector,
    uint8_t      attributes,
    uint8_t      ist
    )
{
    if (vector >= IDT_ENTRIES) {
        return;  /* 无效向量号 */
    }

    idt_set_entry(
        &idt_table[vector],
        handler,
        selector,
        attributes,
        ist
    );
}
