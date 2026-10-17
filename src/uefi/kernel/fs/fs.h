/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 3: 文件系统抽象层
 *
 * 文件: fs.h
 * 描述: 统一的文件系统抽象接口，支持 FAT32 和 NTFS。
 *       提供挂载、文件打开/读/写/关闭、目录遍历等操作。
 *
 * EXE模式: 直接走 Win32 API (支持 FAT32/NTFS/ReFS 等)
 * UEFI模式: 自研 FAT32 驱动 + 基础 NTFS 驱动 (只读)
 */

#ifndef DIMENSOS_FS_H
#define DIMENSOS_FS_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------
 * 文件系统类型
 * --------------------------------------------------------------- */
#define FS_TYPE_UNKNOWN  0
#define FS_TYPE_FAT32    1
#define FS_TYPE_NTFS     2

/* ---------------------------------------------------------------
 * 操作返回值
 * --------------------------------------------------------------- */
#define FS_OK            0
#define FS_ERR_NOT_FOUND   (-1)
#define FS_ERR_IO          (-2)
#define FS_ERR_NOT_FS      (-3)
#define FS_ERR_NO_MEM      (-4)
#define FS_ERR_INVALID     (-5)
#define FS_ERR_NOT_DIR     (-6)
#define FS_ERR_NOT_FILE    (-7)

/* ---------------------------------------------------------------
 * 文件名最大长度
 * --------------------------------------------------------------- */
#define FS_MAX_PATH      256
#define FS_MAX_NAME      256

/* ---------------------------------------------------------------
 * 目录项结构
 * --------------------------------------------------------------- */
typedef struct {
    char     name[FS_MAX_NAME];   /* 文件/目录名 */
    uint32_t attrs;               /* 属性 (FAT32: ATTR_DIRECTORY 等) */
    uint64_t size;                /* 文件大小 (字节) */
    uint8_t  is_directory;        /* 1=目录, 0=文件 */
} fs_dirent_t;

/* ---------------------------------------------------------------
 * 块设备操作接口
 *
 * 由底层 (UEFI Block I/O 或模拟层) 提供。
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t total_sectors;
    uint32_t sector_size;

    /* 读取扇区
     *   start_lba - 起始逻辑块地址
     *   count     - 扇区数
     *   buffer    - 输出缓冲区 (至少 sector_size * count 字节)
     * 返回: 0=成功, 非0=失败 */
    int (*read_sectors)(void *device, uint64_t start_lba,
                        uint32_t count, void *buffer);

    /* 写入扇区
     *   start_lba - 起始逻辑块地址
     *   count     - 扇区数
     *   buffer    - 输入缓冲区
     * 返回: 0=成功, 非0=失败 */
    int (*write_sectors)(void *device, uint64_t start_lba,
                         uint32_t count, const void *buffer);

    /* 设备上下文 (UEFI BlockIO 协议指针等) */
    void *device_context;
} block_dev_t;

/* ---------------------------------------------------------------
 * 抽象文件系统句柄
 * --------------------------------------------------------------- */
typedef struct {
    int       type;            /* FS_TYPE_FAT32 / FS_TYPE_NTFS */
    block_dev_t *block_dev;    /* 底层块设备 */
    void      *fs_data;        /* 驱动私有数据 */
} fs_handle_t;

/* ---------------------------------------------------------------
 * 抽象文件句柄
 * --------------------------------------------------------------- */
typedef struct {
    int       type;            /* FS_TYPE_FAT32 / FS_TYPE_NTFS */
    void      *fs_priv;        /* 文件系统私有指针 (用于回调) */
    void      *file_priv;      /* 文件私有数据 (各驱动自定义) */
    uint64_t  size;            /* 文件总大小 (字节) */
    uint64_t  offset;          /* 当前读写偏移 */
} file_handle_t;

/* ---------------------------------------------------------------
 * Windows 风格盘符系统
 * --------------------------------------------------------------- */
#define MAX_DRIVE_LETTERS 26  /* A-Z */

/* 盘符映射 */
typedef struct {
    char      letter;          /* 'C', 'D', 'E' 等 */
    int       type;            /* FS_TYPE_FAT32 / FS_TYPE_NTFS / FS_TYPE_UNKNOWN */
    block_dev_t *block_dev;    /* 底层块设备 */
    void      *fs_data;        /* 文件系统私有数据 */
    char      label[12];       /* 卷标 */
} drive_entry_t;

extern drive_entry_t g_drives[MAX_DRIVE_LETTERS];

/* 解析 Windows 风格路径 (如 "C:\Windows\file.txt")
 * 返回盘符索引 (0=A, 1=B, ...), path_out 返回盘符后的路径 (不含冒号反斜杠)
 * 如果路径不含盘符, 返回 -1, path_out 不变 */
int fs_parse_drive_path(const char *path, char *path_out);

/* 根据盘符获取文件系统句柄 */
fs_handle_t *fs_get_handle_by_drive(char letter);

/* 挂载所有分区, 分配盘符 */
int fs_mount_all(block_dev_t *dev);

/* 卸载所有盘符 */
void fs_unmount_all(void);

/*
 * fs_open - 打开文件
 *
 *   path - 绝对路径 (如 "/dir1/file.txt")
 *   返回 - file_handle_t 指针, 失败返回 NULL
 */
file_handle_t *fs_open(fs_handle_t *fs, const char *path);

/*
 * fs_read - 从文件读取数据
 *
 *   f           - 文件句柄
 *   buffer      - 输出缓冲区
 *   size        - 请求读取字节数
 *   bytes_read  - 输出: 实际读取字节数
 *   返回 - FS_OK 或错误码
 */
int fs_read(file_handle_t *f, void *buffer, uint64_t size,
            uint64_t *bytes_read);

/*
 * fs_close - 关闭文件
 *
 *   返回 - FS_OK 或错误码
 */
int fs_close(file_handle_t *f);

/*
 * fs_get_size - 获取文件大小
 */
uint64_t fs_get_size(file_handle_t *f);

/*
 * fs_seek - 设置文件读写偏移
 *
 *   offset - 相对于文件开头的偏移
 *   返回 - FS_OK 或错误码
 */
int fs_seek(file_handle_t *f, uint64_t offset);

/*
 * fs_readdir - 读取目录内容
 *
 *   path    - 目录路径
 *   entries - 输出缓冲区 (fs_dirent_t 数组)
 *   max     - entries 数组最大项数
 *   返回 - 实际目录项数 (>=0), 或错误码 (<0)
 */
int fs_readdir(fs_handle_t *fs, const char *path,
               fs_dirent_t *entries, int max);

#endif /* DIMENSOS_FS_H */
