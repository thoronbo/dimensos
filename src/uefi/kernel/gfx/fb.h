/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - UEFI 帧缓冲绘制原语
 *
 * 文件: kernel/gfx/fb.h
 * 描述: 提供像素/矩形/圆角/渐变/文字/alpha混合/位图等绘制原语，内置双缓冲。
 *       所有绘制先写入后备缓冲，由 fb_swap() 一次性贴到屏幕。
 * 阶段: Stage 6 - 图形化抽象层
 */

#ifndef DIMENSOS_GFX_FB_H
#define DIMENSOS_GFX_FB_H

#include <efi.h>
#include "gop.h"

/* 颜色宏（逻辑颜色：0x00RRGGBB） */
#ifndef GFX_RGB
#define GFX_RGB(r,g,b)  ( ((UINT32)(r)<<16) | ((UINT32)(g)<<8) | (UINT32)(b) )
#endif

/* 预定义颜色 - Windows 11 暗色模式实际配色 */
#define GFX_BG_TOP          GFX_RGB(31, 31, 31)      /* #1F1F1F */
#define GFX_BG_BOTTOM       GFX_RGB(24, 24, 24)      /* #181818 */
#define GFX_GLOW_BLUE       GFX_RGB(0, 103, 192)     /* #0067C0 */
#define GFX_GLOW_CYAN       GFX_RGB(0, 180, 255)
#define GFX_TASKBAR         GFX_RGB(42, 42, 42)      /* #2A2A2A */
#define GFX_TASKBAR_HOVER   GFX_RGB(60, 60, 60)      /* #3C3C3C */
#define GFX_START_BTN       GFX_RGB(0, 103, 192)     /* #0067C0 */
#define GFX_START_BTN_HV    GFX_RGB(0, 120, 220)     /* #0078DC */
#define GFX_WINDOW_BG       GFX_RGB(43, 43, 43)      /* #2B2B2B */
#define GFX_WINDOW_BG_LIGHT GFX_RGB(50, 50, 50)      /* #323232 */
#define GFX_WINDOW_TITLE    GFX_RGB(48, 48, 48)      /* #303030 */
#define GFX_WINDOW_BORDER   GFX_RGB(68, 68, 68)      /* #444444 */
#define GFX_WINDOW_ACTIVE   GFX_RGB(0, 103, 192)     /* #0067C0 */
#define GFX_TEXT            GFX_RGB(240, 240, 240)
#define GFX_TEXT_DIM        GFX_RGB(160, 160, 160)
#define GFX_TEXT_WHITE      GFX_RGB(255, 255, 255)
#define GFX_SELECTION       GFX_RGB(0, 103, 192)     /* #0067C0 */
#define GFX_SELECTION_HOVER GFX_RGB(0, 120, 220)     /* #0078DC */
#define GFX_BTN_RED         GFX_RGB(240, 60, 60)
#define GFX_BTN_RED_HV      GFX_RGB(255, 80, 80)
#define GFX_BTN_YELLOW      GFX_RGB(255, 190, 0)
#define GFX_BTN_GREEN       GFX_RGB(0, 200, 100)
#define GFX_BLACK           GFX_RGB(0, 0, 0)
#define GFX_WHITE           GFX_RGB(255, 255, 255)

/* 初始化帧缓冲（分配后备缓冲） */
EFI_STATUS fb_init(void);

/* 释放帧缓冲资源 */
void fb_done(void);

/* 单像素写入后备缓冲 */
void fb_put_pixel(int x, int y, UINT32 color);

/* 像素级 alpha 混合 */
void fb_blend_pixel(int x, int y, UINT32 color, UINT8 alpha);

/* 填充矩形 */
void fb_fill_rect(int x, int y, int w, int h, UINT32 color);

/* 带 alpha 的矩形填充 */
void fb_blend_rect(int x, int y, int w, int h, UINT32 color, UINT8 alpha);

/* 清屏 */
void fb_clear(UINT32 color);

/* 填充圆角矩形 */
void fb_fill_round_rect(int x, int y, int w, int h, int r, UINT32 color);

/* 带 alpha 的圆角矩形填充 */
void fb_blend_round_rect(int x, int y, int w, int h, int r, UINT32 color, UINT8 alpha);

/* 渐变圆角矩形（垂直渐变） */
void fb_gradient_round_rect(int x, int y, int w, int h, int r,
                            UINT32 top, UINT32 bottom);

/* 径向渐变（用于光晕效果） */
void fb_radial_gradient(int cx, int cy, int radius,
                        UINT32 center, UINT32 edge);

/* 绘制文本 */
void fb_draw_text(int x, int y, const CHAR16 *str, UINT32 color);

/* 绘制位图（ARGB 格式） */
void fb_draw_bitmap(int x, int y, int w, int h, const UINT32 *bitmap);

/* 绘制窗口阴影 */
void fb_draw_shadow(int x, int y, int w, int h, int r, int depth);

/* 绘制圆角矩形边框 */
void fb_draw_round_rect_border(int x, int y, int w, int h, int r, UINT32 color);

/* 内发光效果（实现柔和边界） */
void fb_inner_glow(int x, int y, int w, int h, int radius, UINT32 color, UINT8 intensity);

/* 读取像素（从后备缓冲） */
UINT32 fb_get_pixel(int x, int y);

/* 渐变矩形（垂直渐变） */
void fb_gradient_rect(int x, int y, int w, int h, UINT32 top, UINT32 bottom);

/* 计算文本宽度 */
UINT32 fb_text_width(const CHAR16 *str);

/* 绘制水平线 */
static inline void fb_hline(int x0, int x1, int y, UINT32 color) {
    fb_fill_rect(x0, y, x1 - x0 + 1, 1, color);
}

/* 后备缓冲 -> 屏幕（双缓冲翻转） */
void fb_swap(void);

/* 获取屏幕尺寸 */
#define FB_WIDTH  (g_gfx_mode.width)
#define FB_HEIGHT (g_gfx_mode.height)

#endif /* DIMENSOS_GFX_FB_H */
