#!/bin/sh
set -eu

[ "$#" -eq 2 ] || {
	echo "usage: $0 KERNEL CONFIG" >&2
	exit 2
}

KERNEL="$1"
CONFIG="$2"
CONFIG_TOOL="$KERNEL/scripts/config"

[ -x "$CONFIG_TOOL" ] || {
	echo "error: missing $CONFIG_TOOL" >&2
	exit 1
}
[ -f "$CONFIG" ] || {
	echo "error: missing $CONFIG" >&2
	exit 1
}

# These foundations must be built in because policy rules are needed before
# any optional package can load its modules.
for option in \
	IP_ADVANCED_ROUTER IP_MULTIPLE_TABLES IP_ROUTE_MULTIPATH \
	IPV6_MULTIPLE_TABLES NF_CONNTRACK_EVENTS \
	NET_SCHED NET_CLS NET_CLS_ACT NET_EMATCH
do
	"$CONFIG_TOOL" --file "$CONFIG" --enable "$option"
done

# Keep optional networking features modular so the compressed kernel retains
# enough room in the fixed 4 MiB boot filesystem.
for option in \
	NF_CT_NETLINK \
	NET_SCH_HFSC NET_SCH_HTB NET_SCH_TBF NET_SCH_INGRESS \
	NET_SCH_FQ_CODEL NET_SCH_CAKE \
	NET_CLS_BASIC NET_CLS_FLOW NET_CLS_FW NET_CLS_ROUTE4 NET_CLS_U32 \
	NET_ACT_GACT NET_ACT_MIRRED NET_ACT_SKBEDIT NET_CLS_MATCHALL \
	NET_EMATCH_U32 \
	IFB TUN \
	NF_SOCKET_IPV4 NF_SOCKET_IPV6 NFT_SOCKET \
	NF_TPROXY_IPV4 NF_TPROXY_IPV6 NFT_TPROXY
do
	"$CONFIG_TOOL" --file "$CONFIG" --module "$option"
done

# This is an always-on NAND appliance. Removing unused boot-time and diagnostic
# facilities offsets the built-in policy-routing and traffic-control core while
# leaving runtime power management enabled.
for option in IP_PNP AIO COREDUMP KEXEC CRASH_DUMP SUSPEND
do
	"$CONFIG_TOOL" --file "$CONFIG" --disable "$option"
done

# Keep struct module identical between the persistent and initramfs kernels so
# the exact release modules can be exercised during the non-destructive test.
for option in \
	BPF_SYSCALL DEBUG_FS FUNCTION_TRACER FTRACE KALLSYMS KPROBES \
	MODULES_TREE_LOOKUP PERF_EVENTS PRINTK_INDEX TRACING
do
	"$CONFIG_TOOL" --file "$CONFIG" --disable "$option"
done

# ARM_DMA_USE_IOMMU adds a pointer to struct dev_archdata and shifts later
# struct net_device fields. Keep it disabled so persistent networking modules
# can be tested safely against the initramfs kernel.
for option in \
	ARM_SMMU EXYNOS_IOMMU IPMMU_VMSA MTK_IOMMU MTK_IOMMU_V1 \
	QCOM_IOMMU ROCKCHIP_IOMMU SUN50I_IOMMU VIDEO_OMAP3 \
	ARM_DMA_USE_IOMMU
do
	"$CONFIG_TOOL" --file "$CONFIG" --disable "$option"
done
