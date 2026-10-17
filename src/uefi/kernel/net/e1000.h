/*
 * 深维工坊工作室 (DeepDimension Workshop)
 * DimensOS - Stage 5: 网络层
 *
 * 文件: e1000.h
 * 描述: Intel e1000 网卡驱动接口定义
 */

#ifndef DIMENSOS_E1000_H
#define DIMENSOS_E1000_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------
 * e1000 寄存器定义 (MMIO 映射)
 * --------------------------------------------------------------- */

/* 控制寄存器 */
#define E1000_CTRL      0x00000  /* 设备控制 */
#define E1000_STATUS    0x00008  /* 设备状态 */

/* 接收寄存器 */
#define E1000_RCTL      0x00100  /* 接收控制 */
#define E1000_RDLEN     0x00108  /* 接收描述符长度 */
#define E1000_RDH       0x0010C  /* 接收描述符头部 */
#define E1000_RDT       0x00110  /* 接收描述符尾部 */
#define E1000_RDBAL     0x00118  /* 接收描述符基地址低32位 */
#define E1000_RDBAH     0x0011C  /* 接收描述符基地址高32位 */

/* 发送寄存器 */
#define E1000_TCTL      0x00400  /* 发送控制 */
#define E1000_TDLEN     0x00408  /* 发送描述符长度 */
#define E1000_TDH       0x0040C  /* 发送描述符头部 */
#define E1000_TDT       0x00410  /* 发送描述符尾部 */
#define E1000_TDBAL     0x00418  /* 发送描述符基地址低32位 */
#define E1000_TDBAH     0x0041C  /* 发送描述符基地址高32位 */

/* 中断寄存器 */
#define E1000_IMS       0x000D0  /* 中断掩码设置 */
#define E1000_ICR       0x000C0  /* 中断清除寄存器 */

/* ---------------------------------------------------------------
 * e1000 接收描述符结构
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t addr;       /* 缓冲区地址 */
    uint16_t len;        /* 缓冲区长度 */
    uint16_t csum;       /* 校验和 */
    uint8_t  status;     /* 状态 */
    uint8_t  errors;     /* 错误 */
    uint16_t special;    /* 特殊 */
} e1000_rx_desc_t;

/* ---------------------------------------------------------------
 * e1000 发送描述符结构
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t addr;       /* 缓冲区地址 */
    uint16_t len;        /* 数据长度 */
    uint8_t  cso;        /* 校验和偏移 */
    uint8_t  cmd;        /* 命令 */
    uint8_t  status;     /* 状态 */
    uint8_t  css;        /* 校验和起始 */
    uint16_t special;    /* 特殊 */
} e1000_tx_desc_t;

/* ---------------------------------------------------------------
 * 发送命令位
 * --------------------------------------------------------------- */
#define E1000_TXD_CMD_RS   0x08  /* 报告状态 */
#define E1000_TXD_CMD_EOP  0x01  /* 包结束 */

/* ---------------------------------------------------------------
 * 接收状态位
 * --------------------------------------------------------------- */
#define E1000_RXD_STAT_DD  0x01  /* 描述符完成 */

/* ---------------------------------------------------------------
 * e1000 网卡状态结构
 * --------------------------------------------------------------- */
typedef struct {
    volatile uint32_t *mmio_base;  /* MMIO 基地址 */
    uint64_t          mmio_phys;   /* 物理地址 */
    size_t            mmio_size;   /* 映射大小 */

    /* 接收队列 */
    e1000_rx_desc_t   *rx_ring;
    uint64_t          rx_ring_phys;
    uint8_t           *rx_buffers;
    uint64_t          rx_buffers_phys;
    int               rx_count;
    int               rx_next;

    /* 发送队列 */
    e1000_tx_desc_t   *tx_ring;
    uint64_t          tx_ring_phys;
    uint8_t           *tx_buffers;
    uint64_t          tx_buffers_phys;
    int               tx_count;
    int               tx_next;

    /* MAC 地址 */
    uint8_t           mac[6];

    /* 状态 */
    int               initialized;
} e1000_dev_t;

/* ---------------------------------------------------------------
 * e1000 驱动 API
 * --------------------------------------------------------------- */

/* 初始化 e1000 网卡（需要预先分配好的内存） */
int e1000_init(e1000_dev_t *dev, uint64_t mmio_base,
               void *rx_ring, uint64_t rx_ring_phys,
               void *tx_ring, uint64_t tx_ring_phys,
               void *rx_buffers, uint64_t rx_buffers_phys,
               void *tx_buffers, uint64_t tx_buffers_phys);

/* 发送数据包 */
int e1000_send(e1000_dev_t *dev, const uint8_t *data, int len);

/* 接收数据包 */
int e1000_recv(e1000_dev_t *dev, uint8_t *buf, int buf_len);

/* 启用接收 */
void e1000_enable_rx(e1000_dev_t *dev);

/* 启用发送 */
void e1000_enable_tx(e1000_dev_t *dev);

#endif /* DIMENSOS_E1000_H */
