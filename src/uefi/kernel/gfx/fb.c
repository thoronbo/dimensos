/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - UEFI 帧缓冲绘制原语实现
 *
 * 文件: kernel/gfx/fb.c
 * 描述: 双缓冲帧缓冲绘制。支持 alpha 混合、阴影、圆角、渐变、位图绘制。
 *       绝对自研，仅依赖 UEFI Boot Services 与 GOP 几何信息。
 */

#include <efi.h>
#include <eficon.h>
#include "gop.h"
#include "fb.h"
#include "font.h"

extern EFI_SYSTEM_TABLE *g_ST;

/* freestanding 环境下的 memcpy 声明（实现在 kernel/memset.c） */
extern void *memcpy(void *dest, const void *src, size_t n);

static UINT32 *g_back_buffer = NULL;
static UINTN   g_back_size   = 0;

static int g_r_shift = 16;
static int g_g_shift = 8;
static int g_b_shift = 0;

/* 整数平方根 */
static UINT32 isqrt(UINT32 v)
{
    UINT32 r = 0;
    UINT32 bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        UINT32 t = r + bit;
        if (v >= t) { v -= t; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* 计算掩码最低 set bit 位置 */
static int mask_to_shift(UINT32 mask)
{
    int s = 0;
    if (mask == 0) return 0;
    while ((mask & 1) == 0) { mask >>= 1; s++; }
    return s;
}

/* 将逻辑颜色（0x00RRGGBB）转换为帧缓冲写入值 */
static inline UINT32 color_to_native(UINT32 c)
{
    UINT32 r = (c >> 16) & 0xFF;
    UINT32 g = (c >> 8) & 0xFF;
    UINT32 b = c & 0xFF;
    return (r << g_r_shift) | (g << g_g_shift) | (b << g_b_shift);
}

/* 从帧缓冲读取像素并转换为逻辑颜色 */
static inline UINT32 native_to_color(UINT32 native)
{
    UINT32 r = (native >> g_r_shift) & 0xFF;
    UINT32 g = (native >> g_g_shift) & 0xFF;
    UINT32 b = (native >> g_b_shift) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

EFI_STATUS fb_init(void)
{
    EFI_STATUS Status;

    if (g_gfx_mode.fb_size == 0)
        return EFI_NOT_STARTED;

    g_back_size = g_gfx_mode.fb_size;

    Status = g_ST->BootServices->AllocatePool(
        EfiBootServicesData, g_back_size, (VOID **)&g_back_buffer);
    if (EFI_ERROR(Status))
        return Status;

    g_r_shift = mask_to_shift(g_gfx_mode.r_mask);
    g_g_shift = mask_to_shift(g_gfx_mode.g_mask);
    g_b_shift = mask_to_shift(g_gfx_mode.b_mask);

    return EFI_SUCCESS;
}

void fb_done(void)
{
    /* ExitBootServices 后无法再调用 FreePool，只清空指针 */
    g_back_buffer = NULL;
    g_back_size = 0;
}

/* 像素级 alpha 混合: dst = (1-alpha)*dst + alpha*src */
void fb_blend_pixel(int x, int y, UINT32 color, UINT8 alpha)
{
    UINT32 *dst;
    UINT32 src_r, src_g, src_b;
    UINT32 dst_r, dst_g, dst_b;
    UINT32 blended;

    if (g_back_buffer == NULL) return;
    if (x < 0 || y < 0) return;
    if ((UINT32)x >= g_gfx_mode.width || (UINT32)y >= g_gfx_mode.height) return;

    if (alpha >= 255) {
        fb_put_pixel(x, y, color);
        return;
    }
    if (alpha == 0) {
        return;
    }

    dst = g_back_buffer + (UINT32)y * g_gfx_mode.ppsl + (UINT32)x;

    src_r = (color >> 16) & 0xFF;
    src_g = (color >> 8) & 0xFF;
    src_b = color & 0xFF;

    UINT32 native = *dst;
    dst_r = (native >> g_r_shift) & 0xFF;
    dst_g = (native >> g_g_shift) & 0xFF;
    dst_b = (native >> g_b_shift) & 0xFF;

    dst_r = ((255 - alpha) * dst_r + alpha * src_r) >> 8;
    dst_g = ((255 - alpha) * dst_g + alpha * src_g) >> 8;
    dst_b = ((255 - alpha) * dst_b + alpha * src_b) >> 8;

    blended = (dst_r << g_r_shift) | (dst_g << g_g_shift) | (dst_b << g_b_shift);
    *dst = blended;
}

void fb_put_pixel(int x, int y, UINT32 color)
{
    UINT32 *dst;

    if (g_back_buffer == NULL) return;
    if (x < 0 || y < 0) return;
    if ((UINT32)x >= g_gfx_mode.width || (UINT32)y >= g_gfx_mode.height) return;

    dst = g_back_buffer + (UINT32)y * g_gfx_mode.ppsl + (UINT32)x;
    *dst = color_to_native(color);
}

void fb_fill_rect(int x, int y, int w, int h, UINT32 color)
{
    UINT32 native = color_to_native(color);
    int    py, px;
    int    x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (g_back_buffer == NULL) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_gfx_mode.width)  x1 = g_gfx_mode.width;
    if (y1 > (int)g_gfx_mode.height) y1 = g_gfx_mode.height;
    if (x0 >= x1 || y0 >= y1) return;

    for (py = y0; py < y1; py++) {
        UINT32 *row = g_back_buffer + (UINT32)py * g_gfx_mode.ppsl;
        for (px = x0; px < x1; px++)
            row[px] = native;
    }
}

/* 带 alpha 的矩形填充 */
void fb_blend_rect(int x, int y, int w, int h, UINT32 color, UINT8 alpha)
{
    int    py, px;
    int    x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (g_back_buffer == NULL) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_gfx_mode.width)  x1 = g_gfx_mode.width;
    if (y1 > (int)g_gfx_mode.height) y1 = g_gfx_mode.height;
    if (x0 >= x1 || y0 >= y1) return;

    if (alpha >= 255) {
        fb_fill_rect(x, y, w, h, color);
        return;
    }
    if (alpha == 0) {
        return;
    }

    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            fb_blend_pixel(px, py, color, alpha);
        }
    }
}

void fb_clear(UINT32 color)
{
    if (g_back_buffer == NULL) return;
    fb_fill_rect(0, 0, g_gfx_mode.width, g_gfx_mode.height, color);
}

static BOOLEAN in_round_rect(int px, int py, int x, int y, int w, int h, int r)
{
    int cx, cy;
    int dx, dy;

    if (px < x || py < y || px >= x + w || py >= y + h)
        return FALSE;

    if (px >= x + r && px < x + w - r)
        return TRUE;
    if (py >= y + r && py < y + h - r)
        return TRUE;

    if (px < x + r)         cx = x + r;
    else                    cx = x + w - r - 1;
    if (py < y + r)         cy = y + r;
    else                    cy = y + h - r - 1;

    dx = px - cx;
    dy = py - cy;
    return (dx * dx + dy * dy) <= (r * r);
}

void fb_fill_round_rect(int x, int y, int w, int h, int r, UINT32 color)
{
    UINT32 native = color_to_native(color);
    int    py, px;
    int    x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (g_back_buffer == NULL) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_gfx_mode.width)  x1 = g_gfx_mode.width;
    if (y1 > (int)g_gfx_mode.height) y1 = g_gfx_mode.height;

    for (py = y0; py < y1; py++) {
        UINT32 *row = g_back_buffer + (UINT32)py * g_gfx_mode.ppsl;
        for (px = x0; px < x1; px++) {
            if (in_round_rect(px, py, x, y, w, h, r))
                row[px] = native;
        }
    }
}

/* 带 alpha 的圆角矩形填充 */
void fb_blend_round_rect(int x, int y, int w, int h, int r, UINT32 color, UINT8 alpha)
{
    int    py, px;
    int    x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (g_back_buffer == NULL) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_gfx_mode.width)  x1 = g_gfx_mode.width;
    if (y1 > (int)g_gfx_mode.height) y1 = g_gfx_mode.height;

    if (alpha >= 255) {
        fb_fill_round_rect(x, y, w, h, r, color);
        return;
    }
    if (alpha == 0) {
        return;
    }

    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            if (in_round_rect(px, py, x, y, w, h, r))
                fb_blend_pixel(px, py, color, alpha);
        }
    }
}

void fb_gradient_round_rect(int x, int y, int w, int h, int r,
                            UINT32 top, UINT32 bottom)
{
    UINT32 top_n = color_to_native(top);
    UINT32 bot_n = color_to_native(bottom);
    int   top_r = (top_n >> 16) & 0xFF;
    int   top_g = (top_n >> 8) & 0xFF;
    int   top_b = top_n & 0xFF;
    int   bot_r = (bot_n >> 16) & 0xFF;
    int   bot_g = (bot_n >> 8) & 0xFF;
    int   bot_b = bot_n & 0xFF;
    int   py, px;

    if (g_back_buffer == NULL) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    for (py = y; py < y + h; py++) {
        int t = (py - y) * 256 / (h > 1 ? h - 1 : 1);
        int cr = top_r + (bot_r - top_r) * t / 256;
        int cg = top_g + (bot_g - top_g) * t / 256;
        int cb = top_b + (bot_b - top_b) * t / 256;
        UINT32 native = (UINT32)((cr << 16) | (cg << 8) | cb);
        UINT32 *row;
        int ry = py;

        if (ry < 0 || ry >= (int)g_gfx_mode.height) continue;
        row = g_back_buffer + (UINT32)ry * g_gfx_mode.ppsl;

        for (px = x; px < x + w; px++) {
            if (px < 0 || px >= (int)g_gfx_mode.width) continue;
            if (r == 0 || in_round_rect(px, py, x, y, w, h, r))
                row[px] = native;
        }
    }
}

void fb_radial_gradient(int cx, int cy, int radius,
                        UINT32 center, UINT32 edge)
{
    UINT32 ctr_n = color_to_native(center);
    UINT32 edg_n = color_to_native(edge);
    int   ctr_r = (ctr_n >> 16) & 0xFF;
    int   ctr_g = (ctr_n >> 8) & 0xFF;
    int   ctr_b = ctr_n & 0xFF;
    int   edg_r = (edg_n >> 16) & 0xFF;
    int   edg_g = (edg_n >> 8) & 0xFF;
    int   edg_b = edg_n & 0xFF;
    int   r2 = radius * radius;
    int   x0 = cx - radius, x1 = cx + radius;
    int   y0 = cy - radius, y1 = cy + radius;
    int   px, py;

    if (g_back_buffer == NULL || radius <= 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_gfx_mode.width)  x1 = g_gfx_mode.width;
    if (y1 > (int)g_gfx_mode.height) y1 = g_gfx_mode.height;

    for (py = y0; py < y1; py++) {
        UINT32 *row = g_back_buffer + (UINT32)py * g_gfx_mode.ppsl;
        int dy = py - cy;
        int dy2 = dy * dy;
        for (px = x0; px < x1; px++) {
            int dx = px - cx;
            int dist2 = dx * dx + dy2;
            if (dist2 > r2) continue;
            {
                int dist = (int)isqrt((UINT32)dist2);
                int t = dist * 256 / radius;
                int cr = ctr_r + (edg_r - ctr_r) * t / 256;
                int cg = ctr_g + (edg_g - ctr_g) * t / 256;
                int cb = ctr_b + (edg_b - ctr_b) * t / 256;
                row[px] = (UINT32)((cr << 16) | (cg << 8) | cb);
            }
        }
    }
}

void fb_draw_text(int x, int y, const CHAR16 *str, UINT32 color)
{
    int cx = x;
    if (str == NULL) return;
    while (*str != 0) {
        font_draw_char(cx, y, *str, color);
        cx += font_get_width();
        str++;
    }
}

/* 绘制位图（ARGB 格式） */
void fb_draw_bitmap(int x, int y, int w, int h, const UINT32 *bitmap)
{
    int px, py;

    if (g_back_buffer == NULL || bitmap == NULL) return;
    if (x < 0 || y < 0) return;

    for (py = 0; py < h; py++) {
        int sy = y + py;
        if (sy >= (int)g_gfx_mode.height) break;
        for (px = 0; px < w; px++) {
            int sx = x + px;
            if (sx >= (int)g_gfx_mode.width) break;
            UINT32 pixel = bitmap[py * w + px];
            UINT8 alpha = (pixel >> 24) & 0xFF;
            UINT32 color = pixel & 0x00FFFFFF;
            if (alpha > 0) {
                fb_blend_pixel(sx, sy, color, alpha);
            }
        }
    }
}

/* 绘制窗口阴影（带 alpha 渐变） */
void fb_draw_shadow(int x, int y, int w, int h, int r, int depth)
{
    int i;
    
    if (g_back_buffer == NULL) return;
    
    for (i = 1; i <= depth; i++) {
        UINT8 alpha = 128 - (i * 128 / (depth + 1));
        if (alpha < 32) alpha = 32;
        int px;
        for (px = x + i; px < x + w + i; px++) {
            if ((UINT32)px >= g_gfx_mode.width || (UINT32)(y + i) >= g_gfx_mode.height)
                continue;
            fb_blend_pixel(px, y + i, GFX_RGB(0, 0, 0), alpha);
        }
    }
}

void fb_draw_round_rect_border(int x, int y, int w, int h, int r, UINT32 color)
{
    UINT32 native = color_to_native(color);
    int px, py;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    
    if (g_back_buffer == NULL) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    
    for (px = x0 + r; px < x1 - r; px++) {
        if (px >= 0 && px < (int)g_gfx_mode.width && y0 >= 0 && y0 < (int)g_gfx_mode.height)
            g_back_buffer[y0 * g_gfx_mode.ppsl + px] = native;
    }
    
    for (px = x0 + r; px < x1 - r; px++) {
        if (px >= 0 && px < (int)g_gfx_mode.width && y1 - 1 >= 0 && y1 - 1 < (int)g_gfx_mode.height)
            g_back_buffer[(y1 - 1) * g_gfx_mode.ppsl + px] = native;
    }
    
    for (py = y0 + r; py < y1 - r; py++) {
        if (x0 >= 0 && x0 < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height)
            g_back_buffer[py * g_gfx_mode.ppsl + x0] = native;
    }
    
    for (py = y0 + r; py < y1 - r; py++) {
        if (x1 - 1 >= 0 && x1 - 1 < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height)
            g_back_buffer[py * g_gfx_mode.ppsl + (x1 - 1)] = native;
    }
    
    for (py = y0; py < y0 + r; py++) {
        int dy = py - (y0 + r);
        int dx = isqrt((UINT32)(r * r - dy * dy));
        int cx = x0 + r - dx;
        if (cx >= 0 && cx < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height)
            g_back_buffer[py * g_gfx_mode.ppsl + cx] = native;
    }
    for (py = y0; py < y0 + r; py++) {
        int dy = py - (y0 + r);
        int dx = isqrt((UINT32)(r * r - dy * dy));
        int cx = x0 + w - r + dx;
        if (cx >= 0 && cx < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height)
            g_back_buffer[py * g_gfx_mode.ppsl + cx] = native;
    }
    for (py = y1 - r; py < y1; py++) {
        int dy = py - (y1 - r);
        int dx = isqrt((UINT32)(r * r - dy * dy));
        int cx = x0 + r - dx;
        if (cx >= 0 && cx < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height)
            g_back_buffer[py * g_gfx_mode.ppsl + cx] = native;
    }
    for (py = y1 - r; py < y1; py++) {
        int dy = py - (y1 - r);
        int dx = isqrt((UINT32)(r * r - dy * dy));
        int cx = x0 + w - r + dx;
        if (cx >= 0 && cx < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height)
            g_back_buffer[py * g_gfx_mode.ppsl + cx] = native;
    }
}

/* 读取像素（从后备缓冲） */
UINT32 fb_get_pixel(int x, int y)
{
    if (g_back_buffer == NULL) return 0;
    if (x < 0 || x >= (int)g_gfx_mode.width || y < 0 || y >= (int)g_gfx_mode.height)
        return 0;
    UINT32 native = g_back_buffer[y * g_gfx_mode.ppsl + x];
    UINT32 r = (native >> g_r_shift) & 0xFF;
    UINT32 g = (native >> g_g_shift) & 0xFF;
    UINT32 b = (native >> g_b_shift) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

/* 渐变矩形（垂直渐变） */
void fb_gradient_rect(int x, int y, int w, int h, UINT32 top, UINT32 bottom)
{
    if (g_back_buffer == NULL) return;
    
    int r0 = (top >> 16) & 0xFF;
    int g0 = (top >> 8) & 0xFF;
    int b0 = top & 0xFF;
    int r1 = (bottom >> 16) & 0xFF;
    int g1 = (bottom >> 8) & 0xFF;
    int b1 = bottom & 0xFF;
    
    int dr = r1 - r0;
    int dg = g1 - g0;
    int db = b1 - b0;
    
    for (int py = 0; py < h; py++) {
        int t = py * 256 / (h > 1 ? h - 1 : 1);
        UINT32 color = GFX_RGB(
            r0 + (dr * t) >> 8,
            g0 + (dg * t) >> 8,
            b0 + (db * t) >> 8);
        fb_fill_rect(x, y + py, w, 1, color);
    }
}

/* 计算文本宽度 */
UINT32 fb_text_width(const CHAR16 *str)
{
    UINT32 width = 0;
    while (*str) {
        width += 8;
        str++;
    }
    return width;
}

/* 内发光效果（实现柔和边界） */
void fb_inner_glow(int x, int y, int w, int h, int radius, UINT32 color, UINT8 intensity)
{
    if (g_back_buffer == NULL) return;
    if (radius <= 0) return;
    
    UINT8 alpha = intensity;
    UINT32 r = (color >> 16) & 0xFF;
    UINT32 g = (color >> 8) & 0xFF;
    UINT32 b = color & 0xFF;
    
    /* 上边缘发光 */
    for (int i = 0; i < radius; i++) {
        UINT8 row_alpha = alpha * (radius - i) / radius;
        for (int px = x + i; px < x + w - i; px++) {
            if (px >= 0 && px < (int)g_gfx_mode.width && y + i >= 0 && y + i < (int)g_gfx_mode.height) {
                fb_blend_pixel(px, y + i, GFX_RGB(r, g, b), row_alpha);
            }
        }
    }
    
    /* 下边缘发光 */
    for (int i = 0; i < radius; i++) {
        UINT8 row_alpha = alpha * (radius - i) / radius;
        for (int px = x + i; px < x + w - i; px++) {
            if (px >= 0 && px < (int)g_gfx_mode.width && y + h - 1 - i >= 0 && y + h - 1 - i < (int)g_gfx_mode.height) {
                fb_blend_pixel(px, y + h - 1 - i, GFX_RGB(r, g, b), row_alpha);
            }
        }
    }
    
    /* 左边缘发光 */
    for (int i = 0; i < radius; i++) {
        UINT8 col_alpha = alpha * (radius - i) / radius;
        for (int py = y + i; py < y + h - i; py++) {
            if (x + i >= 0 && x + i < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height) {
                fb_blend_pixel(x + i, py, GFX_RGB(r, g, b), col_alpha);
            }
        }
    }
    
    /* 右边缘发光 */
    for (int i = 0; i < radius; i++) {
        UINT8 col_alpha = alpha * (radius - i) / radius;
        for (int py = y + i; py < y + h - i; py++) {
            if (x + w - 1 - i >= 0 && x + w - 1 - i < (int)g_gfx_mode.width && py >= 0 && py < (int)g_gfx_mode.height) {
                fb_blend_pixel(x + w - 1 - i, py, GFX_RGB(r, g, b), col_alpha);
            }
        }
    }
}

void fb_swap(void)
{
    if (g_back_buffer == NULL) return;
    if (g_gfx_mode.fb_base == 0) return;

    /* 使用 memcpy 直接写入帧缓冲（不依赖 UEFI Boot Services）
     * ExitBootServices 后 g_ST->BootServices->CopyMem 不可用 */
    memcpy((void *)(uintptr_t)g_gfx_mode.fb_base,
           (void *)g_back_buffer, g_back_size);
}
