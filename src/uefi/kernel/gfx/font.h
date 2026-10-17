/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - UEFI 自绘 8x16 位图字体
 *
 * 文件: kernel/gfx/font.h
 * 描述: 8 像素宽 × 16 像素高的等宽位图字体接口。
 *       字体数据完全自绘，覆盖 ASCII 32-126 可打印字符。
 * 阶段: Stage 6 - 图形化抽象层
 */

#ifndef DIMENSOS_GFX_FONT_H
#define DIMENSOS_GFX_FONT_H

#include <efi.h>

#define FONT_WIDTH   8
#define FONT_HEIGHT  16

/* 在 指定位置绘制单个字符（前景色 color） */
void font_draw_char(int x, int y, CHAR16 ch, UINT32 color);

/* 获取字体宽度 */
static inline int font_get_width(void) { return FONT_WIDTH; }

#endif /* DIMENSOS_GFX_FONT_H */
