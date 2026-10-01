#!/usr/bin/env python3
"""Run the 1MHz-WiFi ROM's service entry on a 6502 and check what every
machine with the ROM fitted depends on, whether or not a Pi answers:

1. reset (service 1) returns unclaimed with X and Y intact, and changes no
   MOS state but the banner bit of OSBYTE &D7;
2. a command that is not the ROM's (service 4) is passed on - A=4, X and Y
   intact, no error - so other ROMs and the MOS still get it;
3. in a bank that is not writable, the ROM's own commands are declined too,
   because every handler needs the workspace inside the image;
4. *HELP is passed on, and *HELP WIFI lists every command in the table once,
   with any descriptions in one column, not run into the name.

Each check runs with the ROM in sideways RAM and in a read-only bank.  These
paths are the ones that broke unseen: Pi1MHz always serves the ROM into
sideways RAM, and the shipped image uses the brief *HELP.

usage: check_service.py LABEL=ROM [LABEL=ROM ...]
"""
import re
import sys

try:
    from rom6502 import Beeb, RomError, Unmodelled, command_names
except ImportError as e:
    if "py65" in str(e):
        print("py65 not installed (pip install py65): skipped the 6502 service checks")
        sys.exit(0)
    raise

checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print(f"FAIL: {what}")


def run(image, what, reason, writable, **kw):
    """One service call on a fresh machine; (machine, (A, X, Y)) or None.
    Anything but a reset comes after one, as on the machine: reset is where
    the ROM finds out whether its bank is writable."""
    machine_kw = {k: kw.pop(k) for k in ("reset_type", "machine") if k in kw}
    b = Beeb(image, writable=writable, **machine_kw)
    if "startup" in kw:
        b.osvars[0xD7] = kw.pop("startup")
    try:
        if reason != 1:
            b.service(1, slot=kw.get("slot", 5))
            b.text = ""
        return b, b.service(reason, **kw)
    except RomError as e:
        check(False, f"{what}: raised {e}")
    except Unmodelled as e:
        check(False, f"{what}: {e}")
    return b, None


def check_reset(label, image, writable, bank):
    for reset_type in (0, 1, 2):
        for slot in (3, 5, 12, 15):
            for startup in (0x80, 0xA5):
                what = f"{label}, {bank}: reset type {reset_type} in slot {slot}"
                b, r = run(image, what, 1, writable, reset_type=reset_type,
                           slot=slot, y=0x23, startup=startup)
                if r is None:
                    continue
                check(r == (1, slot, 0x23), f"{what}: returned A,X,Y={r}, not unclaimed (1,{slot},&23)")
                check(b.osvars[0xD7] == startup & 0x7F,
                      f"{what}: OSBYTE &D7 left &{b.osvars[0xD7]:02X}, expected "
                      f"&{startup & 0x7F:02X} (calls {[tuple(hex(v) for v in c) for c in b.osbytes]})")


def check_commands(label, image, writable, bank, names):
    others = ["DISC", "TAPE", "FOOBAR", "W", "WIF", "BASIC"]
    for cmd in others:
        what = f"{label}, {bank}: *{cmd}"
        _, r = run(image, what, 4, writable, slot=5, y=0, line=cmd)
        if r is not None:
            check(r == (4, 5, 0), f"{what}: returned A,X,Y={r}, not passed on (4,5,0)")
    if not writable:
        for cmd in names:
            what = f"{label}, {bank}: its own *{cmd}"
            _, r = run(image, what, 4, writable, slot=5, y=0, line=cmd)
            if r is not None:
                check(r[0] == 4, f"{what}: returned A={r[0]}, not declined")


def check_help(label, image, writable, bank, names):
    what = f"{label}, {bank}: *HELP"
    b, r = run(image, what, 9, writable, slot=5, y=0, line="")
    if r is not None:
        check(r == (9, 5, 0), f"{what}: returned A,X,Y={r}, not passed on (9,5,0)")
        check("1MHz-WiFi" in b.text, f"{what}: no title in {b.text!r}")

    what = f"{label}, {bank}: *HELP WIFI"
    b, r = run(image, what, 9, writable, slot=5, y=0, line="WIFI")
    if r is None:
        return
    check(r[0] == 0, f"{what}: returned A={r[0]}, not claimed")
    listed = []
    columns = set()
    for line in b.text.split("\n")[1:]:
        if not line.strip():
            continue
        tokens = line.split()
        if all(t in names for t in tokens):        # the brief form: names only
            listed += tokens
            continue
        m = re.match(r" (\S+)( +)\S", line)
        if not m or m.group(1) not in names:
            check(False, f"{what}: {line!r} does not start with a command name and a space")
            continue
        listed.append(m.group(1))
        if len(m.group(1)) + 1 < 11:                # short names line up
            columns.add(m.end() - 1)
    check(len(columns) <= 1, f"{what}: descriptions start in columns {sorted(columns)}")
    check(sorted(listed) == sorted(names),
          f"{what}: lists {sorted(listed)}, the table has {sorted(names)}")


def main(args):
    if not args:
        print(__doc__)
        return 2
    for arg in args:
        label, _, path = arg.partition("=")
        image = open(path, "rb").read()
        names = command_names(image)
        print(f"== {label}: {len(names)} commands ==")
        for writable, bank in ((True, "sideways RAM"), (False, "read-only bank")):
            check_reset(label, image, writable, bank)
            check_commands(label, image, writable, bank, names)
            check_help(label, image, writable, bank, names)
    print(f"\n{checks} checks, {fails} failures")
    print("WIFI ROM SERVICE TESTS FAILED" if fails else "WIFI ROM SERVICE TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
