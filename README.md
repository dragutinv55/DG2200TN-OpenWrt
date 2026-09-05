# DG2200TN OpenWrt port

This repository contains the source used to build and test OpenWrt 25.12.5
for the Broadcom BCM63138-based DG2200TN router.

## Layout

- `DG220TN_openwrt/` - OpenWrt source, device configuration, root filesystem
  defaults, and the reproducible firmware build script.
- `port/src/linux-6.12/` - custom Linux 6.12 kernel with Ethernet, dual-band
  Wi-Fi, blue Ethernet WAN, USB host, WPS, and reset-button support.
- `port/vendor-bcm63138/` - the modified Broadcom reference header used while
  adapting the Runner data-path source.
- `port/mkelf.py` - creates the CFE-loadable TFTP ELF image.
- `bootloader-dump-tools/` - NAND dumping utilities, including the DG2200TN
  OOB and resumable-dump changes to `cfenand.py`.

Intermediate build trees, private NAND dumps, extracted filesystems,
toolchains, and reverse-engineering workspaces are intentionally excluded.
Recovery artifacts containing temporary password hashes, SSH-key installation
logic, or device-specific filesystem data are also excluded.

## Build

The minimal ready-to-flash UART installation set is included in
`port/images/`, so compiling is optional for a permanent installation:

```text
port/images/dg2200tn-cfe-uart-bootstrap.bin
port/images/dg2200tn-cfe-uart-bootstrap.bin.sha256
port/images/dg2200tn-cfe-uart-loader.bin
port/images/dg2200tn-cfe-uart-loader.bin.sha256
port/images/dg2200tn-cfe-rootfs-loader.bin
port/images/dg2200tn-cfe-rootfs-loader.bin.sha256
port/images/openwrt-dg2200tn-cfe-rootfs.bin.xz
port/images/openwrt-dg2200tn-cfe-rootfs.bin.xz.sha256
port/images/openwrt-dg2200tn-cfe-bootfs-repair.wfi.xz
port/images/openwrt-dg2200tn-cfe-bootfs-repair.wfi.xz.sha256
```

Verify all prebuilt artifacts before use:

```sh
cd port/images
sha256sum -c dg2200tn-cfe-uart-bootstrap.bin.sha256
sha256sum -c dg2200tn-cfe-uart-loader.bin.sha256
sha256sum -c dg2200tn-cfe-rootfs-loader.bin.sha256
sha256sum -c openwrt-dg2200tn-cfe-rootfs.bin.xz.sha256
sha256sum -c openwrt-dg2200tn-cfe-bootfs-repair.wfi.xz.sha256
```

Install the normal OpenWrt build prerequisites and an
`arm-linux-gnueabi-` cross compiler. The exact Runner microcode must be
extracted locally from the owner's stock `rdpa.ko`; proprietary bytes are
written only below the ignored `port/firmware/` directory:

```sh
./port/prepare-stock-runner-firmware.py /path/to/stock/rdpa.ko
```

Then run:

```sh
cd DG220TN_openwrt
./build-dg2200tn.sh
```

Alternatively, set `DG2200TN_RDPA_KO=/path/to/stock/rdpa.ko` and the build
script prepares the local files automatically. The extractor accepts only the
verified DG2200TN module used for this port.

The generic armsr module packages target a different kernel release. The build
therefore removes that incompatible module tree from the final initramfs and
keeps only modules built against the board-specific kernel; required bridge,
VLAN, Ethernet, and wireless support is built in.

The CFE-loadable image is created at:

```text
DG220TN_openwrt/bin/targets/bcm63138/dg2200tn/openwrt-dg2200tn-tftp.elf
```

## UART/TFTP RAM boot

This procedure loads OpenWrt into RAM and does not write NAND. It is the
recommended way to test the port before attempting a permanent installation.

Use a 3.3 V TTL UART adapter at 115200 baud, 8 data bits, no parity, and one
stop bit. Connect GND, router TX to adapter RX, and router RX to adapter TX.
Do not connect the adapter VCC pin, and do not use an RS-232 voltage-level
adapter.

Connect a yellow LAN port directly to the host and configure the host as the
TFTP server. Both addresses are assigned because DG2200TN CFE configurations
have been observed using either subnet:

```sh
sudo ip link set eth1 up
sudo ip address replace 192.168.1.100/24 dev eth1
sudo ip address replace 10.0.0.100/24 dev eth1

mkdir -p port/tftp
cp DG220TN_openwrt/bin/targets/bcm63138/dg2200tn/openwrt-dg2200tn-tftp.elf \
    port/tftp/vmlinux.elf

sudo ./port/run-tftp.sh
```

If the Ethernet or serial device names differ, replace `eth1` and
`/dev/ttyUSB0` in these commands. Open the serial console in another terminal:

```sh
picocom --baud 115200 --flow n --databits 8 --parity n --stopbits 1 \
    /dev/ttyUSB0
```

Power-cycle the router and press a key when CFE prints:

```text
Press any key to stop auto run
```

At the CFE prompt, use the value printed beside `Host IP address`. For example,
if CFE prints `Host IP address : 10.0.0.100`, load and run the ELF with:

```text
r 10.0.0.100:vmlinux.elf
```

CFE should download the ELF by TFTP and boot OpenWrt. Leave both terminals
open while it starts. The router then uses `192.168.1.1/24` on the yellow LAN
ports, and LuCI is available at:

```text
http://192.168.1.1/
```

This initramfs boot is temporary. Rebooting or power-cycling returns to the
stock firmware because NAND is unchanged.

## Permanent UART/CFE installation

The permanent installer uses only a 3.3 V UART connection:

```text
port/install-openwrt-cfe-uart.py
```

Ethernet, TFTP, stock firmware, and USB storage are not used. Installation has
two stages:

1. A 4 MiB JFFS2 bootfs WFI is verified in RAM and written to inactive NAND
   bank 2 with CFE's native `ws` command.
2. A compact UBI image containing only populated rootfs eraseblocks is
   verified in RAM. A second receiver fills the unused partition space with
   `0xff` and calls CFE's lower-level ECC-aware NAND writer for
   `rootfs_update`.

This is necessary because this DG2200TN CFE correctly writes bootfs through
`ws`, but its whole-image path does not preserve all UBI rootfs PEBs. The
staged method is the path verified on hardware. It preserves CFE, NVRAM, the
stock bank, and device-specific calibration data.

A 706-byte bootstrap fits in the verified CFE executable gap and receives the
larger block receiver into RAM. CFE commands remain at 115200 baud, while
binary payloads use 57600 baud. Each 1 KiB payload frame starts with `FRM!` and
contains a protected block number plus CRC32; the router ACKs accepted frames
and the host retries rejected or interrupted frames. The receiver scans for
the next frame marker, so a dropped UART byte cannot permanently shift frame
boundaries. A complete-image CRC32 is still required before either NAND write.

### Requirements and wiring

Install pyserial if it is not already available:

```sh
sudo apt install python3-serial
```

Connect a 3.3 V TTL adapter at 115200 baud, 8N1:

```text
Router GND -> adapter GND
Router TX  -> adapter RX
Router RX  -> adapter TX
```

Do not connect the adapter VCC pin, and do not use an RS-232 voltage-level
adapter.

Verify the prebuilt artifacts:

```sh
cd port/images
sha256sum -c dg2200tn-cfe-uart-bootstrap.bin.sha256
sha256sum -c dg2200tn-cfe-uart-loader.bin.sha256
sha256sum -c dg2200tn-cfe-rootfs-loader.bin.sha256
sha256sum -c openwrt-dg2200tn-cfe-bootfs-repair.wfi.xz.sha256
sha256sum -c openwrt-dg2200tn-cfe-rootfs.bin.xz.sha256
cd ../..
```

### Running the installer

From the repository root:

```sh
./port/install-openwrt-cfe-uart.py --serial /dev/ttyUSB0
```

The `--serial` option may be omitted when exactly one `/dev/ttyUSB*` or
`/dev/ttyACM*` device exists. The installer decompresses the `.xz` images to
temporary host files automatically; do not decompress or rename them before
running it. The installer then verifies:

- BCM63138B0 and DG2200TN identification;
- 256 MiB NAND, 128 KiB eraseblocks, 2 KiB pages, and BCH-4 ECC;
- the known CFE command handler and command-table addresses;
- every injected loader word by reading it back through CFE;
- bootfs WFI size, JFFS2 prefix, tag fields, and JAM CRC32;
- UBI EC/VID headers and the `rootfs` volume record;
- both complete UART transfers with CRC32 calculated independently on the
  router.

The prebuilt images transfer about 17 MiB total and take approximately 82
minutes with the protected 57600-baud block protocol. Progress, speed, and
estimated remaining time are displayed. NAND remains unchanged until the
complete bootfs transfer has been verified and `INSTALL` is entered.

After the bootfs transfer is verified, the installer asks for the exact
confirmation:

```text
INSTALL
```

Anything else cancels without writing NAND. After `INSTALL`, do not disconnect
power. The installer writes bootfs, transfers and verifies rootfs, writes
`rootfs_update`, restores CFE's command pointer, resets, and waits for the
OpenWrt console.

If power or UART is interrupted before `INSTALL`, power-cycle the router and
restart the installer; NAND was not touched. If UART data is corrupted, the
block is retried and the complete-image CRC remains the final gate. If rootfs
transfer is interrupted after bootfs was written successfully, preserve that
bootfs and retry only the rootfs:

```sh
./port/install-openwrt-cfe-uart.py --serial /dev/ttyUSB0 --rootfs-only
```

Power-cycle first and let the installer stop CFE autoboot. The rootfs-only path
does not modify bootfs and does not start its NAND writer until the complete
rootfs CRC passes. Power loss during either NAND write can require recovery.

After a successful installation, normal restarts select the newer bank-2
OpenWrt image automatically. UART is not required for normal operation.

### Updating an existing OpenWrt installation

Firmware flashing through LuCI's **System > Backup / Flash Firmware** page and
the `sysupgrade` command is not supported yet. The generic `armsr` upgrade
handler expects a block device and does not understand the DG2200TN dual-bank
NAND layout. Do not give the RUI, WFI, or compact rootfs files to LuCI or
`sysupgrade`.

Before updating, download a configuration backup from LuCI or save one from
the command line and copy it off the router:

```sh
ssh root@192.168.1.1 'sysupgrade -b /tmp/dg2200tn-backup.tar.gz'
scp root@192.168.1.1:/tmp/dg2200tn-backup.tar.gz .
```

The staged UART installer replaces the writable bank-2 rootfs and does not
automatically preserve configuration. Restore settings selectively after the
update rather than blindly restoring old network files from a release that
used a different interface layout.

For a normal update, verify the new artifacts, connect the 3.3 V UART adapter,
and run the same full installer used for the initial installation:

```sh
cd port/images
sha256sum -c dg2200tn-cfe-uart-loader.bin.sha256
sha256sum -c dg2200tn-cfe-rootfs-loader.bin.sha256
sha256sum -c openwrt-dg2200tn-cfe-bootfs-repair.wfi.xz.sha256
sha256sum -c openwrt-dg2200tn-cfe-rootfs.bin.xz.sha256
cd ../..

./port/install-openwrt-cfe-uart.py --serial /dev/ttyUSB0
```

The full update writes both bank-2 bootfs and rootfs and is the recommended
choice whenever the kernel, device tree, kernel modules, or root filesystem
may have changed.

A rootfs-only update is available for releases that explicitly state that the
installed bootfs is compatible:

```sh
./port/install-openwrt-cfe-uart.py \
    --serial /dev/ttyUSB0 \
    --rootfs-only
```

This reduces the transfer by preserving the existing bootfs, but it still
replaces the bank-2 rootfs and its configuration. Do not use it across kernel
or device-tree changes.

Do not use `--bootfs-only` to preserve configuration. Hardware testing proved
that CFE's `ws` command reinitializes the bank-2 UBI metadata even when the WFI
contains only bootfs. That mode is restricted to staged recovery, requires
`--acknowledge-bootfs-erases-rootfs`, and deliberately leaves the router at
the CFE prompt so `--rootfs-only` can be run next.

After the installer resets the router, reconnect at `192.168.1.1` and confirm
the LAN, both access points, and WAN settings before restoring other
configuration.

### Rebuilding the UART artifacts

Build OpenWrt and the fixed-layout NAND payload, then create the generic CFE
WFI and receiver:

```sh
cd DG220TN_openwrt
./build-dg2200tn.sh
cd ..
./port/build-stock-rui.sh
./port/build-cfe-uart.sh
```

The resulting publishable installation artifacts are:

```text
port/images/dg2200tn-cfe-uart-bootstrap.bin
port/images/dg2200tn-cfe-uart-loader.bin
port/images/dg2200tn-cfe-rootfs-loader.bin
port/images/openwrt-dg2200tn-cfe-bootfs-repair.wfi.xz
port/images/openwrt-dg2200tn-cfe-rootfs.bin.xz
```

The full WFI and stock USB RUI remain development artifacts. The staged
UART/CFE installer is the documented permanent installation path.

## Build 73 network extension update

Kernel build `#73` adds the foundations required by policy-based routing,
multi-WAN, dnsmasq nftsets, SQM/CAKE, TUN/TAP VPNs, and nftables
socket/TPROXY rules. Policy rules and the traffic-control core are built into
the kernel. Larger optional features remain signed modules so the
uncompressed JFFS2 boot filesystem still fits in its fixed 4 MiB area.

For an existing DG2200TN OpenWrt installation, first copy a configuration
backup off the router as described above, then run the full staged installer:

```sh
./port/install-openwrt-cfe-uart.py --serial /dev/ttyUSB0
```

The full update rewrites bank-2 bootfs and rootfs. Restore the off-router
backup afterward to recover `/etc/config`, WireGuard peers and keys, passwords,
and other configuration, then reinstall packages that are not part of the
base image. The separate `/mnt/storage` UBI volume is outside bank 2 and is not
rewritten. Never use `--bootfs-only` as a configuration-preserving shortcut:
CFE reinitializes bank-2 UBI during that operation.

After the router reboots, confirm the new kernel and policy-rule support:

```sh
uname -v
ip -4 rule
ip -6 rule
```

`uname -v` must report:

```text
#73 SMP Sat Sep 5 07:43:20 UTC 2026
```

Official OpenWrt kmods target a different kernel ABI and must not be installed.
Build `#73` instead uses these signed, exact-name packages:

| Package | Purpose |
| --- | --- |
| `kmod-nf-conntrack-netlink-6.12.0-r1.apk` | dependency required by `dnsmasq-full` |
| `kmod-sched-core-6.12.0-r1.apk` | traffic-control classifiers and actions |
| `kmod-sched-cake-6.12.0-r1.apk` | CAKE queue discipline |
| `kmod-ifb-6.12.0-r1.apk` | ingress shaping for SQM |
| `kmod-tun-6.12.0-r1.apk` | OpenVPN, Tailscale, sing-box, and other TUN users |
| `kmod-nf-socket-6.12.0-r1.apk` | socket lookup foundation |
| `kmod-nf-tproxy-6.12.0-r1.apk` | transparent-proxy foundation |
| `kmod-nft-socket-6.12.0-r1.apk` | nftables socket expression |
| `kmod-nft-tproxy-6.12.0-r1.apk` | nftables transparent proxy expression |
| `kmod-nf-ipt-6.12.0-r1.apk` | legacy iptables compatibility core |
| `kmod-ipt-core-6.12.0-r1.apk` | common iptables matches and targets |
| `kmod-ipt-ipopt-6.12.0-r1.apk` | packet-option matches used by SQM |
| `kmod-nft-compat-6.12.0-r1.apk` | iptables-nft compatibility layer |

Verify the kernel-module and SQM userspace bundles before copying them to the
router:

```sh
cd port/packages
sha256sum -c dg2200tn-network-kmods-6.12.0-build73.sha256
sha256sum -c dg2200tn-sqm-userspace-build73.sha256
sha256sum -c dg2200tn-sqm-userspace-build73.adb.sha256
```

Install only the groups that are needed. Related local APKs must be supplied
in the same transaction so apk does not try to download incompatible official
kmods:

```sh
# dnsmasq-full and hostname-based PBR
apk add \
    /tmp/kmod-nf-conntrack-netlink-6.12.0-r1.apk \
    dnsmasq-full

# SQM with CAKE
apk --no-network --repositories-file /dev/null add \
    /tmp/kmod-sched-core-6.12.0-r1.apk \
    /tmp/kmod-sched-cake-6.12.0-r1.apk \
    /tmp/kmod-ifb-6.12.0-r1.apk \
    /tmp/kmod-nf-ipt-6.12.0-r1.apk \
    /tmp/kmod-ipt-core-6.12.0-r1.apk \
    /tmp/kmod-ipt-ipopt-6.12.0-r1.apk \
    /tmp/kmod-nft-compat-6.12.0-r1.apk
apk --no-network --repositories-file /dev/null \
    --repository /tmp/dg2200tn-sqm-userspace-build73.adb \
    add iptables-nft iptables-mod-ipopt sqm-scripts luci-app-sqm

# TUN/TAP VPN foundation
apk add /tmp/kmod-tun-6.12.0-r1.apk

# Optional nftables socket and transparent-proxy support
apk add \
    /tmp/kmod-nf-socket-6.12.0-r1.apk \
    /tmp/kmod-nf-tproxy-6.12.0-r1.apk \
    /tmp/kmod-nft-socket-6.12.0-r1.apk \
    /tmp/kmod-nft-tproxy-6.12.0-r1.apk
```

Each package checks the device, exact kernel build, and built-in-module ABI
marker before installation. Package-owned init services load modules in
dependency order without replacing the base module indexes or invoking the
incompatible runtime `depmod`.

The OpenWrt armsr target feed publishes its iptables binaries for
`arm_cortex-a15_neon-vfpv4`, which is not a safe ABI choice for this
Cortex-A9 router. `build-sqm-apks.sh` therefore compiles the seven required
iptables libraries/tools as `arm_cortex-a9`, downloads the two architecture-
independent SQM packages with pinned checksums, signs the complete userspace
set with the image's trusted release key, and creates
`dg2200tn-sqm-userspace-build73.adb`. Keep that index and its nine APKs in the
same directory when installing. SQM is installed disabled; configure and
enable it under **Network > SQM QoS** after choosing suitable rates.

After installing `dnsmasq-full`, hostname policies can use dnsmasq nftsets:

```sh
uci set pbr.config.resolver_set='dnsmasq.nftset'
uci set pbr.config.enabled='1'
uci commit pbr
/etc/init.d/dnsmasq restart
/etc/init.d/pbr restart
```

Rebuild the kernel, signed package bundle, WireGuard revision, and UART
artifacts with:

```sh
./port/build-stock-rui.sh
./port/build-network-kmods-apk.sh
./port/build-sqm-apks.sh
./port/build-wireguard-apk.sh
./port/build-cfe-uart.sh
```

Hardware validation on build `#73` covered package installation, automatic
module loading across a NAND reboot, iptables-nft DSCP/statistic/CLASSIFY
rules, and a temporary bidirectional `piece_of_cake.qos` instance with CAKE
on both the test interface and its generated ingress IFB. The test instance
was removed afterward and the shipped SQM configuration remains disabled.

## WireGuard kernel package

The current DG2200TN image runs the custom Linux `6.12.0` kernel build `#73`.
Official OpenWrt kernel packages target a different kernel ABI and must not
be used for WireGuard on this image. The signed, LuCI-uploadable module
package built specifically for this release is:

```text
port/packages/kmod-wireguard-6.12.0-r2.apk
```

Verify it before installation:

```sh
cd port/packages
sha256sum -c kmod-wireguard-6.12.0-r2.apk.sha256
```

The package architecture is `arm_cortex-a9`, OpenWrt's package name for this
router's ARMv7-A Cortex-A9 target; the kernel modules themselves report
`ARMv7 p2v8` vermagic. Install the APK through **System > Software > Upload
Package...** in LuCI. This adds the module without replacing or reflashing the
kernel or firmware. The equivalent command-line installation is:

```sh
apk add /tmp/kmod-wireguard-6.12.0-r2.apk
```

The APK is signed by the release key already trusted by the image. Its
pre-install check rejects any device, kernel build, built-in module set, or
required ChaCha implementation that does not match this exact DG2200TN
release. It includes WireGuard and its six new module dependencies; the
matching `chacha-neon` and IPv6 modules are already part of the firmware. A
package-owned S19 service loads the complete dependency chain during
installation and every boot without replacing the firmware's base module
indexes.

The package contains the kernel side only. After installing it, the normal
OpenWrt feeds can provide the kernel-independent tools and LuCI protocol
support:

```sh
apk update
apk add wireguard-tools luci-proto-wireguard
reboot
```

`netifd` discovers protocol handlers when it starts. Without that first
restart or reboot, LuCI can omit **WireGuard VPN** from the **Add new
interface** protocol list even though both packages are installed. After the
router returns, hard-refresh the LuCI page if it was already open.

To rebuild the APK, first complete the normal persistent-image build so that
`port/build/stock-rui/kernel-flash.config` and its matching module tree exist.
Keep the ignored `DG220TN_openwrt/private-key.pem` and
`DG220TN_openwrt/public-key.pem` signing files in place, then run:

```sh
./port/build-wireguard-apk.sh
```

The build script temporarily enables `CONFIG_WIREGUARD=m`, restores the
original kernel configuration on success or failure, verifies every module's
release, ARMv7 vermagic, and dependency set, signs the APK, verifies that
signature, and regenerates its SHA-256 manifest. Hardware validation covered
the original `r1` package on kernel build `#72`: a clean package install,
creation and removal of an active `wg-test` interface, an unattended NAND
reboot, automatic module loading at 15 seconds, and post-reboot LAN, WAN,
dual-radio, storage, and conntrack checks. That `r1` APK remains in
`port/packages/` only for build `#72`; its pre-install guard rejects build
`#73`, while `r2` rejects build `#72`.

## Runtime network layout

Ethernet and any enabled access points share one broadcast domain:

```text
br-lan 192.168.1.1/24
|- eth0       (yellow LAN switch ports)
|- phy0-ap0   (DG2200TN-2G)
`- phy1-ap0   (DG2200TN-5G)
```

DHCP serves `192.168.1.100` through `192.168.1.249` to both wired and wireless
clients. LuCI is available at `http://192.168.1.1/`. On the first boot, both
radios are enabled with one randomly generated WPA2 key. Inspect or replace
that key through LuCI before connecting wireless clients.

The early wireless service loads both board drivers before netifd starts.
The finalizer reapplies the board AP configuration after netifd has generated
its defaults, then reloads networking; this ordering keeps both APs enabled
after a clean first boot as well as subsequent unattended NAND boots.

The blue port is `eth1`, configured as a DHCP WAN with IPv4 masquerading.
Connect it to a LAN port on an external modem or upstream router. The upstream
subnet must not overlap `192.168.1.0/24`. DSL is not supported.

The blue-port hardware path requires SF2 crossbar control value `0x4c`, which
maps Runner WAN internal port 2 to GPHY4/PHY 12. This configuration was
validated on hardware with direct and routed host connections:

- the yellow LAN retained carrier, passed 5/5 pings, and served LuCI;
- the blue port retained carrier and passed 15/15 bidirectional pings;
- direct blue-WAN TCP reached 932.88 Mbit/s router TX and 676.12 Mbit/s
  router RX;
- a 1 GiB router-RX transfer sustained 673.49 Mbit/s without descriptor,
  BBH, DMA, or softnet-drop errors;
- BBH1 completion notifications wake the dedicated EMAC1 transmit task used
  by the basic Runner scheduler; on the persistent NAND build, three
  consecutive 1 GiB LAN streams then completed at 260.69, 278.29, and
  276.46 Mbit/s without an egress stall, netdev error, or BBH drop;
- concurrent LAN ping during that 3 GiB test averaged 0.30 ms, peaked at
  2.00 ms, and lost 0.75% of packets under maximum load; LuCI remained
  responsive afterward;
- a simultaneous 15-minute IIG WAN stress test from one wired host and one
  5 GHz client transferred about 8.5 GiB; 4,701 router pings had zero loss
  and averaged 0.49 ms, while 4,765 client pings lost 0.10% and averaged
  28.51 ms under load;
- that mixed wired/Wi-Fi test added no Ethernet or Wi-Fi interface errors
  and one TX drop among 6.60 million Wi-Fi transmit packets; brcmfmac logged
  three transient host PKTID-pool exhaustion events, but reported zero TX
  failures and neither disconnected nor stalled, while LuCI remained
  responsive throughout;
- both access points joined `br-lan`, and a 128 MiB blue-WAN DNAT transfer to
  a 5 GHz client completed at 230.61 Mbit/s;
- Runner rings drained after testing and the free packet-descriptor pool
  remained at its initialized `128/2944/16` values;
- two consecutive Runner driver unbind/rebind cycles reset the full RDP PMB
  domain before releasing DMA memory, accepted the first new LAN descriptor
  with zero stale-address errors, and immediately restored ping and LuCI;
- a 128 MiB HTTP transfer after the second rebind completed with zero CPU-RX
  descriptor errors or BBH drops.

The exact stock Runner firmware is loaded with the current mapped wakeup
labels `0x1914` (Runner-A CPU RX), `0x01b8` (Runner-A WAN direct), `0x04b4`
(Runner-A WAN normal), and `0x0d9c` (Runner-B CPU RX). No Runner instruction
that sets or clears CPU-RX descriptor word 0 bit 19 was proven, and tested
traffic left the checksum-verified flag clear. The driver therefore uses
`CHECKSUM_UNNECESSARY` only when firmware explicitly sets that bit; otherwise
Linux verifies the checksum in software.

Upstream DHCP, masqueraded Internet access from both yellow LAN and Wi-Fi,
and the default WAN firewall isolation have also been verified. Both Ethernet
interfaces negotiate at 1 Gbit/s; application throughput depends on the
traffic direction and CPU/Runner workload.

The 112 MiB `persistent-ubi-writable` NAND partition can be reformatted once
as a UBI volume named `storage`. The persistent image automatically attaches
that UBI device and mounts `ubi1:storage` at `/mnt/storage`; the resulting
UBIFS provides about 93.5 MiB usable capacity while retaining UBI bad-block
management and wear leveling. Reinstalling bank 2 does not overwrite this
separate storage partition.

Linux derives the default connection-tracking limit from RAM, which produced
8,192 entries on this 512 MiB board. The DG2200TN image raises
`net.netfilter.nf_conntrack_max` to 16,384 while retaining the kernel's 8,192
hash buckets.

## Imported revisions

- Linux port: `3a7496758` (`arm: bcm63138: add DG2200TN buttons`)
- OpenWrt port: `2d157d6` (`dg2200tn: enable reset and WPS buttons`)
- Vendor reference change: `09934c54` (`eth+wifi`)
- NAND tool base: `6b1410e`, with the local DG2200TN dump changes included
