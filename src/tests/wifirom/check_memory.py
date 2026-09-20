#!/usr/bin/env python3
"""Check the 1MHz-WiFi ROM only writes to memory it is allowed to write to.

A sideways ROM owns almost no host memory.  What it may use is short:

  &B0-&BF   the active paged ROM's scratch
  &A8-&AF   command workspace, while a * command is being handled
  &0E00 up  only what it claimed at service call 1 or 2
  &FC00+    the 1MHz bus itself
  its own image, when that image is in sideways RAM

Everything else belongs to the OS, the current filing system, or another
ROM, and writing to it is a fault that shows up as something unrelated
breaking later.  This ROM used to keep its scratch at &0900, &0A00 and
&0D90, which are the RS423 output buffer (and ENVELOPEs 5-16), the
CFS/RFS/RS423 input buffer, and the VFS mouse workspace followed by the
EXTENDED VECTOR TABLE at &0D9F.  The last one hung the machine on the next
OS call after any command; the first killed a *FX3,1 serial redirect.

So this reads the sources rather than the binary: every absolute store, and
every symbol a store goes through, must resolve into the allowed set.
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[2] / "beeb/1mhz-wifi/src"

STORE = re.compile(r"^\s*(sta|stx|sty|inc|dec|asl|lsr|rol|ror)\s+&?([0-9A-Fa-f]{2,4}|[A-Za-z_][A-Za-z0-9_]*)\s*(,\s*[xy])?\s*(?:\\.*)?$",
                   re.IGNORECASE)
EQUATE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*(?:\\.*)?$")

# Addresses a paged ROM may write, and why.
ALLOWED = [
    (0x00B0, 0x00BF, "paged ROM scratch"),
    (0x00A8, 0x00AF, "command workspace"),
    (0x8000, 0xBFFF, "its own image (sideways RAM)"),
    (0xFC00, 0xFEFF, "1MHz bus / hardware"),
]

# Writes outside that set which are deliberate, with the reason. Each one is a
# claim on memory this ROM does not own, so it has to be argued for here.
KNOWN = {
    0x00C7: "pr_y: UEF stream cursor, shared with the filing system ROM",
    0x00C8: "pr_r: UEF stream page register, shared with the filing system ROM",
    0x00F8: "sbufl: UEF stream length low, shared with the filing system ROM",
    0x00F9: "sbufh: UEF stream length high, shared with the filing system ROM",
    0x00F4: "shadow: the MOS's own copy of the selected ROM number",
    0x00F5: "sbuft: UEF stream flags, shared with the filing system ROM",
}


def resolve(name, equates, depth=0):
    """Resolve a symbol to an address, following equates and + offsets."""
    if depth > 8:
        return None
    text = equates.get(name)
    if text is None:
        return None
    total, ok = 0, False
    for term in re.split(r"\s*\+\s*", text):
        term = term.strip()
        m = re.fullmatch(r"&([0-9A-Fa-f]+)", term)
        if m:
            total += int(m.group(1), 16); ok = True; continue
        if re.fullmatch(r"\d+", term):
            total += int(term); ok = True; continue
        sub = resolve(term, equates, depth + 1)
        if sub is None:
            return None
        total += sub; ok = True
    return total if ok else None


def main():
    equates, stores = {}, []
    for path in sorted(SRC.glob("*.asm")):
        for n, line in enumerate(path.read_text().splitlines(), 1):
            m = EQUATE.match(line)
            if m and not line.lstrip().startswith("\\"):
                equates.setdefault(m.group(1), m.group(2))
            m = STORE.match(line)
            if m:
                stores.append((path.name, n, m.group(2), line.strip(), bool(m.group(3))))

    failures, allowed_note, indexed = [], 0, 0
    for fname, n, target, line, index in stores:
        if re.fullmatch(r"[0-9A-Fa-f]{2,4}", target) and "&" in line.split(target)[0][-2:]:
            addr = int(target, 16)
        else:
            addr = resolve(target, equates)
            if addr is None:
                continue                      # a label inside the image
        if index and addr < 0x0100:
            # zero page indexed: the caller passes the base in X or Y, so the
            # address cannot be known here (see string2hex in util.asm).
            indexed += 1
            continue
        if any(lo <= addr <= hi for lo, hi, _ in ALLOWED):
            continue
        if addr in KNOWN:
            allowed_note += 1
            continue
        failures.append(f"{fname}:{n}: writes &{addr:04X} - not memory this ROM owns: {line}")

    for f in failures:
        print(f"  FAIL: {f}")
    if failures:
        print(f"\n  {len(failures)} write(s) into memory the ROM does not own.")
        print("  A sideways ROM may write &B0-&BF, &A8-&AF while handling a command,")
        print("  workspace it claimed at service call 1 or 2, the bus at &FC00+, and")
        print("  its own image. Everything else belongs to the OS or another ROM.")
        return 1
    print(f"ROM MEMORY OK: {len(stores)} stores checked, "
          f"{allowed_note} documented exceptions, "
          f"{indexed} zero-page indexed (base in a register)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
