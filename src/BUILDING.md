# Building Pi1MHz

Builds are out-of-source: each configuration lives in its own directory under
`src/build/<preset>`, driven by CMake presets (`CMakePresets.json`). Requires
CMake ≥ 3.21.

## Platforms

| Preset        | Target              | Kernel image  |
|---------------|---------------------|---------------|
| `rpi`         | Pi 1 / Zero         | `kernel.img`  |
| `rpi3`        | Pi 2 / 3            | `kernel7.img` |
| `rpi-debug`   | Pi 1 / Zero, debug  | `kernel.img`  |
| `rpi3-debug`  | Pi 2 / 3, debug     | `kernel7.img` |

Kernel images are written to the top-level `firmware/` directory.

## In VS Code

With the CMake Tools extension and the `src/` folder open, the presets are
detected automatically:

1. Pick a configure preset from the status bar (e.g. `rpi3`).
2. Press the **Build** button (or F7).

Switching preset and building again just works — each config keeps its own
`build/<preset>` directory.

## On the command line — one command

```sh
./scripts/build.sh              # rpi3, release  (defaults)
./scripts/build.sh rpi          # rpi,  release
./scripts/build.sh rpi3 debug   # rpi3, debug
```

It configures the first time and does a fast incremental build every time after.

Or drive the presets directly:

```sh
cmake --preset rpi3             # configure once
cmake --build --preset rpi3 -j  # build (repeat after edits)
```

## Starting clean

```sh
rm -rf build/rpi3        # one config
rm -rf build/            # everything
```

## Full release (all platforms + debug builds)

```sh
./scripts/release.sh
```

From-scratch build of every platform/debug combination, copied into
`releases/<name>/` with a README and zipped.

## Version stamping

`RELEASENAME`, `GITVERSION` and `BUILD_DATE` are generated from git into
`scripts/gitversion.h` at build time. The release number is the nearest git tag
— bump it by tagging:

```sh
git tag -a v1.29 -m "Release v1.29"
git push origin v1.29
```

## Prerequisites

- `arm-none-eabi-gcc` cross toolchain on your `PATH`
- CMake ≥ 3.21 and `make`
