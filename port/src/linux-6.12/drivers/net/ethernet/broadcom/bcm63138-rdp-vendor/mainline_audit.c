// SPDX-License-Identifier: GPL-2.0-only

#include <linux/etherdevice.h>

#include "mainline_audit.h"
#include "rdd.h"
#include "rdd_bridge.h"
#include "rdd_common.h"
#include "rdd_init.h"
#include "rdd_cpu.h"
#include "rdd_tm.h"
#include "rdp_bbh.h"
#include "rdp_bpm.h"
#include "rdp_dma.h"
#include "rdp_ih.h"
#include "rdp_runner.h"
#include "rdp_sbpm.h"
#include "rdp_drv_bbh.h"
#include "rdp_drv_ih.h"
#include "rdp_drv_sbpm.h"

extern BL_LILAC_RDD_ERROR_DTE
rdd_cpu_tx_write_eth_packet(u8 *packet, u32 length,
			    BL_LILAC_RDD_EMAC_ID_DTE emac_id,
			    u8 wifi_ssid, BL_LILAC_RDD_QUEUE_ID_DTE queue_id);
extern u32 g_runner_tables_ptr;
extern u32 g_runner_ddr_base_addr;
extern u32 g_cpu_tx_queue_write_ptr[4];
extern u32 *g_cpu_tx_data_pointers_reference_array;
extern u32 g_ddr_headroom_size;
extern BL_LILAC_RDD_WAN_PHYSICAL_PORT_DTE g_wan_physical_port;
extern void *bcm63138_rdp_dma_alloc(size_t size, dma_addr_t *dma,
				    gfp_t gfp);
extern void bcm63138_rdp_dma_free(size_t size, void *cpu_addr,
				  dma_addr_t dma);

static u32 saved_bpm_source_enable;
static u32 saved_sbpm_source_enable;
static u32 ethernet_source_mask;
static DEFINE_SPINLOCK(rdd_api_lock);
static DEFINE_SPINLOCK(rdd_api_irq_lock);
static void *wan_abs_tx_buffers;
static dma_addr_t wan_abs_tx_dma;
static u32 wan_abs_tx_slot;
static DEFINE_SPINLOCK(wan_direct_tx_lock);

#define BCM63138_RDD_ETHWAN2_SWITCH_PORT_ADDRESS	0xa928
#define BCM63138_RDP_IH_HOST_MAC_FILTER		3
#define BCM63138_RDP_WAN_ABS_TX_SLOTS		16
#define BCM63138_RDP_WAN_ABS_TX_SLOT_SIZE	2048
#define BCM63138_RDP_WAN_ABS_TX_DATA_OFFSET	18
#define BCM63138_RDP_WAN_ABS_TX_INDEX_BASE	0xb7
#define BCM63138_RDP_WAN_QUEUE_COUNT		8
#define BCM63138_RDD_US_PD_GUARANTEED_PER_QUEUE	16
#define BCM63138_RDD_US_PD_MIN_GUARANTEED	32

static void bcm63138_rdp_vendor_lock(bdmf_fastlock *lock)
{
	spin_lock_bh(&rdd_api_lock);
}

static void bcm63138_rdp_vendor_unlock(bdmf_fastlock *lock)
{
	spin_unlock_bh(&rdd_api_lock);
}

static void bcm63138_rdp_vendor_lock_irq(bdmf_fastlock *lock,
					 unsigned long *flags)
{
	spin_lock_irqsave(&rdd_api_irq_lock, *flags);
}

static void bcm63138_rdp_vendor_unlock_irq(bdmf_fastlock *lock,
					   unsigned long flags)
{
	spin_unlock_irqrestore(&rdd_api_irq_lock, flags);
}

void bcm63138_rdp_vendor_locking_init(void)
{
	rdd_critical_section_config(bcm63138_rdp_vendor_lock,
				    bcm63138_rdp_vendor_unlock,
				    bcm63138_rdp_vendor_lock_irq,
				    bcm63138_rdp_vendor_unlock_irq);
}

void bcm63138_rdp_vendor_audit(struct bcm63138_rdp_audit *audit)
{
	u32 i;

	READ_32(RUNNER_PRIVATE_0_OFFSET + DS_BPM_DDR_BUFFERS_BASE_ADDRESS,
		audit->ds_bpm_base);
	READ_32(RUNNER_PRIVATE_0_OFFSET + DS_BPM_EXTRA_DDR_BUFFERS_BASE_ADDRESS,
		audit->ds_bpm_extra_base);
	READ_32(RUNNER_PRIVATE_0_OFFSET +
		DS_BPM_DDR_OPTIMIZED_BUFFERS_BASE_ADDRESS,
		audit->ds_bpm_optimized_base);
	READ_32(RUNNER_PRIVATE_1_OFFSET + US_BPM_DDR_BUFFERS_BASE_ADDRESS,
		audit->us_bpm_base);
	READ_32(RUNNER_PRIVATE_1_OFFSET + US_BPM_EXTRA_DDR_BUFFERS_BASE_ADDRESS,
		audit->us_bpm_extra_base);
	READ_32(RUNNER_PRIVATE_1_OFFSET +
		US_BPM_DDR_OPTIMIZED_BUFFERS_BASE_ADDRESS,
		audit->us_bpm_optimized_base);
	READ_32(RUNNER_COMMON_0_OFFSET +
		DDR_ADDRESS_FOR_SKB_DATA_POINTERS_TABLE_ADDRESS,
		audit->cpu_tx_data_table);
	READ_32(RUNNER_COMMON_0_OFFSET +
		DDR_ADDRESS_FOR_FREE_SKB_INDEXES_FIFO_TABLE_ADDRESS,
		audit->cpu_tx_free_index_table);
	READ_32(RUNNER_COMMON_0_OFFSET +
		DDR_ADDRESS_FOR_FREE_SKB_INDEXES_FIFO_TABLE_LAST_ENTRY_ADDRESS,
		audit->cpu_tx_free_index_last);
	if (g_cpu_tx_data_pointers_reference_array) {
		for (i = 0; i < ARRAY_SIZE(audit->cpu_tx_data_entries); i++)
			audit->cpu_tx_data_entries[i] =
				be32_to_cpu(READ_ONCE(
					g_cpu_tx_data_pointers_reference_array[i]));
		audit->cpu_tx_data_test_entry =
			be32_to_cpu(READ_ONCE(
				g_cpu_tx_data_pointers_reference_array[
					BCM63138_RDP_WAN_ABS_TX_INDEX_BASE]));
	}

	READ_32(IH_REGS_GENERAL_CONFIGURATION_IQ_BASE_CFG_ADDRESS,
		audit->ih_iq_base);
	READ_32(IH_REGS_GENERAL_CONFIGURATION_IQ_SIZE_CFG_ADDRESS,
		audit->ih_iq_size);
	READ_32(IH_REGS_GENERAL_CONFIGURATION_RNRA_RB_BASE_ADDRESS,
		audit->ih_runner_a_base);
	READ_32(IH_REGS_GENERAL_CONFIGURATION_RNRB_RB_BASE_ADDRESS,
		audit->ih_runner_b_base);

	READ_32(BPM_MODULE_REGS_BPM_SP_EN_ADDRESS, audit->bpm_source_enable);
	READ_32(BPM_MODULE_REGS_BPM_GL_TRSH_ADDRESS,
		audit->bpm_global_threshold);
	READ_32(SBPM_BLOCK_REGS_INIT_FREE_LIST_ADDRESS, audit->sbpm_init);
	READ_32(SBPM_BLOCK_REGS_SBPM_SP_EN_ADDRESS,
		audit->sbpm_source_enable);

	READ_32(DMA_REGS_0_CONFIG_SOURCE_ADDRESS, audit->dma_source);
	READ_I_32(DMA_REGS_0_CONFIG_READ_BASE_ADDRESS, 0,
		  audit->dma_read_base);
	READ_32(DMA_REGS_1_CONFIG_SOURCE_ADDRESS, audit->sdma_source);
	READ_I_32(DMA_REGS_1_CONFIG_READ_BASE_ADDRESS, 0,
		  audit->sdma_read_base);

	READ_32(BBH_RX_0_GENERAL_CONFIGURATION_DDRCFG_ADDRESS,
		audit->bbh_rx0_ddr);
	READ_32(BBH_RX_0_GENERAL_CONFIGURATION_DMAADDR_ADDRESS,
		audit->bbh_rx0_dma);
	READ_32(BBH_RX_0_GENERAL_CONFIGURATION_SDMAADDR_ADDRESS,
		audit->bbh_rx0_sdma);
	READ_32(BBH_TX_0_CONFIGURATIONS_DDRTMBASE_ADDRESS,
		audit->bbh_tx0_tm_base);
	READ_32(BBH_TX_0_CONFIGURATIONS_HNBASE_ADDRESS,
		audit->bbh_tx0_mc_base);

	READ_32(BBH_RX_1_GENERAL_CONFIGURATION_DDRCFG_ADDRESS,
		audit->bbh_rx1_ddr);
	READ_32(BBH_RX_1_GENERAL_CONFIGURATION_DMAADDR_ADDRESS,
		audit->bbh_rx1_dma);
	READ_32(BBH_RX_1_GENERAL_CONFIGURATION_SDMAADDR_ADDRESS,
		audit->bbh_rx1_sdma);
	READ_32(BBH_TX_1_CONFIGURATIONS_DDRTMBASE_ADDRESS,
		audit->bbh_tx1_tm_base);
	READ_32(BBH_TX_1_CONFIGURATIONS_HNBASE_ADDRESS,
		audit->bbh_tx1_mc_base);

	{
		DRV_BBH_RX_COUNTERS counters;
		DRV_BBH_RX_CONFIGURATION configuration;
		BBH_TX_CONFIGURATIONS_TASKLSB tasklsb;
		DRV_BBH_TX_COUNTERS tx_counters;
		unsigned int port;

		for (port = 0; port < 2; port++) {
			BBH_TX_CONFIGURATIONS_TASKLSB_READ(port, tasklsb);
			audit->bbh_tx_task[port] = tasklsb.task0;

			memset(&configuration, 0, sizeof(configuration));
			if (!fi_bl_drv_bbh_rx_get_configuration(
				    port, &configuration)) {
				audit->bbh_rx_pd_base[port] =
					configuration
						.pd_fifo_base_address_normal_queue_in_8_byte;
				audit->bbh_rx_runner0_task[port] =
					configuration.runner_0_task_normal_queue;
				audit->bbh_rx_runner1_task[port] =
					configuration.runner_1_task_normal_queue;
			}

			memset(&counters, 0, sizeof(counters));
			if (fi_bl_drv_bbh_rx_get_counters(port, &counters))
				continue;

			audit->bbh_rx_packets[port] =
				counters.incoming_packets;
			audit->bbh_rx_too_short[port] =
				counters.too_short_error;
			audit->bbh_rx_too_long[port] =
				counters.too_long_error;
			audit->bbh_rx_crc_error[port] =
				counters.crc_error;
			audit->bbh_rx_runner_congestion[port] =
				counters.runner_congestion;
			audit->bbh_rx_no_bpm[port] =
				counters.no_bpm_bn_error;
			audit->bbh_rx_no_sbpm[port] =
				counters.no_sbpm_sbn_error;
			audit->bbh_rx_no_dma[port] =
				counters.no_dma_cd_error;
			audit->bbh_rx_no_sdma[port] =
				counters.no_sdma_cd_error;

			memset(&tx_counters, 0, sizeof(tx_counters));
			if (fi_bl_drv_bbh_tx_get_counters(port, &tx_counters))
				continue;
			audit->bbh_tx_sram[port] =
				tx_counters.tx_packets_from_sram;
			audit->bbh_tx_ddr[port] =
				tx_counters.tx_packets_from_ddr;
			audit->bbh_tx_dropped[port] = tx_counters.dropped_pd;
		}
	}

	{
		RDD_RING_DESCRIPTORS_TABLE_DTS *table =
			RDD_RING_DESCRIPTORS_TABLE_PTR();
		RDD_RING_DESCRIPTOR_DTS *ring = &table->entry[0];

		RDD_RING_DESCRIPTOR_ENTRIES_COUNTER_READ(
			audit->ring0_entries_counter, ring);
		RDD_RING_DESCRIPTOR_SIZE_OF_ENTRY_READ(
			audit->ring0_entry_size, ring);
		RDD_RING_DESCRIPTOR_NUMBER_OF_ENTRIES_READ(
			audit->ring0_entries, ring);
		RDD_RING_DESCRIPTOR_RING_POINTER_READ(
			audit->ring0_pointer, ring);
		RDD_RING_DESCRIPTOR_INTERRUPT_ID_READ(
			audit->ring0_interrupt, ring);
	}

	READ_32(RUNNER_REGS_0_CFG_INT_CTRL_ADDRESS,
		audit->runner0_interrupt_status);
	READ_32(RUNNER_REGS_1_CFG_INT_CTRL_ADDRESS,
		audit->runner1_interrupt_status);
	READ_32(RUNNER_REGS_0_CFG_INT_MASK_ADDRESS,
		audit->runner0_interrupt_mask);
	READ_32(RUNNER_REGS_1_CFG_INT_MASK_ADDRESS,
		audit->runner1_interrupt_mask);
	READ_32(RUNNER_PRIVATE_0_OFFSET + CPU_TX_FAST_QUEUE_ADDRESS,
		audit->ds_cpu_tx_fast_descriptor[0]);
	READ_32(RUNNER_PRIVATE_0_OFFSET + CPU_TX_FAST_QUEUE_ADDRESS + 4,
		audit->ds_cpu_tx_fast_descriptor[1]);
	READ_32(RUNNER_PRIVATE_0_OFFSET + CPU_TX_PICO_QUEUE_ADDRESS,
		audit->ds_cpu_tx_pico_descriptor[0]);
	READ_32(RUNNER_PRIVATE_0_OFFSET + CPU_TX_PICO_QUEUE_ADDRESS + 4,
		audit->ds_cpu_tx_pico_descriptor[1]);
	READ_32(RUNNER_PRIVATE_0_OFFSET + DS_CPU_TX_BBH_DESCRIPTORS_ADDRESS,
		audit->ds_cpu_tx_bbh_descriptor[0]);
	READ_32(RUNNER_PRIVATE_0_OFFSET +
		DS_CPU_TX_BBH_DESCRIPTORS_ADDRESS + 4,
		audit->ds_cpu_tx_bbh_descriptor[1]);
	audit->ds_cpu_tx_write_ptr = g_cpu_tx_queue_write_ptr[FAST_RUNNER_A];
	audit->ds_cpu_tx_pico_write_ptr =
		g_cpu_tx_queue_write_ptr[PICO_RUNNER_A];
	{
		RDD_CPU_TX_DESCRIPTOR_QUEUE_TAIL_TABLE_DTS *tail_table =
			RDD_CPU_TX_DESCRIPTOR_QUEUE_TAIL_TABLE_PTR();
		RDD_ETH_TX_QUEUES_POINTERS_TABLE_DTS *queue_pointers =
			RDD_ETH_TX_QUEUES_POINTERS_TABLE_PTR();
		RDD_ETH_TX_MAC_TABLE_DTS *mac_table =
			RDD_ETH_TX_MAC_TABLE_PTR();

		MREAD_8(&tail_table->entry[FAST_RUNNER_A],
			audit->ds_cpu_tx_tail);
		MREAD_8(&tail_table->entry[PICO_RUNNER_A],
			audit->ds_cpu_tx_pico_tail);
		MREAD_8(&tail_table->entry[FAST_RUNNER_B],
			audit->us_cpu_tx_tail);
		for (i = 0; i < ARRAY_SIZE(audit->emac_queue_pointer); i++) {
			BL_LILAC_RDD_EMAC_ID_DTE emac_id =
				BL_LILAC_RDD_EMAC_ID_0 + i;
			RDD_ETH_TX_QUEUE_POINTERS_ENTRY_DTS *queue_pointer =
				&queue_pointers->entry[
					emac_id *
					LILAC_RDD_EMAC_NUMBER_OF_QUEUES];
			RDD_ETH_TX_QUEUE_DESCRIPTOR_DTS *queue;
			RDD_ETH_TX_MAC_DESCRIPTOR_DTS *mac =
				&mac_table->entry[emac_id];

			RDD_ETH_TX_QUEUE_POINTERS_ENTRY_ETH_MAC_POINTER_READ(
				audit->emac_queue_mac_pointer[i],
				queue_pointer);
			RDD_ETH_TX_QUEUE_POINTERS_ENTRY_TX_QUEUE_POINTER_READ(
				audit->emac_queue_pointer[i], queue_pointer);
			queue = (RDD_ETH_TX_QUEUE_DESCRIPTOR_DTS *)
				(DEVICE_ADDRESS(RUNNER_PRIVATE_0_OFFSET) +
				 audit->emac_queue_pointer[i]);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_HEAD_PTR_READ(
				audit->emac_queue_head[i], queue);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_TAIL_PTR_READ(
				audit->emac_queue_tail[i], queue);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_INGRESS_PACKET_COUNTER_READ(
				audit->emac_queue_ingress[i], queue);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_EGRESS_PACKET_COUNTER_READ(
				audit->emac_queue_egress[i], queue);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_PACKET_THRESHOLD_READ(
				audit->emac_queue_threshold[i], queue);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_PROFILE_PTR_READ(
				audit->emac_queue_profile[i], queue);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_QUEUE_MASK_READ(
				audit->emac_queue_mask[i], queue);
			RDD_ETH_TX_QUEUE_DESCRIPTOR_INDEX_READ(
				audit->emac_queue_index[i], queue);
			RDD_ETH_TX_MAC_DESCRIPTOR_INGRESS_COUNTER_READ(
				audit->emac_mac_ingress[i], mac);
			RDD_ETH_TX_MAC_DESCRIPTOR_EGRESS_COUNTER_READ(
				audit->emac_mac_egress[i], mac);
			RDD_ETH_TX_MAC_DESCRIPTOR_TX_TASK_NUMBER_READ(
				audit->emac_mac_tx_task[i], mac);
			RDD_ETH_TX_MAC_DESCRIPTOR_TX_QUEUES_STATUS_READ(
				audit->emac_mac_queue_status[i], mac);
			RDD_ETH_TX_MAC_DESCRIPTOR_PACKET_COUNTERS_PTR_READ(
				audit->emac_mac_counters_pointer[i], mac);
		}
	}
	for (i = 0; i < ARRAY_SIZE(audit->emac_local_registers); i++) {
		BL_LILAC_RDD_EMAC_ID_DTE emac_id =
			BL_LILAC_RDD_EMAC_ID_0 + i;

		READ_32(RUNNER_PRIVATE_0_OFFSET +
			ETH_TX_LOCAL_REGISTERS_ADDRESS +
			emac_id * 2 * sizeof(u32),
			audit->emac_local_registers[i][0]);
		READ_32(RUNNER_PRIVATE_0_OFFSET +
			ETH_TX_LOCAL_REGISTERS_ADDRESS +
			(emac_id * 2 + 1) * sizeof(u32),
			audit->emac_local_registers[i][1]);
	}
	READ_32(RUNNER_CNTXT_MAIN_0_OFFSET +
		CPU_TX_FAST_THREAD_NUMBER * 32 * sizeof(u32) +
		CS_R16 * sizeof(u32), audit->runner_a_cpu_tx_r16);
	READ_32(RUNNER_CNTXT_MAIN_0_OFFSET +
		CPU_TX_FAST_THREAD_NUMBER * 32 * sizeof(u32) +
		CS_R8 * sizeof(u32), audit->runner_a_cpu_tx_r8);
	READ_32(RUNNER_CNTXT_MAIN_0_OFFSET +
		CPU_TX_FAST_THREAD_NUMBER * 32 * sizeof(u32) +
		CS_R9 * sizeof(u32), audit->runner_a_cpu_tx_r9);
	READ_32(RUNNER_CNTXT_PICO_0_OFFSET +
		(CPU_TX_PICO_THREAD_NUMBER - 32) * 32 * sizeof(u32) +
		CS_R16 * sizeof(u32), audit->runner_a_pico_tx_r16);
	READ_32(RUNNER_CNTXT_PICO_0_OFFSET +
		(CPU_TX_PICO_THREAD_NUMBER - 32) * 32 * sizeof(u32) +
		CS_R8 * sizeof(u32), audit->runner_a_pico_tx_r8);
	READ_32(RUNNER_CNTXT_PICO_0_OFFSET +
		(CPU_TX_PICO_THREAD_NUMBER - 32) * 32 * sizeof(u32) +
		CS_R9 * sizeof(u32), audit->runner_a_pico_tx_r9);
	READ_32(RUNNER_PRIVATE_1_OFFSET + US_CPU_TX_FAST_QUEUE_ADDRESS,
		audit->us_cpu_tx_fast_descriptor[0]);
	READ_32(RUNNER_PRIVATE_1_OFFSET + US_CPU_TX_FAST_QUEUE_ADDRESS + 4,
		audit->us_cpu_tx_fast_descriptor[1]);
	READ_32(RUNNER_PRIVATE_1_OFFSET + US_CPU_TX_BBH_DESCRIPTORS_ADDRESS,
		audit->us_cpu_tx_bbh_descriptor[0]);
	READ_32(RUNNER_PRIVATE_1_OFFSET + US_CPU_TX_BBH_DESCRIPTORS_ADDRESS + 4,
		audit->us_cpu_tx_bbh_descriptor[1]);
	audit->us_cpu_tx_write_ptr =
		g_cpu_tx_queue_write_ptr[FAST_RUNNER_B];
	MREAD_32((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_MAIN_TIMER_TASK_DESCRIPTOR_TABLE_ADDRESS,
		  audit->us_main_timer_task[0]);
	MREAD_32((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_MAIN_TIMER_TASK_DESCRIPTOR_TABLE_ADDRESS + sizeof(u32),
		  audit->us_main_timer_task[1]);
	MREAD_16((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_MAIN_TIMER_CONTROL_DESCRIPTOR_ADDRESS,
		  audit->us_main_timer_active_tasks);
	for (i = 0; i < ARRAY_SIZE(audit->wan_channel0_descriptor); i++)
		READ_32(RUNNER_PRIVATE_1_OFFSET +
			WAN_CHANNELS_0_7_TABLE_ADDRESS + i * sizeof(u32),
			audit->wan_channel0_descriptor[i]);
	READ_32(RUNNER_CNTXT_MAIN_1_OFFSET +
		CPU_TX_FAST_THREAD_NUMBER * 32 * sizeof(u32) +
		CS_R16 * sizeof(u32), audit->runner_b_cpu_tx_r16);
	READ_32(RUNNER_CNTXT_MAIN_1_OFFSET +
		WAN1_TX_THREAD_NUMBER * 32 * sizeof(u32) +
		CS_R16 * sizeof(u32), audit->runner_b_wan1_tx_r16);
	READ_32(RUNNER_CNTXT_MAIN_1_OFFSET +
		WAN1_TX_THREAD_NUMBER * 32 * sizeof(u32) +
		CS_R8 * sizeof(u32), audit->runner_b_wan1_tx_r8);
	READ_32(RUNNER_CNTXT_MAIN_1_OFFSET +
		WAN1_TX_THREAD_NUMBER * 32 * sizeof(u32) +
		CS_R9 * sizeof(u32), audit->runner_b_wan1_tx_r9);
	READ_8(RUNNER_PRIVATE_1_OFFSET +
		ETHWAN_ABSOLUTE_TX_BBH_COUNTER_ADDRESS,
		audit->ethwan_tx_bbh_counter);
	READ_8(RUNNER_PRIVATE_1_OFFSET +
		ETHWAN_ABSOLUTE_TX_FIRMWARE_COUNTER_ADDRESS,
		audit->ethwan_tx_firmware_counter);
	MREAD_16((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_FREE_PACKET_DESCRIPTORS_POOL_DESCRIPTOR_ADDRESS,
		  audit->us_free_pd_guaranteed);
	MREAD_16((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_FREE_PACKET_DESCRIPTORS_POOL_DESCRIPTOR_ADDRESS + 2,
		  audit->us_free_pd_non_guaranteed);
	MREAD_16((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_FREE_PACKET_DESCRIPTORS_POOL_DESCRIPTOR_ADDRESS + 4,
		  audit->us_free_pd_threshold);
	{
		RDD_WAN_TX_QUEUE_DESCRIPTOR_DTS *queue =
			(RDD_WAN_TX_QUEUE_DESCRIPTOR_DTS *)
			(DEVICE_ADDRESS(RUNNER_COMMON_1_OFFSET) +
			 WAN_TX_QUEUES_TABLE_ADDRESS - sizeof(RUNNER_COMMON));
		RDD_WAN_CHANNEL_0_7_DESCRIPTOR_DTS *channel =
			(RDD_WAN_CHANNEL_0_7_DESCRIPTOR_DTS *)
			(DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
			 WAN_CHANNELS_0_7_TABLE_ADDRESS);
		RDD_US_RATE_CONTROLLER_DESCRIPTOR_DTS *rate_controller =
			(RDD_US_RATE_CONTROLLER_DESCRIPTOR_DTS *)
			(DEVICE_ADDRESS(RUNNER_COMMON_1_OFFSET) +
			 US_RATE_CONTROLLERS_TABLE_ADDRESS -
			 sizeof(RUNNER_COMMON));

		RDD_WAN_TX_QUEUE_DESCRIPTOR_HEAD_PTR_READ(
			audit->wan_queue_head, queue);
		RDD_WAN_TX_QUEUE_DESCRIPTOR_TAIL_PTR_READ(
			audit->wan_queue_tail, queue);
		RDD_WAN_TX_QUEUE_DESCRIPTOR_PACKET_COUNTER_READ(
			audit->wan_queue_packets, queue);
		RDD_WAN_TX_QUEUE_DESCRIPTOR_PACKET_THRESHOLD_READ(
			audit->wan_queue_threshold, queue);
		RDD_WAN_TX_QUEUE_DESCRIPTOR_RATE_CONTROLLER_PTR_READ(
			audit->wan_queue_rate_controller, queue);
		RDD_WAN_TX_QUEUE_DESCRIPTOR_QUEUE_MASK_READ(
			audit->wan_queue_mask, queue);
		RDD_WAN_CHANNEL_0_7_DESCRIPTOR_BBH_DESTINATION_READ(
			audit->wan_channel_bbh_destination, channel);
		RDD_WAN_CHANNEL_0_7_DESCRIPTOR_RATE_CONTROLLERS_STATUS_READ(
			audit->wan_channel_rate_controllers_status, channel);
		RDD_US_RATE_CONTROLLER_DESCRIPTOR_WAN_CHANNEL_PTR_READ(
			audit->rate_controller_wan_channel, rate_controller);
		RDD_US_RATE_CONTROLLER_DESCRIPTOR_TX_QUEUE_ADDR_READ(
			audit->rate_controller_queue0, rate_controller, 0);
		MREAD_32((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
			  US_WAN_FLOW_TABLE_ADDRESS + 0xef * sizeof(u32),
			  audit->wan_flow_raw);
		for (i = 0; i < ARRAY_SIZE(audit->wan_queue_raw); i++)
			MREAD_32((u8 *)queue + i * sizeof(u32),
				  audit->wan_queue_raw[i]);
		for (i = 0; i < ARRAY_SIZE(audit->rate_controller_raw); i++)
			MREAD_32((u8 *)rate_controller + i * sizeof(u32),
				  audit->rate_controller_raw[i]);
	}
	audit->wan_physical_port = g_wan_physical_port;
}

u32 bcm63138_rdp_vendor_ring_init(u32 ring_id, u32 ring_dma,
				  u32 entries, u32 entry_size,
				  u32 interrupt_id)
{
	return rdd_ring_init(ring_id, (u32 *)(uintptr_t)ring_dma, entries,
			     entry_size, interrupt_id);
}

u32 bcm63138_rdp_vendor_rx_coalescing_config(u32 timeout_us,
					     u32 max_packet_count)
{
	u32 error;

	error = rdd_cpu_rx_interrupt_coalescing_config(0, timeout_us,
						       max_packet_count);
	if (error)
		return error;

	return rdd_cpu_rx_interrupt_coalescing_config(1, timeout_us,
						      max_packet_count);
}

void bcm63138_rdp_vendor_rx_coalescing_state(
	struct bcm63138_rdp_rx_coalescing_state *state)
{
	RDD_INTERRUPT_COALESCING_CONFIG_TABLE_DTS *table =
		RDD_INTERRUPT_COALESCING_CONFIG_TABLE_PTR();
	u32 ring_id;

	for (ring_id = 0; ring_id < 2; ring_id++) {
		RDD_INTERRUPT_COALESCING_CONFIG_DTS *config =
			&table->entry[ring_id];

		RDD_INTERRUPT_COALESCING_CONFIG_CURRENT_TIMEOUT_READ(
			state->current_timeout[ring_id], config);
		RDD_INTERRUPT_COALESCING_CONFIG_CURRENT_PACKET_COUNT_READ(
			state->current_packets[ring_id], config);
		RDD_INTERRUPT_COALESCING_CONFIG_CONFIGURED_TIMEOUT_READ(
			state->configured_timeout[ring_id], config);
		RDD_INTERRUPT_COALESCING_CONFIG_CONFIGURED_MAX_PACKET_COUNT_READ(
			state->configured_packets[ring_id], config);
	}
	MREAD_16((u8 *)DEVICE_ADDRESS(RUNNER_COMMON_0_OFFSET) +
		  INTERRUPT_COALESCING_TIMER_PERIOD_ADDRESS,
		  state->timer_period);
	MREAD_16((u8 *)DEVICE_ADDRESS(RUNNER_COMMON_0_OFFSET) +
		  INTERRUPT_COALESCING_TIMER_ARMED_ADDRESS,
		  state->timer_armed);
}

u32 bcm63138_rdp_vendor_connection_tables_init(void)
{
	RUNNER_REGS_CFG_CAM_CFG cam = {};
	u32 address;

	cam.stop_value = 0xffff;
	RUNNER_REGS_0_CFG_CAM_CFG_WRITE(cam);
	RUNNER_REGS_1_CFG_CAM_CFG_WRITE(cam);

	address = VIRT_TO_PHYS(g_runner_tables_ptr +
			       DS_CONNECTION_TABLE_ADDRESS);
	MWRITE_32((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_0_OFFSET) +
		  DS_CONNECTION_TABLE_CONFIG_ADDRESS, address);

	address = VIRT_TO_PHYS(g_runner_tables_ptr +
			       US_CONNECTION_TABLE_ADDRESS);
	MWRITE_32((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_CONNECTION_TABLE_CONFIG_ADDRESS, address);

	address = VIRT_TO_PHYS(g_runner_tables_ptr + CONTEXT_TABLE_ADDRESS);
	MWRITE_32((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_0_OFFSET) +
		  DS_CONTEXT_TABLE_CONFIG_ADDRESS, address);
	MWRITE_32((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		  US_CONTEXT_TABLE_CONFIG_ADDRESS, address);

	return BL_LILAC_RDD_OK;
}

u32 bcm63138_rdp_vendor_split_cpu_rx_queues(void)
{
	u32 error = 0;
	u32 reason;

	for (reason = 0; reason < rdpa_cpu_reason__num_of; reason++) {
		error |= rdd_cpu_reason_to_cpu_rx_queue(
			reason, 0, rdpa_dir_us, 0);
		error |= rdd_cpu_reason_to_cpu_rx_queue(
			reason, 0, rdpa_dir_ds, 0);
		error |= rdd_cpu_reason_to_cpu_rx_queue(
			reason, 1, rdpa_dir_ds,
			CPU_REASON_WAN1_TABLE_INDEX);
	}

	return error;
}

void bcm63138_rdp_vendor_interrupt_clear(u32 interrupt, u32 sub_interrupt)
{
	rdd_interrupt_clear(interrupt, sub_interrupt);
}

u32 bcm63138_rdp_vendor_interrupt_mask(u32 interrupt, u32 sub_interrupt)
{
	return rdd_interrupt_mask(interrupt, sub_interrupt);
}

u32 bcm63138_rdp_vendor_interrupt_unmask(u32 interrupt, u32 sub_interrupt)
{
	return rdd_interrupt_unmask(interrupt, sub_interrupt);
}

u32 bcm63138_rdp_vendor_runner_enable(void)
{
	return rdd_runner_enable();
}

u32 bcm63138_rdp_vendor_runner_disable(void)
{
	return rdd_runner_disable();
}

u32 bcm63138_rdp_vendor_ethernet_sources(u32 mask)
{
	BPM_MODULE_REGS_BPM_SP_EN bpm;
	DRV_SBPM_SP_ENABLE sbpm = {};

	if (mask > 3)
		return 1;
	if (mask && !ethernet_source_mask) {
		READ_32(BPM_MODULE_REGS_BPM_SP_EN_ADDRESS,
			saved_bpm_source_enable);
		READ_32(SBPM_BLOCK_REGS_SBPM_SP_EN_ADDRESS,
			saved_sbpm_source_enable);
	}
	if (!mask) {
		if (!ethernet_source_mask)
			return 1;

		WRITE_32(SBPM_BLOCK_REGS_SBPM_SP_EN_ADDRESS,
			 saved_sbpm_source_enable);
		WRITE_32(BPM_MODULE_REGS_BPM_SP_EN_ADDRESS,
			 saved_bpm_source_enable);
		ethernet_source_mask = 0;
		return 0;
	}

	if (mask == BIT(1)) {
		u32 cfe_sources = 0x1b;

		WRITE_32(BPM_MODULE_REGS_BPM_SP_EN_ADDRESS, cfe_sources);
		WRITE_32(SBPM_BLOCK_REGS_SBPM_SP_EN_ADDRESS, cfe_sources);
		ethernet_source_mask = mask;
		return 0;
	}

	WRITE_32(BPM_MODULE_REGS_BPM_SP_EN_ADDRESS,
		 saved_bpm_source_enable);
	BPM_MODULE_REGS_BPM_SP_EN_READ(bpm);
	bpm.rnra_en = 1;
	bpm.rnrb_en = 1;
	bpm.emac0_en = !!(mask & BIT(0));
	bpm.emac1_en = !!(mask & BIT(1));
	BPM_MODULE_REGS_BPM_SP_EN_WRITE(bpm);

	sbpm.rnra_sp_enable = 1;
	sbpm.rnrb_sp_enable = 1;
	sbpm.eth0_sp_enable = !!(mask & BIT(0));
	sbpm.eth1_sp_enable = !!(mask & BIT(1));
	if (fi_bl_drv_sbpm_sp_enable(&sbpm)) {
		WRITE_32(SBPM_BLOCK_REGS_SBPM_SP_EN_ADDRESS,
			 saved_sbpm_source_enable);
		WRITE_32(BPM_MODULE_REGS_BPM_SP_EN_ADDRESS,
			 saved_bpm_source_enable);
		return 1;
	}

	ethernet_source_mask = mask;
	return 0;
}

u32 bcm63138_rdp_vendor_ethernet_sources_disable(void)
{
	BPM_MODULE_REGS_BPM_SP_EN bpm;
	SBPM_BLOCK_REGS_SBPM_SP_EN sbpm;

	BPM_MODULE_REGS_BPM_SP_EN_READ(bpm);
	bpm.emac0_en = 0;
	bpm.emac1_en = 0;
	BPM_MODULE_REGS_BPM_SP_EN_WRITE(bpm);

	SBPM_BLOCK_REGS_SBPM_SP_EN_READ(sbpm);
	sbpm.eth0_sp_en = 0;
	sbpm.eth1_sp_en = 0;
	SBPM_BLOCK_REGS_SBPM_SP_EN_WRITE(sbpm);

	ethernet_source_mask = 0;
	return 0;
}

void bcm63138_rdp_vendor_data_path_exit(void)
{
	void *buffers;
	dma_addr_t buffers_dma;
	unsigned long flags;

	spin_lock_irqsave(&rdd_api_irq_lock, flags);
	buffers = wan_abs_tx_buffers;
	buffers_dma = wan_abs_tx_dma;
	wan_abs_tx_buffers = NULL;
	wan_abs_tx_dma = 0;
	wan_abs_tx_slot = 0;
	spin_unlock_irqrestore(&rdd_api_irq_lock, flags);

	if (buffers)
		bcm63138_rdp_dma_free(
			BCM63138_RDP_WAN_ABS_TX_SLOTS *
				BCM63138_RDP_WAN_ABS_TX_SLOT_SIZE,
			buffers, buffers_dma);

	rdd_exit();
	saved_bpm_source_enable = 0;
	saved_sbpm_source_enable = 0;
	ethernet_source_mask = 0;
}

u32 bcm63138_rdp_vendor_broadcast_trap(bool enable)
{
	RDD_BRIDGE_CONFIGURATION_REGISTER_DTS *us_bridge;
	u8 command = enable ? BL_LILAC_RDD_UNKNOWN_MAC_CMD_CPU_TRAP :
			      BL_LILAC_RDD_UNKNOWN_MAC_CMD_FORWARD;
	u32 error = 0;

	us_bridge = (RDD_BRIDGE_CONFIGURATION_REGISTER_DTS *)
		(DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		 US_BRIDGE_CONFIGURATION_REGISTER_ADDRESS);
	RDD_BRIDGE_CONFIGURATION_REGISTER_ETH0_UNKNOWN_DA_COMMAND_WRITE(
		command, us_bridge);
	RDD_BRIDGE_CONFIGURATION_REGISTER_ETH1_UNKNOWN_DA_COMMAND_WRITE(
		command, us_bridge);

	error |= rdd_broadcast_filter_config(
		BL_LILAC_RDD_LAN0_BRIDGE_PORT, 0,
		enable ? BL_LILAC_RDD_FILTER_ENABLE :
			 BL_LILAC_RDD_FILTER_DISABLE,
		BL_LILAC_RDD_FILTER_ACTION_CPU_TRAP);
	error |= rdd_broadcast_filter_config(
		BL_LILAC_RDD_LAN1_BRIDGE_PORT, 0,
		enable ? BL_LILAC_RDD_FILTER_ENABLE :
			 BL_LILAC_RDD_FILTER_DISABLE,
		BL_LILAC_RDD_FILTER_ACTION_CPU_TRAP);

	return error;
}

u32 bcm63138_rdp_vendor_ipv4_host_address(u32 address)
{
	return rdd_ipv4_host_address_table_set(0, address, address ? 1 : 0);
}

u32 bcm63138_rdp_vendor_host_mac_address(const u8 *address)
{
	u8 mac[ETH_ALEN];
	u32 error;

	ether_addr_copy(mac, address);
	error = fi_bl_drv_ih_set_da_filter_without_mask(
		BCM63138_RDP_IH_HOST_MAC_FILTER, mac);
	if (error)
		return error;

	return fi_bl_drv_ih_enable_da_filter(
		BCM63138_RDP_IH_HOST_MAC_FILTER, 1);
}

u32 bcm63138_rdp_vendor_lan1_fifo_init(void)
{
	return rdd_bbh_reset_firmware_fifo_init(
		BL_LILAC_RDD_LAN1_BRIDGE_PORT);
}

u32 bcm63138_rdp_vendor_wan1_fifo_init(void)
{
	RUNNER_REGS_CFG_CPU_WAKEUP wakeup = {};
	u32 *descriptor;
	u32 i;

	descriptor = (u32 *)(DEVICE_ADDRESS(RUNNER_PRIVATE_0_OFFSET) +
			    ETH0_RX_DESCRIPTORS_ADDRESS);
	for (i = 0; i < 32; i++) {
		MWRITE_32(descriptor++, ETH0_RX_DESCRIPTORS_ADDRESS);
		MWRITE_32(descriptor++, BBH_RESET_WORD_1_DESCRIPTOR_VALUE);
	}

	wakeup.req_trgt = WAN1_FILTERS_AND_CLASSIFICATION_THREAD_NUMBER / 32;
	wakeup.thread_num =
		WAN1_FILTERS_AND_CLASSIFICATION_THREAD_NUMBER % 32;
	wakeup.urgent_req = LILAC_RDD_TRUE;
	RUNNER_REGS_0_CFG_CPU_WAKEUP_WRITE(wakeup);

	return BL_LILAC_RDD_OK;
}

void bcm63138_rdp_vendor_cpu_rx_wakeup(void)
{
	RUNNER_REGS_CFG_CPU_WAKEUP wakeup = {};

	wakeup.req_trgt = CPU_RX_THREAD_NUMBER / 32;
	wakeup.thread_num = CPU_RX_THREAD_NUMBER % 32;
	wakeup.urgent_req = 1;
	RUNNER_REGS_1_CFG_CPU_WAKEUP_WRITE(wakeup);
}

u32 bcm63138_rdp_vendor_wan_tx_base_init(void)
{
	RDD_US_WAN_FLOW_TABLE_DTS *flow_table = RDD_US_WAN_FLOW_TABLE_PTR();
	RDD_US_WAN_FLOW_ENTRY_DTS *flow = &flow_table->entry[0xef];
	RDD_WAN_CHANNEL_ID channel;
	u32 error;

	MWRITE_8((u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
			 BCM63138_RDD_ETHWAN2_SWITCH_PORT_ADDRESS,
		 0xff);

	for (channel = RDD_WAN_CHANNEL_1;
	     channel <= RDD_WAN_CHANNEL_16; channel++) {
		error = rdd_wan_channel_set(
			channel, RDD_WAN_CHANNEL_SCHEDULE_PRIORITY,
			RDD_US_PEAK_SCHEDULING_MODE_ROUND_ROBIN);
		if (error)
			return error;

		error = rdd_wan_channel_rate_limiter_config(
			channel, BL_LILAC_RDD_RATE_LIMITER_DISABLE,
			BL_LILAC_RDD_RATE_LIMITER_LOW);
		if (error)
			return error;
	}

	error = rdd_wan_channel_set(
		RDD_WAN_CHANNEL_0, RDD_WAN_CHANNEL_SCHEDULE_PRIORITY,
		RDD_US_PEAK_SCHEDULING_MODE_ROUND_ROBIN);
	if (error)
		return error;

	error = rdd_wan_channel_rate_limiter_config(
		RDD_WAN_CHANNEL_0, BL_LILAC_RDD_RATE_LIMITER_DISABLE,
		BL_LILAC_RDD_RATE_LIMITER_LOW);
	if (error)
		return error;

	memset(flow, 0, sizeof(*flow));
	RDD_US_WAN_FLOW_ENTRY_WAN_PORT_ID_OR_FSTAT_WRITE(0, flow);
	RDD_US_WAN_FLOW_ENTRY_CRC_CALC_WRITE(
		BL_LILAC_RDD_CRC_CALC_ENABLE, flow);
	RDD_US_WAN_FLOW_ENTRY_PBITS_TO_QUEUE_TABLE_INDEX_WRITE(0, flow);
	RDD_US_WAN_FLOW_ENTRY_TRAFFIC_CLASS_TO_QUEUE_TABLE_INDEX_WRITE(0,
								       flow);
	RDD_US_WAN_FLOW_ENTRY_WAN_CHANNEL_ID_WRITE(RDD_WAN_CHANNEL_0, flow);
	RDD_US_WAN_FLOW_ENTRY_PTM_BONDING_WRITE(0, flow);

	return rdd_eth_tx_queue_config(
		BL_LILAC_RDD_EMAC_ID_0, BL_LILAC_RDD_QUEUE_0, 0,
		rdd_queue_profile_disabled, 0);
}

static void bcm63138_rdp_vendor_us_free_pd_pool_init(u16 queue_count)
{
	u8 *descriptor =
		(u8 *)DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) +
		US_FREE_PACKET_DESCRIPTORS_POOL_DESCRIPTOR_ADDRESS;
	u16 guaranteed = queue_count *
			 BCM63138_RDD_US_PD_GUARANTEED_PER_QUEUE;

	if (guaranteed < BCM63138_RDD_US_PD_MIN_GUARANTEED)
		guaranteed = BCM63138_RDD_US_PD_MIN_GUARANTEED;

	MWRITE_16(descriptor, guaranteed);
	MWRITE_16(descriptor + 2,
		  RDD_US_FREE_PACKET_DESCRIPTORS_POOL_SIZE - guaranteed);
	MWRITE_16(descriptor + 4,
		  BCM63138_RDD_US_PD_GUARANTEED_PER_QUEUE);
}

u32 bcm63138_rdp_vendor_wan_tx_scheduler_init(void)
{
	RDD_RATE_CONTROLLER_PARAMS params = {};
	BL_LILAC_RDD_QUEUE_ID_DTE queue;
	u32 error;

	if (!wan_abs_tx_buffers) {
		wan_abs_tx_buffers = bcm63138_rdp_dma_alloc(
			BCM63138_RDP_WAN_ABS_TX_SLOTS *
			BCM63138_RDP_WAN_ABS_TX_SLOT_SIZE,
			&wan_abs_tx_dma, GFP_KERNEL);
		if (!wan_abs_tx_buffers)
			return BL_LILAC_RDD_ERROR_MALLOC_FAILED;
	}

	error = rdd_rate_controller_config(
		RDD_WAN_CHANNEL_0, BL_LILAC_RDD_RATE_CONTROLLER_0, &params);
	if (error)
		return error;

	for (queue = BL_LILAC_RDD_QUEUE_0;
	     queue <= BL_LILAC_RDD_QUEUE_7; queue++) {
		error = rdd_wan_tx_queue_config(
			RDD_WAN_CHANNEL_0, BL_LILAC_RDD_RATE_CONTROLLER_0,
			queue, 128, rdd_queue_profile_disabled, 0);
		if (error)
			return error;
	}

	bcm63138_rdp_vendor_us_free_pd_pool_init(
		BCM63138_RDP_WAN_QUEUE_COUNT);

	params.sustain_budget = 0x0fffe000;
	params.peak_weight = 0x800;
	error = rdd_rate_controller_modify(
		RDD_WAN_CHANNEL_0, BL_LILAC_RDD_RATE_CONTROLLER_0, &params);
	if (error)
		return error;

	return BL_LILAC_RDD_OK;
}

u32 bcm63138_rdp_vendor_wan_cpu_tx_abs(const void *packet, u32 length)
{
	RDD_CPU_TX_DESCRIPTOR_DTS *descriptor;
	RUNNER_REGS_CFG_CPU_WAKEUP wakeup = {};
	unsigned long flags;
	u32 descriptor_word;
	u32 descriptor_word_1;
	u32 descriptor_valid;
	u32 free_index;
	u32 packet_dma;
	u32 queue_offset;
	u32 slot;
	void *buffer;

	if (!wan_abs_tx_buffers ||
	    length > BCM63138_RDP_WAN_ABS_TX_SLOT_SIZE -
		     BCM63138_RDP_WAN_ABS_TX_DATA_OFFSET)
		return BL_LILAC_RDD_ERROR_ILLEGAL_QUEUE_ID;

	spin_lock_irqsave(&rdd_api_irq_lock, flags);

	queue_offset = g_cpu_tx_queue_write_ptr[FAST_RUNNER_B];
	descriptor = (RDD_CPU_TX_DESCRIPTOR_DTS *)
		(DEVICE_ADDRESS(RUNNER_PRIVATE_1_OFFSET) + queue_offset);
	RDD_CPU_TX_DESCRIPTOR_CORE_VALID_READ(descriptor_valid, descriptor);
	if (descriptor_valid) {
		spin_unlock_irqrestore(&rdd_api_irq_lock, flags);
		return BL_LILAC_RDD_ERROR_CPU_TX_QUEUE_FULL;
	}

	if (g_cpu_tx_queue_free_counter[FAST_RUNNER_B] == 0) {
		f_rdd_get_tx_descriptor_free_count(FAST_RUNNER_B, descriptor);
		if (g_cpu_tx_queue_free_counter[FAST_RUNNER_B] == 0) {
			spin_unlock_irqrestore(&rdd_api_irq_lock, flags);
			return BL_LILAC_RDD_ERROR_CPU_TX_QUEUE_FULL;
		}
	}

	slot = wan_abs_tx_slot++ % BCM63138_RDP_WAN_ABS_TX_SLOTS;
	free_index = BCM63138_RDP_WAN_ABS_TX_INDEX_BASE + slot;
	buffer = wan_abs_tx_buffers +
		 slot * BCM63138_RDP_WAN_ABS_TX_SLOT_SIZE;
	memcpy(buffer + BCM63138_RDP_WAN_ABS_TX_DATA_OFFSET,
	       packet, length);
	packet_dma = (u32)wan_abs_tx_dma +
		     slot * BCM63138_RDP_WAN_ABS_TX_SLOT_SIZE +
		     BCM63138_RDP_WAN_ABS_TX_DATA_OFFSET;
	g_cpu_tx_skb_pointers_reference_array[free_index] = 0;
	g_cpu_tx_data_pointers_reference_array[free_index] =
		swap4bytes(packet_dma);

	descriptor_word = 0;
	RDD_CPU_TX_DESCRIPTOR_BPM_BUFFER_NUMBER_L_WRITE(
		descriptor_word, free_index);
	RDD_CPU_TX_DESCRIPTOR_CORE_PAYLOAD_OFFSET_L_WRITE(
		descriptor_word,
		(g_ddr_headroom_size +
		 BCM63138_RDP_WAN_ABS_TX_DATA_OFFSET) / 2);
	RDD_CPU_TX_DESCRIPTOR_US_FAST_TX_QUEUE_L_WRITE(
		descriptor_word, 0);
	descriptor_word_1 = descriptor_word;
	MWRITE_32((u8 *)descriptor + 4, descriptor_word);

	descriptor_word = 0;
	RDD_CPU_TX_DESCRIPTOR_CORE_PACKET_LENGTH_L_WRITE(
		descriptor_word, length + 4);
	RDD_CPU_TX_DESCRIPTOR_CORE_VALID_L_WRITE(
		descriptor_word, LILAC_RDD_TRUE);
	RDD_CPU_TX_DESCRIPTOR_CORE_COMMAND_L_WRITE(
		descriptor_word,
		LILAC_RDD_CPU_TX_COMMAND_ABSOLUTE_ADDRESS_PACKET);
	RDD_CPU_TX_DESCRIPTOR_CORE_TX_QUEUE_L_WRITE(
		descriptor_word, 0);
	RDD_CPU_TX_DESCRIPTOR_US_FAST_UPSTREAM_GEM_FLOW_L_WRITE(
		descriptor_word, 0xef);
	pr_info("bcm63138-rdp: WAN absolute descriptor %08x/%08x data=%08x index=%u\n",
		descriptor_word, descriptor_word_1, packet_dma, free_index);

	g_cpu_tx_queue_write_ptr[FAST_RUNNER_B] +=
		LILAC_RDD_CPU_TX_DESCRIPTOR_SIZE;
	g_cpu_tx_queue_write_ptr[FAST_RUNNER_B] &=
		LILAC_RDD_CPU_TX_QUEUE_SIZE_MASK;
	g_cpu_tx_queue_free_counter[FAST_RUNNER_B]--;

	dma_wmb();
	MWRITE_32((u8 *)descriptor, descriptor_word);

	wakeup.req_trgt = CPU_TX_FAST_THREAD_NUMBER >> 5;
	wakeup.thread_num = CPU_TX_FAST_THREAD_NUMBER & 0x1f;
	wakeup.urgent_req = LILAC_RDD_FALSE;
	RUNNER_REGS_1_CFG_CPU_WAKEUP_WRITE(wakeup);

	spin_unlock_irqrestore(&rdd_api_irq_lock, flags);

	return BL_LILAC_RDD_OK;
}

static u32 bcm63138_rdp_vendor_wan_cpu_tx_interworking(
	const void *packet, u32 length)
{
	RDD_CPU_TX_DESCRIPTOR_DTS *descriptor;
	RUNNER_REGS_CFG_CPU_WAKEUP wakeup = {};
	unsigned long flags;
	u32 bpm_buffer_number;
	u32 descriptor_word;
	u32 descriptor_valid;
	u32 packet_ddr_ptr;

	spin_lock_irqsave(&wan_direct_tx_lock, flags);

	descriptor = (RDD_CPU_TX_DESCRIPTOR_DTS *)
		(DEVICE_ADDRESS(RUNNER_PRIVATE_0_OFFSET) +
		 g_cpu_tx_queue_write_ptr[FAST_RUNNER_A]);
	RDD_CPU_TX_DESCRIPTOR_CORE_VALID_READ(descriptor_valid, descriptor);
	if (descriptor_valid) {
		spin_unlock_irqrestore(&wan_direct_tx_lock, flags);
		return BL_LILAC_RDD_ERROR_CPU_TX_QUEUE_FULL;
	}

	if (fi_bl_drv_bpm_req_buffer(DRV_BPM_SP_SPARE_0,
				     &bpm_buffer_number) !=
	    DRV_BPM_ERROR_NO_ERROR) {
		spin_unlock_irqrestore(&wan_direct_tx_lock, flags);
		return BL_LILAC_RDD_ERROR_BPM_ALLOC_FAIL;
	}

	packet_ddr_ptr = g_runner_ddr_base_addr +
			 bpm_buffer_number * LILAC_RDD_RUNNER_PACKET_BUFFER_SIZE +
			 g_ddr_headroom_size + LILAC_RDD_PACKET_DDR_OFFSET;
	MWRITE_BLK_8((u8 *)packet_ddr_ptr, packet, length);

	descriptor_word =
		RDD_CPU_TX_DESCRIPTOR_IH_CLASS_L_WRITE(
			f_rdd_bridge_port_to_class_id(
				BL_LILAC_RDD_WAN0_BRIDGE_PORT)) |
		RDD_CPU_TX_DESCRIPTOR_BUFFER_NUMBER_L_WRITE(
			bpm_buffer_number) |
		RDD_CPU_TX_DESCRIPTOR_SSID_L_WRITE(0) |
		RDD_CPU_TX_DESCRIPTOR_ABS_FLAG_L_WRITE(LILAC_RDD_FALSE);
	MWRITE_32((u8 *)descriptor + 4, descriptor_word);

	descriptor_word =
		RDD_CPU_TX_DESCRIPTOR_COMMAND_L_WRITE(
			LILAC_RDD_CPU_TX_COMMAND_INTERWORKING_PACKET) |
		RDD_CPU_TX_DESCRIPTOR_PACKET_LENGTH_L_WRITE(length + 4) |
		RDD_CPU_TX_DESCRIPTOR_EMAC_L_WRITE(
			BL_LILAC_RDD_EMAC_ID_0) |
		RDD_CPU_TX_DESCRIPTOR_DS_GEM_FLOW_L_WRITE(0) |
		RDD_CPU_TX_DESCRIPTOR_VALID_L_WRITE(LILAC_RDD_TRUE);

	g_cpu_tx_queue_write_ptr[FAST_RUNNER_A] +=
		LILAC_RDD_CPU_TX_DESCRIPTOR_SIZE;
	g_cpu_tx_queue_write_ptr[FAST_RUNNER_A] &=
		LILAC_RDD_CPU_TX_QUEUE_SIZE_MASK;

	dma_wmb();
	MWRITE_32((u8 *)descriptor, descriptor_word);

	wakeup.req_trgt = CPU_TX_FAST_THREAD_NUMBER >> 5;
	wakeup.thread_num = CPU_TX_FAST_THREAD_NUMBER & 0x1f;
	wakeup.urgent_req = LILAC_RDD_FALSE;
	RUNNER_REGS_0_CFG_CPU_WAKEUP_WRITE(wakeup);

	spin_unlock_irqrestore(&wan_direct_tx_lock, flags);

	return BL_LILAC_RDD_OK;
}

u32 bcm63138_rdp_vendor_cpu_tx(const void *packet, u32 length, u8 emac_id)
{
	BL_LILAC_RDD_EMAC_ID_DTE vendor_emac_id;

	switch (emac_id) {
	case 0:
		return rdd_cpu_tx_write_gpon_packet(
			(u8 *)packet, length, 0xef, RDD_WAN_CHANNEL_0,
			BL_LILAC_RDD_RATE_CONTROLLER_0,
			BL_LILAC_RDD_QUEUE_0, 1);
	case 1:
		vendor_emac_id = BL_LILAC_RDD_EMAC_ID_1;
		break;
	default:
		return BL_LILAC_RDD_ERROR_ILLEGAL_EMAC_ID;
	}

	return rdd_cpu_tx_write_eth_packet(
		(u8 *)packet, length, vendor_emac_id, 0, 0);
}
