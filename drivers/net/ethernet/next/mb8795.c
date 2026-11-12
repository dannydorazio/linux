// SPDX-License-Identifier: GPL-2.0
/*
 * Fujitsu MB8795 ethernet on NeXT computers
 *
 * The MB8795 has no DMA engine of its own: frames are moved by the
 * ethernet transmit and receive channels of the NeXT DMA controller.
 *
 * The receive channel always has two buffers: the one it is filling
 * ("next" to "limit") and a continuation ("start" to "stop") that it
 * switches to after each frame.  It records where a frame ended in its
 * "saved limit" register, which also tells which buffer the frame is in.
 * If both buffers fill before the interrupt is serviced, only the second
 * frame's end is kept; the first is taken with the length in its header.
 * Frames are sent one at a time from a bounce buffer.
 *
 * The Turbo machines have an AT&T 7213 in its place, with the same
 * registers but other mode bits: the receiver has an enable bit, and a
 * frame goes out when the transmit mode's start bit is set, rather than
 * at a marked end of the DMA buffer.  Their DMA controller has the saved
 * limit elsewhere and resets a channel's buffer with another bit.  This
 * follows NetBSD and what Previous emulates.
 */

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ipv6.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>

#include <asm/nexthw.h>

#define DRV_NAME		"mb8795"

/* MB8795 registers */
#define MB_TXSTAT		0
#define MB_TXMASK		1
#define MB_RXSTAT		2
#define MB_RXMASK		3
#define MB_TXMODE		4
#define  MB_TXMODE_LB_DISABLE	0x02	/* no loopback */
#define  MB_TXMODE_TURBO_TPE	0x04	/* twisted pair, not thin wire */
#define  MB_TXMODE_TURBO_START	0x80	/* send what the DMA channel holds */
#define MB_RXMODE		5
#define  MB_RXMODE_OFF		0x00
#define  MB_RXMODE_NORMAL	0x01	/* own address and broadcast */
#define  MB_RXMODE_MULTICAST	0x02	/* also multicast */
#define  MB_RXMODE_PROMISC	0x03
#define  MB_RXMODE_TURBO_ANY	0x01	/* every frame */
#define  MB_RXMODE_TURBO_OWN	0x02	/* own address and broadcast */
#define  MB_RXMODE_TURBO_ENABLE	0x80
#define MB_RESET		6
#define  MB_RESET_MODE		0x80
#define  MB_CTRL_TURBO_BADTPE	0x40	/* read: no twisted pair link */
#define MB_ENADDR		8
#define MB_STAT_CLEAR		0xff

/* A frame and its CRC; a whole number of 16-byte DMA bursts */
#define MB_BUF_SIZE		1536
#define MB_RX_BUFS		2
#define MB_CRC_LEN		4

#define MB_DMA_ADDR_MASK	0x0fffffff	/* flags live in the top bits */

#define MB_TX_TIMEOUT		(2 * HZ)

struct mb8795 {
	struct net_device	*dev;
	void __iomem		*regs;
	int			tx_irq;
	int			rx_irq;
	void			*buf;		/* tx buffer, then rx buffers */
	dma_addr_t		buf_dma;
	bool			turbo;
	u8			txmode;		/* Turbo: the medium */
};

static void *mb_tx_buf(struct mb8795 *mp)
{
	return mp->buf;
}

static void *mb_rx_buf(struct mb8795 *mp, int i)
{
	return mp->buf + (1 + i) * MB_BUF_SIZE;
}

static dma_addr_t mb_buf_dma(struct mb8795 *mp, int buf)
{
	return mp->buf_dma + buf * MB_BUF_SIZE;
}

static void mb_write(struct mb8795 *mp, int reg, u8 val)
{
	writeb(val, mp->regs + reg);
}

static void mb_dma_stop(unsigned int csr)
{
	next_dma_write(csr, 0, NEXT_DMACSR_RESET | NEXT_DMACSR_CLRCOMPLETE);
	next_dma_write(csr, 0, 0);
}

static void mb_reset_chip(struct mb8795 *mp)
{
	const u8 *addr = mp->dev->dev_addr;
	int i;

	mb_write(mp, MB_RESET, MB_RESET_MODE);
	/* the chip's own interrupts cannot be masked; only DMA ones are used */
	mb_write(mp, MB_TXMASK, 0);
	mb_write(mp, MB_TXSTAT, MB_STAT_CLEAR);
	mb_write(mp, MB_RXMASK, 0);
	mb_write(mp, MB_RXSTAT, MB_STAT_CLEAR);
	for (i = 0; i < ETH_ALEN; i++)
		mb_write(mp, MB_ENADDR + i, addr[i]);
	mb_write(mp, MB_RESET, 0);
}

static void mb_set_rx_mode(struct net_device *dev);

static void mb_rx_dma_start(struct mb8795 *mp)
{
	const unsigned int csr = NEXT_DMA_ENETR_CSR;

	next_dma_write(csr, 0, NEXT_DMACSR_RESET | next_dma_initbuf() |
			       NEXT_DMACSR_SETREAD);
	next_dma_write(csr, NEXT_DMA_NEXT, mb_buf_dma(mp, 1));
	next_dma_write(csr, NEXT_DMA_LIMIT, mb_buf_dma(mp, 1) + MB_BUF_SIZE);
	next_dma_write(csr, NEXT_DMA_START, mb_buf_dma(mp, 2));
	next_dma_write(csr, NEXT_DMA_STOP, mb_buf_dma(mp, 2) + MB_BUF_SIZE);
	next_dma_write(csr, 0, NEXT_DMACSR_SETENABLE | NEXT_DMACSR_SETSUPDATE |
			       NEXT_DMACSR_SETREAD);
	/* the Turbo receiver switches itself off when it overflows */
	if (mp->turbo)
		mb_set_rx_mode(mp->dev);
}

static void mb_set_rx_mode(struct net_device *dev)
{
	struct mb8795 *mp = netdev_priv(dev);
	bool all = dev->flags & IFF_PROMISC;
	bool multi = (dev->flags & IFF_ALLMULTI) || !netdev_mc_empty(dev);
	u8 mode;

	if (mp->turbo)
		/* no multicast filter: take everything */
		mode = MB_RXMODE_TURBO_ENABLE |
		       (all || multi ? MB_RXMODE_TURBO_ANY : MB_RXMODE_TURBO_OWN);
	else
		mode = all ? MB_RXMODE_PROMISC :
		       multi ? MB_RXMODE_MULTICAST : MB_RXMODE_NORMAL;
	mb_write(mp, MB_RXMODE, mode);
}

static void mb_rx_frame(struct mb8795 *mp, int i, unsigned int len)
{
	struct net_device *dev = mp->dev;
	struct sk_buff *skb;

	if (len < ETH_ZLEN + MB_CRC_LEN || len > MB_BUF_SIZE) {
		dev->stats.rx_length_errors++;
		dev->stats.rx_errors++;
		return;
	}
	len -= MB_CRC_LEN;

	skb = netdev_alloc_skb_ip_align(dev, len);
	if (!skb) {
		dev->stats.rx_dropped++;
		return;
	}
	skb_put_data(skb, mb_rx_buf(mp, i), len);
	skb->protocol = eth_type_trans(skb, dev);
	netif_rx(skb);
	dev->stats.rx_packets++;
	dev->stats.rx_bytes += len;
}

/* Which buffer the last frame ended in, and its length; -1 for none */
static int mb_rx_last(struct mb8795 *mp, unsigned int *len)
{
	u32 end;
	int i;

	if (mp->turbo)
		end = next_dma_read(NEXT_DMA_ENETR_TURBO_SAVED_LIMIT, 0);
	else
		end = next_dma_read(NEXT_DMA_ENETR_CSR, NEXT_DMA_SAVED_LIMIT);
	end &= MB_DMA_ADDR_MASK;
	for (i = 0; i < MB_RX_BUFS; i++) {
		dma_addr_t start = mb_buf_dma(mp, 1 + i);

		if (end > start && end <= start + MB_BUF_SIZE) {
			*len = end - start;
			return i;
		}
	}
	return -1;
}

/*
 * Take a frame whose end the DMA channel didn't keep, as when two arrive
 * before the interrupt is serviced, with its length from its own header.
 * Other kinds of frame are lost.
 */
static void mb_rx_earlier_frame(struct mb8795 *mp, int i)
{
	const u8 *buf = mb_rx_buf(mp, i);
	unsigned int len;

	switch (get_unaligned_be16(buf + 2 * ETH_ALEN)) {
	case ETH_P_IP:
		len = ETH_HLEN + get_unaligned_be16(buf + ETH_HLEN + 2);
		break;
	case ETH_P_IPV6:
		len = ETH_HLEN + sizeof(struct ipv6hdr) +
		      get_unaligned_be16(buf + ETH_HLEN + 4);
		break;
	case ETH_P_ARP:
		len = ETH_HLEN + sizeof(struct arphdr) + 2 * (ETH_ALEN + 4);
		break;
	default:
		len = 0;
		break;
	}
	if (len < ETH_HLEN || len > ETH_FRAME_LEN) {
		mp->dev->stats.rx_missed_errors++;
		return;
	}
	mb_rx_frame(mp, i, max_t(unsigned int, len, ETH_ZLEN) + MB_CRC_LEN);
}

static irqreturn_t mb_rx_interrupt(int irq, void *dev_id)
{
	struct mb8795 *mp = netdev_priv(dev_id);
	const unsigned int csr = NEXT_DMA_ENETR_CSR;
	u32 state = next_dma_read(csr, 0);
	unsigned int len;
	int i, j;

	if (!(state & NEXT_DMACSR_COMPLETE))
		return IRQ_NONE;

	if (state & NEXT_DMACSR_BUSEXC) {
		netdev_err(mp->dev, "receive DMA bus error\n");
		mp->dev->stats.rx_errors++;
		mb_rx_dma_start(mp);
		return IRQ_HANDLED;
	}

	i = mb_rx_last(mp, &len);
	if (i < 0) {
		mp->dev->stats.rx_errors++;
		mb_rx_dma_start(mp);
		return IRQ_HANDLED;
	}

	if (!(state & NEXT_DMACSR_ENABLE)) {
		/*
		 * Both buffers were used before we got here: the other one
		 * holds the earlier frame, whose end was not kept.
		 */
		mb_rx_earlier_frame(mp, !i);
		mb_rx_frame(mp, i, len);
		mb_rx_dma_start(mp);
		return IRQ_HANDLED;
	}
	mb_rx_frame(mp, i, len);

	/* Filling the other buffer: queue this one up behind it. */
	next_dma_write(csr, NEXT_DMA_START, mb_buf_dma(mp, 1 + i));
	next_dma_write(csr, NEXT_DMA_STOP, mb_buf_dma(mp, 1 + i) + MB_BUF_SIZE);
	next_dma_write(csr, 0, NEXT_DMACSR_SETSUPDATE | NEXT_DMACSR_CLRCOMPLETE |
			       NEXT_DMACSR_SETREAD);

	/*
	 * If the other buffer filled up before this one was queued, the
	 * channel stopped, and clearing "complete" just now hid that frame:
	 * take it, and start over.
	 */
	if (!(next_dma_read(csr, 0) & NEXT_DMACSR_ENABLE)) {
		j = mb_rx_last(mp, &len);
		if (j >= 0 && j != i)
			mb_rx_frame(mp, j, len);
		mb_rx_dma_start(mp);
	}
	return IRQ_HANDLED;
}

static irqreturn_t mb_tx_interrupt(int irq, void *dev_id)
{
	struct net_device *dev = dev_id;
	struct mb8795 *mp = netdev_priv(dev);
	u32 state = next_dma_read(NEXT_DMA_ENETX_CSR, 0);

	if (!(state & NEXT_DMACSR_COMPLETE))
		return IRQ_NONE;

	mb_dma_stop(NEXT_DMA_ENETX_CSR);
	if (mp->turbo)
		mb_write(mp, MB_TXMODE, mp->txmode);
	if (state & NEXT_DMACSR_BUSEXC) {
		netdev_err(dev, "transmit DMA bus error\n");
		dev->stats.tx_errors++;
	} else {
		dev->stats.tx_packets++;
	}
	netif_wake_queue(dev);
	return IRQ_HANDLED;
}

static netdev_tx_t mb_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct mb8795 *mp = netdev_priv(dev);
	const unsigned int csr = NEXT_DMA_ENETX_CSR;
	unsigned int len = max_t(unsigned int, skb->len, ETH_ZLEN);
	void *buf = mb_tx_buf(mp);

	if (skb->len > ETH_FRAME_LEN) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	netif_stop_queue(dev);

	skb_copy_bits(skb, 0, buf, skb->len);
	memset(buf + skb->len, 0, len - skb->len);
	dev->stats.tx_bytes += len;
	dev_consume_skb_any(skb);

	if (mp->turbo) {
		next_dma_write(csr, 0, NEXT_DMACSR_RESET |
				       NEXT_DMACSR_INITBUF_TURBO);
		next_dma_write(csr, NEXT_DMA_NEXT, mb_buf_dma(mp, 0));
		next_dma_write(csr, NEXT_DMA_LIMIT, mb_buf_dma(mp, 0) + len);
		next_dma_write(csr, 0, NEXT_DMACSR_SETENABLE);
		mb_write(mp, MB_TXMODE, mp->txmode | MB_TXMODE_TURBO_START);
		return NETDEV_TX_OK;
	}

	/*
	 * The limit has to mark the end of the frame and point 15 bytes
	 * past it; that is how the controller has always been driven.
	 */
	next_dma_write(csr, 0, NEXT_DMACSR_RESET | NEXT_DMACSR_INITBUF);
	next_dma_write(csr, NEXT_DMA_NEXT_INITBUF, mb_buf_dma(mp, 0));
	next_dma_write(csr, NEXT_DMA_LIMIT,
		       (mb_buf_dma(mp, 0) + len + 15) | NEXT_DMA_ENETX_EOP);
	next_dma_write(csr, 0, NEXT_DMACSR_SETENABLE);

	return NETDEV_TX_OK;
}

static void mb_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct mb8795 *mp = netdev_priv(dev);

	netdev_warn(dev, "transmit timed out\n");
	mb_dma_stop(NEXT_DMA_ENETX_CSR);
	if (mp->turbo)
		mb_write(mp, MB_TXMODE, mp->txmode);
	dev->stats.tx_errors++;
	netif_wake_queue(dev);
}

static int mb_open(struct net_device *dev)
{
	struct mb8795 *mp = netdev_priv(dev);
	int err;

	err = request_irq(mp->tx_irq, mb_tx_interrupt, 0, "mb8795 tx", dev);
	if (err)
		return err;
	err = request_irq(mp->rx_irq, mb_rx_interrupt, 0, "mb8795 rx", dev);
	if (err) {
		free_irq(mp->tx_irq, dev);
		return err;
	}

	mb_dma_stop(NEXT_DMA_ENETX_CSR);
	mb_reset_chip(mp);
	if (mp->turbo) {
		/* twisted pair if it has a link, else thin wire */
		mb_write(mp, MB_TXMODE, MB_TXMODE_TURBO_TPE);
		if (readb(mp->regs + MB_RESET) & MB_CTRL_TURBO_BADTPE)
			mp->txmode = 0;
		else
			mp->txmode = MB_TXMODE_TURBO_TPE;
		mb_write(mp, MB_TXMODE, mp->txmode);
	} else {
		mb_write(mp, MB_TXMODE, MB_TXMODE_LB_DISABLE);
	}
	mb_rx_dma_start(mp);
	mb_set_rx_mode(dev);

	netif_start_queue(dev);
	return 0;
}

static int mb_stop(struct net_device *dev)
{
	struct mb8795 *mp = netdev_priv(dev);

	netif_stop_queue(dev);
	mb_write(mp, MB_RXMODE, MB_RXMODE_OFF);
	mb_write(mp, MB_RESET, MB_RESET_MODE);
	mb_dma_stop(NEXT_DMA_ENETX_CSR);
	mb_dma_stop(NEXT_DMA_ENETR_CSR);
	free_irq(mp->rx_irq, dev);
	free_irq(mp->tx_irq, dev);
	return 0;
}

static const struct net_device_ops mb_netdev_ops = {
	.ndo_open		= mb_open,
	.ndo_stop		= mb_stop,
	.ndo_start_xmit		= mb_start_xmit,
	.ndo_tx_timeout		= mb_tx_timeout,
	.ndo_set_rx_mode	= mb_set_rx_mode,
	.ndo_set_mac_address	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

static int mb_probe(struct platform_device *pdev)
{
	const u8 *addr = dev_get_platdata(&pdev->dev);
	struct net_device *dev;
	struct mb8795 *mp;
	int err;

	dev = devm_alloc_etherdev(&pdev->dev, sizeof(*mp));
	if (!dev)
		return -ENOMEM;
	SET_NETDEV_DEV(dev, &pdev->dev);
	mp = netdev_priv(dev);
	mp->dev = dev;
	mp->turbo = next_is_turbo();

	mp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mp->regs))
		return PTR_ERR(mp->regs);
	mp->tx_irq = platform_get_irq(pdev, 0);
	if (mp->tx_irq < 0)
		return mp->tx_irq;
	mp->rx_irq = platform_get_irq(pdev, 1);
	if (mp->rx_irq < 0)
		return mp->rx_irq;

	err = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (err)
		return err;
	mp->buf = dmam_alloc_coherent(&pdev->dev,
				      (1 + MB_RX_BUFS) * MB_BUF_SIZE,
				      &mp->buf_dma, GFP_KERNEL);
	if (!mp->buf)
		return -ENOMEM;

	if (addr && is_valid_ether_addr(addr))
		eth_hw_addr_set(dev, addr);
	else
		eth_hw_addr_random(dev);

	dev->netdev_ops = &mb_netdev_ops;
	dev->watchdog_timeo = MB_TX_TIMEOUT;

	/* quiet until opened */
	mb_write(mp, MB_RESET, MB_RESET_MODE);

	err = devm_register_netdev(&pdev->dev, dev);
	if (err)
		return err;

	netdev_info(dev, "%s at %pR, %pM\n", mp->turbo ? "AT&T 7213" : "MB8795",
		    platform_get_resource(pdev, IORESOURCE_MEM, 0),
		    dev->dev_addr);
	return 0;
}

static struct platform_driver mb_driver = {
	.probe	= mb_probe,
	.driver	= {
		.name	= DRV_NAME,
	},
};
module_platform_driver(mb_driver);

MODULE_DESCRIPTION("NeXT MB8795 ethernet driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
