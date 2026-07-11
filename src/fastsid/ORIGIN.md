# FastSID origin

Vendored from VICE 3.1 (`tags/v3.1/vice/src/sid/`):

- `fastsid.c`, `fastsid.h`
- `wave6581.h`, `wave8580.h`
- `sid-snapshot.h`
- `COPYING` (GPLv2)

Source: http://svn.code.sf.net/p/vice-emu/code/tags/v3.1/vice/src/sid/

Firmware builds use local shims (`vice.h`, `types.h`, `sound.h`, …) instead of the full VICE tree.
`beebsid_sid.c` wraps `fastsid_hooks`.
