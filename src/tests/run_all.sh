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
# defaults to wifirom: that suite needs beebasm and py65 and refuses to run
# without beebasm under CI, so beeb-roms.yml runs it where they are installed.
#
# fujinet needs the cJSON submodule (git submodule update --init
# src/fujinet/cJSON); no other suite reads a submodule.
HERE=$(cd "$(dirname "$0")" && pwd)
SKIP=${SKIP-wifirom}

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
   if sh -e "$script"; then pass="$pass $s"; else fail="$fail $s"; fi
   [ -n "$GITHUB_ACTIONS" ] && echo "::endgroup::"
done

echo
echo "passed:${pass:- none}"
echo "failed:${fail:- none}"
[ -z "$fail" ]
