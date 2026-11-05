#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Usage: emu_pattern.sh <xml> <device|-> <channels> <channel-offset> <cmd...>
# Copies <xml> into a fresh temporary directory, writes the RX counting
# pattern for <device> beside the copy with make_pattern.py (32768 instants,
# one full period, so the emulator's wrap at end of file is seamless), then
# runs emu.sh on the copy with <cmd...>. Device "-" writes no pattern file
# (the device's buffer file is absent). The temporary directory is always
# removed; the repository's fixtures are never written. The command sees the
# directory as DATAX_TEST_XML_DIR.
set -u

if [ $# -lt 5 ]; then
	echo "usage: $0 <xml> <device|-> <channels> <channel-offset> <cmd...>" >&2
	exit 2
fi
xml=$1 device=$2 channels=$3 offset=$4
shift 4

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

cp "$xml" "$tmp/" || exit 2
copy="$tmp/$(basename "$xml")"
if [ "$device" != "-" ]; then
	python3 "$here/make_pattern.py" "$copy" "$device" "$channels" 32768 \
		--channel-offset "$offset" >/dev/null || exit 2
fi
export DATAX_TEST_XML_DIR="$tmp"
"$here/emu.sh" "$copy" "$@"
exit $?
