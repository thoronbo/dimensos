/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - UEFI 自研图标数据
 *
 * 文件: kernel/gfx/icons.h
 * 描述: 32x32 ARGB 位图图标数据，完全自研绘制。
 *       每个图标为 UINT32 数组，格式为 ARGB（高字节为 alpha）。
 * 阶段: Stage 6 - 图形化抽象层
 */

#ifndef DIMENSOS_GFX_ICONS_H
#define DIMENSOS_GFX_ICONS_H

#include <efi.h>

#define ICON_WIDTH  32
#define ICON_HEIGHT 32

/* 图标数据类型 */
typedef const UINT32 icon_data_t[ICON_WIDTH * ICON_HEIGHT];

/* 图标颜色定义 */
#define ICON_ALPHA(op) ((UINT32)(op) << 24)
#define ICON_RGB(r,g,b) ( ((UINT32)(r) << 16) | ((UINT32)(g) << 8) | (UINT32)(b) )
#define ICON_ARGB(op,r,g,b) (ICON_ALPHA(op) | ICON_RGB(r,g,b))

#define ICO_BLACK   ICON_RGB(0,0,0)
#define ICO_WHITE   ICON_RGB(255,255,255)
#define ICO_BLUE    ICON_RGB(0,103,192)   /* #0067C0 - Windows 11 强调色 */
#define ICO_CYAN    ICON_RGB(0,180,255)
#define ICO_GREEN   ICON_RGB(0,200,100)
#define ICO_YELLOW  ICON_RGB(255,190,0)
#define ICO_RED     ICON_RGB(240,60,60)
#define ICO_GRAY    ICON_RGB(160,160,160)
#define ICO_DARK    ICON_RGB(43,43,43)    /* #2B2B2B */
#define ICO_LIGHT   ICON_RGB(68,68,68)    /* #444444 */

/* 空图标 */
extern const UINT32 icon_empty[ICON_WIDTH * ICON_HEIGHT];

/* This PC 图标 */
extern const UINT32 *icon_thispc;

/* 文件管理器图标 */
extern const UINT32 *icon_files;

/* Shell 图标 */
extern const UINT32 *icon_shell;

/* 设置图标 */
extern const UINT32 *icon_settings;

/* 网络图标 */
extern const UINT32 *icon_network;

/* 关于图标 */
extern const UINT32 *icon_about;

/* 最小化按钮图标 */
extern const UINT32 *icon_minimize;

/* 最大化/还原按钮图标 */
extern const UINT32 *icon_maximize;

/* 关闭按钮图标 */
extern const UINT32 *icon_close;

/* 初始化图标（生成图标数据） */
void icons_init(void);

#endif /* DIMENSOS_GFX_ICONS_H */
