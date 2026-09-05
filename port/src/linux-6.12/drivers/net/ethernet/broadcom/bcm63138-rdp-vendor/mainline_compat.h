#ifndef BCM63138_RDP_MAINLINE_COMPAT_H
#define BCM63138_RDP_MAINLINE_COMPAT_H

#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/io.h>
#include <linux/types.h>

void __iomem *bcm63138_rdp_device_address(uintptr_t address);
void *bcm63138_rdp_dma_alloc(size_t size, dma_addr_t *dma, gfp_t gfp);
void bcm63138_rdp_dma_free(size_t size, void *cpu_addr, dma_addr_t dma);

typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef struct sk_buff *bdmf_sysb;

#define KMALLOC(size, flags)	kmalloc((size), GFP_KERNEL)
#define KFREE(ptr)		kfree(ptr)

#endif
