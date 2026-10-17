# DimensOS（深维操作系统）· 开源底座

> 深维工坊工作室 (ShenWei Workshop) / 深维云计算

DimensOS 是一个从零自研的 x86-64 操作系统，UEFI 引导。本仓库为其 **开源底座**，
采用 MIT 许可。可用于学习、二次开发与自研操作系统起步。

---

## 开源范围

### 包含（通用底座）

| 模块 | 路径 | 说明 |
|------|------|------|
| UEFI 引导 | `src/uefi/main.c` | 引导流程、内存映射、ExitBootServices、各阶段初始化 |
| GDT | `src/uefi/kernel/gdt.c` | x86-64 全局描述符表 |
| IDT | `src/uefi/kernel/idt.c` | 中断/异常描述符表（含错误码异常处理） |
| 分页 | `src/uefi/kernel/paging.c` | 四级页表、恒等映射 |
| 物理内存管理 | `src/uefi/kernel/pmm.c` | 位图式页分配器 |
| 基础库 | `src/uefi/kernel/lib/io.c` | 端口 IO + UART 串口 |
| 调度 | `src/uefi/kernel/sched/sched.c` | 时间片轮转任务调度 |
| 定时器 | `src/uefi/kernel/sched/timer.c` | PIT 定时器 |
| 文件系统 | `src/uefi/kernel/fs/` | FAT32 / NTFS(只读) / VFS + 盘符系统 |
| 命令行 | `src/uefi/kernel/shell/shell.c` | Shell 命令解析 |
| 图形框架 | `src/uefi/kernel/gfx/` | GOP / 帧缓冲 / 字体 / 图标 / 图形服务 |
| 图形 API | `src/uefi/kernel/api/gfx_api.c` | 内核暴露给用户态的图形接口 |
| 网卡驱动 | `src/uefi/kernel/net/e1000.c` | Intel e1000 标准网卡驱动 |

### 不包含（自研专有，另行发布）

- **SWNP** 传输协议（IP 协议号 253 / PVRT 头部 / 7 状态机）
- **SWSP** 安全层（PSK 协商 / AES-256-GCM）
- **WSWP** Windows 适配层
- **DSXE** 可执行格式与加载器、内核导出表
- 桌面环境 (explorer) 与 Windows EXE 预览态

---

## 目录结构

```
src/uefi/
├── main.c                  引导入口
├── kernel/
│   ├── gdt.c / gdt.h
│   ├── idt.c / idt.h
│   ├── paging.c / paging.h
│   ├── pmm.c / pmm.h
│   ├── memset.c
│   ├── lib/                端口 IO + UART
│   ├── sched/              调度器 + 定时器
│   ├── fs/                 FAT32 / NTFS / VFS
│   ├── shell/              命令行 Shell
│   ├── gfx/                GOP / 帧缓冲 / 字体 / 图标
│   ├── api/                图形 API
│   └── net/                e1000 网卡驱动
```

---

## 构建

### 依赖

- MSYS2 + `mingw-w64-x86_64-gcc`
- gnu-efi 头文件（仅使用类型定义，不链接 efilib）

### 编译

每个源文件独立编译为对象文件，标志一致：

```
x86_64-w64-mingw32-gcc -c <源文件>.c -o <目标>.o ^
    -m64 -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone ^
    -maccumulate-outgoing-args -fpic -mno-stack-arg-probe ^
    -I<gnu-efi 头文件目录>
```

### 链接

```
x86_64-w64-mingw32-gcc <所有 .o> -o bootx64.efi ^
    -m64 -nostdlib -Wl,-dll -shared -Wl,--subsystem,10 -e efi_main
```

生成的 PE 文件子系统须为 **EFI Application (10)**；若链接器输出的子系统标识不正确，
需在链接后修正 PE Optional Header 的 `Subsystem` 字段为 `0xA`。

---

## 运行（QEMU + OVMF）

```
qemu-system-x86_64 -m 256M -cpu qemu64 -machine q35 ^
    -drive if=pflash,format=raw,readonly=on,file=<OVMF_CODE.fd> ^
    -drive if=pflash,format=raw,file=<OVMF_VARS.fd> ^
    -drive file=fat:rw:<EFI 目录>,format=raw ^
    -device e1000,netdev=net0 -netdev user,id=net0 ^
    -serial stdio -vga std -no-reboot
```

启动日志通过串口（COM1）输出。实体机部署时需将 `bootx64.efi` 置于
EFI 分区的 `/EFI/BOOT/` 目录下。

---

## 许可

MIT License，详见 [LICENSE](LICENSE)。

署名：**深维工坊工作室 (ShenWei Workshop) / 深维云计算**
