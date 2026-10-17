/*
 * 深维工坊工作室 (ShenWei Workshop)
 * DimensOS — memset 实现 (freestanding 环境)
 *
 * 文件: memset.c
 * 描述: 在 freestanding 环境中提供标准 memset 函数。
 *       GCC 编译 UEFI 目标时可能生成 memset 调用，
 *       此文件提供实际实现。
 */

#include <stddef.h>

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

/* memcpy - 内存复制 */
void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
    return dest;
}
