#!/usr/bin/env python3
"""Check the 1MHz-WiFi host ROM against the service headers it talks to.

The ROM in beeb/1mhz-wifi/ hard-codes the service command numbers of
src/wifi_service.h and src/net_service.h.  Nothing in either build refers to
the other, so a renumbering here would leave the ROM sending the old number
and the firmware answering a different command - a fault that shows up only
on real hardware, as the wrong operation rather than an error.

So compare the two sides by NAME, not just by set of numbers: drv_svc_join
must equal WIFI_SVC_CMD_JOIN, and if the header renumbers JOIN the ROM must
move with it.

A command the ROM sends that the firmware does not implement is reported but
is not a failure, as long as the ROM's "unsupported" constant still matches
NET_ERR_UNSUPPORTED: that is how the ROM detects the gap and falls back.
That is today's position for net command 58 (bulk page copy).
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]                      # repo root
ROM = ROOT / "beeb/1mhz-wifi/src/service_driver.asm"
WIFI_H = ROOT / "src/wifi_service.h"
NET_H = ROOT / "src/net_service.h"

# beebasm: "name = 58" or "name = &27", one per line, comment after a backslash
ASM_CONST = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(&[0-9A-Fa-f]+|\d+)\s*(?:\\.*)?$")
# C: "#define NAME 58u" / "0x27u", or an alias "#define NAME OTHER_NAME"
C_DEFINE = re.compile(r"^\s*#define\s+([A-Z][A-Z0-9_]*)\s+(0x[0-9A-Fa-f]+u?|\d+u?|[A-Z][A-Z0-9_]*)\s*(?:/\*.*)?$")


def num(text):
    text = text.rstrip("u")
    if text.startswith("&"):
        return int(text[1:], 16)
    if text.startswith("0x"):
        return int(text, 16)
    return int(text)


def asm_constants(path):
    out = {}
    for line in path.read_text().splitlines():
        m = ASM_CONST.match(line)
        if m:
            out[m.group(1)] = num(m.group(2))
    return out


def c_defines(path):
    """Numeric #defines, resolving one level of aliasing (X = Y)."""
    raw, out = {}, {}
    for line in path.read_text().splitlines():
        m = C_DEFINE.match(line)
        if m:
            raw[m.group(1)] = m.group(2)
    for name, value in raw.items():
        seen = set()
        while value in raw and value not in seen:   # alias chain
            seen.add(value)
            value = raw[value]
        try:
            out[name] = num(value)
        except ValueError:
            pass                                    # not a plain number
    return out


def main():
    for path in (ROM, WIFI_H, NET_H):
        if not path.exists():
            print(f"FAIL: missing {path}", file=sys.stderr)
            return 1

    rom = asm_constants(ROM)
    wifi = c_defines(WIFI_H)
    net = c_defines(NET_H)
    failures, notes = [], []

    for required in ("WIFI_SVC_CMD_FIRST", "WIFI_SVC_CMD_LAST"):
        if required not in wifi:
            # Usually an alias chain that no longer resolves, e.g. LAST is
            # defined as the command that was just deleted.
            print(f"  FAIL: {required} missing or unresolvable in wifi_service.h")
            return 1
    first, last = wifi["WIFI_SVC_CMD_FIRST"], wifi["WIFI_SVC_CMD_LAST"]

    # ---- wifi_service: every drv_svc_<name> must match WIFI_SVC_CMD_<NAME> --
    checked = 0
    for rom_name, value in sorted(rom.items()):
        if not rom_name.startswith("drv_svc_"):
            continue
        if "_op_" in rom_name:
            continue                    # sub-opcode of a command, not a command
        if not first <= value <= last:
            continue                    # driver tuning constant, not a command
        stem = rom_name[len("drv_svc_"):]
        # the ROM spells some commands differently from the header
        aliases = {
            "guard_image": "GUARD",
            "vector_mirror": "GUARD",
            "uef_normalize": "UEF",
        }
        header_name = "WIFI_SVC_CMD_" + aliases.get(stem, stem.upper())
        if header_name not in wifi:
            failures.append(f"{rom_name} = {value}: no {header_name} in wifi_service.h")
            continue
        if wifi[header_name] != value:
            failures.append(
                f"{rom_name} = {value} but {header_name} = {wifi[header_name]}"
            )
            continue
        checked += 1

    if checked == 0:
        failures.append("no wifi service commands found in the ROM - parser broken?")

    # ---- net_service: absent commands must degrade, not break ---------------
    implemented = {v for k, v in net.items() if k.startswith("NET_CMD_")}
    for rom_name, value in sorted(rom.items()):
        if not rom_name.startswith("drv_net_") or rom_name == "drv_net_unsupported":
            continue
        if value not in implemented:
            notes.append(
                f"{rom_name} = {value} is not implemented here; the ROM falls back "
                f"on NET_ERR_UNSUPPORTED"
            )

    # the fallback only works if both sides agree on the code
    if "drv_net_unsupported" in rom:
        if "NET_ERR_UNSUPPORTED" not in net:
            failures.append("NET_ERR_UNSUPPORTED is gone from net_service.h")
        elif rom["drv_net_unsupported"] != net["NET_ERR_UNSUPPORTED"]:
            failures.append(
                f"drv_net_unsupported = {rom['drv_net_unsupported']} but "
                f"NET_ERR_UNSUPPORTED = {net['NET_ERR_UNSUPPORTED']}: the ROM's "
                f"fallback path would not trigger"
            )

    for note in notes:
        print(f"  note: {note}")
    for failure in failures:
        print(f"  FAIL: {failure}")
    if failures:
        return 1
    print(f"ROM INTERFACE OK: {checked} wifi commands match wifi_service.h by name")
    return 0


if __name__ == "__main__":
    sys.exit(main())
