# Photo scoring

How the game turns a photo into points, as worked out from the lifted code and checked against the original on real photos.
Addresses are offsets in the main module (v262144). This page describes behavior only; it holds no game code and no game tables.

## Where it runs

The photo evaluator's per-frame update (`0x418350`) measures each Pokémon in a new photo and calls four scorers per subject.
Each scorer reads the scoring parameters (GameParameter `table_c`, loaded by `0x4aea20`; field names in `scripts/fb_names.py`
under `paramb` `R.3`) and writes a small result block. The evaluator then adds the parts into the photo's result record.

| Part | Function | What it measures |
|---|---|---|
| Pose | `0x422190` | the pose the Pokémon is in and its star count |
| Size | `0x421480` (uses `0x426520`) | how large and how unobstructed the Pokémon is on screen |
| Direction | `0x422350` (uses `0x40e740`, `0x40f5e0`, `0x410470`) | which way it faces and the vertical camera angle |
| Framing | `0x422690` | how close its key parts are to the centre of the frame |
| Other Pokémon | inside `0x418350` | a bonus for each other Pokémon in the shot |
| Background | inside `0x418350` | a bonus for background elements of three kinds |

Total points = pose + size + direction + framing + other-Pokémon bonus + background bonus, each part cut to a whole number
before the sum. Size is the larger of the two size estimates below.

## The parts

**Pose.** Each pose of each Pokémon has a base value and a star count (1 to 4). Points = star multiplier (from the parameters, by
star count) × base value + a per-species bonus (zero for most species).

**Size.** Two estimates; the larger wins.
- Pixel estimate: a per-species curve over the share of the screen the Pokémon's pixels cover, times a factor for how much of
  its body is inside the frame, times a factor for the share of its pixels that are visible (not blocked by other things).
- Sphere estimate: the same idea using the Pokémon's bounding sphere (area × visible share), with the in-frame factor counting
  only body parts of two particular kinds (likely the main body; not confirmed).

The in-frame share is a weighted average over the body-part spheres, each weighted by its rough projected area. For each
sphere, `0x426520` finds the four points of its outline as seen from the camera (top, bottom, left, right), projects them,
and returns the share of the box they span that lies outside the screen. It uses the SDK's polynomial sine and cosine and
the hardware's reciprocal square root estimate, so a faithful rewrite has to reproduce both bit for bit.

**Direction.** Points = pitch factor × the better of two facing curves × an edge factor.
- Facing curves map an angle in degrees to points through per-species breakpoints: facing the camera scores highest.
- The pitch curve maps the vertical angle through six per-species breakpoints with a symmetric weight pattern, so level shots
  score best and shots from straight above or below score least.
- The edge factor halves the result when the Pokémon's centre falls outside the screen.
- Some poses carry alternative measurements; the best one counts.

**Framing.** The scorer projects the Pokémon's key parts (two kinds) to the screen and takes four positions: the mean of all
key parts, the mean of the second kind, and the part of each kind nearest the centre. "Distance" is the larger of |x| and |y|
in screen units (0 = centre, 1 = edge). Points come from a curve over 1 − the smallest distance. If even the nearest part of
the second kind is off screen, a fixed value from the start of the curve is used instead. For paired subjects the midpoints between the two Pokémon
also count.

**Other Pokémon.** Every other Pokémon in the photo is run through the size, direction and pose scorers. It is skipped when
the evaluator has no measurements for it, when none of its body is in the frame, or when its pixel area is zero, at or below a
minimum, or fully blocked. Its pixel-size points (not the larger of the two size estimates), direction points and pose points
are each multiplied by a weight and added up. The partner gets its own weight and minimum area. The bonus is the three sums
added together, cut to a whole number.

**Background.** Background subjects come in three kinds, each with its own minimum area. They are scored the same way and with
one shared weight, but only the best two count: the bonus is the sum of the top two scores, cut to a whole number. Subjects
without pixel counts fall back to their bounding-sphere measurements.

The evaluator stores the six parts and the total in the photo's result record.

## Curves

All curves are piecewise linear: below the first breakpoint the first value holds, above the last the last value holds, and
between two breakpoints the value is interpolated. The facing curves sort their points by angle first; the per-species size
curves start at zero area.

## How this was checked

`host/mods/pscap.cpp` (off unless `SNAP_PSCAP` is set) snapshots the memory a function can reach when it is entered.
`tests/photoscore/run_snapshot_diff.py` replays each recorded call twice, through the lifted function and through a readable
rewrite, and compares every byte the caller can see plus the return value. Two Florio runs (5,464 recorded calls) matched on
every call for the four scorers, the three direction curves and `0x426520`. Inside the size scorer, every body-part sphere was
also checked against the lifted `0x426520`, along with moved and resized copies of it, about 534,000 spheres in all.

The bonuses and the total have no function of their own, so they are checked across two calls with `--pair`. The snapshot at
the framing call holds the evaluator's inputs (its locals, the object list, the visibility list and the id map). The readable
code computes the six parts and the total from that snapshot. A later call (`0x415bd0`, or the next evaluator entry) shows
the record the game itself wrote, and the two must match. All 71 photos of the second run matched. In those photos the
other-Pokémon bonus ranged from 0 to 139. The background bonus was 0 in every photo: a background subject appeared but never
passed the checks, so the top-two rule has not been exercised yet. The rewrite itself is reconstructed from the game, so it
stays out of the repository.
