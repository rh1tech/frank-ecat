#!/bin/bash
#
# frank-ecat — run the upstream eCat3 tests of the machines this port ships
# against the console build with the ECAT_EMBEDDED patches.
#
# Usage: tools/host_check.sh [build dir]   (default: build-host-emb)
set -u
cd "$(dirname "$0")/../ecat3"
EXE=${1:-../build-host-emb}/eCat3-headless
MACHINES="rk86 mikrosha apogey spektr orion irisha dialog kaspiy unior argo agat pravetz dvk bk uknc e85 poisk"
fail=0
# Known, explained differences of the embedded profile (ecat3/PATCHES.md):
# work-dvk3-panel-halt logs the PC inside an INC/BR loop, whose phase moves by
# a few cycles because the DL11 is not brought up to date for a status poll;
# work-rk86-vg75 counts 42111 turns of a 31-cycle loop in a second, not 42110:
# the ВГ75's DMA cycles are clocked within a batch, their stolen cycles the same
# disk-pravetz-8a-init counts 15 bits more written by INIT (of 2.35 million,
# the disk the same): a typed key reaches the program a batch later
# work-agat-9-yazs-regs logs the ЯЗС's envelope and drum counters one step
# apart: the keyboard port polled without bringing the devices up to date
KNOWN="work-dvk3-panel-halt work-rk86-vg75 disk-pravetz-8a-init work-agat-9-yazs-regs"
for m in $MACHINES; do
    out=$(python3 tests/run_tests.py -j 8 --exe "$EXE" -k "$m" 2>&1)
    failed=$(echo "$out" | grep -E "^\[[0-9]+/[0-9]+\]" | grep -v " ок " | awk '{print $2}')
    echo "$out" | grep -E "^\[[0-9]+/[0-9]+\]" | grep -v " ок "
    echo "$m: $(echo "$out" | tail -1)"
    for t in $failed; do
        case " $KNOWN " in
            *" $t "*) echo "  ($t: known difference)" ;;
            *) fail=1 ;;
        esac
    done
done
exit $fail
