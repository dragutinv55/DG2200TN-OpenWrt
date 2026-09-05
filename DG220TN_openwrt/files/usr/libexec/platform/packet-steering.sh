#!/bin/sh

/usr/libexec/network/packet-steering.uc "$@"

mask=2
for argument in "$@"; do
	[ "$argument" = 0 ] && mask=0
done

for queue in \
	/sys/bus/platform/devices/80200000.ethernet-controller/net/*/queues/rx-*/rps_cpus
do
	[ -e "$queue" ] || continue
	printf '%s\n' "$mask" > "$queue"
done
