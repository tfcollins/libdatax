# SPDX-License-Identifier: MIT
"""Recompute SHA-256 of every vendored file listed in PROVENANCE.md."""

import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENTRY = re.compile(r"^- `(?P<path>[^`]+)`\s*$")
FIELD = re.compile(r"^\s+- (?P<key>[a-z0-9-]+): (?P<val>.+?)\s*$")


def parse():
    entries, cur = [], None
    for line in (ROOT / "PROVENANCE.md").read_text().splitlines():
        m = ENTRY.match(line)
        if m:
            cur = {"path": m["path"]}
            entries.append(cur)
            continue
        m = FIELD.match(line)
        if m and cur is not None:
            cur[m["key"]] = m["val"].strip("`")
    return entries


def test_all_vendored_files_listed():
    listed = {e["path"] for e in parse()}
    actual = {p.relative_to(ROOT).as_posix()
              for p in (ROOT / "tests" / "vendor" / "tick").iterdir()
              if p.suffix in (".py", ".h") and p.name != "__init__.py"}
    assert listed == actual


def test_hashes_match():
    entries = parse()
    assert entries
    for e in entries:
        for key in ("upstream", "revision", "licence", "sha256"):
            assert key in e, (e["path"], key)
        assert re.fullmatch(r"[0-9a-f]{64}", e["sha256"])
        digest = hashlib.sha256((ROOT / e["path"]).read_bytes()).hexdigest()
        assert digest == e["sha256"], e["path"]
