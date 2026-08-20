#!/usr/bin/env python3
"""
sdio_decode.py - offline SD / SDIO protocol decoder for scope captures

Why this exists
---------------
The Siglent SDS2000X Plus (like every other Siglent scope in that family)
has a *fixed* set of serial decoders in firmware - I2C, SPI, UART, CAN,
LIN, CAN FD, FlexRay, I2S, 1553B.  There is no plugin API, no scripting
hook and no user-installable decoder, so an "SDIO" entry cannot be added
to the scope's Decode menu.  What the scope *is* very good at is capturing
the bus: 2 GSa/s, deep memory, and 16 MSO channels if the logic probe
option is fitted - more than enough for a 25/50 MHz SDIO bus.

So the decode happens here instead: capture CLK/CMD (+ DAT0-3 if you have
the channels) on the scope, export the waveform as CSV, and run this tool
over it.  It prints the same thing an on-scope decoder would, plus the
CYW43-specific annotation the Pi1MHz WiFi bring-up actually needs.

See docs/dev/sdio-protocol-decode.md for the scope setup, the export
procedure and the on-scope SPI-decoder trick for quick eyeball checks.

What it decodes
---------------
  * CMD line: 48-bit commands and R1/R1b/R2/R3/R4/R5/R6/R7 responses,
    with CRC7 checking and per-command argument breakdown.
  * DAT lines: 1-bit (DAT0) and 4-bit (DAT3..DAT0) data blocks with
    per-line CRC16 checking, plus the write CRC status token and the
    following busy period.
  * SDIO specifics: CMD52/CMD53 argument decode, R5 flag decode, CMD5
    OCR/R4 decode, automatic bus-width and block-size tracking from the
    CCCR/FBR writes seen earlier in the same capture.
  * CYW43 (Pi Zero W / Zero 2 W / Pi 3B+ WiFi) annotation: F1 register
    names taken from src/wifi/sdio.c, backplane window tracking so F1
    CMD53 accesses are shown as absolute backplane addresses, and an
    SDPCM/BDC/CDC dissection of the F2 frame payloads.

Typical use
-----------
  # CLK + CMD on two analog channels, times taken from the CSV
  ./sdio_decode.py capture.csv --clk C1 --cmd C2

  # MSO capture, full 4-bit bus, SDPCM payload dissection
  ./sdio_decode.py capture.csv --clk D0 --cmd D1 --dat D2,D3,D4,D5

  # no time column in the export (Siglent binary-to-CSV converters vary)
  ./sdio_decode.py capture.csv --clk C1 --cmd C2 --sample-rate 500e6

  # push the digitised lines into PulseView / sigrok for a visual look
  ./sdio_decode.py capture.csv --clk C1 --cmd C2 --vcd capture.vcd

  # no scope needed: synthesise a bus and check the decoder end to end
  ./sdio_decode.py --selftest

Requires: python3 only.  numpy is used if it happens to be installed
(purely a speed-up on multi-megapoint captures).
"""

import argparse
import json
import re
import sys

try:
   import numpy as _np
except ImportError:   # pragma: no cover - numpy is an optional speed-up
   _np = None


# ---------------------------------------------------------------------------
# CRCs
# ---------------------------------------------------------------------------

def _make_crc7_table():
   """Byte table for CRC7 with poly x^7 + x^3 + 1 (0x89)."""
   table = []
   for value in range(256):
      crc = (value ^ 0x89) if (value & 0x80) else value
      for _ in range(7):
         crc = (crc << 1) & 0xFF
         if crc & 0x80:
            crc ^= 0x89
      table.append(crc)
   return table


_CRC7_TABLE = _make_crc7_table()


def crc7(data):
   """CRC7 (x^7 + x^3 + 1) over whole bytes, returned in the low 7 bits.

   crc7(b'\x40\x00\x00\x00\x00') == 0x4A - the CMD0 token every SD
   card driver hard-codes as 0x95 = (0x4A << 1) | 1.
   """
   crc = 0
   for byte in data:
      crc = _CRC7_TABLE[((crc << 1) ^ byte) & 0xFF]
   return crc


def crc16_bits(bits):
   """CRC16-CCITT (x^16 + x^12 + x^5 + 1, init 0) over a bit sequence.

   SD data lines need the bit-wise form: in 4-bit mode each DAT line
   carries its own CRC over its own - not byte aligned - bit stream.
   """
   crc = 0
   for bit in bits:
      feedback = ((crc >> 15) ^ (bit & 1)) & 1
      crc = (crc << 1) & 0xFFFF
      if feedback:
         crc ^= 0x1021
   return crc


# ---------------------------------------------------------------------------
# Protocol tables
# ---------------------------------------------------------------------------

CMD_NAMES = {
   0: "GO_IDLE_STATE", 2: "ALL_SEND_CID", 3: "SEND_RELATIVE_ADDR",
   4: "SET_DSR", 5: "IO_SEND_OP_COND", 6: "SWITCH_FUNC",
   7: "SELECT/DESELECT_CARD", 8: "SEND_IF_COND", 9: "SEND_CSD",
   10: "SEND_CID", 11: "VOLTAGE_SWITCH", 12: "STOP_TRANSMISSION",
   13: "SEND_STATUS", 15: "GO_INACTIVE_STATE", 16: "SET_BLOCKLEN",
   17: "READ_SINGLE_BLOCK", 18: "READ_MULTIPLE_BLOCK", 19: "SEND_TUNING_BLOCK",
   23: "SET_BLOCK_COUNT", 24: "WRITE_BLOCK", 25: "WRITE_MULTIPLE_BLOCK",
   27: "PROGRAM_CSD", 28: "SET_WRITE_PROT", 29: "CLR_WRITE_PROT",
   30: "SEND_WRITE_PROT", 32: "ERASE_WR_BLK_START", 33: "ERASE_WR_BLK_END",
   38: "ERASE", 42: "LOCK_UNLOCK", 52: "IO_RW_DIRECT", 53: "IO_RW_EXTENDED",
   55: "APP_CMD", 56: "GEN_CMD", 58: "READ_OCR", 59: "CRC_ON_OFF",
}

ACMD_NAMES = {
   6: "SET_BUS_WIDTH", 13: "SD_STATUS", 22: "SEND_NUM_WR_BLOCKS",
   23: "SET_WR_BLK_ERASE_COUNT", 41: "SD_SEND_OP_COND",
   42: "SET_CLR_CARD_DETECT", 51: "SEND_SCR",
}

# Response type per command index.  R1b/R2/R3/R4/R5/R6/R7 all matter for
# how many bits to consume and how to print the payload.
RESPONSE_TYPES = {
   0: None, 2: "R2", 3: "R6", 4: None, 5: "R4", 6: "R1", 7: "R1b",
   8: "R7", 9: "R2", 10: "R2", 11: "R1", 12: "R1b", 13: "R1",
   15: None, 16: "R1", 17: "R1", 18: "R1", 19: "R1", 23: "R1",
   24: "R1", 25: "R1", 27: "R1", 28: "R1b", 29: "R1b", 30: "R1",
   32: "R1", 33: "R1", 38: "R1b", 42: "R1", 52: "R5", 53: "R5",
   55: "R1", 56: "R1", 58: "R3", 59: "R1",
}

ACMD_RESPONSE_TYPES = {
   6: "R1", 13: "R1", 22: "R1", 23: "R1", 41: "R3", 42: "R1", 51: "R1",
}

# Commands with a data phase: (direction, uses_block_count)
DATA_COMMANDS = {
   6: ("read", False), 8: (None, False), 17: ("read", False),
   18: ("read", True), 19: ("read", False), 24: ("write", False),
   25: ("write", True), 30: ("read", False), 42: ("write", False),
   56: (None, False),
}

CARD_STATE_NAMES = {
   0: "idle", 1: "ready", 2: "ident", 3: "stby", 4: "tran",
   5: "data", 6: "rcv", 7: "prg", 8: "dis",
}

# R1 card-status error/informational bits worth naming.
CARD_STATUS_BITS = [
   (31, "OUT_OF_RANGE"), (30, "ADDRESS_ERROR"), (29, "BLOCK_LEN_ERROR"),
   (28, "ERASE_SEQ_ERROR"), (27, "ERASE_PARAM"), (26, "WP_VIOLATION"),
   (25, "CARD_IS_LOCKED"), (24, "LOCK_UNLOCK_FAILED"), (23, "COM_CRC_ERROR"),
   (22, "ILLEGAL_COMMAND"), (21, "CARD_ECC_FAILED"), (20, "CC_ERROR"),
   (19, "ERROR"), (16, "CSD_OVERWRITE"), (15, "WP_ERASE_SKIP"),
   (14, "CARD_ECC_DISABLED"), (13, "ERASE_RESET"), (8, "READY_FOR_DATA"),
   (5, "APP_CMD"), (3, "AKE_SEQ_ERROR"),
]

# R5 (CMD52/CMD53 response) flag byte.
R5_FLAG_BITS = [
   (7, "COM_CRC_ERROR"), (6, "ILLEGAL_COMMAND"), (3, "ERROR"),
   (1, "FUNCTION_NUMBER"), (0, "OUT_OF_RANGE"),
]
R5_STATE_NAMES = {0: "DIS", 1: "CMD", 2: "TRN", 3: "RFU"}

# CCCR - the function 0 register file, common to every SDIO card.
CCCR_NAMES = {
   0x00: "CCCR_SDIO_REV", 0x01: "SD_SPEC_REV", 0x02: "IO_ENABLE",
   0x03: "IO_READY", 0x04: "INT_ENABLE", 0x05: "INT_PENDING",
   0x06: "IO_ABORT", 0x07: "BUS_INTERFACE_CONTROL", 0x08: "CARD_CAPABILITY",
   0x09: "CIS_POINTER_LOW", 0x0A: "CIS_POINTER_MID", 0x0B: "CIS_POINTER_HIGH",
   0x0C: "BUS_SUSPEND", 0x0D: "FUNCTION_SELECT", 0x0E: "EXEC_FLAGS",
   0x0F: "READY_FLAGS", 0x10: "FN0_BLOCK_SIZE_LOW",
   0x11: "FN0_BLOCK_SIZE_HIGH", 0x12: "POWER_CONTROL", 0x13: "HIGH_SPEED",
}

# CYW43 function 1 registers.  Names match src/wifi/sdio.c so a decode
# line can be grepped straight back into the driver.
CYW43_F1_NAMES = {
   0x10000: "SDIOD_CCCR_?_F1BASE", 0x10008: "SDIO_FUNCTION2_WATERMARK",
   0x1000A: "SDIO_BACKPLANE_ADDRESS_LOW",
   0x1000B: "SDIO_BACKPLANE_ADDRESS_MID",
   0x1000C: "SDIO_BACKPLANE_ADDRESS_HIGH",
   0x1000D: "SDIO_FRAME_CONTROL", 0x1000E: "SDIO_CHIP_CLOCK_CSR",
   0x1000F: "SDIO_PULLUP_CONTROL",
   0x1001B: "SDIO_READ_FRAME_BC_LOW", 0x1001C: "SDIO_READ_FRAME_BC_HIGH",
   0x1001E: "SDIO_WAKEUP_CTRL", 0x1001F: "SDIO_SLEEP_CSR",
}

# Bit names for the two CYW43 F1 registers whose contents drive bring-up.
CHIP_CLOCK_CSR_BITS = [
   (0, "FORCE_ALP"), (1, "FORCE_HT"), (2, "FORCE_ILP"), (3, "ALP_AVAIL_REQ"),
   (4, "HT_AVAIL_REQ"), (5, "FORCE_HW_CLKREQ_OFF"), (6, "ALP_AVAIL"),
   (7, "HT_AVAIL"),
]
SLEEP_CSR_BITS = [(0, "KEEP_WL_KSO"), (1, "WL_DEVON")]

SDPCM_CHANNEL_NAMES = {0: "CONTROL", 1: "EVENT", 2: "DATA", 3: "GLOM", 15: "TEST"}


def fbr_name(address):
   """Name a function basic register (FBR) address, or None."""
   function_number = address >> 8
   if function_number < 1 or function_number > 7 or address > 0x7FF:
      return None
   offset = address & 0xFF
   names = {
      0x00: "FUNCTION_INTERFACE_CODE", 0x01: "EXT_FUNCTION_INTERFACE_CODE",
      0x02: "POWER_SELECT", 0x03: "?", 0x09: "CIS_POINTER_LOW",
      0x0A: "CIS_POINTER_MID", 0x0B: "CIS_POINTER_HIGH",
      0x0C: "CSA_POINTER_LOW", 0x0D: "CSA_POINTER_MID",
      0x0E: "CSA_POINTER_HIGH", 0x0F: "CSA_DATA_WINDOW",
      0x10: "BLOCK_SIZE_LOW", 0x11: "BLOCK_SIZE_HIGH",
   }
   if offset in names:
      return "F%u_%s" % (function_number, names[offset])
   return None


def register_name(function_number, address, annotate):
   """Best-effort symbolic name for an SDIO register address."""
   if annotate == "none":
      return None
   if function_number == 0:
      if address in CCCR_NAMES:
         return CCCR_NAMES[address]
      return fbr_name(address)
   if annotate in ("auto", "cyw43") and function_number == 1:
      return CYW43_F1_NAMES.get(address)
   return None


def decode_bit_names(value, bits):
   """['FORCE_ALP', 'ALP_AVAIL'] for the set bits of value."""
   return [name for bit, name in bits if value & (1 << bit)]


# ---------------------------------------------------------------------------
# Capture loading
#
# Siglent's CSV export has changed shape across firmware revisions (and
# differs again between the analog and the MSO export), so nothing here
# assumes a fixed header.  The rule is simply: the first line whose every
# field parses as a number starts the data, and the last non-empty line
# above it - if it has the same field count - names the columns.
# ---------------------------------------------------------------------------

TIME_COLUMN_RE = re.compile(r"^(second|seconds|time|t|s|x)$", re.IGNORECASE)
INTERVAL_KEY_RE = re.compile(r"(sample|sampling|time|horizontal)\s*interval|"
                             r"sampling\s*period", re.IGNORECASE)
NUMBER_RE = re.compile(r"[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?")


class Capture(object):
   """Digitised channels plus the timebase needed to stamp them."""

   def __init__(self, columns, sample_interval, start_time=0.0):
      self.columns = columns                 # name -> list/array of floats
      self.sample_interval = sample_interval
      self.start_time = start_time
      self.length = len(next(iter(columns.values()))) if columns else 0

   def resolve(self, spec):
      """Map a user channel spec ('C2', 'ch2', 'D0', '1') to a column."""
      if spec is None:
         return None
      key = spec.strip().lower()
      lowered = {name.strip().lower(): name for name in self.columns}
      if key in lowered:
         return lowered[key]
      # C2 == CH2 == channel2, D3 == digital 3, and bare indices count
      # from the first non-time column.
      aliases = [key]
      match = re.match(r"^(?:c|ch|channel)\s*(\d+)$", key)
      if match:
         aliases += ["c%s" % match.group(1), "ch%s" % match.group(1),
                     "channel%s" % match.group(1), "chan%s" % match.group(1)]
      match = re.match(r"^(?:d|dig|digital)\s*(\d+)$", key)
      if match:
         aliases += ["d%s" % match.group(1), "digital%s" % match.group(1),
                     "dig%s" % match.group(1), "d%02d" % int(match.group(1))]
      for alias in aliases:
         if alias in lowered:
            return lowered[alias]
      if key.isdigit():
         names = list(self.columns)
         index = int(key)
         if 0 <= index < len(names):
            return names[index]
      raise SystemExit("channel '%s' not in capture (have: %s)"
                       % (spec, ", ".join(self.columns)))


def _split(line, delimiter):
   return [field.strip().strip('"') for field in line.split(delimiter)]


def _all_numeric(fields):
   if len(fields) < 2:
      return False
   for field in fields:
      if field == "":
         return False
      try:
         float(field)
      except ValueError:
         return False
   return True


def _guess_delimiter(line):
   for delimiter in (",", ";", "\t"):
      if delimiter in line:
         return delimiter
   return None


def load_csv(path, sample_rate=None, time_column=None, max_samples=None):
   """Load a scope CSV export into a Capture."""
   header = None
   delimiter = ","
   data_start = 0
   metadata_interval = None

   with open(path, "r", errors="replace") as handle:
      preamble = []
      for line_number, line in enumerate(handle):
         line = line.rstrip("\r\n")
         candidate_delimiter = _guess_delimiter(line) or ","
         fields = _split(line, candidate_delimiter)
         if _all_numeric(fields):
            delimiter = candidate_delimiter
            data_start = line_number
            for previous in reversed(preamble):
               previous_fields = _split(previous, delimiter)
               if previous.strip() == "":
                  continue
               if len(previous_fields) == len(fields):
                  header = previous_fields
               break
            for previous in preamble:
               if INTERVAL_KEY_RE.search(previous):
                  match = NUMBER_RE.search(previous.split(
                     delimiter, 1)[-1] if delimiter in previous else previous)
                  if match:
                     metadata_interval = float(match.group(0))
            break
         preamble.append(line)
         if line_number > 400:
            raise SystemExit("%s: no numeric data found in the first 400 "
                             "lines - is this a scope CSV export?" % path)
      else:
         raise SystemExit("%s: no numeric data found" % path)

   if header is None:
      header = []

   columns = []
   with open(path, "r", errors="replace") as handle:
      for _ in range(data_start):
         handle.readline()
      for line in handle:
         fields = line.rstrip("\r\n").split(delimiter)
         if len(fields) < 2:
            continue
         try:
            values = [float(field) for field in fields]
         except ValueError:
            continue                       # trailing junk / footer lines
         if not columns:
            columns = [[] for _ in values]
         if len(values) != len(columns):
            continue
         for index, value in enumerate(values):
            columns[index].append(value)
         if max_samples and len(columns[0]) >= max_samples:
            break

   if not columns or not columns[0]:
      raise SystemExit("%s: no samples decoded" % path)

   names = []
   for index in range(len(columns)):
      if index < len(header) and header[index]:
         names.append(header[index])
      else:
         names.append("col%d" % index)
   # Siglent writes "Second,Value,Value"; give the value columns the
   # channel names from the "Source,CH1,CH2" line when it is present.
   if names.count("Value") > 1:
      names = [name if name != "Value" else "col%d" % index
               for index, name in enumerate(names)]

   time_index = None
   if time_column is not None:
      for index, name in enumerate(names):
         if name.strip().lower() == time_column.strip().lower():
            time_index = index
            break
      if time_index is None:
         raise SystemExit("time column '%s' not found" % time_column)
   else:
      for index, name in enumerate(names):
         if TIME_COLUMN_RE.match(name.strip()):
            time_index = index
            break

   start_time = 0.0
   interval = None
   if time_index is not None:
      times = columns[time_index]
      start_time = times[0]
      if len(times) > 1:
         interval = (times[-1] - times[0]) / (len(times) - 1)
   if sample_rate:
      interval = 1.0 / sample_rate
   elif interval is None:
      interval = metadata_interval
   if not interval or interval <= 0:
      raise SystemExit("cannot determine the sample interval: the export has "
                       "no time column and no interval metadata - pass "
                       "--sample-rate")

   channels = {}
   for index, name in enumerate(names):
      if index == time_index:
         continue
      channels[name] = columns[index]
   if not channels:
      raise SystemExit("%s: no data channels (only a time column?)" % path)
   return Capture(channels, interval, start_time)


# ---------------------------------------------------------------------------
# Digitising and sampling
# ---------------------------------------------------------------------------

def auto_threshold(values):
   """Midpoint threshold.  Works for 0/1 logic exports and 3V3 analog alike."""
   low = min(values)
   high = max(values)
   if high - low < 1e-9:
      raise SystemExit("channel is flat (%.3f V) - wrong channel, or the "
                       "probe was not connected" % low)
   return low + (high - low) * 0.5


def digitize(values, threshold, hysteresis=0.0):
   """Return a bytes() of 0/1, one per sample."""
   if hysteresis > 0.0:
      low = threshold - hysteresis
      high = threshold + hysteresis
      out = bytearray(len(values))
      state = 1 if values[0] > threshold else 0
      for index, value in enumerate(values):
         if state and value < low:
            state = 0
         elif not state and value > high:
            state = 1
         out[index] = state
      return bytes(out)
   if _np is not None:
      return (_np.asarray(values) > threshold).astype(_np.uint8).tobytes()
   return bytes(1 if value > threshold else 0 for value in values)


def find_edges(digital, rising=True, min_width=0):
   """Indices of the sample at which the line has just changed level."""
   pattern = b"\x00\x01" if rising else b"\x01\x00"
   edges = []
   position = digital.find(pattern)
   while position != -1:
      edges.append(position + 1)
      position = digital.find(pattern, position + 1)
   if min_width > 1 and edges:
      filtered = [edges[0]]
      for edge in edges[1:]:
         if edge - filtered[-1] >= min_width:
            filtered.append(edge)
      edges = filtered
   return edges


class Samples(object):
   """The bus as the card sees it: one sample per active clock edge."""

   def __init__(self, times, cmd_bits, dat_nibbles, dat_lines,
                digital=None, sample_interval=0.0):
      self.times = times                 # float seconds, per clock edge
      self.cmd = cmd_bits                # bytes of 0/1
      self.dat = dat_nibbles             # bytes, bit n = DATn, or None
      self.dat_lines = dat_lines         # count of probed DAT lines
      self.count = len(times)
      self.digital = digital or {}       # full-rate 0/1 traces, for VCD
      self.sample_interval = sample_interval


def sample_bus(capture, clk_name, cmd_name, dat_names, threshold=None,
               hysteresis=0.0, edge="rising", min_width=0):
   """Digitise, find the clock edges, and sample CMD/DAT on each of them."""
   traces = {}

   def digital_for(name, label):
      values = capture.columns[name]
      level = threshold if threshold is not None else auto_threshold(values)
      span = max(values) - min(values)
      trace = digitize(values, level, hysteresis * span)
      traces[label] = trace
      return trace

   clk = digital_for(clk_name, "CLK")
   edges = find_edges(clk, rising=(edge == "rising"), min_width=min_width)
   if not edges:
      raise SystemExit("no clock edges on %s - check the channel mapping, "
                       "the threshold and that the capture actually contains "
                       "bus activity" % clk_name)

   cmd = digital_for(cmd_name, "CMD")
   cmd_bits = bytes(cmd[index] for index in edges)

   dat_nibbles = None
   if dat_names:
      dat_digital = [digital_for(name, "DAT%d" % line)
                     for line, name in enumerate(dat_names)]
      dat_nibbles = bytearray(len(edges))
      for line, digital in enumerate(dat_digital):
         mask = 1 << line
         for position, index in enumerate(edges):
            if digital[index]:
               dat_nibbles[position] |= mask
      dat_nibbles = bytes(dat_nibbles)

   times = [capture.start_time + index * capture.sample_interval
            for index in edges]
   return Samples(times, cmd_bits, dat_nibbles, len(dat_names or []),
                  traces, capture.sample_interval)


# ---------------------------------------------------------------------------
# Bit-stream decoding
# ---------------------------------------------------------------------------

def bits_value(buf, start, count):
   """MSB-first integer from count bits of a 0/1 byte buffer."""
   value = 0
   for index in range(start, start + count):
      value = (value << 1) | buf[index]
   return value


class BusState(object):
   """Everything the decoder learns from the traffic as it goes.

   Bus width and block sizes are not visible in a CMD53 by themselves -
   they were set by CMD52 writes to CCCR/FBR earlier in the boot.  Track
   them so a capture that includes bring-up decodes its own data phases
   without the user having to say how wide the bus is.
   """

   def __init__(self, bus_width=None, annotate="auto"):
      self.forced_width = bus_width
      self.bus_width = bus_width or 1
      self.width_source = "default" if not bus_width else "forced"
      self.block_size = {}               # function number -> bytes
      self.block_size_partial = {}
      self.block_length = 512            # SD memory CMD16 block length
      self.backplane_window = None
      self._window_bytes = [None, None, None]
      self.annotate = annotate
      self.rca = None

   def note_cmd52_write(self, function_number, address, data):
      if function_number == 0:
         if address == 0x07 and self.forced_width is None:
            width_bits = data & 0x03
            self.bus_width = 4 if width_bits == 2 else 1
            self.width_source = "CCCR bus-interface-control write"
         elif address in (0x10, 0x11):
            self._note_block_size(0, address - 0x10, data)
         elif (address & 0xFF) in (0x10, 0x11) and 1 <= (address >> 8) <= 7:
            self._note_block_size(address >> 8, (address & 0xFF) - 0x10, data)
      elif function_number == 1:
         if address in (0x1000A, 0x1000B, 0x1000C):
            self._window_bytes[address - 0x1000A] = data
            if all(byte is not None for byte in self._window_bytes):
               window = (self._window_bytes[0]
                         | (self._window_bytes[1] << 8)
                         | (self._window_bytes[2] << 16))
               self.backplane_window = window << 8

   def _note_block_size(self, function_number, half, data):
      low, high = self.block_size_partial.get(function_number, (0, 0))
      if half == 0:
         low = data
      else:
         high = data
      self.block_size_partial[function_number] = (low, high)
      size = low | (high << 8)
      if size:
         self.block_size[function_number] = size

   def function_block_size(self, function_number):
      return self.block_size.get(function_number, 512)


def _response_length(response_type):
   return 136 if response_type == "R2" else 48


def decode_command(raw):
   """Split a 48-bit host command token."""
   index = (raw >> 40) & 0x3F
   argument = (raw >> 8) & 0xFFFFFFFF
   crc = (raw >> 1) & 0x7F
   end_bit = raw & 1
   expected = crc7(bytes([(raw >> 40) & 0xFF, (raw >> 32) & 0xFF,
                          (raw >> 24) & 0xFF, (raw >> 16) & 0xFF,
                          (raw >> 8) & 0xFF]))
   return {
      "index": index, "arg": argument, "crc": crc, "crc_ok": crc == expected,
      "end_ok": end_bit == 1, "raw": raw,
   }


def decode_response(raw, bit_count):
   if bit_count == 136:
      return {"index": None, "payload": (raw >> 1) & ((1 << 127) - 1),
              "crc": None, "crc_ok": None, "end_ok": raw & 1, "raw": raw}
   index = (raw >> 40) & 0x3F
   payload = (raw >> 8) & 0xFFFFFFFF
   crc = (raw >> 1) & 0x7F
   expected = crc7(bytes([(raw >> 40) & 0xFF, (raw >> 32) & 0xFF,
                          (raw >> 24) & 0xFF, (raw >> 16) & 0xFF,
                          (raw >> 8) & 0xFF]))
   return {"index": index, "payload": payload, "crc": crc,
           "crc_ok": crc == expected, "end_ok": raw & 1, "raw": raw}


def describe_command(packet, state, is_acmd):
   """Human text for a command token, plus any data-phase expectation."""
   index = packet["index"]
   argument = packet["arg"]
   name = (ACMD_NAMES if is_acmd else CMD_NAMES).get(index, "CMD%u" % index)
   detail = "arg 0x%08X" % argument
   data_phase = None

   if not is_acmd and index == 52:
      write = bool(argument & 0x80000000)
      function_number = (argument >> 28) & 0x07
      raw_flag = bool(argument & 0x08000000)
      address = (argument >> 9) & 0x1FFFF
      data = argument & 0xFF
      symbol = register_name(function_number, address, state.annotate)
      detail = "%s F%u [0x%05X]%s %s" % (
         "WR" if write else "RD", function_number, address,
         (" " + symbol) if symbol else "",
         ("<= 0x%02X" % data) if write else "")
      if raw_flag:
         detail += " RAW"
      if write:
         bit_names = None
         if function_number == 1 and address == 0x1000E:
            bit_names = decode_bit_names(data, CHIP_CLOCK_CSR_BITS)
         elif function_number == 1 and address == 0x1001F:
            bit_names = decode_bit_names(data, SLEEP_CSR_BITS)
         if bit_names:
            detail += " {%s}" % ",".join(bit_names)
         state.note_cmd52_write(function_number, address, data)

   elif not is_acmd and index == 53:
      write = bool(argument & 0x80000000)
      function_number = (argument >> 28) & 0x07
      block_mode = bool(argument & 0x08000000)
      increment = bool(argument & 0x04000000)
      address = (argument >> 9) & 0x1FFFF
      count = argument & 0x1FF
      if block_mode:
         blocks = count if count else 0
         block_size = state.function_block_size(function_number)
         length = blocks * block_size
         size_text = "%u blk x %u B" % (blocks, block_size)
      else:
         length = count if count else 512
         size_text = "%u B" % length
      symbol = register_name(function_number, address, state.annotate)
      detail = "%s F%u [0x%05X]%s %s %s%s" % (
         "WR" if write else "RD", function_number, address,
         (" " + symbol) if symbol else "", size_text,
         "block" if block_mode else "byte",
         ", inc" if increment else ", fixed")
      if (function_number == 1 and state.backplane_window is not None
            and state.annotate in ("auto", "cyw43")):
         absolute = state.backplane_window + (address & 0x7FFF)
         detail += " -> backplane 0x%08X" % absolute
         if address & 0x8000:
            detail += " (4-byte access)"
      data_phase = {
         "direction": "write" if write else "read",
         "length": length, "function": function_number,
         "blocks": (count or 0) if block_mode else 1,
         "block_size": (state.function_block_size(function_number)
                        if block_mode else length),
      }

   elif not is_acmd and index == 5:
      if argument == 0:
         detail = "inquiry (arg 0)"
      else:
         detail = "OCR 0x%06X%s" % (argument & 0xFFFFFF,
                                    " S18R" if argument & 0x1000000 else "")

   elif not is_acmd and index in (3, 7, 13, 15, 55):
      detail = "RCA 0x%04X" % ((argument >> 16) & 0xFFFF)

   elif not is_acmd and index == 8:
      detail = "VHS 0x%X pattern 0x%02X" % ((argument >> 8) & 0xF,
                                            argument & 0xFF)

   elif not is_acmd and index == 16:
      detail = "block length %u" % argument
      state.block_length = argument

   elif is_acmd and index == 6:
      state.bus_width = 4 if (argument & 3) == 2 else 1
      state.width_source = "ACMD6"
      detail = "%u-bit bus" % state.bus_width

   elif is_acmd and index == 41:
      detail = "OCR 0x%06X%s" % (argument & 0xFFFFFF,
                                 " HCS" if argument & 0x40000000 else "")

   if not is_acmd and index in DATA_COMMANDS and data_phase is None:
      direction, multi = DATA_COMMANDS[index]
      if direction:
         data_phase = {"direction": direction, "length": state.block_length,
                       "function": None, "blocks": 0 if multi else 1,
                       "block_size": state.block_length}
         detail = "addr 0x%08X, %s" % (argument, "multi-block" if multi
                                       else "%u B" % state.block_length)

   return name, detail, data_phase


def describe_response(packet, response_type, state, command_index):
   payload = packet.get("payload", 0)
   if response_type == "R5":
      flags = (payload >> 8) & 0xFF
      data = payload & 0xFF
      names = decode_bit_names(flags, R5_FLAG_BITS)
      state_name = R5_STATE_NAMES[(flags >> 4) & 3]
      text = "flags 0x%02X state=%s data 0x%02X" % (flags, state_name, data)
      if names:
         text += " {%s}" % ",".join(names)
      return text, bool(names)
   if response_type == "R4":
      ready = bool(payload & 0x80000000)
      functions = (payload >> 28) & 0x07
      memory = bool(payload & 0x08000000)
      return ("%s, %u IO function%s%s, OCR 0x%06X"
              % ("ready" if ready else "busy", functions,
                 "" if functions == 1 else "s",
                 ", memory present" if memory else "",
                 payload & 0xFFFFFF)), False
   if response_type == "R6":
      rca = (payload >> 16) & 0xFFFF
      state.rca = rca
      return "RCA 0x%04X status 0x%04X" % (rca, payload & 0xFFFF), False
   if response_type == "R7":
      return ("voltage accepted 0x%X pattern 0x%02X"
              % ((payload >> 8) & 0xF, payload & 0xFF)), False
   if response_type == "R3":
      return ("OCR 0x%08X%s" % (payload,
                                " (ready)" if payload & 0x80000000 else "")), \
             False
   if response_type == "R2":
      return "CID/CSD 0x%032X" % payload, False
   # R1 / R1b
   names = decode_bit_names(payload, CARD_STATUS_BITS)
   card_state = CARD_STATE_NAMES.get((payload >> 9) & 0xF, "?")
   error = bool(payload & 0xFDF98008)
   text = "status 0x%08X state=%s" % (payload, card_state)
   if names:
      text += " {%s}" % ",".join(names)
   return text, error


def find_data_start(samples, cursor, width, limit):
   """Index of the data start bit, or None if the bus stays idle."""
   mask = 0x0F if width == 4 else 0x01
   dat = samples.dat
   end = min(samples.count, cursor + limit)
   for index in range(cursor, end):
      if (dat[index] & mask) == 0:
         return index
   return None


def decode_data_block(samples, start, byte_count, width):
   """Decode one DAT block starting at its start bit."""
   dat = samples.dat
   lines = 4 if width == 4 else 1
   clocks = byte_count * (2 if width == 4 else 8)
   need = start + 1 + clocks + 16 + 1
   if need > samples.count:
      return None, samples.count
   data = bytearray()
   position = start + 1
   if width == 4:
      for _ in range(byte_count):
         high = dat[position] & 0x0F
         low = dat[position + 1] & 0x0F
         data.append((high << 4) | low)
         position += 2
   else:
      for _ in range(byte_count):
         byte = 0
         for _ in range(8):
            byte = (byte << 1) | (dat[position] & 1)
            position += 1
         data.append(byte)

   crc_ok = True
   for line in range(lines):
      bits = [(dat[index] >> line) & 1
              for index in range(start + 1, start + 1 + clocks)]
      expected = crc16_bits(bits)
      seen = 0
      for index in range(position, position + 16):
         seen = (seen << 1) | ((dat[index] >> line) & 1)
      if seen != expected:
         crc_ok = False
   position += 16
   end_ok = all(((dat[position] >> line) & 1) for line in range(lines))
   position += 1
   return {"data": bytes(data), "crc_ok": crc_ok, "end_ok": end_ok}, position


def decode_crc_status(samples, cursor, limit):
   """The card's per-block write acknowledgement on DAT0, plus busy time."""
   dat = samples.dat
   end = min(samples.count, cursor + limit)
   for index in range(cursor, end):
      if (dat[index] & 1) == 0:
         if index + 4 >= samples.count:
            break
         status = ((dat[index + 1] & 1) << 2 | (dat[index + 2] & 1) << 1
                   | (dat[index + 3] & 1))
         busy_end = index + 5
         while busy_end < samples.count and (dat[busy_end] & 1) == 0:
            busy_end += 1
         names = {2: "accepted", 5: "CRC error", 6: "write error"}
         busy_us = (samples.times[min(busy_end, samples.count - 1)]
                    - samples.times[index]) * 1e6
         return {"status": status, "name": names.get(status, "unknown"),
                 "busy_us": busy_us, "index": index}, busy_end
   return None, cursor


# ---------------------------------------------------------------------------
# Transaction level
# ---------------------------------------------------------------------------

def process_data_phase(samples, state, phase, cursor, options, events):
   """Decode the DAT-line blocks belonging to one data command."""
   if samples.dat is None:
      events.append({"t": samples.times[min(cursor, samples.count - 1)],
                     "kind": "note",
                     "text": "data phase: %s %u B (DAT lines not probed)"
                             % (phase["direction"], phase["length"])})
      return cursor

   width = state.bus_width
   if width == 4 and samples.dat_lines < 4:
      events.append({"t": samples.times[min(cursor, samples.count - 1)],
                     "kind": "note",
                     "text": "data phase: bus is 4-bit but only %u DAT line(s) "
                             "probed - not decoded" % samples.dat_lines})
      return cursor

   block_size = phase["block_size"] or phase["length"]
   blocks = phase["blocks"]
   open_ended = blocks == 0
   remaining = options["max_blocks"] if open_ended else blocks
   if not block_size:
      return cursor

   index = 0
   while remaining > 0:
      start = find_data_start(samples, cursor, width, options["data_limit"])
      if start is None:
         if index == 0:
            events.append({"t": samples.times[min(cursor, samples.count - 1)],
                           "kind": "note",
                           "text": "data phase: no start bit within %u clocks"
                                   % options["data_limit"]})
         break
      block, cursor = decode_data_block(samples, start, block_size, width)
      if block is None:
         events.append({"t": samples.times[min(start, samples.count - 1)],
                        "kind": "note",
                        "text": "data phase: capture ends mid-block"})
         break
      event = {
         "t": samples.times[start], "kind": "data",
         "direction": phase["direction"], "function": phase["function"],
         "width": width, "block": index, "bytes": len(block["data"]),
         "crc_ok": block["crc_ok"], "end_ok": bool(block["end_ok"]),
         "data": block["data"],
      }
      events.append(event)
      if phase["direction"] == "write":
         status, cursor = decode_crc_status(samples, cursor,
                                            options["data_limit"])
         if status:
            events.append({"t": samples.times[status["index"]],
                           "kind": "crc_status", "status": status["status"],
                           "name": status["name"],
                           "busy_us": status["busy_us"]})
      index += 1
      remaining -= 1
   return cursor


def decode_bus(samples, state, options):
   """Walk the sampled bus and produce a time-ordered event list."""
   events = []
   cmd = samples.cmd
   count = samples.count
   dat_cursor = 0
   is_acmd = False
   pending = None
   index = 0

   def finish_pending(from_index):
      nonlocal dat_cursor
      if pending and pending.get("data_phase"):
         dat_cursor = process_data_phase(samples, state,
                                         pending["data_phase"],
                                         max(dat_cursor, from_index),
                                         options, events)
         pending["data_phase"] = None

   while index < count - 1:
      if cmd[index]:
         index += 1
         continue

      if cmd[index + 1] == 1:
         # start bit + transmission bit 1 -> host command
         if index + 48 > count:
            break
         packet = decode_command(bits_value(cmd, index, 48))
         if not packet["end_ok"]:
            index += 1                    # not a real token, keep looking
            continue
         finish_pending(index)
         name, detail, data_phase = describe_command(packet, state, is_acmd)
         response_type = (ACMD_RESPONSE_TYPES if is_acmd
                          else RESPONSE_TYPES).get(packet["index"], "R1")
         events.append({
            "t": samples.times[index], "kind": "command",
            "index": packet["index"], "acmd": is_acmd, "name": name,
            "arg": packet["arg"], "detail": detail,
            "crc_ok": packet["crc_ok"], "raw": packet["raw"],
            "response_type": response_type,
         })
         pending = {"index": packet["index"], "acmd": is_acmd,
                    "response_type": response_type, "data_phase": data_phase,
                    "time": samples.times[index]}
         is_acmd = (packet["index"] == 55 and not is_acmd)
         index += 48
         if response_type is None:
            finish_pending(index)
            pending = None
         continue

      # start bit + transmission bit 0 -> card response
      response_type = pending["response_type"] if pending else "R1"
      bit_count = _response_length(response_type)
      if index + bit_count > count:
         break
      packet = decode_response(bits_value(cmd, index, bit_count), bit_count)
      if not packet["end_ok"]:
         index += 1
         continue
      text, flagged = describe_response(packet, response_type, state,
                                        pending["index"] if pending else None)
      latency_us = None
      if pending:
         latency_us = (samples.times[index] - pending["time"]) * 1e6
      events.append({
         "t": samples.times[index], "kind": "response", "type": response_type,
         "index": packet["index"], "detail": text, "crc_ok": packet["crc_ok"],
         "error": flagged, "raw": packet["raw"], "latency_us": latency_us,
      })
      index += bit_count
      finish_pending(index)
      pending = None

   if pending:
      finish_pending(index)
   events.sort(key=lambda event: event["t"])
   return events


# ---------------------------------------------------------------------------
# CYW43 SDPCM / BDC / CDC dissection of the function 2 payloads
# ---------------------------------------------------------------------------

def dissect_sdpcm(data):
   """One-line summary of an SDPCM frame, or None if it is not one."""
   if len(data) < 12:
      return None
   length = data[0] | (data[1] << 8)
   check = data[2] | (data[3] << 8)
   if length == 0 or (length ^ 0xFFFF) != check:
      return None
   sequence = data[4]
   channel = data[5] & 0x0F
   next_length = data[6]
   header_length = data[7]
   flow_control = data[8]
   credit = data[9]
   text = ("SDPCM len %u seq %u chan %s doff %u flow 0x%02X credit %u"
           % (length, sequence,
              SDPCM_CHANNEL_NAMES.get(channel, str(channel)),
              header_length, flow_control, credit))
   if next_length:
      text += " next %u" % (next_length * 16)

   payload = data[header_length:] if header_length <= len(data) else b""
   if channel == 0 and len(payload) >= 16:
      command = int.from_bytes(payload[0:4], "little")
      cdc_length = int.from_bytes(payload[4:8], "little")
      flags = int.from_bytes(payload[8:12], "little")
      status = int.from_bytes(payload[12:16], "little")
      body = payload[16:16 + cdc_length]
      name = body.split(b"\x00", 1)[0]
      printable = name.decode("ascii", "replace") if 0 < len(name) < 40 \
         and all(32 <= byte < 127 for byte in name) else ""
      text += ("\n      CDC cmd %u len %u id %u %s status %u%s"
               % (command, cdc_length, flags >> 16,
                  "SET" if flags & 0x02 else "GET", status,
                  (" iovar '%s'" % printable) if printable else ""))
      if flags & 0x01:
         text += " ERROR"
   elif channel in (1, 2) and len(payload) >= 4:
      bdc_flags = payload[0]
      data_offset = payload[3] * 4
      frame = payload[4 + data_offset:]
      text += "\n      BDC flags 0x%02X prio %u doff %u" % (
         bdc_flags, payload[1] & 0x07, data_offset)
      if len(frame) >= 14:
         ethertype = (frame[12] << 8) | frame[13]
         text += (" | %s -> %s ethertype 0x%04X"
                  % (":".join("%02X" % byte for byte in frame[6:12]),
                     ":".join("%02X" % byte for byte in frame[0:6]),
                     ethertype))
         if ethertype == 0x886C and len(frame) >= 24:
            text += " (BRCM event)"
   return text


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def hexdump(data, limit, indent="        "):
   lines = []
   shown = data if limit <= 0 or len(data) <= limit else data[:limit]
   for offset in range(0, len(shown), 16):
      chunk = shown[offset:offset + 16]
      hex_part = " ".join("%02X" % byte for byte in chunk)
      text = "".join(chr(byte) if 32 <= byte < 127 else "."
                     for byte in chunk)
      lines.append("%s%04X  %-47s  %s" % (indent, offset, hex_part, text))
   if len(shown) < len(data):
      lines.append("%s...   (%u more bytes)" % (indent, len(data) - len(shown)))
   return lines


def format_events(events, options):
   out = []
   previous = None
   for event in events:
      delta = 0.0 if previous is None else (event["t"] - previous) * 1e6
      previous = event["t"]
      stamp = "%14.3f %10.3f " % (event["t"] * 1e6, delta)
      kind = event["kind"]
      if kind == "command":
         label = "%s%u" % ("ACMD" if event["acmd"] else "CMD", event["index"])
         line = "%s ->   %-7s %-20s %s" % (stamp, label, event["name"],
                                           event["detail"])
         if not event["crc_ok"]:
            line += "   [CRC7 BAD]"
         out.append(line)
      elif kind == "response":
         line = "%s <-   %-7s %-20s %s" % (
            stamp, event["type"], "", event["detail"])
         if event["crc_ok"] is False and event["type"] not in ("R3", "R4"):
            line += "   [CRC7 BAD]"
         if event.get("latency_us") is not None and options["timing"]:
            line += "   (+%.3f us)" % event["latency_us"]
         out.append(line)
      elif kind == "data":
         line = ("%s %s  %-7s %-20s %u B, %u-bit, block %u, CRC16 %s"
                 % (stamp, "-->" if event["direction"] == "write" else "<--",
                    "DAT", "F%s" % (event["function"]
                                    if event["function"] is not None else "-"),
                    event["bytes"], event["width"], event["block"],
                    "ok" if event["crc_ok"] else "BAD"))
         if not event["end_ok"]:
            line += " [END BAD]"
         out.append(line)
         if options["sdpcm"] and event["function"] == 2:
            summary = dissect_sdpcm(event["data"])
            if summary:
               out.append("      %s" % summary)
         if options["dump"] != 0:
            out.extend(hexdump(event["data"], options["dump"]))
      elif kind == "crc_status":
         out.append("%s <-   %-7s %-20s %s (0b%s), busy %.1f us"
                    % (stamp, "CRCSTA", "", event["name"],
                       format(event["status"], "03b"), event["busy_us"]))
      else:
         out.append("%s     %s" % (stamp, event["text"]))
   return out


def summarise(events, samples, state):
   commands = [event for event in events if event["kind"] == "command"]
   responses = [event for event in events if event["kind"] == "response"]
   data_events = [event for event in events if event["kind"] == "data"]
   crc_errors = sum(1 for event in commands if not event["crc_ok"])
   crc_errors += sum(1 for event in responses
                     if event["crc_ok"] is False
                     and event["type"] not in ("R3", "R4"))
   data_crc_errors = sum(1 for event in data_events if not event["crc_ok"])
   errors = sum(1 for event in responses if event.get("error"))

   periods = []
   step = max(1, samples.count // 2000)
   for index in range(step, min(samples.count, 2000 * step), step):
      periods.append((samples.times[index] - samples.times[index - step])
                     / step)
   clock = 0.0
   if periods:
      periods.sort()
      middle = periods[len(periods) // 2]
      if middle > 0:
         clock = 1.0 / middle

   histogram = {}
   for event in commands:
      key = "%s%u" % ("ACMD" if event["acmd"] else "CMD", event["index"])
      histogram[key] = histogram.get(key, 0) + 1

   lines = ["", "Summary", "-------"]
   lines.append("  clock edges     : %u" % samples.count)
   if samples.count > 1:
      lines.append("  capture window  : %.3f ms"
                   % ((samples.times[-1] - samples.times[0]) * 1e3))
   lines.append("  bus clock       : %.3f MHz (median over active clocks)"
                % (clock / 1e6))
   lines.append("  bus width       : %u-bit (%s)"
                % (state.bus_width, state.width_source))
   if state.block_size:
      lines.append("  block sizes     : %s"
                   % ", ".join("F%u=%u" % (function, size)
                               for function, size
                               in sorted(state.block_size.items())))
   if state.backplane_window is not None:
      lines.append("  backplane window: 0x%08X" % state.backplane_window)
   lines.append("  commands        : %u (%s)"
                % (len(commands),
                   ", ".join("%s x%u" % (key, value) for key, value
                             in sorted(histogram.items(),
                                       key=lambda item: -item[1]))))
   lines.append("  responses       : %u" % len(responses))
   lines.append("  data blocks     : %u (%u bytes)"
                % (len(data_events),
                   sum(event["bytes"] for event in data_events)))
   lines.append("  CRC7 failures   : %u" % crc_errors)
   lines.append("  CRC16 failures  : %u" % data_crc_errors)
   lines.append("  flagged replies : %u" % errors)
   return lines


# ---------------------------------------------------------------------------
# VCD export (for eyeballing the same capture in PulseView / GTKWave)
# ---------------------------------------------------------------------------

VCD_SYMBOLS = "!\"#$%&'()*+,-./"


def write_vcd(path, samples):
   names = [name for name in ("CLK", "CMD", "DAT0", "DAT1", "DAT2", "DAT3")
            if name in samples.digital]
   if not names:
      raise SystemExit("nothing to write to the VCD")
   traces = [samples.digital[name] for name in names]
   length = min(len(trace) for trace in traces)
   tick = samples.sample_interval / 1e-12
   with open(path, "w") as handle:
      handle.write("$comment generated by tools/sdio_decode.py $end\n")
      handle.write("$timescale 1ps $end\n")
      handle.write("$scope module sdio $end\n")
      for index, name in enumerate(names):
         handle.write("$var wire 1 %s %s $end\n" % (VCD_SYMBOLS[index], name))
      handle.write("$upscope $end\n$enddefinitions $end\n")
      previous = [None] * len(names)
      for sample in range(length):
         changes = []
         for index, trace in enumerate(traces):
            value = trace[sample]
            if value != previous[index]:
               changes.append("%u%s" % (value, VCD_SYMBOLS[index]))
               previous[index] = value
         if changes:
            handle.write("#%u\n%s\n" % (int(sample * tick),
                                        "\n".join(changes)))
      handle.write("#%u\n" % int(length * tick))


# ---------------------------------------------------------------------------
# Waveform synthesis - used by --selftest and --emit-test-csv, so the tool
# can be exercised (and the docs followed) without a scope on the bench.
# ---------------------------------------------------------------------------

class BusBuilder(object):
   """Assemble a clock-by-clock SD bus, then render it as a waveform."""

   def __init__(self):
      self.clocks = []                   # (cmd bit, dat nibble)

   def idle(self, count=8):
      for _ in range(count):
         self.clocks.append((1, 0x0F))

   def _emit_cmd(self, bits):
      for bit in bits:
         self.clocks.append((bit, 0x0F))

   def command(self, index, argument):
      raw = (1 << 46) | (index << 40) | (argument << 8)
      crc = crc7(bytes([(raw >> 40) & 0xFF, (raw >> 32) & 0xFF,
                        (raw >> 24) & 0xFF, (raw >> 16) & 0xFF,
                        (raw >> 8) & 0xFF]))
      raw |= (crc << 1) | 1
      self._emit_cmd([(raw >> shift) & 1 for shift in range(47, -1, -1)])
      self.idle(2)

   def response(self, index, payload, valid_crc=True):
      raw = (index << 40) | ((payload & 0xFFFFFFFF) << 8)
      crc = crc7(bytes([(raw >> 40) & 0xFF, (raw >> 32) & 0xFF,
                        (raw >> 24) & 0xFF, (raw >> 16) & 0xFF,
                        (raw >> 8) & 0xFF]))
      if not valid_crc:
         crc ^= 0x55
      raw |= (crc << 1) | 1
      self._emit_cmd([(raw >> shift) & 1 for shift in range(47, -1, -1)])
      self.idle(2)

   def data_block(self, payload, width=4):
      """Append one data block (start bit, payload, per-line CRC16, end)."""
      lines = 4 if width == 4 else 1
      nibbles = [0x00]                    # start bit: all data lines low
      if width == 4:
         for byte in payload:
            nibbles.append((byte >> 4) & 0x0F)
            nibbles.append(byte & 0x0F)
      else:
         for byte in payload:
            for shift in range(7, -1, -1):
               nibbles.append(((byte >> shift) & 1) | 0x0E)
      body = nibbles[1:]
      crc_nibbles = [0] * 16
      for line in range(lines):
         bits = [(nibble >> line) & 1 for nibble in body]
         crc = crc16_bits(bits)
         for position in range(16):
            if (crc >> (15 - position)) & 1:
               crc_nibbles[position] |= 1 << line
      if width == 1:
         crc_nibbles = [value | 0x0E for value in crc_nibbles]
      nibbles.extend(crc_nibbles)
      nibbles.append(0x0F)                # end bit
      for nibble in nibbles:
         self.clocks.append((1, nibble))

   def crc_status(self, status=0b010, busy_clocks=6):
      """Card's write acknowledgement: start, 3 status bits, end, busy."""
      self.clocks.append((1, 0x0E))       # start bit on DAT0
      for shift in range(2, -1, -1):
         self.clocks.append((1, 0x0E | ((status >> shift) & 1)))
      self.clocks.append((1, 0x0F))       # end bit
      for _ in range(busy_clocks):
         self.clocks.append((1, 0x0E))    # DAT0 held low = busy
      self.clocks.append((1, 0x0F))

   def to_capture(self, sample_rate=200e6, clock_hz=25e6, high=3.3):
      """Render to an analog-looking Capture the normal path can chew on."""
      half = max(1, int(round(sample_rate / clock_hz / 2)))
      clk, cmd, dat = [], [], [[], [], [], []]
      for cmd_bit, nibble in self.clocks:
         for level in (0.0, high):
            for _ in range(half):
               clk.append(level)
               cmd.append(high if cmd_bit else 0.0)
               for line in range(4):
                  dat[line].append(high if (nibble >> line) & 1 else 0.0)
      columns = {"CLK": clk, "CMD": cmd}
      for line in range(4):
         columns["DAT%d" % line] = dat[line]
      return Capture(columns, 1.0 / (2 * half * clock_hz), 0.0)


def write_capture_csv(path, capture):
   names = list(capture.columns)
   with open(path, "w") as handle:
      handle.write("Second,%s\n" % ",".join(names))
      for index in range(capture.length):
         handle.write("%.10e,%s\n"
                      % (capture.start_time + index * capture.sample_interval,
                         ",".join("%.3f" % capture.columns[name][index]
                                  for name in names)))


def build_demo_bus():
   """A short but representative CYW43 bring-up + frame read."""
   bus = BusBuilder()
   bus.idle(8)
   # CMD52 write CCCR bus-interface-control = 4-bit
   bus.command(52, 0x80000E02)
   bus.response(52, 0x00001002)
   # CMD52 write F2 block size = 64 (low then high byte)
   bus.command(52, 0x80042040)
   bus.response(52, 0x00001040)
   bus.command(52, 0x80042200)
   bus.response(52, 0x00001000)
   # CMD52 write F1 SDIO_CHIP_CLOCK_CSR = ALP_AVAIL_REQ
   bus.command(52, 0x92001C08)
   bus.response(52, 0x00001008)
   # CMD52 write backplane window low/mid/high for 0x18002000
   bus.command(52, 0x92001420)
   bus.response(52, 0x00001020)
   bus.command(52, 0x92001600)
   bus.response(52, 0x00001000)
   bus.command(52, 0x92001818)
   bus.response(52, 0x00001018)
   # CMD53 read F2, block mode, 1 x 64 bytes: an SDPCM control frame
   payload = bytearray(64)
   frame_length = 44
   payload[0] = frame_length & 0xFF
   payload[1] = (frame_length >> 8) & 0xFF
   payload[2] = (~frame_length) & 0xFF
   payload[3] = ((~frame_length) >> 8) & 0xFF
   payload[4] = 0x05                      # sequence
   payload[5] = 0x00                      # channel 0 = control
   payload[6] = 0x00
   payload[7] = 12                        # data offset
   payload[8] = 0x00
   payload[9] = 0x04                      # credit
   cdc = bytearray(16)
   cdc[0] = 262 & 0xFF                    # WLC_GET_VAR
   cdc[1] = (262 >> 8) & 0xFF
   cdc[4] = 12                            # length
   cdc[10] = 0x01                         # id 1 in the high half of flags
   payload[12:28] = cdc
   payload[28:40] = b"cur_etheraddr"[:12]
   bus.command(53, 0x29000001)
   bus.response(53, 0x00001000)
   bus.data_block(bytes(payload), width=4)
   bus.idle(4)
   # CMD53 write F2, byte mode, 16 bytes, then the CRC status token
   bus.command(53, 0xA1000010)
   bus.response(53, 0x00001000)
   bus.data_block(bytes(range(16)), width=4)
   bus.crc_status()
   bus.idle(8)
   return bus


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def selftest():
   """Synthesise a bus, push it through the whole pipeline, check the decode."""
   failures = []

   def check(condition, message):
      if not condition:
         failures.append(message)

   check(crc7(bytes([0x40, 0, 0, 0, 0])) == 0x4A, "CRC7 CMD0 vector")
   check(crc7(bytes([0x48, 0, 0, 1, 0xAA])) == 0x43, "CRC7 CMD8 vector")
   bits = []
   for byte in b"123456789":
      bits.extend((byte >> shift) & 1 for shift in range(7, -1, -1))
   check(crc16_bits(bits) == 0x31C3, "CRC16 check vector")

   capture = build_demo_bus().to_capture()
   samples = sample_bus(capture, "CLK", "CMD",
                        ["DAT0", "DAT1", "DAT2", "DAT3"])
   state = BusState(annotate="cyw43")
   options = {"max_blocks": 64, "data_limit": 20000, "dump": 0,
              "sdpcm": True, "timing": True}
   events = decode_bus(samples, state, options)

   commands = [event for event in events if event["kind"] == "command"]
   responses = [event for event in events if event["kind"] == "response"]
   data_events = [event for event in events if event["kind"] == "data"]
   status_events = [event for event in events if event["kind"] == "crc_status"]

   check(len(commands) == 9, "expected 9 commands, got %u" % len(commands))
   check(all(event["crc_ok"] for event in commands), "command CRC7 mismatch")
   check(len(responses) == 9, "expected 9 responses, got %u" % len(responses))
   check(all(event["crc_ok"] for event in responses), "response CRC7 mismatch")
   check(state.bus_width == 4, "bus width not learned from the CCCR write")
   check(state.block_size.get(2) == 64,
         "F2 block size not learned (got %s)" % state.block_size.get(2))
   check(state.backplane_window == 0x18002000,
         "backplane window not tracked (got %s)"
         % (state.backplane_window and hex(state.backplane_window)))
   check(len(data_events) == 2, "expected 2 data blocks, got %u"
         % len(data_events))
   check(all(event["crc_ok"] for event in data_events), "data CRC16 mismatch")
   check(all(event["end_ok"] for event in data_events), "data end bit mismatch")
   if len(data_events) == 2:
      check(data_events[0]["bytes"] == 64, "read block length")
      check(data_events[1]["data"] == bytes(range(16)), "write block payload")
      check(dissect_sdpcm(data_events[0]["data"]) is not None,
            "SDPCM header not recognised")
   check(len(status_events) == 1 and status_events[0]["status"] == 0b010,
         "write CRC status token not decoded")

   # The same bus, 1-bit, to cover the other data path.
   narrow = BusBuilder()
   narrow.idle(4)
   narrow.command(53, 0x34001010)         # CMD53 read F1, byte mode, 16 B
   narrow.response(53, 0x00001000)
   narrow.data_block(bytes(range(0x10, 0x20)), width=1)
   narrow.idle(4)
   narrow_samples = sample_bus(narrow.to_capture(), "CLK", "CMD",
                               ["DAT0", "DAT1", "DAT2", "DAT3"])
   narrow_state = BusState(bus_width=1, annotate="cyw43")
   narrow_events = decode_bus(narrow_samples, narrow_state, options)
   narrow_data = [event for event in narrow_events if event["kind"] == "data"]
   check(len(narrow_data) == 1 and narrow_data[0]["crc_ok"]
         and narrow_data[0]["data"] == bytes(range(0x10, 0x20)),
         "1-bit data block decode")

   # CSV round trip, including the header sniffing.
   import tempfile
   import os
   handle, path = tempfile.mkstemp(suffix=".csv")
   os.close(handle)
   try:
      write_capture_csv(path, capture)
      reloaded = load_csv(path)
      check(abs(reloaded.sample_interval - capture.sample_interval)
            < capture.sample_interval * 1e-6, "CSV sample interval round trip")
      reloaded_samples = sample_bus(reloaded, reloaded.resolve("CLK"),
                                    reloaded.resolve("CMD"),
                                    [reloaded.resolve("DAT%d" % line)
                                     for line in range(4)])
      reloaded_events = decode_bus(reloaded_samples, BusState(), options)
      check(len([event for event in reloaded_events
                 if event["kind"] == "command"]) == 9,
            "CSV round trip lost commands")
   finally:
      os.unlink(path)

   # A deliberately corrupted response must be reported, not hidden.
   broken = BusBuilder()
   broken.idle(4)
   broken.command(52, 0x00001C00)
   broken.response(52, 0x00001000, valid_crc=False)
   broken.idle(4)
   broken_events = decode_bus(sample_bus(broken.to_capture(), "CLK", "CMD", []),
                              BusState(), options)
   broken_responses = [event for event in broken_events
                       if event["kind"] == "response"]
   check(len(broken_responses) == 1 and broken_responses[0]["crc_ok"] is False,
         "bad response CRC7 not flagged")

   for failure in failures:
      print("FAIL: %s" % failure)
   print("selftest: %s (%u checks failed)"
         % ("PASS" if not failures else "FAIL", len(failures)))
   return 1 if failures else 0


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def guess_channel(capture, *keywords):
   for name in capture.columns:
      lowered = name.lower()
      for keyword in keywords:
         if keyword in lowered:
            return name
   return None


def main(argv=None):
   parser = argparse.ArgumentParser(
      description="Decode SD / SDIO traffic from an oscilloscope CSV export "
                  "(this runs on a PC - the scope itself cannot be taught "
                  "new protocol decoders).",
      formatter_class=argparse.RawDescriptionHelpFormatter,
      epilog="See docs/dev/sdio-protocol-decode.md for the capture setup.")
   parser.add_argument("capture", nargs="?", help="scope CSV export")
   parser.add_argument("--clk", help="clock channel (e.g. C1, D0)")
   parser.add_argument("--cmd", help="CMD channel (e.g. C2, D1)")
   parser.add_argument("--dat", help="comma separated DAT0..DAT3 channels")
   parser.add_argument("--bus-width", type=int, choices=(1, 4),
                       help="force the data bus width instead of learning it "
                            "from the CCCR/ACMD6 writes in the capture")
   parser.add_argument("--threshold", type=float,
                       help="logic threshold in volts (default: midpoint of "
                            "each channel's range)")
   parser.add_argument("--hysteresis", type=float, default=0.0,
                       help="hysteresis as a fraction of the channel span "
                            "(e.g. 0.1) for noisy analog captures")
   parser.add_argument("--edge", choices=("rising", "falling"),
                       default="rising",
                       help="clock edge the card samples on (default rising)")
   parser.add_argument("--min-width", type=int, default=0,
                       help="ignore clock edges closer than N samples "
                            "(glitch filter)")
   parser.add_argument("--sample-rate", type=float,
                       help="sample rate in Sa/s when the export has no time "
                            "column")
   parser.add_argument("--time-col", help="name of the time column")
   parser.add_argument("--max-samples", type=int,
                       help="stop reading the CSV after N samples")
   parser.add_argument("--annotate", choices=("auto", "cyw43", "none"),
                       default="auto",
                       help="register-name annotation (default auto = CYW43)")
   parser.add_argument("--no-sdpcm", action="store_true",
                       help="do not dissect SDPCM/BDC/CDC in F2 payloads")
   parser.add_argument("--dump", type=int, default=0, metavar="N",
                       help="hexdump the first N bytes of each data block "
                            "(0 = none, -1 = all)")
   parser.add_argument("--max-blocks", type=int, default=64,
                       help="block cap for open-ended multi-block transfers")
   parser.add_argument("--data-limit", type=int, default=20000, metavar="N",
                       help="clocks to search for a data start bit")
   parser.add_argument("--no-timing", action="store_true",
                       help="omit the command-to-response latency column")
   parser.add_argument("--no-summary", action="store_true")
   parser.add_argument("--json", metavar="PATH", help="write events as JSON")
   parser.add_argument("--vcd", metavar="PATH",
                       help="write the digitised lines as VCD for PulseView")
   parser.add_argument("--list-channels", action="store_true",
                       help="print the channel names in the capture and exit")
   parser.add_argument("--emit-test-csv", metavar="PATH",
                       help="write a synthetic SDIO capture in scope-CSV form")
   parser.add_argument("--selftest", action="store_true",
                       help="decode a synthesised bus and check the result")
   args = parser.parse_args(argv)

   if args.selftest:
      return selftest()

   if args.emit_test_csv:
      write_capture_csv(args.emit_test_csv, build_demo_bus().to_capture())
      print("wrote %s" % args.emit_test_csv)
      return 0

   if not args.capture:
      parser.error("a capture file is required (or use --selftest)")

   capture = load_csv(args.capture, sample_rate=args.sample_rate,
                      time_column=args.time_col, max_samples=args.max_samples)

   if args.list_channels:
      print("%u samples, %.4f ns interval, channels:"
            % (capture.length, capture.sample_interval * 1e9))
      for name in capture.columns:
         values = capture.columns[name]
         print("  %-16s min %8.3f  max %8.3f" % (name, min(values),
                                                 max(values)))
      return 0

   clk_name = (capture.resolve(args.clk) if args.clk
               else guess_channel(capture, "clk", "clock", "sck"))
   cmd_name = (capture.resolve(args.cmd) if args.cmd
               else guess_channel(capture, "cmd"))
   if clk_name is None or cmd_name is None:
      raise SystemExit("could not work out the CLK/CMD channels - pass --clk "
                       "and --cmd (channels: %s)" % ", ".join(capture.columns))
   dat_names = []
   if args.dat:
      dat_names = [capture.resolve(part) for part in args.dat.split(",")
                   if part.strip()]
   else:
      for line in range(4):
         guess = guess_channel(capture, "dat%d" % line, "d%d_dat" % line)
         if guess is None:
            break
         dat_names.append(guess)

   samples = sample_bus(capture, clk_name, cmd_name, dat_names,
                        threshold=args.threshold, hysteresis=args.hysteresis,
                        edge=args.edge, min_width=args.min_width)
   state = BusState(bus_width=args.bus_width, annotate=args.annotate)
   options = {"max_blocks": args.max_blocks, "data_limit": args.data_limit,
              "dump": args.dump, "sdpcm": not args.no_sdpcm,
              "timing": not args.no_timing}
   events = decode_bus(samples, state, options)

   print("# %s: CLK=%s CMD=%s%s, %u samples at %.4f ns"
         % (args.capture, clk_name, cmd_name,
            (" DAT=" + ",".join(dat_names)) if dat_names else "",
            capture.length, capture.sample_interval * 1e9))
   print("%14s %10s  dir   %-7s %-20s %s"
         % ("time(us)", "delta(us)", "packet", "name", "detail"))
   for line in format_events(events, options):
      print(line)
   if not args.no_summary:
      for line in summarise(events, samples, state):
         print(line)

   if args.json:
      serialisable = []
      for event in events:
         copy = dict(event)
         if "data" in copy:
            copy["data"] = copy["data"].hex()
         serialisable.append(copy)
      with open(args.json, "w") as handle:
         json.dump(serialisable, handle, indent=1)
   if args.vcd:
      write_vcd(args.vcd, samples)
   return 0


if __name__ == "__main__":
   sys.exit(main())
