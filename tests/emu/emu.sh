#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Usage: emu.sh <xml> <cmd...>
# Starts iiod-emu on a free port serving <xml>, exports DATAX_TEST_URI, runs
# <cmd...>, always kills the emulator and exits with the command's status.
# Needs DATAX_LIBIIO_BUILD (libiio build tree containing iiod-emu/iiod-emu).
set -u

if [ $# -lt 2 ]; then
	echo "usage: $0 <xml> <cmd...>" >&2
	exit 2
fi
xml=$1
shift

emu=${DATAX_LIBIIO_BUILD:-}/iiod-emu/iiod-emu
if [ -z "${DATAX_LIBIIO_BUILD:-}" ] || [ ! -x "$emu" ]; then
	echo "emu.sh: iiod-emu not found (set DATAX_LIBIIO_BUILD to the libiio build tree)" >&2
	exit 2
fi
if [ ! -f "$xml" ]; then
	echo "emu.sh: no such fixture: $xml" >&2
	exit 2
fi

log=$(mktemp)
pid=
cleanup() {
	if [ -n "$pid" ]; then
		kill "$pid" 2>/dev/null
		wait "$pid" 2>/dev/null
	fi
	rm -f "$log"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

free_port() {
	python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'
}

port_open() {
	python3 -c 'import socket,sys; socket.create_connection(("127.0.0.1",int(sys.argv[1])),1).close()' "$1" 2>/dev/null
}

# The port is released before iiod-emu binds it, so retry on a lost race.
started=0
for _ in 1 2 3 4 5; do
	port=$(free_port) || exit 2
	: >"$log"
	LD_LIBRARY_PATH="$DATAX_LIBIIO_BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
		"$emu" -p "$port" "emu:$xml" >"$log" 2>&1 &
	pid=$!
	for _ in $(seq 1 100); do
		kill -0 "$pid" 2>/dev/null || break
		if grep -q "Listening on port" "$log" && port_open "$port"; then
			started=1
			break
		fi
		sleep 0.1
	done
	[ "$started" = 1 ] && break
	kill "$pid" 2>/dev/null
	wait "$pid" 2>/dev/null
	pid=
done
if [ "$started" != 1 ]; then
	echo "emu.sh: iiod-emu failed to start:" >&2
	cat "$log" >&2
	exit 3
fi

export DATAX_TEST_URI="ip:127.0.0.1:$port"
LD_LIBRARY_PATH="$DATAX_LIBIIO_BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$@"
exit $?
