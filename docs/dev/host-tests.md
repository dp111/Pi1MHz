# Host-side test suites and CI

STATUS 2026-10-06: until now CI ran two of the fourteen host suites
(wifirom and helpers, from `beeb-roms.yml`); the other twelve were run by
hand, and the services suite stopped linking at 368e244 (SD eject) without
anyone seeing it.  `.github/workflows/host-tests.yml` now runs every suite on
each push to master and each pull request against master, through `src/tests/run_all.sh`:

    sh src/tests/run_all.sh                # all of them
    sh src/tests/run_all.sh services net   # just these

- It takes each `src/tests/<name>/run_tests.sh` (`run.sh` for config and
  teletext, which predate the name), runs it with `sh -e` (so a script that forgets `set -e` still fails on its first
  failed step), carries on after a
  failure so one run lists every broken suite, and exits non-zero if any
  failed.  A new suite directory is picked up without editing the workflow.
- Needs gcc (ASan/UBSan), python3, awk and gzip.  Only fujinet needs a
  submodule, `src/fujinet/cJSON`; the workflow initialises that one and not
  tinyusb, lwIP or mbedTLS.
- wifirom is skipped by default (`SKIP`, default `wifirom`): it needs beebasm
  and py65 and fails under `CI` without beebasm.  `beeb-roms.yml` runs it,
  with the image checks that need the same tools.
- aun's interop layer skips itself (exit 0) when no econet-hpbridge is
  installed, so in CI aun is the engine unit tests, the fuzzers and the
  lockstep ROM checks only.
- The services suite captures the callbacks the services hand to
  `filesystemRegisterEject` and calls them: fat_service's (closes the open
  files and directories, drops the locks and handle gates) and
  fujibus_service's (calls `fn_disk_drop_sd`).
- Nothing here runs the ARM build.  CI does not compile the firmware, so a
  host suite passing says nothing about the kernel linking.
