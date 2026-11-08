#!/usr/bin/env python3
#
# frank-ecat — eCat3 (retro computers emulator) on RP2350 (FRANK M2)
#
# Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sampling profiler, end to end: "~C prof US~" to the running firmware,
# the sampled PCs read back, resolved against the ELF's symbols, a table of
# functions by share of the samples.
#
# Usage: tools/prof.py build/frank-ecat-agat.elf [US | c1:US for core 1 | lr:US with callers] [PORT] [TOP]

import bisect, subprocess, sys, time
import serial

def main():
    elf = sys.argv[1]
    us = sys.argv[2] if len(sys.argv) > 2 else '200'
    port = sys.argv[3] if len(sys.argv) > 3 else '/dev/cu.usbmodem213101'
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 40

    nm = subprocess.run(['arm-none-eabi-nm', '-C', '--defined-only', '-S', elf],
                        capture_output=True, text=True).stdout
    syms = []
    for ln in nm.splitlines():
        p = ln.split(' ', 3)
        if len(p) == 4 and p[2] in 'tTwW':
            syms.append((int(p[0], 16) & ~1, int(p[1], 16), p[3]))
    syms.sort()
    starts = [s[0] for s in syms]

    s = serial.Serial(port, 115200, timeout=0.5)
    s.read(100000)
    pairs = us.startswith('lr:')
    cmd = 'prof1' if us.startswith('c1:') else ('profl' if pairs else 'prof')
    us = us[3:] if (us.startswith('c1:') or pairs) else us
    s.write(('~C %s %s~' % (cmd, us)).encode())
    buf, t = b'', time.time()
    while b'PROF END' not in buf and time.time() - t < 60:
        buf += s.read(8192)
    text = buf.decode(errors='replace')
    if 'PROF BEGIN' not in text:
        sys.exit('no profile:\n' + text[-800:])
    body = text.split('PROF BEGIN', 1)[1].split('PROF END', 1)[0]
    pcs = [int(w, 16) for w in body.split()[1:] if len(w) == 8]

    def name_of(pc):
        i = bisect.bisect_right(starts, pc & ~1) - 1
        if i >= 0:
            a, sz, n = syms[i]
            if pc < a + max(sz, 1) or sz == 0:
                return n
        return '?'
    if pairs:
        # PC, LR pairs: who calls each of the hottest functions
        callers = {}
        for k in range(0, len(pcs) - 1, 2):
            f, c = name_of(pcs[k]), name_of(pcs[k + 1])
            callers.setdefault(f, {}).setdefault(c, 0)
            callers[f][c] += 1
        tot = len(pcs) // 2
        for f, cs in sorted(callers.items(), key=lambda x: -sum(x[1].values()))[:top]:
            print('%5.1f%%  %s' % (100.0 * sum(cs.values()) / tot, f[:90]))
            for c, n in sorted(cs.items(), key=lambda x: -x[1])[:4]:
                print('          %5.1f%%  <- %s' % (100.0 * n / tot, c[:80]))
        return
    count = {}
    for pc in pcs:
        i = bisect.bisect_right(starts, pc & ~1) - 1
        name = '?'
        if i >= 0:
            a, sz, n = syms[i]
            if pc < a + max(sz, 1) or sz == 0:
                name = n
        if pc >= 0x20000000 and pc < 0x20082000:
            name += ' [ram]'
        count[name] = count.get(name, 0) + 1
    total = len(pcs)
    print('%d samples every %s us' % (total, us))
    for n, c in sorted(count.items(), key=lambda x: -x[1])[:top]:
        print('%5.1f%%  %s' % (100.0 * c / total, n[:110]))

if __name__ == '__main__':
    main()
