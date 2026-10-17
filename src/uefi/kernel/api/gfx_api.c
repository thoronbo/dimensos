/* ---------------------------------------------------------------------------
 * DimensOS 内核服务 API 实现
 * ---------------------------------------------------------------------------
 * 文件: kernel/api/gfx_api.c
 * 描述: 将内核图形驱动封装为统一 API，供用户态软件调用
 * --------------------------------------------------------------------------- */

#include "gfx_api.h"
#include "../gfx/gop.h"
#include "../gfx/fb.h"
#include "../gfx/font.h"
#include "../gfx/icons.h"
#include "../lib/io.h"

extern EFI_SYSTEM_TABLE *g_ST;

/* ---------------------------------------------------------------------------
 * 全局 API 实例
 * --------------------------------------------------------------------------- */
static gfx_api_t g_gfx_api_instance;
gfx_api_t *g_gfx_api = &g_gfx_api_instance;

/* ---------------------------------------------------------------------------
 * 定时器相关
 * --------------------------------------------------------------------------- */
static EFI_EVENT g_timer_event = NULL;
static void (*g_timer_callback)(void) = NULL;

static VOID EFIAPI timer_notify(IN EFI_EVENT Event, IN VOID *Context)
{
    if (g_timer_callback)
        g_timer_callback();
}

/* ---------------------------------------------------------------------------
 * API 实现
 * --------------------------------------------------------------------------- */

static EFI_STATUS gfx_api_init(void)
{
    EFI_STATUS st;

    st = gfx_init();
    if (EFI_ERROR(st)) {
        uart_puts("[GFX] gfx_init failed\n");
        return st;
    }

    st = fb_init();
    if (EFI_ERROR(st)) {
        uart_puts("[GFX] fb_init failed\n");
        gfx_done();
        return st;
    }

    uart_puts("[GFX] API initialized\n");
    return EFI_SUCCESS;
}

static void gfx_api_done(void)
{
    fb_done();
    gfx_done();
}

static void gfx_api_get_mode(gfx_mode_info_t *info)
{
    if (info) {
        info->width = g_gfx_mode.width;
        info->height = g_gfx_mode.height;
        info->pitch = g_gfx_mode.pitch;
        info->bpp = 32;
    }
}

static void gfx_api_swap(void)
{
    fb_swap();
}

static void gfx_api_clear(gfx_color_t color)
{
    fb_fill_rect(0, 0, g_gfx_mode.width, g_gfx_mode.height, color);
}

static void gfx_api_put_pixel(int x, int y, gfx_color_t color)
{
    fb_put_pixel(x, y, color);
}

static gfx_color_t gfx_api_get_pixel(int x, int y)
{
    return fb_get_pixel(x, y);
}

static void gfx_api_fill_rect(int x, int y, int w, int h, gfx_color_t color)
{
    fb_fill_rect(x, y, w, h, color);
}

static void gfx_api_fill_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color)
{
    fb_fill_round_rect(x, y, w, h, radius, color);
}

static void gfx_api_gradient_rect(int x, int y, int w, int h, gfx_color_t top, gfx_color_t bottom)
{
    fb_gradient_rect(x, y, w, h, top, bottom);
}

static void gfx_api_gradient_round_rect(int x, int y, int w, int h, int radius, gfx_color_t top, gfx_color_t bottom)
{
    fb_gradient_round_rect(x, y, w, h, radius, top, bottom);
}

static void gfx_api_radial_gradient(int cx, int cy, int radius, gfx_color_t center, gfx_color_t edge)
{
    fb_radial_gradient(cx, cy, radius, center, edge);
}

static void gfx_api_draw_text(int x, int y, const CHAR16 *text, gfx_color_t color)
{
    fb_draw_text(x, y, text, color);
}

static UINT32 gfx_api_text_width(const CHAR16 *text)
{
    return fb_text_width(text);
}

static UINT32 gfx_api_text_height(void)
{
    return 16;
}

/* ---------------------------------------------------------------------------
 * 图标 API（用户态通过 ID 访问内核图标资源）
 * --------------------------------------------------------------------------- */
static void gfx_api_draw_icon(int x, int y, gfx_icon_id_t icon_id)
{
    const UINT32 *icon = NULL;
    
    switch (icon_id) {
        case GFX_ICON_THISPC:   icon = icon_thispc;   break;
        case GFX_ICON_FILES:    icon = icon_files;    break;
        case GFX_ICON_SHELL:    icon = icon_shell;    break;
        case GFX_ICON_NETWORK:  icon = icon_network;  break;
        case GFX_ICON_SETTINGS: icon = icon_settings; break;
        case GFX_ICON_ABOUT:    icon = icon_about;    break;
        default: return;
    }
    
    if (icon) {
        fb_draw_bitmap(x, y, ICON_WIDTH, ICON_HEIGHT, icon);
    }
}

/* ---------------------------------------------------------------------------
 * 阴影 API（绘制窗口阴影效果）
 * --------------------------------------------------------------------------- */
static void gfx_api_draw_shadow(int x, int y, int w, int h, int radius, int depth)
{
    fb_draw_shadow(x, y, w, h, radius, depth);
}

/* ---------------------------------------------------------------------------
 * Alpha 混合 API（实现毛玻璃效果）
 * --------------------------------------------------------------------------- */
static void gfx_api_blend_rect(int x, int y, int w, int h, gfx_color_t color, UINT8 alpha)
{
    fb_blend_rect(x, y, w, h, color, alpha);
}

static void gfx_api_blend_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color, UINT8 alpha)
{
    fb_blend_round_rect(x, y, w, h, radius, color, alpha);
}

/* ---------------------------------------------------------------------------
 * 内发光 API（替代生硬边框）
 * --------------------------------------------------------------------------- */
static void gfx_api_inner_glow(int x, int y, int w, int h, int radius, gfx_color_t color, UINT8 intensity)
{
    fb_inner_glow(x, y, w, h, radius, color, intensity);
}

/* ---------------------------------------------------------------------------
 * 光标 API（绘制标准箭头光标）
 * --------------------------------------------------------------------------- */
static void gfx_api_draw_cursor(int x, int y)
{
    UINT32 white = GFX_RGB(255, 255, 255);
    UINT32 black = GFX_RGB(0, 0, 0);
    UINT32 gray = GFX_RGB(68, 68, 68);
    
    /* 标准 16x16 箭头光标（热点在左上角） */
    /* 外形: 箭头指向左上方 */
    
    /* 白色轮廓 */
    /* 横线 */
    fb_fill_rect(x + 0, y + 6, 7, 1, white);
    fb_fill_rect(x + 0, y + 7, 5, 1, white);
    fb_fill_rect(x + 0, y + 8, 3, 1, white);
    
    /* 竖线 */
    fb_fill_rect(x + 6, y + 0, 1, 7, white);
    fb_fill_rect(x + 5, y + 0, 1, 5, white);
    fb_fill_rect(x + 4, y + 0, 1, 3, white);
    
    /* 斜线 */
    for (int i = 0; i < 6; i++) {
        fb_put_pixel(x + 7 + i, y + 7 + i, white);
    }
    for (int i = 0; i < 4; i++) {
        fb_put_pixel(x + 9 + i, y + 5 + i, white);
    }
    for (int i = 0; i < 4; i++) {
        fb_put_pixel(x + 5 + i, y + 9 + i, white);
    }
    
    /* 黑色内部（箭头尖端） */
    fb_fill_rect(x + 1, y + 7, 3, 1, black);
    fb_fill_rect(x + 6, y + 1, 1, 3, black);
    fb_put_pixel(x + 7, y + 7, black);
    
    /* 灰色阴影（右下侧） */
    for (int i = 0; i < 5; i++) {
        fb_put_pixel(x + 8 + i, y + 8 + i, gray);
    }
}

/* ---------------------------------------------------------------------------
 * 输入设备 API（鼠标/键盘）
 * --------------------------------------------------------------------------- */
static EFI_SIMPLE_POINTER_PROTOCOL *g_mouse_proto = NULL;
static int g_mouse_x = 0, g_mouse_y = 0;
static BOOLEAN g_mouse_enabled = FALSE;

static void mouse_init_internal(void)
{
    EFI_STATUS st;
    static EFI_GUID gSimplePointerGuid =
        {0x31878c87, 0xb75, 0x11d5,
         {0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};

    st = g_ST->BootServices->LocateProtocol(
        &gSimplePointerGuid, NULL, (VOID **)&g_mouse_proto);
    if (EFI_ERROR(st)) {
        uart_puts("[MOUSE] Simple Pointer not found\n");
        return;
    }

    st = g_mouse_proto->Reset(g_mouse_proto, FALSE);
    if (EFI_ERROR(st)) {
        uart_puts("[MOUSE] Reset failed\n");
        g_mouse_proto = NULL;
        return;
    }

    g_mouse_x = g_gfx_mode.width / 2;
    g_mouse_y = g_gfx_mode.height / 2;
    g_mouse_enabled = TRUE;
    uart_puts("[MOUSE] initialized\n");
}

static void gfx_api_poll_mouse(gfx_mouse_state_t *state)
{
    EFI_STATUS st;
    EFI_SIMPLE_POINTER_STATE mouse_state;

    if (!state || !g_mouse_enabled || !g_mouse_proto) {
        state->x = 0;
        state->y = 0;
        state->left_button = FALSE;
        state->right_button = FALSE;
        return;
    }

    st = g_mouse_proto->GetState(g_mouse_proto, &mouse_state);
    if (EFI_ERROR(st)) {
        state->x = g_mouse_x;
        state->y = g_mouse_y;
        state->left_button = FALSE;
        state->right_button = FALSE;
        return;
    }

    if (mouse_state.RelativeMovementX != 0 || mouse_state.RelativeMovementY != 0) {
        g_mouse_x += (int)mouse_state.RelativeMovementX;
        g_mouse_y += (int)mouse_state.RelativeMovementY;

        if (g_mouse_x < 0) g_mouse_x = 0;
        if (g_mouse_y < 0) g_mouse_y = 0;
        if ((UINT32)g_mouse_x >= g_gfx_mode.width)
            g_mouse_x = g_gfx_mode.width - 1;
        if ((UINT32)g_mouse_y >= g_gfx_mode.height)
            g_mouse_y = g_gfx_mode.height - 1;
    }

    state->x = g_mouse_x;
    state->y = g_mouse_y;
    state->left_button = mouse_state.LeftButton;
    state->right_button = mouse_state.RightButton;
}

static BOOLEAN gfx_api_poll_keyboard(gfx_key_state_t *state)
{
    EFI_STATUS st;
    EFI_INPUT_KEY key;

    if (!state || !g_ST || !g_ST->ConIn) {
        state->key = 0;
        state->pressed = FALSE;
        return FALSE;
    }

    st = g_ST->ConIn->ReadKeyStroke(g_ST->ConIn, &key);
    if (EFI_ERROR(st)) {
        state->key = 0;
        state->pressed = FALSE;
        return FALSE;
    }

    state->key = key.UnicodeChar;
    state->pressed = (key.ScanCode != 0 || key.UnicodeChar != 0);
    return TRUE;
}

static EFI_STATUS gfx_api_create_timer(UINT64 period_us, void (*callback)(void))
{
    EFI_STATUS st;

    if (g_timer_event)
        g_ST->BootServices->CloseEvent(g_timer_event);

    g_timer_callback = callback;

    st = g_ST->BootServices->CreateEvent(
        EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY,
        timer_notify, NULL, &g_timer_event);
    if (EFI_ERROR(st))
        return st;

    st = g_ST->BootServices->SetTimer(
        g_timer_event, TimerPeriodic, period_us);
    return st;
}

static void gfx_api_destroy_timer(void)
{
    if (g_timer_event) {
        g_ST->BootServices->CloseEvent(g_timer_event);
        g_timer_event = NULL;
    }
    g_timer_callback = NULL;
}

/* ---------------------------------------------------------------------------
 * API 初始化（注册所有函数指针）
 * --------------------------------------------------------------------------- */
void gfx_api_init_all(void)
{
    g_gfx_api_instance.init = gfx_api_init;
    g_gfx_api_instance.done = gfx_api_done;
    g_gfx_api_instance.get_mode = gfx_api_get_mode;
    g_gfx_api_instance.swap = gfx_api_swap;
    g_gfx_api_instance.clear = gfx_api_clear;
    g_gfx_api_instance.put_pixel = gfx_api_put_pixel;
    g_gfx_api_instance.get_pixel = gfx_api_get_pixel;
    g_gfx_api_instance.fill_rect = gfx_api_fill_rect;
    g_gfx_api_instance.fill_round_rect = gfx_api_fill_round_rect;
    g_gfx_api_instance.gradient_rect = gfx_api_gradient_rect;
    g_gfx_api_instance.gradient_round_rect = gfx_api_gradient_round_rect;
    g_gfx_api_instance.radial_gradient = gfx_api_radial_gradient;
    g_gfx_api_instance.draw_text = gfx_api_draw_text;
    g_gfx_api_instance.text_width = gfx_api_text_width;
    g_gfx_api_instance.text_height = gfx_api_text_height;
    g_gfx_api_instance.draw_icon = gfx_api_draw_icon;
    g_gfx_api_instance.draw_cursor = gfx_api_draw_cursor;
    g_gfx_api_instance.draw_shadow = gfx_api_draw_shadow;
    g_gfx_api_instance.blend_rect = gfx_api_blend_rect;
    g_gfx_api_instance.blend_round_rect = gfx_api_blend_round_rect;
    g_gfx_api_instance.inner_glow = gfx_api_inner_glow;
    g_gfx_api_instance.poll_mouse = gfx_api_poll_mouse;
    g_gfx_api_instance.poll_keyboard = gfx_api_poll_keyboard;
    g_gfx_api_instance.create_timer = gfx_api_create_timer;
    g_gfx_api_instance.destroy_timer = gfx_api_destroy_timer;

    /* 初始化图标数据（在内核中） */
    icons_init();
    
    /* 初始化鼠标（在 API 注册后） */
    mouse_init_internal();
}
