#include <stdint.h>

#ifndef LOAD_ADDRESS
#define LOAD_ADDRESS 0x10000000u
#endif

#ifndef MAX_LOAD_SIZE
#define MAX_LOAD_SIZE (96u * 1024u * 1024u)
#endif

#define LOADER_STACK 0x0f000000u
#define UART_BASE 0xfffe8600u
#define UART_BAUDWORD (*(volatile uint32_t *)(UART_BASE + 0x04u))
#define UART_FIFO_LEVEL (*(volatile uint32_t *)(UART_BASE + 0x08u))
#define UART_DATA (*(volatile uint32_t *)(UART_BASE + 0x14u))
#define UART_FIFO_SIZE 16u
#define TRANSFER_BAUDWORD 27u
#define TRANSFER_BLOCK_SIZE 1024u
#define FRAME_MAGIC 0x214d5246u
#define ACK_MAGIC 0x214b4341u
#define NAK_MAGIC 0x214b414eu
#define REPAIR_SIZE (*(volatile uint32_t *)0x00f0097cu)
#define REPAIR_STATE (*(volatile uint32_t *)0x00f00980u)
#define REPAIR_READY 0x44524752u

static __attribute__((always_inline)) inline uint32_t
save_and_disable_interrupts(void)
{
	uint32_t cpsr;

	__asm__ volatile(
		"mrs %0, cpsr\n"
		"cpsid if\n"
		: "=r"(cpsr)
		:
		: "cc", "memory"
	);
	return cpsr;
}

static __attribute__((always_inline)) inline void
restore_interrupts(uint32_t cpsr)
{
	__asm__ volatile(
		"msr cpsr_c, %0\n"
		:
		: "r"(cpsr)
		: "cc", "memory"
	);
}

static __attribute__((always_inline)) inline uint8_t read_byte(void)
{
	while (((UART_FIFO_LEVEL >> 16) & 0xffu) == 0) {
	}
	return (uint8_t)UART_DATA;
}

static __attribute__((always_inline)) inline void write_byte(uint8_t value)
{
	while (((UART_FIFO_LEVEL >> 24) & 0xffu) >= UART_FIFO_SIZE) {
	}
	UART_DATA = value;
}

static __attribute__((always_inline)) inline void wait_tx_empty(void)
{
	while (((UART_FIFO_LEVEL >> 24) & 0xffu) != 0) {
	}
}

static __attribute__((always_inline)) inline void write_text(const char *text)
{
	while (*text != '\0') {
		write_byte((uint8_t)*text++);
	}
}

static void write_repeated(const char *text)
{
	unsigned int repeat;

	for (repeat = 0; repeat < 8; repeat++)
		write_text(text);
}

static void write_u32_le(uint32_t value)
{
	write_byte((uint8_t)value);
	write_byte((uint8_t)(value >> 8));
	write_byte((uint8_t)(value >> 16));
	write_byte((uint8_t)(value >> 24));
}

static void write_response(uint32_t magic, uint32_t block)
{
	unsigned int repeat;

	for (repeat = 0; repeat < 8; repeat++) {
		write_u32_le(magic);
		write_u32_le(block);
		write_u32_le(~block);
	}
}

static uint32_t read_u32_le(void)
{
	uint32_t value = read_byte();

	value |= (uint32_t)read_byte() << 8;
	value |= (uint32_t)read_byte() << 16;
	value |= (uint32_t)read_byte() << 24;
	return value;
}

static void wait_for_transfer_sync(void)
{
	while (read_u32_le() != 0xc33caa55u) {
	}
}

static void wait_for_frame(void)
{
	uint32_t value = 0;

	while (value != FRAME_MAGIC)
		value = (value >> 8) | ((uint32_t)read_byte() << 24);
}

static uint32_t crc32_byte(uint32_t crc, uint8_t value)
{
	unsigned int bit;

	crc ^= value;
	for (bit = 0; bit < 8; bit++) {
		uint32_t mask = 0u - (crc & 1u);

		crc = (crc >> 1) ^ (0xedb88320u & mask);
	}
	return crc;
}

static __attribute__((used, noinline)) int receiver_main(void)
{
	uint8_t *destination = (uint8_t *)LOAD_ADDRESS;
	uint32_t expected_crc;
	uint32_t expected_crc_inverse;
	uint32_t load_size;
	uint32_t load_size_inverse;
	uint32_t original_baudword;
	uint32_t full_crc = 0xffffffffu;
	uint32_t received = 0;
	uint32_t block = 0;
	uint32_t cpsr = save_and_disable_interrupts();
	int status = -1;

	write_text("DG2200TN_READY\n");
	load_size = read_u32_le();
	load_size_inverse = read_u32_le();
	expected_crc = read_u32_le();
	expected_crc_inverse = read_u32_le();
	if (load_size_inverse != ~load_size ||
	    expected_crc_inverse != ~expected_crc ||
	    load_size < 20u || load_size > MAX_LOAD_SIZE) {
		write_text("DG2200TN_SIZE_ERROR\n");
		goto out;
	}
	write_repeated("DG2200TN_HEADER_OK\n");
	wait_tx_empty();
	original_baudword = UART_BAUDWORD;
	UART_BAUDWORD = TRANSFER_BAUDWORD;
	wait_for_transfer_sync();
	write_repeated("DG2200TN_B\n");

	while (received < load_size) {
		uint32_t frame_block;
		uint32_t frame_block_inverse;
		uint32_t frame_crc;
		uint32_t frame_crc_inverse;
		uint32_t header_valid;
		uint32_t current;
		uint32_t duplicate;
		uint32_t response_block;
		uint32_t frame_offset;
		uint32_t frame_size;
		uint32_t chunk_crc = 0xffffffffu;
		uint32_t candidate_crc = full_crc;
		uint32_t offset;

		wait_for_frame();
		frame_block = read_u32_le();
		frame_block_inverse = read_u32_le();
		frame_crc = read_u32_le();
		frame_crc_inverse = read_u32_le();
		header_valid = frame_block_inverse == ~frame_block &&
			       frame_crc_inverse == ~frame_crc;
		current = header_valid && frame_block == block;
		duplicate = header_valid && block != 0 &&
			    frame_block == block - 1;
		frame_offset = duplicate ? frame_block * TRANSFER_BLOCK_SIZE :
					   received;
		frame_size = load_size - frame_offset;
		if (frame_size > TRANSFER_BLOCK_SIZE)
			frame_size = TRANSFER_BLOCK_SIZE;

		for (offset = 0; offset < frame_size; offset++) {
			uint8_t value = read_byte();

			destination[frame_offset + offset] = value;
			chunk_crc = crc32_byte(chunk_crc, value);
			candidate_crc = crc32_byte(candidate_crc, value);
		}
		chunk_crc ^= 0xffffffffu;
		response_block = current || duplicate ? frame_block : block;
		if ((current || duplicate) && chunk_crc == frame_crc) {
			if (current) {
				full_crc = candidate_crc;
				received += frame_size;
				block++;
			}
			write_response(ACK_MAGIC, response_block);
		} else {
			write_response(NAK_MAGIC, response_block);
		}
	}
	full_crc ^= 0xffffffffu;
	if (full_crc != expected_crc) {
		write_repeated("DG2200TN_CRC_ERROR\n");
	} else {
		REPAIR_SIZE = load_size;
		REPAIR_STATE = REPAIR_READY;
		write_repeated("DG2200TN_CRC_OK\n");
		status = 0;
	}
	write_repeated("DG2200TN_D\n");
	wait_tx_empty();
	UART_BAUDWORD = original_baudword;

out:
	restore_interrupts(cpsr);
	return status;
}

__attribute__((naked, section(".text.start")))
void receiver(void)
{
	__asm__ volatile(
#ifdef CFE_COMMAND_HOOK
		"push {r4, lr}\n"
		"bl receiver_main\n"
		"pop {r4, pc}\n"
#else
		"push {r4-r11, lr}\n"
		"mov r4, sp\n"
		"ldr sp, =%c0\n"
		"bl receiver_main\n"
		"mov sp, r4\n"
		"pop {r4-r11, pc}\n"
#endif
		:
		: "i"(LOADER_STACK)
	);
}
