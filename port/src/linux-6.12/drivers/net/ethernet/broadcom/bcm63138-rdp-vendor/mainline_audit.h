/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BCM63138_RDP_MAINLINE_AUDIT_H
#define BCM63138_RDP_MAINLINE_AUDIT_H

#include <linux/types.h>

struct bcm63138_rdp_audit {
	u32 ds_bpm_base;
	u32 ds_bpm_extra_base;
	u32 ds_bpm_optimized_base;
	u32 us_bpm_base;
	u32 us_bpm_extra_base;
	u32 us_bpm_optimized_base;
	u32 cpu_tx_data_table;
	u32 cpu_tx_free_index_table;
	u32 cpu_tx_free_index_last;
	u32 cpu_tx_data_entries[4];
	u32 cpu_tx_data_test_entry;
	u32 ih_iq_base;
	u32 ih_iq_size;
	u32 ih_runner_a_base;
	u32 ih_runner_b_base;
	u32 bpm_source_enable;
	u32 bpm_global_threshold;
	u32 sbpm_init;
	u32 sbpm_source_enable;
	u32 dma_source;
	u32 dma_read_base;
	u32 sdma_source;
	u32 sdma_read_base;
	u32 bbh_rx0_ddr;
	u32 bbh_rx0_dma;
	u32 bbh_rx0_sdma;
	u32 bbh_tx0_tm_base;
	u32 bbh_tx0_mc_base;
	u32 bbh_rx1_ddr;
	u32 bbh_rx1_dma;
	u32 bbh_rx1_sdma;
	u32 bbh_tx1_tm_base;
	u32 bbh_tx1_mc_base;
	u32 bbh_rx_packets[2];
	u32 bbh_rx_too_short[2];
	u32 bbh_rx_too_long[2];
	u32 bbh_rx_crc_error[2];
	u32 bbh_rx_runner_congestion[2];
	u32 bbh_rx_no_bpm[2];
	u32 bbh_rx_no_sbpm[2];
	u32 bbh_rx_no_dma[2];
	u32 bbh_rx_no_sdma[2];
	u32 bbh_rx_pd_base[2];
	u32 bbh_rx_runner0_task[2];
	u32 bbh_rx_runner1_task[2];
	u32 bbh_tx_sram[2];
	u32 bbh_tx_ddr[2];
	u32 bbh_tx_dropped[2];
	u8 bbh_tx_task[2];
	u32 ring0_entries_counter;
	u32 ring0_entry_size;
	u32 ring0_entries;
	u32 ring0_pointer;
	u32 ring0_interrupt;
	u32 runner0_interrupt_status;
	u32 runner1_interrupt_status;
	u32 runner0_interrupt_mask;
	u32 runner1_interrupt_mask;
	u32 ds_cpu_tx_fast_descriptor[2];
	u32 ds_cpu_tx_pico_descriptor[2];
	u32 ds_cpu_tx_bbh_descriptor[2];
	u32 ds_cpu_tx_write_ptr;
	u8 ds_cpu_tx_tail;
	u32 ds_cpu_tx_pico_write_ptr;
	u8 ds_cpu_tx_pico_tail;
	u32 runner_a_cpu_tx_r16;
	u32 runner_a_cpu_tx_r8;
	u32 runner_a_cpu_tx_r9;
	u32 runner_a_pico_tx_r16;
	u32 runner_a_pico_tx_r8;
	u32 runner_a_pico_tx_r9;
	u16 emac_queue_mac_pointer[2];
	u16 emac_queue_pointer[2];
	u16 emac_queue_head[2];
	u16 emac_queue_tail[2];
	u16 emac_queue_ingress[2];
	u16 emac_queue_egress[2];
	u16 emac_queue_threshold[2];
	u16 emac_queue_profile[2];
	u8 emac_queue_mask[2];
	u8 emac_queue_index[2];
	u8 emac_mac_ingress[2];
	u8 emac_mac_egress[2];
	u8 emac_mac_tx_task[2];
	u8 emac_mac_queue_status[2];
	u16 emac_mac_counters_pointer[2];
	u32 emac_local_registers[2][2];
	u32 us_cpu_tx_fast_descriptor[2];
	u32 us_cpu_tx_bbh_descriptor[2];
	u32 us_cpu_tx_write_ptr;
	u8 us_cpu_tx_tail;
	u32 us_main_timer_task[2];
	u16 us_main_timer_active_tasks;
	u32 wan_channel0_descriptor[22];
	u32 runner_b_cpu_tx_r16;
	u32 runner_b_wan1_tx_r16;
	u32 runner_b_wan1_tx_r8;
	u32 runner_b_wan1_tx_r9;
	u8 ethwan_tx_bbh_counter;
	u8 ethwan_tx_firmware_counter;
	u16 us_free_pd_guaranteed;
	u16 us_free_pd_non_guaranteed;
	u16 us_free_pd_threshold;
	u16 wan_queue_head;
	u16 wan_queue_tail;
	u16 wan_queue_packets;
	u16 wan_queue_threshold;
	u16 wan_queue_rate_controller;
	u16 wan_queue_mask;
	u16 wan_channel_bbh_destination;
	u32 wan_channel_rate_controllers_status;
	u16 rate_controller_wan_channel;
	u16 rate_controller_queue0;
	u32 wan_flow_raw;
	u32 wan_queue_raw[4];
	u32 rate_controller_raw[8];
	u32 wan_physical_port;
};

struct bcm63138_rdp_rx_coalescing_state {
	u16 current_timeout[2];
	u8 current_packets[2];
	u16 configured_timeout[2];
	u8 configured_packets[2];
	u16 timer_period;
	u16 timer_armed;
};

void bcm63138_rdp_vendor_audit(struct bcm63138_rdp_audit *audit);
u32 bcm63138_rdp_vendor_ring_init(u32 ring_id, u32 ring_dma,
				  u32 entries, u32 entry_size,
				  u32 interrupt_id);
u32 bcm63138_rdp_vendor_rx_coalescing_config(u32 timeout_us,
					     u32 max_packet_count);
void bcm63138_rdp_vendor_rx_coalescing_state(
	struct bcm63138_rdp_rx_coalescing_state *state);
u32 bcm63138_rdp_vendor_connection_tables_init(void);
u32 bcm63138_rdp_vendor_split_cpu_rx_queues(void);
void bcm63138_rdp_vendor_interrupt_clear(u32 interrupt, u32 sub_interrupt);
u32 bcm63138_rdp_vendor_interrupt_mask(u32 interrupt, u32 sub_interrupt);
u32 bcm63138_rdp_vendor_interrupt_unmask(u32 interrupt, u32 sub_interrupt);
u32 bcm63138_rdp_vendor_runner_enable(void);
u32 bcm63138_rdp_vendor_runner_disable(void);
u32 bcm63138_rdp_vendor_ethernet_sources(u32 mask);
u32 bcm63138_rdp_vendor_ethernet_sources_disable(void);
void bcm63138_rdp_vendor_data_path_exit(void);
u32 bcm63138_rdp_vendor_broadcast_trap(bool enable);
u32 bcm63138_rdp_vendor_ipv4_host_address(u32 address);
u32 bcm63138_rdp_vendor_host_mac_address(const u8 *address);
u32 bcm63138_rdp_vendor_lan1_fifo_init(void);
u32 bcm63138_rdp_vendor_wan1_fifo_init(void);
u32 bcm63138_rdp_vendor_wan_tx_base_init(void);
u32 bcm63138_rdp_vendor_wan_tx_scheduler_init(void);
void bcm63138_rdp_vendor_cpu_rx_wakeup(void);
void bcm63138_rdp_vendor_locking_init(void);
u32 bcm63138_rdp_vendor_cpu_tx(const void *packet, u32 length, u8 emac_id);
u32 bcm63138_rdp_vendor_wan_cpu_tx_abs(const void *packet, u32 length);

#endif
