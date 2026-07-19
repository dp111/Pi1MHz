# newlib-str — strcase* recompiled to avoid the ctype table

Vendored, unmodified, from newlib `newlib/libc/string/`.

    upstream: newlib-4.6.0.20260123
    licence:  see each file's own header (newlib / BSD-family)

    strcasecmp.c  strncasecmp.c  strcasestr.c  str-two-way.h

`str-two-way.h` is not used directly; `strcasestr.c` includes it.

## Why

libc's prebuilt copies of these three reference the 257-byte `_ctype_` table
directly, so the force-included `../ascii_ctype.h` cannot reach them. Compiling
newlib's own sources here — where the force-include applies — replaces those
table lookups with 3-instruction range checks. Verified at the object level:
zero `_ctype_` references.

## Built -Os, deliberately

CMakeLists.txt applies `-Os` to these three via `set_source_files_properties`.
At `-O2` newlib's two-way `strcasestr` unrolls into roughly 2.5KB:

| build | rpi text | rpi3 text |
| --- | --- | --- |
| without these files | 358008 | 353216 |
| at -O2 | 360256 (+2248) | 356104 (+2888) |
| **at -Os** | **358080 (+72)** | **353384 (+168)** |

There are only ~12 call sites tree-wide, all in config and filename handling —
cold paths. We recompile them to drop the table lookups, *not* for speed, so
size is the right thing to optimise. Do not remove the `-Os` override.

## Not here: strtol

`_strtol_l` is the sole remaining `_ctype_` referrer. Recompiling newlib's
`strtol.c` here does **not** remove it — the object still lists `_ctype_` as
undefined, so strtol does not reach the table through the `<ctype.h>` macros.
It also drags in internal newlib headers (`<reent.h>`,
`"../locale/setlocale.h"`). See `../armstring-pi/NOTES.md` for why replacing it
is not worth the risk.

## Updating

Re-copy the same four files from a newer newlib. Do not edit them locally.
