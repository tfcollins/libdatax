#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Have labgrid boot a board on its catalog Kuiper image (adi-lg request
# --mode uri: the coordinator's board catalog picks the image, the place's
# boot strategy boots it, and iiod is checked), run tests/hardware against the
# exported IIO_URI from this checkout, then release the board and power it off.
# Used by .github/workflows/hw-test.yml on the hw-nemo runner for every board
# except tron (tron boots exact pinned images: tron-run.sh). Runnable by hand
# with the same environment.
#
#   scripts/hw/kuiper-run.sh <name> <part> <carrier> <out-dir>
#
# <name> names the outputs (evidence-<name>.json, junit-<name>.xml,
# session-<name>.log). Needs build/libdatax.so, libiio/build (the pinned v1
# fork) and a venv with this package and labgrid-plugins at its pins.json
# revision (DATAX_LAB_VENV, default ./.venv-hw).
#
# Host facts come from the runner's environment (its .env file):
#   DATAX_LAB_COORDINATOR   labgrid coordinator host:port
#
# The session log is written to <out-dir>, and only a redacted copy is kept.
# Nothing unredacted reaches stdout (public job logs).
set -uo pipefail

usage="usage: kuiper-run.sh <name> <part> <carrier> <out-dir>"
name=${1:?$usage}
part=${2:?$usage}
carrier=${3:?$usage}
out=${4:?$usage}
root=$(cd "$(dirname "$0")/../.." && pwd)
venv=${DATAX_LAB_VENV:-$root/.venv-hw}
[ -n "${DATAX_LAB_COORDINATOR:-}" ] ||
	{ echo "kuiper-run: DATAX_LAB_COORDINATOR is not set (runner .env)" >&2; exit 2; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
lib="$root/libiio/build"

raw="$out/session-$name.raw.log"
(
	export PATH="$venv/bin:$PATH" LG_COORDINATOR="$DATAX_LAB_COORDINATOR"
	export DATAX_LIBRARY="$root/build/libdatax.so" DATAX_LIBIIO_LIB="$lib/libiio.so.1"
	export LD_LIBRARY_PATH="$lib" DATAX_HW_REQUIRE=1 DATAX_HW_IMAGE=stock
	export DATAX_HW_IMAGE_HASHES="kuiper=labgrid catalog default for $part/$carrier"
	export DATAX_HW_EVIDENCE="$out/evidence-$name.json"
	cd "$root" &&
	adi-lg request --part "$part" --carrier "$carrier" --mode uri --wait 1800 --power-down \
		--run "echo \"### board booted on place \$LG_PLACE; running tests/hardware\";
		       DATAX_HW_URI=\"\$IIO_URI\" python -m pytest tests/hardware -v -rs -p no:cacheprovider \
		         --junitxml='$out/junit-$name.xml'"
) >"$raw" 2>&1
rc=$?

"$venv/bin/python" "$root/scripts/hw/redact.py" "$out" && rm -f "$raw"
echo "----- session log (redacted): session-$name.log -----"
cat "$out/session-$name.log"
echo "kuiper-run: $name ($part/$carrier) exit $rc"
exit "$rc"
