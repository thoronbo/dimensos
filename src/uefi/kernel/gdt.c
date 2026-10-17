/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: gdt.c
 * 描述: x86_64 全局描述符表 (GDT) 实现
 *       构建并加载长模式 GDT，包含 NULL 描述符、
 *       内核代码段 (Ring 0) 和 内核数据段 (Ring 0)。
 *
 * GDT 布局:
 *   索引0: NULL 描述符 (必须为零)
 *   索引1: 内核代码段 (Ring 0, 64位代码, 基址0, 限长0)
 *   索引2: 内核数据段 (Ring 0, 数据, 基址0, 限长0)
 */

#include "gdt.h"
#include "pmm.h"
#include "lib/io.h"

/* ---------------------------------------------------------------
 * x86_64 GDT 描述符结构 (8字节)
 * ---------------------------------------------------------------
 * 位域布局:
 *   字节0-1: Limit [15:0]
 *   字节2-3: Base  [23:0]
 *   字节4:   Access Byte
 *   字节5:   Flags(高4位) | Limit[19:16](低4位)
 *   字节6-7: Base  [63:24] (64位模式下高32位为0)
 * --------------------------------------------------------------- */
typedef struct {
    uint16_t limit_low;      /* Limit [15:0]   */
    uint16_t base_low;       /* Base  [23:0]   */
    uint8_t  base_mid;       /* Base  [31:24]  */
    uint8_t  access;         /* Access Byte    */
    uint8_t  flags_limit_hi; /* Flags(高4位) | Limit[19:16](低4位) */
    uint8_t  base_high;      /* Base  [39:32]  (x86_64中为0) */
} __attribute__((packed)) gdt_entry_t;

/* GDT 系统段描述符 (TSS, 16字节) */
typedef struct {
    uint16_t limit_low;      /* Limit [15:0] */
    uint16_t base_low;       /* Base  [15:0] */
    uint8_t  base_mid;       /* Base  [23:16] */
    uint8_t  access;         /* Access Byte */
    uint8_t  flags_limit_hi; /* Flags + Limit[19:16] */
    uint8_t  base_31_24;     /* Base  [31:24] */
    uint32_t base_63_32;     /* Base  [63:32] */
    uint32_t reserved;       /* 保留 */
} __attribute__((packed)) gdt_system_entry_t;

/* GDTR 结构 (6字节) - 用于 lgdt 指令 */
typedef struct {
    uint16_t limit;          /* GDT 大小 - 1 */
    uint64_t base;           /* GDT 基地址   */
} __attribute__((packed)) gdt_ptr_t;

/* x86_64 TSS 结构 (104 字节) */
typedef struct {
    uint32_t reserved1;
    uint64_t rsp0;           /* Ring 0 栈指针 */
    uint64_t rsp1;           /* Ring 1 栈指针 */
    uint64_t rsp2;           /* Ring 2 栈指针 */
    uint64_t reserved2;
    uint64_t ist1;           /* IST 表 1 */
    uint64_t ist2;           /* IST 表 2 */
    uint64_t ist3;           /* IST 表 3 */
    uint64_t ist4;           /* IST 表 4 */
    uint64_t ist5;           /* IST 表 5 */
    uint64_t ist6;           /* IST 表 6 */
    uint64_t ist7;           /* IST 表 7 */
    uint64_t reserved3;
    uint16_t reserved4;
    uint16_t iopb_offset;    /* I/O 权限位图偏移 */
} __attribute__((packed)) tss_t;

/* GDT 表项数: NULL + KernelCode + KernelData + 5个UEFI兼容数据段 + TSS(16字节=2项) */
#define GDT_ENTRIES  10

/* 长模式 GDT */
static gdt_entry_t gdt_table[GDT_ENTRIES];

/* TSS 结构 */
static tss_t tss_table;

/* ---------------------------------------------------------------
 * 构建单个 GDT 描述符
 * 在 x86_64 长模式下，基址和限长被忽略 (强制为 0 和 0xFFFFF)，
 * 但仍需正确设置 Access 和 Flags。
 * --------------------------------------------------------------- */
static void gdt_set_entry(
    gdt_entry_t *entry,
    uint32_t     base,
    uint32_t     limit,
    uint8_t      access,
    uint8_t      flags
    )
{
    entry->limit_low      = limit & 0xFFFF;
    entry->base_low       = base & 0xFFFF;
    entry->base_mid       = (base >> 16) & 0xFF;
    entry->access         = access;
    entry->flags_limit_hi = (flags << 4) | ((limit >> 16) & 0x0F);
    entry->base_high      = (base >> 24) & 0xFF;
}

/* ---------------------------------------------------------------
 * 构建 GDT 系统段描述符 (TSS)
 * --------------------------------------------------------------- */
static void gdt_set_tss_entry(
    gdt_system_entry_t *entry,
    uint64_t           base,
    uint32_t           limit,
    uint8_t            access
    )
{
    entry->limit_low      = limit & 0xFFFF;
    entry->base_low       = base & 0xFFFF;
    entry->base_mid       = (base >> 16) & 0xFF;
    entry->access         = access;
    entry->flags_limit_hi = 0x00 | ((limit >> 16) & 0x0F);
    entry->base_31_24     = (base >> 24) & 0xFF;
    entry->base_63_32     = (base >> 32) & 0xFFFFFFFF;
    entry->reserved       = 0;
}

/* ---------------------------------------------------------------
 * GDT 初始化
 *
 * 在 UEFI 环境下:
 *   - UEFI 已经在长模式下运行，有自己的 GDT 和段寄存器设置
 *   - 只需要加载新的 GDT 表，不需要进行段重载（lretq）
 *   - 在退出 Boot Services 后，此 GDT 将用于内核运行
 *
 * 步骤:
 *   1. 构建 GDT 表 (NULL + 内核代码 + 内核数据)
 *   2. 通过 lgdt 加载 GDTR
 *   3. 不进行段重载 (UEFI 已经设置好)
 * --------------------------------------------------------------- */
void gdt_init(void)
{
    gdt_ptr_t gdt_ptr;
    int       i;

    uart_puts("[GDT] start\n");

    /* 索引 0: NULL 描述符 (全零) */
    gdt_table[0] = (gdt_entry_t){0, 0, 0, 0, 0, 0};
    uart_puts("[GDT] null done\n");

    /*
     * 索引 1: 内核代码段 (Ring 0, 64位代码)
     *   access = 0x9A => Present=1, Ring=00, S=1(代码/数据),
     *                     Type=1010 (代码, 执行/读)
     *   flags  = 0xA  => Gr=1(4KB粒度), L=1(64位模式),
     *                     D/B=0(忽略), AVL=0
     */
    uart_puts("[GDT] set code seg\n");
    gdt_set_entry(
        &gdt_table[1],
        0,           /* 基址 (长模式下忽略) */
        0xFFFFF,     /* 限长 (长模式下忽略) */
        0x9A,        /* Access: Present|Ring0|Code|Exec/Read */
        0xA          /* Flags:  Gr=1, L=1 (64位长模式) */
        );

    /*
     * 索引 2: 内核数据段 (Ring 0, 数据)
     *   access = 0x92 => Present=1, Ring=00, S=1(代码/数据),
     *                     Type=0010 (数据, 读/写)
     *   flags  = 0xC  => Gr=1(4KB粒度), D/B=1(32位,长模式下忽略),
     *                     L=0, AVL=0
     */
    uart_puts("[GDT] set data seg\n");
    gdt_set_entry(
        &gdt_table[2],
        0,
        0xFFFFF,
        0x92,        /* Access: Present|Ring0|Data|Read/Write */
        0xC          /* Flags:  Gr=1, D/B=1 */
        );

    /*
     * 索引 3-6: UEFI 兼容数据段 (选择子 0x18, 0x20, 0x28, 0x30)
     *
     * UEFI 固件在 x64 模式下使用多个数据段选择子 (例如 FS/GS 用于
     * TLS/运行时数据)。加载自研 GDT 后这些选择子不能超出 GDT 范围，
     * 否则触发 #GP。此处预置 Ring 0 数据段覆盖这些选择子。
     */
    uart_puts("[GDT] uefi compat segs\n");
    for (i = 3; i < 7; i++) {
        gdt_set_entry(
            &gdt_table[i],
            0,
            0xFFFFF,
            0x92,        /* Present|Ring0|Data|Read/Write */
            0xC          /* Gr=1, D/B=1 */
        );
    }

    /*
     * 索引 7: UEFI 代码段 (选择子 0x38)
     *
     * UEFI 64 位代码段选择子为 0x38 = 索引 7。IDT 条目使用 UEFI_CS=0x38
     * 作为代码段选择子，中断/异常触发时 CPU 用此值索引 GDT。
     * 同时 CS 寄存器当前值也为 0x38（UEFI 原本的代码段），iretq 恢复
     * 时会重新加载此选择子。因此索引 7 必须设为 64 位代码段。
     */
    gdt_set_entry(
        &gdt_table[7],
        0,
        0xFFFFF,
        0x9A,        /* Present|Ring0|Code|Exec/Read */
        0xA          /* Gr=1, L=1 (64位长模式) */
        );

    /*
     * 索引 8: TSS 描述符 (16字节)
     *   access = 0x89 => Present=1, Ring=00, Type=0001 (TSS available)
     */
    /* 分配 IST 栈 (每个 IST 分配 4KB 栈空间) */
    uart_puts("[GDT] alloc ist1\n");
    uint64_t ist1_addr = pmm_alloc_page();
    uart_puts("[GDT] alloc ist2\n");
    uint64_t ist2_addr = pmm_alloc_page();
    
    uart_puts("[GDT] init tss\n");
    tss_table.reserved1 = 0;
    tss_table.rsp0 = 0;  /* 将在切换到内核栈时设置 */
    tss_table.rsp1 = 0;
    tss_table.rsp2 = 0;
    tss_table.reserved2 = 0;
    
    /* 分配失败时 ist=0, CPU 回退到常规栈, 避免访问无效地址 */
    if (ist1_addr != 0) {
        tss_table.ist1 = ist1_addr + PMM_PAGE_SIZE;  /* 栈顶 (向下生长) */
    } else {
        tss_table.ist1 = 0;
    }
    if (ist2_addr != 0) {
        tss_table.ist2 = ist2_addr + PMM_PAGE_SIZE;
    } else {
        tss_table.ist2 = 0;
    }
    
    tss_table.ist3 = 0;
    tss_table.ist4 = 0;
    tss_table.ist5 = 0;
    tss_table.ist6 = 0;
    tss_table.ist7 = 0;
    tss_table.reserved3 = 0;
    tss_table.reserved4 = 0;
    tss_table.iopb_offset = sizeof(tss_table);

    /* 设置 TSS 描述符 */
    gdt_set_tss_entry(
        (gdt_system_entry_t *)&gdt_table[8],
        (uint64_t)(uintptr_t)&tss_table,
        sizeof(tss_table) - 1,
        0x89          /* Present|Ring0|TSS available */
        );

    /* 填充 GDTR */
    gdt_ptr.limit = sizeof(gdt_table) - 1;
    gdt_ptr.base  = (uint64_t)(uintptr_t)gdt_table;

    /*
     * 加载 GDT (仅 lgdt，不进行段重载)
     *
     * UEFI 已经在长模式下运行，段寄存器已经正确设置。
     * 在 UEFI Boot Services 环境下进行 lretq 段重载会导致崩溃。
     * 我们只加载新的 GDT 表，段寄存器保持 UEFI 设置的值。
     */
    uart_puts("[GDT] lgdt\n");
    __asm__ volatile(
        "lgdt   %[ptr]\n"                     /* 仅加载 GDT，不进行段重载 */
        "ltr    %[tss_selector]\n"           /* 加载 TSS */
        :
        : [ptr]           "m"(gdt_ptr),
          [tss_selector]  "r"(0x40)          /* TSS 选择子 = 8 * 8 = 0x40 */
        : "memory"
    );
}
