/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - UEFI 自研图标数据实现
 *
 * 文件: kernel/gfx/icons.c
 * 描述: 32x32 ARGB 位图图标数据，完全自研绘制。
 *       使用程序化方式生成图标，避免手动输入庞大像素数据。
 * 阶段: Stage 6 - 图形化抽象层
 */

#include <efi.h>
#include "icons.h"
static void ico_set(UINT32 *icon, int x, int y, UINT32 color) {
    if (x >= 0 && x < ICON_WIDTH && y >= 0 && y < ICON_HEIGHT)
        icon[y * ICON_WIDTH + x] = color;
}

static void ico_hline(UINT32 *icon, int x0, int x1, int y, UINT32 color) {
    for (int x = x0; x <= x1; x++)
        ico_set(icon, x, y, color);
}

static void ico_vline(UINT32 *icon, int x, int y0, int y1, UINT32 color) {
    for (int y = y0; y <= y1; y++)
        ico_set(icon, x, y, color);
}

static void ico_rect(UINT32 *icon, int x, int y, int w, int h, UINT32 color) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            ico_set(icon, xx, yy, color);
}

static void ico_circle(UINT32 *icon, int cx, int cy, int r, UINT32 color) {
    int r2 = r * r;
    for (int y = cy - r; y <= cy + r; y++) {
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r2)
                ico_set(icon, x, y, color);
        }
    }
}

/* 初始化空图标 */
static UINT32 icon_empty_data[ICON_WIDTH * ICON_HEIGHT];

/* This PC 图标 - 计算机形状 */
static UINT32 icon_thispc_data[ICON_WIDTH * ICON_HEIGHT];

/* 文件管理器图标 - 文件夹形状 */
static UINT32 icon_files_data[ICON_WIDTH * ICON_HEIGHT];

/* Shell 图标 - 终端窗口形状 */
static UINT32 icon_shell_data[ICON_WIDTH * ICON_HEIGHT];

/* 设置图标 - 齿轮形状 */
static UINT32 icon_settings_data[ICON_WIDTH * ICON_HEIGHT];

/* 网络图标 - 网络连接形状 */
static UINT32 icon_network_data[ICON_WIDTH * ICON_HEIGHT];

/* 关于图标 - 信息形状 */
static UINT32 icon_about_data[ICON_WIDTH * ICON_HEIGHT];

/* 窗口按钮图标 */
static UINT32 icon_minimize_data[ICON_WIDTH * ICON_HEIGHT];
static UINT32 icon_maximize_data[ICON_WIDTH * ICON_HEIGHT];
static UINT32 icon_close_data[ICON_WIDTH * ICON_HEIGHT];

/* 生成 This PC 图标（计算机显示器形状） */
static void gen_thispc(UINT32 *icon) {
    UINT32 blue = ICON_ARGB(255, 0, 103, 192);
    UINT32 cyan = ICON_ARGB(255, 0, 180, 255);
    UINT32 gray = ICON_ARGB(255, 68, 68, 68);
    
    /* 显示器主体 */
    ico_rect(icon, 4, 4, 24, 18, blue);
    ico_rect(icon, 6, 6, 20, 14, ICON_ARGB(255, 0, 0, 0));
    
    /* 显示器边框 */
    ico_hline(icon, 4, 27, 4, cyan);
    ico_hline(icon, 4, 27, 21, gray);
    ico_vline(icon, 4, 4, 21, cyan);
    ico_vline(icon, 27, 4, 21, gray);
    
    /* 支架 */
    ico_rect(icon, 10, 22, 12, 2, gray);
    ico_rect(icon, 8, 24, 16, 4, gray);
    
    /* 屏幕内容（简单线条） */
    ico_hline(icon, 8, 23, 10, ICON_ARGB(255, 0, 200, 100));
    ico_hline(icon, 8, 20, 13, ICON_ARGB(255, 0, 180, 255));
    ico_hline(icon, 8, 17, 16, ICON_ARGB(255, 255, 190, 0));
}

/* 生成文件管理器图标（文件夹形状） */
static void gen_files(UINT32 *icon) {
    UINT32 yellow = ICON_ARGB(255, 255, 190, 0);
    UINT32 dark = ICON_ARGB(255, 43, 43, 43);
    
    /* 文件夹主体 */
    ico_rect(icon, 6, 10, 20, 18, yellow);
    
    /* 文件夹顶部 */
    ico_rect(icon, 6, 10, 20, 6, ICON_ARGB(255, 220, 160, 0));
    ico_rect(icon, 6, 8, 12, 6, yellow);
    
    /* 高光 */
    ico_vline(icon, 7, 12, 27, ICON_ARGB(128, 255, 255, 255));
    ico_hline(icon, 8, 25, 12, ICON_ARGB(128, 255, 255, 255));
}

/* 生成 Shell 图标（终端窗口形状） */
static void gen_shell(UINT32 *icon) {
    UINT32 bg = ICON_ARGB(255, 43, 43, 43);
    UINT32 border = ICON_ARGB(255, 68, 68, 68);
    UINT32 green = ICON_ARGB(255, 0, 200, 100);
    UINT32 gray = ICON_ARGB(255, 160, 160, 160);
    
    /* 窗口主体 */
    ico_rect(icon, 4, 4, 24, 24, bg);
    ico_rect(icon, 4, 4, 24, 24, border);
    
    /* 标题栏 */
    ico_rect(icon, 4, 4, 24, 5, ICON_ARGB(255, 48, 48, 48));
    
    /* 终端内容 */
    ico_hline(icon, 6, 25, 12, green);
    ico_hline(icon, 6, 22, 16, gray);
    ico_hline(icon, 6, 20, 20, gray);
    
    /* 闪烁光标 */
    ico_set(icon, 8, 12, ICON_ARGB(255, 0, 200, 100));
}

/* 生成设置图标（齿轮形状） */
static void gen_settings(UINT32 *icon) {
    UINT32 gray = ICON_ARGB(255, 160, 160, 160);
    UINT32 dark = ICON_ARGB(255, 68, 68, 68);
    
    /* 齿轮中心 */
    ico_circle(icon, 16, 16, 8, gray);
    ico_circle(icon, 16, 16, 4, dark);
    
    /* 齿轮齿 */
    for (int i = 0; i < 8; i++) {
        int angle = i * 45;
        int cx = 16 + (int)(13 * (angle == 0 ? 1 : angle == 180 ? -1 : 0));
        int cy = 16 + (int)(13 * (angle == 90 ? 1 : angle == 270 ? -1 : 0));
        if (angle % 90 != 0) {
            cx = 16 + (int)(10 * (angle < 90 || angle > 270 ? 1 : -1));
            cy = 16 + (int)(10 * (angle < 180 ? 1 : -1));
        }
        ico_rect(icon, cx - 3, cy - 3, 6, 6, gray);
    }
}

/* 生成网络图标（网络连接形状） */
static void gen_network(UINT32 *icon) {
    UINT32 blue = ICON_ARGB(255, 0, 103, 192);
    UINT32 cyan = ICON_ARGB(255, 0, 180, 255);
    
    /* 底部弧线 */
    for (int i = 0; i <= 16; i++) {
        int y = 28 - (i * i) / 8;
        ico_set(icon, 4 + i, y, blue);
        ico_set(icon, 27 - i, y, blue);
    }
    
    /* 中间弧线 */
    for (int i = 0; i <= 12; i++) {
        int y = 22 - (i * i) / 6;
        ico_set(icon, 8 + i, y, cyan);
        ico_set(icon, 23 - i, y, cyan);
    }
    
    /* 顶部弧线 */
    for (int i = 0; i <= 8; i++) {
        int y = 16 - (i * i) / 4;
        ico_set(icon, 12 + i, y, blue);
        ico_set(icon, 19 - i, y, blue);
    }
    
    /* 中心点 */
    ico_circle(icon, 16, 10, 3, cyan);
}

/* 生成关于图标（信息形状） */
static void gen_about(UINT32 *icon) {
    UINT32 blue = ICON_ARGB(255, 0, 103, 192);
    UINT32 white = ICON_ARGB(255, 240, 240, 240);
    
    /* 圆圈背景 */
    ico_circle(icon, 16, 16, 12, blue);
    
    /* 字母 'i' */
    ico_vline(icon, 15, 6, 22, white);
    ico_hline(icon, 14, 16, 6, white);
    ico_circle(icon, 16, 24, 2, white);
}

/* 生成窗口按钮图标 */
static void gen_window_buttons(void) {
    UINT32 gray = ICON_ARGB(255, 68, 68, 68);
    UINT32 white = ICON_ARGB(255, 240, 240, 240);
    UINT32 red = ICON_ARGB(255, 240, 60, 60);
    
    /* 最小化 - 短横线 */
    ico_hline(icon_minimize_data, 10, 21, 16, white);
    
    /* 最大化 - 方块 */
    ico_rect(icon_maximize_data, 10, 10, 12, 12, gray);
    ico_rect(icon_maximize_data, 11, 11, 10, 10, white);
    
    /* 关闭 - X */
    for (int i = 0; i < 10; i++) {
        ico_set(icon_close_data, 11 + i, 11 + i, red);
        ico_set(icon_close_data, 20 - i, 11 + i, red);
    }
}

/* 图标初始化函数 */
void icons_init(void) {
    /* 清空所有图标 */
    for (int i = 0; i < ICON_WIDTH * ICON_HEIGHT; i++) {
        icon_empty_data[i] = 0;
        icon_thispc_data[i] = 0;
        icon_files_data[i] = 0;
        icon_shell_data[i] = 0;
        icon_settings_data[i] = 0;
        icon_network_data[i] = 0;
        icon_about_data[i] = 0;
        icon_minimize_data[i] = 0;
        icon_maximize_data[i] = 0;
        icon_close_data[i] = 0;
    }
    
    /* 生成图标 */
    gen_thispc(icon_thispc_data);
    gen_files(icon_files_data);
    gen_shell(icon_shell_data);
    gen_settings(icon_settings_data);
    gen_network(icon_network_data);
    gen_about(icon_about_data);
    gen_window_buttons();
}

/* 图标数据导出 */
const UINT32 icon_empty[ICON_WIDTH * ICON_HEIGHT] = {0};
const UINT32 *icon_thispc = icon_thispc_data;
const UINT32 *icon_files = icon_files_data;
const UINT32 *icon_shell = icon_shell_data;
const UINT32 *icon_settings = icon_settings_data;
const UINT32 *icon_network = icon_network_data;
const UINT32 *icon_about = icon_about_data;
const UINT32 *icon_minimize = icon_minimize_data;
const UINT32 *icon_maximize = icon_maximize_data;
const UINT32 *icon_close = icon_close_data;
