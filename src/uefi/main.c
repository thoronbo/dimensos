/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 1: UEFI Kernel Core
 *
 * 文件: main.c
 * 描述: UEFI 引导程序入口，负责初始化内核基础组件:
 *         GDT、IDT、分页系统、物理内存管理器
 * 初始化顺序:
 *   1. 获取并打印 UEFI 内存映射
 *   2. pmm_init()  - 初始化物理内存管理器（读取UEFI内存映射）
 *   3. gdt_init()  - 加载全局描述符表
 *   4. idt_init()  - 加载中断描述符表
 *   5. paging_init() - 建立内核页表并切换 CR3
 *   6. 输出成功信息，等待按键后退出
 * 编译目标: x86_64 UEFI PE32+ (bootx64.efi)
 * 框架: gnu-efi (仅使用头文件类型定义，不链接 efilib)
 */

#include <efi.h>
#include <eficon.h>
#include <efiprot.h>

/* 内核组件 */
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/fs/fat32.h"
#include "kernel/paging.h"
#include "kernel/pmm.h"
#include "kernel/lib/io.h"

/* 调度与Syscall组件 (Stage 2) */
#include "kernel/sched/sched.h"
#include "kernel/sched/timer.h"

/* 文件系统抽象层 (Stage 3) */
#include "kernel/fs/fs.h"

/* 命令行 Shell (Stage 4) */
#include "kernel/shell/shell.h"

/* 网络层组件 (Stage 5) */
#include "kernel/net/e1000.h"

/* 图形化组件 (Stage 6) */
#include "kernel/gfx/gop.h"
#include "kernel/gfx/fb.h"

/* ---------------------------------------------------------------
 * 内存类型名称映射表 (用于调试输出)
 * --------------------------------------------------------------- */
static const CHAR16 *mmap_type_names[] = {
    L"Reserved",
    L"LoaderCode",
    L"LoaderData",
    L"BootServicesCode",
    L"BootServicesData",
    L"RuntimeServicesCode",
    L"RuntimeServicesData",
    L"ConventionalMemory",
    L"UnusableMemory",
    L"ACPIReclaimMemory",
    L"ACPIMemoryNVS",
    L"MemoryMappedIO",
    L"MemoryMappedIOPortSpace",
    L"PalCode",
    L"PersistentMemory",
    L"UnacceptedMemoryType"
};

#define MMAP_TYPE_NAME_COUNT  (sizeof(mmap_type_names) / sizeof(mmap_type_names[0]))

/* ---------------------------------------------------------------
 * 辅助函数: 将 64 位无符号整数转为宽字符串
 *
 * 在 freestanding 环境下无法使用 swprintf，此处实现一个
 * 简单的转换函数用于输出数字信息。
 * --------------------------------------------------------------- */
static void uint64_to_wstr(CHAR16 *buf, UINT64 val, UINTN buf_size)
{
    CHAR16  tmp[32];
    UINTN   idx = 0;
    UINTN   pos;
    UINT64  v = val;

    /* 逐位转换 */
    if (v == 0) {
        tmp[idx++] = L'0';
    } else {
        while (v > 0 && idx < sizeof(tmp)/sizeof(tmp[0]) - 1) {
            tmp[idx++] = L'0' + (v % 10);
            v /= 10;
        }
    }

    /* 反转 */
    pos = 0;
    while (idx > 0 && pos < buf_size - 1) {
        buf[pos++] = tmp[--idx];
    }
    buf[pos] = L'\0';
}

/* ---------------------------------------------------------------
 * 辅助函数: 将 64 位整数转为十六进制宽字符串
 * --------------------------------------------------------------- */
static void uint64_to_hex_wstr(CHAR16 *buf, UINT64 val, UINTN buf_size)
{
    static const CHAR16 hex_chars[] = L"0123456789ABCDEF";
    CHAR16  tmp[32];
    UINTN   idx = 0;
    UINTN   pos;
    UINT64  v = val;

    /* 先存储数字部分（低位在前） */
    if (v == 0) {
        tmp[idx++] = L'0';
    } else {
        while (v > 0 && idx < sizeof(tmp)/sizeof(tmp[0]) - 3) {
            tmp[idx++] = hex_chars[v & 0xF];
            v >>= 4;
        }
    }

    /* 添加 0x 前缀（反转后会变成正确顺序） */
    tmp[idx++] = L'x';
    tmp[idx++] = L'0';

    /* 反转 */
    pos = 0;
    while (idx > 0 && pos < buf_size - 1) {
        buf[pos++] = tmp[--idx];
    }
    buf[pos] = L'\0';
}

/* ---------------------------------------------------------------
 * 辅助函数: 输出字符串到控制台
 * --------------------------------------------------------------- */
static void print(EFI_SIMPLE_TEXT_OUT_PROTOCOL *conout, const CHAR16 *str)
{
    conout->OutputString(conout, (CHAR16 *)str);
}

/* ---------------------------------------------------------------
 * 辅助函数: 输出字符串 + 数字 + 换行
 * --------------------------------------------------------------- */
static void print_num(SIMPLE_TEXT_OUTPUT_INTERFACE *conout,
                      const CHAR16 *prefix, UINT64 val)
{
    CHAR16  num_buf[32];
    uint64_to_wstr(num_buf, val, 32);
    print(conout, prefix);
    print(conout, num_buf);
    print(conout, L"\r\n");
}

/* ---------------------------------------------------------------
 * 辅助函数: 输出字符串 + 十六进制数字 + 换行
 * --------------------------------------------------------------- */
static void print_hex(SIMPLE_TEXT_OUTPUT_INTERFACE *conout,
                      const CHAR16 *prefix, UINT64 val)
{
    CHAR16  hex_buf[24];
    uint64_to_hex_wstr(hex_buf, val, 24);
    print(conout, prefix);
    print(conout, hex_buf);
    print(conout, L"\r\n");
}

/* ---------------------------------------------------------------
 * UEFI 系统表全局指针
 *
 * 保存 SystemTable 指针供 SYS_WRITE syscall 使用。
 * 在 sched_run 启动前设置。
 * --------------------------------------------------------------- */
EFI_SYSTEM_TABLE *g_ST = NULL;

/* ---------------------------------------------------------------
 * UEFI Block I/O 包装 (文件系统层块设备驱动)
 *
 * EFI_BLOCK_IO_PROTOCOL GUID
 * {0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}
 * --------------------------------------------------------------- */
static EFI_GUID gBlockIoProtocolGuid =
    {0x964E5B21, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

/* UEFI Block I/O 回调: 读取扇区 */
static int uefi_block_read(void *device, uint64_t start_lba,
                            uint32_t count, void *buffer)
{
    EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)device;
    EFI_STATUS st;

    st = bio->ReadBlocks(bio, bio->Media->MediaId, start_lba,
                          (UINTN)count * bio->Media->BlockSize, buffer);
    return EFI_ERROR(st) ? -1 : 0;
}

/* UEFI Block I/O 回调: 写入扇区 */
static int uefi_block_write(void *device, uint64_t start_lba,
                             uint32_t count, const void *buffer)
{
    EFI_BLOCK_IO_PROTOCOL *bio = (EFI_BLOCK_IO_PROTOCOL *)device;
    EFI_STATUS st;

    st = bio->WriteBlocks(bio, bio->Media->MediaId, start_lba,
                           (UINTN)count * bio->Media->BlockSize, (VOID *)buffer);
    return EFI_ERROR(st) ? -1 : 0;
}

/* ---------------------------------------------------------------
 * 串口宽字符输出函数 (直接 UART I/O 版本)
 *
 * 供 g_sched_puts 使用，将 UTF-16 宽字符转换为 ASCII 通过
 * 串口 UART 直接输出。不依赖 ConOut（ExitBootServices 后失效）。
 *
 * 在 ExitBootServices 之前，此函数仍可用串口输出调试信息。
 * --------------------------------------------------------------- */
static void uart_puts_wide(const uint16_t *str)
{
    while (str && *str) {
        uint16_t ch = *str;
        if (ch == L'\n') {
            /* 确保 \n 发送前先发 \r */
            uart_putc('\r');
            uart_putc('\n');
        } else if (ch < 256) {
            /* 可打印 ASCII 或扩展字符 */
            uart_putc((char)(ch & 0xFF));
        }
        str++;
    }
}



/* ---------------------------------------------------------------
 * UEFI 应用程序入口
 * --------------------------------------------------------------- */

/* ---------------------------------------------------------------
 * ExitBootServices 后的输出辅助函数
 *
 * 通过 UART 串口直接输出，不依赖 ConOut（ExitBootServices 后失效）。
 * --------------------------------------------------------------- */
static void uart_print(const CHAR16 *str)
{
    uart_puts_wide(str);
}

static void uart_print_num(const CHAR16 *prefix, UINT64 val)
{
    CHAR16 num_buf[32];
    uint64_to_wstr(num_buf, val, 32);
    uart_puts_wide(prefix);
    uart_puts_wide(num_buf);
    uart_puts_wide(L"\r\n");
}

static void uart_print_hex(const CHAR16 *prefix, UINT64 val)
{
    CHAR16 hex_buf[24];
    uint64_to_hex_wstr(hex_buf, val, 24);
    uart_puts_wide(prefix);
    uart_puts_wide(hex_buf);
    uart_puts_wide(L"\r\n");
}

EFI_STATUS
EFIAPI
efi_main (
    IN EFI_HANDLE        ImageHandle,
    IN EFI_SYSTEM_TABLE *SystemTable
    )
{
    EFI_STATUS            Status;
    EFI_BOOT_SERVICES    *BS;
    SIMPLE_TEXT_OUTPUT_INTERFACE *ConOut;

    EFI_MEMORY_DESCRIPTOR *MemoryMap    = NULL;
    UINTN                  MemoryMapSize = 0;
    UINTN                  MapKey;
    UINTN                  DescriptorSize;
    UINT32                 DescriptorVersion;

    UINTN                  i;
    UINT64                 total_conv_memory = 0;
    UINT64                 desc_count;
    UINT8                 *desc_ptr;
    UINT32                 desc_type;
    UINT64                 desc_phys;
    UINT64                 desc_pages;

    BS     = SystemTable->BootServices;
    ConOut = SystemTable->ConOut;

    /* 立即保存系统表指针（供图形化模块等早期使用） */
    g_ST = SystemTable;

    /* -----------------------------------------------------------
     * 阶段 0: 清屏并显示启动横幅
     * ----------------------------------------------------------- */
    ConOut->ClearScreen(ConOut);

    print(ConOut, L"   _____    _                                     ____   _____ \r\n");
    print(ConOut, L"  |  __ \\  (_)                                   / __ \\ / ____| \r\n");
    print(ConOut, L"  | |  | |  _   _ __ ___     ___   _ __    ___  | |  | | (___  \r\n");
    print(ConOut, L"  | |  | | | | | '_ ` _ \\   / _ \\ | '_ \\  / __| | |  | |\\___ \\ \r\n");
    print(ConOut, L"  | |__| | | | | | | | | | |  __/ | | | | \\__ \\ | |__| |____) | \r\n");
    print(ConOut, L"  |_____/  |_| |_| |_| |_|  \\___| |_| |_| |___/  \\____/|_____/ \r\n");
    print(ConOut, L"                                                               \r\n");
    print(ConOut, L"                                                               \r\n");
    print(ConOut, L"         Stage 4: Command Line Interface\r\n");
    print(ConOut, L"        Copyright (c) 2025 DeepDimension Workshop\r\n");
    print(ConOut, L"   ------------------------------------------------------\r\n");
    print(ConOut, L"\r\n");

    /* -----------------------------------------------------------
     * 阶段 1: 获取 UEFI 内存映射
     * ----------------------------------------------------------- */
    print(ConOut, L">>> PHASE 1: Memory Map <<<\r\n\r\n");

    /* 获取所需缓冲区大小 */
    MemoryMapSize = 0;
    Status = BS->GetMemoryMap(&MemoryMapSize, NULL, &MapKey,
                              &DescriptorSize, &DescriptorVersion);

    if (Status != EFI_BUFFER_TOO_SMALL) return Status;

    /* 分配缓冲区 */
    MemoryMapSize += DescriptorSize * 8;
    Status = BS->AllocatePool(EfiBootServicesData, MemoryMapSize,
                              (VOID **)&MemoryMap);
    if (EFI_ERROR(Status)) return Status;

    /* 获取实际内存映射 */
    Status = BS->GetMemoryMap(&MemoryMapSize, MemoryMap, &MapKey,
                              &DescriptorSize, &DescriptorVersion);
    if (EFI_ERROR(Status)) { BS->FreePool(MemoryMap); return Status; }
    
    desc_count = MemoryMapSize / DescriptorSize;
    print_num(ConOut, L"      - ", desc_count);
    print(ConOut, L" descriptors (");
    print_num(ConOut, L"", DescriptorSize);
    print(ConOut, L" B each)\r\n");

    /* 遍历统计 */
    desc_ptr = (UINT8 *)MemoryMap;
    for (i = 0; i < desc_count; i++) {
        desc_type = *(UINT32 *)desc_ptr;
        desc_phys = *(UINT64 *)(desc_ptr + 8);
        desc_pages = *(UINT64 *)(desc_ptr + 16);
        if (desc_type == EfiConventionalMemory)
            total_conv_memory += desc_pages;
        desc_ptr += DescriptorSize;
    }

    print_num(ConOut, L"      - Conventional: ", total_conv_memory);
    print(ConOut, L" MB\r\n\r\n");

    /* -----------------------------------------------------------
     * 阶段 2: 初始化物理内存管理器 (PMM)
     * ----------------------------------------------------------- */
    print(ConOut, L">>> PHASE 2: Physical Memory <<<\r\n");
    print(ConOut, L"[2/6] Initializing PMM...\r\n");
    
    pmm_init((uint64_t)(uintptr_t)MemoryMap, MemoryMapSize, DescriptorSize);
    
    print_num(ConOut, L"      - Free: ", pmm_get_total_memory() / (1024 * 1024));
    print(ConOut, L" MB / Used: ");
    print_num(ConOut, L"", pmm_get_used_memory() / (1024 * 1024));
    print(ConOut, L" MB\r\n\r\n");

    /* -----------------------------------------------------------
     * 阶段 3: 初始化全局描述符表 (GDT)
     * ----------------------------------------------------------- */
    print(ConOut, L">>> PHASE 3: Global Descriptor Table <<<\r\n");
    print(ConOut, L"[3/6] Initializing GDT...\r\n");
    gdt_init();
    print(ConOut, L"      - OK\r\n\r\n");

    /* -----------------------------------------------------------
     * 阶段 3a: 文件系统枚举 (Stage 3) - 在 IDT 之前执行
     *          使用 UEFI Boot Services (无需中断)
     * ----------------------------------------------------------- */
    fs_handle_t *root_fs = NULL;
    {
        EFI_BLOCK_IO_PROTOCOL *bio = NULL;
        static block_dev_t bdev;

        uart_puts(">>> PHASE 3a: File System <<<\n");
        print(ConOut, L">>> PHASE 3a: File System <<<\r\n");

        /* -------------------------------------------------------
         * 通过 LoadedImageProtocol 获取启动设备
         * -------------------------------------------------------
         * 直接获取 bootx64.efi 所在设备的 Block I/O 句柄，
         * 避免遍历所有设备（实体机上某些设备的 ReadBlocks 会挂死）。
         * ------------------------------------------------------- */
        print(ConOut, L"[4/6] Finding boot device...\r\n");
        uart_puts("[4/6] Finding boot device...\n");
        {
            /* LoadedImage Protocol GUID */
            EFI_GUID LoadedImageGuid =
                {0x5B1B31A1, 0x9562, 0x11D2,
                 {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
            EFI_LOADED_IMAGE_PROTOCOL *loaded_image = NULL;

            uart_puts("  HandleProtocol(LoadedImage)...\n");
            Status = BS->HandleProtocol(ImageHandle, &LoadedImageGuid,
                                        (void **)&loaded_image);
            uart_puts("  HandleProtocol done: ");
            uart_puthex(Status);
            uart_puts("\n");
            if (!EFI_ERROR(Status) && loaded_image != NULL &&
                loaded_image->DeviceHandle != NULL) {
                uart_puts("  HandleProtocol(BlockIO)...\n");
                Status = BS->HandleProtocol(loaded_image->DeviceHandle,
                                            &gBlockIoProtocolGuid,
                                            (void **)&bio);
                uart_puts("  BlockIO HandleProtocol done: ");
                uart_puthex(Status);
                uart_puts("\n");
                if (EFI_ERROR(Status) || bio == NULL || bio->Media == NULL) {
                    print(ConOut, L"      - Boot device has no Block I/O\r\n");
                } else {
                    /* DeviceHandle 直接支持 Block I/O */
                    print(ConOut, L"      - Direct Block I/O from boot device\r\n");
                    if (bio->Media->BlockSize == 512 &&
                        bio->Media->LastBlock > 0) {
                        bdev.total_sectors = bio->Media->LastBlock + 1;
                        bdev.sector_size = bio->Media->BlockSize;
                        bdev.read_sectors = uefi_block_read;
                        bdev.write_sectors = uefi_block_write;
                        bdev.device_context = bio;

                        mbr_t mbr;
                        uart_puts("[FS] Reading MBR...\n");
                        int r = uefi_block_read(bio, 0, 1, &mbr);
                        uart_puts("[FS] Read done, r=");
                        uart_puthex(r);
                        uart_puts(" sig=0x");
                        uart_puthex(mbr.signature);
                        uart_puts(" MBR_SIG=0x");
                        uart_puthex(MBR_SIGNATURE);
                        uart_putc('\n');
                        if (r == 0 && mbr.signature == MBR_SIGNATURE) {
                            uart_puts("[FS] MBR detected, mounting partitions\n");
                            fs_mount_all(&bdev);
                        } else {
                            /* 无 MBR: 整个磁盘作为裸卷（QEMU VVFAT 等） */
                            uart_puts("[FS] No MBR, try whole disk mount\n");
                            fs_mount_all(&bdev);
                        }
                        uart_puts("[FS] Mount done\n");
                        /* 查找第一个可用的盘符 */
                        for (int di = 2; di < MAX_DRIVE_LETTERS;
                                 di++) {
                                if (g_drives[di].type != FS_TYPE_UNKNOWN) {
                                    root_fs = fs_get_handle_by_drive(
                                        'A' + di);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (root_fs != NULL) {
            print(ConOut, L"      - ");
            print_num(ConOut, L"", root_fs->block_dev->sector_size / 1024);
            print(ConOut, L"K ");
            if (root_fs->type == FS_TYPE_FAT32) print(ConOut, L"FAT32");
            else if (root_fs->type == FS_TYPE_NTFS) print(ConOut, L"NTFS");
            else print(ConOut, L"FS");
            print(ConOut, L"\r\n");
        } else {
            print(ConOut, L"      - No bootable volume found\r\n");
        }
        print(ConOut, L"\r\n");

    /*
     * ===========================================================
     * 阶段 3b: 初始化 GOP 图形输出 (Stage 6) - 在 EBS 之前
     *
     * 必须在 ExitBootServices 之前完成，因为 LocateProtocol
     * 和 SetMode 都需要 UEFI Boot Services。
     * ===========================================================
     */
    uart_puts_wide(L">>> PHASE 3b: Graphics Output <<<\r\n");
    {
        EFI_STATUS gfx_st = gfx_init();
        if (!EFI_ERROR(gfx_st)) {
            uart_puts_wide(L"      - GOP mode: ");
            uart_print_num(L"", g_gfx_mode.width);
            uart_puts_wide(L" x ");
            uart_print_num(L"", g_gfx_mode.height);
            uart_puts_wide(L" fb_base=0x");
            uart_print_hex(L"", g_gfx_mode.fb_base);
            uart_puts_wide(L"\r\n");

            /* 分配双缓冲（需在 ExitBootServices 前完成） */
            gfx_st = fb_init();
            if (!EFI_ERROR(gfx_st)) {
                uart_puts_wide(L"      - Framebuffer OK\r\n");
            } else {
                uart_puts_wide(L"      - fb_init failed\r\n");
            }
        } else {
            uart_puts_wide(L"      - No GOP\r\n");
        }
    }
    uart_puts_wide(L"\r\n");

    /*
     * ===========================================================
     * 阶段 3c: 初始化网络层 (Stage 5) - 在 EBS 之前
     *
     * PCI 配置空间访问和 DMA 内存分配都依赖 UEFI Boot Services，
     * 必须在 ExitBootServices 之前完成。
     * ===========================================================
     */
    uart_puts_wide(L">>> PHASE 3c: Network Layer <<<\r\n");
    {
        static e1000_dev_t nic_dev;
        void *rx_ring = NULL, *tx_ring = NULL;
        void *rx_buffers = NULL, *tx_buffers = NULL;
        uint64_t rx_ring_phys = 0, tx_ring_phys = 0;
        uint64_t rx_buffers_phys = 0, tx_buffers_phys = 0;

        uint64_t found_mmio = 0;
        EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL *pci_root_io = NULL;
        EFI_GUID pci_guid = EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_GUID;
        EFI_HANDLE *pci_handles = NULL;
        UINTN pci_handle_count = 0;
        EFI_STATUS pci_st;

        uart_puts_wide(L"      - Scanning PCI bus...\r\n");
        pci_st = BS->LocateHandleBuffer(ByProtocol, &pci_guid, NULL, &pci_handle_count, &pci_handles);
        if (!EFI_ERROR(pci_st) && pci_handle_count > 0) {
            pci_st = BS->HandleProtocol(pci_handles[0], &pci_guid, (VOID **)&pci_root_io);
            if (!EFI_ERROR(pci_st)) {
                for (int bus = 0; bus < 256 && !found_mmio; bus++)
                    for (int dev = 0; dev < 32 && !found_mmio; dev++)
                        for (int func = 0; func < 8 && !found_mmio; func++) {
                            UINT64 pci_addr = (UINT64)bus << 20 | (UINT64)dev << 15 | (UINT64)func << 12;
                            UINT32 vendor_dev_id = 0;
                            pci_st = pci_root_io->Pci.Read(pci_root_io, EfiPciIoWidthUint32, pci_addr, 1, &vendor_dev_id);
                            if (!EFI_ERROR(pci_st) && vendor_dev_id == 0x100E8086) {
                                UINT32 bar0 = 0;
                                pci_root_io->Pci.Read(pci_root_io, EfiPciIoWidthUint32, pci_addr + 0x10, 1, &bar0);
                                found_mmio = bar0 & 0xFFFFFFF0;
                            }
                        }
            }
            BS->FreePool(pci_handles);
        }

        if (found_mmio) {
            /* 检查 MMIO 地址是否在 4GB 内（页表映射范围） */
            if (found_mmio < 0x100000000ULL) {
                uart_puts_wide(L"      - Allocating DMA buffers...\r\n");
                BS->AllocatePool(EfiBootServicesData, 256 * sizeof(e1000_rx_desc_t), &rx_ring);
                BS->AllocatePool(EfiBootServicesData, 256 * sizeof(e1000_tx_desc_t), &tx_ring);
                BS->AllocatePool(EfiBootServicesData, 256 * 2048, &rx_buffers);
                BS->AllocatePool(EfiBootServicesData, 256 * 2048, &tx_buffers);
                rx_ring_phys = (uint64_t)(uintptr_t)rx_ring;
                tx_ring_phys = (uint64_t)(uintptr_t)tx_ring;
                rx_buffers_phys = (uint64_t)(uintptr_t)rx_buffers;
                tx_buffers_phys = (uint64_t)(uintptr_t)tx_buffers;

                if (e1000_init(&nic_dev, found_mmio, rx_ring, rx_ring_phys,
                               tx_ring, tx_ring_phys, rx_buffers, rx_buffers_phys,
                               tx_buffers, tx_buffers_phys) == 0) {
                    uart_puts_wide(L"      - e1000 @ 0x");
                    uart_print_hex(L"", found_mmio);
                    uart_puts_wide(L"\r\n");
                    uart_puts_wide(L"      - NIC ready\r\n");
                } else {
                    uart_puts_wide(L"      - e1000 init failed\r\n");
                }
            } else {
                uart_puts_wide(L"      - e1000 MMIO > 4GB, skipped\r\n");
            }
        } else {
            uart_puts_wide(L"      - No e1000 found\r\n");
        }
        uart_puts_wide(L"\r\n");
    }

    /*
     * ===========================================================
     * ExitBootServices - 终止 UEFI Boot Services
     *
     * 在此之后，所有 Boot Services 函数（AllocatePool,
     * HandleProtocol, LocateHandleBuffer, GetMemoryMap 等）
     * 将不再可用。必须在此前完成所有依赖 Boot Services 的初始化。
     *
     * 在实体机上，必须在接管中断和定时器硬件前调用，
     * 否则固件的中断处理程序会与内核冲突。
     *
     * 使用重试循环：如果 ExitBootServices 因 MapKey 过期而失败，
     * 重新获取内存映射并重试。
     *
     * 注意：此阶段使用 UART 输出（非 ConOut），因为 GOP 模式切换后
     * ConOut 在部分固件上可能不可靠。
     * ===========================================================
     */
    uart_puts_wide(L">>> ExitBootServices <<<\r\n");
    {
        EFI_MEMORY_DESCRIPTOR *EbsMemMap = NULL;
        UINTN                  EbsMemMapSize = 0;
        UINTN                  EbsMapKey;
        UINTN                  EbsDescSize;
        UINT32                 EbsDescVer;
        int                    ebs_retries = 3;

        uart_puts_wide(L"      - Getting memory map size...\r\n");
        do {
            /* 获取当前 MapKey 所需的缓冲区大小 */
            EbsMemMapSize = 0;
            Status = BS->GetMemoryMap(&EbsMemMapSize, NULL, &EbsMapKey,
                                      &EbsDescSize, &EbsDescVer);
            if (Status != EFI_BUFFER_TOO_SMALL) {
                uart_puts_wide(L"      - GetMemoryMap size failed\r\n");
                break;
            }

            /* 分配缓冲区 */
            EbsMemMapSize += EbsDescSize * 8;
            if (EbsMemMap != NULL) {
                BS->FreePool(EbsMemMap);
                EbsMemMap = NULL;
            }
            uart_puts_wide(L"      - Allocating memory map buffer...\r\n");
            Status = BS->AllocatePool(EfiBootServicesData, EbsMemMapSize,
                                      (VOID **)&EbsMemMap);
            if (EFI_ERROR(Status)) {
                uart_puts_wide(L"      - AllocatePool failed\r\n");
                break;
            }

            /* 获取最新内存映射和 MapKey */
            uart_puts_wide(L"      - Fetching memory map...\r\n");
            Status = BS->GetMemoryMap(&EbsMemMapSize, EbsMemMap, &EbsMapKey,
                                      &EbsDescSize, &EbsDescVer);
            if (EFI_ERROR(Status)) {
                uart_puts_wide(L"      - GetMemoryMap failed, retrying...\r\n");
                BS->FreePool(EbsMemMap);
                EbsMemMap = NULL;
                continue;
            }

            /* 实际调用 ExitBootServices */
            uart_puts_wide(L"      - Calling ExitBootServices...\r\n");
            Status = BS->ExitBootServices(ImageHandle, EbsMapKey);
            if (EFI_ERROR(Status)) {
                uart_puts_wide(L"      - ExitBootServices failed (MapKey), retrying...\r\n");
                BS->FreePool(EbsMemMap);
                EbsMemMap = NULL;
                ebs_retries--;
                continue;
            }

            /* 成功 */
            uart_puts_wide(L"      - OK (Boot Services terminated)\r\n");
            EbsMemMap = NULL;
            break;
        } while (ebs_retries > 0);

        if (EFI_ERROR(Status)) {
            uart_puts_wide(L"      - ExitBootServices FAILED after retries\r\n");
        }
    }
    uart_puts_wide(L"\r\n");

    /* ===========================================================
     * 以下阶段在 ExitBootServices 之后运行，不再使用 Boot Services
     * =========================================================== */

    /* -----------------------------------------------------------
     * 阶段 4: 初始化 IDT (在 ExitBootServices 之后)
     * ----------------------------------------------------------- */
    uart_print(L">>> PHASE 4: Interrupt Descriptor Table <<<\r\n");
    idt_init();
    uart_print(L"      - OK\r\n\r\n");

    /* -----------------------------------------------------------
     * 阶段 5: 初始化分页系统 (切换CR3到内核页表)
     * ----------------------------------------------------------- */
    uart_print(L">>> PHASE 5: Paging System <<<\r\n");
    paging_init();

    /* 检查帧缓冲地址是否在页表映射范围内 */
    if (g_gfx_mode.fb_base != 0) {
        if (g_gfx_mode.fb_base >= IDENTITY_MAP_LIMIT) {
            uart_print(L"      - WARNING: framebuffer > 4GB\r\n");
        } else {
            uart_print(L"      - Framebuffer in mapped range\r\n");
        }
    }
    uart_print(L"      - OK\r\n\r\n");

    /* -----------------------------------------------------------
     * 阶段 6: 初始化调度器和 Syscall
     * ----------------------------------------------------------- */
    uart_print(L">>> PHASE 6: Task Scheduler <<<\r\n");
    uart_print(L"      - ");
    uart_print_num(L"", TIMER_DEFAULT_HZ);
    uart_print(L" Hz, 3 syscalls\r\n");
    sched_init();
    sched_register_syscall();

    /* 保存 SystemTable 到全局变量供 syscall 输出使用 */
    g_ST = SystemTable;
    g_sched_puts = uart_puts_wide;

    /* -----------------------------------------------------------
     * 阶段 7: 初始化 Shell (Stage 4)
     * ----------------------------------------------------------- */
    uart_print(L">>> PHASE 7: Command Line Shell <<<\r\n");
    if (root_fs != NULL) {
        shell_init(root_fs);
        uart_print(L"      - Ready\r\n");
    } else {
        shell_init(NULL);
        uart_print(L"      - Ready (no fs)\r\n");
    }

    /* 创建 Shell 任务 */
    sched_create_task("Shell", shell_main);

    /*
     * 启动调度器 (永不返回)
     */
    uart_print(L">>> Finalizing boot process...\r\n");
    timer_init(TIMER_DEFAULT_HZ);

    sched_run();

    /* 永远不会到达这里 */
    return EFI_SUCCESS;
}
