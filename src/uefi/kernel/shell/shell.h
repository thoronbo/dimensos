/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS - Stage 4: 命令行 Shell (CLI)
 *
 * 文件: shell.h
 * 描述: UEFI 模式下的文本命令行 Shell 接口
 *       通过串口 (COM1) 与用户交互，支持 ls、cd、cat、run、help 命令。
 */

#ifndef DIMENSOS_SHELL_H
#define DIMENSOS_SHELL_H

#include "../fs/fs.h"
#include <efi.h>

/* 全局 SystemTable 指针 (在 main.c 中定义) */
extern EFI_SYSTEM_TABLE *g_ST;

/* ---------------------------------------------------------------
 * Shell 初始化
 *
 * 在创建 Shell 任务前调用，传入已挂载的文件系统句柄。
 * --------------------------------------------------------------- */
void shell_init(fs_handle_t *fs);

/* ---------------------------------------------------------------
 * Shell 主入口 (任务函数)
 *
 * 作为调度器中的一个任务运行，永不返回。
 * 通过串口与用户交互，解析并执行命令。
 * --------------------------------------------------------------- */
void shell_main(void);

#endif /* DIMENSOS_SHELL_H */
