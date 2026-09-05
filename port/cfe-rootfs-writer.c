#include <stdint.h>

#define LOAD_ADDRESS 0x10000000u
#define ROOTFS_SIZE 0x03fc0000u
#define ROOTFS_FLASH_ADDRESS 0x04a20000u
#define REPAIR_SIZE (*(volatile uint32_t *)0x00f0097cu)
#define REPAIR_STATE (*(volatile uint32_t *)0x00f00980u)
#define REPAIR_READY 0x44524752u
#define UART_BASE 0xfffe8600u
#define UART_FIFO_LEVEL (*(volatile uint32_t *)(UART_BASE + 0x08u))
#define UART_DATA (*(volatile uint32_t *)(UART_BASE + 0x14u))
#define UART_FIFO_SIZE 16u

typedef int (*nand_write_fn)(uint32_t, const void *, uint32_t, int);

static __attribute__((always_inline)) inline void write_byte(uint8_t value)
{
	while (((UART_FIFO_LEVEL >> 24) & 0xffu) >= UART_FIFO_SIZE) {
	}
	UART_DATA = value;
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

static __attribute__((used, noinline)) int writer_main(void)
{
	nand_write_fn nand_write = (nand_write_fn)0x00f1757cu;
	uint8_t *image = (uint8_t *)LOAD_ADDRESS;
	uint32_t size = REPAIR_SIZE;
	uint32_t offset;
	int status;

	if (REPAIR_STATE != REPAIR_READY ||
	    size < 3u * 0x20000u || size > ROOTFS_SIZE ||
	    (size & 0x1ffffu) != 0) {
		write_repeated("DG2200TN_ROOTFS_STATE_ERROR\n");
		return -1;
	}

	REPAIR_STATE = 0;
	write_text("DG2200TN_ROOTFS_WRITING\n");
	for (offset = size; offset < ROOTFS_SIZE; offset++)
		image[offset] = 0xff;
	status = nand_write(ROOTFS_FLASH_ADDRESS, image, ROOTFS_SIZE, 1);
	if (status == 0)
		write_repeated("DG2200TN_ROOTFS_WRITE_OK\n");
	else
		write_repeated("DG2200TN_ROOTFS_WRITE_ERROR\n");
	return status;
}

__attribute__((naked, section(".text.start")))
void receiver(void)
{
	__asm__ volatile(
		"push {r4, lr}\n"
		"bl writer_main\n"
		"pop {r4, pc}\n"
	);
}
