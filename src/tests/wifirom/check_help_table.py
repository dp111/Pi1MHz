#!/usr/bin/env python3
"""Walk the assembled command table the way the ROM's own dispatcher does.

Two faults this catches, both of which assemble cleanly and look right:

  * A bare address terminates the table, so an entry that loses its name -
    easy to do, because the first entry shares its line with the .commandtable
    label - silently ends the table early. Every command after it is still in
    the image, and none of them can be reached.

  * .help_descriptions is read in lockstep with the table, one line per entry,
    so an IF around a table entry needs the same IF around its description.
    Without it every later command prints its neighbour's text.

The ROM is built here in each supported flag combination, because both faults
appear only in some of them.
"""

import ast
import itertools
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "beeb" / "1mhz-wifi" / "src"
ROM_BASE = 0x8000
ROM_TOP = 0xC000

# The filing system half is inherited and is not in this tree; see
# beeb/1mhz-wifi/README.md. Point WICFS_SRC at a directory holding it to
# check the merged image too.
WICFS_SRC = os.environ.get("WICFS_SRC")
WICFS_FILES = ("wicfs.asm", "wicfs_errors.asm", "wicfs_messages.asm",
               "wicfs_catalogue.asm", "uef.asm", "host_launch.asm")


def assemble(work, flags):
    """Assemble into `work`, returning (image, labels) or None if it overflows."""
    beebasm = os.environ.get("BEEBASM", "beebasm")
    args = [beebasm, "-i", "1mhzwifi.asm", "-dd", "-labels", "labels.json"]
    for name, value in flags.items():
        args += ["-D", f"{name}={value}"]
    done = subprocess.run(args, cwd=work, capture_output=True, text=True)
    if done.returncode != 0:
        # Overflowing the bank is a legitimate answer for some combinations:
        # the merged image only fits with the RAM disc's neighbours dropped.
        if "skip backwards" in done.stdout + done.stderr:
            return None
        sys.exit(f"assembly failed for {flags}:\n{done.stdout}{done.stderr}")
    name = "1mhz-wicfs.rom" if flags.get("INCLUDE_WICFS") else "1mhz-wifi.rom"
    image = (work / name).read_bytes()
    # beebasm writes label values with a trailing L that JSON does not allow.
    labels = ast.literal_eval(re.sub(r"(?<=\d)L\b", "", (work / "labels.json").read_text()))
    if isinstance(labels, list):
        labels = labels[0]
    return image, labels


def walk_table(image, labels, flags):
    """Return the command names, exactly as the ROM's dispatcher finds them."""
    offset = labels[".commandtable"] - ROM_BASE
    names = []
    while image[offset] < 0x80:            # a name byte is always ASCII
        end = offset
        while image[end] < 0x80:
            end += 1
        name = image[offset:end].decode("ascii")
        address = (image[end] << 8) | image[end + 1]
        if not ROM_BASE <= address < ROM_TOP:
            sys.exit(f"{flags}: *{name} dispatches outside the bank, to &{address:04X}")
        names.append(name)
        offset = end + 2
    return names


def walk_descriptions(image, labels):
    """Every line the list actually holds, not just as many as there are
    commands: reading only that many would quietly accept a list with a line
    too few or too many, which is exactly what shifts the text by one."""
    offset = labels[".help_descriptions"] - ROM_BASE
    stop = labels[".print_help_end"] - ROM_BASE
    out = []
    while offset < stop:
        end = image.index(b"\r", offset)
        out.append(image[offset:end].decode("latin1"))
        offset = end + 1
    return out


def check(work, flags, expected):
    built = assemble(work, flags)
    if built is None:
        return "does not fit"
    image, labels = built
    names = walk_table(image, labels, flags)
    missing = [name for name in expected if name not in names]
    if missing:
        sys.exit(f"{flags}: unreachable through the table: {', '.join(missing)}")

    if ".help_descriptions" not in labels:
        return f"{len(names)} commands, names-only help"

    texts = walk_descriptions(image, labels)
    if len(texts) != len(names):
        sys.exit(f"{flags}: {len(names)} commands but {len(texts)} description "
                 f"lines, so *HELP pairs them off by one - every IF around a "
                 f"table entry needs the same IF around its description")
    for name, text in zip(names, texts):
        if not text.strip():
            sys.exit(f"{flags}: *{name} has an empty description")
    return f"{len(names)} commands, {len(texts)} descriptions"


def main():
    beebasm = os.environ.get("BEEBASM", "beebasm")
    if not shutil.which(beebasm):
        print("beebasm not found (set BEEBASM): skipped the command table check")
        return 0

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for source in SRC.glob("*.asm"):
            shutil.copy(source, work)

        merged = False
        if WICFS_SRC:
            for name in WICFS_FILES:
                shutil.copy(Path(WICFS_SRC) / name, work)
            merged = True

        for wicfs in (0, 1) if merged else (0,):
            # Every command the image claims to offer, whatever else is off.
            expected = ["WGET", "WIFI", "NSLOOK", "MODE"]
            if wicfs:
                expected += ["UEF", "WICFS", "REWIND", "QUPRUN", "QUPCFS"]
            for ramdisk, pdump, brief in itertools.product((1, 0), repeat=3):
                flags = {"INCLUDE_RAMDISK": ramdisk, "INCLUDE_PDUMP": pdump,
                         "HELP_BRIEF": brief, "INCLUDE_WICFS": wicfs}
                wanted = list(expected)
                if ramdisk:
                    wanted.append("RDLOAD")
                if pdump:
                    wanted.append("PRD")
                result = check(work, flags, wanted)
                print(f"  wicfs={wicfs} ramdisc={ramdisk} pdump={pdump} "
                      f"brief={brief}: {result}")

    print("COMMAND TABLE OK" + ("" if merged else " (network image only)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
