#include <stdint.h>

#define LOAD_ADDRESS 0x0e000000u
#define MAX_LOAD_SIZE 0x00010000u
#define UART_BASE 0xfffe8600u
#define UART_BAUDWORD (*(volatile uint32_t *)(UART_BASE + 0x04u))
#define UART_FIFO_LEVEL (*(volatile uint32_t *)(UART_BASE + 0x08u))
#define UART_DATA (*(volatile uint32_t *)(UART_BASE + 0x14u))
#define UART_FIFO_SIZE 16u
#define TRANSFER_BAUDWORD 27u

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

static void write_text(const char *text)
{
	while (*text != '\0')
		write_byte((uint8_t)*text++);
}

static void write_repeated(const char *text)
{
	unsigned int repeat;

	for (repeat = 0; repeat < 8; repeat++)
		write_text(text);
}

static uint32_t read_u32_le(void)
{
	uint32_t value = read_byte();

	value |= (uint32_t)read_byte() << 8;
	value |= (uint32_t)read_byte() << 16;
	value |= (uint32_t)read_byte() << 24;
	return value;
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

static __attribute__((used, noinline)) int bootstrap_main(void)
{
	uint8_t *destination = (uint8_t *)LOAD_ADDRESS;
	uint32_t size;
	uint32_t size_inverse;
	uint32_t expected_crc;
	uint32_t expected_crc_inverse;
	uint32_t original_baudword;
	uint32_t crc = 0xffffffffu;
	uint32_t received;
	uint32_t cpsr = save_and_disable_interrupts();
	int status = -1;

	write_text("DG2200TN_READY\n");
	size = read_u32_le();
	size_inverse = read_u32_le();
	expected_crc = read_u32_le();
	expected_crc_inverse = read_u32_le();
	if (size < 64u || size > MAX_LOAD_SIZE ||
	    size_inverse != ~size || expected_crc_inverse != ~expected_crc) {
		write_text("DG2200TN_SIZE_ERROR\n");
		goto out;
	}
	write_repeated("DG2200TN_HEADER_OK\n");
	wait_tx_empty();
	original_baudword = UART_BAUDWORD;
	UART_BAUDWORD = TRANSFER_BAUDWORD;
	while (read_u32_le() != 0xc33caa55u) {
	}
	write_repeated("DG2200TN_B\n");

	for (received = 0; received < size; received++) {
		uint8_t value = read_byte();

		destination[received] = value;
		crc = crc32_byte(crc, value);
	}
	if ((crc ^ 0xffffffffu) == expected_crc) {
		write_repeated("DG2200TN_CRC_OK\n");
		status = 0;
	} else {
		write_repeated("DG2200TN_CRC_ERROR\n");
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
		"push {r4, lr}\n"
		"bl bootstrap_main\n"
		"pop {r4, pc}\n"
	);
}
