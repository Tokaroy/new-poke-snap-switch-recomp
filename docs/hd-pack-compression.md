# HD pack compression

Lossless, transparent Windows file compression for the generated HD texture packs (`hd-auto`, HD shadows, character packs).
The game opens and reads the files exactly as before; only the space on disk changes. NTFS drives only.

## Commands

```
python scripts/hd/compress_pack.py <pack folder> [more folders]            # LZNT1 (default), prints before/after per format
python scripts/hd/compress_pack.py <pack folder> --algo lzx                # smaller, slower reads (xpress4k/8k/16k, lzx)
python scripts/hd/compress_pack.py <pack folder> --status                  # sizes only, changes nothing
python scripts/hd/compress_pack.py <pack folder> --undo                    # compact /U for both kinds
```

Right after a build: `snap_builder.py hdtex ... --compress` (or `--compress xpress16k`). Compression resets when a file is
rewritten (LZNT1 and the /EXE kinds both), so run it after the pack is final. Run `--undo` before switching algorithms.
A file the game has open is skipped with an error count; close the game and run it again.

## Measurements (2026-11-22, A: NVMe, 320 files, 0.90 GB sample across the 5 formats in hd-auto)

Size saved (compact on disk size versus file size):

| Format | LZNT1 | XPRESS4K | XPRESS8K | XPRESS16K | LZX |
|---|---|---|---|---|---|
| BC1 sRGB (5.8 GB in hd-auto) | 19.7% | 29.1% | 31.9% | 33.4% | 42.1% |
| BC7 sRGB (5.4 GB) | 42.6% | 47.1% | 49.9% | 51.7% | 57.1% |
| BC7 (4.9 GB) | 56.8% | 60.6% | 63.9% | 66.0% | 70.5% |
| BC1 (0.8 GB) | 46.5% | 54.5% | 57.2% | 58.6% | 64.6% |
| BC6H (0.8 GB) | 55.3% | 57.7% | 60.4% | 62.0% | 64.8% |
| BC3 / BC3 sRGB (small) | 50.3 / 46.9% | 56.9 / 52.7% | 59.7 / 55.9% | 61.3 / 57.4% | 68.0 / 65.8% |

Weighted by the bytes of each format in hd-auto (16.6 GB), the estimate for the whole pack is about 35% saved with LZNT1,
47% with XPRESS16K and 53% with LZX. The tool prints the real numbers.

Read speed, no-buffering reads on one thread / eight threads, in GB/s of file data (steady state, second pass):

| Format | none | LZNT1 | XPRESS4K | XPRESS8K | XPRESS16K | LZX |
|---|---|---|---|---|---|---|
| BC1 sRGB | 2.0 / 5.6 | 2.1 / 4.5 | 0.5 / 2.2 | 0.7 / 2.1 | 0.8 / 2.8 | 0.5 / 1.5 |
| BC7 sRGB | 2.0 / 5.9 | 2.6 / 9.0 | 0.7 / 4.0 | 0.8 / 4.5 | 0.9 / 5.2 | 0.8 / 2.9 |
| BC7 | 0.5 / 2.6 | 1.3 / 3.7 | 0.5 / 1.6 | 0.6 / 1.6 | 0.5 / 1.8 | 0.6 / 1.5 |
| BC6H (small files) | 0.3 / 1.0 | 0.5 / 0.8 | 0.3 / 0.4 | 0.3 / 0.7 | 0.2 / 0.5 | 0.3 / 0.6 |

Reading the numbers: LZNT1 reads as fast as plain files (less data crosses the bus, decompression is cheap), so it is the default
and is safe for every format. XPRESS and LZX save 5 to 22 more points but cost 2 to 4 times the single-thread read time (still
0.5 to 0.9 GB/s, far above what a course load streams). Use them if disk space matters more than the last bit of load time.

Caveats: small files (under about 1 MB) give noisy timings; XPRESS/LZX data written by the test was still in the system cache
(no admin rights to flush the standby list), so their disk wait is understated, while their decompress cost is fully counted. No
game run was timed. HD shadow packs were not on this PC to measure; they use the same texture files.
