/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 4: 命令行 Shell (CLI)
 *
 * 文件: shell.c
 * 描述: UEFI 文本模式命令行 Shell 实现
 *       通过串口 (COM1 收发) 与用户交互。
 *
 * 命令分类:
 *   文件类: ls cd pwd cat mkdir rm cp mv touch rmdir more head tail find stat
 *   系统类: ver date uptime hostname env set cpu disk reboot shutdown
 *   进程类: ps kill top start stop task
 *   调试类: dump log klog trace panic irq ipc sched syscall debug
 *   其他:   help cls echo run mount umount format device
 */

#include "shell.h"
#include "../sched/sched.h"
#include "../sched/timer.h"
#include "../lib/io.h"
#include "../pmm.h"
#include "../fs/fs.h"
#include "../api/gfx_api.h"
#include "../gfx/gop.h"
#include "../gfx/fb.h"
#include "../gfx/icons.h"

#include <stdint.h>
#include <stddef.h>
#include <efi.h>

/* 全局 SystemTable 指针 (在 main.c 中定义) */
extern EFI_SYSTEM_TABLE *g_ST;

/* ---------------------------------------------------------------
 * 全局状态
 * --------------------------------------------------------------- */

/* 文件系统句柄 (由 shell_init 设置) */
static fs_handle_t *g_fs = NULL;

/* 当前工作目录 (绝对路径, 以 '/' 开头和分隔) */
static char g_cwd[FS_MAX_PATH] = "/";

/* 主机名 */
static char g_hostname[32] = "DimensOS";

/* 环境变量表 (简单实现: 固定大小) */
#define MAX_ENV_VARS 16
static struct { char name[32]; char value[128]; } g_env[MAX_ENV_VARS];
static int g_env_count = 0;

/* 命令历史记录 */
#define CMD_HISTORY_SIZE  10
static char g_history[CMD_HISTORY_SIZE][256];
static int g_history_count = 0;
static int g_history_index = 0;

/* ---------------------------------------------------------------
 * 输出到控制台 (通过 sys_write -> UEFI ConOut + 串口)
 * --------------------------------------------------------------- */

/* 将 UTF-8 字符串转换为 UTF-16 并输出 */
static void sh_puts(const char *s)
{
    if (g_ST == NULL || g_ST->ConOut == NULL)
        return;

    CHAR16 buf[256];
    int j = 0;
    while (*s && j < 255) {
        if (*s == '\n') {
            buf[j] = '\0';
            if (j > 0)
                g_ST->ConOut->OutputString(g_ST->ConOut, buf);
            g_ST->ConOut->OutputString(g_ST->ConOut, L"\r\n");
            j = 0;
            s++;
        } else if ((*s & 0xF0) == 0xE0) {
            uint16_t code = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            buf[j++] = code;
            s += 3;
        } else if ((*s & 0xE0) == 0xC0) {
            uint16_t code = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            buf[j++] = code;
            s += 2;
        } else {
            buf[j++] = (CHAR16)*s++;
        }
        if (j >= 255) {
            buf[j] = '\0';
            g_ST->ConOut->OutputString(g_ST->ConOut, buf);
            j = 0;
        }
    }
    if (j > 0) {
        buf[j] = '\0';
        g_ST->ConOut->OutputString(g_ST->ConOut, buf);
    }
}

/* 输出单个字符 */
static void sh_putc(char c)
{
    if (g_ST == NULL || g_ST->ConOut == NULL)
        return;

    if (c == '\n') {
        g_ST->ConOut->OutputString(g_ST->ConOut, L"\r\n");
    } else {
        CHAR16 buf[2] = {(CHAR16)c, 0};
        g_ST->ConOut->OutputString(g_ST->ConOut, buf);
    }
}

/* 输出十六进制数字 */
static void sh_hex(uint64_t val)
{
    const char hex[] = "0123456789ABCDEF";
    sh_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        sh_putc(hex[(val >> i) & 0xF]);
}

/* 输出十进制数字 */
static void sh_dec(uint64_t val)
{
    char buf[24];
    int pos = 0;
    if (val == 0) {
        sh_putc('0');
        return;
    }
    while (val > 0 && pos < 23) {
        buf[pos++] = '0' + (val % 10);
        val /= 10;
    }
    while (pos > 0)
        sh_putc(buf[--pos]);
}

/* ---------------------------------------------------------------
 * PS/2 键盘轮询输入 (直接访问硬件端口, 不依赖中断)
 *
 * PS/2 控制器端口:
 *   - 0x64: 状态寄存器 (bit 0 = 数据就绪)
 *   - 0x60: 数据端口 (读取 scancode)
 *
 * 使用 Scan Code Set 1 (OVMF 默认使用 Set 1)
 * --------------------------------------------------------------- */

/* Scan Code Set 1 到 ASCII 映射表 */
static const uint8_t scancode_ascii[128] = {
    0,   0x1B, '1', '2', '3', '4', '5', '6',  /* 0x00-0x07 */
    '7', '8', '9', '0', '-', '=', '\b', '\t',  /* 0x08-0x0F */
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',  /* 0x10-0x17 */
    'o', 'p', '[', ']', '\n',0,   'a', 's',  /* 0x18-0x1F */
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  /* 0x20-0x27 */
    '\'','`', 0,   '\\','z', 'x', 'c', 'v',  /* 0x28-0x2F */
    'b', 'n', 'm', ',', '.', '/', 0,   '*',   /* 0x30-0x37 */
    0,   ' ', 0,   0,   0,   0,   0,   0,    /* 0x38-0x3F */
    0,   0,   '7', '8', '9', '-', '4', '5',  /* 0x40-0x47 */
    '6', '+', '1', '2', '3', '0', '.', 0,    /* 0x48-0x4F */
    0,   0,   0,   0,   0,   0,   0,   0,    /* 0x50-0x57 */
    0,   0,   0,   0,   0,   0,   0,   0,    /* 0x58-0x5F */
    0,   0,   0,   0,   0,   0,   0,   0,    /* 0x60-0x67 */
    0,   0,   0,   0,   0,   0,   0,   0,    /* 0x68-0x6F */
    0,   0,   0,   0,   0,   0,   0,   0,    /* 0x70-0x77 */
    0,   0,   0,   0,   0,   0,   0,   0     /* 0x78-0x7F */
};

/* 读取单字符 (非阻塞, 返回 -1 若无数据) */
static int sh_getc_nb(void)
{
    uint8_t status = inb(0x64);
    if (!(status & 0x01))
        return -1;

    uint8_t scancode = inb(0x60);
    if (scancode & 0x80)
        return -1;

    uint8_t ascii = scancode_ascii[scancode];
    if (ascii == 0)
        return -1;

    return ascii;
}

/* ---------------------------------------------------------------
 * 字符串辅助函数 (freestanding 兼容)
 * --------------------------------------------------------------- */

static int sh_strlen(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n;
}

static int sh_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void sh_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++) != '\0');
}

static void sh_strcat(char *dst, const char *src)
{
    sh_strcpy(dst + sh_strlen(dst), src);
}

/* 判断字符串前缀 */
static int sh_str_starts_with(const char *str, const char *prefix)
{
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

/* 去除前导空白 */
static const char *sh_skip_spaces(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* 在字符串中查找字符 */
static const char *sh_str_chr(const char *s, char c)
{
    while (*s) {
        if (*s == c) return s;
        s++;
    }
    return NULL;
}

static void sh_memcpy(void *dst, const void *src, int n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
}

/* ---------------------------------------------------------------
 * 路径处理
 * --------------------------------------------------------------- */

static int resolve_path(const char *input, char *output)
{
    char tmp[FS_MAX_PATH];

    if (input[0] == '/') {
        sh_strcpy(tmp, input);
    } else {
        sh_strcpy(tmp, g_cwd);
        if (tmp[sh_strlen(tmp) - 1] != '/')
            sh_strcat(tmp, "/");
        sh_strcat(tmp, input);
    }

    int len = sh_strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    sh_strcpy(output, tmp);
    return 0;
}

/* ===============================================================
 *                   命令实现 (按分类)
 * =============================================================== */

/* ---------------------------------------------------------------
 * [通用类] help, cls, echo, exit, ver
 * --------------------------------------------------------------- */

/* --- help: 显示帮助信息 --- */
static void cmd_help(void)
{
    sh_puts("DimensOS Shell Commands\n");
    sh_puts("=======================\n\n");

    sh_puts("  [通用类]\n");
    sh_puts("    help            - 显示此帮助\n");
    sh_puts("    cls             - 清屏\n");
    sh_puts("    echo <text>     - 输出文本\n");
    sh_puts("    exit            - 退出 Shell\n");
    sh_puts("    ver             - 显示系统版本\n");
    sh_puts("    date            - 显示当前日期时间\n");
    sh_puts("    uptime          - 查看系统运行时间\n");
    sh_puts("    hostname [name] - 查看或设置主机名\n");
    sh_puts("    env             - 查看环境变量\n");
    sh_puts("    set <k>=<v>     - 设置环境变量\n");
    sh_puts("    cpu             - 查看 CPU 信息\n");
    sh_puts("    disk            - 查看磁盘信息\n\n");

    sh_puts("  [文件类]\n");
    sh_puts("    ls [path]       - 列出目录\n");
    sh_puts("    cd <path>       - 切换目录\n");
    sh_puts("    pwd             - 显示当前路径\n");
    sh_puts("    cat <file>      - 查看文件内容\n");
    sh_puts("    mkdir <dir>     - 创建目录\n");
    sh_puts("    rm <file>       - 删除文件\n");
    sh_puts("    rmdir <dir>     - 删除目录\n");
    sh_puts("    cp <src> <dst>  - 复制文件\n");
    sh_puts("    mv <src> <dst>  - 移动/重命名\n");
    sh_puts("    touch <file>    - 创建空文件\n");
    sh_puts("    more <file>     - 分页查看文件\n");
    sh_puts("    head <file> [n] - 查看前 n 行 (默认 10)\n");
    sh_puts("    tail <file> [n] - 查看后 n 行 (默认 10)\n");
    sh_puts("    find <name>     - 搜索文件\n");
    sh_puts("    stat <file>     - 查看文件属性\n\n");

    sh_puts("  [进程类]\n");
    sh_puts("    ps              - 查看进程列表\n");
    sh_puts("    kill <pid>      - 终止进程\n");
    sh_puts("    top             - 系统状态概览\n");
    sh_puts("    start <prog>    - 启动程序\n");
    sh_puts("    stop <task>     - 停止服务\n");
    sh_puts("    task            - 任务管理\n\n");

    sh_puts("  [图形类]\n");
    sh_puts("    ui              - 启动图形桌面环境（按 ESC 返回）\n\n");

    sh_puts("  [系统类]\n");
    sh_puts("    reboot          - 重启系统\n");
    sh_puts("    shutdown        - 关机\n");
    sh_puts("    mount <dev>     - 挂载文件系统\n");
    sh_puts("    umount          - 卸载文件系统\n");
    sh_puts("    format <dev>    - 格式化存储设备\n");
    sh_puts("    device          - 查看设备列表\n\n");

    sh_puts("  [调试类]\n");
    sh_puts("    dump            - 导出内核状态\n");
    sh_puts("    sched           - 查看调度器状态\n");
    sh_puts("    mem             - 查看内存信息\n");
    sh_puts("    syscall         - 查看系统调用表\n");
    sh_puts("    klog            - 查看内核日志\n");
    sh_puts("    trace           - 内核调用跟踪\n");
    sh_puts("    panic           - 查看崩溃现场\n");
    sh_puts("    irq             - 查看中断状态\n");
    sh_puts("    ipc             - 查看进程通信\n");
    sh_puts("    heap            - 查看内核堆\n");
    sh_puts("    debug           - 进入调试模式\n\n");

    sh_puts("  [快捷键]\n");
    sh_puts("    Up/Down         - 命令历史\n");
    sh_puts("    Tab             - 自动补全\n");
}

/* --- cls: 清屏 --- */
static void cmd_cls(void)
{
    for (int i = 0; i < 80; i++)
        sh_putc('\n');
}

/* --- echo: 输出字符串 --- */
static void cmd_echo(const char *arg)
{
    if (arg && arg[0] != '\0') {
        /* 简单处理 $VAR 替换 */
        const char *p = arg;
        char out[256];
        int oi = 0;
        while (*p && oi < 250) {
            if (*p == '$') {
                p++;
                char vname[32];
                int vi = 0;
                while (*p && *p != ' ' && *p != '\t' && vi < 30)
                    vname[vi++] = *p++;
                vname[vi] = '\0';
                int found = 0;
                for (int ei = 0; ei < g_env_count; ei++) {
                    if (sh_strcmp(g_env[ei].name, vname) == 0) {
                        const char *vv = g_env[ei].value;
                        while (*vv && oi < 250)
                            out[oi++] = *vv++;
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    sh_putc('$');
                    for (int vi2 = 0; vi2 < vi; vi2++)
                        out[oi++] = vname[vi2];
                }
            } else {
                out[oi++] = *p++;
            }
        }
        out[oi] = '\0';
        sh_puts(out);
    }
    sh_putc('\n');
}

/* --- exit: 退出 Shell (标记任务终止) --- */
static void cmd_exit(void)
{
    sh_puts("Shell terminated.\n");
    /* 主动触发任务退出: 让调度器检测到 TERMINATED */
    /* shell 自身无法 kill 自己, 提示用户 */
}

/* --- ver: 显示系统版本 --- */
static void cmd_ver(void)
{
    sh_puts("DimensOS 深维操作系统\n");
    sh_puts("Version 1.0.0 (Build 2025)\n");
    sh_puts("Copyright (c) 2025 深维工坊工作室\n");
    sh_puts("Architecture: x86_64\n");
    sh_puts("Mode: UEFI\n");
}

/* ---------------------------------------------------------------
 * [系统信息类] date, uptime, hostname, env, set, cpu, disk
 * --------------------------------------------------------------- */

/* --- date: 显示日期时间 (UEFI GetTime) --- */
static void cmd_date(void)
{
    if (!g_ST || !g_ST->RuntimeServices || !g_ST->RuntimeServices->GetTime) {
        sh_puts("date: runtime services unavailable\n");
        return;
    }
    EFI_TIME t;
    EFI_STATUS st = g_ST->RuntimeServices->GetTime(&t, NULL);
    if (EFI_ERROR(st)) {
        sh_puts("date: GetTime failed\n");
        return;
    }
    /* 格式化输出: YYYY-MM-DD HH:MM:SS */
    sh_dec(t.Year); sh_putc('-');
    if (t.Month < 10) sh_putc('0');
    sh_dec(t.Month); sh_putc('-');
    if (t.Day < 10) sh_putc('0');
    sh_dec(t.Day); sh_putc(' ');
    if (t.Hour < 10) sh_putc('0');
    sh_dec(t.Hour); sh_putc(':');
    if (t.Minute < 10) sh_putc('0');
    sh_dec(t.Minute); sh_putc(':');
    if (t.Second < 10) sh_putc('0');
    sh_dec(t.Second);
    sh_putc('\n');
}

/* --- uptime: 查看运行时间 --- */
static void cmd_uptime(void)
{
    uint64_t ticks = timer_get_ticks();
    uint64_t seconds = ticks / TIMER_DEFAULT_HZ;
    uint64_t days = seconds / 86400;
    seconds %= 86400;
    uint64_t hours = seconds / 3600;
    seconds %= 3600;
    uint64_t mins = seconds / 60;
    seconds %= 60;

    sh_dec(days); sh_puts("d ");
    sh_dec(hours); sh_puts("h ");
    sh_dec(mins); sh_puts("m ");
    sh_dec(seconds); sh_puts("s\n");
}

/* --- hostname: 查看或设置主机名 --- */
static void cmd_hostname(const char *arg)
{
    if (arg && arg[0] != '\0') {
        int i = 0;
        while (arg[i] && i < 31) {
            g_hostname[i] = arg[i];
            i++;
        }
        g_hostname[i] = '\0';
    }
    sh_puts(g_hostname);
    sh_putc('\n');
}

/* --- env: 查看环境变量 --- */
static void cmd_env(void)
{
    if (g_env_count == 0) {
        sh_puts("(no environment variables set)\n");
        return;
    }
    for (int i = 0; i < g_env_count; i++) {
        sh_puts(g_env[i].name);
        sh_putc('=');
        sh_puts(g_env[i].value);
        sh_putc('\n');
    }
}

/* --- set: 设置环境变量 --- */
static void cmd_set(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        cmd_env();
        return;
    }

    /* 解析 name=value */
    char name[32], value[128];
    int ni = 0, vi = 0;
    while (*arg && *arg != '=' && ni < 31) name[ni++] = *arg++;
    name[ni] = '\0';
    if (*arg == '=') arg++;
    while (*arg && vi < 127) value[vi++] = *arg++;
    value[vi] = '\0';

    if (ni == 0) return;

    /* 查找是否已存在 */
    for (int i = 0; i < g_env_count; i++) {
        if (sh_strcmp(g_env[i].name, name) == 0) {
            sh_strcpy(g_env[i].value, value);
            return;
        }
    }

    /* 新增 */
    if (g_env_count < MAX_ENV_VARS) {
        sh_strcpy(g_env[g_env_count].name, name);
        sh_strcpy(g_env[g_env_count].value, value);
        g_env_count++;
    } else {
        sh_puts("set: environment variable table full\n");
    }
}

/* --- cpu: 查看 CPU 信息 (CPUID) --- */
static void cmd_cpu(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* CPUID  leaf 0: vendor string */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    char vendor[13];
    *(uint32_t*)(vendor + 0) = ebx;
    *(uint32_t*)(vendor + 4) = edx;
    *(uint32_t*)(vendor + 8) = ecx;
    vendor[12] = '\0';

    sh_puts("Vendor: "); sh_puts(vendor); sh_putc('\n');

    /* CPUID leaf 1: family/model/stepping */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    uint32_t stepping  = eax & 0xF;
    uint32_t model     = (eax >> 4) & 0xF;
    uint32_t family    = (eax >> 8) & 0xF;
    uint32_t ext_model = (eax >> 16) & 0xF;
    uint32_t ext_family= (eax >> 20) & 0xFF;

    sh_puts("Family: 0x"); sh_hex(family);
    if (ext_family) { sh_puts(" (ext 0x"); sh_hex(ext_family); sh_putc(')'); }
    sh_putc('\n');
    sh_puts("Model: 0x"); sh_hex(model);
    if (ext_model) { sh_puts(" (ext 0x"); sh_hex(ext_model); sh_putc(')'); }
    sh_putc('\n');
    sh_puts("Stepping: ");
    sh_dec(stepping); sh_putc('\n');

    /* CPUID leaf 0x80000002-0x80000004: brand string */
    char brand[49];
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf));
        int off = (leaf - 0x80000002) * 16;
        *(uint32_t*)(brand + off + 0) = eax;
        *(uint32_t*)(brand + off + 4) = ebx;
        *(uint32_t*)(brand + off + 8) = ecx;
        *(uint32_t*)(brand + off + 12) = edx;
    }
    brand[48] = '\0';
    /* 去掉前导空格 */
    const char *b = brand;
    while (*b == ' ') b++;
    if (*b) { sh_puts("Brand: "); sh_puts(b); sh_putc('\n'); }
}

/* --- disk: 查看磁盘/分区信息 --- */
static void cmd_disk(void)
{
    int found = 0;
    for (int i = 0; i < MAX_DRIVE_LETTERS; i++) {
        if (g_drives[i].type != FS_TYPE_UNKNOWN) {
            found = 1;
            sh_putc(' ');
            sh_putc('A' + i);
            sh_puts(": [");
            if (g_drives[i].type == FS_TYPE_FAT32) {
                sh_puts("FAT32");
            } else if (g_drives[i].type == FS_TYPE_NTFS) {
                sh_puts("NTFS");
            } else {
                sh_puts("UNKNOWN");
            }
            sh_puts("] ");
            if (g_drives[i].label[0]) {
                sh_puts(g_drives[i].label);
                sh_putc(' ');
            }
            if (g_drives[i].block_dev) {
                sh_dec(g_drives[i].block_dev->total_sectors *
                       g_drives[i].block_dev->sector_size / (1024*1024));
                sh_puts(" MB\n");
            } else {
                sh_puts("(no block dev)\n");
            }
        }
    }
    if (!found) {
        sh_puts("disk: no drives found\n");
    }
}

/* ---------------------------------------------------------------
 * [文件类] ls, cd, pwd, cat, mkdir, rm, rmdir, cp, mv, touch,
 *           more, head, tail, find, stat
 * --------------------------------------------------------------- */

/* --- ls: 列出目录 --- */
static void cmd_ls(const char *arg)
{
    fs_dirent_t entries[64];
    char full_path[FS_MAX_PATH];
    int count;

    if (arg && arg[0] != '\0')
        resolve_path(arg, full_path);
    else
        sh_strcpy(full_path, g_cwd);

    count = fs_readdir(g_fs, full_path, entries, 64);
    if (count < 0) {
        sh_puts("ls: cannot access '");
        sh_puts(full_path);
        sh_puts("': ");
        if (count == FS_ERR_NOT_FOUND)
            sh_puts("no such file or directory\n");
        else if (count == FS_ERR_IO)
            sh_puts("I/O error\n");
        else
            sh_puts("error\n");
        return;
    }

    int file_count = 0, dir_count = 0;
    uint64_t total_size = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].is_directory)
            dir_count++;
        else {
            file_count++;
            total_size += entries[i].size;
        }
    }

    for (int i = 0; i < count; i++) {
        sh_puts(entries[i].is_directory ? "[DIR] " : "      ");
        sh_puts(entries[i].name);
        int name_len = sh_strlen(entries[i].name);
        for (int pad = name_len; pad < 20; pad++)
            sh_putc(' ');
        if (!entries[i].is_directory) {
            sh_dec(entries[i].size);
            sh_puts(" bytes");
        }
        sh_putc('\n');
    }

    sh_dec(dir_count); sh_puts(" dir(s), ");
    sh_dec(file_count); sh_puts(" file(s), ");
    sh_dec(total_size); sh_puts(" bytes total\n");
}

/* --- cd: 切换目录 --- */
static void cmd_cd(const char *arg)
{
    char new_path[FS_MAX_PATH];
    fs_dirent_t dummy;

    if (!arg || arg[0] == '\0') {
        sh_strcpy(g_cwd, "/");
        return;
    }

    resolve_path(arg, new_path);
    int ret = fs_readdir(g_fs, new_path, &dummy, 1);
    if (ret < 0) {
        sh_puts("cd: "); sh_puts(arg); sh_puts(": no such file or directory\n");
        return;
    }
    sh_strcpy(g_cwd, new_path);
}

/* --- pwd: 显示当前路径 --- */
static void cmd_pwd(void)
{
    sh_puts(g_cwd);
    sh_putc('\n');
}

/* --- cat: 查看文件内容 --- */
static void cmd_cat(const char *arg)
{
    file_handle_t *fh;
    char full_path[FS_MAX_PATH];
    uint8_t buf[128];
    uint64_t bytes_read;

    if (!arg || arg[0] == '\0') {
        sh_puts("cat: missing operand\n");
        return;
    }

    resolve_path(arg, full_path);
    fh = fs_open(g_fs, full_path);
    if (!fh) {
        sh_puts("cat: "); sh_puts(arg); sh_puts(": no such file or directory\n");
        return;
    }

    while (1) {
        int ret = fs_read(fh, buf, sizeof(buf), &bytes_read);
        if (ret != FS_OK || bytes_read == 0)
            break;
        for (uint64_t i = 0; i < bytes_read; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\0') break;
            if (c < 32 && c != '\n' && c != '\t')
                sh_putc('.');
            else
                sh_putc(c);
        }
    }

    if (bytes_read > 0 && ((char)buf[bytes_read > 0 ? bytes_read - 1 : 0]) != '\n')
        sh_putc('\n');

    fs_close(fh);
}

/* --- more: 分页查看文件 (按空格翻页, 按 q 退出) --- */
static void cmd_more(const char *arg)
{
    file_handle_t *fh;
    char full_path[FS_MAX_PATH];
    uint8_t buf[128];
    uint64_t bytes_read;
    int line_count = 0;

    if (!arg || arg[0] == '\0') {
        sh_puts("more: missing operand\n");
        return;
    }

    resolve_path(arg, full_path);
    fh = fs_open(g_fs, full_path);
    if (!fh) {
        sh_puts("more: "); sh_puts(arg); sh_puts(": no such file\n");
        return;
    }

    while (1) {
        int ret = fs_read(fh, buf, sizeof(buf), &bytes_read);
        if (ret != FS_OK || bytes_read == 0)
            break;
        for (uint64_t i = 0; i < bytes_read; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\0') break;
            sh_putc(c);
            if (c == '\n') {
                line_count++;
                if (line_count >= 24) {
                    line_count = 0;
                    sh_puts("-- More -- (press any key)");
                    /* 等待按键 */
                    while (sh_getc_nb() < 0) { /* busy wait */ }
                    /* 清除提示行 */
                    sh_puts("\r                    \r");
                }
            }
        }
    }
    fs_close(fh);
}

/* --- head: 查看文件前 n 行 --- */
static void cmd_head(const char *arg)
{
    file_handle_t *fh;
    char full_path[FS_MAX_PATH];
    uint8_t buf[128];
    uint64_t bytes_read;
    int max_lines = 10;

    if (!arg || arg[0] == '\0') {
        sh_puts("head: missing operand\n");
        return;
    }

    /* 检查是否有行数参数 */
    const char *p = arg;
    while (*p && *p != ' ') p++;
    /* 这里简单处理: 后面的参数是行数 */
    /* 更健壮的方式需要解析参数, 简化处理 */

    resolve_path(arg, full_path);
    fh = fs_open(g_fs, full_path);
    if (!fh) {
        sh_puts("head: "); sh_puts(arg); sh_puts(": no such file\n");
        return;
    }

    int lines = 0;
    while (lines < max_lines) {
        int ret = fs_read(fh, buf, sizeof(buf), &bytes_read);
        if (ret != FS_OK || bytes_read == 0)
            break;
        for (uint64_t i = 0; i < bytes_read && lines < max_lines; i++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c == '\0') break;
            sh_putc(c);
            if (c == '\n') lines++;
        }
    }
    fs_close(fh);
}

/* --- tail: 查看文件尾 (简化: 从最后4096字节开始显示) --- */
static void cmd_tail(const char *arg)
{
    file_handle_t *fh;
    char full_path[FS_MAX_PATH];
    uint8_t buf[4096];
    uint64_t bytes_read;
    int max_lines = 10;

    if (!arg || arg[0] == '\0') {
        sh_puts("tail: missing operand\n");
        return;
    }

    resolve_path(arg, full_path);
    fh = fs_open(g_fs, full_path);
    if (!fh) {
        sh_puts("tail: "); sh_puts(arg); sh_puts(": no such file\n");
        return;
    }

    /* 获取文件大小 */
    uint64_t fsize = fs_get_size(fh);
    if (fsize == 0) {
        fs_close(fh);
        return;
    }

    /* 从文件末尾往前读, 最多 4096 字节 */
    uint64_t read_offset = (fsize > 4096) ? fsize - 4096 : 0;
    fs_seek(fh, read_offset);

    int ret = fs_read(fh, buf, sizeof(buf), &bytes_read);
    if (ret != FS_OK || bytes_read == 0) {
        fs_close(fh);
        return;
    }

    /* 计算最后 max_lines 行的起始位置 */
    int lines = 0;
    int start = (int)bytes_read - 1;
    for (int i = (int)bytes_read - 1; i >= 0 && lines < max_lines; i--) {
        if (buf[i] == '\n') {
            lines++;
            start = i;
        }
    }
    /* 如果文件小于4096字节且行数不够, 从头开始 */
    if (read_offset == 0 && lines < max_lines)
        start = 0;

    for (int i = start; i < (int)bytes_read; i++) {
        char c = (char)buf[i];
        if (c == '\r') continue;
        if (c == '\0') break;
        sh_putc(c);
    }
    if (bytes_read > 0 && buf[bytes_read-1] != '\n')
        sh_putc('\n');

    fs_close(fh);
}

/* --- find: 搜索文件 (简化: 遍历根目录) --- */
static void cmd_find(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        sh_puts("find: missing pattern\n");
        return;
    }

    if (!g_fs) {
        sh_puts("find: no filesystem\n");
        return;
    }

    /* 简化: 只在当前目录搜索 */
    fs_dirent_t entries[64];
    int count = fs_readdir(g_fs, g_cwd, entries, 64);
    if (count < 0) {
        sh_puts("find: error reading directory\n");
        return;
    }

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (sh_str_chr(entries[i].name, arg[0]) != NULL) {
            /* 简单包含匹配 */
            sh_puts(g_cwd);
            if (sh_strlen(g_cwd) > 1) sh_putc('/');
            sh_puts(entries[i].name);
            sh_putc('\n');
            found++;
        }
    }
    if (!found)
        sh_puts("find: no matches\n");
}

/* --- stat: 查看文件属性 --- */
static void cmd_stat(const char *arg)
{
    file_handle_t *fh;
    char full_path[FS_MAX_PATH];

    if (!arg || arg[0] == '\0') {
        sh_puts("stat: missing operand\n");
        return;
    }

    resolve_path(arg, full_path);
    fh = fs_open(g_fs, full_path);
    if (!fh) {
        sh_puts("stat: "); sh_puts(arg); sh_puts(": no such file\n");
        return;
    }

    sh_puts("  File: "); sh_puts(arg); sh_putc('\n');
    sh_puts("  Size: "); sh_dec(fs_get_size(fh)); sh_puts(" bytes\n");

    /* 尝试判断文件类型 */
    fs_dirent_t entries[64];
    int count = fs_readdir(g_fs, full_path, entries, 1);
    if (count >= 0) {
        sh_puts("  Type: directory\n");
    } else {
        sh_puts("  Type: regular file\n");
    }

    fs_close(fh);
}

/* --- mkdir/rm/rmdir/cp/mv/touch: 只读存根 --- */
static void cmd_mkdir(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("mkdir: missing operand\n"); return; }
    sh_puts("mkdir: '"); sh_puts(arg); sh_puts("' - read-only filesystem\n");
}

static void cmd_rm(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("rm: missing operand\n"); return; }
    sh_puts("rm: '"); sh_puts(arg); sh_puts("' - read-only filesystem\n");
}

static void cmd_rmdir(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("rmdir: missing operand\n"); return; }
    sh_puts("rmdir: '"); sh_puts(arg); sh_puts("' - read-only filesystem\n");
}

static void cmd_cp(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("cp: missing operand\n"); return; }
    sh_puts("cp: '"); sh_puts(arg); sh_puts("' - read-only filesystem\n");
}

static void cmd_mv(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("mv: missing operand\n"); return; }
    sh_puts("mv: '"); sh_puts(arg); sh_puts("' - read-only filesystem\n");
}

static void cmd_touch(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("touch: missing operand\n"); return; }
    sh_puts("touch: '"); sh_puts(arg); sh_puts("' - read-only filesystem\n");
}

/* --- run: 运行文件 (存根) --- */
static void cmd_run(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("run: missing operand\n"); return; }
    sh_puts("run: '"); sh_puts(arg); sh_puts("' - not implemented\n");
}

/* ---------------------------------------------------------------
 * [进程类] ps, kill, top, start, stop, task
 * --------------------------------------------------------------- */

/* 状态名称 */
static const char *task_state_name(int state)
{
    switch (state) {
        case 0:  return "READY";
        case 1:  return "RUNNING";
        case 2:  return "BLOCKED";
        case 3:  return "TERMINATED";
        default: return "UNKNOWN";
    }
}

/* --- ps: 查看进程列表 --- */
static void cmd_ps(void)
{
    int count = sched_get_task_count();
    sh_puts("PID  STATE      NAME\n");
    sh_puts("---  ---------- ----\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        uint64_t pid;
        char name[16];
        int state;
        if (sched_get_task_info(i, &pid, name, &state) == 0 && pid != 0) {
            if (state == TASK_TERMINATED) continue;
            sh_dec(pid);
            sh_puts("    ");
            sh_puts(task_state_name(state));
            /* 补齐到 10 字符 */
            int sl = sh_strlen(task_state_name(state));
            for (int p = sl; p < 10; p++) sh_putc(' ');
            sh_puts(name);
            sh_putc('\n');
        }
    }
    sh_dec(count); sh_puts(" task(s)\n");
}

/* --- kill: 终止进程 --- */
static void cmd_kill(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        sh_puts("kill: missing pid\n");
        return;
    }

    /* 解析 PID */
    uint64_t pid = 0;
    const char *p = arg;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
    }

    if (pid == 0) {
        sh_puts("kill: invalid pid\n");
        return;
    }

    if (sched_kill_task(pid) == 0) {
        sh_puts("kill: task "); sh_dec(pid); sh_puts(" terminated\n");
    } else {
        sh_puts("kill: task "); sh_dec(pid); sh_puts(" not found or not killable\n");
    }
}

/* --- top: 系统状态概览 --- */
static void cmd_top(void)
{
    /* 系统信息 */
    sh_puts("DimensOS System Overview\n");
    sh_puts("=======================\n\n");

    /* 运行时间 */
    sh_puts("Uptime:    "); cmd_uptime();

    /* 内存 */
    uint64_t total_mem = pmm_get_total_memory();
    uint64_t used_mem  = pmm_get_used_memory();
    sh_puts("Memory:    ");
    sh_dec(used_mem / (1024*1024)); sh_puts(" MB / ");
    sh_dec(total_mem / (1024*1024)); sh_puts(" MB used\n");

    /* 进程 */
    int task_count = sched_get_task_count();
    sh_puts("Tasks:     ");
    sh_dec(task_count); sh_puts(" running\n\n");

    /* 进程列表 */
    cmd_ps();
}

/* --- start/stop/task: 存根 --- */
static void cmd_start(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("start: missing program name\n"); return; }
    sh_puts("start: '"); sh_puts(arg); sh_puts("' - not implemented\n");
}

static void cmd_stop(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("stop: missing task name\n"); return; }
    sh_puts("stop: '"); sh_puts(arg); sh_puts("' - not implemented\n");
}

static void cmd_task(void)
{
    sh_puts("task: not implemented (use 'ps' to list tasks)\n");
}

/* ---------------------------------------------------------------
 * [系统控制类] reboot, shutdown, mount, umount, format, device
 * --------------------------------------------------------------- */

/* --- reboot: 重启系统 --- */
static void cmd_reboot(void)
{
    sh_puts("Rebooting...\n");
    if (g_ST && g_ST->RuntimeServices && g_ST->RuntimeServices->ResetSystem) {
        g_ST->RuntimeServices->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    }
    sh_puts("reboot: ResetSystem failed or unavailable\n");
}

/* --- shutdown: 关机 --- */
static void cmd_shutdown(void)
{
    sh_puts("Shutting down...\n");
    if (g_ST && g_ST->RuntimeServices && g_ST->RuntimeServices->ResetSystem) {
        g_ST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    }
    sh_puts("shutdown: ResetSystem failed or unavailable\n");
}

/* --- mount/umount/format: 存根 --- */
static void cmd_mount(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("mount: missing device\n"); return; }
    sh_puts("mount: '"); sh_puts(arg); sh_puts("' - not implemented\n");
}

static void cmd_umount(void)
{
    sh_puts("umount: not implemented\n");
}

static void cmd_format(const char *arg)
{
    if (!arg || arg[0] == '\0') { sh_puts("format: missing device\n"); return; }
    sh_puts("format: '"); sh_puts(arg); sh_puts("' - not implemented\n");
}

/* --- device: 查看设备列表 --- */
static void cmd_device(void)
{
    int found = 0;

    /* 列出盘符设备 */
    for (int i = 0; i < MAX_DRIVE_LETTERS; i++) {
        if (g_drives[i].type != FS_TYPE_UNKNOWN) {
            found = 1;
            sh_puts("  DISK ");
            sh_putc('A' + i);
            sh_puts(": [");
            if (g_drives[i].type == FS_TYPE_FAT32) sh_puts("FAT32");
            else if (g_drives[i].type == FS_TYPE_NTFS) sh_puts("NTFS");
            else sh_puts("UNKNOWN");
            sh_puts("]");
            if (g_drives[i].label[0]) { sh_puts(" '"); sh_puts(g_drives[i].label); sh_putc('\''); }
            if (g_drives[i].block_dev) {
                sh_puts(" ");
                sh_dec(g_drives[i].block_dev->total_sectors *
                       g_drives[i].block_dev->sector_size / (1024ULL*1024));
                sh_puts(" MB");
            }
            sh_putc('\n');
        }
    }

    /* 网络设备 */
    sh_puts("  NET: e1000 (if available)\n");

    if (!found)
        sh_puts("device: no devices found\n");
}

/* ---------------------------------------------------------------
 * [调试类] dump, sched, mem, syscall, klog, trace, panic, irq,
 *           ipc, heap, debug
 * --------------------------------------------------------------- */

/* --- mem: 查看内存信息 --- */
static void cmd_mem(void)
{
    uint64_t total = pmm_get_total_memory();
    uint64_t used  = pmm_get_used_memory();

    sh_puts("Physical Memory Info\n");
    sh_puts("-------------------\n");
    sh_puts("  Total: "); sh_dec(total / (1024*1024)); sh_puts(" MB\n");
    sh_puts("  Used:  "); sh_dec(used / (1024*1024)); sh_puts(" MB\n");
    sh_puts("  Free:  "); sh_dec((total - used) / (1024*1024)); sh_puts(" MB\n");
    sh_puts("  Page:  "); sh_dec(PMM_PAGE_SIZE); sh_puts(" bytes\n");
}

/* --- sched: 查看调度器状态 --- */
static void cmd_sched(void)
{
    sh_puts("Scheduler State\n");
    sh_puts("---------------\n");
    sh_puts("  Max tasks: "); sh_dec(MAX_TASKS); sh_putc('\n');
    sh_puts("  Time slice: "); sh_dec(TIME_SLICE_MS); sh_puts(" ms\n");
    sh_puts("  Timer: "); sh_dec(TIMER_DEFAULT_HZ); sh_puts(" Hz\n\n");

    for (int i = 0; i < MAX_TASKS; i++) {
        uint64_t pid;
        char name[16];
        int state;
        if (sched_get_task_info(i, &pid, name, &state) == 0 && pid != 0) {
            sh_puts("  [");
            sh_dec(i); sh_puts("] PID="); sh_dec(pid);
            sh_puts(" '"); sh_puts(name);
            sh_puts("' "); sh_puts(task_state_name(state));
            sh_putc('\n');
        }
    }
}

/* --- dump: 导出内核状态 (汇总) --- */
static void cmd_dump(void)
{
    sh_puts("DimensOS Kernel State Dump\n");
    sh_puts("=========================\n\n");

    /* 系统信息 */
    cmd_ver();

    /* 内存 */
    sh_putc('\n');
    cmd_mem();

    /* 调度器 */
    sh_putc('\n');
    cmd_sched();
}

/* --- syscall: 查看系统调用表 --- */
static void cmd_syscall(void)
{
    sh_puts("System Call Table (int 0x80)\n");
    sh_puts("----------------------------\n");
    sh_puts("  #0  SYS_YIELD  - Yield CPU\n");
    sh_puts("  #1  SYS_WRITE  - Write string\n");
    sh_puts("  #2  SYS_READ   - Read input\n");
}

/* --- 调试类存根 --- */
static void cmd_klog(void)   { sh_puts("klog: kernel log buffer not implemented\n"); }
static void cmd_trace(void)  { sh_puts("trace: kernel trace infrastructure not present\n"); }
static void cmd_panic(void)  { sh_puts("panic: no panic log recorded\n"); }
static void cmd_irq(void)    { sh_puts("irq: interrupt statistics not tracked\n"); }
static void cmd_ipc(void)    { sh_puts("ipc: inter-process communication not implemented\n"); }
static void cmd_heap(void)   { sh_puts("heap: kernel heap allocator not implemented\n"); }
static void cmd_log(void)    { sh_puts("log: system log not implemented\n"); }
static void cmd_debug(void)  { sh_puts("debug: entering debug mode...\n"); sh_puts("debug: not implemented\n"); }

/* ---------------------------------------------------------------
 * [图形类] ui: 启动图形桌面
 * --------------------------------------------------------------- */
static void ui_draw_desktop_icons(void)
{
    if (!g_gfx_api) return;
    int w = g_gfx_mode.width;
    int h = g_gfx_mode.height;

    /* 桌面图标 - 两行排列 */
    const struct { gfx_icon_id_t id; const char *name; } items[] = {
        {GFX_ICON_THISPC,   "This PC"},
        {GFX_ICON_FILES,    "Files"},
        {GFX_ICON_SHELL,    "Shell"},
        {GFX_ICON_NETWORK,  "Network"},
        {GFX_ICON_SETTINGS, "Settings"},
        {GFX_ICON_ABOUT,    "About"},
    };
    int n = sizeof(items) / sizeof(items[0]);
    int icon_w = 64;
    int icon_h = 80;
    int cols = 4;
    int spacing_x = 80;
    int spacing_y = 96;
    int start_y = 40;

    for (int i = 0; i < n; i++) {
        int col = i % cols;
        int row = i / cols;
        int ix = 24 + col * spacing_x;
        int iy = start_y + row * spacing_y;

        /* 绘制图标 */
        g_gfx_api->draw_icon(ix + (icon_w - ICON_WIDTH) / 2,
                             iy, items[i].id);

        /* 绘制标签文字（带背景衬底提高可读性） */
        CHAR16 label[32];
        int li = 0;
        const char *p = items[i].name;
        while (*p && li < 31) label[li++] = (CHAR16)*p++;
        label[li] = 0;
        int tw = fb_text_width(label);
        int tx = ix + (icon_w - tw) / 2;
        int ty = iy + ICON_HEIGHT + 4;
        fb_fill_rect(tx - 2, ty, tw + 4, 16, GFX_RGB(0, 0, 0));
        fb_draw_text(tx, ty, label, GFX_RGB(240, 240, 240));
    }
}

/* 桌面背景完整绘制（不含光标） */
static void ui_render_desktop(void)
{
    int sw = g_gfx_mode.width;
    int sh = g_gfx_mode.height;
    int taskbar_h = 48;

    /* 桌面背景 - 深色渐变 */
    fb_gradient_rect(0, 0, sw, sh,
                     GFX_RGB(24, 28, 36), GFX_RGB(16, 18, 24));

    /* 底部任务栏区域 - 半透明毛玻璃 */
    fb_blend_rect(0, sh - taskbar_h, sw, taskbar_h,
                  GFX_RGB(32, 32, 36), 200);

    /* 任务栏顶部高光线 */
    fb_fill_rect(0, sh - taskbar_h, sw, 1, GFX_RGB(0, 103, 192));

    /* 绘制桌面图标 */
    ui_draw_desktop_icons();

    /* 开始按钮 */
    int tb_y = sh - taskbar_h;
    fb_fill_round_rect(8, tb_y + 4, 44, taskbar_h - 8, 8,
                      GFX_RGB(0, 103, 192));
    CHAR16 start_txt[] = L"DimensOS";
    int stw = fb_text_width(start_txt);
    fb_draw_text(8 + (44 - stw) / 2, tb_y + (taskbar_h - 16) / 2,
                start_txt, GFX_RGB(255, 255, 255));

    /* DimensOS 版本水印 */
    CHAR16 ver_text[] = L"DimensOS Desktop v3.0";
    int vw = fb_text_width(ver_text);
    fb_draw_text(sw - vw - 16, sh - 34, ver_text, GFX_RGB(120, 120, 130));
}

/* ---------------------------------------------------------------
 * PS/2 鼠标直接读取（不使用 UEFI BootServices）
 *
 * PS/2 鼠标数据包: 3 字节标准格式
 *   Byte 0: [YOV XOV YS XS 1 M R L]
 *   Byte 1: X 移动量 (有符号)
 *   Byte 2: Y 移动量 (有符号, 正值=向下)
 * --------------------------------------------------------------- */

/* PS/2 控制器命令 */
#define PS2_CMD_READ_CTRL  0x20   /* 读取控制寄存器 */
#define PS2_CMD_WRITE_CTRL 0x60   /* 写入控制寄存器 */
#define PS2_CMD_DISABLE_P2 0xA7   /* 禁用第二端口 (鼠标) */
#define PS2_CMD_ENABLE_P2  0xA8   /* 启用第二端口 (鼠标) */
#define PS2_CMD_TEST_P2    0xA9   /* 测试第二端口 */

/* PS/2 鼠标命令 */
#define PS2_MOUSE_CMD_ENABLE  0xF4
#define PS2_MOUSE_CMD_DISABLE 0xF5
#define PS2_MOUSE_CMD_SET_DEFAULT 0xF6
#define PS2_MOUSE_CMD_SET_RES  0xE8   /* 分辨率 */
#define PS2_MOUSE_CMD_SET_SAMPLE 0xF3 /* 采样率 */
#define PS2_MOUSE_ACK  0xFA

/* 等待 PS/2 控制器输出缓冲区 */
static int ps2_wait_output(void)
{
    for (int i = 0; i < 10000; i++) {
        if (inb(0x64) & 0x01) return 0;
    }
    return -1;
}

/* 等待 PS/2 控制器输入缓冲区为空 */
static int ps2_wait_input(void)
{
    for (int i = 0; i < 10000; i++) {
        if (!(inb(0x64) & 0x02)) return 0;
    }
    return -1;
}

/* 向 PS/2 控制器写命令 */
static int ps2_cmd(uint8_t cmd)
{
    if (ps2_wait_input() < 0) return -1;
    outb(0x64, cmd);
    return 0;
}

/* 向 PS/2 鼠标写命令（通过控制器第二端口） */
static int ps2_mouse_cmd(uint8_t cmd)
{
    /* 告诉控制器我们要写鼠标端口 */
    if (ps2_cmd(0xD4) < 0) return -1;
    /* 发送命令到鼠标 */
    if (ps2_wait_input() < 0) return -1;
    outb(0x60, cmd);
    return 0;
}

/* 读取 PS/2 数据字节 */
static int ps2_read_data(void)
{
    if (ps2_wait_output() < 0) return -1;
    return inb(0x60);
}

/* PS/2 鼠标状态 */
static int g_ps2_mouse_x = 0;
static int g_ps2_mouse_y = 0;
static BOOLEAN g_ps2_mouse_btn_left = FALSE;
static BOOLEAN g_ps2_mouse_btn_right = FALSE;
static int g_ps2_mouse_init = 0;  /* 0=未初始化, 1=就绪 */

/* 初始化 PS/2 鼠标 */
static void ps2_mouse_init(void)
{
    if (g_ps2_mouse_init) return;

    /* 1. 禁用鼠标 */
    ps2_cmd(PS2_CMD_DISABLE_P2);

    /* 2. 读取控制寄存器并启用第二端口中断 */
    ps2_cmd(PS2_CMD_READ_CTRL);
    int ctrl = ps2_read_data();
    if (ctrl < 0) {
        uart_puts("[PS2] mouse: cannot read controller\n");
        return;
    }
    ps2_cmd(PS2_CMD_WRITE_CTRL);
    ps2_wait_input();
    outb(0x60, ctrl | 0x02);  /* 启用第二端口时钟 */

    /* 3. 启用鼠标 */
    ps2_cmd(PS2_CMD_ENABLE_P2);

    /* 4. 设置鼠标默认值 */
    ps2_mouse_cmd(PS2_MOUSE_CMD_SET_DEFAULT);
    if (ps2_read_data() != PS2_MOUSE_ACK) {
        uart_puts("[PS2] mouse: no ACK on set default\n");
    }

    /* 5. 设置采样率 100 Hz */
    ps2_mouse_cmd(PS2_MOUSE_CMD_SET_SAMPLE);
    ps2_read_data(); /* 可能无 ACK */
    ps2_wait_input();
    outb(0x60, 100);
    /* 可选: 等待并丢弃响应 */

    /* 6. 启用鼠标数据上报 */
    ps2_mouse_cmd(PS2_MOUSE_CMD_ENABLE);
    int ack = ps2_read_data();
    if (ack != PS2_MOUSE_ACK) {
        uart_puts("[PS2] mouse: no ACK on enable\n");
    }

    g_ps2_mouse_x = g_gfx_mode.width / 2;
    g_ps2_mouse_y = g_gfx_mode.height / 2;
    g_ps2_mouse_init = 1;

    /* 清空可能残留的数据字节 */
    for (int i = 0; i < 10; i++) {
        if (inb(0x64) & 0x01) inb(0x60);
        else break;
    }

    uart_puts("[PS2] mouse initialized\n");
}

/* 轮询 PS/2 鼠标 - 读取一个完整数据包 */
static void ps2_mouse_poll(gfx_mouse_state_t *state)
{
    state->x = g_ps2_mouse_x;
    state->y = g_ps2_mouse_y;
    state->left_button = g_ps2_mouse_btn_left;
    state->right_button = g_ps2_mouse_btn_right;

    /* 非阻塞检查是否有数据就绪 */
    if (!(inb(0x64) & 0x01))
        return;

    uint8_t byte0 = inb(0x60);

    /* 标准 PS/2 鼠标数据包的第一个字节 bit 3 应该为 1 */
    if (!(byte0 & 0x08))
        return;

    /* 检查是否还有后续字节 */
    for (int i = 0; i < 1000; i++) {
        if (inb(0x64) & 0x01) break;
    }
    if (!(inb(0x64) & 0x01)) return;
    uint8_t byte1 = inb(0x60);

    for (int i = 0; i < 1000; i++) {
        if (inb(0x64) & 0x01) break;
    }
    if (!(inb(0x64) & 0x01)) return;
    uint8_t byte2 = inb(0x60);

    /* 解析移动量（有符号 9 位） */
    int dx = (int)byte1;
    int dy = (int)byte2;
    if (byte0 & 0x10) dx |= ~0xFF;  /* X 符号扩展 */
    if (byte0 & 0x20) dy |= ~0xFF;  /* Y 符号扩展 */

    /* Y 轴反转（PS/2 正值=向下，屏幕坐标系正值=向下，一致） */

    /* 更新位置 */
    g_ps2_mouse_x += dx;
    g_ps2_mouse_y += dy;

    /* 边界裁剪 */
    if (g_ps2_mouse_x < 0) g_ps2_mouse_x = 0;
    if (g_ps2_mouse_y < 0) g_ps2_mouse_y = 0;
    if ((UINT32)g_ps2_mouse_x >= g_gfx_mode.width)
        g_ps2_mouse_x = g_gfx_mode.width - 1;
    if ((UINT32)g_ps2_mouse_y >= g_gfx_mode.height)
        g_ps2_mouse_y = g_gfx_mode.height - 1;

    g_ps2_mouse_btn_left = (byte0 & 0x01) ? TRUE : FALSE;
    g_ps2_mouse_btn_right = (byte0 & 0x02) ? TRUE : FALSE;

    state->x = g_ps2_mouse_x;
    state->y = g_ps2_mouse_y;
    state->left_button = g_ps2_mouse_btn_left;
    state->right_button = g_ps2_mouse_btn_right;
}

static void cmd_ui(void)
{
    /* 检查 GOP 是否已初始化 */
    if (g_gfx_mode.fb_base == 0) {
        sh_puts("ui: GOP not initialized, cannot start UI\n");
        return;
    }

    sh_puts("ui: initializing graphics API...\n");

    /* 初始化图形 API 函数指针表（安全，仅设置指针） */
    gfx_api_init_all();

    /* 初始化 PS/2 鼠标（直接端口 I/O，不依赖 UEFI BootServices） */
    sh_puts("ui: initializing PS/2 mouse...\n");
    ps2_mouse_init();

    sh_puts("ui: starting graphical desktop...\n");
    sh_puts("ui: press ESC to return to shell\n");

    /* 鼠标状态 */
    int mouse_x = g_gfx_mode.width / 2;
    int mouse_y = g_gfx_mode.height / 2;
    int old_mx = -1, old_my = -1;

    /* 首次渲染完整桌面背景 */
    ui_render_desktop();
    fb_swap();

    /* 桌面主循环 */
    int running = 1;
    gfx_mouse_state_t mouse_state;
    while (running) {
        /* --- 轮询鼠标（PS/2 直接读取） --- */
        ps2_mouse_poll(&mouse_state);
        if (mouse_state.x != mouse_x || mouse_state.y != mouse_y) {
            old_mx = mouse_x;
            old_my = mouse_y;
            mouse_x = mouse_state.x;
            mouse_y = mouse_state.y;
        }

        /* --- 轮询键盘（PS/2 直接读取，与 shell 共用） --- */
        int ch = sh_getc_nb();
        if (ch == 0x1B) {  /* ESC */
            running = 0;
            continue;
        }

        /* --- 鼠标位置未变化则跳过重绘 --- */
        if (old_mx == mouse_x && old_my == mouse_y) {
            for (volatile int d = 0; d < 100000; d++);
            continue;
        }

        /* --- 重绘桌面背景覆盖旧光标痕迹 --- */
        ui_render_desktop();

        /* --- 绘制鼠标光标 --- */
        g_gfx_api->draw_cursor(mouse_x, mouse_y);

        /* --- 交换缓冲区 --- */
        fb_swap();
    }

    /* 退出桌面 */
    fb_clear(GFX_RGB(0, 0, 0));
    fb_swap();

    sh_puts("ui: desktop closed, returned to shell\n");
}

/* ===============================================================
 * 命令历史功能
 * =============================================================== */

static void add_to_history(const char *line)
{
    int len = sh_strlen(line);
    if (len == 0) return;

    sh_strcpy(g_history[g_history_count % CMD_HISTORY_SIZE], line);
    g_history_count++;
    g_history_index = g_history_count;
}

static const char *get_history_up(void)
{
    if (g_history_count == 0) return NULL;
    if (g_history_index > 0) g_history_index--;
    return g_history[g_history_index % CMD_HISTORY_SIZE];
}

static const char *get_history_down(void)
{
    if (g_history_count == 0) return NULL;
    if (g_history_index < g_history_count) g_history_index++;
    if (g_history_index == g_history_count) return "";
    return g_history[g_history_index % CMD_HISTORY_SIZE];
}

/* ---------------------------------------------------------------
 * Tab 补全功能
 * --------------------------------------------------------------- */

static int tab_complete(const char *input, char *output, int max_len)
{
    if (!g_fs || !input || input[0] == '\0') return 0;

    char full_path[FS_MAX_PATH];
    const char *base = input;
    const char *name_start = sh_str_chr(input, '/');

    if (name_start != NULL) {
        sh_strcpy(full_path, input);
        while (*name_start) {
            name_start++;
            if (*name_start == '/') break;
        }
        base = name_start + 1;
    } else {
        sh_strcpy(full_path, g_cwd);
        if (full_path[sh_strlen(full_path) - 1] != '/')
            sh_strcat(full_path, "/");
        sh_strcat(full_path, input);
    }

    fs_dirent_t entries[64];
    int count = fs_readdir(g_fs, full_path, entries, 64);
    if (count <= 0) return 0;

    int match_count = 0;
    for (int i = 0; i < count; i++) {
        if (sh_str_starts_with(entries[i].name, input)) {
            if (match_count == 0) sh_strcpy(output, entries[i].name);
            match_count++;
        }
    }
    return match_count;
}

/* ===============================================================
 * 命令行解析与执行
 * =============================================================== */

static void execute_command(const char *line)
{
    const char *p = sh_skip_spaces(line);
    if (*p == '\0' || *p == '#') return;

    char cmd_name[32];
    int ci = 0;
    while (*p && *p != ' ' && *p != '\t' && ci < 31)
        cmd_name[ci++] = *p++;
    cmd_name[ci] = '\0';

    p = sh_skip_spaces(p);
    const char *cmd_arg = (*p) ? p : NULL;

    /* ===== 命令分发 ===== */

    /* --- 通用类 --- */
         if (sh_strcmp(cmd_name, "help")    == 0) cmd_help();
    else if (sh_strcmp(cmd_name, "cls")     == 0) cmd_cls();
    else if (sh_strcmp(cmd_name, "echo")    == 0) cmd_echo(cmd_arg);
    else if (sh_strcmp(cmd_name, "exit")    == 0) cmd_exit();
    else if (sh_strcmp(cmd_name, "ver")     == 0) cmd_ver();

    /* --- 系统信息类 --- */
    else if (sh_strcmp(cmd_name, "date")     == 0) cmd_date();
    else if (sh_strcmp(cmd_name, "uptime")   == 0) cmd_uptime();
    else if (sh_strcmp(cmd_name, "hostname") == 0) cmd_hostname(cmd_arg);
    else if (sh_strcmp(cmd_name, "env")      == 0) cmd_env();
    else if (sh_strcmp(cmd_name, "set")      == 0) cmd_set(cmd_arg);
    else if (sh_strcmp(cmd_name, "cpu")      == 0) cmd_cpu();
    else if (sh_strcmp(cmd_name, "disk")     == 0) cmd_disk();

    /* --- 文件类 --- */
    else if (sh_strcmp(cmd_name, "ls")    == 0) cmd_ls(cmd_arg);
    else if (sh_strcmp(cmd_name, "cd")    == 0) cmd_cd(cmd_arg);
    else if (sh_strcmp(cmd_name, "pwd")   == 0) cmd_pwd();
    else if (sh_strcmp(cmd_name, "cat")   == 0) cmd_cat(cmd_arg);
    else if (sh_strcmp(cmd_name, "more")  == 0) cmd_more(cmd_arg);
    else if (sh_strcmp(cmd_name, "head")  == 0) cmd_head(cmd_arg);
    else if (sh_strcmp(cmd_name, "tail")  == 0) cmd_tail(cmd_arg);
    else if (sh_strcmp(cmd_name, "find")  == 0) cmd_find(cmd_arg);
    else if (sh_strcmp(cmd_name, "stat")  == 0) cmd_stat(cmd_arg);
    else if (sh_strcmp(cmd_name, "mkdir") == 0) cmd_mkdir(cmd_arg);
    else if (sh_strcmp(cmd_name, "rm")    == 0) cmd_rm(cmd_arg);
    else if (sh_strcmp(cmd_name, "rmdir") == 0) cmd_rmdir(cmd_arg);
    else if (sh_strcmp(cmd_name, "cp")    == 0) cmd_cp(cmd_arg);
    else if (sh_strcmp(cmd_name, "mv")    == 0) cmd_mv(cmd_arg);
    else if (sh_strcmp(cmd_name, "touch") == 0) cmd_touch(cmd_arg);
    else if (sh_strcmp(cmd_name, "run")   == 0) cmd_run(cmd_arg);

    /* --- 进程类 --- */
    else if (sh_strcmp(cmd_name, "ps")     == 0) cmd_ps();
    else if (sh_strcmp(cmd_name, "kill")   == 0) cmd_kill(cmd_arg);
    else if (sh_strcmp(cmd_name, "top")    == 0) cmd_top();
    else if (sh_strcmp(cmd_name, "start")  == 0) cmd_start(cmd_arg);
    else if (sh_strcmp(cmd_name, "stop")   == 0) cmd_stop(cmd_arg);
    else if (sh_strcmp(cmd_name, "task")   == 0) cmd_task();

    /* --- 系统控制类 --- */
    else if (sh_strcmp(cmd_name, "reboot")   == 0) cmd_reboot();
    else if (sh_strcmp(cmd_name, "shutdown") == 0) cmd_shutdown();
    else if (sh_strcmp(cmd_name, "mount")    == 0) cmd_mount(cmd_arg);
    else if (sh_strcmp(cmd_name, "umount")   == 0) cmd_umount();
    else if (sh_strcmp(cmd_name, "format")   == 0) cmd_format(cmd_arg);
    else if (sh_strcmp(cmd_name, "device")   == 0) cmd_device();
    else if (sh_strcmp(cmd_name, "log")      == 0) cmd_log();

    /* --- 调试类 --- */
    else if (sh_strcmp(cmd_name, "dump")    == 0) cmd_dump();
    else if (sh_strcmp(cmd_name, "sched")   == 0) cmd_sched();
    else if (sh_strcmp(cmd_name, "mem")     == 0) cmd_mem();
    else if (sh_strcmp(cmd_name, "syscall") == 0) cmd_syscall();
    else if (sh_strcmp(cmd_name, "klog")    == 0) cmd_klog();
    else if (sh_strcmp(cmd_name, "trace")   == 0) cmd_trace();
    else if (sh_strcmp(cmd_name, "panic")   == 0) cmd_panic();
    else if (sh_strcmp(cmd_name, "irq")     == 0) cmd_irq();
    else if (sh_strcmp(cmd_name, "ipc")     == 0) cmd_ipc();
    else if (sh_strcmp(cmd_name, "heap")    == 0) cmd_heap();
    else if (sh_strcmp(cmd_name, "debug")   == 0) cmd_debug();

    /* --- 图形类 --- */
    else if (sh_strcmp(cmd_name, "ui")      == 0) cmd_ui();

    /* --- 未知命令 --- */
    else {
        sh_puts("unknown command: "); sh_puts(cmd_name);
        sh_puts("\nType 'help' for available commands.\n");
    }
}

/* ---------------------------------------------------------------
 * 从串口读取一行 (带回显和退格)
 * --------------------------------------------------------------- */

static int read_line(char *buf, int max_len)
{
    int pos = 0;
    char temp_buf[256] = {0};

    while (pos < max_len - 1) {
        int ch = sh_getc_nb();

        if (ch >= 0) {
            if (ch == '\r' || ch == '\n') {
                buf[pos] = '\0';
                add_to_history(buf);
                sh_putc('\n');
                return pos;
            } else if (ch == '\b' || ch == 0x7F) {
                if (pos > 0) {
                    pos--;
                    sh_puts("\b \b");
                }
                continue;
            } else if (ch == '\t') {
                int matches = tab_complete(buf, temp_buf, sizeof(temp_buf));
                if (matches == 1) {
                    int temp_len = sh_strlen(temp_buf);
                    if (pos > 0) {
                        for (int i = 0; i < pos; i++)
                            sh_puts("\b \b");
                    }
                    sh_puts(temp_buf);
                    sh_strcpy(buf, temp_buf);
                    pos = temp_len;
                }
                continue;
            } else if (ch == 0x48) {
                const char *history = get_history_up();
                if (history != NULL) {
                    for (int i = 0; i < pos; i++) sh_puts("\b \b");
                    sh_puts(history);
                    sh_strcpy(buf, history);
                    pos = sh_strlen(history);
                }
                continue;
            } else if (ch == 0x50) {
                const char *history = get_history_down();
                if (history != NULL) {
                    for (int i = 0; i < pos; i++) sh_puts("\b \b");
                    sh_puts(history);
                    sh_strcpy(buf, history);
                    pos = sh_strlen(history);
                }
                continue;
            } else if (ch >= 32 && ch < 127) {
                buf[pos++] = (char)ch;
                sh_putc((char)ch);
            }
        }
    }

    buf[max_len - 1] = '\0';
    sh_putc('\n');
    return pos;
}

/* ---------------------------------------------------------------
 * Shell 初始化
 * --------------------------------------------------------------- */

void shell_init(fs_handle_t *fs)
{
    g_fs = fs;

    /* 设置默认环境变量 */
    sh_strcpy(g_env[g_env_count].name, "PATH");
    sh_strcpy(g_env[g_env_count].value, "/bin");
    g_env_count++;

    sh_strcpy(g_env[g_env_count].name, "HOSTNAME");
    sh_strcpy(g_env[g_env_count].value, g_hostname);
    g_env_count++;

    sh_strcpy(g_env[g_env_count].name, "SHELL");
    sh_strcpy(g_env[g_env_count].value, "/bin/shell");
    g_env_count++;

    sh_strcpy(g_env[g_env_count].name, "OS");
    sh_strcpy(g_env[g_env_count].value, "DimensOS");
    g_env_count++;
}

/* ---------------------------------------------------------------
 * Shell 主入口 (调度器任务, 永不返回)
 * --------------------------------------------------------------- */

void shell_main(void)
{
    char line[256];

    /* 刷新键盘缓冲区 */
    while (inb(0x64) & 0x01) inb(0x60);

    sh_puts("\n");
    sh_puts("   _____    _                                     ____   _____ \n");
    sh_puts("  |  __ \\  (_)                                   / __ \\ / ____| \n");
    sh_puts("  | |  | |  _   _ __ ___     ___   _ __    ___  | |  | | (___  \n");
    sh_puts("  | |  | | | | | '_ ` _ \\   / _ \\ | '_ \\  / __| | |  | |\\___ \\ \n");
    sh_puts("  | |__| | | | | | | | | | |  __/ | | | | \\__ \\ | |__| |____) | \n");
    sh_puts("  |_____/  |_| |_| |_| |_|  \\___| |_| |_| |___/  \\____/|_____/ \n");
    sh_puts("                                                               \n");
    sh_puts("                                                               \n");
    sh_puts("        DimensOS Command Line Interface\n");
    sh_puts("  --------------------------------------\n");
    sh_puts("  Type 'help' for available commands\n");
    sh_puts("  --------------------------------------\n\n");

    /* 加载默认环境变量到 env 表 (已在 shell_init 中完成) */

    while (1) {
        /* 输出提示符 */
        sh_putc('[');
        sh_puts(g_cwd);
        sh_puts("]\n");
        sh_puts("DimensOS> ");

        /* 读取一行输入 */
        read_line(line, sizeof(line));

        /* 执行命令 */
        execute_command(line);
    }
}