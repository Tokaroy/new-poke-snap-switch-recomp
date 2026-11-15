# SPDX-License-Identifier: MIT
"""GPU cost of refined meshes from a SNAP_MODEL_GPU log (runtime/nvn/nvn_model.cpp).

Each line is "present gpu_ms refined_draws original_draws". gpu_ms is the GPU busy time of the last frame the timer
collected, which trails the present by a small fixed lag. With the A/B on (SNAP_MODS_AB=1 or SNAP_MODEL_AB=1) presents
alternate between the refined and the original set, so for each lag L in 0..4 this pairs gpu_ms[n] with the set drawn
at present n-L and reports the mean difference (refined - original). The lag where the difference is largest in size
is the real one; the other lags show the noise floor.

usage: python -I gpu_cost.py <log> [--skip N] [--exclude p1,p2,...]
  --skip N       ignore the first N presents (boot, menus)
  --exclude ...  presents to drop with their neighbours (frame dumps stall the GPU)
"""
import statistics
import sys


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    path = args[0]
    skip = 0
    excl = set()
    i = 1
    while i < len(args):
        if args[i] == "--skip":
            skip = int(args[i + 1]); i += 2
        elif args[i] == "--exclude":
            for p in args[i + 1].split(","):
                if p:
                    for d in range(-3, 4):
                        excl.add(int(p) + d)
            i += 2
        else:
            print("unknown argument", args[i]); return 2
    rows = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            p, g, r, o = int(parts[0]), float(parts[1]), int(parts[2]), int(parts[3])
            rows[p] = (g, r, o)
    print(f"{len(rows)} presents, {sum(1 for v in rows.values() if v[1])} with refined draws, "
          f"{sum(1 for v in rows.values() if v[2])} with original draws")
    best = None
    for lag in range(0, 5):
        ref, org = [], []
        for p, (g, _, _) in rows.items():
            if p < skip or p in excl or g <= 0:
                continue
            src = rows.get(p - lag)
            if not src:
                continue
            _, r, o = src
            if r and not o:
                ref.append(g)
            elif o and not r:
                org.append(g)
        if len(ref) < 50 or len(org) < 50:
            print(f"lag {lag}: too few samples ({len(ref)} refined, {len(org)} original)")
            continue
        d = statistics.fmean(ref) - statistics.fmean(org)
        se = (statistics.pvariance(ref) / len(ref) + statistics.pvariance(org) / len(org)) ** 0.5
        med = statistics.median(ref) - statistics.median(org)
        print(f"lag {lag}: refined {statistics.fmean(ref):.3f} ms (n={len(ref)})  original {statistics.fmean(org):.3f} ms "
              f"(n={len(org)})  mean diff {d:+.3f} +- {se:.3f} ms  median diff {med:+.3f} ms")
        if best is None or abs(d) > abs(best[1]):
            best = (lag, d, se)
    if best:
        print(f"largest difference at lag {best[0]}: {best[1]:+.3f} +- {best[2]:.3f} ms per frame")
    return 0


if __name__ == "__main__":
    sys.exit(main())
