/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 3: 文件系统抽象层
 *
 * 文件: ntfs.h
 * 描述: NTFS 文件系统驱动 — 只读 (基础版)
 *       实现 VBR 解析、MFT 入口读取、属性解析、目录索引。
 */

#ifndef DIMENSOS_NTFS_H
#define DIMENSOS_NTFS_H

#include <stdint.h>
#include "fs.h"

/* ---------------------------------------------------------------
 * NTFS 文件系统标识
 * --------------------------------------------------------------- */
#define NTFS_OEM_SIGNATURE  0x202020205346544EULL  /* "NTFS    " (小端序) */

/* ---------------------------------------------------------------
 * NTFS MFT 入口签名
 * --------------------------------------------------------------- */
#define MFT_SIGNATURE_FILE  0x454C4946  /* "FILE" */
#define MFT_SIGNATURE_INDX  0x58444E49  /* "INDX" */

/* ---------------------------------------------------------------
 * MFT 入口标志
 * --------------------------------------------------------------- */
#define MFT_ENTRY_IN_USE        0x01
#define MFT_ENTRY_IS_DIRECTORY  0x02

/* ---------------------------------------------------------------
 * MFT 标准属性类型
 * --------------------------------------------------------------- */
#define ATTR_TYPE_UNUSED            0x00
#define ATTR_TYPE_STANDARD_INFO     0x10
#define ATTR_TYPE_ATTRIBUTE_LIST    0x20
#define ATTR_TYPE_FILE_NAME         0x30
#define ATTR_TYPE_OBJECT_ID         0x40
#define ATTR_TYPE_SECURITY_DESC     0x50
#define ATTR_TYPE_VOLUME_NAME       0x60
#define ATTR_TYPE_VOLUME_INFO       0x70
#define ATTR_TYPE_DATA              0x80
#define ATTR_TYPE_INDEX_ROOT        0x90
#define ATTR_TYPE_INDEX_ALLOCATION  0xA0
#define ATTR_TYPE_BITMAP            0xB0
#define ATTR_TYPE_REPARSE_POINT     0xC0

/* ---------------------------------------------------------------
 * 属性标志
 * --------------------------------------------------------------- */
#define ATTR_FLAG_COMPRESSED    0x0001
#define ATTR_FLAG_ENCRYPTED     0x4000
#define ATTR_FLAG_SPARSE        0x8000

/* ---------------------------------------------------------------
 * NTFS VBR 结构 (部分字段)
 * --------------------------------------------------------------- */
typedef struct {
    uint8_t   jmp_boot[3];
    char      oem_name[8];       /* "NTFS    " */
    uint16_t  bytes_per_sector;
    uint8_t   sectors_per_cluster;
    uint8_t   reserved[7];
    uint8_t   media_descriptor;
    uint8_t   reserved2[2];
    uint16_t  spt;               /* sectors per track */
    uint16_t  heads;
    uint32_t  hidden_sectors;
    uint32_t  reserved3[2];
    uint64_t  total_sectors;
    uint64_t  mft_lcn;           /* MFT 起始逻辑簇号 */
    uint64_t  mft_mirror_lcn;
    int8_t    clusters_per_mft_record;  /* 正数=簇数, 负数=log2大小 */
    int8_t    reserved4[3];
    int8_t    clusters_per_index_record;
    int8_t    reserved5[3];
    uint64_t  volume_serial;
    uint32_t  checksum;
} __attribute__((packed)) ntfs_vbr_t;

/* ---------------------------------------------------------------
 * MFT 入口头
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t  signature;         /* "FILE" */
    uint16_t  fixup_offset;
    uint16_t  fixup_count;
    uint8_t   reserved[8];
    uint16_t  sequence_number;
    uint16_t  link_count;
    uint16_t  attr_offset;       /* 第一个属性相对于入口头的偏移 */
    uint16_t  flags;             /* MFT_ENTRY_IN_USE, MFT_ENTRY_IS_DIRECTORY */
    uint32_t  bytes_used;
    uint32_t  bytes_allocated;
    uint64_t  base_mft_record;
    uint16_t  next_attr_id;
} __attribute__((packed)) ntfs_mft_header_t;

/* ---------------------------------------------------------------
 * 属性头 (公共部分)
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t  type;
    uint32_t  length;
    uint8_t   non_resident;
    uint8_t   name_length;
    uint16_t  name_offset;
    uint16_t  flags;             /* ATTR_FLAG_* */
    uint16_t  instance;
    /* 联合: resident / non-resident 的区别从这里开始 */
} __attribute__((packed)) ntfs_attr_header_t;

/* ---------------------------------------------------------------
 * 常驻属性头扩展
 * --------------------------------------------------------------- */
typedef struct {
    ntfs_attr_header_t header;
    uint32_t  value_length;
    uint16_t  value_offset;      /* 相对于属性头起始 */
    uint8_t   resident_flags;
    int8_t    reserved;
} __attribute__((packed)) ntfs_attr_resident_t;

/* ---------------------------------------------------------------
 * 非常驻属性头扩展
 * --------------------------------------------------------------- */
typedef struct {
    ntfs_attr_header_t header;
    uint64_t  lowest_vcn;
    uint64_t  highest_vcn;
    uint16_t  runlist_offset;    /* 数据运行列表偏移 (相对于属性头) */
    uint8_t   compression_unit;
    uint8_t   reserved[5];
    uint64_t  alloc_size;
    uint64_t  real_size;
    uint64_t  init_size;
} __attribute__((packed)) ntfs_attr_nonresident_t;

/* ---------------------------------------------------------------
 * $FILE_NAME 属性内容
 *
 * 包含文件名 (UTF-16) 在 name 字段的末尾。
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t  parent_mft_ref;
    uint64_t  creation_time;
    uint64_t  last_mod_time;
    uint64_t  last_access_time;
    uint64_t  alloc_size;
    uint64_t  real_size;
    uint32_t  file_attributes;
    uint32_t  ea_size_or_unk;
    uint8_t   name_length;
    uint8_t   name_type;         /* 0x01=long, 0x02=short (8.3) */
    /* uint16_t name[]; 跟着的 UTF-16 名字 */
} __attribute__((packed)) ntfs_filename_attr_t;

/* ---------------------------------------------------------------
 * $INDEX_ROOT 头
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t  attr_type;         /* 索引的属性类型 (通常是 $FILE_NAME) */
    uint32_t  collation_rule;
    uint32_t  index_block_size;
    int8_t    clusters_per_index_block;
} __attribute__((packed)) ntfs_index_root_header_t;

/* ---------------------------------------------------------------
 * 索引项头
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t  mft_ref;           /* MFT 引用 (包含序列号) */
    uint16_t  index_entry_length;
    uint16_t  stream_length;     /* 属性流长度 */
    uint8_t   flags;             /* 0x01=子节点存在, 0x02=最后一项 */
    uint8_t   reserved[3];
    /* 后面跟属性数据 (如 $FILE_NAME) */
} __attribute__((packed)) ntfs_index_entry_header_t;

/* ---------------------------------------------------------------
 * NTFS 内部文件系统上下文
 * --------------------------------------------------------------- */
typedef struct {
    block_dev_t *dev;
    uint64_t     partition_lba;

    uint32_t  bytes_per_sector;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint64_t  total_sectors;

    uint64_t  mft_lcn;                    /* $MFT 的逻辑簇号 */
    uint32_t  mft_record_size;            /* MFT 记录大小 (字节) */

    uint8_t  *mft_record_buf;             /* MFT 记录读取缓冲区 */
} ntfs_fs_t;

/* ---------------------------------------------------------------
 * NTFS 内部文件句柄
 *
 * 文件的 MFT entry 号决定了文件的所有元数据。
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t  mft_ref;            /* MFT 条目号 */
    uint64_t  size;               /* 文件大小 */
    uint64_t  offset;             /* 当前偏移 */

    /* 对非常驻 $DATA 属性的数据运行列表缓存 */
    uint64_t  data_real_size;     /* $DATA 的实际大小 */
    uint64_t  data_alloc_size;    /* $DATA 的分配大小 */
    uint8_t  *runlist_cache;      /* 数据运行列表缓存 */
    uint32_t  runlist_size;       /* 数据运行列表大小 */

    uint8_t   is_directory;       /* 是否是目录 */
    uint8_t   data_resident;      /* $DATA 是否常驻 */
    uint16_t  data_res_off;       /* 常驻数据的偏移 (相对属性头) */
    uint32_t  data_res_len;       /* 常驻数据的长度 */
    uint16_t  runlist_offset;     /* 数据运行列表偏移 */
} ntfs_file_t;

/* ---------------------------------------------------------------
 * NTFS 驱动接口
 * --------------------------------------------------------------- */

/* 尝试挂载 NTFS 文件系统
 * 返回: 0=成功, 负值=错误码 */
int ntfs_mount(block_dev_t *dev, uint64_t partition_lba,
               ntfs_fs_t *out_fs);

/* 打开文件/目录 (通过 MFT 条目号)
 * 返回: 0=成功, 负值=错误码 */
int ntfs_open_by_ref(ntfs_fs_t *fs, uint64_t mft_ref,
                     ntfs_file_t *out);

/* 打开文件/目录 (通过路径)
 * 返回: 0=成功, 负值=错误码 */
int ntfs_open_file(ntfs_fs_t *fs, const char *path,
                   ntfs_file_t *out);

/* 从文件读取数据
 * 返回: 0=成功, 负值=错误码 */
int ntfs_read_file(ntfs_fs_t *fs, ntfs_file_t *file,
                   void *buffer, uint64_t size,
                   uint64_t *bytes_read);

/* 关闭文件 */
void ntfs_close_file(ntfs_file_t *file);

/* 设置文件偏移 */
int ntfs_seek_file(ntfs_file_t *file, uint64_t offset);

/* 读取目录内容
 * 返回: 实际项数 (>=0), 负值=错误码 */
int ntfs_read_dir(ntfs_fs_t *fs, const char *path,
                  fs_dirent_t *entries, int max);

/* 获取 $MFT 中第 index 个 MFT 记录的偏移 (LBA) */
int ntfs_get_mft_lba(ntfs_fs_t *fs, uint64_t mft_index,
                     uint64_t *out_lba);

/* 应用 MFT 修复 (fixup) */
int ntfs_apply_fixup(uint8_t *record, uint32_t record_size);

/* 解析数据运行列表, 获取指定 VCN 的 LBA */
int ntfs_resolve_vcn(ntfs_fs_t *fs, const uint8_t *runlist,
                     uint32_t runlist_size, uint64_t vcn,
                     uint64_t *out_lba, uint64_t *run_length);

#endif /* DIMENSOS_NTFS_H */
