#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""strip_reg_access.py <in.xml> <out.xml> <device-name>...

Copy a fixture, removing the direct_reg_access debug attribute from the named
devices (the streaming devices, for the no-status-register tests)."""
import re
import sys

src, dst, *names = sys.argv[1:]
text = open(src, encoding="utf-8").read()
for name in names:
    m = re.search(r'<device id="[^"]*" name="%s".*?</device>' % re.escape(name), text, re.S)
    if not m:
        sys.exit(f"strip_reg_access: no device {name} in {src}")
    body, n = re.subn(r'<debug-attribute name="direct_reg_access"[^>]*/>', "", m.group(0))
    if n != 1:
        sys.exit(f"strip_reg_access: {name} has {n} direct_reg_access attributes")
    text = text[:m.start()] + body + text[m.end():]
open(dst, "w", encoding="utf-8").write(text)
