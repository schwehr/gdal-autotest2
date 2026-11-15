<!-- disableFinding(LINE_OVER_80) -->
# Overview resampling test grids

Test data for GDAL overview resampling (`OVERVIEW_RESAMPLING`). See
[PRD.md](PRD.md), [PLAN.md](PLAN.md), [TASKS.md](TASKS.md) and
[LEDGER.md](LEDGER.md).

*   `make_resample_grids` writes an **Int16** and a **Float32** grid
    (6848 × 15840 px, 83 panels of 1024 px in 14 rows, 100 px guards, every
    valid pixel within ±1000, nodata −9999 / NaN only in row 13), plus
    `manifest.json` and `panels.geojson`. With `--make_cogs` it also builds
    reference COGs with the GDAL COG driver for all 9 methods.
*   `analyze_resample_grids` writes `stats.csv`, previews, per-panel contact
    sheets, and a `--verify` report of the manifest's behavior assertions.

Every TIFF is ZSTD level 22 (`tiff_compression_options()`).

## Commands

# Unmasked code path (no nodata tag, no row 13).
$B run $PKG:make_resample_grids -- --output_dir=$OUT-nonodata --nodata=none --make_cogs

# Workstation COG validator.
for f in $OUT/cog/*/*.tif; do
  python3 gdal/swig/python/samples/validate_cloud_optimized_geotiff.py "$f"
done
```

Useful generator flags: `--small` (256 px panels, 2240 × 5088),
`--panels=0,13,slope_x` (rows or IDs), `--dtypes`, `--methods`, `--jobs`
(parallel COG builds, threads), `--num_threads` (GDAL threads per build),
`--seed`, `--columns=12`, `--force` (skip the free-disk check). Analysis
flags: `--panels`, `--methods`, `--dtypes`, `--rounding`, `--stats`,
`--previews`, `--contact_sheets`, `--output_dir` (default
`<input_dir>/analysis`).

## Outputs

| Path | Content |
| --- | --- |
| `resample_grid_{int16,float32}.tif` | Base grids, 512² tiles, no overviews; metadata has the seed, version, layout and GDAL version. |
| `manifest.json` | Grid, per-panel origin, block offsets and inner windows per level, parameters, subregions, expectations, file SHA-256s, COG list (`ee_default` marks CUBIC/Float32 and NEAREST/Int16), timings. |
| `panels.geojson` | Panel and subregion outlines in WGS84 with `block_offset_32`, for QGIS labels. |
| `cog/<dtype>/resample_grid_<dtype>_<METHOD>.tif` | COG-driver references, 5 overview levels (2×–32×). |
| `analysis/stats.csv` | min/max/mean/std, valid and nodata counts, counts beyond ±1000, distinct values, per dtype × method × level × panel/subregion. |
| `analysis/previews/*.png`, `analysis/contact/*.png` | Colormap: gray [−1000, 1000], red above, blue below, magenta nodata. |
| `analysis/verify_report.json` | Every expectation result; `source` failures exit 1, `expected` ones are recorded only. |

## Reading contact sheets and block offsets

Rows are methods, columns are the base panel and levels 1–5; each tile is the
panel's *inner window* (overview pixels whose footprint lies fully inside
the panel), upscaled with NEAREST. Panels start at x0 = 100 + 1124·c,
y0 = 100 + 1124·r, all ≡ 0 (mod 4): block-aligned at 2× and 4×, but offset by
the title's "offset mod 32" at 8×–32×. So a 1 px feature may survive NEAREST
in one panel and vanish in the same spot of another; expectations use
absolute coordinates.

## Measured behavior worth knowing

See LEDGER.md for details. MODE writes 0 (not nodata) for all-nodata blocks;
CUBIC spreads isolated NaN even with the NaN nodata tag; NEAREST copies
nodata; AVERAGE/RMS mask it. The COG driver builds levels cascaded. Int16
overview rounding is half away from zero.

## Disk, time and memory (full grid, this workstation)

| Step | Wall | Peak RSS | Size |
|---|---|---|---|
| make (grids only) | ~2 min | - | Int16 12.1 MB, Float32 40.0 MB |
| make --make_cogs (4 threads) | 21:26 | 6.2 GB | 710 MB total (COGs: int16 156 MB, float32 504 MB) |
| analyze --verify (+sheets) | 5:21 | 6.2 GB | +150 MB |
| make --small --make_cogs | COGs 34-95 s each | ~3.6 GB | - |

## GDAL upgrade check (CUJ 4)

1.  Before the upgrade: generate into `$OUT-before` with `--make_cogs` and
    run `analyze_resample_grids --input_dir=$OUT-before --verify`.
2.  After: generate into `$OUT` with `--make_cogs`, run `$B test $PKG:all`
    and `analyze_resample_grids --input_dir=$OUT --verify`.
3.  Review every `source` failure and every assertion whose status changed
    between the two `verify_report.json` files.

## Assets

See [assets/ASSETS.md](assets/ASSETS.md): EE logo (Google, internal use),
Landsat 8 B8 chip (USGS public domain), skimage cat (CC0), Roboto (Apache-2.0).
