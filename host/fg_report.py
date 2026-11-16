# SPDX-License-Identifier: MIT
# Frame generation timing report from a SNAP_FRAMEGEN_TRACE csv (one row per present: t_ms, generated, active).
#   python -I host\fg_report.py <trace.csv> [--from-s N] [--to-s N] [--json out.json]
# Only rows with active=1 are measured (frame generation on). Prints presented avg fps, 1% / 0.1% low (from the present-to-present time),
# base fps (real frames only), the generated share, and the pacing split: the gap from a generated present to the real present after it
# against the gap from the real present to the next generated one (an even cadence is 50 / 50).
import csv, json, sys


def pct_low(intervals_ms, frac):
    # the average fps of the slowest `frac` of the frames (the usual "1% low" figure)
    v = sorted(intervals_ms, reverse=True)
    n = max(1, int(len(v) * frac))
    worst = v[:n]
    return 1000.0 / (sum(worst) / len(worst))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    lo, hi, jout = 0.0, 1e12, None
    i = 2
    while i < len(argv):
        if argv[i] == '--from-s': lo = float(argv[i + 1]) * 1000; i += 2
        elif argv[i] == '--to-s': hi = float(argv[i + 1]) * 1000; i += 2
        elif argv[i] == '--json': jout = argv[i + 1]; i += 2
        else: i += 1
    rows = []
    with open(path, newline='') as f:
        r = csv.reader(f)
        next(r, None)
        for row in r:
            if len(row) != 3:
                continue   # a torn last line
            try:
                rows.append((float(row[0]), int(row[1]), int(row[2])))
            except ValueError:
                continue
    rows = [x for x in rows if lo <= x[0] <= hi]
    # contiguous active runs (long gaps, loads and menus, split a run so they do not count as one slow frame)
    ivs, real_ivs, gen_to_real, real_to_gen = [], [], [], []
    ngen = nreal = 0
    prev = None
    last_real_t = None
    for t, g, a in rows:
        if not a:
            prev = None
            last_real_t = None
            continue
        if g: ngen += 1
        else: nreal += 1
        if prev is not None:
            d = t - prev[0]
            if 0.2 < d < 250:
                ivs.append(d)
                if prev[1] and not g: gen_to_real.append(d)
                if (not prev[1]) and g: real_to_gen.append(d)
            if not g and last_real_t is not None and 0.5 < t - last_real_t < 250:
                real_ivs.append(t - last_real_t)
        if not g: last_real_t = t
        prev = (t, g)
    if not ivs:
        print('no active frame generation rows in range')
        return 1
    dur_s = sum(ivs) / 1000.0
    out = {
        'presented_frames': len(ivs) + 1, 'measured_s': round(dur_s, 2),
        'presented_avg_fps': round(len(ivs) / dur_s, 1),
        'presented_1pct_low_fps': round(pct_low(ivs, 0.01), 1),
        'presented_0p1pct_low_fps': round(pct_low(ivs, 0.001), 1),
        'base_avg_fps': round(len(real_ivs) / (sum(real_ivs) / 1000.0), 1) if real_ivs else 0,
        'base_1pct_low_fps': round(pct_low(real_ivs, 0.01), 1) if real_ivs else 0,
        'generated_share': round(ngen / max(1, ngen + nreal), 3),
        'gen_to_real_ms_median': round(sorted(gen_to_real)[len(gen_to_real) // 2], 2) if gen_to_real else 0,
        'real_to_gen_ms_median': round(sorted(real_to_gen)[len(real_to_gen) // 2], 2) if real_to_gen else 0,
    }
    for k, v in out.items():
        print(f'{k}: {v}')
    if jout:
        with open(jout, 'w') as f:
            json.dump(out, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
