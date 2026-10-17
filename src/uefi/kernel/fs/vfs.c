/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 3: 文件系统抽象层
 *
 * 文件: vfs.c
 * 描述: 虚拟文件系统层 — 实现 Windows 风格盘符系统 (C:\, D:\, E:\)
 *       支持 MBR 分区表解析、FAT32/NTFS 驱动调度。
 */

#include "fs.h"
#include "fat32.h"
#include "ntfs.h"
#include "../lib/io.h"

/* memset/memcpy/strcpy 声明 (UEFI 环境无标准库) */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
char *strcpy(char *dest, const char *src);

/* ---------------------------------------------------------------
 * MBR 分区类型 (mbr_t 定义在 fat32.h 中)
 * --------------------------------------------------------------- */
#define PART_TYPE_FAT12      0x01  /* FAT12 */
#define PART_TYPE_FAT16      0x06  /* FAT16 */
#define PART_TYPE_FAT32      0x0B  /* FAT32 CHS */
#define PART_TYPE_FAT32_LBA  0x0C  /* FAT32 LBA */
#define PART_TYPE_NTFS       0x07  /* NTFS/HPFS */
#define PART_TYPE_EXTENDED   0x05  /* 扩展分区 */
#define PART_TYPE_EXTENDED_LBA 0x0F  /* 扩展分区 LBA */

/* ---------------------------------------------------------------
 * 对象池配置
 * --------------------------------------------------------------- */
#define MAX_MOUNTED_FS   4    /* 最大挂载文件系统数 */
#define MAX_OPEN_FILES   16   /* 最大同时打开文件数 */

/* 文件系统对象池 */
static fs_handle_t fs_pool[MAX_MOUNTED_FS];
static uint8_t fs_data_pool[MAX_MOUNTED_FS][4096];  /* 每个 FS 4KB 私有数据 */

/* 文件句柄对象池 */
static file_handle_t file_pool[MAX_OPEN_FILES];
static uint8_t file_priv_pool[MAX_OPEN_FILES][512];  /* 每个文件 512 字节私有数据 */

/* 盘符表 */
drive_entry_t g_drives[MAX_DRIVE_LETTERS];

/* ---------------------------------------------------------------
 * 初始化对象池
 * --------------------------------------------------------------- */
static void vfs_init_pools(void)
{
    static int initialized = 0;
    if (initialized) return;

    /* 清零所有文件系统句柄 */
    for (int i = 0; i < MAX_MOUNTED_FS; i++) {
        memset(&fs_pool[i], 0, sizeof(fs_handle_t));
        fs_pool[i].type = FS_TYPE_UNKNOWN;
    }

    /* 清零所有文件句柄 */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        memset(&file_pool[i], 0, sizeof(file_handle_t));
        memset(file_priv_pool[i], 0, sizeof(file_priv_pool[i]));
        file_pool[i].type = FS_TYPE_UNKNOWN;
    }

    /* 清零盘符表 */
    for (int i = 0; i < MAX_DRIVE_LETTERS; i++) {
        memset(&g_drives[i], 0, sizeof(drive_entry_t));
        g_drives[i].type = FS_TYPE_UNKNOWN;
        g_drives[i].letter = 'A' + i;
    }

    initialized = 1;
}

/* ---------------------------------------------------------------
 * 解析 MBR 分区表 (使用 fat32.h 中的 mbr_t 定义)
 * --------------------------------------------------------------- */
static int parse_mbr(block_dev_t *dev, mbr_part_entry_t parts[], int max_parts)
{
    mbr_t mbr;
    int count = 0;

    /* 读取 MBR (LBA 0) */
    if (dev->read_sectors(dev->device_context, 0, 1, &mbr) != 0)
        return -1;

    /* 验证 MBR 签名 */
    if (mbr.signature != MBR_SIGNATURE)
        return -1;

    /* 提取分区 */
    for (int i = 0; i < MBR_PARTITION_COUNT && count < max_parts; i++) {
        mbr_part_entry_t *part = &mbr.partitions[i];
        if (part->type != 0) {
            memcpy(&parts[count], part, sizeof(mbr_part_entry_t));
            count++;
        }
    }

    return count;
}

/* ---------------------------------------------------------------
 * fs_mount_all - 挂载所有分区, 分配盘符
 * --------------------------------------------------------------- */
int fs_mount_all(block_dev_t *dev)
{
    mbr_part_entry_t parts[4];
    int num_parts;
    int drive_idx = 2;  /* 从 C: 开始 (A:, B: 保留给软盘) */

    vfs_init_pools();

    /* 尝试解析 MBR */
    num_parts = parse_mbr(dev, parts, 4);

    if (num_parts > 0) {
        /* 有分区表: 按分区挂载 */
        uart_puts("[VFS] Found MBR with ");
        uart_puthex(num_parts);
        uart_puts(" partitions\n");

        for (int i = 0; i < num_parts && drive_idx < MAX_DRIVE_LETTERS; i++) {
            mbr_part_entry_t *part = &parts[i];

            /* 跳过扩展分区 */
            if (part->type == PART_TYPE_EXTENDED ||
                part->type == PART_TYPE_EXTENDED_LBA) {
                continue;
            }

            char letter = 'A' + drive_idx;
            uint64_t lba_start = part->lba_first;

            uart_puts("[VFS] Partition ");
            uart_putc(letter);
            uart_puts(": LBA=");
            uart_puthex((uint32_t)lba_start);
            uart_puts(" type=0x");
            uart_puthex(part->type);
            uart_putc('\n');

            /* 尝试 NTFS */
            if (part->type == PART_TYPE_NTFS) {
                ntfs_fs_t *ntfs_fs = (ntfs_fs_t *)fs_data_pool[i];
                if (ntfs_mount(dev, lba_start, ntfs_fs) == 0) {
                    g_drives[drive_idx].type = FS_TYPE_NTFS;
                    g_drives[drive_idx].block_dev = dev;
                    g_drives[drive_idx].fs_data = ntfs_fs;
                    g_drives[drive_idx].letter = letter;
                    uart_puts("[VFS] NTFS mounted as ");
                    uart_putc(letter);
                    uart_puts(":\\\n");
                    drive_idx++;
                    continue;
                }
            }

            /* 尝试 FAT12/FAT16/FAT32 */
            if (part->type == PART_TYPE_FAT12 ||
                part->type == PART_TYPE_FAT16 ||
                part->type == PART_TYPE_FAT32 ||
                part->type == PART_TYPE_FAT32_LBA) {
                fat32_fs_t *fat32_fs = (fat32_fs_t *)fs_data_pool[i];
                if (fat32_mount(dev, lba_start, fat32_fs) == 0) {
                    g_drives[drive_idx].type = FS_TYPE_FAT32;
                    g_drives[drive_idx].block_dev = dev;
                    g_drives[drive_idx].fs_data = fat32_fs;
                    g_drives[drive_idx].letter = letter;
                    uart_puts("[VFS] FAT mounted as ");
                    uart_putc(letter);
                    uart_puts(":\\\n");
                    drive_idx++;
                    continue;
                }
            }

            /* 未知类型, 尝试自动检测 */
            {
                ntfs_fs_t *ntfs_fs = (ntfs_fs_t *)fs_data_pool[i];
                if (ntfs_mount(dev, lba_start, ntfs_fs) == 0) {
                    g_drives[drive_idx].type = FS_TYPE_NTFS;
                    g_drives[drive_idx].block_dev = dev;
                    g_drives[drive_idx].fs_data = ntfs_fs;
                    g_drives[drive_idx].letter = letter;
                    uart_puts("[VFS] NTFS (auto) mounted as ");
                    uart_putc(letter);
                    uart_puts(":\\\n");
                    drive_idx++;
                    continue;
                }

                fat32_fs_t *fat32_fs = (fat32_fs_t *)fs_data_pool[i];
                if (fat32_mount(dev, lba_start, fat32_fs) == 0) {
                    g_drives[drive_idx].type = FS_TYPE_FAT32;
                    g_drives[drive_idx].block_dev = dev;
                    g_drives[drive_idx].fs_data = fat32_fs;
                    g_drives[drive_idx].letter = letter;
                    uart_puts("[VFS] FAT32 (auto) mounted as ");
                    uart_putc(letter);
                    uart_puts(":\\\n");
                    drive_idx++;
                    continue;
                }
            }
        }
    } else {
        /* 无分区表: 整个磁盘作为一个卷 (VVFAT 等) */
        uart_puts("[VFS] No MBR found, trying whole disk\n");

        /* 尝试 NTFS */
        ntfs_fs_t *ntfs_fs = (ntfs_fs_t *)fs_data_pool[0];
        if (ntfs_mount(dev, 0, ntfs_fs) == 0) {
            g_drives[drive_idx].type = FS_TYPE_NTFS;
            g_drives[drive_idx].block_dev = dev;
            g_drives[drive_idx].fs_data = ntfs_fs;
            g_drives[drive_idx].letter = 'C';
            uart_puts("[VFS] NTFS mounted as C:\\\n");
            return 0;
        }

        /* 尝试 FAT32 */
        fat32_fs_t *fat32_fs = (fat32_fs_t *)fs_data_pool[0];
        if (fat32_mount(dev, 0, fat32_fs) == 0) {
            g_drives[drive_idx].type = FS_TYPE_FAT32;
            g_drives[drive_idx].block_dev = dev;
            g_drives[drive_idx].fs_data = fat32_fs;
            g_drives[drive_idx].letter = 'C';
            uart_puts("[VFS] FAT32 mounted as C:\\\n");
            return 0;
        }

        uart_puts("[VFS] No file system found\n");
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------
 * fs_unmount_all - 卸载所有盘符
 * --------------------------------------------------------------- */
void fs_unmount_all(void)
{
    for (int i = 0; i < MAX_DRIVE_LETTERS; i++) {
        g_drives[i].type = FS_TYPE_UNKNOWN;
        g_drives[i].fs_data = NULL;
    }
}

/* ---------------------------------------------------------------
 * fs_parse_drive_path - 解析 Windows 风格路径
 * --------------------------------------------------------------- */
int fs_parse_drive_path(const char *path, char *path_out)
{
    if (!path || !path_out)
        return -1;

    /* 检查是否是 "C:\" 或 "C:/" 格式 */
    if (path[0] >= 'A' && path[0] <= 'Z' && path[1] == ':') {
        int idx = path[0] - 'A';
        int i = 2;
        /* 跳过斜杠 */
        if (path[i] == '\\' || path[i] == '/')
            i++;
        /* 复制剩余路径 */
        while (path[i] && i < FS_MAX_PATH) {
            path_out[i - 2 - ((path[2] == '\\' || path[2] == '/') ? 1 : 0)] = path[i];
            i++;
        }
        path_out[i - 2 - ((path[2] == '\\' || path[2] == '/') ? 1 : 0)] = '\0';
        return idx;
    }

    /* 不含盘符 */
    strcpy(path_out, path);
    return -1;
}

/* ---------------------------------------------------------------
 * fs_get_handle_by_drive - 根据盘符获取文件系统句柄
 * --------------------------------------------------------------- */
fs_handle_t *fs_get_handle_by_drive(char letter)
{
    int idx = letter - 'A';
    if (idx < 0 || idx >= MAX_DRIVE_LETTERS)
        return NULL;

    if (g_drives[idx].type == FS_TYPE_UNKNOWN)
        return NULL;

    /* 查找对应的 fs_pool 项 */
    for (int i = 0; i < MAX_MOUNTED_FS; i++) {
        if (fs_pool[i].fs_data == g_drives[idx].fs_data)
            return &fs_pool[i];
    }

    /* 创建新的 fs_handle */
    for (int i = 0; i < MAX_MOUNTED_FS; i++) {
        if (fs_pool[i].type == FS_TYPE_UNKNOWN) {
            fs_pool[i].type = g_drives[idx].type;
            fs_pool[i].block_dev = g_drives[idx].block_dev;
            fs_pool[i].fs_data = g_drives[idx].fs_data;
            return &fs_pool[i];
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------
 * fs_mount - 兼容旧接口 (保留)
 * --------------------------------------------------------------- */
fs_handle_t *fs_mount(block_dev_t *dev)
{
    vfs_init_pools();

    /* 尝试 C: 盘 */
    if (g_drives[2].type != FS_TYPE_UNKNOWN) {
        return fs_get_handle_by_drive('C');
    }

    /* 挂载所有分区 */
    fs_mount_all(dev);

    /* 返回第一个可用的文件系统 */
    for (int i = 2; i < MAX_DRIVE_LETTERS; i++) {
        if (g_drives[i].type != FS_TYPE_UNKNOWN) {
            return fs_get_handle_by_drive('A' + i);
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------
 * fs_unmount - 卸载
 * --------------------------------------------------------------- */
void fs_unmount(fs_handle_t *fs)
{
    if (fs) {
        fs->type = FS_TYPE_UNKNOWN;
        fs->fs_data = NULL;
    }
}

/* ---------------------------------------------------------------
 * fs_open - 打开文件 (支持 Windows 路径)
 * --------------------------------------------------------------- */
file_handle_t *fs_open(fs_handle_t *fs, const char *path)
{
    vfs_init_pools();

    /* 如果路径包含盘符, 获取对应的文件系统 */
    char internal_path[FS_MAX_PATH];
    int drive_idx = fs_parse_drive_path(path, internal_path);

    if (drive_idx >= 0) {
        char letter = 'A' + drive_idx;
        fs = fs_get_handle_by_drive(letter);
        if (!fs)
            return NULL;
        path = internal_path;
    }

    /* 查找空闲文件句柄槽 */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (file_pool[i].type == FS_TYPE_UNKNOWN) {
            memset(&file_pool[i], 0, sizeof(file_handle_t));
            memset(file_priv_pool[i], 0, sizeof(file_priv_pool[i]));
            file_pool[i].type = fs->type;
            file_pool[i].fs_priv = fs->fs_data;
            file_pool[i].file_priv = file_priv_pool[i];

            /* 将反斜杠转换为正斜杠 */
            char normalized_path[FS_MAX_PATH];
            int pi = 0;
            while (path[pi] && pi < FS_MAX_PATH - 1) {
                normalized_path[pi] = (path[pi] == '\\') ? '/' : path[pi];
                pi++;
            }
            normalized_path[pi] = '\0';

            if (fs->type == FS_TYPE_FAT32) {
                fat32_file_t *ff = (fat32_file_t *)file_priv_pool[i];
                int ret = fat32_open_file((fat32_fs_t *)fs->fs_data, normalized_path, ff);
                if (ret != FS_OK) {
                    file_pool[i].type = FS_TYPE_UNKNOWN;
                    return NULL;
                }
                file_pool[i].size = ff->size;
            } else if (fs->type == FS_TYPE_NTFS) {
                ntfs_file_t *nf = (ntfs_file_t *)file_priv_pool[i];
                int ret = ntfs_open_file((ntfs_fs_t *)fs->fs_data, normalized_path, nf);
                if (ret != FS_OK) {
                    file_pool[i].type = FS_TYPE_UNKNOWN;
                    return NULL;
                }
                file_pool[i].size = nf->size;
            } else {
                file_pool[i].type = FS_TYPE_UNKNOWN;
                return NULL;
            }

            file_pool[i].offset = 0;
            return &file_pool[i];
        }
    }

    return NULL;
}

/* ---------------------------------------------------------------
 * fs_read - 读取文件
 * --------------------------------------------------------------- */
int fs_read(file_handle_t *f, void *buffer, uint64_t size,
            uint64_t *bytes_read)
{
    if (f->type == FS_TYPE_FAT32) {
        uint32_t br = 0;
        int ret = fat32_read_file((fat32_fs_t *)f->fs_priv,
                                   (fat32_file_t *)f->file_priv,
                                   buffer, (uint32_t)size, &br);
        if (bytes_read) *bytes_read = br;
        return ret;
    } else if (f->type == FS_TYPE_NTFS) {
        uint64_t br = 0;
        int ret = ntfs_read_file((ntfs_fs_t *)f->fs_priv,
                                  (ntfs_file_t *)f->file_priv,
                                  buffer, size, &br);
        if (bytes_read) *bytes_read = br;
        return ret;
    }
    return FS_ERR_INVALID;
}

/* ---------------------------------------------------------------
 * fs_close - 关闭文件
 * --------------------------------------------------------------- */
int fs_close(file_handle_t *f)
{
    if (f->type == FS_TYPE_FAT32) {
        fat32_close_file((fat32_file_t *)f->file_priv);
    } else if (f->type == FS_TYPE_NTFS) {
        ntfs_close_file((ntfs_file_t *)f->file_priv);
    }
    f->type = FS_TYPE_UNKNOWN;
    memset(f, 0, sizeof(*f));
    return FS_OK;
}

/* ---------------------------------------------------------------
 * fs_get_size - 获取文件大小
 * --------------------------------------------------------------- */
uint64_t fs_get_size(file_handle_t *f)
{
    return f->size;
}

/* ---------------------------------------------------------------
 * fs_seek - 设置文件偏移
 * --------------------------------------------------------------- */
int fs_seek(file_handle_t *f, uint64_t offset)
{
    if (offset > f->size)
        offset = f->size;
    f->offset = offset;

    if (f->type == FS_TYPE_FAT32) {
        return fat32_seek_file((fat32_file_t *)f->file_priv, offset);
    } else if (f->type == FS_TYPE_NTFS) {
        return ntfs_seek_file((ntfs_file_t *)f->file_priv, offset);
    }
    return FS_ERR_INVALID;
}

/* ---------------------------------------------------------------
 * fs_readdir - 读取目录 (支持 Windows 路径)
 * --------------------------------------------------------------- */
int fs_readdir(fs_handle_t *fs, const char *path,
               fs_dirent_t *entries, int max)
{
    /* 如果路径包含盘符, 获取对应的文件系统 */
    char internal_path[FS_MAX_PATH];
    int drive_idx = fs_parse_drive_path(path, internal_path);

    if (drive_idx >= 0) {
        char letter = 'A' + drive_idx;
        fs = fs_get_handle_by_drive(letter);
        if (!fs)
            return FS_ERR_NOT_FOUND;
        path = internal_path;
    }

    /* 将反斜杠转换为正斜杠 */
    char normalized_path[FS_MAX_PATH];
    int pi = 0;
    while (path[pi] && pi < FS_MAX_PATH - 1) {
        normalized_path[pi] = (path[pi] == '\\') ? '/' : path[pi];
        pi++;
    }
    normalized_path[pi] = '\0';

    if (fs->type == FS_TYPE_FAT32) {
        return fat32_read_dir((fat32_fs_t *)fs->fs_data, normalized_path, entries, max);
    } else if (fs->type == FS_TYPE_NTFS) {
        return ntfs_read_dir((ntfs_fs_t *)fs->fs_data, normalized_path, entries, max);
    }
    return FS_ERR_INVALID;
}

/* strcpy 实现 (UEFI 环境无标准库) */
char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while (*src) {
        *d++ = *src++;
    }
    *d = '\0';
    return dest;
}
