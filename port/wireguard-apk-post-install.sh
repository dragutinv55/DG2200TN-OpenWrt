#!/bin/sh

[ "${IPKG_NO_SCRIPT:-0}" = "1" ] && exit 0

functions="${IPKG_INSTROOT:-}/lib/functions.sh"
[ -s "$functions" ] || {
	echo "kmod-wireguard: missing $functions" >&2
	exit 1
}

. "$functions"
export root="${IPKG_INSTROOT:-}"
export pkgname="kmod-wireguard"

default_postinst || exit $?

if [ -z "${IPKG_INSTROOT:-}" ] &&
	! grep -q '^wireguard ' /proc/modules; then
	echo "kmod-wireguard: module did not load; check dmesg" >&2
	exit 1
fi

exit 0
