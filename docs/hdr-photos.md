# HDR photo copies

Off by default. With HDR output on, each HD photo copy and each screenshot is also written as an HDR PNG next to the normal file.

**This reconstructs highlights. It does not capture the scene's real light.** The game tone maps its scene to 8-bit sRGB before any photo is made, and the photo, the screenshot and the HD copy all read that 8-bit picture. The HDR file is made from it with the same inverse tone map and roll-off the HDR display path uses (`runtime/nvn/shaders/hdr.frag`): the bright end above 75% of white is lifted by `SNAP_HDR_EXPAND` and rolled off toward the display peak. A highlight the game had already clipped stays a flat bright patch; it only gets brighter. A true scene-referred export would need the pre-tonemap buffer, which is not available at the photo capture draw (the capture reads the finished window texture; the Fx pre-tonemap tap does not run on photo frames). See the HDR section of AGENTS.md.

## Turn it on

| How | Setting |
| --- | --- |
| F1 > Photos > "HDR photo copies" (restart) | settings key `hdr_photos` |
| Environment | `SNAP_HDR_PHOTOS=1` |
| Command line | `--hdr-photos on` (or `off`), `--set hdr_photos=1` |

It needs HDR output: `SNAP_HDR=hdr10` or `scrgb` (settings key `gfx_hdr`) and a display or driver that gives an HDR swapchain. If HDR output is off or fell back to SDR, the log says `[hdrphoto] HDR photo copies are on but HDR output is off ... nothing is written` and no file is made. The HD photo copies themselves need HD photos on (`SNAP_HD_PHOTOS`, the default with the Enhanced look).

## Files

- HD photo copy: `<save root>\photos_hd\<crc32>.hdr.png` beside `<crc32>.jpg` (2160x1440). It is made from the same pixels as the HD JPEG (the q95 JPEG is decoded again), at the moment the JPEG is written. It is removed with its JPEG by the 1200-file trim.
- Screenshot (F12): `SnapRecomp_<date>_<time>_<n>.hdr.png` beside the normal PNG, at render size (HUD choice follows `shot_hud`).
- The game's own photo in the save, its JPEG, the thumbnail and the score screen are never touched. The code only reads, on the writer threads after the normal files are written.

## Format

16-bit RGB PNG written through the Windows Imaging Component, then three chunks are inserted after IHDR:

- `cICP` 9, 16, 0, 1: BT.2020 primaries, PQ (SMPTE ST 2084), RGB, full range.
- `mDCv`: BT.2020 primaries, D65 white, mastering maximum = the HDR peak (`SNAP_HDR_PEAK_NITS`, the display's own maximum by default), minimum 0.0001 nits.
- `cLLi`: MaxCLL (brightest max(R,G,B) of any pixel, nits) and MaxFALL (mean of max(R,G,B)).

PNG with PQ and cICP was chosen over AVIF and JPEG XR because it needs only WIC (already used for the other PNGs) and a few lines of chunk code, no new library. Viewers that read cICP show it as HDR; others treat it as an untagged PQ picture, which looks washed out. Which viewers do was not tested here.

Paper white, peak and the highlight lift come from the same rows as the HDR display (`gfx_hdr_paper`, `gfx_hdr_peak`, `gfx_hdr_expand`), read when each file is written.

## Code

- `runtime/nvn/hdrphoto_core.h`: the conversion and chunk writer (no Windows includes; unit test `hdrphoto.core` in `tests/hle/hle_tests.cpp`).
- `runtime/nvn/nvn_hdrphoto.{h,cpp}`: setting, "is HDR output active" check, WIC encode, atomic file write.
- Hooks: `WriteIfReady` in `nvn_hdphoto.cpp` (the JPEG writer lambda) and the writer job in `nvn_shot.cpp`. `snap_hdr::Active()` and `ExpandAmount()` are new in `nvn_hdr.{h,cpp}`.
- Not covered: the full-size PNG option (`hires_photos`), the PNG export of the album (`photo_export`) and the album screenshots.

## Checked

On the real exe (hidden desktop, scRGB swapchain, 775 nit peak, 4K): `host\routes\hdr_photos.txt` (florio_eval with ride screenshots) with the option on wrote `.hdr.png` beside every HD copy and screenshot and reached `ui=evaluation:End`; off wrote none. A written file decodes (own PNG reader plus PIL), carries `cICP 9,16,0,1`, `mDCv` (peak 774.87 nits) and `cLLi`, has no `sRGB`/`gAMA` chunk, and its brightest pixels are 243 nits against the 200 nit paper white. The conversion is unit tested (`hdrphoto.core`).

Not shown: byte-identical game photos and scores off versus on. The photos a route takes land on different frames in every run, so two runs never share a photo. The save-state driver `host\hdr_photos_ck.ps1` (same photos scored twice, option flipped between) did not complete its second pass on this PC (the state load returned to an album screen, a later run's state save timed out). What backs the claim instead: the option adds no GPU command, no guest memory access and no change to any capture draw; it runs on writer threads after the normal file is written, from pixels the host already holds.
