/* ---------------------------------------------------------------------------
 * DimensOS 内核服务 API
 * ---------------------------------------------------------------------------
 * 文件: kernel/api/gfx_api.h
 * 描述: 内核图形服务接口定义，供用户态软件调用
 * 遵循 Windows 模型：内核提供 API，软件实现 UI
 * --------------------------------------------------------------------------- */

#ifndef __GFX_API_H__
#define __GFX_API_H__

#include <efi.h>

/* ---------------------------------------------------------------------------
 * 像素格式定义
 * --------------------------------------------------------------------------- */
typedef UINT32 gfx_color_t;

#define GFX_RGB(r, g, b)  ((gfx_color_t)((((r) & 0xFF) << 16) | (((g) & 0xFF) << 8) | ((b) & 0xFF)))
#define GFX_A(color, a)   ((color) | (((a) & 0xFF) << 24))

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
    int x;
    int y;
    BOOLEAN left_button;
    BOOLEAN right_button;
} gfx_mouse_state_t;

/* ---------------------------------------------------------------------------
 * 键盘状态
 * --------------------------------------------------------------------------- */
typedef struct {
    CHAR16 key;
    BOOLEAN pressed;
} gfx_key_state_t;

/* ---------------------------------------------------------------------------
 * 图标 ID（用户态通过 ID 访问内核图标资源）
 * --------------------------------------------------------------------------- */
typedef enum {
    GFX_ICON_NONE = 0,
    GFX_ICON_THISPC,
    GFX_ICON_FILES,
    GFX_ICON_SHELL,
    GFX_ICON_NETWORK,
    GFX_ICON_SETTINGS,
    GFX_ICON_ABOUT,
    GFX_ICON_COUNT
} gfx_icon_id_t;

/* ---------------------------------------------------------------------------
 * 图形服务函数表（类似 Windows 的 GDI API）
 * --------------------------------------------------------------------------- */
typedef struct {
    /* 初始化/清理 */
    EFI_STATUS (*init)(void);
    void (*done)(void);

    /* 模式查询 */
    void (*get_mode)(gfx_mode_info_t *info);

    /* 帧缓冲操作 */
    void (*swap)(void);
    void (*clear)(gfx_color_t color);

    /* 像素级操作 */
    void (*put_pixel)(int x, int y, gfx_color_t color);
    gfx_color_t (*get_pixel)(int x, int y);

    /* 矩形操作 */
    void (*fill_rect)(int x, int y, int w, int h, gfx_color_t color);
    void (*fill_round_rect)(int x, int y, int w, int h, int radius, gfx_color_t color);

    /* 渐变操作 */
    void (*gradient_rect)(int x, int y, int w, int h, gfx_color_t top, gfx_color_t bottom);
    void (*gradient_round_rect)(int x, int y, int w, int h, int radius, gfx_color_t top, gfx_color_t bottom);
    void (*radial_gradient)(int cx, int cy, int radius, gfx_color_t center, gfx_color_t edge);

    /* 文字操作 */
    void (*draw_text)(int x, int y, const CHAR16 *text, gfx_color_t color);
    UINT32 (*text_width)(const CHAR16 *text);
    UINT32 (*text_height)(void);

    /* 图标操作（用户态通过 ID 访问内核图标资源） */
    void (*draw_icon)(int x, int y, gfx_icon_id_t icon_id);
    
    /* 光标操作 */
    void (*draw_cursor)(int x, int y);
    
    /* 阴影效果 */
    void (*draw_shadow)(int x, int y, int w, int h, int radius, int depth);
    
    /* Alpha 混合操作（实现毛玻璃效果） */
    void (*blend_rect)(int x, int y, int w, int h, gfx_color_t color, UINT8 alpha);
    void (*blend_round_rect)(int x, int y, int w, int h, int radius, gfx_color_t color, UINT8 alpha);
    
    /* 内发光效果（替代生硬边框） */
    void (*inner_glow)(int x, int y, int w, int h, int radius, gfx_color_t color, UINT8 intensity);

    /* 输入设备 */
    void (*poll_mouse)(gfx_mouse_state_t *state);
    BOOLEAN (*poll_keyboard)(gfx_key_state_t *state);

    /* 定时器 */
    EFI_STATUS (*create_timer)(UINT64 period_us, void (*callback)(void));
    void (*destroy_timer)(void);

} gfx_api_t;

/* ---------------------------------------------------------------------------
 * 全局 API 指针（软件通过此表调用内核服务）
 * --------------------------------------------------------------------------- */
extern gfx_api_t *g_gfx_api;

/* ---------------------------------------------------------------------------
 * API 初始化函数（注册所有函数指针）
 * --------------------------------------------------------------------------- */
void gfx_api_init_all(void);

#endif /* __GFX_API_H__ */
