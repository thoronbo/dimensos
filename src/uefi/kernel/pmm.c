/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: pmm.c
 * 描述: 物理内存管理器 (Physical Memory Manager) 实现
 *
 * 设计:
 *   使用位图 (Bitmap) 跟踪每个 4KB 物理页的使用状态。
 *   位 = 1 表示已使用, 位 = 0 表示空闲。
 *
 *   位图存放在物理地址 PMM_BITMAP_ADDR 处。
 *   分配算法: 简单的前向线性搜索。
 *
 * 注意:
 *   - 最大管理 4GB 物理内存 (128KB 位图)
 *   - 一次 pmm_init 后不可重新初始化
 */

#include "pmm.h"

/* ---------------------------------------------------------------
 * 常量与配置
 * --------------------------------------------------------------- */

/* 位图存储的物理基地址 */
#define PMM_BITMAP_ADDR   ((uint64_t)0x70000)

/* 位图能管理的最大物理内存 (4GB) */
#define PMM_MAX_MEMORY    ((uint64_t)4 * 1024 * 1024 * 1024ULL)

/* 最大页数 (4GB / 4KB) */
#define PMM_MAX_PAGES     (PMM_MAX_MEMORY / PMM_PAGE_SIZE)

/* 位图大小 (字节) = 最大页数 / 8 */
#define PMM_BITMAP_SIZE   (PMM_MAX_PAGES / 8)

/* UEFI 内存类型: EfiConventionalMemory = 7 */
#define EFI_CONVENTIONAL_MEMORY  7

/* ---------------------------------------------------------------
 * 全局状态
 * --------------------------------------------------------------- */

/* 位图指针 (指向 PMM_BITMAP_ADDR 的虚拟地址) */
static uint8_t  *bitmap;
/* 管理的总页数 */
static uint64_t  total_pages;
/* 已使用的页数 */
static uint64_t  used_pages;
/* 物理内存总量 (字节) */
static uint64_t  total_memory;
/* 是否已初始化 */
static bool      initialized;

/* ---------------------------------------------------------------
 * 位图操作 (内联)
 * --------------------------------------------------------------- */

/* 设置第 n 位为 1 (标记已用) */
static inline void bitmap_set(uint64_t page)
{
    bitmap[page / 8] |= (uint8_t)(1 << (page % 8));
}

/* 清除第 n 位 (标记空闲) */
static inline void bitmap_clear(uint64_t page)
{
    bitmap[page / 8] &= (uint8_t)~(1 << (page % 8));
}

/* 测试第 n 位: 1=已用, 0=空闲 */
static inline bool bitmap_test(uint64_t page)
{
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

/* ---------------------------------------------------------------
 * 根据物理地址计算页号
 * --------------------------------------------------------------- */
static inline uint64_t addr_to_page(uint64_t addr)
{
    return addr / PMM_PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * 从 UEFI 内存映射中识别可用的物理内存区域
 *
 * 内存描述符结构 (EFI_MEMORY_DESCRIPTOR):
 *   Offset  Size   Field
 *   0       4      Type      (UINT32)
 *   4       4      Pad       (UINT32)
 *   8       8      PhysicalStart (UINT64)
 *   16      8      VirtualStart  (UINT64)
 *   24      8      NumberOfPages (UINT64)
 *   32      8      Attribute     (UINT64)
 *   Total: 40 bytes
 * --------------------------------------------------------------- */

/* UEFI 内存描述符各字段偏移 */
#define MMAP_OFF_TYPE           0
#define MMAP_OFF_PHYSICAL_START 8
#define MMAP_OFF_NUMBER_OF_PAGES 24

/* ---------------------------------------------------------------
 * PMM 初始化
 *
 * 处理 UEFI 内存映射:
 *   1. 确定最大物理地址, 计算所需位图大小
 *   2. 从 UEFI 内存映射中查找可用区域分配位图
 *   3. 将所有页标记为已用
 *   4. 遍历内存映射, 将 EfiConventionalMemory 标记为空闲
 *   5. 将位图自身占用的页标记为已用
 * --------------------------------------------------------------- */
void pmm_init(uint64_t mmap_addr, uint64_t mmap_size, uint64_t mmap_desc_size)
{
    uint64_t  i;
    uint64_t  desc_base;
    uint32_t  type;
    uint64_t  phys_start;
    uint64_t  num_pages;
    uint64_t  page_start, page_end, page;
    uint64_t  bitmap_pages;
    uint64_t  max_phys_addr;
    uint64_t  bitmap_phys_addr = 0;

    /* 初始化全局状态 */
    total_pages = 0;
    used_pages  = 0;
    total_memory = 0;

    /*
     * 确定最大物理地址:
     *   遍历内存映射找到最大的 (PhysicalStart + NumberOfPages*4KB)
     */
    max_phys_addr = 0;
    i = 0;
    while (i < mmap_size) {
        desc_base = mmap_addr + i;
        phys_start = *(uint64_t *)(uintptr_t)(desc_base + MMAP_OFF_PHYSICAL_START);
        num_pages = *(uint64_t *)(uintptr_t)(desc_base + MMAP_OFF_NUMBER_OF_PAGES);
        if (phys_start + num_pages * PMM_PAGE_SIZE > max_phys_addr) {
            max_phys_addr = phys_start + num_pages * PMM_PAGE_SIZE;
        }
        i += mmap_desc_size;
    }

    /* 限制最大地址不超过 PMM_MAX_MEMORY */
    if (max_phys_addr > PMM_MAX_MEMORY) {
        max_phys_addr = PMM_MAX_MEMORY;
    }

    total_pages  = max_phys_addr / PMM_PAGE_SIZE;
    total_memory = total_pages * PMM_PAGE_SIZE;

    /* 计算所需位图大小 */
    uint64_t bitmap_size = (total_pages + 7) / 8;
    bitmap_pages = (bitmap_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;

    /* 从 UEFI 内存映射中查找可用区域分配位图 */
    i = 0;
    while (i < mmap_size && bitmap_phys_addr == 0) {
        desc_base = mmap_addr + i;
        type       = *(uint32_t *)(uintptr_t)(desc_base + MMAP_OFF_TYPE);
        phys_start = *(uint64_t *)(uintptr_t)(desc_base + MMAP_OFF_PHYSICAL_START);
        num_pages  = *(uint64_t *)(uintptr_t)(desc_base + MMAP_OFF_NUMBER_OF_PAGES);

        /* 在 EfiConventionalMemory 中寻找足够大的连续区域 */
        if (type == EFI_CONVENTIONAL_MEMORY && num_pages >= bitmap_pages) {
            /* 选择此区域的起始地址 */
            bitmap_phys_addr = phys_start;
        }

        i += mmap_desc_size;
    }

    /* 如果没有找到合适的区域, 使用默认地址作为后备 */
    if (bitmap_phys_addr == 0) {
        bitmap_phys_addr = PMM_BITMAP_ADDR;
    }

    /* 设置位图指针 (恒等映射下物理地址 == 虚拟地址) */
    bitmap = (uint8_t *)(uintptr_t)bitmap_phys_addr;

    /* 位图初始化为全 1 (所有页都标记为已用) */
    for (i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }

    /*
     * 遍历 UEFI 内存映射，将 EfiConventionalMemory 区域标记为空闲
     */
    i = 0;
    while (i < mmap_size) {
        desc_base = mmap_addr + i;
        type       = *(uint32_t *)(uintptr_t)(desc_base + MMAP_OFF_TYPE);
        phys_start = *(uint64_t *)(uintptr_t)(desc_base + MMAP_OFF_PHYSICAL_START);
        num_pages  = *(uint64_t *)(uintptr_t)(desc_base + MMAP_OFF_NUMBER_OF_PAGES);

        /* 仅处理 EfiConventionalMemory */
        if (type == EFI_CONVENTIONAL_MEMORY) {
            page_start = addr_to_page(phys_start);
            page_end   = page_start + num_pages;

            if (page_end > total_pages) {
                page_end = total_pages;
            }

            for (page = page_start; page < page_end; page++) {
                bitmap_clear(page);
            }
        }

        i += mmap_desc_size;
    }

    /* 将位图自身占用的页重新标记为已用 (避免被常规内存覆盖) */
    for (i = 0; i < bitmap_pages; i++) {
        uint64_t bitmap_page = addr_to_page(bitmap_phys_addr) + i;
        if (bitmap_page < total_pages) {
            bitmap_set(bitmap_page);
        }
    }

    /* 计算已用页数 */
    used_pages = 0;
    for (i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) {
            used_pages++;
        }
    }

    initialized = true;
}

/* ---------------------------------------------------------------
 * 分配一个物理页 (4KB)
 *
 * 前向线性搜索, 找到第一个空闲页。
 * 返回: 物理页基地址 (4KB 对齐), 失败返回 0。
 * --------------------------------------------------------------- */
uint64_t pmm_alloc_page(void)
{
    uint64_t i;

    if (!initialized) {
        return 0;
    }

    for (i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_pages++;
            return i * PMM_PAGE_SIZE;
        }
    }

    /* 无可用页 */
    return 0;
}

/* ---------------------------------------------------------------
 * 释放一个物理页
 *
 * addr - 之前由 pmm_alloc_page 返回的物理地址
 * --------------------------------------------------------------- */
void pmm_free_page(uint64_t addr)
{
    uint64_t page;

    if (!initialized) {
        return;
    }

    page = addr_to_page(addr);

    if (page >= total_pages) {
        return;  /* 地址超出管理范围 */
    }

    if (bitmap_test(page)) {
        bitmap_clear(page);
        used_pages--;
    }
}

/* ---------------------------------------------------------------
 * 获取可用物理内存总量 (字节)
 * --------------------------------------------------------------- */
uint64_t pmm_get_total_memory(void)
{
    return total_memory;
}

/* ---------------------------------------------------------------
 * 获取已使用的物理内存量 (字节)
 * --------------------------------------------------------------- */
uint64_t pmm_get_used_memory(void)
{
    return used_pages * PMM_PAGE_SIZE;
}
