# FastSID origin

Vendored from VICE 3.1 (`tags/v3.1/vice/src/sid/`):

- `fastsid.c`, `fastsid.h`
- `wave6581.h`, `wave8580.h`
- `sid-snapshot.h`
- `COPYING` (GPLv2)

Source: http://svn.code.sf.net/p/vice-emu/code/tags/v3.1/vice/src/sid/

Local, not from VICE:

- `fixpoint.h` - a cut-down copy of VICE's `fixpoint.h`: the float path only
  (no `FIXPOINT_ARITHMETIC`), with `REAL_VALUE` forced to `float`.

Firmware builds use local shims (`vice.h`, `types.h`, `sound.h`, …) instead of the full VICE tree.

## Local changes to the vendored files

Each is marked with a `Pi1MHz:` comment and compiled out with a named `#if`,
not deleted, so an upstream pull still diffs cleanly (8333603):

- `fastsid.c`: `FASTSID_COMBINED_WAVEFORMS 0` - the 6581 $60/$70 combined
  waveform tables (all-zero here) are not built; waveforms 6 and 7 route to
  the silent `wavetable00`. Output is byte-identical.
- `fastsid.c`: `FASTSID_DUMP_STATE 0` - VICE's textual state dump is compiled
  out; `hooks.dump_state` is `NULL`.
- `wave8580.h`: the combined-waveform 8580 tables sit under the same
  `FASTSID_COMBINED_WAVEFORMS` switch.
`beebsid_sid.c` wraps `fastsid_hooks`.
