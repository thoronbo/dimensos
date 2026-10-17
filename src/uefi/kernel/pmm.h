/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: pmm.h
 * 描述: 物理内存管理器 (Physical Memory Manager) 接口
 *       基于位图 (Bitmap) 的物理页分配器。
 *       管理以 4KB 为单位的物理页帧。
 */

#ifndef DIMENSOS_PMM_H
#define DIMENSOS_PMM_H

#include <stdint.h>
#include <stdbool.h>

/* 物理页大小 */
#define PMM_PAGE_SIZE  0x1000  /* 4KB */

/* PMM 初始化
 *
 * 从 UEFI 固件获取的内存映射中提取可用物理内存区域，
 * 建立位图并标记所有页的使用状态。
 *
 * mmap_addr        - UEFI 内存映射缓冲区地址
 * mmap_size        - 内存映射总大小 (字节)
 * mmap_desc_size   - 每个描述符的大小 (用于遍历)
 */
void pmm_init(uint64_t mmap_addr, uint64_t mmap_size, uint64_t mmap_desc_size);

/* 分配一个物理页 (4KB)
 * 返回物理页基地址 (4KB 对齐)，失败返回 0 */
uint64_t pmm_alloc_page(void);

/* 释放一个物理页 (4KB)
 * addr - 之前由 pmm_alloc_page 返回的物理地址 */
void pmm_free_page(uint64_t addr);

/* 获取可用物理内存总量 (字节) */
uint64_t pmm_get_total_memory(void);

/* 获取已使用的物理内存量 (字节) */
uint64_t pmm_get_used_memory(void);

#endif /* DIMENSOS_PMM_H */
