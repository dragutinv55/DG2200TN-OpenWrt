// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM63138 Runner data-path discovery driver
 */

#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/inetdevice.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/reset/bcm63xx_pmb.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "bcm63138-rdp-vendor/mainline_audit.h"

#define RDP_RUNNER_0_GLOBAL_CTRL	0x99000
#define RDP_RUNNER_1_GLOBAL_CTRL	0x9a000
#define RDP_RUNNER_1_CPU_WAKEUP		0x9a004
#define RDP_RUNNER_1_PICO_PROFILING	0x9a1b0
#define RDP_RUNNER_0_MAIN_INST		0x20000
#define RDP_RUNNER_1_MAIN_INST		0x60000
#define RDP_RUNNER_0_PICO_INST		0x30000
#define RDP_RUNNER_1_PICO_INST		0x70000
#define RDP_RUNNER_1_MAIN_PRED		0x6c000
#define RDP_RUNNER_0_PICO_PRED		0x3c000
#define RDP_RUNNER_1_PICO_PRED		0x7c000
#define RDP_RING_DESCRIPTORS_TABLE	0x07500
#define RDP_RUNNER_PRIVATE_0		0x10000
#define RDP_RUNNER_COMMON_1		0x40000
#define RDP_RUNNER_PRIVATE_1		0x50000
#define RDP_DS_WAN_FLOW_TABLE		0x07600
#define RDP_DS_CONNECTION_CONFIG	0x0bc78
#define RDP_DS_CONTEXT_CONFIG		0x0bc7c
#define RDP_US_CONNECTION_CONFIG	0x0a844
#define RDP_US_CONTEXT_CONFIG		0x0a848
#define RDP_DS_BRIDGE_CONFIG		0x0ab00
#define RDP_IPV4_HOST_TABLE		0x019e0
#define RDP_US_CPU_RX_PICO_QUEUE	0x0a400
#define RDP_US_CPU_RX_FAST_QUEUE	0x0a500
#define RDP_US_FORWARDING_MATRIX	0x08a00
#define RDP_US_BRIDGE_CONFIG		0x08b00
#define RDP_ETH1_RX_DESCRIPTORS		0x0b200
#define RDP_US_FLOW_IH_RESPONSE		0x0b6f0
#define RDP_LAN_INGRESS_FIFO_TABLE	0x0a440
#define RDP_SWITCH_TO_BRIDGE_TABLE	0x0a860
#define RDP_CPU_REASON_TO_QUEUE		0x00280
#define RDP_RUNNER_PICO_1_CONTEXT	0x78000
#define RDP_LAN0_THREAD_CONTEXT		(RDP_RUNNER_PICO_1_CONTEXT + 8 * 32 * 4)
#define RDP_LAN1_THREAD_CONTEXT		(RDP_RUNNER_PICO_1_CONTEXT + 9 * 32 * 4)
#define RDP_RUNNER_MAIN_0_CONTEXT	0x28000
#define RDP_WAN1_THREAD_CONTEXT		(RDP_RUNNER_MAIN_0_CONTEXT + 8 * 32 * 4)
#define RDP_RUNNER_MAIN_1_CONTEXT	0x68000
#define RDP_CPU_RX_THREAD_CONTEXT	(RDP_RUNNER_MAIN_1_CONTEXT + 32 * 4)
#define BCM63138_RUNNER_IRQS		10
#define BCM63138_RDP_TM_SIZE		(19 * SZ_1M)
#define BCM63138_RDP_MC_SIZE		SZ_4M
#define BCM63138_RDP_HEADROOM_SIZE	40
#define BCM63138_CPU_RX_RING_ENTRIES	32
#define BCM63138_CPU_RX_NAPI_WEIGHT	(2 * BCM63138_CPU_RX_RING_ENTRIES)
#define BCM63138_CPU_RX_BUFFER_SIZE	SZ_2K
#define BCM63138_CPU_RX_DESC_SIZE	16
#define BCM63138_CPU_RX_CSUM_VERIFIED	BIT(19)
#define BCM63138_CPU_RX_BUFFER_PTR_MASK	GENMASK(28, 0)
#define BCM63138_UNIMAC0_BASE		0x0d4000
#define BCM63138_UNIMAC1_BASE		0x0d5000
#define BCM63138_UNIMAC_HD_BKP		0x04
#define BCM63138_UNIMAC_CMD		0x08
#define BCM63138_UNIMAC_FRAME_LEN	0x14
#define BCM63138_UNIMAC_PAUSE_QUANTA	0x18
#define BCM63138_UNIMAC_MODE		0x44
#define BCM63138_UNIMAC_MISC_CFG		0x0db800
#define BCM63138_UNIMAC_MISC_EXT_CFG1	0x0db804
#define BCM63138_UNIMAC_MISC_EXT_CFG2	0x0db808
#define BCM63138_UNIMAC_MISC_STRIDE	0x400
#define BCM63138_UNIMAC_CMD_TX_EN	BIT(0)
#define BCM63138_UNIMAC_CMD_RX_EN	BIT(1)
#define BCM63138_UNIMAC_CMD_SPEED_MASK	GENMASK(3, 2)
#define BCM63138_UNIMAC_CMD_HD_EN	BIT(10)
#define BCM63138_RDP_EMAC0		0
#define BCM63138_RDP_EMAC1		1

#define BCM63138_FW_RUNNER_B		"brcm/bcm63138/runner-b.bin"
#define BCM63138_FW_RUNNER_C		"brcm/bcm63138/runner-c.bin"
#define BCM63138_FW_RUNNER_D		"brcm/bcm63138/runner-d.bin"
#define BCM63138_FW_PREDICT_B		"brcm/bcm63138/runner-b-predict.bin"
#define BCM63138_FW_PREDICT_C		"brcm/bcm63138/runner-c-predict.bin"
#define BCM63138_FW_PREDICT_D		"brcm/bcm63138/runner-d-predict.bin"

struct bcm63138_rdp {
	struct device *dev;
	void __iomem *base;
	void *tm_base;
	dma_addr_t tm_dma;
	void *mc_base;
	dma_addr_t mc_dma;
	struct mutex init_lock;
	bool data_path_init_attempted;
	bool data_path_allocated;
	bool data_path_initialized;
	dma_addr_t auxiliary_dma[2];
	size_t auxiliary_size[2];
	unsigned int auxiliary_count;
	void *cpu_rx_ring;
	dma_addr_t cpu_rx_ring_dma;
	void *cpu_rx_buffers[BCM63138_CPU_RX_RING_ENTRIES];
	dma_addr_t cpu_rx_buffers_dma[BCM63138_CPU_RX_RING_ENTRIES];
	void *cpu_rx_wan_ring;
	dma_addr_t cpu_rx_wan_ring_dma;
	void *cpu_rx_wan_buffers[BCM63138_CPU_RX_RING_ENTRIES];
	dma_addr_t cpu_rx_wan_buffers_dma[BCM63138_CPU_RX_RING_ENTRIES];
	bool cpu_rx_ring_setup_attempted;
	bool cpu_rx_ring_ready;
	spinlock_t cpu_rx_lock;
	unsigned int cpu_rx_head;
	unsigned int cpu_rx_wan_head;
	u64 cpu_rx_packets;
	u64 cpu_rx_bytes;
	u64 cpu_rx_errors;
	u64 cpu_rx_checksum_verified;
	u64 cpu_rx_checksum_unverified;
	u64 cpu_rx_gro_results[GRO_CONSUMED + 1];
	u32 cpu_rx_last_word[4];
	struct napi_struct napi;
	bool napi_added;
	bool napi_enabled;
	bool rx_coalescing_enabled;
	u16 rx_coalescing_timeout_us;
	u8 rx_coalescing_max_packets;
	struct delayed_work cpu_rx_work;
	bool cpu_rx_polling;
	bool shutting_down;
	bool runner_execution_enabled;
	bool wan_tx_base_initialized;
	bool wan_tx_initialized;
	u32 ethernet_source_mask;
	bool unimac0_configured;
	u32 unimac0_saved[8];
	bool unimac1_configured;
	u32 unimac1_saved[8];
	bool broadcast_trap_enabled;
	bool cpu_irq_setup_attempted;
	bool cpu_irq_registered;
	bool cpu_irq_enabled;
	bool cpu_irq_fault_pending;
	unsigned int cpu_irq_empty_count;
	atomic_t cpu_irq_count;
	struct work_struct cpu_irq_fault_work;
	struct notifier_block inetaddr_nb;
	__be32 wan_ipv4_address;
	int irq[BCM63138_RUNNER_IRQS];
	struct net_device *netdev[2];
	struct phy_device *wan_phy;
};

struct bcm63138_rdp_netdev {
	struct bcm63138_rdp *rdp;
	u8 emac_id;
};

void __iomem *bcm63138_rdp_io_base;
static struct bcm63138_rdp *bcm63138_rdp_active;

static void bcm63138_rdp_clear_globals(struct bcm63138_rdp *rdp)
{
	if (bcm63138_rdp_active == rdp)
		bcm63138_rdp_active = NULL;
	if (bcm63138_rdp_io_base == rdp->base)
		bcm63138_rdp_io_base = NULL;
}

static void bcm63138_rdp_reserved_mem_release(void *data)
{
	of_reserved_mem_device_release(data);
}

struct bcm63138_dpi_cfg {
	u32 mtu_size;
	u32 headroom_size;
	u32 runner_freq;
	u32 runner_tm_base_addr;
	u32 runner_tm_base_addr_phys;
	u32 runner_tm_size;
	u32 runner_mc_base_addr;
	u32 runner_mc_base_addr_phys;
};

extern u32 data_path_init(void *config);
void __iomem *bcm63138_rdp_device_address(uintptr_t address);
u32 bcm63138_rdp_virt_to_dma(u32 address);
void *bcm63138_rdp_dma_alloc(size_t size, dma_addr_t *dma, gfp_t gfp);
void bcm63138_rdp_dma_free(size_t size, void *cpu_addr, dma_addr_t dma);

static void bcm63138_rdp_init_ubus_masters(struct bcm63138_rdp *rdp)
{
	static const u32 hp[] = {
		0x01010200,
		0x010b0d00,
		0x05010600,
	};
	unsigned int i, port;

	for (i = 0; i < ARRAY_SIZE(hp); i++) {
		void __iomem *master = rdp->base + 0xd2000 + i * 0x400;

		for (port = 0; port < 8; port++) {
			void __iomem *bridge = master + port * 0x20;

			iowrite32(0x00000190, bridge + 0x04);
			iowrite32(0x0c000c00, bridge + 0x08);
			iowrite32(hp[i], bridge + 0x0c);
			iowrite32(0x0000ffff, bridge + 0x10);
			iowrite32(0x01000000, bridge + 0x00);
		}
	}
}

void __iomem *bcm63138_rdp_device_address(uintptr_t address)
{
	uintptr_t mapped = (uintptr_t)bcm63138_rdp_io_base;

	if (address >= mapped && address < mapped + SZ_1M)
		return (void __iomem *)address;

	if (address >= 0x80200000 && address < 0x80300000)
		return bcm63138_rdp_io_base + address - 0x80200000;

	WARN_ONCE(1, "BCM63138 RDP: invalid register address 0x%08lx\n",
		  (unsigned long)address);
	return NULL;
}

void *bcm63138_rdp_dma_alloc(size_t size, dma_addr_t *dma, gfp_t gfp)
{
	struct bcm63138_rdp *rdp = bcm63138_rdp_active;

	if (!rdp)
		return NULL;

	{
		void *cpu_addr = dma_alloc_coherent(rdp->dev, size, dma, gfp);

		if (cpu_addr && rdp->auxiliary_count < ARRAY_SIZE(rdp->auxiliary_dma)) {
			rdp->auxiliary_dma[rdp->auxiliary_count] = *dma;
			rdp->auxiliary_size[rdp->auxiliary_count] = size;
			rdp->auxiliary_count++;
		}

		return cpu_addr;
	}
}

void bcm63138_rdp_dma_free(size_t size, void *cpu_addr, dma_addr_t dma)
{
	struct bcm63138_rdp *rdp = bcm63138_rdp_active;

	if (rdp)
		dma_free_coherent(rdp->dev, size, cpu_addr, dma);
}

u32 bcm63138_rdp_virt_to_dma(u32 address)
{
	struct bcm63138_rdp *rdp = bcm63138_rdp_active;
	u32 tm_base;
	u32 mc_base;

	if (!rdp)
		return 0;

	tm_base = (u32)(uintptr_t)rdp->tm_base;
	if (address >= tm_base &&
	    address - tm_base < BCM63138_RDP_TM_SIZE)
		return (u32)rdp->tm_dma + address - tm_base;

	mc_base = (u32)(uintptr_t)rdp->mc_base;
	if (address >= mc_base &&
	    address - mc_base < BCM63138_RDP_MC_SIZE)
		return (u32)rdp->mc_dma + address - mc_base;

	WARN_ONCE(1, "BCM63138 RDP: unmapped DMA address 0x%08x\n",
		  address);
	return 0;
}

static ssize_t data_path_init_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	struct bcm63138_dpi_cfg config;
	bool initialize;
	int err;

	err = kstrtobool(buf, &initialize);
	if (err)
		return err;
	if (!initialize)
		return -EINVAL;

	mutex_lock(&rdp->init_lock);
	if (rdp->data_path_init_attempted) {
		err = -EALREADY;
		goto out_unlock;
	}
	rdp->data_path_init_attempted = true;

	memset(rdp->tm_base, 0, BCM63138_RDP_TM_SIZE);
	memset(rdp->mc_base, 0, BCM63138_RDP_MC_SIZE);

	config.mtu_size = 1536;
	config.headroom_size = BCM63138_RDP_HEADROOM_SIZE;
	config.runner_freq = 800;
	config.runner_tm_base_addr = (u32)(uintptr_t)rdp->tm_base;
	config.runner_tm_base_addr_phys = (u32)rdp->tm_dma;
	config.runner_tm_size = BCM63138_RDP_TM_SIZE / SZ_1M;
	config.runner_mc_base_addr = (u32)(uintptr_t)rdp->mc_base;
	config.runner_mc_base_addr_phys = (u32)rdp->mc_dma;

	bcm63138_rdp_init_ubus_masters(rdp);
	err = data_path_init(&config);
	if (err) {
		dev_err(dev, "RDD basic data-path initialization failed: %d\n",
			err);
		err = -EIO;
		goto out_unlock;
	}
	rdp->data_path_allocated = true;
	bcm63138_rdp_vendor_locking_init();
	err = bcm63138_rdp_vendor_connection_tables_init();
	if (err) {
		dev_err(dev,
			"RDD connection-table initialization failed: %d\n",
			err);
		err = -EIO;
		goto out_unlock;
	}
	err = bcm63138_rdp_vendor_wan_tx_base_init();
	if (err) {
		dev_err(dev, "RDD pre-Runner WAN initialization failed: %d\n",
			err);
		err = -EIO;
		goto out_unlock;
	}
	rdp->wan_tx_base_initialized = true;
	rdp->data_path_initialized = true;
	dev_info(dev,
		 "RDD basic data path and pre-Runner WAN state initialized; Runner execution and WAN scheduler remain disabled\n");
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(data_path_init);

static ssize_t cfe_state_adopt_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool adopt;
	int err;

	err = kstrtobool(buf, &adopt);
	if (err)
		return err;
	if (!adopt)
		return -EINVAL;

	mutex_lock(&rdp->init_lock);
	if (rdp->data_path_init_attempted) {
		err = -EALREADY;
		goto out_unlock;
	}

	rdp->data_path_init_attempted = true;
	rdp->data_path_initialized = true;
	rdp->runner_execution_enabled = true;
	rdp->unimac1_configured = true;
	dev_info(dev, "adopted the active CFE RDP and UniMAC state\n");
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(cfe_state_adopt);

static void bcm63138_rdp_cpu_rx_buffers_free(struct bcm63138_rdp *rdp,
					     void **buffers,
					     dma_addr_t *buffers_dma)
{
	unsigned int i;

	for (i = 0; i < BCM63138_CPU_RX_RING_ENTRIES; i++) {
		if (!buffers[i])
			continue;

		dma_unmap_single(rdp->dev, buffers_dma[i],
				 BCM63138_CPU_RX_BUFFER_SIZE,
				 DMA_FROM_DEVICE);
		kfree(buffers[i]);
		buffers[i] = NULL;
		buffers_dma[i] = 0;
	}
}

static void bcm63138_rdp_cpu_rx_rings_free(struct bcm63138_rdp *rdp)
{
	bcm63138_rdp_cpu_rx_buffers_free(
		rdp, rdp->cpu_rx_wan_buffers, rdp->cpu_rx_wan_buffers_dma);
	if (rdp->cpu_rx_wan_ring) {
		dma_free_coherent(
			rdp->dev,
			BCM63138_CPU_RX_RING_ENTRIES *
				BCM63138_CPU_RX_DESC_SIZE,
			rdp->cpu_rx_wan_ring, rdp->cpu_rx_wan_ring_dma);
		rdp->cpu_rx_wan_ring = NULL;
		rdp->cpu_rx_wan_ring_dma = 0;
	}

	bcm63138_rdp_cpu_rx_buffers_free(
		rdp, rdp->cpu_rx_buffers, rdp->cpu_rx_buffers_dma);
	if (rdp->cpu_rx_ring) {
		dma_free_coherent(
			rdp->dev,
			BCM63138_CPU_RX_RING_ENTRIES *
				BCM63138_CPU_RX_DESC_SIZE,
			rdp->cpu_rx_ring, rdp->cpu_rx_ring_dma);
		rdp->cpu_rx_ring = NULL;
		rdp->cpu_rx_ring_dma = 0;
	}

	rdp->cpu_rx_ring_ready = false;
}

static int bcm63138_rdp_cpu_rx_buffers_alloc(struct bcm63138_rdp *rdp,
					      void **buffers,
					      dma_addr_t *buffers_dma,
					      const char *ring_name)
{
	unsigned int i;
	int err;

	for (i = 0; i < BCM63138_CPU_RX_RING_ENTRIES; i++) {
		dma_addr_t dma;
		void *buffer;

		buffer = kzalloc(BCM63138_CPU_RX_BUFFER_SIZE, GFP_KERNEL);
		if (!buffer) {
			err = -ENOMEM;
			goto free_buffers;
		}

		dma = dma_map_single(rdp->dev, buffer,
				     BCM63138_CPU_RX_BUFFER_SIZE,
				     DMA_FROM_DEVICE);
		if (dma_mapping_error(rdp->dev, dma)) {
			kfree(buffer);
			err = -EIO;
			goto free_buffers;
		}
		if (dma > BCM63138_CPU_RX_BUFFER_PTR_MASK ||
		    BCM63138_CPU_RX_BUFFER_SIZE - 1 >
		    BCM63138_CPU_RX_BUFFER_PTR_MASK - dma) {
			dev_err(rdp->dev,
				"%s CPU RX buffer %u DMA address %pad exceeds the 29-bit Runner limit\n",
				ring_name, i, &dma);
			dma_unmap_single(rdp->dev, dma,
					 BCM63138_CPU_RX_BUFFER_SIZE,
					 DMA_FROM_DEVICE);
			kfree(buffer);
			err = -ERANGE;
			goto free_buffers;
		}

		buffers[i] = buffer;
		buffers_dma[i] = dma;
	}

	return 0;

free_buffers:
	bcm63138_rdp_cpu_rx_buffers_free(rdp, buffers, buffers_dma);
	return err;
}

static ssize_t cpu_ring_setup_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	__be32 *descriptor;
	bool setup;
	unsigned int i;
	int err;

	err = kstrtobool(buf, &setup);
	if (err)
		return err;
	if (!setup)
		return -EINVAL;

	mutex_lock(&rdp->init_lock);
	if (!rdp->data_path_initialized) {
		err = -EAGAIN;
		goto out_unlock;
	}
	if (rdp->cpu_rx_ring_setup_attempted) {
		err = -EALREADY;
		goto out_unlock;
	}
	rdp->cpu_rx_ring_setup_attempted = true;

	rdp->cpu_rx_ring = dma_alloc_coherent(
		rdp->dev,
		BCM63138_CPU_RX_RING_ENTRIES * BCM63138_CPU_RX_DESC_SIZE,
		&rdp->cpu_rx_ring_dma, GFP_KERNEL);
	if (!rdp->cpu_rx_ring) {
		err = -ENOMEM;
		goto out_unlock;
	}

	err = bcm63138_rdp_cpu_rx_buffers_alloc(
		rdp, rdp->cpu_rx_buffers, rdp->cpu_rx_buffers_dma, "LAN");
	if (err)
		goto free_lan_ring;

	rdp->cpu_rx_wan_ring = dma_alloc_coherent(
		rdp->dev,
		BCM63138_CPU_RX_RING_ENTRIES * BCM63138_CPU_RX_DESC_SIZE,
		&rdp->cpu_rx_wan_ring_dma, GFP_KERNEL);
	if (!rdp->cpu_rx_wan_ring) {
		err = -ENOMEM;
		goto free_lan_buffers;
	}

	err = bcm63138_rdp_cpu_rx_buffers_alloc(
		rdp, rdp->cpu_rx_wan_buffers,
		rdp->cpu_rx_wan_buffers_dma, "WAN");
	if (err)
		goto free_wan_ring;

	memset(rdp->cpu_rx_ring, 0,
	       BCM63138_CPU_RX_RING_ENTRIES * BCM63138_CPU_RX_DESC_SIZE);
	memset(rdp->cpu_rx_wan_ring, 0,
	       BCM63138_CPU_RX_RING_ENTRIES * BCM63138_CPU_RX_DESC_SIZE);

	for (i = 0; i < BCM63138_CPU_RX_RING_ENTRIES; i++) {
		descriptor = rdp->cpu_rx_ring +
			     i * BCM63138_CPU_RX_DESC_SIZE;
		descriptor[2] =
			cpu_to_be32((u32)rdp->cpu_rx_buffers_dma[i]);

		descriptor = rdp->cpu_rx_wan_ring +
			     i * BCM63138_CPU_RX_DESC_SIZE;
		descriptor[2] =
			cpu_to_be32((u32)rdp->cpu_rx_wan_buffers_dma[i]);
	}

	err = bcm63138_rdp_vendor_ring_init(
		0, (u32)rdp->cpu_rx_ring_dma,
		BCM63138_CPU_RX_RING_ENTRIES,
		BCM63138_CPU_RX_DESC_SIZE, 0);
	if (err) {
		dev_err(dev, "RDD LAN CPU RX ring initialization failed: %d\n",
			err);
		err = -EIO;
		goto free_wan_buffers;
	}

	err = bcm63138_rdp_vendor_ring_init(
		1, (u32)rdp->cpu_rx_wan_ring_dma,
		BCM63138_CPU_RX_RING_ENTRIES,
		BCM63138_CPU_RX_DESC_SIZE, 0);
	if (err) {
		dev_err(dev, "RDD WAN CPU RX ring initialization failed: %d\n",
			err);
		err = -EIO;
		goto free_wan_buffers;
	}

	err = bcm63138_rdp_vendor_split_cpu_rx_queues();
	if (err) {
		dev_err(dev, "RDD CPU RX queue split failed: %d\n", err);
		err = -EIO;
		goto free_wan_buffers;
	}

	rdp->cpu_rx_ring_ready = true;
	dev_info(dev,
		 "CPU RX rings prepared with streaming DMA buffers: LAN=%pad/%pad WAN=%pad/%pad; IRQ remains disabled\n",
		 &rdp->cpu_rx_ring_dma, &rdp->cpu_rx_buffers_dma[0],
		 &rdp->cpu_rx_wan_ring_dma,
		 &rdp->cpu_rx_wan_buffers_dma[0]);
	err = count;
	goto out_unlock;

free_wan_buffers:
	bcm63138_rdp_cpu_rx_buffers_free(
		rdp, rdp->cpu_rx_wan_buffers, rdp->cpu_rx_wan_buffers_dma);
free_wan_ring:
	dma_free_coherent(
		rdp->dev,
		BCM63138_CPU_RX_RING_ENTRIES * BCM63138_CPU_RX_DESC_SIZE,
		rdp->cpu_rx_wan_ring, rdp->cpu_rx_wan_ring_dma);
	rdp->cpu_rx_wan_ring = NULL;
free_lan_buffers:
	bcm63138_rdp_cpu_rx_buffers_free(
		rdp, rdp->cpu_rx_buffers, rdp->cpu_rx_buffers_dma);
free_lan_ring:
	dma_free_coherent(
		rdp->dev,
		BCM63138_CPU_RX_RING_ENTRIES * BCM63138_CPU_RX_DESC_SIZE,
		rdp->cpu_rx_ring, rdp->cpu_rx_ring_dma);
	rdp->cpu_rx_ring = NULL;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(cpu_ring_setup);

static ssize_t cpu_ring_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	const __be32 *descriptor;
	ssize_t len = 0;
	unsigned int ring_id;
	unsigned int i;

	mutex_lock(&rdp->init_lock);
	if (!rdp->cpu_rx_ring_ready) {
		len = -EAGAIN;
		goto out_unlock;
	}

	dma_rmb();
	for (ring_id = 0; ring_id < 2; ring_id++) {
		const void *ring = ring_id ? rdp->cpu_rx_wan_ring :
					    rdp->cpu_rx_ring;
		unsigned int host_owned = 0;

		for (i = 0; i < BCM63138_CPU_RX_RING_ENTRIES; i++) {
			u32 word0;
			u32 word1;
			u32 word2;
			u32 word3;

			descriptor = ring +
				     i * BCM63138_CPU_RX_DESC_SIZE;
			word0 = be32_to_cpu(READ_ONCE(descriptor[0]));
			word1 = be32_to_cpu(READ_ONCE(descriptor[1]));
			word2 = be32_to_cpu(READ_ONCE(descriptor[2]));
			word3 = be32_to_cpu(READ_ONCE(descriptor[3]));

			if (word2 & BIT(31)) {
				host_owned++;
				len += sysfs_emit_at(
					buf, len,
					"ring%u_entry%u host=1 len=%u source=%u flow=%u reason=%u buffer=0x%08x words=%08x/%08x/%08x/%08x\n",
					ring_id, i, word0 & 0x3fff,
					(word0 >> 14) & 0x1f,
					(word0 >> 20) & 0xfff,
					(word1 >> 25) & 0x3f,
					(u32)(word2 &
					      BCM63138_CPU_RX_BUFFER_PTR_MASK),
					word0, word1, word2, word3);
			}
		}

		len += sysfs_emit_at(buf, len,
				     "ring%u entries=%u host_owned=%u\n",
				     ring_id, BCM63138_CPU_RX_RING_ENTRIES,
				     host_owned);
	}

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return len;
}
static DEVICE_ATTR_RO(cpu_ring_state);

static ssize_t pipeline_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	static const struct {
		const char *name;
		u32 offset;
		unsigned int words;
	} regions[] = {
		{ "ring_table", RDP_RING_DESCRIPTORS_TABLE, 4 },
		{ "cpu_reason_to_queue", RDP_CPU_REASON_TO_QUEUE, 32 },
		{ "ds_wan_flow0", RDP_RUNNER_PRIVATE_0 +
				  RDP_DS_WAN_FLOW_TABLE, 1 },
		{ "ds_wan_flow238_239", RDP_RUNNER_PRIVATE_0 +
					 RDP_DS_WAN_FLOW_TABLE + 238 * 2, 1 },
		{ "ds_connection_config", RDP_RUNNER_PRIVATE_0 +
					  RDP_DS_CONNECTION_CONFIG, 1 },
		{ "ds_context_config", RDP_RUNNER_PRIVATE_0 +
				       RDP_DS_CONTEXT_CONFIG, 1 },
		{ "us_connection_config", RDP_RUNNER_PRIVATE_1 +
					  RDP_US_CONNECTION_CONFIG, 1 },
		{ "us_context_config", RDP_RUNNER_PRIVATE_1 +
				       RDP_US_CONTEXT_CONFIG, 1 },
		{ "ds_bridge_config", RDP_RUNNER_PRIVATE_0 +
				      RDP_DS_BRIDGE_CONFIG, 8 },
		{ "ipv4_host_table", RDP_RUNNER_COMMON_1 +
				     RDP_IPV4_HOST_TABLE, 8 },
		{ "cpu_rx_pico", RDP_RUNNER_PRIVATE_1 +
				 RDP_US_CPU_RX_PICO_QUEUE, 16 },
		{ "cpu_rx_fast", RDP_RUNNER_PRIVATE_1 +
				 RDP_US_CPU_RX_FAST_QUEUE, 16 },
		{ "forwarding_matrix", RDP_RUNNER_PRIVATE_1 +
				       RDP_US_FORWARDING_MATRIX, 36 },
		{ "bridge_config", RDP_RUNNER_PRIVATE_1 +
				   RDP_US_BRIDGE_CONFIG, 64 },
		{ "eth1_rx", RDP_RUNNER_PRIVATE_1 +
			    RDP_ETH1_RX_DESCRIPTORS, 16 },
		{ "flow_ih_response", RDP_RUNNER_PRIVATE_1 +
				    RDP_US_FLOW_IH_RESPONSE, 2 },
		{ "lan_ingress_fifo", RDP_RUNNER_PRIVATE_1 +
				      RDP_LAN_INGRESS_FIFO_TABLE, 9 },
		{ "switch_to_bridge", RDP_RUNNER_PRIVATE_1 +
				       RDP_SWITCH_TO_BRIDGE_TABLE, 2 },
		{ "lan0_thread_context", RDP_LAN0_THREAD_CONTEXT, 32 },
		{ "lan1_thread_context", RDP_LAN1_THREAD_CONTEXT, 32 },
		{ "wan1_thread_context", RDP_WAN1_THREAD_CONTEXT, 32 },
		{ "cpu_rx_thread_context", RDP_CPU_RX_THREAD_CONTEXT, 32 },
	};
	ssize_t len = 0;
	unsigned int i;

	mutex_lock(&rdp->init_lock);
	if (!rdp->data_path_initialized) {
		len = -EAGAIN;
		goto out_unlock;
	}

	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		unsigned int word;

		len += sysfs_emit_at(buf, len, "%s=", regions[i].name);
		for (word = 0; word < regions[i].words; word++)
			len += sysfs_emit_at(
				buf, len, "%s%08x", word ? "/" : "",
				ioread32be(rdp->base + regions[i].offset +
					   word * sizeof(u32)));
		len += sysfs_emit_at(buf, len, "\n");
	}

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return len;
}
static DEVICE_ATTR_RO(pipeline_state);

static ssize_t eth1_packet_state_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	const u8 *packet;
	u32 word0;
	u32 word1;
	u32 buffer_number;
	u32 length;
	ssize_t len;
	static const unsigned int strides[] = { 2048, 2560, 4096 };
	const u8 *tm = rdp->tm_base;
	size_t packet_pool_size = min_t(size_t, BCM63138_RDP_TM_SIZE,
					5120 * 2560);
	size_t first_nonzero = packet_pool_size;
	unsigned int i;
	unsigned int stride;

	word0 = ioread32be(rdp->base + RDP_RUNNER_PRIVATE_1 +
			   RDP_ETH1_RX_DESCRIPTORS);
	word1 = ioread32be(rdp->base + RDP_RUNNER_PRIVATE_1 +
			   RDP_ETH1_RX_DESCRIPTORS + 4);
	length = word0 & 0x3fff;
	buffer_number = word1 & 0x7fff;
	len = sysfs_emit(buf, "descriptor=%08x/%08x length=%u buffer=%u\n",
			 word0, word1, length, buffer_number);

	if (!length)
		return len;

	for (stride = 0; stride < ARRAY_SIZE(strides); stride++) {
		if (buffer_number >= BCM63138_RDP_TM_SIZE / strides[stride])
			continue;

		packet = rdp->tm_base + buffer_number * strides[stride];
		len += sysfs_emit_at(buf, len, "data%u=", strides[stride]);
		for (i = 0; i < 64; i++)
			len += sysfs_emit_at(buf, len, "%02x",
					     READ_ONCE(packet[i]));
		len += sysfs_emit_at(buf, len, "\n");
	}

	for (i = 0; i < packet_pool_size; i++) {
		if (READ_ONCE(tm[i])) {
			first_nonzero = i;
			break;
		}
	}
	len += sysfs_emit_at(buf, len, "tm_first_nonzero=%#zx\n",
			     first_nonzero);

	return len;
}
static DEVICE_ATTR_RO(eth1_packet_state);

static ssize_t lan1_fifo_init_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool wake;
	int err;

	err = kstrtobool(buf, &wake);
	if (err)
		return err;
	if (!wake)
		return -EINVAL;

	mutex_lock(&rdp->init_lock);
	if (!rdp->runner_execution_enabled) {
		err = -EAGAIN;
		goto out_unlock;
	}

	err = bcm63138_rdp_vendor_lan1_fifo_init();
	if (err) {
		err = -EIO;
		goto out_unlock;
	}
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(lan1_fifo_init);

static ssize_t wan1_fifo_init_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool wake;
	int err;

	err = kstrtobool(buf, &wake);
	if (err)
		return err;
	if (!wake)
		return -EINVAL;

	mutex_lock(&rdp->init_lock);
	if (!rdp->runner_execution_enabled) {
		err = -EAGAIN;
		goto out_unlock;
	}

	err = bcm63138_rdp_vendor_wan1_fifo_init();
	if (err) {
		err = -EIO;
		goto out_unlock;
	}
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(wan1_fifo_init);

static ssize_t cpu_rx_wakeup_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool wake;
	int err;

	err = kstrtobool(buf, &wake);
	if (err)
		return err;
	if (!wake)
		return -EINVAL;
	if (!rdp->runner_execution_enabled)
		return -EAGAIN;

	bcm63138_rdp_vendor_cpu_rx_wakeup();

	return count;
}
static DEVICE_ATTR_WO(cpu_rx_wakeup);

static ssize_t bcm63138_rdp_cpu_tx_test(const char *buf, size_t count,
				       u8 emac_id, bool absolute)
{
	static const u8 packet[98] = {
		[0 ... 5] = 0xff,
		[6] = 0x98, [7] = 0x77, [8] = 0xe7,
		[9] = 0x45, [10] = 0x62, [11] = 0xde,
		[12] = 0x88, [13] = 0xb5,
		[14] = 'B', [15] = 'C', [16] = 'M', [17] = '6',
		[18] = '3', [19] = '1', [20] = '3', [21] = '8',
	};
	bool send;
	int err;

	err = kstrtobool(buf, &send);
	if (err)
		return err;
	if (!send)
		return -EINVAL;

	if (absolute)
		err = bcm63138_rdp_vendor_wan_cpu_tx_abs(packet,
						       sizeof(packet));
	else
		err = bcm63138_rdp_vendor_cpu_tx(packet, sizeof(packet),
					       emac_id);
	if (err)
		return -EIO;

	return count;
}

static ssize_t cpu_tx_test_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	return bcm63138_rdp_cpu_tx_test(buf, count, BCM63138_RDP_EMAC1, false);
}
static DEVICE_ATTR_WO(cpu_tx_test);

static ssize_t wan_cpu_tx_test_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	return bcm63138_rdp_cpu_tx_test(buf, count, BCM63138_RDP_EMAC0, false);
}
static DEVICE_ATTR_WO(wan_cpu_tx_test);

static ssize_t wan_cpu_tx_abs_test_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	return bcm63138_rdp_cpu_tx_test(buf, count, BCM63138_RDP_EMAC0, true);
}
static DEVICE_ATTR_WO(wan_cpu_tx_abs_test);

static unsigned int bcm63138_rdp_cpu_rx_drain(struct bcm63138_rdp *rdp,
					      unsigned int ring_id,
					      unsigned int budget,
					      struct napi_struct *napi)
{
	struct sk_buff_head received;
	struct net_device *netdev;
	dma_addr_t *buffers_dma;
	unsigned int *head;
	void **buffers;
	void *ring;
	unsigned int completed = 0;

	__skb_queue_head_init(&received);
	if (ring_id == 1) {
		ring = rdp->cpu_rx_wan_ring;
		buffers = rdp->cpu_rx_wan_buffers;
		buffers_dma = rdp->cpu_rx_wan_buffers_dma;
		head = &rdp->cpu_rx_wan_head;
		netdev = rdp->netdev[BCM63138_RDP_EMAC0];
	} else {
		ring = rdp->cpu_rx_ring;
		buffers = rdp->cpu_rx_buffers;
		buffers_dma = rdp->cpu_rx_buffers_dma;
		head = &rdp->cpu_rx_head;
		netdev = rdp->netdev[BCM63138_RDP_EMAC1];
	}

	spin_lock_bh(&rdp->cpu_rx_lock);
	while (completed < budget) {
		__be32 *descriptor;
		dma_addr_t expected_dma;
		u32 word0;
		u32 word1;
		u32 word2;
		u32 word3;
		u32 length;

		descriptor = ring + *head * BCM63138_CPU_RX_DESC_SIZE;
		word2 = be32_to_cpu(READ_ONCE(descriptor[2]));
		if (!(word2 & BIT(31)))
			break;

		dma_rmb();
		word0 = be32_to_cpu(READ_ONCE(descriptor[0]));
		word1 = be32_to_cpu(READ_ONCE(descriptor[1]));
		word3 = be32_to_cpu(READ_ONCE(descriptor[3]));
		expected_dma = buffers_dma[*head];
		length = word0 & 0x3fff;

		rdp->cpu_rx_last_word[0] = word0;
		rdp->cpu_rx_last_word[1] = word1;
		rdp->cpu_rx_last_word[2] = word2;
		rdp->cpu_rx_last_word[3] = word3;

		if ((word2 & BCM63138_CPU_RX_BUFFER_PTR_MASK) !=
		    (u32)expected_dma ||
		    length > BCM63138_CPU_RX_BUFFER_SIZE)
			rdp->cpu_rx_errors++;
		else {
			if (netdev && netif_running(netdev)) {
				struct sk_buff *skb;
				void *packet = buffers[*head];

				if (napi)
					skb = napi_alloc_skb(napi, length);
				else
					skb = netdev_alloc_skb_ip_align(netdev,
								       length);
				if (skb) {
					dma_sync_single_for_cpu(
						rdp->dev, expected_dma,
						BCM63138_CPU_RX_BUFFER_SIZE,
						DMA_FROM_DEVICE);
					memcpy(skb_put(skb, length), packet,
					       length);
					dma_sync_single_for_device(
						rdp->dev, expected_dma,
						BCM63138_CPU_RX_BUFFER_SIZE,
						DMA_FROM_DEVICE);
					if (word0 &
					    BCM63138_CPU_RX_CSUM_VERIFIED) {
						rdp->cpu_rx_checksum_verified++;
						if (netdev->features &
						    NETIF_F_RXCSUM)
							skb->ip_summed =
								CHECKSUM_UNNECESSARY;
					} else {
						rdp->cpu_rx_checksum_unverified++;
					}
					skb->protocol = eth_type_trans(skb, netdev);
					__skb_queue_tail(&received, skb);
					netdev->stats.rx_packets++;
					netdev->stats.rx_bytes += length;
				} else {
					netdev->stats.rx_dropped++;
				}
			}
			rdp->cpu_rx_packets++;
			rdp->cpu_rx_bytes += length;
		}

		dma_mb();
		WRITE_ONCE(descriptor[2],
			   cpu_to_be32((u32)expected_dma));

		(*head)++;
		if (*head == BCM63138_CPU_RX_RING_ENTRIES)
			*head = 0;
		completed++;
	}
	spin_unlock_bh(&rdp->cpu_rx_lock);

	while (!skb_queue_empty(&received)) {
		struct sk_buff *skb = __skb_dequeue(&received);

		if (napi) {
			gro_result_t result = napi_gro_receive(napi, skb);

			if (result <= GRO_CONSUMED)
				rdp->cpu_rx_gro_results[result]++;
		} else {
			netif_rx(skb);
		}
	}

	return completed;
}

static ssize_t cpu_ring_drain_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool drain;
	int err;

	err = kstrtobool(buf, &drain);
	if (err)
		return err;
	if (!drain)
		return -EINVAL;

	mutex_lock(&rdp->init_lock);
	if (rdp->shutting_down)
		err = -ESHUTDOWN;
	else if (!rdp->cpu_rx_ring_ready)
		err = -EAGAIN;
	else if (READ_ONCE(rdp->cpu_irq_enabled) ||
		 READ_ONCE(rdp->cpu_rx_polling))
		err = -EBUSY;
	else
		err = 0;
	if (err)
		goto out_unlock;
	bcm63138_rdp_cpu_rx_drain(rdp, 0,
				 BCM63138_CPU_RX_RING_ENTRIES, NULL);
	bcm63138_rdp_cpu_rx_drain(rdp, 1,
				 BCM63138_CPU_RX_RING_ENTRIES, NULL);
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(cpu_ring_drain);

static void bcm63138_rdp_cpu_rx_work(struct work_struct *work)
{
	struct bcm63138_rdp *rdp =
		container_of(to_delayed_work(work), struct bcm63138_rdp,
			     cpu_rx_work);
	unsigned int lan_completed;
	unsigned int wan_completed;
	unsigned long delay;

	if (READ_ONCE(rdp->cpu_irq_enabled)) {
		if (READ_ONCE(rdp->cpu_rx_polling))
			schedule_delayed_work(&rdp->cpu_rx_work,
					      msecs_to_jiffies(100));
		return;
	}

	lan_completed = bcm63138_rdp_cpu_rx_drain(
		rdp, 0, BCM63138_CPU_RX_RING_ENTRIES, NULL);
	wan_completed = bcm63138_rdp_cpu_rx_drain(
		rdp, 1, BCM63138_CPU_RX_RING_ENTRIES, NULL);
	if (!READ_ONCE(rdp->cpu_rx_polling))
		return;

	if (READ_ONCE(rdp->cpu_irq_enabled))
		delay = msecs_to_jiffies(100);
	else if (lan_completed == BCM63138_CPU_RX_RING_ENTRIES ||
		 wan_completed == BCM63138_CPU_RX_RING_ENTRIES)
		delay = 0;
	else
		delay = msecs_to_jiffies(10);
	schedule_delayed_work(&rdp->cpu_rx_work, delay);
}

static ssize_t cpu_ring_polling_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool enable;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	mutex_lock(&rdp->init_lock);
	if (rdp->shutting_down) {
		err = -ESHUTDOWN;
		goto out_unlock;
	}
	if (enable && (!rdp->cpu_rx_ring_ready ||
		       !rdp->runner_execution_enabled)) {
		err = -EAGAIN;
		goto out_unlock;
	}
	if (enable) {
		if (xchg(&rdp->cpu_rx_polling, true)) {
			err = -EALREADY;
			goto out_unlock;
		}
		schedule_delayed_work(&rdp->cpu_rx_work, 0);
		dev_info(dev, "CPU RX ring polling enabled\n");
	} else {
		if (!xchg(&rdp->cpu_rx_polling, false)) {
			err = -EALREADY;
			goto out_unlock;
		}
		cancel_delayed_work_sync(&rdp->cpu_rx_work);
		dev_info(dev, "CPU RX ring polling disabled\n");
	}
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(cpu_ring_polling);

static ssize_t runner_execution_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool enable;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	mutex_lock(&rdp->init_lock);
	if (!rdp->data_path_initialized) {
		err = -EAGAIN;
		goto out_unlock;
	}

	if (enable) {
		if (rdp->runner_execution_enabled) {
			err = -EALREADY;
			goto out_unlock;
		}
		err = bcm63138_rdp_vendor_runner_enable();
		if (err) {
			err = -EIO;
			goto out_unlock;
		}
		usleep_range(5000, 10000);
		if (!rdp->wan_tx_initialized) {
			err = bcm63138_rdp_vendor_wan_tx_scheduler_init();
			if (err) {
				bcm63138_rdp_vendor_runner_disable();
				err = -EIO;
				goto out_unlock;
			}
			rdp->wan_tx_initialized = true;
		}
		rdp->runner_execution_enabled = true;
		dev_info(dev,
			 "Runner execution enabled and WAN scheduler attached post-start; packet sources and IRQs remain disabled\n");
	} else {
		if (!rdp->runner_execution_enabled) {
			err = -EALREADY;
			goto out_unlock;
		}
		err = bcm63138_rdp_vendor_runner_disable();
		if (err) {
			err = -EIO;
			goto out_unlock;
		}
		rdp->runner_execution_enabled = false;
		dev_info(dev, "Runner execution disabled\n");
	}

	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(runner_execution);

static u32 bcm63138_rdp_unimac_read(struct bcm63138_rdp *rdp,
				   unsigned int offset)
{
	return ioread32be(rdp->base + offset);
}

static void bcm63138_rdp_unimac_write(struct bcm63138_rdp *rdp,
				     unsigned int offset, u32 value)
{
	iowrite32be(value, rdp->base + offset);
}

static ssize_t unimac0_setup_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	static const unsigned int offsets[] = {
		BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_HD_BKP,
		BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_CMD,
		BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_FRAME_LEN,
		BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_PAUSE_QUANTA,
		BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_MODE,
		BCM63138_UNIMAC_MISC_CFG,
		BCM63138_UNIMAC_MISC_EXT_CFG1,
		BCM63138_UNIMAC_MISC_EXT_CFG2,
	};
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool enable;
	unsigned int i;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	mutex_lock(&rdp->init_lock);
	if (!rdp->data_path_initialized) {
		err = -EAGAIN;
		goto out_unlock;
	}

	if (enable) {
		if (rdp->unimac0_configured) {
			err = -EALREADY;
			goto out_unlock;
		}

		for (i = 0; i < ARRAY_SIZE(offsets); i++)
			rdp->unimac0_saved[i] =
				bcm63138_rdp_unimac_read(rdp, offsets[i]);

		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC_MISC_CFG,
			0x00000001);
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC_MISC_EXT_CFG1,
			0x00e00800);
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC_MISC_EXT_CFG2,
			0x00000020);
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_HD_BKP,
			0x00000014);
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_FRAME_LEN,
			0x00003fff);
		bcm63138_rdp_unimac_write(
			rdp,
			BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_PAUSE_QUANTA,
			0x0000ffff);
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_MODE,
			0x0000001a);
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_CMD,
			0x018000d8);
		rdp->unimac0_configured = true;
		dev_info(dev, "UniMAC0 configured for the blue WAN port\n");
	} else {
		if (!rdp->unimac0_configured) {
			err = -EALREADY;
			goto out_unlock;
		}

		for (i = 0; i < ARRAY_SIZE(offsets); i++)
			bcm63138_rdp_unimac_write(
				rdp, offsets[i], rdp->unimac0_saved[i]);
		rdp->unimac0_configured = false;
		dev_info(dev, "UniMAC0 configuration restored\n");
	}

	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(unimac0_setup);

static ssize_t unimac1_setup_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	static const unsigned int offsets[] = {
		BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_HD_BKP,
		BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_CMD,
		BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_FRAME_LEN,
		BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_PAUSE_QUANTA,
		BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_MODE,
		BCM63138_UNIMAC_MISC_CFG + BCM63138_UNIMAC_MISC_STRIDE,
		BCM63138_UNIMAC_MISC_EXT_CFG1 + BCM63138_UNIMAC_MISC_STRIDE,
		BCM63138_UNIMAC_MISC_EXT_CFG2 + BCM63138_UNIMAC_MISC_STRIDE,
	};
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool enable;
	unsigned int i;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	mutex_lock(&rdp->init_lock);
	if (!rdp->data_path_initialized) {
		err = -EAGAIN;
		goto out_unlock;
	}

	if (enable) {
		if (rdp->unimac1_configured) {
			err = -EALREADY;
			goto out_unlock;
		}

		for (i = 0; i < ARRAY_SIZE(offsets); i++)
			rdp->unimac1_saved[i] =
				bcm63138_rdp_unimac_read(rdp, offsets[i]);

		bcm63138_rdp_unimac_write(
			rdp,
			BCM63138_UNIMAC_MISC_CFG +
				BCM63138_UNIMAC_MISC_STRIDE,
			rdp->unimac1_saved[5] | BIT(0));
		bcm63138_rdp_unimac_write(
			rdp,
			BCM63138_UNIMAC_MISC_EXT_CFG1 +
				BCM63138_UNIMAC_MISC_STRIDE,
			(rdp->unimac1_saved[6] & ~GENMASK(13, 0)) | 0x800);
		bcm63138_rdp_unimac_write(
			rdp,
			BCM63138_UNIMAC_MISC_EXT_CFG2 +
				BCM63138_UNIMAC_MISC_STRIDE,
			rdp->unimac1_saved[7] | BIT(17));
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_FRAME_LEN,
			0x00003fff);
		bcm63138_rdp_unimac_write(
			rdp, BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_CMD,
			0x018000db);
		rdp->unimac1_configured = true;
		dev_info(dev, "UniMAC1 configured for SF2 at 1 Gbit/s\n");
	} else {
		if (!rdp->unimac1_configured) {
			err = -EALREADY;
			goto out_unlock;
		}

		for (i = 0; i < ARRAY_SIZE(offsets); i++)
			bcm63138_rdp_unimac_write(
				rdp, offsets[i], rdp->unimac1_saved[i]);
		rdp->unimac1_configured = false;
		dev_info(dev, "UniMAC1 configuration restored\n");
	}

	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(unimac1_setup);

static ssize_t broadcast_trap_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool enable;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	mutex_lock(&rdp->init_lock);
	if (!rdp->runner_execution_enabled || !rdp->cpu_rx_polling) {
		err = -EAGAIN;
		goto out_unlock;
	}
	if (enable == rdp->broadcast_trap_enabled) {
		err = -EALREADY;
		goto out_unlock;
	}

	err = bcm63138_rdp_vendor_broadcast_trap(enable);
	if (err) {
		err = -EIO;
		goto out_unlock;
	}
	rdp->broadcast_trap_enabled = enable;
	dev_info(dev, "RDD LAN1 broadcast CPU trap %s\n",
		 enable ? "enabled" : "disabled");
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(broadcast_trap);

static ssize_t ethernet_sources_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	unsigned int mask;
	int err;

	err = kstrtouint(buf, 0, &mask);
	if (err)
		return err;
	if (mask > 3)
		return -ERANGE;

	mutex_lock(&rdp->init_lock);
	if (mask && (!rdp->runner_execution_enabled ||
		     !rdp->cpu_rx_polling)) {
		err = -EAGAIN;
		goto out_unlock;
	}
	if (mask == rdp->ethernet_source_mask) {
		err = -EALREADY;
		goto out_unlock;
	}

	err = bcm63138_rdp_vendor_ethernet_sources(mask);
	if (err) {
		err = -EIO;
		goto out_unlock;
	}

	rdp->ethernet_source_mask = mask;
	dev_info(dev, "Ethernet packet source mask set to 0x%x\n", mask);
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(ethernet_sources);

static ssize_t rx_irq_coalescing_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	struct bcm63138_rdp_rx_coalescing_state state = {};

	if (!rdp->data_path_initialized)
		return -EAGAIN;

	bcm63138_rdp_vendor_rx_coalescing_state(&state);

	return sysfs_emit(buf,
		"enabled=%u requested_timeout_us=%u requested_max_packets=%u timer_period_us=%u timer_armed=0x%04x ring0=current:%u/%u configured:%u/%u ring1=current:%u/%u configured:%u/%u\n",
		rdp->rx_coalescing_enabled,
		rdp->rx_coalescing_timeout_us,
		rdp->rx_coalescing_max_packets,
		state.timer_period,
		state.timer_armed,
		state.current_timeout[0],
		state.current_packets[0],
		state.configured_timeout[0],
		state.configured_packets[0],
		state.current_timeout[1],
		state.current_packets[1],
		state.configured_timeout[1],
		state.configured_packets[1]);
}

static ssize_t rx_irq_coalescing_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	unsigned int timeout_us;
	unsigned int max_packets;
	char extra;
	int parsed;
	int err;

	parsed = sscanf(buf, "%u %u %c", &timeout_us, &max_packets, &extra);
	if (parsed != 2)
		return -EINVAL;
	if (timeout_us < 100 || timeout_us > 1023 ||
	    !max_packets ||
	    max_packets > BCM63138_CPU_RX_RING_ENTRIES / 2)
		return -ERANGE;

	mutex_lock(&rdp->init_lock);
	if (!rdp->data_path_initialized || !rdp->cpu_rx_ring_ready ||
	    !rdp->runner_execution_enabled || !rdp->napi_enabled ||
	    !rdp->cpu_irq_enabled) {
		err = -EAGAIN;
		goto out_unlock;
	}

	err = bcm63138_rdp_vendor_rx_coalescing_config(timeout_us,
						       max_packets);
	if (err) {
		err = -EIO;
		goto out_unlock;
	}

	rdp->rx_coalescing_enabled = true;
	rdp->rx_coalescing_timeout_us = timeout_us;
	rdp->rx_coalescing_max_packets = max_packets;
	dev_info(dev,
		 "Runner CPU RX interrupt coalescing set to %u us / %u packets\n",
		 timeout_us, max_packets);
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_RW(rx_irq_coalescing);

static ssize_t cpu_rx_gro_state_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	struct net_device *lan = rdp->netdev[BCM63138_RDP_EMAC1];
	struct net_device *wan = rdp->netdev[BCM63138_RDP_EMAC0];

	if (!lan || !wan)
		return -EAGAIN;

	return sysfs_emit(buf,
		"lan=gro:%u rxcsum:%u features:%016llx hw:%016llx wanted:%016llx wan=gro:%u rxcsum:%u features:%016llx hw:%016llx wanted:%016llx checksum=verified:%llu unverified:%llu results=merged:%llu merged_free:%llu held:%llu normal:%llu consumed:%llu\n",
		!!(lan->features & NETIF_F_GRO),
		!!(lan->features & NETIF_F_RXCSUM),
		(unsigned long long)lan->features,
		(unsigned long long)lan->hw_features,
		(unsigned long long)lan->wanted_features,
		!!(wan->features & NETIF_F_GRO),
		!!(wan->features & NETIF_F_RXCSUM),
		(unsigned long long)wan->features,
		(unsigned long long)wan->hw_features,
		(unsigned long long)wan->wanted_features,
		rdp->cpu_rx_checksum_verified,
		rdp->cpu_rx_checksum_unverified,
		rdp->cpu_rx_gro_results[GRO_MERGED],
		rdp->cpu_rx_gro_results[GRO_MERGED_FREE],
		rdp->cpu_rx_gro_results[GRO_HELD],
		rdp->cpu_rx_gro_results[GRO_NORMAL],
		rdp->cpu_rx_gro_results[GRO_CONSUMED]);
}
static DEVICE_ATTR_RO(cpu_rx_gro_state);

static int bcm63138_rdp_cpu_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct bcm63138_rdp *rdp =
		container_of(napi, struct bcm63138_rdp, napi);
	unsigned int completed;
	unsigned int ring_budget;
	u32 error;

	ring_budget = min_t(unsigned int, budget,
			    BCM63138_CPU_RX_RING_ENTRIES);
	completed = bcm63138_rdp_cpu_rx_drain(rdp, 0, ring_budget, napi);
	if (completed < budget) {
		ring_budget = min_t(unsigned int, budget - completed,
				    BCM63138_CPU_RX_RING_ENTRIES);
		completed += bcm63138_rdp_cpu_rx_drain(rdp, 1, ring_budget,
						      napi);
	}

	if (completed)
		rdp->cpu_irq_empty_count = 0;
	if (completed == budget)
		return completed;

	if (!completed && ++rdp->cpu_irq_empty_count == 32 &&
		   !xchg(&rdp->cpu_irq_fault_pending, true)) {
		napi_complete_done(napi, completed);
		schedule_work(&rdp->cpu_irq_fault_work);
		dev_err_ratelimited(rdp->dev,
				    "masked empty Runner CPU RX interrupt storm; switching to polling\n");
		return completed;
	}

	if (!napi_complete_done(napi, completed) ||
	    !READ_ONCE(rdp->cpu_irq_enabled))
		return completed;

	error = bcm63138_rdp_vendor_interrupt_unmask(0, 0);
	if (error && !xchg(&rdp->cpu_irq_fault_pending, true)) {
		schedule_work(&rdp->cpu_irq_fault_work);
		dev_err_ratelimited(rdp->dev,
				    "failed to unmask Runner CPU RX interrupt; switching to polling\n");
	}

	return completed;
}

static irqreturn_t bcm63138_rdp_cpu_irq(int irq, void *data)
{
	struct bcm63138_rdp *rdp = data;

	atomic_inc(&rdp->cpu_irq_count);
	bcm63138_rdp_vendor_interrupt_mask(0, 0);
	bcm63138_rdp_vendor_interrupt_clear(0, 0);
	if (unlikely(!READ_ONCE(rdp->napi_enabled)))
		return IRQ_HANDLED;
	if (napi_schedule_prep(&rdp->napi))
		__napi_schedule_irqoff(&rdp->napi);

	return IRQ_HANDLED;
}

static int __bcm63138_rdp_set_cpu_irq(struct bcm63138_rdp *rdp, bool enable)
{
	u32 error;

	if (!rdp->cpu_irq_registered)
		return -EAGAIN;
	if (enable && (!rdp->napi_added || !rdp->napi_enabled))
		return -EAGAIN;
	if (enable == rdp->cpu_irq_enabled)
		return -EALREADY;

	if (enable) {
		rdp->cpu_irq_empty_count = 0;
		WRITE_ONCE(rdp->cpu_irq_enabled, true);
		cancel_delayed_work_sync(&rdp->cpu_rx_work);
		bcm63138_rdp_vendor_interrupt_clear(0, 0);
		bcm63138_rdp_cpu_rx_drain(
			rdp, 0, BCM63138_CPU_RX_RING_ENTRIES, NULL);
		bcm63138_rdp_cpu_rx_drain(
			rdp, 1, BCM63138_CPU_RX_RING_ENTRIES, NULL);
		enable_irq(rdp->irq[0]);
		error = bcm63138_rdp_vendor_interrupt_unmask(0, 0);
		if (error) {
			disable_irq(rdp->irq[0]);
			WRITE_ONCE(rdp->cpu_irq_enabled, false);
			if (rdp->cpu_rx_polling)
				schedule_delayed_work(&rdp->cpu_rx_work, 0);
			return -EIO;
		}
		if (rdp->cpu_rx_polling)
			schedule_delayed_work(&rdp->cpu_rx_work,
					      msecs_to_jiffies(100));
		dev_info(rdp->dev, "Runner CPU RX IRQ enabled\n");
	} else {
		WRITE_ONCE(rdp->cpu_irq_enabled, false);
		error = bcm63138_rdp_vendor_interrupt_mask(0, 0);
		if (error) {
			WRITE_ONCE(rdp->cpu_irq_enabled, true);
			return -EIO;
		}
		disable_irq(rdp->irq[0]);
		dev_info(rdp->dev, "Runner CPU RX IRQ disabled\n");
	}

	return 0;
}

static int bcm63138_rdp_set_cpu_irq(struct bcm63138_rdp *rdp, bool enable)
{
	int err;

	mutex_lock(&rdp->init_lock);
	if (rdp->shutting_down)
		err = -ESHUTDOWN;
	else
		err = __bcm63138_rdp_set_cpu_irq(rdp, enable);
	mutex_unlock(&rdp->init_lock);

	return err;
}

static void bcm63138_rdp_cpu_irq_fault_work(struct work_struct *work)
{
	struct bcm63138_rdp *rdp =
		container_of(work, struct bcm63138_rdp, cpu_irq_fault_work);
	int err;

	mutex_lock(&rdp->init_lock);
	if (rdp->shutting_down) {
		mutex_unlock(&rdp->init_lock);
		return;
	}
	if (!rdp->cpu_rx_polling)
		rdp->cpu_rx_polling = true;
	err = __bcm63138_rdp_set_cpu_irq(rdp, false);
	mod_delayed_work(system_wq, &rdp->cpu_rx_work, 0);
	mutex_unlock(&rdp->init_lock);
	if (err && err != -EALREADY)
		dev_err(rdp->dev,
			"failed to disable faulty Runner CPU RX IRQ: %d\n", err);
	WRITE_ONCE(rdp->cpu_irq_fault_pending, false);
}

static void bcm63138_rdp_shutdown(void *data)
{
	struct bcm63138_rdp *rdp = data;
	u32 error;
	unsigned int i;
	int ret;

	mutex_lock(&rdp->init_lock);
	if (rdp->shutting_down) {
		mutex_unlock(&rdp->init_lock);
		return;
	}
	WRITE_ONCE(rdp->shutting_down, true);

	for (i = 0; i < ARRAY_SIZE(rdp->netdev); i++) {
		if (rdp->netdev[i] &&
		    rdp->netdev[i]->reg_state == NETREG_REGISTERED)
			netif_tx_disable(rdp->netdev[i]);
	}

	if (READ_ONCE(rdp->cpu_irq_enabled)) {
		error = __bcm63138_rdp_set_cpu_irq(rdp, false);
		if (error) {
			WRITE_ONCE(rdp->cpu_irq_enabled, false);
			disable_irq(rdp->irq[0]);
			dev_warn(rdp->dev,
				 "failed to mask Runner CPU RX interrupt during shutdown: %u\n",
				 error);
		}
	} else if (rdp->cpu_irq_registered) {
		error = bcm63138_rdp_vendor_interrupt_mask(0, 0);
		if (error)
			dev_warn(rdp->dev,
				 "failed to mask inactive Runner CPU RX interrupt during shutdown: %u\n",
				 error);
	}
	WRITE_ONCE(rdp->cpu_rx_polling, false);

	if (rdp->data_path_init_attempted || rdp->ethernet_source_mask) {
		error = bcm63138_rdp_vendor_ethernet_sources_disable();
		if (error)
			dev_warn(rdp->dev,
				 "failed to disable Ethernet packet sources during shutdown: %u\n",
				 error);
		else
			rdp->ethernet_source_mask = 0;
	}
	if (rdp->broadcast_trap_enabled) {
		error = bcm63138_rdp_vendor_broadcast_trap(false);
		if (error)
			dev_warn(rdp->dev,
				 "failed to disable broadcast trap during shutdown: %u\n",
				 error);
		else
			rdp->broadcast_trap_enabled = false;
	}
	if (rdp->runner_execution_enabled || rdp->data_path_allocated) {
		error = bcm63138_rdp_vendor_runner_disable();
		if (error)
			dev_warn(rdp->dev,
				 "failed to disable Runner execution during shutdown: %u\n",
				 error);
		else
			rdp->runner_execution_enabled = false;
	}
	mutex_unlock(&rdp->init_lock);

	cancel_delayed_work_sync(&rdp->cpu_rx_work);
	if (rdp->napi_enabled) {
		napi_disable(&rdp->napi);
		rdp->napi_enabled = false;
	}
	cancel_work_sync(&rdp->cpu_irq_fault_work);
	WRITE_ONCE(rdp->cpu_irq_fault_pending, false);

	if (rdp->data_path_init_attempted || rdp->data_path_allocated) {
		ret = bcm_pmb_reset_rdp(rdp->dev);
		if (ret)
			dev_err(rdp->dev,
				"failed to reset RDP PMB domain: %d\n", ret);
		else
			dev_info(rdp->dev, "RDP PMB domain reset\n");
	}

	bcm63138_rdp_cpu_rx_rings_free(rdp);
	if (rdp->data_path_allocated) {
		bcm63138_rdp_vendor_data_path_exit();
		rdp->data_path_allocated = false;
		rdp->auxiliary_count = 0;
	}
	rdp->data_path_initialized = false;
	bcm63138_rdp_clear_globals(rdp);
}

static ssize_t cpu_irq_enable_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool enable;
	int err;

	err = kstrtobool(buf, &enable);
	if (err)
		return err;

	err = bcm63138_rdp_set_cpu_irq(rdp, enable);
	if (err)
		return err;

	return count;
}
static DEVICE_ATTR_WO(cpu_irq_enable);

static ssize_t cpu_irq_setup_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	bool setup;
	int err;

	err = kstrtobool(buf, &setup);
	if (err)
		return err;
	if (!setup)
		return -EINVAL;

	mutex_lock(&rdp->init_lock);
	if (!rdp->cpu_rx_ring_ready) {
		err = -EAGAIN;
		goto out_unlock;
	}
	if (rdp->cpu_irq_setup_attempted) {
		err = -EALREADY;
		goto out_unlock;
	}
	rdp->cpu_irq_setup_attempted = true;
	if (rdp->irq[0] < 0) {
		dev_warn(dev, "Runner CPU RX IRQ unavailable; using polling\n");
		err = count;
		goto out_unlock;
	}

	bcm63138_rdp_vendor_interrupt_clear(0, 0);
	err = devm_request_irq(dev, rdp->irq[0], bcm63138_rdp_cpu_irq,
			       IRQF_NO_AUTOEN,
			       "bcm63138-rdp-rx0", rdp);
	if (err) {
		dev_warn(dev,
			 "failed to request Runner CPU RX IRQ (%d); using polling\n",
			 err);
		err = count;
		goto out_unlock;
	}

	rdp->cpu_irq_registered = true;
	dev_info(dev, "Runner CPU RX IRQ %d registered and disabled\n",
		 rdp->irq[0]);
	err = count;

out_unlock:
	mutex_unlock(&rdp->init_lock);

	return err;
}
static DEVICE_ATTR_WO(cpu_irq_setup);

static ssize_t hardware_state_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	struct bcm63138_rdp_audit audit = {};
	ssize_t len = 0;
	unsigned int i;

	if (!rdp->data_path_initialized)
		return -EAGAIN;

	bcm63138_rdp_vendor_audit(&audit);

	len += sysfs_emit_at(buf, len,
		"rdd_ds_bpm=0x%08x extra=0x%08x optimized=0x%08x\n",
		audit.ds_bpm_base, audit.ds_bpm_extra_base,
		audit.ds_bpm_optimized_base);
	len += sysfs_emit_at(buf, len,
		"rdd_us_bpm=0x%08x extra=0x%08x optimized=0x%08x\n",
		audit.us_bpm_base, audit.us_bpm_extra_base,
		audit.us_bpm_optimized_base);
	len += sysfs_emit_at(buf, len,
		"cpu_tx_tables=data:0x%08x free:0x%08x last:0x%08x\n",
		audit.cpu_tx_data_table, audit.cpu_tx_free_index_table,
		audit.cpu_tx_free_index_last);
	len += sysfs_emit_at(buf, len,
		"cpu_tx_data_entries=%08x/%08x/%08x/%08x test_b7:%08x\n",
		audit.cpu_tx_data_entries[0], audit.cpu_tx_data_entries[1],
		audit.cpu_tx_data_entries[2], audit.cpu_tx_data_entries[3],
		audit.cpu_tx_data_test_entry);
	if (rdp->auxiliary_count > 0)
		len += sysfs_emit_at(buf, len,
			"aux_dma0=%pad size=0x%zx\n",
			&rdp->auxiliary_dma[0], rdp->auxiliary_size[0]);
	if (rdp->auxiliary_count > 1)
		len += sysfs_emit_at(buf, len,
			"aux_dma1=%pad size=0x%zx\n",
			&rdp->auxiliary_dma[1], rdp->auxiliary_size[1]);
	len += sysfs_emit_at(buf, len,
		"ih_iq_base=0x%08x size=0x%08x runner_a=0x%08x runner_b=0x%08x\n",
		audit.ih_iq_base, audit.ih_iq_size, audit.ih_runner_a_base,
		audit.ih_runner_b_base);
	len += sysfs_emit_at(buf, len,
		"bpm_source_enable=0x%08x threshold=0x%08x\n",
		audit.bpm_source_enable, audit.bpm_global_threshold);
	len += sysfs_emit_at(buf, len,
		"sbpm_init=0x%08x source_enable=0x%08x\n",
		audit.sbpm_init, audit.sbpm_source_enable);
	len += sysfs_emit_at(buf, len,
		"dma_source=0x%08x read_base0=0x%08x\n",
		audit.dma_source, audit.dma_read_base);
	len += sysfs_emit_at(buf, len,
		"sdma_source=0x%08x read_base0=0x%08x\n",
		audit.sdma_source, audit.sdma_read_base);
	len += sysfs_emit_at(buf, len,
		"bbh0_rx_ddr=0x%08x dma=0x%08x sdma=0x%08x\n",
		audit.bbh_rx0_ddr, audit.bbh_rx0_dma, audit.bbh_rx0_sdma);
	len += sysfs_emit_at(buf, len,
		"bbh0_tx_tm=0x%08x mc=0x%08x\n",
		audit.bbh_tx0_tm_base, audit.bbh_tx0_mc_base);
	len += sysfs_emit_at(buf, len,
		"bbh1_rx_ddr=0x%08x dma=0x%08x sdma=0x%08x bbh1_tx_tm=0x%08x mc=0x%08x\n",
		audit.bbh_rx1_ddr, audit.bbh_rx1_dma,
		audit.bbh_rx1_sdma, audit.bbh_tx1_tm_base,
		audit.bbh_tx1_mc_base);
	len += sysfs_emit_at(buf, len,
		"bbh_rx_tasks=port0:pd:%u runner0:%u runner1:%u port1:pd:%u runner0:%u runner1:%u\n",
		audit.bbh_rx_pd_base[0], audit.bbh_rx_runner0_task[0],
		audit.bbh_rx_runner1_task[0], audit.bbh_rx_pd_base[1],
		audit.bbh_rx_runner0_task[1],
		audit.bbh_rx_runner1_task[1]);
	len += sysfs_emit_at(buf, len,
		"bbh0_rx_counters=packets:%u short:%u long:%u crc:%u runner_cong:%u no_bpm:%u no_sbpm:%u no_dma:%u no_sdma:%u\n",
		audit.bbh_rx_packets[0], audit.bbh_rx_too_short[0],
		audit.bbh_rx_too_long[0], audit.bbh_rx_crc_error[0],
		audit.bbh_rx_runner_congestion[0], audit.bbh_rx_no_bpm[0],
		audit.bbh_rx_no_sbpm[0], audit.bbh_rx_no_dma[0],
		audit.bbh_rx_no_sdma[0]);
	len += sysfs_emit_at(buf, len,
		"bbh1_rx_counters=packets:%u short:%u long:%u crc:%u runner_cong:%u no_bpm:%u no_sbpm:%u no_dma:%u no_sdma:%u\n",
		audit.bbh_rx_packets[1], audit.bbh_rx_too_short[1],
		audit.bbh_rx_too_long[1], audit.bbh_rx_crc_error[1],
		audit.bbh_rx_runner_congestion[1], audit.bbh_rx_no_bpm[1],
		audit.bbh_rx_no_sbpm[1], audit.bbh_rx_no_dma[1],
		audit.bbh_rx_no_sdma[1]);
	len += sysfs_emit_at(buf, len,
		"bbh_tx_counters=port0:sram:%u ddr:%u dropped:%u task:%u port1:sram:%u ddr:%u dropped:%u task:%u\n",
		audit.bbh_tx_sram[0], audit.bbh_tx_ddr[0],
		audit.bbh_tx_dropped[0], audit.bbh_tx_task[0],
		audit.bbh_tx_sram[1], audit.bbh_tx_ddr[1],
		audit.bbh_tx_dropped[1], audit.bbh_tx_task[1]);
	len += sysfs_emit_at(buf, len,
		"cpu_ring_ready=%u ring_dma=%pad buffers_dma=lan:%pad wan:%pad mode:streaming\n",
		rdp->cpu_rx_ring_ready, &rdp->cpu_rx_ring_dma,
		&rdp->cpu_rx_buffers_dma[0],
		&rdp->cpu_rx_wan_buffers_dma[0]);
	len += sysfs_emit_at(buf, len,
		"rdd_ring0=count:%u size:%u entries:%u pointer:0x%08x irq:0x%04x\n",
		audit.ring0_entries_counter, audit.ring0_entry_size,
		audit.ring0_entries, audit.ring0_pointer,
		audit.ring0_interrupt);
	len += sysfs_emit_at(buf, len,
		"runner_irq_status=0x%08x/0x%08x mask=0x%08x/0x%08x\n",
		audit.runner0_interrupt_status,
		audit.runner1_interrupt_status,
		audit.runner0_interrupt_mask,
		audit.runner1_interrupt_mask);
	len += sysfs_emit_at(buf, len,
		"runner_a_tx=cpu:%08x/%08x bbh:%08x/%08x write:%04x tail:%u context:r16:%08x r8:%08x r9:%08x\n",
		audit.ds_cpu_tx_fast_descriptor[0],
		audit.ds_cpu_tx_fast_descriptor[1],
		audit.ds_cpu_tx_bbh_descriptor[0],
		audit.ds_cpu_tx_bbh_descriptor[1],
		audit.ds_cpu_tx_write_ptr, audit.ds_cpu_tx_tail,
		audit.runner_a_cpu_tx_r16, audit.runner_a_cpu_tx_r8,
		audit.runner_a_cpu_tx_r9);
	len += sysfs_emit_at(buf, len,
		"runner_a_pico_tx=cpu:%08x/%08x write:%04x tail:%u context:r16:%08x r8:%08x r9:%08x\n",
		audit.ds_cpu_tx_pico_descriptor[0],
		audit.ds_cpu_tx_pico_descriptor[1],
		audit.ds_cpu_tx_pico_write_ptr,
		audit.ds_cpu_tx_pico_tail,
		audit.runner_a_pico_tx_r16, audit.runner_a_pico_tx_r8,
		audit.runner_a_pico_tx_r9);
	for (i = 0; i < ARRAY_SIZE(audit.emac_queue_pointer); i++) {
		len += sysfs_emit_at(buf, len,
			"emac%u_queue=ptr:%04x/%04x head:%04x tail:%04x packets:%u/%u threshold:%u profile:%04x mask:%02x index:%u\n",
			i, audit.emac_queue_mac_pointer[i],
			audit.emac_queue_pointer[i],
			audit.emac_queue_head[i], audit.emac_queue_tail[i],
			audit.emac_queue_ingress[i],
			audit.emac_queue_egress[i],
			audit.emac_queue_threshold[i],
			audit.emac_queue_profile[i],
			audit.emac_queue_mask[i],
			audit.emac_queue_index[i]);
		len += sysfs_emit_at(buf, len,
			"emac%u_mac=packets:%u/%u task:%u status:%02x counters:%04x local:%08x/%08x\n",
			i, audit.emac_mac_ingress[i],
			audit.emac_mac_egress[i],
			audit.emac_mac_tx_task[i],
			audit.emac_mac_queue_status[i],
			audit.emac_mac_counters_pointer[i],
			audit.emac_local_registers[i][0],
			audit.emac_local_registers[i][1]);
	}
	len += sysfs_emit_at(buf, len,
		"runner_b_tx=cpu:%08x/%08x bbh:%08x/%08x write:%04x tail:%u wan0:%08x/%08x counters:%u/%u\n",
		audit.us_cpu_tx_fast_descriptor[0],
		audit.us_cpu_tx_fast_descriptor[1],
		audit.us_cpu_tx_bbh_descriptor[0],
		audit.us_cpu_tx_bbh_descriptor[1],
		audit.us_cpu_tx_write_ptr, audit.us_cpu_tx_tail,
		audit.wan_channel0_descriptor[0],
		audit.wan_channel0_descriptor[1],
		audit.ethwan_tx_firmware_counter,
		audit.ethwan_tx_bbh_counter);
	len += sysfs_emit_at(buf, len,
		"runner_b_timer=task0:%08x/%08x active:%u\n",
		audit.us_main_timer_task[0],
		audit.us_main_timer_task[1],
		audit.us_main_timer_active_tasks);
	len += sysfs_emit_at(buf, len, "wan_channel_raw=");
	for (i = 0; i < ARRAY_SIZE(audit.wan_channel0_descriptor); i++)
		len += sysfs_emit_at(buf, len, "%s%08x",
				     i ? "/" : "",
				     audit.wan_channel0_descriptor[i]);
	len += sysfs_emit_at(buf, len, "\n");
	len += sysfs_emit_at(buf, len,
		"runner_b_context=cpu_tx_r16:%08x wan1_r16:%08x r8:%08x r9:%08x\n",
		audit.runner_b_cpu_tx_r16, audit.runner_b_wan1_tx_r16,
		audit.runner_b_wan1_tx_r8, audit.runner_b_wan1_tx_r9);
	len += sysfs_emit_at(buf, len,
		"us_free_pd_pool=guaranteed:%u non_guaranteed:%u threshold:%u\n",
		audit.us_free_pd_guaranteed,
		audit.us_free_pd_non_guaranteed,
		audit.us_free_pd_threshold);
	len += sysfs_emit_at(buf, len,
		"wan_scheduler=queue:%04x/%04x packets:%u threshold:%u rc:%04x mask:%04x channel_bbh:%04x status:%08x rc_channel:%04x rc_queue:%04x\n",
		audit.wan_queue_head, audit.wan_queue_tail,
		audit.wan_queue_packets, audit.wan_queue_threshold,
		audit.wan_queue_rate_controller, audit.wan_queue_mask,
		audit.wan_channel_bbh_destination,
		audit.wan_channel_rate_controllers_status,
		audit.rate_controller_wan_channel,
		audit.rate_controller_queue0);
	len += sysfs_emit_at(buf, len,
		"wan_raw=physical:%u flow_ef:%08x queue:%08x/%08x/%08x/%08x\n",
		audit.wan_physical_port, audit.wan_flow_raw,
		audit.wan_queue_raw[0], audit.wan_queue_raw[1],
		audit.wan_queue_raw[2], audit.wan_queue_raw[3]);
	len += sysfs_emit_at(buf, len,
		"wan_rate_controller_raw=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
		audit.rate_controller_raw[0], audit.rate_controller_raw[1],
		audit.rate_controller_raw[2], audit.rate_controller_raw[3],
		audit.rate_controller_raw[4], audit.rate_controller_raw[5],
		audit.rate_controller_raw[6], audit.rate_controller_raw[7]);
	len += sysfs_emit_at(buf, len,
		"cpu_irq_registered=%u enabled=%u count=%d linux_irq=%d\n",
		rdp->cpu_irq_registered, READ_ONCE(rdp->cpu_irq_enabled),
		atomic_read(&rdp->cpu_irq_count),
		rdp->irq[0]);
	len += sysfs_emit_at(buf, len,
		"cpu_rx_head=%u packets=%llu bytes=%llu errors=%llu last=%08x/%08x/%08x/%08x\n",
		rdp->cpu_rx_head, rdp->cpu_rx_packets, rdp->cpu_rx_bytes,
		rdp->cpu_rx_errors, rdp->cpu_rx_last_word[0],
		rdp->cpu_rx_last_word[1], rdp->cpu_rx_last_word[2],
		rdp->cpu_rx_last_word[3]);
	len += sysfs_emit_at(buf, len, "runner_execution_enabled=%u\n",
			     rdp->runner_execution_enabled);
	len += sysfs_emit_at(buf, len,
			     "wan_tx_initialized=base:%u scheduler:%u\n",
			     rdp->wan_tx_base_initialized,
			     rdp->wan_tx_initialized);
	len += sysfs_emit_at(buf, len, "cpu_rx_polling=%u\n",
			     rdp->cpu_rx_polling);
	len += sysfs_emit_at(buf, len, "cpu_rx_napi=added:%u enabled:%u\n",
			     rdp->napi_added, rdp->napi_enabled);
	len += sysfs_emit_at(buf, len, "ethernet_source_mask=0x%x\n",
			     rdp->ethernet_source_mask);
	len += sysfs_emit_at(
		buf, len,
		"unimac1_configured=%u cmd=0x%08x mode=0x%08x misc=0x%08x/0x%08x/0x%08x\n",
		rdp->unimac1_configured,
		bcm63138_rdp_unimac_read(
			rdp, BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_CMD),
		bcm63138_rdp_unimac_read(
			rdp, BCM63138_UNIMAC1_BASE + BCM63138_UNIMAC_MODE),
		bcm63138_rdp_unimac_read(
			rdp, BCM63138_UNIMAC_MISC_CFG +
				BCM63138_UNIMAC_MISC_STRIDE),
		bcm63138_rdp_unimac_read(
			rdp, BCM63138_UNIMAC_MISC_EXT_CFG1 +
				BCM63138_UNIMAC_MISC_STRIDE),
		bcm63138_rdp_unimac_read(
			rdp, BCM63138_UNIMAC_MISC_EXT_CFG2 +
				BCM63138_UNIMAC_MISC_STRIDE));
	len += sysfs_emit_at(buf, len, "broadcast_trap_enabled=%u\n",
			     rdp->broadcast_trap_enabled);

	return len;
}
static DEVICE_ATTR_RO(hardware_state);

static int bcm63138_rdp_write_u32_firmware(struct bcm63138_rdp *rdp,
					   unsigned int offset,
					   const struct firmware *fw)
{
	size_t i;

	if (fw->size % sizeof(u32))
		return -EINVAL;

	for (i = 0; i < fw->size; i += sizeof(u32))
		iowrite32be(get_unaligned_be32(fw->data + i),
			    rdp->base + offset + i);

	for (i = 0; i < fw->size; i += sizeof(u32)) {
		if (ioread32be(rdp->base + offset + i) !=
		    get_unaligned_be32(fw->data + i))
			return -EIO;
	}

	return 0;
}

static int bcm63138_rdp_write_prediction(struct bcm63138_rdp *rdp,
					 unsigned int offset,
					 const struct firmware *fw)
{
	size_t i;

	if (fw->size % sizeof(u16))
		return -EINVAL;

	for (i = 0; i < fw->size; i += sizeof(u16))
		iowrite32be(get_unaligned_be16(fw->data + i),
			    rdp->base + offset + i * 2);

	for (i = 0; i < fw->size; i += sizeof(u16)) {
		if (ioread32be(rdp->base + offset + i * 2) !=
		    get_unaligned_be16(fw->data + i))
			return -EIO;
	}

	return 0;
}

static int bcm63138_rdp_load_one(struct device *dev, unsigned int offset,
				 const char *name, bool prediction)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	const struct firmware *fw;
	int err;

	err = request_firmware(&fw, name, dev);
	if (err)
		return dev_err_probe(dev, err, "failed to request %s\n", name);

	if (prediction)
		err = bcm63138_rdp_write_prediction(rdp, offset, fw);
	else
		err = bcm63138_rdp_write_u32_firmware(rdp, offset, fw);

	release_firmware(fw);

	if (err)
		dev_err(dev, "firmware write verification failed for %s\n",
			name);

	return err;
}

static ssize_t firmware_reload_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	bool reload;
	int err;

	err = kstrtobool(buf, &reload);
	if (err)
		return err;
	if (!reload)
		return -EINVAL;

	err = bcm63138_rdp_load_one(dev, RDP_RUNNER_1_MAIN_INST,
				    BCM63138_FW_RUNNER_B, false);
	if (err)
		return err;
	err = bcm63138_rdp_load_one(dev, RDP_RUNNER_0_PICO_INST,
				    BCM63138_FW_RUNNER_C, false);
	if (err)
		return err;
	err = bcm63138_rdp_load_one(dev, RDP_RUNNER_1_PICO_INST,
				    BCM63138_FW_RUNNER_D, false);
	if (err)
		return err;
	err = bcm63138_rdp_load_one(dev, RDP_RUNNER_1_MAIN_PRED,
				    BCM63138_FW_PREDICT_B, true);
	if (err)
		return err;
	err = bcm63138_rdp_load_one(dev, RDP_RUNNER_0_PICO_PRED,
				    BCM63138_FW_PREDICT_C, true);
	if (err)
		return err;
	err = bcm63138_rdp_load_one(dev, RDP_RUNNER_1_PICO_PRED,
				    BCM63138_FW_PREDICT_D, true);
	if (err)
		return err;

	dev_info(dev,
		 "Runner B/C/D firmware and prediction tables loaded and verified\n");

	return count;
}
static DEVICE_ATTR_WO(firmware_reload);

static ssize_t runner_state_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct bcm63138_rdp *rdp = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	len += sysfs_emit_at(buf, len, "runner0_global_ctrl=0x%08x\n",
			     ioread32be(rdp->base +
					RDP_RUNNER_0_GLOBAL_CTRL));
	len += sysfs_emit_at(buf, len, "runner1_global_ctrl=0x%08x\n",
			     ioread32be(rdp->base +
					RDP_RUNNER_1_GLOBAL_CTRL));
	len += sysfs_emit_at(buf, len,
			     "runner1_cpu_wakeup=0x%08x pico_profiling=0x%08x\n",
			     ioread32be(rdp->base + RDP_RUNNER_1_CPU_WAKEUP),
			     ioread32be(rdp->base +
					RDP_RUNNER_1_PICO_PROFILING));
	len += sysfs_emit_at(buf, len, "tm_dma=%pad size=0x%x\n",
			     &rdp->tm_dma, BCM63138_RDP_TM_SIZE);
	len += sysfs_emit_at(buf, len, "mc_dma=%pad size=0x%x\n",
			     &rdp->mc_dma, BCM63138_RDP_MC_SIZE);
	len += sysfs_emit_at(buf, len, "data_path_init_attempted=%u\n",
			     rdp->data_path_init_attempted);
	len += sysfs_emit_at(buf, len, "data_path_initialized=%u\n",
			     rdp->data_path_initialized);

	for (i = 0; i < 4; i++)
		len += sysfs_emit_at(buf, len,
				     "runner0_main_inst[%d]=0x%08x\n", i,
				     ioread32be(rdp->base +
						RDP_RUNNER_0_MAIN_INST +
						i * sizeof(u32)));
	for (i = 0; i < 4; i++)
		len += sysfs_emit_at(buf, len,
				     "runner1_main_inst[%d]=0x%08x\n", i,
				     ioread32be(rdp->base +
						RDP_RUNNER_1_MAIN_INST +
						i * sizeof(u32)));
	for (i = 0; i < 4; i++)
		len += sysfs_emit_at(buf, len,
				     "runner0_pico_inst[%d]=0x%08x\n", i,
				     ioread32be(rdp->base +
						RDP_RUNNER_0_PICO_INST +
						i * sizeof(u32)));
	for (i = 0; i < 4; i++)
		len += sysfs_emit_at(buf, len,
				     "runner1_pico_inst[%d]=0x%08x\n", i,
				     ioread32be(rdp->base +
						RDP_RUNNER_1_PICO_INST +
						i * sizeof(u32)));

	return len;
}
static DEVICE_ATTR_RO(runner_state);

static struct attribute *bcm63138_rdp_attrs[] = {
	&dev_attr_broadcast_trap.attr,
	&dev_attr_cfe_state_adopt.attr,
	&dev_attr_cpu_irq_enable.attr,
	&dev_attr_cpu_irq_setup.attr,
	&dev_attr_cpu_ring_drain.attr,
	&dev_attr_cpu_ring_polling.attr,
	&dev_attr_cpu_ring_setup.attr,
	&dev_attr_cpu_ring_state.attr,
	&dev_attr_cpu_rx_gro_state.attr,
	&dev_attr_cpu_rx_wakeup.attr,
	&dev_attr_cpu_tx_test.attr,
	&dev_attr_wan_cpu_tx_abs_test.attr,
	&dev_attr_wan_cpu_tx_test.attr,
	&dev_attr_data_path_init.attr,
	&dev_attr_ethernet_sources.attr,
	&dev_attr_eth1_packet_state.attr,
	&dev_attr_firmware_reload.attr,
	&dev_attr_hardware_state.attr,
	&dev_attr_lan1_fifo_init.attr,
	&dev_attr_pipeline_state.attr,
	&dev_attr_runner_execution.attr,
	&dev_attr_runner_state.attr,
	&dev_attr_rx_irq_coalescing.attr,
	&dev_attr_unimac0_setup.attr,
	&dev_attr_unimac1_setup.attr,
	&dev_attr_wan1_fifo_init.attr,
	NULL,
};

static const struct attribute_group bcm63138_rdp_group = {
	.attrs = bcm63138_rdp_attrs,
};

static int bcm63138_rdp_net_open(struct net_device *netdev)
{
	struct bcm63138_rdp_netdev *priv = netdev_priv(netdev);
	struct bcm63138_rdp *rdp = priv->rdp;

	if (!rdp->cpu_rx_polling || !rdp->ethernet_source_mask)
		return -EAGAIN;

	if (priv->emac_id == BCM63138_RDP_EMAC0)
		phy_start(rdp->wan_phy);
	else
		netif_carrier_on(netdev);
	netif_start_queue(netdev);

	return 0;
}

static int bcm63138_rdp_net_stop(struct net_device *netdev)
{
	struct bcm63138_rdp_netdev *priv = netdev_priv(netdev);

	netif_stop_queue(netdev);
	if (priv->emac_id == BCM63138_RDP_EMAC0)
		phy_stop(priv->rdp->wan_phy);
	netif_carrier_off(netdev);

	return 0;
}

static netdev_tx_t bcm63138_rdp_net_xmit(struct sk_buff *skb,
					 struct net_device *netdev)
{
	struct bcm63138_rdp_netdev *priv = netdev_priv(netdev);
	struct bcm63138_rdp *rdp = priv->rdp;
	int err;

	if (unlikely(READ_ONCE(rdp->shutting_down) ||
		     !READ_ONCE(rdp->runner_execution_enabled) ||
		     !READ_ONCE(rdp->ethernet_source_mask))) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	if (skb->len > 1536) {
		netdev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	err = bcm63138_rdp_vendor_cpu_tx(skb->data, skb->len,
					 priv->emac_id);
	if (err)
		netdev->stats.tx_dropped++;
	else {
		netdev->stats.tx_packets++;
		netdev->stats.tx_bytes += skb->len;
	}
	dev_kfree_skb(skb);

	return NETDEV_TX_OK;
}

static int bcm63138_rdp_net_set_mac_address(struct net_device *netdev,
					    void *address)
{
	struct bcm63138_rdp_netdev *priv = netdev_priv(netdev);
	struct sockaddr *sockaddr = address;
	u8 old_address[ETH_ALEN];
	u32 error;
	int err;

	if (priv->emac_id != BCM63138_RDP_EMAC0)
		return eth_mac_addr(netdev, address);
	if (ether_addr_equal(netdev->dev_addr, sockaddr->sa_data))
		return 0;

	err = eth_prepare_mac_addr_change(netdev, address);
	if (err)
		return err;

	mutex_lock(&priv->rdp->init_lock);
	if (priv->rdp->shutting_down) {
		err = -ESHUTDOWN;
		goto out_unlock;
	}

	ether_addr_copy(old_address, netdev->dev_addr);
	error = bcm63138_rdp_vendor_host_mac_address(sockaddr->sa_data);
	if (error) {
		if (bcm63138_rdp_vendor_host_mac_address(old_address))
			netdev_err(netdev,
				   "failed to restore Runner WAN host MAC after update error\n");
		err = -EIO;
		goto out_unlock;
	}

	eth_commit_mac_addr_change(netdev, address);
	netdev_info(netdev, "Runner WAN host MAC set to %pM\n",
		    netdev->dev_addr);
	err = 0;

out_unlock:
	mutex_unlock(&priv->rdp->init_lock);
	return err;
}

static const struct net_device_ops bcm63138_rdp_netdev_ops = {
	.ndo_open = bcm63138_rdp_net_open,
	.ndo_stop = bcm63138_rdp_net_stop,
	.ndo_start_xmit = bcm63138_rdp_net_xmit,
	.ndo_set_mac_address = bcm63138_rdp_net_set_mac_address,
	.ndo_validate_addr = eth_validate_addr,
};

static void bcm63138_rdp_adjust_wan_link(struct net_device *netdev)
{
	struct bcm63138_rdp_netdev *priv = netdev_priv(netdev);
	struct bcm63138_rdp *rdp = priv->rdp;
	struct phy_device *phydev = rdp->wan_phy;
	u32 command;

	command = bcm63138_rdp_unimac_read(
		rdp, BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_CMD);
	command &= ~(BCM63138_UNIMAC_CMD_TX_EN |
		     BCM63138_UNIMAC_CMD_RX_EN |
		     BCM63138_UNIMAC_CMD_SPEED_MASK |
		     BCM63138_UNIMAC_CMD_HD_EN);

	switch (phydev->speed) {
	case SPEED_1000:
		command |= 2 << 2;
		break;
	case SPEED_100:
		command |= 1 << 2;
		break;
	case SPEED_10:
	default:
		break;
	}

	if (phydev->duplex == DUPLEX_HALF)
		command |= BCM63138_UNIMAC_CMD_HD_EN;
	if (phydev->link)
		command |= BCM63138_UNIMAC_CMD_TX_EN |
			   BCM63138_UNIMAC_CMD_RX_EN;

	bcm63138_rdp_unimac_write(
		rdp, BCM63138_UNIMAC0_BASE + BCM63138_UNIMAC_CMD, command);
	phy_print_status(phydev);
}

static void bcm63138_rdp_phy_disconnect(void *data)
{
	phy_disconnect(data);
}

static int bcm63138_rdp_inetaddr_event(struct notifier_block *nb,
				       unsigned long event, void *data)
{
	struct bcm63138_rdp *rdp =
		container_of(nb, struct bcm63138_rdp, inetaddr_nb);
	struct in_ifaddr *ifa = data;
	u32 address;
	u32 error;

	if (!rdp->netdev[BCM63138_RDP_EMAC0] ||
	    ifa->ifa_dev->dev != rdp->netdev[BCM63138_RDP_EMAC0])
		return NOTIFY_DONE;

	if (event == NETDEV_UP) {
		address = be32_to_cpu(ifa->ifa_local);
		error = bcm63138_rdp_vendor_ipv4_host_address(address);
		if (error) {
			dev_err(rdp->dev,
				"failed to program Runner WAN IPv4 host %pI4: %u\n",
				&ifa->ifa_local, error);
			return NOTIFY_DONE;
		}
		rdp->wan_ipv4_address = ifa->ifa_local;
		dev_info(rdp->dev, "Runner WAN IPv4 host set to %pI4\n",
			 &ifa->ifa_local);
	} else if (event == NETDEV_DOWN &&
		   rdp->wan_ipv4_address == ifa->ifa_local) {
		error = bcm63138_rdp_vendor_ipv4_host_address(0);
		if (error)
			dev_err(rdp->dev,
				"failed to clear Runner WAN IPv4 host: %u\n",
				error);
		else
			rdp->wan_ipv4_address = 0;
	}

	return NOTIFY_DONE;
}

static void bcm63138_rdp_unregister_inetaddr_notifier(void *data)
{
	struct bcm63138_rdp *rdp = data;

	unregister_inetaddr_notifier(&rdp->inetaddr_nb);
}

static void bcm63138_rdp_napi_delete(void *data)
{
	struct bcm63138_rdp *rdp = data;

	if (!rdp->napi_added)
		return;

	netif_napi_del(&rdp->napi);
	rdp->napi_added = false;
}

static void bcm63138_rdp_napi_enable(struct bcm63138_rdp *rdp)
{
	if (rdp->napi_enabled)
		return;

	napi_enable(&rdp->napi);
	rdp->napi_enabled = true;
}

static int bcm63138_rdp_auto_start(struct device *dev)
{
	static const struct {
		ssize_t (*store)(struct device *, struct device_attribute *,
				 const char *, size_t);
		const char *value;
	} steps[] = {
		{ data_path_init_store, "1" },
		{ unimac0_setup_store, "1" },
		{ unimac1_setup_store, "1" },
		{ runner_execution_store, "1" },
		{ wan1_fifo_init_store, "1" },
		{ cpu_ring_setup_store, "1" },
		{ cpu_irq_setup_store, "1" },
		{ cpu_ring_polling_store, "1" },
		{ broadcast_trap_store, "1" },
		{ ethernet_sources_store, "3" },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(steps); i++) {
		ssize_t err = steps[i].store(dev, NULL, steps[i].value, 1);

		if (err < 0)
			return err;
	}

	return 0;
}

static int bcm63138_rdp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *sf2_pdev;
	struct device_node *sf2_node;
	static const u8 mac_address[ETH_ALEN] = {
		0x98, 0x77, 0xe7, 0x45, 0x62, 0xde,
	};
	struct bcm63138_rdp *rdp;
	struct bcm63138_rdp_netdev *priv;
	struct net_device *netdev;
	struct device_node *phy_node;
	struct resource *res;
	phy_interface_t phy_mode;
	int err;

	sf2_node = of_parse_phandle(dev->of_node, "brcm,sf2", 0);
	if (!sf2_node)
		return dev_err_probe(dev, -EINVAL,
				     "missing brcm,sf2 dependency\n");

	sf2_pdev = of_find_device_by_node(sf2_node);
	of_node_put(sf2_node);
	if (!sf2_pdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for SF2 device\n");

	if (!device_is_bound(&sf2_pdev->dev)) {
		put_device(&sf2_pdev->dev);
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "waiting for SF2 initialization\n");
	}

	if (!device_link_add(dev, &sf2_pdev->dev,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(&sf2_pdev->dev);
		return -ENOMEM;
	}
	put_device(&sf2_pdev->dev);
	int i;

	rdp = devm_kzalloc(dev, sizeof(*rdp), GFP_KERNEL);
	if (!rdp)
		return -ENOMEM;
	rdp->dev = dev;
	mutex_init(&rdp->init_lock);
	spin_lock_init(&rdp->cpu_rx_lock);
	INIT_DELAYED_WORK(&rdp->cpu_rx_work, bcm63138_rdp_cpu_rx_work);
	INIT_WORK(&rdp->cpu_irq_fault_work,
		  bcm63138_rdp_cpu_irq_fault_work);
	atomic_set(&rdp->cpu_irq_count, 0);

	rdp->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(rdp->base))
		return PTR_ERR(rdp->base);

	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32)))
		return dev_err_probe(dev, -EIO,
				     "failed to set 32-bit DMA mask\n");

	if (of_reserved_mem_device_init(dev))
		return dev_err_probe(dev, -ENODEV,
				     "failed to attach Runner DMA pool\n");
	err = devm_add_action_or_reset(
		dev, bcm63138_rdp_reserved_mem_release, dev);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to register Runner DMA pool cleanup\n");

	rdp->tm_base = dmam_alloc_coherent(dev, BCM63138_RDP_TM_SIZE,
					   &rdp->tm_dma, GFP_KERNEL);
	if (!rdp->tm_base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate Runner TM memory\n");

	rdp->mc_base = dmam_alloc_coherent(dev, BCM63138_RDP_MC_SIZE,
					   &rdp->mc_dma, GFP_KERNEL);
	if (!rdp->mc_base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate Runner multicast memory\n");

	for (i = 0; i < BCM63138_RUNNER_IRQS; i++) {
		rdp->irq[i] = platform_get_irq_optional(pdev, i);
		if (rdp->irq[i] == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (rdp->irq[i] < 0)
			break;
	}

	bcm63138_rdp_io_base = rdp->base;
	bcm63138_rdp_active = rdp;
	platform_set_drvdata(pdev, rdp);

	err = bcm63138_rdp_auto_start(dev);
	if (err) {
		bcm63138_rdp_shutdown(rdp);
		return dev_err_probe(dev, err,
				     "failed to start Runner Ethernet\n");
	}

	netdev = devm_alloc_etherdev(dev, sizeof(*priv));
	if (!netdev) {
		err = -ENOMEM;
		goto stop_rx;
	}
	priv = netdev_priv(netdev);
	priv->rdp = rdp;
	priv->emac_id = BCM63138_RDP_EMAC1;
	rdp->netdev[BCM63138_RDP_EMAC1] = netdev;
	SET_NETDEV_DEV(netdev, dev);
	netdev->netdev_ops = &bcm63138_rdp_netdev_ops;
	netdev->hw_features |= NETIF_F_RXCSUM;
	netdev->features |= NETIF_F_RXCSUM;
	netdev->min_mtu = 68;
	netdev->max_mtu = 1536;
	eth_hw_addr_set(netdev, mac_address);
	netif_carrier_off(netdev);
	netif_napi_add_weight(netdev, &rdp->napi,
			      bcm63138_rdp_cpu_rx_napi_poll,
			      BCM63138_CPU_RX_NAPI_WEIGHT);
	rdp->napi_added = true;
	err = devm_add_action_or_reset(dev, bcm63138_rdp_napi_delete, rdp);
	if (err)
		goto stop_rx;

	err = devm_register_netdev(dev, netdev);
	if (err)
		goto stop_rx;

	netdev = devm_alloc_etherdev(dev, sizeof(*priv));
	if (!netdev) {
		err = -ENOMEM;
		goto stop_rx;
	}
	priv = netdev_priv(netdev);
	priv->rdp = rdp;
	priv->emac_id = BCM63138_RDP_EMAC0;
	rdp->netdev[BCM63138_RDP_EMAC0] = netdev;
	SET_NETDEV_DEV(netdev, dev);
	netdev->netdev_ops = &bcm63138_rdp_netdev_ops;
	netdev->hw_features |= NETIF_F_RXCSUM;
	netdev->features |= NETIF_F_RXCSUM;
	netdev->min_mtu = 68;
	netdev->max_mtu = 1536;
	eth_hw_addr_set(netdev, mac_address);
	netif_carrier_off(netdev);

	err = bcm63138_rdp_vendor_host_mac_address(netdev->dev_addr);
	if (err)
		goto stop_rx;
	dev_info(dev, "Runner WAN host MAC set to %pM\n", netdev->dev_addr);

	err = of_get_phy_mode(dev->of_node, &phy_mode);
	if (err)
		goto lan_only;

	phy_node = of_parse_phandle(dev->of_node, "phy-handle", 0);
	if (!phy_node)
		goto lan_only;
	rdp->wan_phy = of_phy_connect(netdev, phy_node,
				      bcm63138_rdp_adjust_wan_link, 0,
				      phy_mode);
	of_node_put(phy_node);
	if (!rdp->wan_phy)
		goto lan_only;
	err = devm_add_action_or_reset(dev, bcm63138_rdp_phy_disconnect,
				       rdp->wan_phy);
	if (err)
		goto stop_rx;

	err = devm_register_netdev(dev, netdev);
	if (err)
		goto stop_rx;
	phy_attached_info(rdp->wan_phy);

	bcm63138_rdp_napi_enable(rdp);
	err = bcm63138_rdp_set_cpu_irq(rdp, true);
	if (err)
		dev_warn(dev,
			 "Runner CPU RX IRQ unavailable (%d); continuing with polling\n",
			 err);
	err = devm_add_action_or_reset(dev, bcm63138_rdp_shutdown, rdp);
	if (err)
		return err;

	rdp->inetaddr_nb.notifier_call = bcm63138_rdp_inetaddr_event;
	err = register_inetaddr_notifier(&rdp->inetaddr_nb);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to register IPv4 address notifier\n");
	err = devm_add_action_or_reset(
		dev, bcm63138_rdp_unregister_inetaddr_notifier, rdp);
	if (err)
		return err;

	err = devm_device_add_group(dev, &bcm63138_rdp_group);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to create state attributes\n");

	dev_info(dev,
		 "registered %s (LAN) and %s (blue WAN) with %d Runner IRQs; TM=%pad MC=%pad\n",
		 rdp->netdev[BCM63138_RDP_EMAC1]->name,
		 rdp->netdev[BCM63138_RDP_EMAC0]->name,
		 i, &rdp->tm_dma, &rdp->mc_dma);

	return 0;

lan_only:
	rdp->netdev[BCM63138_RDP_EMAC0] = NULL;
	bcm63138_rdp_napi_enable(rdp);
	err = bcm63138_rdp_set_cpu_irq(rdp, true);
	if (err)
		dev_warn(dev,
			 "Runner CPU RX IRQ unavailable (%d); continuing with polling\n",
			 err);
	err = devm_add_action_or_reset(dev, bcm63138_rdp_shutdown, rdp);
	if (err)
		return err;
	err = devm_device_add_group(dev, &bcm63138_rdp_group);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to create state attributes\n");
	dev_warn(dev, "WAN PHY unavailable; registered %s (LAN) only\n",
		 rdp->netdev[BCM63138_RDP_EMAC1]->name);
	return 0;

stop_rx:
	bcm63138_rdp_shutdown(rdp);
	return dev_err_probe(dev, err, "failed to register Ethernet devices\n");
}

static const struct of_device_id bcm63138_rdp_of_match[] = {
	{ .compatible = "brcm,bcm63138-rdp" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm63138_rdp_of_match);

static void bcm63138_rdp_remove(struct platform_device *pdev)
{
	bcm63138_rdp_shutdown(platform_get_drvdata(pdev));
}

static struct platform_driver bcm63138_rdp_driver = {
	.probe = bcm63138_rdp_probe,
	.remove = bcm63138_rdp_remove,
	.driver = {
		.name = "bcm63138-rdp",
		.of_match_table = bcm63138_rdp_of_match,
	},
};
module_platform_driver(bcm63138_rdp_driver);

MODULE_DESCRIPTION("Broadcom BCM63138 Runner discovery driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(BCM63138_FW_RUNNER_B);
MODULE_FIRMWARE(BCM63138_FW_RUNNER_C);
MODULE_FIRMWARE(BCM63138_FW_RUNNER_D);
MODULE_FIRMWARE(BCM63138_FW_PREDICT_B);
MODULE_FIRMWARE(BCM63138_FW_PREDICT_C);
MODULE_FIRMWARE(BCM63138_FW_PREDICT_D);
