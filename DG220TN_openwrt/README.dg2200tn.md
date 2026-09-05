# DG2200TN OpenWrt port

This branch builds an OpenWrt 25.12 Cortex-A9 soft-float initramfs with LuCI
and embeds it into the BCM63138 Linux 6.12 kernel developed in the adjacent
`port/` tree. The userspace intentionally avoids VFP and NEON because the
BCM63138 secondary CPU has no VFP unit and Linux consequently disables VFP
for userspace on both CPUs.

Build:

```sh
../port/prepare-stock-runner-firmware.py /path/to/stock/rdpa.ko
./build-dg2200tn.sh
```

The extractor keeps the proprietary Runner bytes in the ignored
`port/firmware/` directory. The build script can perform the same preparation
automatically:

```sh
DG2200TN_RDPA_KO=/path/to/stock/rdpa.ko ./build-dg2200tn.sh
```

The generic armsr module tree is built for a different kernel release, so the
final initramfs omits it and contains only modules built against the
board-specific kernel. Required bridge, VLAN, Ethernet, and wireless support
is built into that kernel.

TFTP output:

```text
bin/targets/bcm63138/dg2200tn/openwrt-dg2200tn-tftp.elf
```

The router uses `192.168.1.1/24` on `br-lan`, with `eth0` providing the four
yellow LAN ports. LuCI is available at `http://192.168.1.1/`. On the first
boot, both radios are enabled as `DG2200TN-2G` and `DG2200TN-5G` access points
with one randomly generated WPA2 key. Inspect or replace that key through
LuCI before connecting wireless clients.
