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
   service call back claimed with the stack balanced, as the MOS needs;
6. a command is found wherever the MOS's Y says its name starts: OSCLI
   "WIFI ON" with no star, "**WIFI ON" and "* WIFI ON" all reach *WIFI;
7. the driver's read_buffer hands back every byte of a reply that runs past
   the end of a JIM page, the byte at each page boundary included;
8. a command whose name only starts with one of the ROM's (*TIMER, *LAPSE,
   *PINGALL) is passed on, while the ROM's own name followed by anything but
   a letter, and its abbreviations, still reach the handler;
9. *JOIN's password prompt ends a 128-character password with a CR in the
   parameter block, and Escape at the prompt is acknowledged and gives the
   call back claimed without sending anything to the Pi.

Checks 1-4 run with the ROM in sideways RAM and in a read-only bank, checks
5-9 in sideways RAM only (without it the ROM declines the command).  These paths
are the ones that broke unseen: Pi1MHz always serves the ROM into
sideways RAM, and the shipped image uses the brief *HELP.

usage: check_service.py LABEL=ROM [LABEL=ROM ...]
"""
import os
import re
import sys

try:
    from rom6502 import (Beeb, RomError, SimPi, Unmodelled, command_names,
                         read_buffer_address)
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
            # Y past the star, as the MOS leaves it for "*WGET".
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


# The MOS steps over any spaces and stars in front of a command and passes
# the offset of its name from (&F2) in Y, so OSCLI "WIFI ON" arrives with
# Y=0 and "**WIFI ON" with Y=2.  The last line leaves Y on spaces the MOS
# would have skipped, which the ROM skips too.
NET_RADIO = 91          # drv_svc_radio, what *WIFI ON sends
OSCLI_LINES = [("WIFI ON", 0), ("*WIFI ON", 1), ("**WIFI ON", 2),
               ("  * WIFI ON", 4), ("*  WIFI ON", 1)]


def check_oscli(label, image):
    for line, y in OSCLI_LINES:
        what = f"{label}: OSCLI {line!r} with Y={y}"
        # The Pi's reply, "OK" and a CR, from byte 1 of the service page.
        pi = SimPi(replies={NET_RADIO: {0xFFFF01: ord("O"), 0xFFFF02: ord("K"),
                                        0xFFFF03: 13, 0xFFFF04: 0}})
        b = Beeb(image, writable=True, pi=pi)
        try:
            b.service(1, slot=5)
            b.text = ""
            r = b.service(4, slot=5, y=y, line=line)
        except (RomError, Unmodelled) as e:
            check(False, f"{what}: {e}")
            continue
        check(r == (0, 5, y), f"{what}: returned A,X,Y={r}, not claimed (0,5,{y})")
        check(b.sp == 0xFF, f"{what}: returned with SP=&{b.sp:02X}, not &FF")
        check(pi.commands == [NET_RADIO],
              f"{what}: the Pi saw commands {pi.commands}, not [{NET_RADIO}]")
        check("Switching wifi on" in b.text and "OK" in b.text,
              f"{what}: printed {b.text!r}")


# Lines that only start with a name of the ROM's belong to other ROMs; a name
# ended by a space, a digit, a dot or the end of the line, or abbreviated with
# a dot, is the ROM's.  A handler that runs may claim the call or raise an
# error (no Pi answers here); either way it was not passed on.
OTHERS_PREFIXED = ["TIMER", "LAPSE", "PINGALL", "WIFIX", "QRX", "LAPOPTS"]
OWN_ENDED = ["TIME", "TIME.", "WTIME", "WTIME.", "TIME1", "TI.", "LAP", "LAPOPT", "PING",
             "PING ", "WIF. ON", "WIFI ON"]


def check_word_boundary(label, image):
    for line in OTHERS_PREFIXED:
        what = f"{label}: *{line}"
        _, r = run(image, what, 4, True, slot=5, y=0, line=line)
        if r is not None:
            check(r == (4, 5, 0), f"{what}: returned A,X,Y={r}, not passed on (4,5,0)")
    for line in OWN_ENDED:
        what = f"{label}: *{line}"
        b = Beeb(image, writable=True, pi=SimPi())
        try:
            b.service(1, slot=5)
            r = b.service(4, slot=5, y=0, line=line)
        except (RomError, Unmodelled):
            continue                            # the handler ran
        check(r != (4, 5, 0), f"{what}: passed on, not handled")


# The parameter block *JOIN builds, "<ssid> CR <password> CR" (machine.asm).
HEAP, STRBUF = 0xBE00, 0xBF00


def join_prompt(image, keys):
    """*JOIN NET with `keys` typed at the password prompt; the machine, the
    Pi and the (A, X, Y) it returned, or None if it raised a MOS error."""
    pi = SimPi()
    b = Beeb(image, writable=True, pi=pi)
    b.service(1, slot=5)
    b.mem.ram[STRBUF:STRBUF + 0x100] = b"Z" * 0x100     # no CR to find by luck
    b.keys = list(keys)
    try:
        return b, pi, b.service(4, slot=5, y=0, line="JOIN NET")
    except RomError:                        # no Pi answers the join itself
        return b, pi, None


def check_join(label, image):
    what = f"{label}: *JOIN with a 130-character password typed"
    try:
        b, pi, _ = join_prompt(image, [ord("A")] * 130 + [13])
    except Unmodelled as e:
        check(False, f"{what}: {e}")
    else:
        block = bytes(b.mem.ram[HEAP:HEAP + 0x100])
        check(block.startswith(b"NET\r" + b"A" * 128 + b"\r"),
              f"{what}: parameter block {block[:140]!r}, not the first 128 ended by a CR")
    what = f"{label}: Escape at *JOIN's password prompt"
    try:
        b, pi, r = join_prompt(image, [ord("a"), ord("b"), 0x1B])
    except Unmodelled as e:
        check(False, f"{what}: {e}")
        return
    check(r == (0, 5, 0), f"{what}: returned A,X,Y={r}, not claimed (0,5,0)")
    check(not pi.commands, f"{what}: the Pi saw commands {pi.commands}")
    check(not b.escape, f"{what}: Escape left unacknowledged")


def check_read_buffer(label, image):
    """read_buffer walks a reply in the JIM page window, stepping to the
    next page when X wraps.  Nearly three pages must come back byte for
    byte, in A and in N/Z, as print_string, fnd and search0a rely on."""
    what = f"{label}: read_buffer across JIM pages"
    pi = SimPi()
    b = Beeb(image, writable=True, pi=pi)
    try:
        address = read_buffer_address(image)
        b.service(1, slot=5)
    except (ValueError, RomError, Unmodelled) as e:
        check(False, f"{what}: {e}")
        return
    shadow = image[address - 0x8000 + 3] | image[address - 0x8000 + 4] << 8
    b.mem[shadow] = 0                       # where reset_buffer leaves it, X=0
    data = [(i * 7 + 1) % 255 + 1 for i in range(700)]   # never 0: no early Z
    for i, v in enumerate(data):
        pi.jim[i] = v                       # JIM 00:00:page, the reply buffer
    x, wrong = 0, []
    for i, v in enumerate(data):
        b.cpu.sp = 0xFF
        try:
            a, x, _ = b.call(address, 0, x)
        except (RomError, Unmodelled) as e:
            check(False, f"{what}: byte {i}: {e}")
            return
        z = bool(b.cpu.p & 0x02)
        if a != v or z:
            wrong.append(f"byte {i}: A=&{a:02X} Z={int(z)}, expected &{v:02X}")
    check(not wrong, f"{what}: {len(wrong)} wrong, first {wrong[:3]}")
    check(b.mem[shadow] == len(data) >> 8 and x == len(data) & 0xFF,
          f"{what}: ended at page {b.mem[shadow]} X=&{x:02X}, not page "
          f"{len(data) >> 8} X=&{len(data) & 0xFF:02X}")


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
        check_oscli(label, image)
        check_word_boundary(label, image)
        check_join(label, image)
        check_read_buffer(label, image)
    print(f"\n{checks} checks, {fails} failures")
    print("WIFI ROM SERVICE TESTS FAILED" if fails else "WIFI ROM SERVICE TESTS PASSED")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
