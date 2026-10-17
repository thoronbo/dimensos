/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 3: 文件系统抽象层
 *
 * 文件: ntfs.c
 * 描述: NTFS 文件系统驱动实现 (只读, 基础版)
 *       支持 MFT 解析、属性遍历、数据运行解析、目录索引。
 */

#include "ntfs.h"

/* memset 声明 (由 GCC 内建函数提供) */
void *memset(void *s, int c, uint64_t n);

/* ---------------------------------------------------------------
 * 内部辅助
 * --------------------------------------------------------------- */

static inline int sector_read(block_dev_t *dev, uint64_t lba,
                              uint32_t count, void *buf)
{
    return dev->read_sectors(dev->device_context, lba, count, buf);
}

/* 将 LCN 转换为 LBA (相对于分区起始) */
static inline uint64_t lcn_to_lba(ntfs_fs_t *fs, int64_t lcn)
{
    return fs->partition_lba + (uint64_t)lcn * fs->sectors_per_cluster;
}

/* ---------------------------------------------------------------
 * MFT 修复 (Fixup)
 *
 * NTFS 使用 fixup 机制来检测介质错误:
 *   1. 每个扇区的最后 2 个字节被替换为 fixup 值
 *   2. 原始的 2 个字节存储在 fixup 数组中
 * --------------------------------------------------------------- */
int ntfs_apply_fixup(uint8_t *record, uint32_t record_size)
{
    ntfs_mft_header_t *hdr = (ntfs_mft_header_t *)record;
    uint16_t *fixup_array;
    uint16_t fixup_value;
    uint32_t i;
    uint32_t sectors = record_size / 512;

    if (hdr->signature != MFT_SIGNATURE_FILE &&
        hdr->signature != MFT_SIGNATURE_INDX)
        return FS_ERR_INVALID;

    if (hdr->fixup_offset == 0 || hdr->fixup_count < 2)
        return FS_ERR_INVALID;

    fixup_array = (uint16_t *)(record + hdr->fixup_offset);
    fixup_value = fixup_array[0];  /* 第一个是 fixup 值 */

    for (i = 1; i < hdr->fixup_count && i <= sectors; i++) {
        uint16_t *sector_end = (uint16_t *)(record + i * 512 - 2);
        *sector_end = fixup_array[i];  /* 恢复原始值 */
    }

    return FS_OK;
}

/* ---------------------------------------------------------------
 * 获取 MFT 记录的 LBA
 * --------------------------------------------------------------- */
int ntfs_get_mft_lba(ntfs_fs_t *fs, uint64_t mft_index, uint64_t *out_lba)
{
    uint64_t byte_offset = mft_index * fs->mft_record_size;
    uint64_t cluster_offset = byte_offset / fs->bytes_per_cluster;
    uint64_t lcn;

    /* 使用 $MFT 的数据运行列表来解析偏移 */
    /* 简化: 假设 $MFT 是连续的 (通常如此) */
    lcn = fs->mft_lcn + cluster_offset;
    *out_lba = lcn_to_lba(fs, lcn)
               + (byte_offset % fs->bytes_per_cluster) / fs->bytes_per_sector;

    return FS_OK;
}

/* ---------------------------------------------------------------
 * 读取 MFT 记录
 * --------------------------------------------------------------- */
static int read_mft_record(ntfs_fs_t *fs, uint64_t mft_index,
                           uint8_t *buffer)
{
    uint64_t lba;
    uint32_t sectors_per_record;
    uint32_t i;
    int ret;

    ret = ntfs_get_mft_lba(fs, mft_index, &lba);
    if (ret != FS_OK)
        return ret;

    sectors_per_record = fs->mft_record_size / fs->bytes_per_sector;

    if (sector_read(fs->dev, lba, sectors_per_record, buffer) != 0)
        return FS_ERR_IO;

    ret = ntfs_apply_fixup(buffer, fs->mft_record_size);
    return ret;
}

/* ---------------------------------------------------------------
 * 在 MFT 记录中查找指定类型的属性
 * --------------------------------------------------------------- */
static const uint8_t *find_attribute(const uint8_t *mft_record,
                                     uint32_t attr_type,
                                     int index)
{
    const ntfs_mft_header_t *hdr = (const ntfs_mft_header_t *)mft_record;
    const ntfs_attr_header_t *attr;
    uint16_t offset;
    int found = 0;

    offset = hdr->attr_offset;

    while (offset < hdr->bytes_used) {
        attr = (const ntfs_attr_header_t *)(mft_record + offset);

        if (attr->type == 0xFFFFFFFF)
            break;  /* 属性结束标志 */

        if (attr->type == attr_type) {
            if (found == index)
                return (const uint8_t *)attr;
            found++;
        }

        if (attr->length == 0)
            break;

        offset += attr->length;
    }

    return NULL;
}

/* ---------------------------------------------------------------
 * VCN → LBA (通过数据运行列表)
 * --------------------------------------------------------------- */
int ntfs_resolve_vcn(ntfs_fs_t *fs, const uint8_t *runlist,
                     uint32_t runlist_size, uint64_t vcn,
                     uint64_t *out_lba, uint64_t *run_length)
{
    uint64_t current_vcn = 0;
    uint32_t pos = 0;
    int64_t current_lcn = 0;

    while (pos < runlist_size) {
        uint8_t header = runlist[pos];
        int count_bytes = header & 0x0F;
        int lcn_bytes = (header >> 4) & 0x0F;
        uint64_t cluster_count = 0;
        int64_t lcn_offset = 0;
        uint32_t i;

        if (count_bytes == 0)
            break;  /* 运行列表结束 */

        pos++;
        if (pos + count_bytes + lcn_bytes > runlist_size)
            break;

        /* 读取簇数 */
        for (i = 0; i < count_bytes; i++)
            cluster_count |= (uint64_t)runlist[pos + i] << (i * 8);

        /* 读取 LCN 偏移 (有符号) */
        for (i = 0; i < lcn_bytes; i++)
            lcn_offset |= (uint64_t)runlist[pos + count_bytes + i] << (i * 8);

        /* 符号扩展 */
        if (lcn_bytes > 0 && (runlist[pos + count_bytes + lcn_bytes - 1] & 0x80))
            lcn_offset |= ~((1ULL << (lcn_bytes * 8)) - 1);

        pos += count_bytes + lcn_bytes;

        if (vcn >= current_vcn && vcn < current_vcn + cluster_count) {
            uint64_t offset_in_run = vcn - current_vcn;
            if (lcn_offset == 0) {
                /* 稀疏运行 */
                *out_lba = 0;
                *run_length = cluster_count - offset_in_run;
                return FS_OK;
            }
            current_lcn += lcn_offset;
            *out_lba = lcn_to_lba(fs, current_lcn + offset_in_run);
            *run_length = cluster_count - offset_in_run;
            return FS_OK;
        }

        if (lcn_offset != 0)
            current_lcn += lcn_offset;

        current_vcn += cluster_count;
    }

    return FS_ERR_NOT_FOUND;
}

/* ---------------------------------------------------------------
 * ntfs_mount - 挂载 NTFS 文件系统
 * --------------------------------------------------------------- */
int ntfs_mount(block_dev_t *dev, uint64_t partition_lba,
               ntfs_fs_t *out_fs)
{
    ntfs_vbr_t vbr;
    int8_t cpmr;
    uint32_t record_size;

    /* 读取 VBR */
    if (sector_read(dev, partition_lba, 1, &vbr) != 0)
        return FS_ERR_IO;

    /* 验证 OEM 签名 */
    {
        uint64_t sig = *(uint64_t *)vbr.oem_name;
        if (sig != NTFS_OEM_SIGNATURE)
            return FS_ERR_NOT_FS;
    }

    /* 填充上下文 */
    out_fs->dev = dev;
    out_fs->partition_lba = partition_lba;
    out_fs->bytes_per_sector = vbr.bytes_per_sector;
    out_fs->sectors_per_cluster = vbr.sectors_per_cluster;
    out_fs->bytes_per_cluster = vbr.bytes_per_sector * vbr.sectors_per_cluster;
    out_fs->total_sectors = vbr.total_sectors;
    out_fs->mft_lcn = vbr.mft_lcn;

    /* 计算 MFT 记录大小 */
    cpmr = vbr.clusters_per_mft_record;
    if (cpmr > 0) {
        record_size = cpmr * out_fs->bytes_per_cluster;
    } else {
        /* 负数: 以 2 的绝对值的次幂计算 */
        record_size = 1 << (-cpmr);
    }
    out_fs->mft_record_size = record_size;

    return FS_OK;
}

/* ---------------------------------------------------------------
 * ntfs_open_by_ref - 通过 MFT 引用打开文件
 * --------------------------------------------------------------- */
int ntfs_open_by_ref(ntfs_fs_t *fs, uint64_t mft_ref,
                     ntfs_file_t *out)
{
    uint8_t record_buf[4096];  /* 足够容纳 MFT 记录 (典型 1024 字节) */
    ntfs_mft_header_t *hdr;
    const uint8_t *attr_data;
    int ret;

    memset(out, 0, sizeof(*out));

    /* 防止 MFT 记录过大导致 record_buf[4096] 栈溢出 */
    if (fs->mft_record_size > 4096) {
        return FS_ERR_INVALID;
    }

    ret = read_mft_record(fs, mft_ref, record_buf);
    if (ret != FS_OK)
        return ret;

    hdr = (ntfs_mft_header_t *)record_buf;
    if (!(hdr->flags & MFT_ENTRY_IN_USE))
        return FS_ERR_NOT_FOUND;

    out->mft_ref = mft_ref;
    out->is_directory = (hdr->flags & MFT_ENTRY_IS_DIRECTORY) ? 1 : 0;

    /* 查找 $DATA 属性 */
    attr_data = find_attribute(record_buf, ATTR_TYPE_DATA, 0);
    if (attr_data != NULL) {
        const ntfs_attr_header_t *ah = (const ntfs_attr_header_t *)attr_data;

        if (ah->non_resident) {
            const ntfs_attr_nonresident_t *nr =
                (const ntfs_attr_nonresident_t *)attr_data;
            out->data_real_size = nr->real_size;
            out->data_alloc_size = nr->alloc_size;
            out->data_resident = 0;
            out->runlist_offset = nr->runlist_offset;
            /* 复制运行列表到缓存 */
            /* 为了简化, 直接从记录读取时不缓存 */
            out->size = nr->real_size;
        } else {
            const ntfs_attr_resident_t *res =
                (const ntfs_attr_resident_t *)attr_data;
            out->data_resident = 1;
            out->data_res_off = res->value_offset;
            out->data_res_len = res->value_length;
            out->size = res->value_length;
        }
    } else {
        /* 无 $DATA 属性 (通常是目录) */
        out->size = 0;
    }

    return FS_OK;
}

/* ---------------------------------------------------------------
 * 在目录中查找组件名 (通过 $INDEX_ROOT)
 * --------------------------------------------------------------- */
static int find_in_directory(ntfs_fs_t *fs, uint64_t dir_mft_ref,
                             const char *component,
                             uint64_t *out_mft_ref)
{
    uint8_t record_buf[4096];
    const uint8_t *idx_root;
    int ret;

    ret = read_mft_record(fs, dir_mft_ref, record_buf);
    if (ret != FS_OK)
        return ret;

    /* 查找 $INDEX_ROOT 属性 */
    idx_root = find_attribute(record_buf, ATTR_TYPE_INDEX_ROOT, 0);
    if (idx_root == NULL)
        return FS_ERR_NOT_FOUND;

    /* 解析索引根 */
    const ntfs_attr_resident_t *res =
        (const ntfs_attr_resident_t *)idx_root;
    const uint8_t *data = (const uint8_t *)idx_root + res->value_offset;
    uint32_t data_len = res->value_length;

    /* 跳过索引根头 */
    const ntfs_index_root_header_t *irh =
        (const ntfs_index_root_header_t *)data;
    uint32_t offset = sizeof(ntfs_index_root_header_t);

    /* 遍历索引项 */
    while (offset + sizeof(ntfs_index_entry_header_t) <= data_len) {
        const ntfs_index_entry_header_t *ie =
            (const ntfs_index_entry_header_t *)(data + offset);

        if (ie->index_entry_length == 0)
            break;

        /* 索引项后跟着 $FILE_NAME 数据 */
        const ntfs_filename_attr_t *fn =
            (const ntfs_filename_attr_t *)(data + offset + sizeof(ntfs_index_entry_header_t));

        /* $FILE_NAME 中的名称是 UTF-16LE */
        int name_len = fn->name_length;
        /* 转换为 ASCII 并比较 */
        char ascii_name[512];
        int i;

        for (i = 0; i < name_len && i < 255; i++) {
            uint16_t ch = *(const uint16_t *)
                ((const uint8_t *)fn + sizeof(ntfs_filename_attr_t) + i * 2);
            ascii_name[i] = (ch <= 0x7F) ? (char)ch : '?';
        }
        ascii_name[i] = '\0';

        /* 不区分大小写比较 */
        {
            const char *a = ascii_name;
            const char *b = component;
            while (*a && *b) {
                char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
                char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
                if (ca != cb) break;
                a++; b++;
            }
            if (*a == '\0' && *b == '\0') {
                /* 匹配! */
                *out_mft_ref = ie->mft_ref & 0x0000FFFFFFFFFFFFULL;
                return FS_OK;
            }
        }

        if (ie->flags & 0x02)
            break;  /* 最后一项 */

        offset += ie->index_entry_length;
    }

    return FS_ERR_NOT_FOUND;
}

/* ---------------------------------------------------------------
 * ntfs_open_file - 通过路径打开文件
 * --------------------------------------------------------------- */
int ntfs_open_file(ntfs_fs_t *fs, const char *path,
                   ntfs_file_t *out)
{
    char component[256];
    const char *p = path;
    uint64_t current_mft = 5;  /* 根目录是 MFT 条目 5 */
    int ret;

    /* 跳过前导 '/' */
    while (*p == '/') p++;

    if (*p == '\0') {
        /* 根目录 */
        return ntfs_open_by_ref(fs, 5, out);
    }

    while (*p != '\0') {
        /* 提取组件 */
        int ci = 0;
        while (*p != '\0' && *p != '/' && ci < 255)
            component[ci++] = *p++;
        component[ci] = '\0';
        if (*p == '/') p++;

        if (ci == 0) continue;

        uint64_t child_mft;
        ret = find_in_directory(fs, current_mft, component, &child_mft);
        if (ret != FS_OK)
            return ret;

        current_mft = child_mft;
    }

    /* 打开找到的文件 */
    return ntfs_open_by_ref(fs, current_mft, out);
}

/* ---------------------------------------------------------------
 * ntfs_read_file - 读取文件数据
 * --------------------------------------------------------------- */
int ntfs_read_file(ntfs_fs_t *fs, ntfs_file_t *file,
                   void *buffer, uint64_t size,
                   uint64_t *bytes_read)
{
    uint8_t *buf = (uint8_t *)buffer;
    uint64_t total = 0;
    uint64_t to_read;
    int ret;

    if (file->offset >= file->size) {
        *bytes_read = 0;
        return FS_OK;
    }

    if (file->offset + size > file->size)
        to_read = file->size - file->offset;
    else
        to_read = size;

    if (file->data_resident) {
        /* 常驻数据: 直接从 MFT 记录中复制 */
        uint8_t record_buf[4096];

        ret = read_mft_record(fs, file->mft_ref, record_buf);
        if (ret != FS_OK)
            return ret;

        const uint8_t *attr_data = find_attribute(record_buf,
                                                   ATTR_TYPE_DATA, 0);
        if (attr_data == NULL)
            return FS_ERR_NOT_FOUND;

        const ntfs_attr_resident_t *res =
            (const ntfs_attr_resident_t *)attr_data;
        uint32_t avail = res->value_length - file->offset;

        if (to_read > avail)
            to_read = avail;

        for (uint64_t i = 0; i < to_read; i++)
            buf[i] = record_buf[res->value_offset + file->offset + i];

        total = to_read;
    } else {
        /* 非常驻数据: 解析数据运行列表 */
        uint8_t record_buf[4096];

        ret = read_mft_record(fs, file->mft_ref, record_buf);
        if (ret != FS_OK)
            return ret;

        const ntfs_attr_nonresident_t *nr =
            (const ntfs_attr_nonresident_t *)
            find_attribute(record_buf, ATTR_TYPE_DATA, 0);
        if (nr == NULL)
            return FS_ERR_NOT_FOUND;

        const uint8_t *runlist = (const uint8_t *)nr + nr->runlist_offset;
        uint32_t runlist_max = nr->header.length - nr->runlist_offset;
        uint64_t vcn = file->offset / fs->bytes_per_cluster;
        uint64_t offset_in_cluster = file->offset % fs->bytes_per_cluster;

        while (total < to_read) {
            uint64_t lba, run_length;
            uint64_t cluster_size = fs->bytes_per_cluster;
            uint32_t to_copy;

            ret = ntfs_resolve_vcn(fs, runlist, runlist_max,
                                    vcn, &lba, &run_length);
            if (ret != FS_OK)
                break;

            if (lba == 0) {
                /* 稀疏区域: 填充零 */
                to_copy = (uint32_t)(cluster_size - offset_in_cluster);
                if (to_copy > to_read - total)
                    to_copy = (uint32_t)(to_read - total);
                for (uint32_t i = 0; i < to_copy; i++)
                    buf[total + i] = 0;
                total += to_copy;
            } else {
                /* 有数据的簇 */
                uint8_t sector_buf[512];

                to_copy = (uint32_t)(cluster_size - offset_in_cluster);
                if (to_copy > to_read - total)
                    to_copy = (uint32_t)(to_read - total);

                uint32_t sector_off = offset_in_cluster / fs->bytes_per_sector;
                uint32_t byte_off = offset_in_cluster % fs->bytes_per_sector;
                uint32_t copied_in_cluster = 0;
                uint32_t sector_count = (byte_off + to_copy + fs->bytes_per_sector - 1) / fs->bytes_per_sector;

                for (uint32_t s = 0; s < sector_count && total < to_read; s++) {
                    if (sector_read(fs->dev, lba + sector_off + s, 1, sector_buf) != 0)
                        break;

                    uint32_t chunk = fs->bytes_per_sector - byte_off;
                    if (chunk > to_read - total)
                        chunk = (uint32_t)(to_read - total);

                    for (uint32_t k = 0; k < chunk; k++)
                        buf[total + k] = sector_buf[byte_off + k];

                    total += chunk;
                    byte_off = 0;
                }
            }

            file->offset += (to_read - total == 0) ? 0 : 0;
            /* 没有完全跟踪 offset, 简化处理 */
            vcn++;
            offset_in_cluster = 0;
        }
    }

    file->offset += total;
    *bytes_read = total;
    return FS_OK;
}

/* ---------------------------------------------------------------
 * ntfs_close_file - 关闭文件
 * --------------------------------------------------------------- */
void ntfs_close_file(ntfs_file_t *file)
{
    memset(file, 0, sizeof(*file));
}

/* ---------------------------------------------------------------
 * ntfs_seek_file - 设置文件偏移
 * --------------------------------------------------------------- */
int ntfs_seek_file(ntfs_file_t *file, uint64_t offset)
{
    if (offset > file->size)
        offset = file->size;
    file->offset = offset;
    return FS_OK;
}

/* ---------------------------------------------------------------
 * ntfs_read_dir - 读取目录内容
 * --------------------------------------------------------------- */
int ntfs_read_dir(ntfs_fs_t *fs, const char *path,
                  fs_dirent_t *entries, int max)
{
    ntfs_file_t dir_file;
    int entry_count = 0;
    uint8_t record_buf[4096];
    const uint8_t *idx_root;
    int ret;

    /* 打开目录 */
    ret = ntfs_open_file(fs, path, &dir_file);
    if (ret != FS_OK)
        return ret;

    if (!dir_file.is_directory) {
        ntfs_close_file(&dir_file);
        return FS_ERR_NOT_DIR;
    }

    /* 读取目录的 MFT 记录 */
    ret = read_mft_record(fs, dir_file.mft_ref, record_buf);
    if (ret != FS_OK) {
        ntfs_close_file(&dir_file);
        return ret;
    }

    /* 查找 $INDEX_ROOT */
    idx_root = find_attribute(record_buf, ATTR_TYPE_INDEX_ROOT, 0);
    if (idx_root == NULL) {
        ntfs_close_file(&dir_file);
        return FS_ERR_NOT_FOUND;
    }

    const ntfs_attr_resident_t *res =
        (const ntfs_attr_resident_t *)idx_root;
    const uint8_t *data = (const uint8_t *)idx_root + res->value_offset;
    uint32_t data_len = res->value_length;

    /* 跳过索引根头 */
    uint32_t offset = sizeof(ntfs_index_root_header_t);

    /* 遍历索引项 */
    while (offset + sizeof(ntfs_index_entry_header_t) <= data_len
           && entry_count < max) {
        const ntfs_index_entry_header_t *ie =
            (const ntfs_index_entry_header_t *)(data + offset);

        if (ie->index_entry_length == 0)
            break;

        const ntfs_filename_attr_t *fn =
            (const ntfs_filename_attr_t *)
            (data + offset + sizeof(ntfs_index_entry_header_t));

        int name_len = fn->name_length;
        int i;

        for (i = 0; i < name_len && i < FS_MAX_NAME - 1; i++) {
            uint16_t ch = *(const uint16_t *)
                ((const uint8_t *)fn + sizeof(ntfs_filename_attr_t) + i * 2);
            entries[entry_count].name[i] = (ch <= 0x7F) ? (char)ch : '?';
        }
        entries[entry_count].name[i] = '\0';

        entries[entry_count].size = fn->real_size;
        entries[entry_count].attrs = fn->file_attributes;
        entries[entry_count].is_directory =
            (fn->file_attributes & 0x10) ? 1 : 0;

        entry_count++;

        if (ie->flags & 0x02)
            break;  /* 最后一项 */

        offset += ie->index_entry_length;
    }

    ntfs_close_file(&dir_file);
    return entry_count;
}
