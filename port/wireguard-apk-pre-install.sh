#!/bin/sh

[ -n "${IPKG_INSTROOT:-}" ] && exit 0

expected_release="6.12.0"
expected_version="#73 SMP Sat Sep 5 07:43:20 UTC 2026"
expected_builtins="3e818bfd94fe16d2827223234a13dce012f8c7cb0a32f165983d537361daa650"
expected_chacha="1f28b715f3c8bedf0345255003d987d0558bb4643bc96214961f548cce31c384"
expected_base_indexes="812dce4e327a851769c25abcc77fa38665349e80553ce4111d7bcbb501138a1e c7c6def4d098e2d5273c74489a305d5c2fbfd4b4fdf21451d99ec8577106b415"
module_root="/lib/modules/$expected_release"

fail() {
	echo "kmod-wireguard: $*" >&2
	exit 1
}

[ "$(uname -r)" = "$expected_release" ] ||
	fail "requires DG2200TN kernel $expected_release"
[ "$(uname -v)" = "$expected_version" ] ||
	fail "requires the published DG2200TN kernel build #73"
[ "$(tr '\000' '\n' </proc/device-tree/model 2>/dev/null | head -n 1)" = "DG2200TN" ] ||
	fail "this package is only for DG2200TN"
[ -f "$module_root/modules.builtin" ] ||
	fail "missing kernel ABI marker modules.builtin"
[ -f "$module_root/modules.dep.bin" ] ||
	fail "missing kernel dependency index"
[ -f "$module_root/kernel/arch/arm/crypto/chacha-neon.ko" ] ||
	fail "missing required chacha-neon module"

actual_builtins="$(sha256sum "$module_root/modules.builtin" | cut -d ' ' -f 1)"
actual_index="$(sha256sum "$module_root/modules.dep.bin" | cut -d ' ' -f 1)"
actual_chacha="$(sha256sum \
	"$module_root/kernel/arch/arm/crypto/chacha-neon.ko" | cut -d ' ' -f 1)"
[ "$actual_builtins" = "$expected_builtins" ] ||
	fail "modules.builtin does not match the supported firmware"
case " $expected_base_indexes " in
	*" $actual_index "*) ;;
	*) fail "kernel dependency index does not match the supported firmware" ;;
esac
[ "$actual_chacha" = "$expected_chacha" ] ||
	fail "chacha-neon does not match the supported firmware"

exit 0
