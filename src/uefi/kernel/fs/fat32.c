/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 3: 文件系统抽象层
 *
 * 文件: fat32.c
 * 描述: FAT32 文件系统驱动实现 (只读)
 *       支持 MBR 分区表解析、簇链遍历、长/短文件名。
 */

#include "fat32.h"
#include "../lib/io.h"

/* 自定义内存操作 (freestanding 兼容) */
static void mem_zero(void *dst, uint64_t size)
{
    uint8_t *d = (uint8_t *)dst;
    while (size--) *d++ = 0;
}

static int str_icmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

static int str_len(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

/* ---------------------------------------------------------------
 * 内部辅助函数
 * --------------------------------------------------------------- */

/* 读取扇区 (简写) */
static inline int sector_read(block_dev_t *dev, uint64_t lba,
                              uint32_t count, void *buf)
{
    return dev->read_sectors(dev->device_context, lba, count, buf);
}

/* 将簇号转换为 LBA */
static inline uint32_t cluster_to_lba(const fat32_fs_t *fs, uint32_t cluster)
{
    if (cluster == 0) {
        /* FAT12/FAT16 的根目录: 在保留扇区 + FAT 表之后 */
        return fs->partition_lba + fs->reserved_sector_count 
            + (fs->num_fats * fs->fat_size);
    }
    if (cluster < 2)
        return 0;
    return fs->data_region_lba + (cluster - 2) * fs->sectors_per_cluster;
}

/* 计算短文件名 8.3 校验和 (与 LFN 匹配) */
static uint8_t lfn_checksum(const char *short_name_83)
{
    int i;
    uint8_t sum = 0;
    for (i = 11; i > 0; i--)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)(short_name_83[i - 1]);
    return sum;
}

/* 清除 FAT 缓存 */
static void fat_cache_invalidate(fat32_fs_t *fs)
{
    fs->fat_cache_sector = 0;
}

/* 从 FAT 表中读取一个簇的下一个簇 */
static int get_next_cluster(fat32_fs_t *fs, uint32_t current, uint32_t *next)
{
    uint32_t fat_lba;
    uint32_t sector_in_fat;
    uint32_t offset_in_sector;
    uint32_t cached;

    if (current >= FAT32_CLUSTER_MAX)
        return FS_ERR_INVALID;

    fat_lba = fs->partition_lba + fs->reserved_sector_count;
    sector_in_fat = (current * 4) / fs->bytes_per_sector;  /* 每个FAT项4字节 */
    offset_in_sector = (current * 4) % fs->bytes_per_sector;

    /* 检查缓存 */
    if (fs->fat_cache_sector == fat_lba + sector_in_fat && fs->fat_cache != NULL) {
        cached = *(uint32_t *)(fs->fat_cache + offset_in_sector);
        *next = cached & 0x0FFFFFFF;
        return FS_OK;
    }

    /* 读取 FAT 扇区到缓存 */
    if (fs->fat_cache == NULL)
        return FS_ERR_NO_MEM;

    if (sector_read(fs->dev, fat_lba + sector_in_fat, 1, fs->fat_cache) != 0)
        return FS_ERR_IO;

    fs->fat_cache_sector = fat_lba + sector_in_fat;

    cached = *(uint32_t *)(fs->fat_cache + offset_in_sector);
    *next = cached & 0x0FFFFFFF;
    return FS_OK;
}

/* ---------------------------------------------------------------
 * 路径解析辅助
 * --------------------------------------------------------------- */

/* ---------------------------------------------------------------
 * 长文件名拼接 (保留 format_short_name, 其他 LFN 函数暂未使用)
 * --------------------------------------------------------------- */

/* 从短文件名 (8+3) 构建可读文件名 */
static void format_short_name(const uint8_t *name_83, char *out, int max_len)
{
    int i, pos = 0;
    int skip_spaces = 1;

    /* 复制名字 (去掉尾部空格) */
    for (i = 7; i >= 0; i--) {
        if (name_83[i] != ' ') {
            skip_spaces = 0;
            break;
        }
    }
    for (i = 0; i <= 7 && name_83[i] != ' ' && pos < max_len - 1; i++) {
        out[pos++] = (char)name_83[i];
    }

    /* 扩展名 */
    skip_spaces = 1;
    for (i = 10; i >= 8; i--) {
        if (name_83[i] != ' ') {
            skip_spaces = 0;
            break;
        }
    }
    if (!skip_spaces) {
        out[pos++] = '.';
        for (i = 8; i <= 10 && name_83[i] != ' ' && pos < max_len - 1; i++) {
            out[pos++] = (char)name_83[i];
        }
    }

    out[pos] = '\0';
}



/* ---------------------------------------------------------------
 * fat32_mount - 挂载 FAT12/FAT16/FAT32 文件系统
 * --------------------------------------------------------------- */
int fat32_mount(block_dev_t *dev, uint32_t partition_lba,
                fat32_fs_t *out_fs)
{
    /* 使用静态缓冲区确保对齐 */
    static fat32_bpb_t bpb __attribute__((aligned(4096)));

    /* 读取 BPB (位于分区起始 LBA 的扇区 0) */
    if (sector_read(dev, partition_lba, 1, &bpb) != 0) {
        return FS_ERR_IO;
    }

    /* 验证 BPB */
    if (bpb.bytes_per_sector == 0 || bpb.sectors_per_cluster == 0)
        return FS_ERR_NOT_FS;

    /* 填充通用字段 */
    out_fs->dev = dev;
    out_fs->partition_lba = partition_lba;
    out_fs->bytes_per_sector = bpb.bytes_per_sector;
    out_fs->sectors_per_cluster = bpb.sectors_per_cluster;
    out_fs->bytes_per_cluster = bpb.bytes_per_sector * bpb.sectors_per_cluster;
    out_fs->reserved_sector_count = bpb.reserved_sector_count;
    out_fs->num_fats = bpb.num_fats;

    /* 判断 FAT 类型 */
    if (bpb.root_entry_count == 0 && bpb.fat_size_16 == 0) {
        /* FAT32: root_entry_count == 0 && fat_size_16 == 0 */
        out_fs->fat_size = bpb.fat_size_32;
        out_fs->root_cluster = bpb.root_cluster;
        out_fs->data_region_lba = partition_lba
            + bpb.reserved_sector_count
            + (bpb.num_fats * bpb.fat_size_32);
    } else {
        /* FAT12/FAT16: 使用 fat_size_16 */
        out_fs->fat_size = bpb.fat_size_16;
        /* FAT12/FAT16 的根目录在保留扇区 + FAT 表之后的固定位置 */
        out_fs->root_cluster = 0;  /* 标记为 FAT12/FAT16 */
        out_fs->data_region_lba = partition_lba
            + bpb.reserved_sector_count
            + (bpb.num_fats * bpb.fat_size_16)
            + ((bpb.root_entry_count * 32 + bpb.bytes_per_sector - 1) / bpb.bytes_per_sector);
    }

    /* FAT 缓存指向内嵌的 fat_cache_buf */
    out_fs->fat_cache = out_fs->fat_cache_buf;
    out_fs->fat_cache_sector = 0;

    return FS_OK;
}

/* ---------------------------------------------------------------
 * fat32_open_file - 打开文件 (路径遍历)
 * --------------------------------------------------------------- */
int fat32_open_file(fat32_fs_t *fs, const char *path,
                    fat32_file_t *out)
{
    /* 清零输出 */
    mem_zero(out, sizeof(*out));

    /* 设置文件系统指针 (用于 seek 时遍历簇链) */
    out->fs = fs;

    /* 跳过前导 '/' */
    while (*path == '/') path++;

    if (*path == '\0') {
        /* 根目录 */
        out->first_cluster = fs->root_cluster;
        out->current_cluster = fs->root_cluster;
        out->size = 0;  /* 目录大小未知, 设为0 */
        out->offset = 0;
        return FS_OK;
    }

    /* 逐组件遍历路径 */
    uint32_t current_cluster = fs->root_cluster;
    const char *p = path;
    char component[256];

    while (*p != '\0') {
        /* 提取一个路径组件 */
        int ci = 0;
        while (*p != '\0' && *p != '/' && ci < 255)
            component[ci++] = *p++;
        component[ci] = '\0';
        if (*p == '/') p++;

        if (ci == 0) continue;

        /* 在 current_cluster 目录中查找组件 */
        fat32_dir_entry_t entry;
        /* 使用栈上单扇区缓冲区扫描目录 */
        uint8_t sector_buf[512];
        uint32_t cluster = current_cluster;
        int found = 0;

        while (1) {
            uint32_t lba = cluster_to_lba(fs, cluster);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
                if (sector_read(fs->dev, lba + s, 1, sector_buf) != 0)
                    return FS_ERR_IO;

                for (uint32_t e = 0; e < fs->bytes_per_sector / 32; e++) {
                    fat32_dir_entry_t *ent =
                        (fat32_dir_entry_t *)(sector_buf + e * 32);

                    if (ent->name[0] == 0x00) {
                        /* 目录结束 */
                        goto not_found;
                    }
                    if ((uint8_t)ent->name[0] == 0xE5) continue;
                    if (ent->attrs == FAT32_ATTR_LFN) continue;
                    if (ent->attrs & FAT32_ATTR_VOLUME_ID) continue;

                    /* 短文件名匹配 */
                    char short_name[13];
                    format_short_name((const uint8_t *)ent->name,
                                     short_name, sizeof(short_name));
                    if (str_icmp(short_name, component) == 0) {
                        entry = *ent;
                        found = 1;
                        goto found_it;
                    }
                }
            }

            /* 下一个簇 */
            uint32_t next;
            int ret = get_next_cluster(fs, cluster, &next);
            if (ret != FS_OK) return ret;
            if (next >= FAT32_CLUSTER_LAST) break;
            cluster = next;
        }

        found_it:
        if (!found)
            goto not_found;

        /* 如果是最后一个组件, 或者中间组件必须是目录 */
        if (*p == '\0') {
            /* 最后一个组件 */
            uint32_t fc = ((uint32_t)entry.first_cluster_hi << 16)
                         | entry.first_cluster_lo;
            out->first_cluster = fc;
            out->current_cluster = fc;
            out->size = entry.file_size;
            out->offset = 0;
            return FS_OK;
        } else {
            /* 必须是目录 */
            if (!(entry.attrs & FAT32_ATTR_DIRECTORY))
                return FS_ERR_NOT_DIR;
            uint32_t fc = ((uint32_t)entry.first_cluster_hi << 16)
                         | entry.first_cluster_lo;
            current_cluster = (fc != 0) ? fc : fs->root_cluster;
        }
    }

    /* 整个路径就是根目录 */
    out->first_cluster = current_cluster;
    out->current_cluster = current_cluster;
    out->size = 0;
    out->offset = 0;
    return FS_OK;

not_found:
    return FS_ERR_NOT_FOUND;
}

/* ---------------------------------------------------------------
 * fat32_read_file - 读取文件数据
 * --------------------------------------------------------------- */
int fat32_read_file(fat32_fs_t *fs, fat32_file_t *file,
                    void *buffer, uint32_t size,
                    uint32_t *bytes_read)
{
    uint8_t *buf = (uint8_t *)buffer;
    uint32_t total = 0;
    uint32_t to_read;
    int ret;

    /* 限制读取不超过文件大小 */
    if (file->offset >= file->size) {
        *bytes_read = 0;
        return FS_OK;
    }

    if (file->offset + size > file->size)
        to_read = (uint32_t)(file->size - file->offset);
    else
        to_read = size;

    /* 如果当前簇无效, 定位到 offset 所在的簇 */
    if (file->current_cluster == 0) {
        /* 回到开头 */
        file->current_cluster = file->first_cluster;
        file->cluster_offset = 0;
        file->offset = 0;
    }

    while (total < to_read) {
        uint32_t cluster_avail;  /* 当前簇剩余可读字节 */
        uint32_t cluster_lba;
        uint32_t sector_start;
        uint32_t sector_off;
        uint32_t read_chunk;
        uint8_t sector_buf[512];

        if (file->current_cluster < 2 || file->current_cluster >= FAT32_CLUSTER_LAST)
            break;  /* 无效簇 */

        cluster_lba = cluster_to_lba(fs, file->current_cluster);
        cluster_avail = fs->bytes_per_cluster - file->cluster_offset;

        if (cluster_avail == 0) {
            /* 已读完当前簇, 跳到下一个 */
            uint32_t next;
            ret = get_next_cluster(fs, file->current_cluster, &next);
            if (ret != FS_OK) break;
            if (next >= FAT32_CLUSTER_LAST) break;  /* 链尾 */
            file->current_cluster = next;
            file->cluster_offset = 0;
            cluster_lba = cluster_to_lba(fs, next);
            cluster_avail = fs->bytes_per_cluster;
        }

        /* 在当前簇内读取 */
        sector_start = file->cluster_offset / fs->bytes_per_sector;
        sector_off = file->cluster_offset % fs->bytes_per_sector;

        /* 逐个扇区读 */
        for (uint32_t s = sector_start;
             s < fs->sectors_per_cluster && total < to_read; s++) {

            if (sector_read(fs->dev, cluster_lba + s, 1, sector_buf) != 0) {
                ret = FS_ERR_IO;
                goto done;
            }

            read_chunk = fs->bytes_per_sector - sector_off;
            if (read_chunk > to_read - total)
                read_chunk = to_read - total;
            if (read_chunk > 0) {
                /* 复制数据 */
                for (uint32_t k = 0; k < read_chunk; k++)
                    buf[total + k] = sector_buf[sector_off + k];
                total += read_chunk;
                file->offset += read_chunk;
                file->cluster_offset += read_chunk;
                sector_off = 0;

                /* 如果到了簇末尾, 准备跳到下一个簇 */
                if (file->cluster_offset >= fs->bytes_per_cluster) {
                    uint32_t nxt;
                    ret = get_next_cluster(fs, file->current_cluster, &nxt);
                    if (ret != FS_OK || nxt >= FAT32_CLUSTER_LAST)
                        goto done;
                    file->current_cluster = nxt;
                    file->cluster_offset = 0;
                    break;  /* 跳到外层循环继续 */
                }
            }
        }
    }

done:
    *bytes_read = total;
    return FS_OK;
}

/* ---------------------------------------------------------------
 * fat32_close_file - 关闭文件
 * --------------------------------------------------------------- */
void fat32_close_file(fat32_file_t *file)
{
    mem_zero(file, sizeof(*file));
}

/* ---------------------------------------------------------------
 * fat32_seek_file - 设置文件偏移
 * --------------------------------------------------------------- */
int fat32_seek_file(fat32_file_t *file, uint64_t offset)
{
    /* 边界检查 */
    if (offset > file->size)
        offset = file->size;

    /* 如果偏移为0, 回到文件开头 */
    if (offset == 0) {
        file->current_cluster = file->first_cluster;
        file->cluster_offset = 0;
        file->offset = 0;
        return FS_OK;
    }

    /* 如果目标偏移小于当前偏移, 从头开始遍历 */
    if (offset < file->offset) {
        file->current_cluster = file->first_cluster;
        file->cluster_offset = 0;
        file->offset = 0;
    }

    /* 计算需要跳过的簇数 */
    uint64_t bytes_to_skip = offset - file->offset;
    uint64_t clusters_to_skip = bytes_to_skip / file->fs->bytes_per_cluster;

    /* 跳过完整的簇 */
    for (uint64_t i = 0; i < clusters_to_skip; i++) {
        if (file->current_cluster < 2 || file->current_cluster >= FAT32_CLUSTER_LAST)
            return FS_ERR_INVALID;

        uint32_t next;
        int ret = get_next_cluster(file->fs, file->current_cluster, &next);
        if (ret != FS_OK || next >= FAT32_CLUSTER_LAST)
            return FS_ERR_INVALID;

        file->current_cluster = next;
        file->offset += file->fs->bytes_per_cluster;
        file->cluster_offset = 0;
    }

    /* 计算簇内偏移 */
    uint64_t remaining = offset - file->offset;
    file->cluster_offset = (uint32_t)remaining;
    file->offset = offset;

    return FS_OK;
}

/* ---------------------------------------------------------------
 * fat32_read_dir - 读取目录内容
 * --------------------------------------------------------------- */
int fat32_read_dir(fat32_fs_t *fs, const char *path,
                   fs_dirent_t *entries, int max)
{
    fat32_file_t dir_file;
    uint32_t cluster;
    uint8_t sector_buf[512];
    int entry_count = 0;
    int ret;
    uint32_t i;

    /* 打开目录 */
    ret = fat32_open_file(fs, path, &dir_file);
    if (ret != FS_OK)
        return ret;

    cluster = dir_file.first_cluster;
    if (cluster == 0)
        cluster = fs->root_cluster;

    /* 遍历目录簇 */
    while (cluster >= 2 && cluster < FAT32_CLUSTER_LAST && entry_count < max) {
        uint32_t lba = cluster_to_lba(fs, cluster);

        for (uint32_t s = 0; s < fs->sectors_per_cluster && entry_count < max; s++) {
            if (sector_read(fs->dev, lba + s, 1, sector_buf) != 0) {
                fat32_close_file(&dir_file);
                return FS_ERR_IO;
            }

            for (uint32_t e = 0; e < fs->bytes_per_sector / 32; e++) {
                fat32_dir_entry_t *entry =
                    (fat32_dir_entry_t *)(sector_buf + e * 32);

                if (entry->name[0] == 0x00) {
                    /* 目录结束 */
                    fat32_close_file(&dir_file);
                    return entry_count;  /* 返回已找到的项 */
                }
                if ((uint8_t)entry->name[0] == 0xE5) continue;
                if (entry->attrs == FAT32_ATTR_LFN) continue;
                if (entry->attrs & FAT32_ATTR_VOLUME_ID) continue;

                /* 格式化名称 */
                format_short_name((const uint8_t *)entry->name,
                                 entries[entry_count].name,
                                 FS_MAX_NAME);

                entries[entry_count].size = entry->file_size;
                entries[entry_count].attrs = entry->attrs;
                entries[entry_count].is_directory =
                    (entry->attrs & FAT32_ATTR_DIRECTORY) ? 1 : 0;

                entry_count++;
            }
        }

        /* 下一个簇 */
        uint32_t next;
        ret = get_next_cluster(fs, cluster, &next);
        if (ret != FS_OK) break;
        if (next >= FAT32_CLUSTER_LAST) break;
        cluster = next;
    }

    fat32_close_file(&dir_file);
    return entry_count;
}


