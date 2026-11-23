"""Smoothness numbers from a smooth camera trace (SNAP_SMOOTH_CAMERA_TRACE=<csv>, runtime/nvn/nvn_smoothcam.cpp).

One row per vkQueuePresentKHR: present time, game frame, pass, alpha, re-projected or not, the shown camera's yaw / pitch.
The display is modelled as a fixed-refresh panel at --hz: at each refresh it shows the newest present made before it. For every
refresh inside a turning stretch (camera yaw speed between --min-speed and --max-speed degrees a second over the game frame) the script takes the
yaw change since the previous refresh and reports how even those steps are:

  zero_steps   refreshes that showed no camera change while the camera was turning (a repeated frame: the 60 Hz stair step)
  cv           coefficient of variation of the steps (0 = perfectly even)
  p95_ratio    95th percentile step / median step

It also reports the same numbers present to present (each vkQueuePresentKHR against the one before it), which does not depend on
how the present times line up with the modelled refresh grid.

  python -I scripts/smoothcam_trace.py <trace.csv> [--hz 144] [--from-ms 0] [--to-ms 1e12] [--min-speed 5] [--max-speed 360]
"""
import argparse
import csv
import statistics
import sys


def load(path):
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            if not r.get('yaw_deg'):
                continue
            rows.append((float(r['t_ms']), int(r['frame']), float(r['yaw_deg']), int(r['warped']), int(r['active'])))
    return rows


def unwrap(prev, cur):
    d = cur - prev
    while d > 180:
        d -= 360
    while d < -180:
        d += 360
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--hz', type=float, default=144.0)
    ap.add_argument('--from-ms', type=float, default=0.0)
    ap.add_argument('--to-ms', type=float, default=1e12)
    ap.add_argument('--min-speed', type=float, default=5.0, help='degrees per second; slower stretches are not counted')
    ap.add_argument('--max-speed', type=float, default=360.0, help='degrees per second; faster frames are cuts and not counted')
    a = ap.parse_args()
    rows = [r for r in load(a.trace) if a.from_ms <= r[0] <= a.to_ms]
    if len(rows) < 10:
        print('too few rows with a camera'); return 1
    # camera speed per game frame (degrees a second), from the frame's last present to the next frame's last present
    last_of = {}
    for t, fr, yaw, w, act in rows:
        last_of[fr] = (t, yaw)
    frames = sorted(last_of)
    speed = {}
    for i in range(1, len(frames)):
        (t0, y0), (t1, y1) = last_of[frames[i - 1]], last_of[frames[i]]
        if t1 > t0:
            speed[frames[i]] = abs(unwrap(y0, y1)) / ((t1 - t0) / 1000.0)
    period = 1000.0 / a.hz
    t = rows[0][0]
    end = rows[-1][0]
    j = 0
    shown = None
    steps = []
    prev_yaw, prev_fr = None, 0
    while t <= end:
        while j < len(rows) and rows[j][0] <= t:
            shown = rows[j]; j += 1
        if shown is not None:
            yaw, fr = shown[2], shown[1]
            if prev_yaw is not None and fr - prev_fr in (0, 1) and a.min_speed <= speed.get(fr, 0.0) <= a.max_speed:
                steps.append(abs(unwrap(prev_yaw, yaw)))
            prev_yaw, prev_fr = yaw, fr
        t += period
    turning = lambda fr: a.min_speed <= speed.get(fr, 0.0) <= a.max_speed
    pp = [abs(unwrap(r0[2], r1[2])) for r0, r1 in zip(rows, rows[1:]) if r1[1] - r0[1] in (0, 1) and turning(r0[1]) and turning(r1[1])]
    presents = len(rows)
    dur = (rows[-1][0] - rows[0][0]) / 1000.0
    game = len(set(r[1] for r in rows))
    warped = sum(r[3] for r in rows)
    print(f'{presents} presents in {dur:.1f} s ({presents / dur:.1f}/s), {game} game frames ({game / dur:.1f}/s), {warped} re-projected')
    if not steps:
        print('no turning stretch'); return 0
    nz = [s for s in steps if s > 1e-6]
    zero = len(steps) - len(nz)
    med = statistics.median(steps)
    mean = statistics.fmean(steps)
    cv = statistics.pstdev(steps) / mean if mean > 0 else 0.0
    p95 = sorted(steps)[int(0.95 * (len(steps) - 1))]
    print(f'refreshes while turning: {len(steps)}; zero_steps {zero} ({100.0 * zero / len(steps):.1f}%); '
          f'mean step {mean:.4f} deg, median {med:.4f}; cv {cv:.3f}; p95_ratio {p95 / med if med > 0 else float("inf"):.2f}')
    if pp:
        pm = statistics.fmean(pp)
        pz = sum(1 for s in pp if s <= 1e-6)
        print(f'present to present while turning: {len(pp)}; zero_steps {pz} ({100.0 * pz / len(pp):.1f}%); '
              f'median step {statistics.median(pp):.4f} deg; cv {statistics.pstdev(pp) / pm if pm > 0 else 0.0:.3f}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
