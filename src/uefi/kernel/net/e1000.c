/*
 * 深维工坊工作室 (DeepDimension Workshop)
 * DimensOS - Stage 5: 网络层
 *
 * 文件: e1000.c
 * 描述: Intel e1000 网卡驱动实现
 */

#include "e1000.h"
#include <string.h>

/* 队列大小 */
#define RX_RING_SIZE  256
#define TX_RING_SIZE  256
#define BUF_SIZE      2048

/* ---------------------------------------------------------------
 * 寄存器读写辅助函数
 * --------------------------------------------------------------- */
static inline uint32_t e1000_read(e1000_dev_t *dev, uint32_t reg)
{
    return dev->mmio_base[reg / 4];
}

static inline void e1000_write(e1000_dev_t *dev, uint32_t reg, uint32_t val)
{
    dev->mmio_base[reg / 4] = val;
}

/* ---------------------------------------------------------------
 * e1000 网卡初始化
 * --------------------------------------------------------------- */
int e1000_init(e1000_dev_t *dev, uint64_t mmio_base,
               void *rx_ring, uint64_t rx_ring_phys,
               void *tx_ring, uint64_t tx_ring_phys,
               void *rx_buffers, uint64_t rx_buffers_phys,
               void *tx_buffers, uint64_t tx_buffers_phys)
{
    memset(dev, 0, sizeof(*dev));

    /* 映射 MMIO 空间 */
    dev->mmio_base = (volatile uint32_t *)mmio_base;
    dev->mmio_phys = mmio_base;
    dev->mmio_size = 0x10000;

    /* 获取 MAC 地址（从设备 ID 寄存器区域读取） */
    uint32_t mac_low = e1000_read(dev, 0x5400);
    uint32_t mac_high = e1000_read(dev, 0x5404);

    dev->mac[0] = (mac_low >> 0) & 0xFF;
    dev->mac[1] = (mac_low >> 8) & 0xFF;
    dev->mac[2] = (mac_low >> 16) & 0xFF;
    dev->mac[3] = (mac_low >> 24) & 0xFF;
    dev->mac[4] = (mac_high >> 0) & 0xFF;
    dev->mac[5] = (mac_high >> 8) & 0xFF;

    /* 配置接收队列 */
    dev->rx_ring = (e1000_rx_desc_t *)rx_ring;
    dev->rx_ring_phys = rx_ring_phys;
    dev->rx_buffers = (uint8_t *)rx_buffers;
    dev->rx_buffers_phys = rx_buffers_phys;
    dev->rx_count = RX_RING_SIZE;
    dev->rx_next = 0;

    /* 初始化接收描述符 */
    int i;
    for (i = 0; i < RX_RING_SIZE; i++) {
        dev->rx_ring[i].addr = rx_buffers_phys + i * BUF_SIZE;
        dev->rx_ring[i].len = BUF_SIZE;
        dev->rx_ring[i].status = 0;
        dev->rx_ring[i].errors = 0;
        dev->rx_ring[i].special = 0;
    }

    /* 配置发送队列 */
    dev->tx_ring = (e1000_tx_desc_t *)tx_ring;
    dev->tx_ring_phys = tx_ring_phys;
    dev->tx_buffers = (uint8_t *)tx_buffers;
    dev->tx_buffers_phys = tx_buffers_phys;
    dev->tx_count = TX_RING_SIZE;
    dev->tx_next = 0;

    /* 初始化发送描述符 */
    for (i = 0; i < TX_RING_SIZE; i++) {
        dev->tx_ring[i].addr = tx_buffers_phys + i * BUF_SIZE;
        dev->tx_ring[i].len = 0;
        dev->tx_ring[i].cmd = 0;
        dev->tx_ring[i].status = 0x01;  /* 标记为可用 */
        dev->tx_ring[i].special = 0;
    }

    /* 配置接收描述符基地址 */
    e1000_write(dev, E1000_RDBAL, (uint32_t)(rx_ring_phys & 0xFFFFFFFF));
    e1000_write(dev, E1000_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    e1000_write(dev, E1000_RDLEN, RX_RING_SIZE * sizeof(e1000_rx_desc_t));
    e1000_write(dev, E1000_RDH, 0);
    e1000_write(dev, E1000_RDT, RX_RING_SIZE - 1);

    /* 配置发送描述符基地址 */
    e1000_write(dev, E1000_TDBAL, (uint32_t)(tx_ring_phys & 0xFFFFFFFF));
    e1000_write(dev, E1000_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    e1000_write(dev, E1000_TDLEN, TX_RING_SIZE * sizeof(e1000_tx_desc_t));
    e1000_write(dev, E1000_TDH, 0);
    e1000_write(dev, E1000_TDT, 0);

    /* 启用接收和发送 */
    e1000_enable_rx(dev);
    e1000_enable_tx(dev);

    dev->initialized = 1;
    return 0;
}

/* ---------------------------------------------------------------
 * 发送数据包
 * --------------------------------------------------------------- */
int e1000_send(e1000_dev_t *dev, const uint8_t *data, int len)
{
    if (!dev->initialized)
        return -1;

    if (len <= 0 || len > BUF_SIZE)
        return -1;

    /* 等待发送描述符可用 */
    e1000_tx_desc_t *desc = &dev->tx_ring[dev->tx_next];
    while (!(desc->status & 0x01))
        ;

    /* 复制数据到发送缓冲区 */
    memcpy(&dev->tx_buffers[dev->tx_next * BUF_SIZE], data, len);

    /* 更新描述符 */
    desc->addr = dev->tx_buffers_phys + dev->tx_next * BUF_SIZE;
    desc->len = len;
    desc->cmd = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;
    desc->status = 0;

    /* 更新尾部指针 */
    dev->tx_next = (dev->tx_next + 1) % dev->tx_count;
    e1000_write(dev, E1000_TDT, dev->tx_next);

    return len;
}

/* ---------------------------------------------------------------
 * 接收数据包
 * --------------------------------------------------------------- */
int e1000_recv(e1000_dev_t *dev, uint8_t *buf, int buf_len)
{
    if (!dev->initialized)
        return -1;

    e1000_rx_desc_t *desc = &dev->rx_ring[dev->rx_next];

    /* 检查是否有新数据包 */
    if (!(desc->status & E1000_RXD_STAT_DD))
        return 0;

    /* 获取数据长度 */
    int len = desc->len;
    if (len > buf_len)
        len = buf_len;

    /* 复制数据 */
    memcpy(buf, &dev->rx_buffers[dev->rx_next * BUF_SIZE], len);

    /* 重置描述符状态 */
    desc->status = 0;
    desc->addr = dev->rx_buffers_phys + dev->rx_next * BUF_SIZE;

    /* 更新尾部指针 */
    dev->rx_next = (dev->rx_next + 1) % dev->rx_count;
    e1000_write(dev, E1000_RDT, dev->rx_next);

    return len;
}

/* ---------------------------------------------------------------
 * 启用接收
 * --------------------------------------------------------------- */
void e1000_enable_rx(e1000_dev_t *dev)
{
    if (!dev->initialized)
        return;

    /* 设置接收控制寄存器 */
    uint32_t rctl = e1000_read(dev, E1000_RCTL);
    rctl |= 0x00000002;  /* RFU */
    rctl |= 0x00000400;  /* BAM */
    rctl |= 0x00000080;  /* SECRC */
    e1000_write(dev, E1000_RCTL, rctl);
}

/* ---------------------------------------------------------------
 * 启用发送
 * --------------------------------------------------------------- */
void e1000_enable_tx(e1000_dev_t *dev)
{
    if (!dev->initialized)
        return;

    /* 设置发送控制寄存器 */
    uint32_t tctl = e1000_read(dev, E1000_TCTL);
    tctl |= 0x00000002;  /* EN */
    tctl |= 0x00000008;  /* PS */
    tctl |= 0x00000010;  /* IFGPD */
    e1000_write(dev, E1000_TCTL, tctl);
}
