# Sharp photo album (HD photos in the album, photo select and score screen)

Status 2026-11-23 (audit and proof, no new code): the feature the "sharp photo album" queue item asks for already ships as **HD photos**. This page is the one place that says how the pieces fit and what was checked.

## What it does

When the game takes a photo under a scaled frame (4K, DLAA, DLSS, FSR, render scale above 1) the port also keeps a 2160x1440 q95 JPEG of the same scene region. When the game later shows that photo, the port draws the HD copy in place of the game's 1080x720 texture. Same UVs, same quad, linear filtering. The game's stored photo, its save files, its JPEG and the score inputs are only read.

| Where | What the game draws | What you see with HD photos on |
| --- | --- | --- |
| Photo select preview, score screen photo, album save, album full view (`photo_detail`) | the 1080x720 photo, 700 to 1100 UI units wide | the 2160x1440 copy |
| Photo select strip, ride shot preview (under 480 UI units) | 1080x720 or 360x240 | a box-filtered level of the HD copy (1080x720, 540x360 or 270x180) when it is at least 1.2x the game texture |
| Album grid cells | the game's 204x136 thumbnail JPEG | a box-filtered level of the matching HD crop (540x360 at 4K) |
| Photo Editing screens | the 1080x720 photo | unchanged on purpose: the effect passes feed the photo the game saves |

The Photodex (`zukan_*`) pages draw the game's own art, not stored photos, and the game has no zoom on a photo, so there is nothing more to substitute.

## How a stored photo is tied to its HD copy

1. At capture, each game capture draw also blits the region it samples into a 2160x1440 image and encodes it on a worker.
2. When the game encodes its own JPEG (`JpegEncoder::Encode` hook) the port matches it to a capture by 135x90 box averages after a per-channel tone fit, and names the file `photos_hd\<jpeg crc32>.jpg` in the profile's save folder.
3. When the game decodes a stored JPEG for the album, the decoded pixels' CRC picks the file (the album copies the pixels before upload, so the texture is matched by a hash of the first 64 KB).
4. Grid thumbnails are tied to a photo by content at encode time (`photos_hd\thumbs.txt`).

A photo with no match (taken before HD photos existed, or taken with the option off) keeps the game's image. No file means no change. Details: `host/README.md` "HD photos", `analysis/core/hd_photos.md`.

## Settings

| How | Name |
| --- | --- |
| Settings file key | `hd_photos` (`0` or `1`) |
| Environment | `SNAP_HD_PHOTOS` |
| Command line | `--set hd_photos=1` (every settings key works with `--set`, see `docs/command-line.md`) |
| F1 menu | Photos > HD photos (restart) |
| In-game Settings | PC tab, Photos, row 2 |

The default follows the Look row: on for Enhanced, off for Original (`gfx_look=original`). `SNAP_HD_PHOTOS_SMALL=0` keeps the game's texture for draws under 480 UI units. `SNAP_HD_PHOTOS_LOG=1` prints `[hdphoto]` lines.

## Proof (2026-11-23, build `build\w\ha`, hidden runs at 3840x2160, route `host/routes/hd_album_view.txt`)

- Ride: `hd_eval` (Florio) reached `ui=evaluation:End`, three photos saved with HD copies.
- Album: the full view of a ride photo logged `showing HD copy of photo 95a7ffbd ... 1080x720 drawn 702 wide`, the grid cell logged `showing HD grid thumbnail ... level 2, 540x360`. The off run logs neither.
- Frames: 3840x2160 captures of `photo_detail`, off and on, from the same save copy. The difference is small: the photo is drawn about 2100 host pixels wide, so the 2160-wide copy is roughly 1:1 where the game's 1080 texture is stretched 1.95x. It shows as cleaner leaf and trunk edges, not as new detail. A Laplacian-variance sharpness metric did not separate them (off 274, on 231, the stretched texture carries blocky aliasing), so no number is claimed.
- Save files: every file under the save folder except two small encrypted files was byte-identical between the off and on runs. The same two files also differ between two off runs, so they hold play time and a first-visit flag, not photos.
- Tests: HLE suite 2845 checks, 1 failed (`padfx`, known); `run_cli_flags_test.ps1` all passed; `tests/mods/run_tests.ps1` all passed.

## Not verified, not done

- Zoom-out mip levels for large draws: UI samplers have no mip filtering, only the CPU-built levels for small draws exist.
- A photo older than the first HD-photos run has no HD copy and cannot get one (the original scene is gone).
- Hidden runs only; no live visible-window run, no DLSS or FSR re-check this session.
