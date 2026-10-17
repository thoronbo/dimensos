/* ---------------------------------------------------------------------------
 * 图形服务 API（内核暴露给用户态应用的接口）
 * ---------------------------------------------------------------------------
 * 架构设计：
 *   - 内核侧：gop.c / fb.c / font.c 提供底层驱动
 *   - 服务层：gfx_service.c 封装并暴露统一 API
 *   - 应用层：desktop_app.c 通过函数指针表调用服务
 * --------------------------------------------------------------------------- */

#ifndef __GFX_SERVICE_H__
#define __GFX_SERVICE_H__

#include <efi.h>

/* ---------------------------------------------------------------------------
 * 像素格式定义
 * --------------------------------------------------------------------------- */
typedef UINT32 gfx_color_t;

#define GFX_RGB(r, g, b)  (((r) << 16) | ((g) << 8) | (b))
#define GFX_GET_R(c)      ((c >> 16) & 0xFF)
#define GFX_GET_G(c)      ((c >> 8) & 0xFF)
#define GFX_GET_B(c)      (c & 0xFF)

/* ---------------------------------------------------------------------------
 * 模式信息
 * --------------------------------------------------------------------------- */
typedef struct {
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT32 bpp;
} gfx_mode_info_t;

/* ---------------------------------------------------------------------------
 * 鼠标状态
 * --------------------------------------------------------------------------- */
typedef struct {
    INT32 x;
    INT32 y;
    BOOLEAN left_button;
    BOOLEAN right_button;
} gfx_mouse_state_t;

/* ---------------------------------------------------------------------------
 * 键盘状态
 * --------------------------------------------------------------------------- */
typedef struct {
    UINT16 keycode;
    BOOLEAN pressed;
} gfx_key_state_t;

/* ---------------------------------------------------------------------------
 * 图形服务函数指针表
 * --------------------------------------------------------------------------- */
typedef struct {
    /* 初始化与模式查询 */
    EFI_STATUS (*init)(void);
    EFI_STATUS (*done)(void);
    void       (*get_mode_info)(gfx_mode_info_t *info);

    /* 帧缓冲操作 */
    void       (*fill_rect)(UINT32 x, UINT32 y, UINT32 w, UINT32 h, gfx_color_t color);
    void       (*put_pixel)(UINT32 x, UINT32 y, gfx_color_t color);
    void       (*gradient_rect)(UINT32 x, UINT32 y, UINT32 w, UINT32 h,
                                gfx_color_t top, gfx_color_t bottom);
    void       (*radial_gradient)(UINT32 cx, UINT32 cy, UINT32 radius,
                                   gfx_color_t center, gfx_color_t edge);
    void       (*round_rect)(UINT32 x, UINT32 y, UINT32 w, UINT32 h,
                             UINT32 radius, gfx_color_t color);
    void       (*gradient_round_rect)(UINT32 x, UINT32 y, UINT32 w, UINT32 h,
                                      UINT32 radius, gfx_color_t top, gfx_color_t bottom);

    /* 文字渲染 */
    UINT32     (*get_font_width)(void);
    UINT32     (*get_font_height)(void);
    void       (*draw_text)(UINT3