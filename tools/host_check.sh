#!/bin/bash
#
# frank-ecat — run the upstream eCat3 tests of the machines this port ships
# against the console build with the ECAT_EMBEDDED patches.
#
# Usage: tools/host_check.sh [build dir]   (default: build-host-emb)
set -u
cd "$(dirname "$0")/../ecat3"
EXE=${1:-../build-host-emb}/eCat3-headless
MACHINES="rk86 mikrosha apogey spektr orion irisha dialog kaspiy unior argo agat pravetz dvk"
fail=0
for m in $MACHINES; do
    out=$(python3 tests/run_tests.py -j 8 --exe "$EXE" -k "$m" 2>&1)
    echo "$out" | grep -E "^\[[0-9]+/[0-9]+\]" | grep -v " ок " 
    echo "$m: $(echo "$out" | tail -1)"
    echo "$out" | tail -1 | grep -q "Пройдено \([0-9]*\) из \1 " || fail=1
done
exit $fail
