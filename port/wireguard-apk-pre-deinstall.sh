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

default_prerm
