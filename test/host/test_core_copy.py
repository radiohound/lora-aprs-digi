#!/usr/bin/env python3
"""The signing code in tools/setup.html is a copy of the one in tools/remote_code.html. If they drift apart,
commands sent from the setup page would be refused. This fails when they are not identical."""
import re, sys

def core(path):
    s = open(path, encoding="utf-8").read()
    m = re.search(r'<script id="core">(.*?)</script>', s, re.S)
    if not m:
        sys.exit("no <script id=\"core\"> in " + path)
    # the comment line that says it is a copy is the only allowed difference
    return "\n".join(l for l in m.group(1).splitlines() if "A copy of the core" not in l).strip()

a, b = core("tools/remote_code.html"), core("tools/setup.html")
if a != b:
    sys.exit("FAIL: the core in tools/setup.html differs from tools/remote_code.html")
print("OK: the signing code in setup.html is identical to remote_code.html (%d bytes)" % len(a))
