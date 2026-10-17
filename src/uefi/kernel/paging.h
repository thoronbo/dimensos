/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: paging.h
 * 描述: x86_64 4级分页系统接口
 *       提供页表初始化、恒等映射、地址查询等功能。
 */

#ifndef DIMENSOS_PAGING_H
#define DIMENSOS_PAGING_H

#include <stdint.h>
#include <stdbool.h>

/* 页大小常量 */
#define PAGE_SIZE_4KB     0x1000
#define PAGE_SIZE_2MB     0x200000
#define PAGE_SIZE_1GB     0x40000000

/* 页表项类型 (x86_64 页表项为 8 字节) */
typedef uint64_t page_entry_t;

/* 页表项标志位 */
#define PAGE_PRESENT    (1ULL << 0)   /* 页存在 */
#define PAGE_WRITE      (1ULL << 1)   /* 可写 */
#define PAGE_USER       (1ULL << 2)   /* 用户模式可访问 */
#define PAGE_WRITETHROUGH (1ULL << 3) /* Write-through 缓存 */
#define PAGE_CACHE_DISABLE (1ULL << 4) /* 禁用缓存 */
#define PAGE_ACCESSED   (1ULL << 5)   /* 已访问 */
#define PAGE_DIRTY      (1ULL << 6)   /* 已脏 (仅页目录和页表) */
#define PAGE_HUGE       (1ULL << 7)   /* 大页 (2MB/1GB) */
#define PAGE_GLOBAL     (1ULL << 8)   /* 全局页 (需 CR4.PGE) */

/* 恒等映射前 8GB 物理内存的最大范围 */
#define IDENTITY_MAP_LIMIT  0x120000000ULL  /* 4GB */

/* 初始化分页系统 (建立新页表并切换 CR3) */
void paging_init(void);

/* 将指定物理地址范围建立恒等映射 */
void paging_map_identity(uint64_t phys_addr, uint64_t size, uint64_t flags);

/* 查询虚拟地址对应的物理地址 */
uint64_t paging_query(uint64_t virt_addr);

/* 获取当前页表基址 (CR3 值) */
uint64_t paging_get_cr3(void);

#endif /* DIMENSOS_PAGING_H */
