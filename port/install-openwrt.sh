#!/bin/bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
PORT_DIR="$ROOT/port"
TFTP_DIR="$PORT_DIR/tftp"
RECOVERY_IMAGE="$PORT_DIR/images/openwrt-dg2200tn-recovery-installer.elf"
BUILD_IMAGE="$ROOT/DG220TN_openwrt/bin/targets/bcm63138/dg2200tn/openwrt-dg2200tn-tftp.elf"
RUI_IMAGE="$PORT_DIR/images/openwrt-dg2200tn-usb.rui"
RUI_CHECKSUM="$PORT_DIR/images/openwrt-dg2200tn-usb.rui.sha256"
INTERFACE="${DG2200TN_INTERFACE:-}"
SERIAL_DEVICE="${DG2200TN_SERIAL:-}"
TFTP_PID=
TFTP_LOG=
FIREWALL_INPUT_ADDED=0
FIREWALL_OUTPUT_ADDED=0

usage()
{
	cat <<'EOF'
Usage: sudo ./port/install-openwrt.sh [options]

Options:
  --interface DEVICE   Ethernet interface connected to a yellow LAN port
  --serial DEVICE      3.3 V UART device, for example /dev/ttyUSB0
  --help               Show this help
EOF
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--interface)
			[ "$#" -ge 2 ] || { echo "error: --interface requires a value" >&2; exit 2; }
			INTERFACE="$2"
			shift 2
			;;
		--serial)
			[ "$#" -ge 2 ] || { echo "error: --serial requires a value" >&2; exit 2; }
			SERIAL_DEVICE="$2"
			shift 2
			;;
		--help|-h)
			usage
			exit 0
			;;
		*)
			echo "error: unknown argument: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "Restarting with sudo..."
	exec sudo \
		DG2200TN_INTERFACE="$INTERFACE" \
		DG2200TN_SERIAL="$SERIAL_DEVICE" \
		"$0"
fi

for tool in ip iptables python3 sha256sum; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "error: required command not found: $tool" >&2
		exit 1
	}
done
[ -x /usr/sbin/atftpd ] || {
	echo "error: /usr/sbin/atftpd is missing" >&2
	echo "Install it with: sudo apt install atftpd" >&2
	exit 1
}
python3 -c 'import serial' >/dev/null 2>&1 || {
	echo "error: Python pyserial is missing" >&2
	echo "Install it with: sudo apt install python3-serial" >&2
	exit 1
}

if [ ! -f "$RECOVERY_IMAGE" ]; then
	RECOVERY_IMAGE="$BUILD_IMAGE"
fi
[ -f "$RECOVERY_IMAGE" ] || {
	echo "error: recovery ELF not found; run DG220TN_openwrt/build-dg2200tn.sh" >&2
	exit 1
}
[ -f "$RUI_IMAGE" ] || {
	echo "error: persistent RUI not found; run ./port/build-stock-rui.sh" >&2
	exit 1
}

if [ -f "$RUI_CHECKSUM" ]; then
	(
		cd "$(dirname "$RUI_CHECKSUM")"
		sha256sum -c "$(basename "$RUI_CHECKSUM")"
	)
fi

if [ -z "$INTERFACE" ]; then
	INTERFACE="$(ip -o -4 address show | awk '$4 == "192.168.1.100/24" { print $2; exit }')"
fi
if [ -z "$INTERFACE" ] && [ -d /sys/class/net/eth1 ]; then
	INTERFACE=eth1
fi
if [ -z "$INTERFACE" ]; then
	echo "error: Ethernet interface could not be selected automatically" >&2
	echo "Run again with --interface DEVICE" >&2
	exit 1
fi
[ -d "/sys/class/net/$INTERFACE" ] || {
	echo "error: Ethernet interface does not exist: $INTERFACE" >&2
	exit 1
}

if [ -z "$SERIAL_DEVICE" ]; then
	shopt -s nullglob
	SERIAL_PORTS=(/dev/ttyUSB* /dev/ttyACM*)
	shopt -u nullglob
	case "${#SERIAL_PORTS[@]}" in
		0)
			echo "error: no /dev/ttyUSB* or /dev/ttyACM* UART adapter found" >&2
			exit 1
			;;
		1)
			SERIAL_DEVICE="${SERIAL_PORTS[0]}"
			;;
		*)
			echo "Multiple UART adapters were found:"
			select selected in "${SERIAL_PORTS[@]}"; do
				if [ -n "$selected" ]; then
					SERIAL_DEVICE="$selected"
					break
				fi
			done
			;;
	esac
fi
[ -c "$SERIAL_DEVICE" ] || {
	echo "error: serial device does not exist: $SERIAL_DEVICE" >&2
	exit 1
}

cleanup()
{
	status=$?
	if [ -n "${TFTP_PID:-}" ] && kill -0 "$TFTP_PID" 2>/dev/null; then
		kill "$TFTP_PID" 2>/dev/null || true
		wait "$TFTP_PID" 2>/dev/null || true
	fi
	if [ "$FIREWALL_INPUT_ADDED" -eq 1 ]; then
		iptables -D INPUT -i "$INTERFACE" -j ACCEPT 2>/dev/null || true
	fi
	if [ "$FIREWALL_OUTPUT_ADDED" -eq 1 ]; then
		iptables -D OUTPUT -o "$INTERFACE" -j ACCEPT 2>/dev/null || true
	fi
	if [ -n "${TFTP_LOG:-}" ]; then
		if [ "$status" -ne 0 ]; then
			echo
			echo "TFTP server log:"
			cat "$TFTP_LOG" 2>/dev/null || true
		fi
		rm -f "$TFTP_LOG"
	fi
	return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$TFTP_DIR"
cp "$RECOVERY_IMAGE" "$TFTP_DIR/vmlinux.elf"
cp "$RUI_IMAGE" "$TFTP_DIR/openwrt-dg2200tn-usb.rui"

ip link set "$INTERFACE" up
ip address replace 192.168.1.100/24 dev "$INTERFACE"
ip address replace 10.0.0.100/24 dev "$INTERFACE"
ip neigh flush dev "$INTERFACE" 2>/dev/null || true

if ! iptables -C INPUT -i "$INTERFACE" -j ACCEPT 2>/dev/null; then
	iptables -I INPUT 1 -i "$INTERFACE" -j ACCEPT
	FIREWALL_INPUT_ADDED=1
fi
if ! iptables -C OUTPUT -o "$INTERFACE" -j ACCEPT 2>/dev/null; then
	iptables -I OUTPUT 1 -o "$INTERFACE" -j ACCEPT
	FIREWALL_OUTPUT_ADDED=1
fi

TFTP_LOG="$(mktemp)"
TFTP_USER="${SUDO_USER:-root}"
TFTP_GROUP="$(id -gn "$TFTP_USER")"
TFTP_USER="$TFTP_USER" TFTP_GROUP="$TFTP_GROUP" \
	"$PORT_DIR/run-tftp.sh" >"$TFTP_LOG" 2>&1 &
TFTP_PID=$!
sleep 1
if ! kill -0 "$TFTP_PID" 2>/dev/null; then
	echo "error: TFTP server failed to start" >&2
	cat "$TFTP_LOG" >&2
	exit 1
fi

echo
echo "DG2200TN installer"
echo "  Ethernet: $INTERFACE (192.168.1.100/24 and 10.0.0.100/24)"
echo "  UART:     $SERIAL_DEVICE (115200 8N1)"
echo "  Recovery: $RECOVERY_IMAGE"
echo "  RUI:      $RUI_IMAGE"
echo
echo "Connect the yellow LAN port and 3.3 V UART now."
echo "Do not connect the UART adapter VCC pin."
echo

python3 - "$SERIAL_DEVICE" <<'PY'
import glob
import os
import re
import sys
import time

import serial


SERIAL_DEVICE = sys.argv[1]
BAUD = 115200
OPENWRT_PROMPT_RE = re.compile(
    rb"(?:^|\n)root@[^:\r\n]+:[^\r\n]*#[ \t]*$"
)
STOCK_PROMPT_RE = re.compile(rb"(?:^|\n)/ #[ \t]*$")
PROMPT_RE = re.compile(
    rb"(?:^|\n)(?:root@[^:\r\n]+:[^\r\n]*#|/ #)[ \t]*$"
)
CFE_PROMPT_RE = re.compile(rb"(?:^|\n)CFE>[ \t]*$")
TTY_INPUT = open("/dev/tty", "r", encoding="utf-8")


class Console:
    def __init__(self, device):
        self.device = device
        self.serial = self._open(device)
        self.buffer = bytearray()

    @staticmethod
    def _open(device):
        return serial.Serial(device, BAUD, timeout=0.1, write_timeout=2)

    def reconnect(self):
        try:
            self.serial.close()
        except (OSError, serial.SerialException):
            pass

        print(
            "\nUART disconnected; waiting for the adapter to reappear...",
            file=sys.stderr,
            flush=True,
        )
        deadline = time.monotonic() + 30
        last_error = None
        while time.monotonic() < deadline:
            candidates = [self.device]
            available = sorted(
                glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
            )
            if self.device not in available and len(available) == 1:
                candidates = available

            for candidate in candidates:
                try:
                    self.serial = self._open(candidate)
                    if candidate != self.device:
                        self.device = candidate
                        print(
                            f"UART adapter is now {candidate}.",
                            file=sys.stderr,
                            flush=True,
                        )
                    print("UART connection restored.", file=sys.stderr, flush=True)
                    return
                except (OSError, serial.SerialException) as error:
                    last_error = error
            time.sleep(0.25)

        raise RuntimeError(
            f"UART adapter did not reconnect within 30 seconds: {last_error}"
        )

    def read(self):
        try:
            data = self.serial.read(4096)
        except (OSError, serial.SerialException):
            self.reconnect()
            return b""
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
            self.buffer.extend(data.replace(b"\r", b""))
            if len(self.buffer) > 262144:
                del self.buffer[:-131072]
        return data

    def send(self, text, paced=False):
        data = text.encode("ascii")
        if paced:
            for byte in data:
                self.write(bytes([byte]))
                time.sleep(0.04)
        else:
            self.write(data)

    def write(self, data):
        offset = 0
        while offset < len(data):
            try:
                written = self.serial.write(data[offset:])
                if written:
                    offset += written
                self.serial.flush()
            except (OSError, serial.SerialException):
                self.reconnect()

    def clear(self):
        self.buffer.clear()

    def wait_for(self, patterns, timeout, description):
        compiled = [
            pattern if hasattr(pattern, "search") else re.compile(pattern)
            for pattern in patterns
        ]
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.read()
            data = bytes(self.buffer)
            for index, pattern in enumerate(compiled):
                if pattern.search(data):
                    return index, data
        raise RuntimeError(f"timed out waiting for {description}")

    def wait_for_shell(self, timeout, description, failure_patterns=()):
        failures = [re.compile(pattern) for pattern in failure_patterns]
        deadline = time.monotonic() + timeout
        activated = False
        while time.monotonic() < deadline:
            self.read()
            data = bytes(self.buffer)
            for pattern in failures:
                if pattern.search(data):
                    raise RuntimeError(f"failed while waiting for {description}")
            if b"Please press Enter to activate this console" in data and not activated:
                self.send("\r")
                activated = True
                self.clear()
                continue
            if PROMPT_RE.search(data):
                return
        raise RuntimeError(f"timed out waiting for {description}")

    def run_command(self, command, timeout=120):
        token = f"DG2200TN_{os.getpid()}_{int(time.time())}"
        result = f"__{token}_RC__"

        self.clear()
        self.send(f"{command}; rc=$?; echo {result}$rc\r")
        self.wait_for_shell(timeout, f"command completion: {command}")
        data = bytes(self.buffer)
        text = data.decode("utf-8", errors="replace")
        matches = re.findall(re.escape(result) + r"(\d+)", text)
        match = matches[-1] if matches else None
        rc = int(match) if match is not None else 255
        body = re.sub(
            rf"\n?{re.escape(result)}\d+\s*(?:root@[^\r\n]+:[^\r\n]*#|/ #)?\s*$",
            "",
            text,
        )
        return rc, body.strip()


def operator_prompt(message):
    print(f"\n{message}\nPress Enter to continue: ", end="", flush=True)
    if TTY_INPUT.readline() == "":
        raise RuntimeError("interactive terminal input is required")


console = Console(SERIAL_DEVICE)

try:
    console.clear()
    console.send("\r")
    try:
        existing_shell, _ = console.wait_for(
            [OPENWRT_PROMPT_RE, STOCK_PROMPT_RE],
            3,
            "an existing router shell",
        )
        recovery_running = existing_shell == 0
        stock_ready = False
        if recovery_running:
            print("\nDetected an already-running recovery OpenWrt shell.")
        else:
            rc, _ = console.run_command(
                "test -x /usr/sbin/dg2200tn-install-rui",
                10,
            )
            stock_ready = rc == 0
            if stock_ready:
                print("\nDetected an already-prepared stock root shell.")
    except RuntimeError:
        recovery_running = False
        stock_ready = False

    if not recovery_running and not stock_ready:
        operator_prompt(
            "After pressing Enter, immediately power-cycle the router. "
            "The script will interrupt CFE automatically."
        )

        console.clear()
        index, cfe_output = console.wait_for(
            [rb"Press any key to stop auto run", CFE_PROMPT_RE],
            180,
            "the CFE autoboot prompt",
        )
        host_match = re.search(
            rb"Host IP address\s*:\s*([0-9]+(?:\.[0-9]+){3})",
            cfe_output,
        )
        tftp_host = (
            host_match.group(1).decode("ascii")
            if host_match
            else "10.0.0.100"
        )
        if index == 0:
            console.send(" ", paced=True)
            console.clear()
            console.wait_for([CFE_PROMPT_RE], 30, "the CFE command prompt")

        print(f"\nLoading the recovery ELF through TFTP from {tftp_host}...")
        time.sleep(2)
        console.clear()
        console.send(f"r {tftp_host}:vmlinux.elf\r", paced=True)
        console.wait_for_shell(
            300,
            "the RAM-booted OpenWrt shell",
            failure_patterns=(rb"\*\*\* command status = -[0-9]+",),
        )

    if not stock_ready:
        print("\nEnabling the reusable stock installer...")
        rc, output = console.run_command("dg2200tn-enable-stock-installer", 180)
        print(output)
        if rc != 0 or "guarded RUI installer are enabled" not in output:
            raise RuntimeError("failed to enable the stock installer")

        print("\nRebooting into stock firmware...")
        console.clear()
        console.send("reboot -f\r")
        console.wait_for_shell(300, "the stock UART root shell")

    while True:
        operator_prompt(
            "Insert the FAT32 USB drive containing "
            "openwrt-dg2200tn-usb.rui into the router."
        )
        time.sleep(5)
        rc, output = console.run_command(
            "find /var/usbmount -type f "
            "-name openwrt-dg2200tn-usb.rui 2>/dev/null",
            30,
        )
        paths = [
            line.strip()
            for line in output.splitlines()
            if line.strip().startswith("/var/usbmount/")
        ]
        if rc == 0 and paths:
            rui_path = paths[0]
            break
        print("\nThe RUI was not found. Wait for the USB drive to mount and retry.")

    if not re.fullmatch(r"[A-Za-z0-9_./:-]+", rui_path):
        raise RuntimeError(f"unsupported characters in USB path: {rui_path}")

    print(f"\nValidating {rui_path} without writing NAND...")
    validation_command = (
        f'test "$(wc -c < {rui_path})" -eq 72220813 && '
        f"/bin/rui info -r {rui_path} && "
        f"/bin/rui verify -r {rui_path}"
    )
    rc, output = console.run_command(
        validation_command,
        180,
    )
    print(output)
    if rc != 0 or "SeemsCorrupted    : no" not in output:
        raise RuntimeError("RUI validation failed; NAND was not written")

    print(
        "\nType INSTALL to begin the permanent NAND write, "
        "or anything else to cancel: ",
        end="",
        flush=True,
    )
    confirmation = TTY_INPUT.readline().rstrip("\r\n")
    if confirmation != "INSTALL":
        print("Installation cancelled. NAND was not written.")
        raise SystemExit(0)

    print("\nStarting permanent installation. Do not disconnect power.")
    console.clear()
    console.send(
        "/bin/pcb_cli -l "
        f"'UsbUpgrade.doUsbUpgrade(\"{rui_path}\")'\r"
    )

    deadline = time.monotonic() + 900
    saw_reboot = False
    while time.monotonic() < deadline:
        console.read()
        data = bytes(console.buffer)
        if b"error_scenario" in data:
            raise RuntimeError("the stock updater reported an error")
        if (
            b"Restarting system" in data
            or b"reboot: Restarting" in data
            or b"CFE version" in data
        ):
            saw_reboot = True
        if saw_reboot and (
            b"OpenWrt" in data
            and (
                b"Please press Enter to activate this console" in data
                or PROMPT_RE.search(data)
            )
        ):
            print("\nOpenWrt has booted from NAND.")
            raise SystemExit(0)

    raise RuntimeError(
        "installation monitoring timed out; inspect the UART output before "
        "power-cycling"
    )
except KeyboardInterrupt:
    print("\nInterrupted by operator.")
    raise SystemExit(130)
except RuntimeError as error:
    print(f"\nERROR: {error}", file=sys.stderr)
    raise SystemExit(1)
finally:
    console.serial.close()
    TTY_INPUT.close()
PY
