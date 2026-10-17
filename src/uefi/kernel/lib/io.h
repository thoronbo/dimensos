/*
 * 深维工坊工作室 (DeepDimension Workshop)
 * DimensOS - Stage 4: 统一IO库
 *
 * 文件: io.h
 * 描述: IO操作统一封装接口
 */

#ifndef DIMENSOS_IO_H
#define DIMENSOS_IO_H

#include <stdint.h>

/* ---------------------------------------------------------------
 * 端口IO操作
 * --------------------------------------------------------------- */

/* 读取字节 */
static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

/* 写入字节 */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "dN"(port));
}

/* 读取16位 */
static inline uint16_t inw(uint16_t port)
{
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

/* 写入16位 */
static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "dN"(port));
}

/* 读取32位 */
static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "dN"(port));
    return val;
}

/* 写入32位 */
static inline void outl(uint16_t port, uint32_t val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "dN"(port));
}

/* ---------------------------------------------------------------
 * UART串口操作
 * --------------------------------------------------------------- */

/* COM1 端口地址 */
#define UART_COM1  0x3F8

/* 初始化UART */
void uart_init(void);

/* 发送一个字符 */
void uart_putc(char c);

/* 发送字符串 */
void uart_puts(const char *str);

/* 发送十六进制数字 */
void uart_puthex(uint64_t val);

/* 检查是否有数据可读 */
int uart_has_data(void);

/* 读取一个字符 */
char uart_getc(void);

#endif /* DIMENSOS_IO_H */
