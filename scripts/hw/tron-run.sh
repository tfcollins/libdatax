#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Boot tron (ADRV9009-ZU11EG) on one pinned image over JTAG, run tests/hardware
# against it from this checkout, then restore and power it off. One labgrid
# reservation per image. Used by .github/workflows/hw-test.yml on the hw-nemo
# runner; runnable by hand with the same environment.
#
#   scripts/hw/tron-run.sh <stock|hopcap-sfp> <out-dir>
#
# Needs: build/libdatax.so and libiio/build (the pinned v1 fork), and a venv
# with this package, hopcap and labgrid-plugins at their pins.json revisions
# (DATAX_LAB_VENV, default ./.venv-hw).
#
# Host facts come from the runner's environment (its .env file), never from
# this repository:
#   DATAX_LAB_ARTIFACTS     directory holding tron-<image>/ boot files
#                           (hashes in tests/hardware/images.json)
#   DATAX_LAB_COORDINATOR   labgrid coordinator host:port
#   DATAX_LAB_HTTP_BIND     address of this host that the board can reach
#   DATAX_LAB_EXPORTER_TRON ssh name of tron's exporter (JTAG inputs go there)
#   DATAX_LAB_HTTP_PORT     optional, default 8750
#   DATAX_LAB_LOCK          optional, default /tmp/vrt49-corundum-hardware.lock
#
# The session log is written to <out-dir>, and only a redacted copy is kept.
# Nothing unredacted reaches stdout (public job logs).
set -uo pipefail

image=${1:?usage: tron-run.sh <stock|hopcap-sfp> <out-dir>}
out=${2:?usage: tron-run.sh <stock|hopcap-sfp> <out-dir>}
root=$(cd "$(dirname "$0")/../.." && pwd)
venv=${DATAX_LAB_VENV:-$root/.venv-hw}
port=${DATAX_LAB_HTTP_PORT:-8750}
lock=${DATAX_LAB_LOCK:-/tmp/vrt49-corundum-hardware.lock}
for v in DATAX_LAB_ARTIFACTS DATAX_LAB_COORDINATOR DATAX_LAB_HTTP_BIND DATAX_LAB_EXPORTER_TRON; do
	[ -n "${!v:-}" ] || { echo "tron-run: $v is not set (runner .env)" >&2; exit 2; }
done
mkdir -p "$out"
dir="$DATAX_LAB_ARTIFACTS/tron-$image"

# The image must match tests/hardware/images.json byte for byte.
hashes=$("$venv/bin/python" - "$root/tests/hardware/images.json" "$image" "$dir" <<'EOF'
import hashlib, json, pathlib, sys
spec = json.load(open(sys.argv[1]))["tron"].get(sys.argv[2])
if spec is None:
    sys.exit(f"tron-run: unknown image {sys.argv[2]!r}")
d = pathlib.Path(sys.argv[3])
for name, want in spec["files"].items():
    p = d / name
    got = hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else "missing"
    if got != want:
        sys.exit(f"tron-run: {sys.argv[2]}/{name}: sha256 {got[:16]}, want {want[:16]}")
f = spec["files"]
parts = ([f"xsa={spec['xsa']}"] if "xsa" in spec else [f"bit={f['system_top.bit']}"])
print(" ".join(parts + [f"Image={f['Image']}", f"dtb={f['system.dtb']}"]), spec["pps"])
EOF
) || exit 2
read -r hash_xsa hash_image hash_dtb pps <<<"$hashes"
echo "tron-run: $image boot set verified against images.json"

remote="/tmp/libdatax-hw-${GITHUB_RUN_ID:-manual}-${GITHUB_RUN_ATTEMPT:-0}-$image"
http_pid=
cleanup() {
	[ -n "$http_pid" ] && kill "$http_pid" 2>/dev/null
	ssh -o BatchMode=yes -o ConnectTimeout=15 "$DATAX_LAB_EXPORTER_TRON" "rm -rf -- '$remote'" >/dev/null 2>&1
}
trap cleanup EXIT

"$venv/bin/python" -m http.server "$port" --bind "$DATAX_LAB_HTTP_BIND" --directory "$dir" >/dev/null 2>&1 &
http_pid=$!
ssh -o BatchMode=yes -o ConnectTimeout=15 "$DATAX_LAB_EXPORTER_TRON" "rm -rf -- '$remote' && mkdir -p '$remote'" &&
	rsync -a -- "$dir/system_top.bit" "$dir/psu_init.tcl" "$DATAX_LAB_EXPORTER_TRON:$remote/" ||
	{ echo "tron-run: could not stage the JTAG inputs on the exporter" >&2; exit 3; }

url="http://$DATAX_LAB_HTTP_BIND:$port"
raw="$out/session-tron-$image.raw.log"
(
	export PATH="$venv/bin:$PATH" LG_COORDINATOR="$DATAX_LAB_COORDINATOR"
	export VRT49_CORUNDUM_BOOT=jtag HOPCAP_BOOT_SOURCE="libdatax hw-test ($image)" VRT49_PHYSICAL_PORT=sfp
	export VRT49_CORUNDUM_KERNEL_URL="$url/Image" VRT49_CORUNDUM_KERNEL_SHA256="${hash_image#Image=}"
	export VRT49_CORUNDUM_DTB_URL="$url/system.dtb" VRT49_CORUNDUM_DTB_SHA256="${hash_dtb#dtb=}"
	export VRT49_CORUNDUM_BITSTREAM_REMOTE="$remote/system_top.bit" VRT49_CORUNDUM_PSU_INIT_REMOTE="$remote/psu_init.tcl"
	export DATAX_HW_IMAGE="$image" DATAX_HW_PPS="$pps" DATAX_HW_IMAGE_HASHES="$hash_xsa $hash_image $hash_dtb"
	export DATAX_HW_OUT="$out"
	flock -w 1800 "$lock" adi-lg request --part adrv9009zu11eg --carrier adrv2crr-fmc --mode reserve --wait 1800 \
		--run "test \"\$LG_PLACE\" = tron && python '$root/scripts/hw/tron_session.py'"
) >"$raw" 2>&1
rc=$?

"$venv/bin/python" "$root/scripts/hw/redact.py" "$out" && rm -f "$raw"
echo "----- session log (redacted): $out/session-tron-$image.log -----"
cat "$out/session-tron-$image.log"
echo "tron-run: $image exit $rc"
exit "$rc"
