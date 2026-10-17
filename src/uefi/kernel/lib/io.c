/*
 * 深维工坊工作室 (DeepDimension Workshop)
 * DimensOS - Stage 4: 统一IO库
 *
 * 文件: io.c
 * 描述: IO操作统一封装实现
 */

#include "io.h"

/* ---------------------------------------------------------------
 * UART串口操作实现
 * --------------------------------------------------------------- */

void uart_init(void)
{
    outb(UART_COM1 + 1, 0x00); /* 禁用中断 */
    outb(UART_COM1 + 3, 0x80); /* 启用DLAB */
    outb(UART_COM1 + 0, 0x03); /* 设置波特率 38400 (115200 / 3) */
    outb(UART_COM1 + 1, 0x00);
    outb(UART_COM1 + 3, 0x03); /* 8位, 无奇偶校验, 1停止位 */
    outb(UART_COM1 + 2, 0xC7); /* 启用FIFO */
    outb(UART_COM1 + 4, 0x0B); /* IRQs启用, RTS/DSR设置 */
}

void uart_putc(char c)
{
    /* 等待发送缓冲区为空 */
    while (!(inb(UART_COM1 + 5) & 0x20));
    outb(UART_COM1, c);

    /* 转换\n为\r\n */
    if (c == '\n') {
        while (!(inb(UART_COM1 + 5) & 0x20));
        outb(UART_COM1, '\r');
    }
}

void uart_puts(const char *str)
{
    while (*str) {
        uart_putc(*str++);
    }
}

void uart_puthex(uint64_t val)
{
    char hex_chars[] = "0123456789ABCDEF";
    int i;

    uart_puts("0x");
    for (i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (val >> i) & 0xF;
        uart_putc(hex_chars[nibble]);
    }
}

int uart_has_data(void)
{
    return inb(UART_COM1 + 5) & 0x01;
}

char uart_getc(void)
{
    while (!uart_has_data());
    return inb(UART_COM1);
}
