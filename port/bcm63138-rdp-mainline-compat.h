#ifndef BCM63138_RDP_MAINLINE_COMPAT_H
#define BCM63138_RDP_MAINLINE_COMPAT_H

#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/types.h>

typedef u8 uint8_t;
typedef u16 uint16_t;
typedef u32 uint32_t;
typedef u64 uint64_t;
typedef struct sk_buff *bdmf_sysb;

#define KMALLOC(size, flags)	kmalloc((size), GFP_KERNEL)
#define KFREE(ptr)		kfree(ptr)

#endif
