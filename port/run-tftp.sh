#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
TFTP_USER="${TFTP_USER:-$(id -un)}"
TFTP_GROUP="${TFTP_GROUP:-$(id -gn)}"

mkdir -p "$ROOT/tftp"

exec /usr/sbin/atftpd \
    --daemon \
    --no-fork \
    --logfile - \
    --verbose=7 \
    --trace \
    --no-blksize \
    --no-tsize \
    --no-timeout \
    --no-multicast \
    --prevent-sas \
    --no-source-port-checking \
    --user "$TFTP_USER" \
    --group "$TFTP_GROUP" \
    "$ROOT/tftp"
