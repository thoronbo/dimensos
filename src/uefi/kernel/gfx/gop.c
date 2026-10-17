/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - UEFI 图形输出协议 (GOP) 初始化
 *
 * 文件: kernel/gfx/gop.c
 * 描述: GOP 协议定位与模式切换实现。绝对自研，仅依赖 UEFI 规范。
 */

#include <efi.h>
#include <eficon.h>
#include <efiprot.h>
#include "gop.h"

/* GOP Protocol GUID */
static EFI_GUID gGopGuid =
    {0x9042a9de, 0x23dc, 0x4a38,
     {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

/* 全局图形模式信息 */
gfx_mode_t g_gfx_mode;

/* 获取当前系统表指针（由 main.c 设置的 g_ST） */
extern EFI_SYSTEM_TABLE *g_ST;

EFI_STATUS gfx_init(void)
{
    EFI_STATUS                   Status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
    UINTN                        SizeOfInfo = 0;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *ModeInfo = NULL;
    UINT32                       BestMode = 0;
    UINT32                       BestArea = 0;
    UINT32                       ModeIndex;
    UINT32                       NumModes;

    if (g_ST == NULL || g_ST->BootServices == NULL)
        return EFI_INVALID_PARAMETER;

    /* 定位 GOP 协议 */
    Status = g_ST->BootServices->LocateProtocol(
        &gGopGuid, NULL, (VOID **)&Gop);
    if (EFI_ERROR(Status) || Gop == NULL)
        return Status;

    NumModes = Gop->Mode->MaxMode;

    /* 遍历所有模式，选择最大分辨率（支持 BGRA/RGBA/BitMask 32 位格式） */
    for (ModeIndex = 0; ModeIndex < NumModes; ModeIndex++)
    {
        Status = Gop->QueryMode(Gop, ModeIndex, &SizeOfInfo, &ModeInfo);
        if (EFI_ERROR(Status) || ModeInfo == NULL)
            continue;

        /* 排除 BltOnly（无直接帧缓冲访问），支持至 1920x1080 */
        if (ModeInfo->PixelFormat != PixelBltOnly &&
            ModeInfo->HorizontalResolution <= 1920 &&
            ModeInfo->VerticalResolution <= 1080)
        {
            UINT32 area = ModeInfo->HorizontalResolution *
                          ModeInfo->VerticalResolution;
            BOOLEAN ok = FALSE;

            if (ModeInfo->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
                ok = TRUE;
            } else if (ModeInfo->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
                ok = TRUE;
            } else if (ModeInfo->PixelFormat == PixelBitMask) {
                /* 仅接受每通道 8 位的 32 位 BitMask */
                UINT32 rm = ModeInfo->PixelInformation.RedMask;
                UINT32 gm = ModeInfo->PixelInformation.GreenMask;
                UINT32 bm = ModeInfo->PixelInformation.BlueMask;
                if (rm != 0 && gm != 0 && bm != 0)
                    ok = TRUE;
            }

            if (ok && area > BestArea) {
                BestArea = area;
                BestMode = ModeIndex;
            }
        }

        g_ST->BootServices->FreePool(ModeInfo);
        ModeInfo = NULL;
    }

    if (BestArea == 0)
        return EFI_UNSUPPORTED;

    /* 切换到最佳模式 */
    Status = Gop->SetMode(Gop, BestMode);
    if (EFI_ERROR(Status))
        return Status;

    /* 记录帧缓冲信息 */
    g_gfx_mode.fb_base  = Gop->Mode->FrameBufferBase;
    g_gfx_mode.fb_size  = Gop->Mode->FrameBufferSize;
    g_gfx_mode.width    = Gop->Mode->Info->HorizontalResolution;
    g_gfx_mode.height   = Gop->Mode->Info->VerticalResolution;
    g_gfx_mode.ppsl     = Gop->Mode->Info->PixelsPerScanLine;
    g_gfx_mode.pixel_fmt= Gop->Mode->Info->PixelFormat;

    /* 计算每扫描线字节数（32 位像素 = 4 字节） */
    g_gfx_mode.pitch    = g_gfx_mode.ppsl * 4;

    /* 记录颜色位掩码（fb.c 据此转换颜色） */
    switch (g_gfx_mode.pixel_fmt) {
        case PixelBlueGreenRedReserved8BitPerColor:
            g_gfx_mode.r_mask = 0x00FF0000;
            g_gfx_mode.g_mask = 0x0000FF00;
            g_gfx_mode.b_mask = 0x000000FF;
            break;
        case PixelRedGreenBlueReserved8BitPerColor:
            g_gfx_mode.r_mask = 0x000000FF;
            g_gfx_mode.g_mask = 0x0000FF00;
            g_gfx_mode.b_mask = 0x00FF0000;
            break;
        case PixelBitMask:
            g_gfx_mode.r_mask = Gop->Mode->Info->PixelInformation.RedMask;
            g_gfx_mode.g_mask = Gop->Mode->Info->PixelInformation.GreenMask;
            g_gfx_mode.b_mask = Gop->Mode->Info->PixelInformation.BlueMask;
            break;
        default:
            g_gfx_mode.r_mask = 0x00FF0000;
            g_gfx_mode.g_mask = 0x0000FF00;
            g_gfx_mode.b_mask = 0x000000FF;
            break;
    }

    return EFI_SUCCESS;
}

void gfx_done(void)
{
    /* GOP 模式切换在 Boot Services 退出前保持有效，无需显式恢复 */
    /* 清零全局信息 */
    g_gfx_mode.fb_base = 0;
    g_gfx_mode.fb_size = 0;
    g_gfx_mode.width   = 0;
    g_gfx_mode.height  = 0;
}
