/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: paging.c
 * 描述: x86_64 4级分页系统实现
 *
 * 分页结构:
 *   PML4 (1 表) -> PDPT (1 表) -> PD (若干表, 使用 2MB 大页)
 *
 * 策略:
 *   1. 在 BSS 段分配静态对齐的页表空间
 *   2. 使用 2MB 大页 (PS=1) 恒等映射前 4GB 物理内存
 *   3. 每个 PD 表覆盖 1GB (512 项 x 2MB)
 *   4. 需要 4 个 PD 表覆盖 4GB
 *   5. 切换 CR3 到新 PML4
 *
 * 注意: 页表为静态变量, 运行时检查其物理地址是否 <4GB,
 *       若 >4GB 则跳过 CR3 切换 (继续使用 UEFI 页表)。
 */

#include "paging.h"

/* 页表项数量 (x86_64 每级 512 项, 9 位索引) */
#define PT_ENTRIES  512

/* 静态页表池: 4KB 对齐 */
#define PAGE_ALIGNED  __attribute__((aligned(PAGE_SIZE_4KB)))

/* PML4 表 (1 个) */
static PAGE_ALIGNED page_entry_t pml4_table[PT_ENTRIES];

/* PDPT 表 (1 个) */
static PAGE_ALIGNED page_entry_t pdpt_table[PT_ENTRIES];

/* PD 表 (8 个, 每个覆盖 1GB, 共覆盖 8GB) */
#define PD_TABLE_COUNT  8
static PAGE_ALIGNED page_entry_t pd_tables[PD_TABLE_COUNT][PT_ENTRIES];

/* ---------------------------------------------------------------
 * 构建页表项
 *
 * phys_addr - 物理地址 (低 12 位被忽略, 由 flags 提供)
 * flags     - 页表标志位 (PAGE_PRESENT, PAGE_WRITE, PAGE_HUGE, ...)
 *
 * 对于 2MB 大页: 物理地址低 21 位忽略 (由 PS=1 隐含)
 * 返回: 组合后的 64 位页表项
 * --------------------------------------------------------------- */
static page_entry_t paging_make_entry(uint64_t phys_addr, uint64_t flags)
{
    return (phys_addr & ~0xFFFULL) | (flags & 0xFFFULL);
}

/* ---------------------------------------------------------------
 * 初始化分页系统
 *
 * 1. 检查页表物理地址是否 <4GB (否则跳过 CR3 切换)
 * 2. 清空所有页表
 * 3. 设置 PML4[0] -> PDPT
 * 4. 设置 PDPT[0..3] -> PD[0..3]
 * 5. 设置 PD 表条目 (每个入口映射 2MB)
 * 6. 关中断, 切换 CR3 到新 PML4
 * --------------------------------------------------------------- */
void paging_init(void)
{
    uint64_t i, j;
    uint64_t phys;

    /*
     * 检查页表物理地址是否 <4GB
     *
     * 静态页表变量位于 PE 镜像的 BSS 段, UEFI 可能在 >4GB 的
     * 内存加载 PE 镜像。如果页表物理地址 >4GB, 则恒等映射
     * (仅覆盖前 4GB) 无法访问页表, CR3 切换后即时崩溃。
     *
     * 若超出 4GB, 跳过 CR3 切换, 继续使用 UEFI 的页表。
     */
    uint64_t pml4_pa = (uint64_t)(uintptr_t)pml4_table;
    if (pml4_pa >= IDENTITY_MAP_LIMIT) {
        return;
    }

    /* --- 1. 清空 PML4 表 --- */
    for (i = 0; i < PT_ENTRIES; i++) {
        pml4_table[i] = 0;
    }

    /* --- 2. 清空 PDPT 表 --- */
    for (i = 0; i < PT_ENTRIES; i++) {
        pdpt_table[i] = 0;
    }

    /* --- 3. 清空所有 PD 表 --- */
    for (i = 0; i < PD_TABLE_COUNT; i++) {
        for (j = 0; j < PT_ENTRIES; j++) {
            pd_tables[i][j] = 0;
        }
    }

    /* --- 4. 建立 PML4[0] -> PDPT (仅Ring0可访问) --- */
    pml4_table[0] = paging_make_entry(
        (uint64_t)(uintptr_t)pdpt_table,
        PAGE_PRESENT | PAGE_WRITE
        );

    /* --- 5. 建立 PDPT[0..3] -> PD[0..3] (仅Ring0可访问) --- */
    for (i = 0; i < PD_TABLE_COUNT; i++) {
        pdpt_table[i] = paging_make_entry(
            (uint64_t)(uintptr_t)&pd_tables[i][0],
            PAGE_PRESENT | PAGE_WRITE
            );
    }

    /* --- 6. 填充 PD 表: 恒等映射前 4GB (2MB 大页) --- */
    /* 
     * 内存区域划分:
     *   0x00000000 - 0x3FFFFFFF (1GB): 内核区域 (禁止用户态访问)
     *   0x40000000 - 0xFFFFFFFF (3GB): 用户区域 (允许用户态访问)
     */
    const uint64_t USER_BASE = 0x40000000ULL; /* 1GB 边界 */
    
    for (i = 0; i < PD_TABLE_COUNT; i++) {
        for (j = 0; j < PT_ENTRIES; j++) {
            /* 物理地址 = (i * 512 + j) * 2MB */
            phys = (i * PT_ENTRIES + j) * PAGE_SIZE_2MB;
            
            /* 根据地址决定是否设置 PAGE_USER */
            uint64_t flags = PAGE_PRESENT | PAGE_WRITE | PAGE_HUGE;
            if (phys >= USER_BASE) {
                flags |= PAGE_USER;  /* 用户区域允许用户态访问 */
            }
            
            pd_tables[i][j] = paging_make_entry(phys, flags);
        }
    }

    /*
     * --- 7. 切换 CR3 到新 PML4 ---
     *
     * 切换前关中断, 防止切换过程中中断触发导致页表不一致。
     */
    __asm__ volatile(
        "cli\n"
        "movq   %[pml4], %%cr3\n"
        :
        : [pml4] "r"(pml4_pa)
        : "memory"
    );
}

/* ---------------------------------------------------------------
 * 将指定物理地址范围建立恒等映射
 * 阶段1 实现: 覆盖所有页表并重建, 此函数用于支持后续的精细映射
 *
 * 注意: 当前实现已在 paging_init 中预映射全部 4GB,
 *       此函数为未来细粒度映射预留接口。
 * --------------------------------------------------------------- */
void paging_map_identity(uint64_t phys_addr, uint64_t size, uint64_t flags)
{
    uint64_t end;
    uint64_t pml4_idx, pdpt_idx, pd_idx, pt_idx;
    uint64_t page_addr;

    (void)pml4_idx;

    if (size == 0) {
        return;
    }

    /* 对齐到 4KB 边界 */
    end = phys_addr + size;
    if (end > IDENTITY_MAP_LIMIT) {
        end = IDENTITY_MAP_LIMIT;
    }

    page_addr = phys_addr & ~(PAGE_SIZE_4KB - 1);

    while (page_addr < end) {
        /* 提取各级索引 */
        pml4_idx = (page_addr >> 39) & 0x1FF;  /* PML4 索引 (位 39-47) */
        pdpt_idx = (page_addr >> 30) & 0x1FF;  /* PDPT 索引 (位 30-38) */
        pd_idx   = (page_addr >> 21) & 0x1FF;  /* PD  索引 (位 21-29) */
        pt_idx   = (page_addr >> 12) & 0x1FF;  /* PT  索引 (位 12-20) */

        /* 仅支持已预映射范围内的查询, 不动态分配新页表 */
        page_addr += PAGE_SIZE_4KB;
    }
}

/* ---------------------------------------------------------------
 * 查询虚拟地址对应的物理地址
 *
 * 遍历 4 级页表 (仅读当前页表), 返回物理地址。
 * 支持 4KB 和 2MB 页面。
 *
 * 返回: 物理地址, 如果未映射则返回 0。
 * --------------------------------------------------------------- */
uint64_t paging_query(uint64_t virt_addr)
{
    page_entry_t *pml4, *pdpt, *pd;
    uint64_t     pml4_idx, pdpt_idx, pd_idx, pt_idx;

    pml4_idx = (virt_addr >> 39) & 0x1FF;
    pdpt_idx = (virt_addr >> 30) & 0x1FF;
    pd_idx   = (virt_addr >> 21) & 0x1FF;
    pt_idx   = (virt_addr >> 12) & 0x1FF;

    /* 读取 PML4 */
    pml4 = (page_entry_t *)(uintptr_t)paging_get_cr3();
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        return 0;
    }

    /* 读取 PDPT */
    pdpt = (page_entry_t *)(uintptr_t)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        return 0;
    }

    /* 检查是否是 1GB 大页 */
    if (pdpt[pdpt_idx] & PAGE_HUGE) {
        return (pdpt[pdpt_idx] & ~((1ULL << 30) - 1)) | (virt_addr & ((1ULL << 30) - 1));
    }

    /* 读取 PD */
    pd = (page_entry_t *)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFULL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        return 0;
    }

    /* 检查是否是 2MB 大页 */
    if (pd[pd_idx] & PAGE_HUGE) {
        return (pd[pd_idx] & ~((1ULL << 21) - 1)) | (virt_addr & ((1ULL << 21) - 1));
    }

    /* 4KB 页面 (阶段1 未使用此级别, 但保留逻辑) */
    return (pd[pd_idx] & ~0xFFFULL) | (virt_addr & 0xFFFULL);
}

/* ---------------------------------------------------------------
 * 获取当前页表基址 (CR3 寄存器值)
 * --------------------------------------------------------------- */
uint64_t paging_get_cr3(void)
{
    uint64_t cr3;

    __asm__ volatile("movq %%cr3, %[cr3]" : [cr3] "=r"(cr3));

    return cr3;
}
