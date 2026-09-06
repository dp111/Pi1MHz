# uzlib — vendored, unmodified

Tiny DEFLATE/gzip/zlib decompressor, used by `src/uef_stream.c` to serve a
compressed UEF tape image to the Beeb without ever holding the decompressed
file in RAM.

* Upstream: <https://github.com/pfalcon/uzlib>
* Vendored at commit `6d60d651a4499a64f2e5b21b4cc08d98cb84b5c1` (2026-09-06)
* Licence: zlib (see `LICENSE`) — permissive, compatible with Pi1MHz's GPLv3
* Origin: derived from Joergen Ibsen's tinf, extended by Paul Sokolovsky

## Why this one

The UEF stream needs a **resumable** inflater: the Beeb reads a tape through
one JIM window at a time, so decompression has to stop when the window is
full and carry on from exactly there. uzlib is the only one of the obvious
candidates built for that — `struct uzlib_uncomp` takes a caller-supplied
`dict_ring`/`dict_size` (the 32 KB DEFLATE history) and a `source_read_cb`
that pulls compressed input on demand, so neither side of the transform is
ever fully resident.

zlib's `inflate()` would also work but drags in allocation hooks for no
benefit here; tinf and zlib's `puff.c` are one-shot by construction and
cannot stop mid-stream, which is what ruled `puff.c` out — it was what the
original PR #20 used, and why that version needed two 16 MB buffers.

## What we compile

Only the decompression half is in `CMakeLists.txt`:

    tinflate.c   the resumable inflater
    tinfgzip.c   gzip header/trailer handling
    crc32.c      gzip CRC32, also used to verify the whole stream
    adler32.c    referenced by tinflate.c's checksum path

`defl_static.c` and `genlz77.c` (the *compressor*) and `tinfzlib.c` are
present so the tree matches upstream and future diffs stay clean, but are
not built — we never compress, and UEFs are never raw-zlib.

## Local changes

**None.** Keep it that way: fix things in `uef_stream.c` instead, so pulling
a newer uzlib stays a straight file copy. The build silences upstream's
warnings per-file in `src/CMakeLists.txt` via `set_source_files_properties`,
the same way `fastsid/fastsid.c` is handled, rather than editing the source.
