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
before the sum.

## The parts

**Pose.** Each pose of each Pokémon has a base value and a star count (1 to 4). Points = star multiplier (from the parameters, by
star count) × base value + a per-species bonus (zero for most species).

**Size.** Two estimates; the larger wins.
- Pixel estimate: a per-species curve over the share of the screen the Pokémon's pixels cover, times a factor for how much of
  its body is hidden behind other things, times a factor for the share of its pixels that are visible.
- Sphere estimate: the same idea using the Pokémon's bounding sphere (area × visible share), with the hidden factor counting only
  body parts of two particular kinds (likely the main body; not confirmed).
The hidden share is a weighted average over the body-part spheres, each weighted by its rough projected area, using a
per-sphere visibility test against the camera (`0x426520`).

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

**Other Pokémon and background.** The evaluator adds a bonus per other Pokémon visible in the shot (weights depend on whether it
is the paired partner) and per background element of three kinds, scaled by how visible each is. These sums live inside
`0x418350` and are not yet rewritten or tested.

## Curves

All curves are piecewise linear: below the first breakpoint the first value holds, above the last the last value holds, and
between two breakpoints the value is interpolated. The facing curves sort their points by angle first; the per-species size
curves start at zero area.

## How this was checked

`host/mods/pscap.cpp` (off unless `SNAP_PSCAP` is set) snapshots the memory a function can reach when it is entered.
`tests/photoscore/run_snapshot_diff.py` replays each recorded call twice, through the lifted function and through a readable
rewrite, and compares every byte the caller can see plus the return value. One Florio run (72 shots, 168 pose-score calls,
2675 recorded calls) matched on every call for the four scorers and the three direction curves. The rewrite itself is
reconstructed from the game, so it stays out of the repository.
