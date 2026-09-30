#!/usr/bin/env python3
"""Compare fptest outputs: reference QEMU vs JIT build.

Accepted deviations of the JIT (documented):
  - VX detail bits: the host has one "invalid" flag, reported as VXSOFT
    (VX and FX are exact).
  - FI (and FR): FI says whether the last instruction was inexact; the JIT
    reports whether any instruction since the last FPSCR read was.
Everything else must be bit-identical.
"""
import sys
from collections import Counter, defaultdict

VX_DETAIL = (0x3f << 19) | (1 << 10) | (1 << 9) | (1 << 8)
FI_FR = (1 << 17) | (1 << 18)
STRICT = 0xffffffff & ~(VX_DETAIL | FI_FR)
FPRF = 0x1f << 12
VX = 1 << 29

import re, struct

def sp_exact(u):
    """binary64 bits u holds a binary32 value (or inf/NaN without low bits)"""
    d = struct.unpack('<d', struct.pack('<Q', u))[0]
    try:
        f = struct.unpack('<f', struct.pack('<f', d))[0]
    except OverflowError:
        return False
    return d != d and (u & 0x1fffffff) == 0 or f == d

SP_SCALAR = re.compile(r'^xs(n?m(add|sub)[am]|add|sub|mul|div|sqrt)sp$')
UX, OX = 1 << 27, 1 << 28

def parse(path):
    with open(path) as f:
        return [l.rstrip('\n') for l in f]

def main(ref_path, jit_path, show=8):
    ref, jit = parse(ref_path), parse(jit_path)
    if len(ref) != len(jit):
        print(f"different line counts: {len(ref)} vs {len(jit)}")
    stats = defaultdict(Counter)
    samples = defaultdict(list)
    for r, j in zip(ref, jit):
        op = r.split(' ', 1)[0]
        if r == j:
            stats[op]['identical'] += 1
            continue
        rt, jt = r.split(), j.split()
        if '->' in rt and '->' in jt and rt[:rt.index('->')] == jt[:jt.index('->')]:
            k = rt.index('->')
            rres, jres = rt[k + 1], jt[k + 1]
            rest_r, rest_j = rt[k + 3:], jt[k + 3:]
            if SP_SCALAR.match(op):
                ins = [int(x.split(':')[0], 16) for x in rt[1:k]]
                if not all(sp_exact(u) for u in ins):
                    if rres != jres or rt[k + 2] != jt[k + 2]:
                        # QEMU rounds to binary64 first, then to binary32
                        stats[op]['QEMU double rounding, non-SP input (ok)'] += 1
                        continue
            if rres != jres or rest_r != rest_j:
                kind = 'RESULT' if rres != jres else 'CR'
                stats[op][kind] += 1
                if len(samples[op]) < show:
                    samples[op].append((kind, r, j))
                continue
            if len(rt) > k + 2:
                rf, jf = int(rt[k + 2], 16), int(jt[k + 2], 16)
                d = rf ^ jf
                if d & FPRF and rf & VX and (op.startswith('fcti') or op == 'fcmpo'):
                    # fcti*: FPRF is undefined after an invalid conversion
                    # (QEMU sets C|FU); fcmpo: the ISA leaves C unchanged
                    # on a NaN compare (QEMU sets it).
                    stats[op]['FPRF undef/ISA (ok)'] += 1
                    d &= ~FPRF
                    if not d & STRICT:
                        continue
                if (d & STRICT) == UX and jf & UX and rf & OX:
                    # QEMU does not report underflow in one element of a
                    # vector when another one overflowed
                    stats[op]['QEMU drops UX next to OX (ok)'] += 1
                    continue
                if d & STRICT:
                    stats[op]['FPSCR'] += 1
                    if len(samples[op]) < show:
                        samples[op].append((f'FPSCR diff {d & STRICT:08x}', r, j))
                elif d & VX_DETAIL and not d & FI_FR:
                    stats[op]['vx-detail (ok)'] += 1
                elif d & FI_FR and not d & VX_DETAIL:
                    stats[op]['FI/FR (ok)'] += 1
                else:
                    stats[op]['vx-detail+FI (ok)'] += 1
            else:
                stats[op]['OTHER'] += 1
                samples[op].append(('OTHER', r, j))
        elif (len(rt) >= 2 and rt[-2] == 'fpscr' and rt[:-1] == jt[:-1]
              and len(jt) == len(rt)):
            # "... fpscr XXXXXXXX" (mmatest): same rules as above
            rf, jf = int(rt[-1], 16), int(jt[-1], 16)
            d = (rf ^ jf) & STRICT
            if d == 0:
                stats[op]['vx-detail/FI (ok)'] += 1
            elif d == UX and jf & UX and rf & OX:
                stats[op]['QEMU drops UX next to OX (ok)'] += 1
            else:
                stats[op]['FPSCR'] += 1
                if len(samples[op]) < show:
                    samples[op].append((f'FPSCR diff {d:08x}', r, j))
        else:
            # sequences: FPSCR values follow 'fpscr'; FI is approximate
            def fi_masked(t):
                out, after = [], False
                for w in t:
                    if after and len(w) == 8:
                        try:
                            w = '%08x' % (int(w, 16) & ~FI_FR)
                        except ValueError:
                            pass
                    after = after or w.startswith('fpscr') or w in ('handler', 'after', 'now', 'fault')
                    out.append(w)
                return out
            if fi_masked(rt) == fi_masked(jt):
                stats[op]['FI/FR (ok)'] += 1
                continue
            stats[op]['OTHER'] += 1
            if len(samples[op]) < show:
                samples[op].append(('OTHER', r, j))
    bad = 0
    for op, c in stats.items():
        errs = sum(v for k, v in c.items() if k in ('RESULT', 'CR', 'FPSCR', 'OTHER'))
        bad += errs
        flag = 'ERR ' if errs else 'ok  '
        print(f"{flag}{op:10s} " + ', '.join(f"{k}={v}" for k, v in sorted(c.items())))
    for op, ss in samples.items():
        for kind, r, j in ss:
            print(f"--- {op} {kind}\n  ref: {r}\n  jit: {j}")
    print("TOTAL errors:", bad)
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
