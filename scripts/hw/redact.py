# SPDX-License-Identifier: MIT
"""Redact hardware-run outputs before they leave the lab (public job logs and
artifacts, committed evidence).

    python scripts/hw/redact.py <dir-or-file>...

- *.raw.log becomes *.log (the raw file is left for the caller to delete);
  other *.log, *.json and *.xml files are rewritten in place.
- IPv4 addresses become <ip>, *.local names become <host>, and the words in
  DATAX_LAB_REDACT (space separated, from the runner's .env) become <host>.
- Absolute paths to this checkout become relative, and /home/<user>/ becomes ~/.
- JUnit `hostname` attributes are removed.
"""

import os
import re
import sys
from pathlib import Path

ROOT = str(Path(__file__).resolve().parents[2])
IPV4 = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
LOCAL = re.compile(r"\b[A-Za-z0-9][A-Za-z0-9-]*\.local\b")
HOME = re.compile(r"/home/[A-Za-z0-9_.-]+/")
HOSTNAME_ATTR = re.compile(r' hostname="[^"]*"')
SUFFIXES = (".log", ".json", ".xml")


def redact(text: str, words: list[str]) -> str:
    text = text.replace(ROOT + "/", "").replace(ROOT, ".")
    text = HOSTNAME_ATTR.sub("", text)
    text = IPV4.sub("<ip>", text)
    text = LOCAL.sub("<host>", text)
    for w in words:
        text = re.sub(rf"\b{re.escape(w)}\b", "<host>", text)
    return HOME.sub("~/", text)


def files(args):
    for a in map(Path, args):
        if a.is_dir():
            yield from (p for p in sorted(a.rglob("*")) if p.is_file() and p.name.endswith(SUFFIXES))
        elif a.is_file():
            yield a


def main(argv) -> int:
    words = [w for w in os.environ.get("DATAX_LAB_REDACT", "").split() if w]
    for p in list(files(argv)):
        dest = p.with_name(p.name.replace(".raw.log", ".log")) if p.name.endswith(".raw.log") else p
        dest.write_text(redact(p.read_text(errors="replace"), words))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
