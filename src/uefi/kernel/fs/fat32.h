/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 3: 文件系统抽象层
 *
 * 文件: fat32.h
 * 描述: FAT32 文件系统驱动 — 只读
 *       实现 MBR 分区识别、BPB 解析、簇链遍历、目录项读取。
 */

#ifndef DIMENSOS_FAT32_H
#define DIMENSOS_FAT32_H

#include <stdint.h>
#include "fs.h"

/* ---------------------------------------------------------------
 * FAT32 目录项属性标志
 * --------------------------------------------------------------- */
#define FAT32_ATTR_READ_ONLY     0x01
#define FAT32_ATTR_HIDDEN        0x02
#define FAT32_ATTR_SYSTEM        0x04
#define FAT32_ATTR_VOLUME_ID     0x08
#define FAT32_ATTR_DIRECTORY     0x10
#define FAT32_ATTR_ARCHIVE       0x20
#define FAT32_ATTR_LFN           0x0F   /* 长文件名项的属性值 */

/* ---------------------------------------------------------------
 * FAT32 特殊簇值
 * --------------------------------------------------------------- */
#define FAT32_CLUSTER_FREE       0x00000000
#define FAT32_CLUSTER_RESERVED   0x00000001
#define FAT32_CLUSTER_MIN        0x00000002
#define FAT32_CLUSTER_MAX        0x0FFFFFF6
#define FAT32_CLUSTER_BAD        0x0FFFFFF7
#define FAT32_CLUSTER_LAST       0x0FFFFFF8  /* >= 此值为链尾 */

/* ---------------------------------------------------------------
 * MBR 分区表
 * --------------------------------------------------------------- */
#define MBR_PARTITION_COUNT  4
#define MBR_SIGNATURE        0xAA55

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_first;
    uint32_t sector_count;
} __attribute__((packed)) mbr_part_entry_t;

typedef struct {
    uint8_t             boot_code[440];
    uint32_t            disk_signature;
    uint16_t            reserved;
    mbr_part_entry_t    partitions[4];
    uint16_t            signature;   /* 0xAA55 */
} __attribute__((packed)) mbr_t;

/* ---------------------------------------------------------------
 * FAT32 BPB (BIOS Parameter Block)
 * --------------------------------------------------------------- */
typedef struct {
    uint8_t   jmp_boot[3];
    char      oem_name[8];
    uint16_t  bytes_per_sector;
    uint8_t   sectors_per_cluster;
    uint16_t  reserved_sector_count;
    uint8_t   num_fats;
    uint16_t  root_entry_count;
    uint16_t  total_sectors_16;
    uint8_t   media_descriptor;
    uint16_t  fat_size_16;
    uint16_t  sectors_per_track;
    uint16_t  num_heads;
    uint32_t  hidden_sectors;
    uint32_t  total_sectors_32;

    /* FAT32 扩展字段 */
    uint32_t  fat_size_32;
    uint16_t  ext_flags;
    uint16_t  fs_version;
    uint32_t  root_cluster;
    uint16_t  fs_info_sector;
    uint16_t  backup_boot_sector;
    uint8_t   reserved[12];
    uint8_t   drive_number;
    uint8_t   reserved1;
    uint8_t   boot_signature;
    uint32_t  volume_id;
    char      volume_label[11];
    char      fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

/* ---------------------------------------------------------------
 * FAT32 短文件名目录项 (32 字节)
 * --------------------------------------------------------------- */
typedef struct {
    char      name[8];
    char      ext[3];
    uint8_t   attrs;
    uint8_t   nt_reserved;
    uint8_t   create_time_tenth;
    uint16_t  create_time;
    uint16_t  create_date;
    uint16_t  last_access_date;
    uint16_t  first_cluster_hi;
    uint16_t  last_mod_time;
    uint16_t  last_mod_date;
    uint16_t  first_cluster_lo;
    uint32_t  file_size;
} __attribute__((packed)) fat32_dir_entry_t;

/* ---------------------------------------------------------------
 * FAT32 长文件名目录项 (32 字节)
 * --------------------------------------------------------------- */
#define LFN_ENTRY_ORDER_MASK   0x1F
#define LFN_ENTRY_LAST         0x40   /* 最后一个 LFN 项标志 */

typedef struct {
    uint8_t   order;          /* 序号 (0x40|N..1) */
    uint16_t  name1[5];       /* 名字片段 1 (5个UTF-16字符) */
    uint8_t   attrs;          /* 固定 0x0F */
    uint8_t   type;           /* 固定 0x00 */
    uint8_t   checksum;       /* 短文件名校验和 */
    uint16_t  name2[6];       /* 名字片段 2 (6个字符) */
    uint16_t  first_cluster;  /* 固定 0x0000 */
    uint16_t  name3[2];       /* 名字片段 3 (2个字符) */
} __attribute__((packed)) fat32_lfn_entry_t;

/* ---------------------------------------------------------------
 * FAT32 内部文件系统上下文
 * --------------------------------------------------------------- */
typedef struct {
    block_dev_t *dev;           /* 块设备 */
    uint64_t     partition_lba; /* 分区起始 LBA */

    /* 从 BPB 解析的值 */
    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  reserved_sector_count;
    uint32_t  num_fats;
    uint32_t  fat_size;         /* FAT32 的扇区数 (fat_size_32) */
    uint32_t  root_cluster;
    uint32_t  data_region_lba;  /* 数据区起始 LBA */

    /* 缓存: 当前 FAT 扇区 */
    uint8_t  *fat_cache;        /* 指向 fat_cache_buf */
    uint32_t  fat_cache_sector; /* 缓存的扇区号 (0=无效) */

    /* FAT 扇区数据缓存 (内嵌, 避免动态分配) */
    uint8_t   fat_cache_buf[512];
} fat32_fs_t;

/* ---------------------------------------------------------------
 * FAT32 内部文件句柄
 * --------------------------------------------------------------- */
typedef struct {
    fat32_fs_t *fs;             /* 所属文件系统 (用于 seek 时遍历簇链) */
    uint32_t  first_cluster;    /* 文件起始簇 */
    uint32_t  current_cluster;  /* 当前簇 */
    uint32_t  cluster_offset;   /* 当前簇内偏移 (字节) */
    uint64_t  size;             /* 文件总大小 */
    uint64_t  offset;           /* 全局文件偏移 */
    uint32_t  dir_sector_lba;   /* 目录项所在扇区 (用于写回) */
    uint32_t  dir_entry_offset; /* 目录项在扇区中的偏移 */
} fat32_file_t;

/* ---------------------------------------------------------------
 * FAT32 驱动接口
 * --------------------------------------------------------------- */

/* 尝试挂载 FAT32 文件系统
 *   dev           - 块设备
 *   partition_lba - 分区起始 LBA (MBR 中的 lba_first)
 *   out_fs        - 输出: 挂载成功后的上下文
 * 返回: 0=成功, 负值=错误码 */
int fat32_mount(block_dev_t *dev, uint32_t partition_lba,
                fat32_fs_t *out_fs);

/* 打开文件 (绝对路径)
 *   fs   - FAT32 文件系统上下文
 *   path - 路径 (如 "/dir/file.txt")
 *   out  - 输出: 文件句柄
 * 返回: 0=成功, 负值=错误码 */
int fat32_open_file(fat32_fs_t *fs, const char *path,
                    fat32_file_t *out);

/* 从文件读取数据
 *   fs         - FAT32 文件系统
 *   file       - 文件句柄
 *   buffer     - 输出缓冲区
 *   size       - 请求读取字节数
 *   bytes_read - 输出: 实际读取字节数
 * 返回: 0=成功, 负值=错误码 */
int fat32_read_file(fat32_fs_t *fs, fat32_file_t *file,
                    void *buffer, uint32_t size,
                    uint32_t *bytes_read);

/* 关闭文件 (清空文件句柄) */
void fat32_close_file(fat32_file_t *file);

/* 设置文件偏移 */
int fat32_seek_file(fat32_file_t *file, uint64_t offset);

/* 读取目录内容
 *   fs      - FAT32 文件系统
 *   path    - 目录路径
 *   entries - 输出缓冲区
 *   max     - 最大项数
 * 返回: 实际项数 (>=0), 负值=错误码 */
int fat32_read_dir(fat32_fs_t *fs, const char *path,
                   fs_dirent_t *entries, int max);

/* 检查路径是否为目录
 *   fs   - FAT32 文件系统
 *   path - 路径
 * 返回: 1=是目录, 0=否, 负值=错误 */
int fat32_is_directory(fat32_fs_t *fs, const char *path);

#endif /* DIMENSOS_FAT32_H */
