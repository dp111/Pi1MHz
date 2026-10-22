# Our lwIP fixes

The lwIP submodule (`src/wifi/lwip`) is a plain upstream pin. Our fixes to
it live here as a `git format-patch` series. They are the same files we send
upstream, and the build applies them in order (`NNNN-*.patch`).

`src/CMakeLists.txt` reads the series and copies each lwIP file it changes
into the build tree (`lwip-patched-tree/`). It applies the `src/` hunks there
with `git apply` (`apply.cmake`), then compiles the patched copies in place
of the upstream files. The `test/` hunks are lwIP unit tests. They go
upstream with the fix, and the firmware build skips them.

`git apply` has no fuzz. If a patch stops applying, the build stops and
names the patch.

## When upstream takes a fix

Bump the submodule, then delete that patch file. If upstream changed the code
around a fix it has not taken, rebase the patch (see below) and replace the
file.

## Adding or rebasing a fix

1. Clone lwIP at the submodule's commit.
2. `git am` the series, then commit the new fix with a unit test that fails
   without it.
3. Run lwIP's own suite (`contrib/ports/unix/check`, needs libcheck). The new
   test must fail without the fix, and the whole suite must pass with it.
4. Export with `git format-patch -o src/wifi/lwip-patches/ <base>..` and keep
   the numbering in order.

The suite needs two local Makefile changes to build with GCC 16 on Arch:
`-Wno-error=cast-qual`, and no `-lsubunit`. Neither belongs in a patch.
