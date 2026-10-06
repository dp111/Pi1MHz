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
   with any descriptions in one column, not run into the name;
5. *WGET, when the Pi answers with an error (a failed open or read, an HTTP
   status, or a stream that never delivers a byte), reports it and gives the
   service call back claimed with the stack balanced, as the MOS needs.

Checks 1-4 run with the ROM in sideways RAM and in a read-only bank, check 5
in sideways RAM only (without it the ROM declines the command).  These paths
are the ones that broke unseen: Pi1MHz always serves the ROM into
sideways RAM, and the shipped image uses the brief *HELP.

usage: check_service.py LABEL=ROM [LABEL=ROM ...]
"""
import os
import re
import sys

try:
    from rom6502 import Beeb, RomError, SimPi, Unmodelled, command_names
except ImportError as e:
    if "py65" in str(e):
        # Skipping is for a desk without py65; in CI it would hide every
        # check here behind a green step, as a missing beebasm once did.
        print("py65 not installed (pip install py65): skipped the 6502 service checks")
        sys.exit(1 if os.environ.get("CI") else 0)
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


# Service commands *WGET sends (net_wget.asm), and where the Pi publishes
# its reply: the command page at &FFF000, byte 1 on a read being the count.
NET_URL_OPEN, NET_URL_READ, NET_URL_CLOSE, NET_URL_STATUS = 60, 61, 63, 64
NET_COPY_PUBLIC = 58
NET_COUNT = 0xFFF001
NET_HTTP_STATUS = 0xFFF007

# One entry per place *WGET can fail: (what, option, Pi results by command,
# Pi replies by command, text the ROM must print).  -T prints the download
# and -U stores it in paged RAM, so each reaches different code.
WGET_ERRORS = [
    ("open fails (DNS, connect)", "-T", {NET_URL_OPEN: 0x2B}, {},
     "Network error &2B"),
    ("read fails", "-T", {NET_URL_READ: 0x2C}, {},
     "Network error &2C"),
    ("HTTP status", "-T", {NET_URL_OPEN: 0x30},
     {NET_URL_STATUS: {NET_HTTP_STATUS: 0x94, NET_HTTP_STATUS + 1: 0x01}},
     "Network error &30\nHTTP status &0194"),
    ("copy to paged RAM fails", "-U", {NET_COPY_PUBLIC: 0x2D},
     {NET_URL_READ: {NET_COUNT: 10}},
     "Network error &2D"),
]

# An open stream that never delivers a byte: the ROM polls for about fifty
# seconds of video frames (about 2,560 reads), which is 30 s of 6502 steps.
# The ROM reloads its own counter, so it cannot be shortened from here; the
# source builds share this code, so only the shipped image runs it.
WGET_TIMEOUT = [
    ("no bytes ever arrive", "-T", {}, {NET_URL_READ: {NET_COUNT: 0}},
     "Network timeout"),
]


def check_wget_errors(label, image, slow=False):
    """The Pi answers *WGET with an error.  The handler must print it, close
    the URL, and return to the MOS as service call 4 claimed (A=0, X and Y
    as offered, the stack back where the MOS left it).  Each error exit
    ends by restoring the X and Y the service entry pushed, so a handler
    that reaches it with a return address on top of them returns into the
    stack page instead."""
    for what, option, results, replies, text in WGET_ERRORS + (WGET_TIMEOUT if slow else []):
        what = f"{label}: *WGET, {what}"
        pi = SimPi(results, replies)
        b = Beeb(image, writable=True, pi=pi, max_steps=40_000_000)
        try:
            b.service(1, slot=5)
            b.text = ""
            # The command entry steps over the first character of the line
            # (the star) before it reads the name, whatever Y says.
            r = b.service(4, slot=5, y=1, line=f"*WGET {option} http://example.com/")
        except (RomError, Unmodelled) as e:
            check(False, f"{what}: {e} (Pi saw {len(pi.commands)} commands, last "
                         f"{pi.commands[-3:]}, pc &{b.cpu.pc:04X}, sp &{b.cpu.sp:02X})")
            continue
        check(r == (0, 5, 1), f"{what}: returned A,X,Y={r}, not claimed (0,5,1)")
        check(b.sp == 0xFF, f"{what}: returned with SP=&{b.sp:02X}, not &FF")
        check(text in b.text, f"{what}: printed {b.text!r}, not {text!r}")
        check(pi.commands.count(NET_URL_CLOSE) == 1,
              f"{what}: the URL was closed {pi.commands.count(NET_URL_CLOSE)} times, not once"
              f" (last commands {pi.commands[-3:]})")


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
        check_wget_errors(label, image, slow=label == "shipped")
    print(f"\n{checks} checks, {fails} failures")
    print("WIFI ROM SERVICE TESTS FAILED" if fails else "WIFI ROM SERVICE TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
