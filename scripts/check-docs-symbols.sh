#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fail if a public datax_* symbol exported by src/libdatax.map is not
# mentioned on the SDK docs C-core API page.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
url="${DATAX_DOCS_URL:-https://raw.githubusercontent.com/tfcollins/datax_sdk_sdr/main/docs/api-c-core.md}"

page="$(curl -fsSL --retry 3 --max-time 30 "$url")"
# Only symbol lines ("  datax_foo;"), so words in map comments are ignored.
symbols="$(grep -oE '^[[:space:]]*datax_[a-z0-9_]*;' "$root/src/libdatax.map" \
  | tr -d ' \t;' | { grep -v '^datax_internal_' || true; } | sort -u)"
if [ -z "$symbols" ]; then
  echo "no public symbols found in src/libdatax.map" >&2
  exit 1
fi

missing=()
while IFS= read -r sym; do
  [ -n "$sym" ] || continue
  if ! grep -qwF -- "$sym" <<<"$page"; then
    missing+=("$sym")
  fi
done <<<"$symbols"

if [ "${#missing[@]}" -gt 0 ]; then
  echo "Symbols in src/libdatax.map missing from $url:" >&2
  printf '  %s\n' "${missing[@]}" >&2
  exit 1
fi
echo "docs symbols: all $(wc -l <<<"$symbols") public symbols documented"
