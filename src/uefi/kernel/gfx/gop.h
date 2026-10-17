/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - UEFI 图形输出协议 (GOP) 初始化
 *
 * 文件: kernel/gfx/gop.h
 * 描述: 定位 EFI Graphics Output Protocol，切换到最佳图形模式，
 *       暴露帧缓冲基地址与几何参数供帧缓冲绘制层使用。
 * 阶段: Stage 6 - 图形化抽象层
 */

#ifndef DIMENSOS_GFX_GOP_H
#define DIMENSOS_GFX_GOP_H

#include <efi.h>

/* 像素格式统一为 32 位 B8G8R8A8（与 PixelBlueGreenRedReserved8BitPerColor 对齐） */
typedef struct {
    EFI_PHYSICAL_ADDRESS  fb_base;       /* 帧缓冲物理基地址 */
    UINTN                 fb_size;       /* 帧缓冲总字节数 */
    UINT32                width;         /* 可见宽度（像素） */
    UINT32                height;        /* 可见高度（像素） */
    UINT32                ppsl;          /* 每扫描线像素数（可能 > width） */
    UINT32                pitch;         /* 每扫描线字节数 = ppsl * 4 */
    EFI_GRAPHICS_PIXEL_FORMAT pixel_fmt; /* 像素格式 */
    UINT32                r_mask;        /* 红色位掩码（PixelBitMask 用） */
    UINT32                g_mask;        /* 绿色位掩码 */
    UINT32                b_mask;        /* 蓝色位掩码 */
} gfx_mode_t;

/* 全局图形模式信息 */
extern gfx_mode_t g_gfx_mode;

/* 初始化 GOP，切换到最佳图形模式（优先最大分辨率） */
EFI_STATUS gfx_init(void);

/* 释放图形资源（恢复默认模式） */
void gfx_done(void);

#endif /* DIMENSOS_GFX_GOP_H */
