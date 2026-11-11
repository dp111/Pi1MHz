#!/bin/sh
# Run every host test suite under src/tests/ and fail if any of them does.
# One suite per directory: its run_tests.sh, or run.sh where the directory
# predates that name (config, teletext).  This is what the host-tests CI
# workflow runs, so a suite added here is in CI without editing the workflow.
#
#   sh src/tests/run_all.sh              every suite except the skipped ones
#   sh src/tests/run_all.sh services net just those
#
# SKIP is a space-separated list of suites left out of the default run.  It
# defaults to wifirom and telnetdrom: those suites need beebasm and py65 and
# refuse to run without beebasm under CI, so beeb-roms.yml runs them where they
# are installed.
#
# fujinet needs the cJSON submodule (git submodule update --init
# src/fujinet/cJSON), mtpsession the TinyUSB one (src/usb/tinyusb, for its
# MTP class headers); no other suite reads a submodule.
HERE=$(cd "$(dirname "$0")" && pwd)
SKIP=${SKIP-wifirom telnetdrom}

if [ $# -gt 0 ]; then
   suites=$*
else
   suites=
   for d in "$HERE"/*/; do
      s=$(basename "$d")
      case " $SKIP " in *" $s "*) continue ;; esac
      suites="$suites $s"
   done
fi

pass=
fail=
skipped=      # suites left out of the default run, with the reason
partial=      # suites that ran but printed SKIP/SKIPPED (a tool was missing)
out=$(mktemp)
trap 'rm -f "$out"' EXIT
for d in "$HERE"/*/; do
   s=$(basename "$d")
   [ $# -eq 0 ] || continue
   case " $SKIP " in *" $s "*) skipped="$skipped
   $s (opt-in: in SKIP, run by beeb-roms.yml, needs beebasm and py65)" ;; esac
done
for s in $suites; do
   if   [ -f "$HERE/$s/run_tests.sh" ]; then script=$HERE/$s/run_tests.sh
   elif [ -f "$HERE/$s/run.sh" ];       then script=$HERE/$s/run.sh
   else echo "no run_tests.sh or run.sh in $HERE/$s" >&2; fail="$fail $s"; continue
   fi
   [ -n "$GITHUB_ACTIONS" ] && echo "::group::$s"
   echo "=== $s ==="
   # "sh -e" on purpose: the runner, not each script, guarantees that a
   # failed step stops the suite instead of reaching its banner.  Every
   # script is POSIX sh.
   # output goes through a file, not a pipe, so sh -e's exit status is kept
   if sh -e "$script" > "$out" 2>&1; then pass="$pass $s"; else fail="$fail $s"; fi
   cat "$out"
   # suites that skip a check say so in their output and still exit 0
   if grep -q -E "SKIP|skipped the" "$out"; then
      partial="$partial
   $s: $(grep -E -m1 "SKIP|skipped the" "$out")"
   fi
   [ -n "$GITHUB_ACTIONS" ] && echo "::endgroup::"
done

echo
echo "passed:${pass:- none}"
echo "failed:${fail:- none}"
echo "not run (opt-in):${skipped:- none}"
echo "ran but skipped checks (tool missing):${partial:- none}"
[ -z "$skipped$partial" ] || echo "NOTE: 'passed' does not cover the skipped items above"
[ -z "$fail" ]
