<!-- disableFinding(LINE_OVER_80) -->
# Product Requirements Document (PRD): Overview Resampling Test Grids

*   **Document Status**: Draft (design only; nothing implemented yet).
    Revised 2026-09-28: Int16 instead of Int32, literal 100 px guards, NaN
    nodata for Float32, GDAL COG driver only.
*   **Target Feature**: QA test data for GeoTIFF / COG export overview
    resampling (`TiffPyramidOptions.ResamplingMethod`, which maps to GDAL
    `OVERVIEW_RESAMPLING`)
*   **Related Documents**:
    *   [Implementation Plan (`PLAN.md`)](PLAN.md)
    *   [Task List (`TASKS.md`)](TASKS.md)
    *   [Feature progress (`progress.md`)](progress.md#L30)
        has the open item *"How do we have the QA team test this?"*. This
        project answers it.

--------------------------------------------------------------------------------

## 1. Executive Summary & Problem Statement

### 1.1 Context

GDAL users choose the resampling method used to
build the overview levels (pyramids) of Cloud Optimized GeoTIFF exports. The
methods are `NEAREST`, `AVERAGE`, `BILINEAR`, `CUBIC`, `CUBIC_SPLINE` (GDAL
`CUBICSPLINE`), `LANCZOS`, `MODE`, `RMS` and `GAUSS`.

Overviews are what users see when they zoom out on a COG. They are also what EE
shows for COG-backed assets. The nine methods treat the same data very
differently:

*   some invent class codes that do not exist in the input;
*   some overshoot the input data range;
*   one throws away the sign of the data;
*   some delete thin or small features.

### 1.2 Problem Statement

No test data exercises these differences in a controlled, documented way. As a
result:

*   QA cannot confirm that the method chosen in the API is the one EE actually
    applied.
*   Engineers and doc writers cannot show users what each method does to their
    kind of data (categorical, continuous, imagery, sparse, nodata-heavy).
*   A GDAL upgrade in `//third_party/gdal` could silently change overview
    output with no regression signal.

### 1.3 Proposed Solution

A Python script, `make_resample_grids.py`, that deterministically generates
two large single-band GeoTIFF test grids:

*   one **Int16** grid and one **Float32** grid;
*   both with the same layout of **83 test panels**, grouped by pattern family
    and separated by 100 px guard regions;
*   every valid pixel within **±1000**.

Optionally, the script also builds reference COGs for all nine methods with
GDAL's **COG driver** (`OVERVIEW_RESAMPLING=<method>`).

A companion analysis step (P1) computes per-panel statistics, draws contact
sheets and runs automated behavior checks.

--------------------------------------------------------------------------------

## 2. Answers to the Two Design Questions

### 2.1 What other test patterns should it use?

§5.5 maps each of the 15 required categories to its panels. On top of those,
add the patterns below. Each one targets a specific failure mode of overview
resampling. ★ marks the strongly recommended ones.

| Added pattern | Why it matters for overview resampling |
| --- | --- |
| ★ **Calibration constants** (+1000, −1000, +333, −777) | Checks that every method keeps a flat region's value. Shows that RMS drops the sign (−777 becomes +777). Also measures how far each kernel bleeds into the guard. |
| ★ **Impulse (point-spread) phase sweep**, ±1000 | Shows each method's kernel directly, and how it changes with the pixel's position inside the 2^L block. NEAREST keeps an impulse only when both offsets are multiples of 2^L. |
| ★ **Zone plate** and **x/y chirps** | The classic resampling test: aliasing (moiré) against frequency and orientation in a single image. |
| **Siemens star**, **USAF-1951 bar target** | Human-readable resolution limit for each method and level. |
| ★ **Sine gratings** (periods 2.5–16 px, 0/30/45/60/90°) | Measures the frequency response: how much amplitude each method keeps at each period (an MTF table per method). |
| ★ **Int16 rounding probes** (2×2 motifs with block means of k + 0.5 and k ± 0.25) | GDAL computes most methods on Int16 input in Float32 and converts back to Int16 (§6.3). These probes expose the rounding rule and the drift that builds up across cascaded levels. |
| ★ **MODE tie probes** (2–2 ties, all-distinct blocks) | Documents how MODE breaks ties, and where cascaded MODE differs from MODE over the full block. |
| **Majority dither** (10/25/40/50 % minority class) | Whether MODE keeps the majority class; how random NEAREST is; how AVERAGE mixes classes. |
| **Salt-and-pepper noise** (1/5/20/50 %) | Robustness to outliers: MODE removes them, AVERAGE spreads them, RMS inflates them. |
| **Narrow bars and small steps** (widths 1–16 px, step heights 1–200) | Ringing relative to feature width and step height. Int16 rounding hides ringing smaller than one unit. |
| **5° slanted edge** (ISO 12233 style) | The standard way to measure edge spread with sub-pixel sampling. |
| ★ **Nodata adjacency**: nodata gaps between ± steps, and a curved nodata "coastline" | Whether nodata erodes, dilates, or leaks the sentinel into valid pixels (coastline halos). |
| **Thin class features and realistic land-cover codes** (NLCD-style 11–95) | MODE deletes roads and rivers. AVERAGE invents codes that do not exist (the mean of 11 and 95 is 53). |
| **Gentle zero-crossing ramps** (0.1 and 0.37 units/px) | Int16 staircases, and whether rounding is symmetric around 0. |
| **Fine terraces near Nyquist** (2–4 px) | Aliasing in data that looks continuous. |
| **Post/hole dipoles** | A +1000 post next to a −1000 hole: AVERAGE cancels them, RMS doubles them. |
| Optional: **1/f (pink) noise**, **native-resolution cat** | A natural-image spectrum, and photo detail that has not been smoothed by upscaling. |

### 2.2 How big should the grid be?

**Decision: 6848 × 15840 px per grid.** That is 6 columns × 14 rows of
1024 × 1024 px panels, one pattern family per row, with the **literal 100 px
guards** from the request (PRD D1) and 100 px outer margins. The right and
bottom margins are widened by 4 px so that both dimensions are multiples of
32. The layout has 84 slots and 83 are used.

| Quantity | Value |
| --- | --- |
| Panel size | 1024 × 1024 px (32 × 32 px at the coarsest level, 32×) |
| Guard between panels / outer margin | 100 px / 100 px (right and bottom: 104 px) |
| Panel pitch | 1124 px |
| Width | 2·100 + 6·1024 + 5·100 = 6844, padded to **6848** (214 × 32) |
| Height | 2·100 + 14·1024 + 13·100 = 15836, padded to **15840** (495 × 32) |
| Pixels per grid | 108.5 M: about 217 MB raw for Int16 (2 bytes/px) and 434 MB for Float32 (4 bytes/px) |
| Overview dimensions at 2×, 4×, 8×, 16×, 32× | 3424 × 7920, 1712 × 3960, 856 × 1980, 428 × 990, 214 × 495 |

Why this size:

1.  **Each level is exactly half the previous one.** The COG driver halves
    each dimension and rounds down
    ([`cogdriver.cpp` L1079–1100](gdal/frmts/gtiff/cogdriver.cpp#L1079-L1100)).
    Because W and H are multiples of 32 = 2^5, no rounding happens at any of
    the five levels. The ratio stays exactly 2^L, so level L covers the same
    ground as the base grid, and NEAREST samples exactly `base[::2^L, ::2^L]`.
    Without the 4 px padding, the ratio would be slightly off 2^L and
    NEAREST's sampling phase would drift across the grid.
2.  **Five levels (2×, 4×, 8×, 16×, 32×).** The COG driver's default rule
    (keep halving until both sides are ≤ 512) gives exactly five levels at
    this size. The script still passes `OVERVIEW_COUNT=5`.
3.  **Panel offsets are known and recorded.** Panel origins are
    x0 = 100 + 1124·c and y0 = 100 + 1124·r. Both 100 and 1124 are multiples
    of 4, so:
    *   at 2× and 4×, every panel starts on a block boundary;
    *   at 8×, 16× and 32×, panels start at different offsets inside the 2^L
        blocks. Mod 32, the x offsets are 4, 8, 12, 16, 20, 24 (columns 0–5)
        and the y offsets are 4, 8, …, 28, 0, 4, … (rows 0–13).

    The manifest records every panel's offset at every level. Expected
    results are computed from **absolute** grid coordinates, not
    panel-relative ones. At coarse levels, a 1 px feature can survive NEAREST
    in one panel and vanish in the same position of another. That is
    expected, and the manifest predicts it. The offset sweeps (impulses and
    posts at offsets 0–7) still cover every phase at 2×, 4× and 8×.
4.  **Panels stay isolated through 16×.** When levels are built one from the
    next (§6.3), the kernel reach is 2r(2^L − 1) base pixels. At 16× that is
    30 px for BILINEAR/GAUSS, 60 px for CUBIC/CUBICSPLINE and 90 px for
    LANCZOS, all under the 100 px guard. At 32× it is 62, 124 and 186 px:
    CUBIC and CUBICSPLINE reach less than 1 overview pixel into the next
    panel's edge, and LANCZOS about 3.
    *   Where a panel does not start on a block boundary, its first and last
        overview row and column mix guard and panel pixels for **every**
        method. The analysis only uses overview pixels whose footprint lies
        entirely inside the panel.
5.  **Readable at 32× and still manageable.** 1024 px panels leave room for
    128–256 px sub-cells, which are still 4–8 px wide at 32×. 512 px panels
    would be only 16 px at 32×. 2048 px panels would make each grid 4× larger.

Alternative: **squarer packing (`--columns=12`)**: 13600 × 7968 px (after
padding). Same pixel count and five levels, but two pattern families per row.

--------------------------------------------------------------------------------

## 3. Goals & Non-Goals

### 3.1 Goals

*   **G1**: Two deterministic grids, Int16 and Float32. They contain every
    required pattern category plus the recommended additions. Every valid
    pixel is within ±1000; only nodata (−9999 in Int16, NaN in Float32) is
    outside that range.
*   **G2**: Panels are separated by 100 px guards. Each panel's offset inside
    the 2^L blocks is recorded, so comparing panels across methods and levels
    is meaningful.
*   **G3**: A machine-readable description of every panel (`manifest.json`,
    `panels.geojson`): location, block offsets, parameters, and expected
    behavior per method.
*   **G4**: Local reference COGs for all nine methods, built with the GDAL COG
    driver.
*   **G5 (P1)**: Per-panel statistics, visual contact sheets and automated
    behavior checks. These double as a GDAL-upgrade regression check.

### 3.2 Non-Goals

*   Changing the EE export code or API. That is done in the feature CLs.
*   Testing reprojection resampling (`RESAMPLING`), `ee.Image.resample()`, or
    EE's own pyramiding policies for ingested assets (`MEAN`, `SAMPLE`,
    `MODE`, …). Those are separate systems.
*   Multi-band, paletted or complex data (`AVERAGE_MAGPHASE`).
*   Byte, UInt16 and Int32 grids. GDAL uses different working types for them
    (Byte and UInt16 stay integer for most methods; Int32 uses Float64), so
    Int16 results do not carry over. This is listed as future work (P2).
*   Benchmarking export performance, or perceptual quality scoring.
*   External publication of the grids or figures without brand review (§9).

--------------------------------------------------------------------------------

## 4. Personas & Critical User Journeys

| Persona | Needs |
| --- | --- |
| **EE backend engineer** (feature owner) | Confidence that each method behaves as designed, plus evidence to justify the defaults. |
| **Technical writer / DevRel** | Figures that show what each method does to different kinds of data. |

**CUJ 1: Generate the grids.** Run one command `make_resample_grids -- --output_dir=…`).
In under 5 minutes it produces both grids, `manifest.json` and
`panels.geojson`.

**CUJ 2: Compare methods locally.** Add `--make_cogs`, then run
`analyze_resample_grids`. This produces 18 reference COGs (9 methods × 2
data types), `stats.csv`, previews and per-panel contact sheets. For a visual
review, open them in QGIS with the `panels.geojson` labels.

**CUJ 3: GDAL upgrade regression.** Before and after a GDAL import, run the
integration test and `analyze_resample_grids --verify` on the full grid.
Review any assertion that changes.

**CUJ 4: Documentation (P2).** After brand review, select contact sheets for
the EE docs that explain each method.

--------------------------------------------------------------------------------

## 5. Grid Specification

### 5.1 Layout

*   Panels are placed row by row, **one pattern family per row** (rows 0–13).
    Each panel has a stable string ID, for example `sparse_posts_square`.
*   Pixel (0, 0) is the top-left corner. Panel (row r, column c) starts at
    x = 100 + 1124·c, y = 100 + 1124·r, and is 1024 × 1024 px.
*   Sub-tests inside a panel sit on a 128 px lattice (8 × 8 cells) relative to
    the panel origin, or on a 256 px lattice where they need more room. Their
    absolute block offset is the panel's offset (§2.2) plus the local
    offset; the manifest records it.
*   Guard, margin and empty-slot pixels are set to `guard_value` (default 0).
    This is valid data, not nodata.
*   Interpretation of "a guard region of 100 pixels between each group":
    *group* is read as *each panel*. There are **exactly 100 px** guards
    between neighboring panels and 100 px outer margins. The right and bottom
    margins are 104 px, so that W and H are multiples of 32 (§2.2).

### 5.2 Values, Data Types and Nodata

*   Each panel is first generated as a float64 array and clipped to
    [−1000, 1000].
    *   The **Float32** grid is that array cast to Float32.
    *   The **Int16** grid is that array rounded half away from zero.
*   Panels designed with integer values (categorical, lines, checkers, steps,
    posts, binary text, constants, probes) are **identical in both grids**.
    Continuous panels differ only by rounding: at most 0.5, plus Float32
    precision (below 1e-4 at |x| ≤ 1000). Rounding a value already clipped to
    [−1000, 1000] cannot leave that range, so the Int16 grid also stays within
    ±1000, far inside the Int16 limits (−32768…32767).
*   **Nodata**, with the band nodata tag set in both grids:
    *   **Int16: `−9999`.**
    *   **Float32: NaN.**
    *   Nodata pixels appear **only in the row 13 panels**.
    *   `--nodata=none` drops row 13 and both nodata tags. The grid is then
        6848 × 14720 px (14712 padded to a multiple of 32), still with five
        levels.
*   **Determinism**: the default seed is `559677208`. Each panel gets its own
    random generator seeded from (seed, panel ID), so adding or reordering
    panels never changes the other panels. The checksums hash raw bytes, so
    NaN pixels are compared exactly.
*   The ±1000 range applies to the **input** grids. CUBIC and LANCZOS overviews
    are expected to go beyond ±1000. That overshoot is one of the behaviors
    the grids are designed to reveal.

### 5.3 Georeferencing and File Format

*   CRS `EPSG:3857`, north-up, 10 m pixels, top-left corner at (0, 0). That
    places the grid in the open ocean of the Gulf of Guinea. All of these are
    configurable.
*   **Every TIFF the project writes uses ZSTD compression at level 22** (the
    maximum; GTiff `ZSTD_LEVEL=22`, COG `LEVEL=22`). That covers the base
    grids, the COG-driver references, the Landsat asset chip and test files.
    ZSTD is lossless, so the level changes file size and write time, never
    pixel values.
*   Base grids are GeoTIFFs, 512 × 512 tiles, ZSTD level 22 with predictor 2
    (Int16) or 3 (Float32), `BIGTIFF=IF_SAFER`, and **no overviews**.
*   The files record the seed, generator version, GDAL version and layout as
    metadata. They are named `resample_grid_int16.tif` and
    `resample_grid_float32.tif`.

### 5.4 Panel Catalog

Parameters are initial proposals. The manifest records the final values. All
sizes are in base-resolution pixels. "±1000" means +1000 and −1000.

#### Row 0: Calibration (new)

| ID | Content | Reveals |
| --- | --- | --- |
| `calib_constants` | Four 512 px quadrants: +1000, −1000, +333, −777. | Whether every method keeps a flat region's value; RMS sign loss (−777 becomes +777); Int16/Float32 parity; ringing at the quadrant seams. |
| `calib_full_pos` | The whole panel set to +1000, surrounded by the 0 guard. | How far each kernel bleeds into the guard at each level (isolation check). |
| `calib_rounding_int` | 128 px sub-cells tiled with 2×2 and 4×4 integer motifs whose block means are k + 0.5 or k ± 0.25 (k = −4…3). | The Float32 → Int16 rounding rule; rounding drift at 4× and coarser. |
| `calib_mode_ties` | 128 px sub-cells tiled with 2×2 motifs: 2–2 ties (row, column and diagonal arrangements), all-distinct, 3–1 majority. Also 4×4 motifs where cascaded and direct MODE disagree. | How MODE breaks ties; effects of computing levels one from the next. |
| `calib_impulse_pos` | 64 single +1000 pixels on 0, one per 128 px cell, at offset (i, j) with i, j ∈ 0…7. | The point-spread function against offset. NEAREST keeps 16 of the 64 at 2×, 4 at 4× and 1 at 8×. |
| `calib_impulse_neg` | The same with −1000. | Sign symmetry; RMS turns holes into posts. |

#### Row 1: Categorical / classified

| ID | Content | Reveals |
| --- | --- | --- |
| `cat_voronoi_large` | About 40 Voronoi cells (about 150 px across) with 8 class codes: {−1000, −750, −500, −250, 250, 500, 750, 1000}. | Classes preserved (MODE, NEAREST) versus invented codes at boundaries (AVERAGE and the convolution kernels). |
| `cat_voronoi_fine` | Voronoi cells 4–16 px across, same classes. | Behavior when classes are near or below the overview pixel size. |
| `cat_landcover_codes` | Smooth blobs coded with NLCD-style classes {11, 21, 22, 23, 24, 31, 41, 42, 43, 52, 71, 81, 82, 90, 95}. | Realistic code corruption, for example 53 (the mean of 11 and 95), which is not a real class. |
| `cat_thin_features` | A land-cover background with 1–5 px "roads" and "rivers", straight and curved, of a distinct class. | MODE deletes thin features; NEAREST breaks them into dashes. |
| `cat_majority_dither` | Four quadrants of class A with random class-B dither at 10/25/40/50 %. | Whether MODE keeps the majority (it flips near 50 %); NEAREST randomness; AVERAGE mixing. |
| `cat_edge_phase` | Two-class boundaries at offsets p = 0…7 px from the lattice, vertical and horizontal. | How the boundary shifts with its offset and the method. |

#### Row 2: High-frequency lines

| ID | Content | Reveals |
| --- | --- | --- |
| `lines_horizontal` | 1 px +1000 horizontal lines on −1000, at spacings of 2, 3, 4, 5, 8, 16 and 32 px (one band per spacing). | Lines vanishing or aliasing. AVERAGE turns them into intermediate values; whether NEAREST keeps a line depends on its offset. |
| `lines_vertical` | The same, transposed. | The same, for columns. |
| `lines_crosshatch` | Crossing horizontal and vertical 1 px lines at spacings of 4, 8, 16 and 32 px. | Behavior at intersections (doubled energy); 2D aliasing. |
| `lines_widths` | Isolated lines 1–8 px wide, both orientations, spaced 64 px apart. | The narrowest line that survives each level; ringing beside lines. |
| `lines_angled` | Aliased (Bresenham) 1 px lines at 5, 15, 30, 45, 60 and 75°. | Staircase aliasing; lines broken up by NEAREST. |
| `lines_crossing_diagonal` | Crossing 45° and 135° 1 px line grids at spacings of 4–32 px. | Aliasing of crossing diagonal lines. |

#### Row 3: Checkerboards and alternating stripes

| ID | Content | Reveals |
| --- | --- | --- |
| `checker_fine` | ±1000 checkerboards with cells of 1, 2, 3 and 4 px (one per quadrant). | A 1 px checker becomes 0 under AVERAGE, 1000 under RMS, and a constant ±1000 under NEAREST and MODE. The 3 px cells produce moiré. |
| `checker_coarse` | Cells of 5, 8, 16 and 32 px. | At which level checkers stop surviving; odd cell sizes that do not line up with the lattice. |
| `stripes_vertical` | ±1000 vertical stripes with periods of 2, 3, 4 and 6 px. | 1D aliasing; beat patterns from the 3 px period. |
| `stripes_horizontal` | The same, transposed. | The same, for rows. |
| `checker_offset` | 1 px checkers with value pairs {−1000, +1000}, {0, +1000}, {−1000, 0} and {+500, +1000}. | AVERAGE gives the mean; RMS loses the sign and is biased upward; MODE tie behavior. |
| `stripes_duty_cycle` | ±1000 stripes that are 1 px on / 3 px off and 1 px on / 7 px off. | AVERAGE gives fractional levels; NEAREST gives all or nothing, depending on offset. |

#### Row 4: Frequency response (new)

| ID | Content | Reveals |
| --- | --- | --- |
| `freq_zone_plate` | A ±1000 zone plate whose frequency rises linearly with radius and reaches the base Nyquist limit at r = 512 px. Pixels beyond r = 512 are set to 0. | Aliasing rings against frequency and orientation. |
| `freq_chirp_xy` | Top half: an x-chirp from a 64 px period down to 2 px. Bottom half: the same chirp in y. | The cutoff frequency and attenuation of each method. |
| `freq_siemens_star` | A binary ±1000 star with 36 cycles (72 spokes). | The resolution limit (as a radius) for each method and level. |
| `freq_sine_gratings` | 4 × 4 sub-cells of ±1000 sinusoids: periods {2.5, 3, 4, 5, 6, 8, 12, 16} px × orientations {0°, 90°}. | Amplitude kept at each period (the MTF table). |
| `freq_sine_gratings_oblique` | The same at 30°, 45° and 60°. | Anisotropy of separable kernels. |
| `freq_usaf1951` | USAF-1951-style groups of 3 bars, bar widths 1–16 px, both orientations. | Resolving power in a form people can read. |

#### Row 5: Sparse extreme features (posts and holes)

| ID | Content | Reveals |
| --- | --- | --- |
| `sparse_posts_square` | +1000 squares on 0, sizes {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33} px, each at offsets {0…7}. | Survival and amplitude against size and offset; ringing halos. |
| `sparse_holes_square` | −1000 squares, same layout. | Sign symmetry; RMS turns holes into posts. |
| `sparse_posts_disk` | +1000 disks, diameters 1–64 px. | Isotropy; rounded features. |
| `sparse_holes_disk` | −1000 disks, diameters 1–64 px. | The same, for holes. |
| `sparse_on_slope` | 1–16 px posts and holes on a ramp from −500 to +500. | Features on a sloped background; MODE on continuous data. |
| `sparse_dipoles` | A +1000 post next to a −1000 hole, separated by 0–16 px. | Cancellation under AVERAGE versus doubling under RMS. |

#### Row 6: Step functions / sharp edges (horizontal, vertical, both)

| ID | Content | Reveals |
| --- | --- | --- |
| `step_horizontal` | −1000 above, +1000 below, with the edge running horizontally on a lattice row. | Overshoot and undershoot beyond ±1000 (CUBIC, LANCZOS); the width of the edge blur. |
| `step_vertical` | −1000 on the left, +1000 on the right. | The same, for columns. |
| `step_both` | A +1000 square on −1000, plus a 2 × 2 quadrant checker. These give corners where edges in both directions meet. | 2D ringing at corners, where overshoot is largest. |
| `step_phase` | Eight strips with the edge at offset p = 0…7 px from the lattice: vertical edges in the top half, horizontal edges in the bottom half. | How overshoot and edge position depend on the edge's offset. |
| `step_bars` | Isolated +1000 bars on −1000, 1, 2, 3, 4, 6, 8, 12 and 16 px wide. | Ringing and survival of narrow features. |
| `step_small` | Steps from 0 up to h, for h = 1, 2, 3, 5, 10, 50 and 200. | Ringing relative to step height; ringing below one unit disappears in Int16. |

#### Row 7: Diagonal steps

| ID | Content | Reveals |
| --- | --- | --- |
| `diag_step_45` | An aliased ±1000 edge at 45°. | Diagonal ringing; staircase edges. |
| `diag_step_5deg` | An edge slanted at 5° (ISO 12233 style). | The edge-spread function at sub-pixel resolution. |
| `diag_step_30_60` | Two halves, with edges at 30° and 60°. | How behavior changes with angle. |
| `diag_diamond` | A +1000 diamond on −1000. This is the diagonal version of `step_both`. | Corners where diagonal edges meet. |
| `diag_bars_45` | 45° bars, 1–16 px wide. | Survival of narrow diagonal features. |
| `diag_edge_antialiased` | A 22.5° edge anti-aliased by area coverage, next to an aliased copy of the same edge. | Whether pre-smoothed input reduces ringing. |

#### Row 8: Saddles, ridges, mountains and valleys

| ID | Content | Reveals |
| --- | --- | --- |
| `terrain_mountains` | +1000 Gaussian peaks with σ = 1, 2, 4, 8, 16, 32, 64 and 150 px. | Whether peak height survives at each scale: AVERAGE flattens small peaks and NEAREST misses them. |
| `terrain_pits` | The same with −1000 (pits and valleys). | Sign symmetry; RMS turns pits into peaks. |
| `terrain_ridges_valleys` | Straight ridges (+) and valleys (−) with Gaussian cross-sections, σ = 1–32 px, at 0/30/60/90°. | Survival of linear features against width. |
| `terrain_saddle` | z = k(x² − y²), scaled to ±1000. | Fidelity on a smooth surface. |
| `terrain_monkey_saddle` | z = k(x³ − 3xy²), scaled to ±1000. | Fidelity on a smooth surface with higher-order curvature. |
| `terrain_peaks` | MATLAB `peaks`, scaled to ±1000. | CUBIC and LANCZOS should be nearly exact here; NEAREST and MODE look blocky. |

#### Row 9: Curved terraces

| ID | Content | Reveals |
| --- | --- | --- |
| `terrace_cone` | A cone cut into 20 circular terraces spanning ±1000. | Ringing at curved risers. |
| `terrace_peaks` | `peaks` cut into 100-unit terraces. | Terraces of irregular shape. |
| `terrace_spiral` | Archimedean spiral terraces. | Curved risers at every orientation. |
| `terrace_sinuous` | Meandering and elliptical terraces. | Risers of varying curvature. |
| `terrace_fine` | Circular terraces 2–4 px wide. | Moiré in data that looks continuous. |
| `terrace_soft_vs_sharp` | The same terraces with sharp risers and with smoothed (tanh) risers. | Ringing appears only at sharp risers. |

#### Row 10: Sloped regions

| ID | Content | Reveals |
| --- | --- | --- |
| `slope_x` | A linear ramp from −1000 to +1000 in x. | Symmetric kernels reproduce the ramp. NEAREST and MODE shift it by half a block. |
| `slope_y` | The same in y. | The same, for rows. |
| `slope_diagonal` | A ramp along x + y. | The same, diagonally. |
| `slope_gentle` | Ramps of 0.1 and 0.37 units/px that cross 0. | Int16 staircases; rounding bias around 0; MODE on repeated values. |
| `slope_sawtooth` | Sawtooth waves with periods of 8, 32 and 128 px. | A mix of ramps and cliffs. |
| `slope_cone_bowl` | A cone (+) and a bowl (−). | Kinks at the apex; RMS on the negative bowl. |

#### Row 11: Random noise

| ID | Content | Reveals |
| --- | --- | --- |
| `noise_uniform` | Independent uniform noise on [−1000, 1000]. | Variance reduction per method. |
| `noise_biased` | Quadrants: U(0, 1000), U(−1000, 0), N(+500, 150) and N(−500, 150), clipped. | AVERAGE keeps the mean; RMS is biased upward and ignores the sign. |
| `noise_gaussian` | N(0, 300), clipped to ±1000. | Behavior on a symmetric distribution with long tails. |
| `noise_perlin` | Quadrants of Perlin noise with feature scales of 8, 32 and 128 px, plus 6-octave fBm. | Smooth noise at several scales. |
| `noise_simplex` | The same, using simplex noise. | Isotropic smooth noise. |
| `noise_salt_pepper` | ±1000 impulses on 0 at 1/5/20/50 % density. | Robustness to outliers. |

#### Row 12: Imagery and text

| ID | Content | Reveals |
| --- | --- | --- |
| `img_cat` | Chelsea the cat (skimage). Grayscale, Lanczos-upscaled to fit, scaled to ±1000. | Natural-image appearance. |
| `img_text_google_binary` | "Google" in Roboto Bold at cap heights of 256, 128, 64, 32, 16 and 8 px. Binary: +1000 text on −1000. | The smallest legible text; halos around letters. |
| `img_text_google_aa` | Anti-aliased and inverted: −1000 text on +1000. | Whether pre-smoothed text rings less. |
| `img_ee_logo` | The Earth Engine logo (1000 × 1000 RGBA), converted to grayscale over 0. | Logo edges and fine detail. |
| `img_landsat` | A 1024 × 1024 chip of Landsat 8 OLI band 8 (15 m panchromatic) over Grand Bahama, stretched between the 2nd and 98th percentiles to ±1000. | Real satellite texture. |
| *(spare slot)* | Guard value, or the optional native-resolution cat or NAIP chip (P2). | — |

#### Row 13: Nodata patches

| ID | Content | Reveals |
| --- | --- | --- |
| `nodata_holes_on_ramp` | Square nodata holes of 1, 2, 3, 4, 8, 16, 32 and 64 px, at offsets 0–7, on an x-ramp. | Whether holes survive or erode. NEAREST keeps a hole only if it covers the sampled top-left pixel. |
| `nodata_random_masks` | A smooth field with 5/25/50/95 % random nodata. | Valid-fraction thresholds; the bias from averaging block means level by level. |
| `nodata_lines_checker` | 1 px nodata lines (horizontal and vertical) and a 1 px nodata checker over +500. | Whether thin nodata vanishes (mask-aware methods) or propagates (NEAREST, depending on offset). |
| `nodata_step_gaps` | Nodata strips 1–16 px wide between a −1000 half and a +1000 half. | Whether kernels bridge the gap; overshoot next to nodata. |
| `nodata_coastline` | A curved coastline: terrain "land" next to nodata "ocean". | Coastline halos (nodata, −9999 or NaN, leaking into valid pixels); erosion or dilation. |
| `nodata_categorical` | Nodata patches inside categorical data. | Whether MODE treats nodata as a class. |

### 5.5 Traceability to the Required Categories

| Required category | Panels |
| --- | --- |
| Categorical / classified patches | Row 1 (all six) |
| High-frequency lines: horizontal, vertical, crossing | Row 2 (all six) |
| High-frequency checkerboards / alternating stripes | Row 3 (all six) |
| Sparse extremes: + posts and − holes of varying size | Row 5 (all six) |
| Step / sharp-edge overshoot: horizontal, vertical, both | `step_horizontal`, `step_vertical`, `step_both` (plus the rest of row 6) |
| Saddles, ridges, mountains, valleys | Row 8 (all six) |
| Curved terraces | Row 9 (all six) |
| The same step function, diagonally | Row 7 (all six) |
| Sloped regions | Row 10 (all six) |
| Noise: uniform, biased, Perlin, simplex | `noise_uniform`, `noise_biased`, `noise_perlin`, `noise_simplex` (plus Gaussian and salt-and-pepper) |
| A cat picture | `img_cat` |
| The text "Google" | `img_text_google_binary`, `img_text_google_aa` |
| The Earth Engine logo | `img_ee_logo` |
| A grayscale satellite image | `img_landsat` |
| Patches of nodata | Row 13 (all six) |

--------------------------------------------------------------------------------

## 6. Expected GDAL Behavior (for QA)

### 6.1 GDAL COG Driver

*   Defaults: `BLOCKSIZE=512`; overview levels are added until both sides are
    ≤ BLOCKSIZE; `OVERVIEW_COUNT` and `OVERVIEWS=IGNORE_EXISTING` override
    that
    ([`cogdriver.cpp` L987–1024](gdal/frmts/gtiff/cogdriver.cpp#L987-L1024)).
*   `GAUSS` is not in the COG driver's documented `OVERVIEW_RESAMPLING` list.
    The option string is passed to the generic overview code, which does
    accept it
    ([`overview.cpp` L4611–4616](gdal/gcore/overview.cpp#L4611-L4616)).
    Phase 0 confirms this works.

### 6.2 Per-Method Expectations

**Basis** column:

*   **Source**: read directly from GDAL source at the linked lines.
*   **Expected**: derived from the kernel's shape; to be confirmed by
    `stats.csv` and `--verify`.

**Radius** comes from
[`GDALGetResampleFunction`](gdal/gcore/overview.cpp#L4601-L4651)
and is measured in destination pixels. Kernel types come from
[`gdalwarpkernel.cpp` L107–123](gdal/alg/gdalwarpkernel.cpp#L107-L123).

| Method | Kernel | Radius | Can exceed input range? | Can produce new values? | Signature in these grids | Basis |
| --- | --- | --- | --- | --- | --- | --- |
| `NEAREST` | Takes the top-left pixel of each 2^L block ([L125](gdal/gcore/overview.cpp#L125)) | 0 | No | No | Level L equals `base[::2^L, ::2^L]` exactly. Small features survive only when their absolute position is a multiple of 2^L. A 1 px checker becomes constant. Moiré on the zone plate. Ignores the nodata mask. | Source |
| `AVERAGE` | Box mean | 0 | No | Yes | A 1 px ±1000 checker becomes 0. Invents class codes. A 1 px post drops to 250 at 2× and 62.5 at 4×. Mask-aware. | Source |
| `RMS` | √(mean of x²) ([L291–306](gdal/gcore/overview.cpp#L291-L306)) | 0 | No (never exceeds the largest \|x\|) | Yes | **Loses the sign**: −777 becomes 777, holes become posts, a ±1000 checker becomes 1000. Never negative. Biased upward compared with AVERAGE. | Source |
| `BILINEAR` | Tent | 1 | No | Yes | Smooth; slightly blurrier than AVERAGE. | Expected |
| `CUBIC` | Catmull-Rom cubic convolution (has negative lobes) | 2 | **Yes** | Yes | Overshoot past ±1000 at steps. Negative rings around +1000 posts. EE's default for float data. | Expected |
| `CUBICSPLINE` | Cubic B-spline (no negative weights) | 2 | No | Yes | The blurriest of the convolution kernels. No ringing. | Expected |
| `LANCZOS` | Lanczos-3 windowed sinc | 3 | **Yes** (most) | Yes | The sharpest result with the strongest, multi-lobe ringing. The largest reach (§6.3). | Expected |
| `MODE` | Most frequent value in the block ([L2355–2626](gdal/gcore/overview.cpp#L2355-L2626)) | 0 | No | No | Keeps class codes; removes isolated and thin features. Ties go to the value that reaches the top count first in scan order. When all values in a block differ, that is the top-left pixel, so MODE **equals NEAREST on continuous Float32 data** (noise, terrain, imagery). | Source |
| `GAUSS` | Gaussian (no negative weights) | 1 | No | Yes | Smooth, similar to BILINEAR and CUBICSPLINE. Always computed in Float64. | Expected |

### 6.3 Cross-Cutting Behaviors

1.  **Each level is built from the previous one ("cascading").**
    *   The multi-band code computes each level from the previous level
        ([L5768–5778](gdal/gcore/overview.cpp#L5768-L5778)).
    *   The single-band code switches to that multi-band code when the raster
        is tiled and a work chunk is larger than 10 MB. Otherwise it may
        compute each level directly from the base
        ([L4995–5015](gdal/gcore/overview.cpp#L4995-L5015)).
    *   Consequences:
        *   Kernel reach grows to 2r(2^L − 1) base pixels instead of r·2^L
            (LANCZOS at 32×: 186 px instead of 96 px).
        *   With partial nodata, cascaded AVERAGE takes a plain mean of the
            previous level's means, not a mean weighted by valid-pixel count.
        *   Cascaded MODE differs from MODE computed over the full 2^L block.
        *   A small smoke-test grid may be computed differently from the full
            grid, so it is not a bit-exact stand-in for it.
2.  **Working data types**
    ([`GDALGetOvrWorkDataType`](gdal/gcore/overview.cpp#L4657-L4692)).
    *   Int16 input: AVERAGE, RMS, BILINEAR, CUBIC, CUBICSPLINE and LANCZOS
        are computed in **Float32** and converted back to Int16. GAUSS is
        computed in Float64. `calib_rounding_int` measures how that
        conversion rounds. (Unlike Byte and UInt16, Int16 has no integer-only
        fast path.)
    *   Float32 input: computed in Float32, except GAUSS (Float64).
    *   NEAREST and MODE always work in the input's data type.
3.  **Nodata.**
    *   NEAREST ignores the nodata mask and copies whatever pixel it samples
        ([L5672–5674](gdal/gcore/overview.cpp#L5672-L5674)).
        In Float32 that means NaN pixels are copied into the overview.
    *   The other methods skip nodata pixels. They output nodata only when
        every pixel in the block is nodata, unless `GDAL_OVR_PROPAGATE_NODATA`
        is set (default `NO`,
        [L5686–5687](gdal/gcore/overview.cpp#L5686-L5687)).
    *   Setting a nodata tag switches **all** non-NEAREST methods to the
        mask-aware code, even for panels that contain no nodata. Setting
        `--nodata=none` covers the case without a nodata tag.
    *   **Float32 NaN nodata.** With a NaN nodata tag, GDAL's nodata mask is
        expected to flag NaN pixels (NaN never equals itself, so this needs
        special handling). Phase 0 confirms it. MODE's value comparison
        already treats NaN as equal to NaN
        ([L2311–2321](gdal/gcore/overview.cpp#L2311-L2321)).
        Without the tag, NaN would spread through every averaging kernel. The
        analysis must compare values with `isnan` rather than `==`.
4.  **Output range.**
    *   CUBIC and LANCZOS results can fall outside ±1000. In Int16 they can
        also produce values that are not in the input's value set. Int16
        results are clamped only at the Int16 limits (−32768…32767), which
        overshoot from ±1000 input cannot reach.
    *   The analysis must not assume that the output range equals the input
        range.
5.  **How to read the frequency panels.** Each level halves the finest
    pattern it can show. A pattern with a period of P base pixels can be
    represented at level L only if P > 2^(L+1). An ideal filter removes
    anything finer. If a finer pattern shows up in an overview as a coarser
    one, that is aliasing.

--------------------------------------------------------------------------------

## 7. Requirements

### P0: Minimum Viable Product

*   **P0-1**: The absl CLI `make_resample_grids.py` writes both grids to
    `--output_dir`. That flag is required.
*   **P0-2**: The layout follows §5.1: panel size, guard, margin and column
    count are configurable. The script widens the right and bottom margins so
    that W and H are multiples of 2^overview_count, and records every panel's
    offset inside the 2^L blocks.
*   **P0-3**: All 83 panels in §5.4, built from a data-driven catalog.
*   **P0-4**: The value contract in §5.2: every valid pixel within ±1000;
    nodata (Int16 −9999, Float32 NaN) only in row 13; guard pixels exactly
    `guard_value`; identical output for the same seed and library versions.
*   **P0-5**: `manifest.json` (grid and per-panel metadata, block offsets,
    parameters, sub-regions, expected behaviors) and `panels.geojson` (panel
    and sub-region polygons in WGS84, per RFC 7946).
*   **P0-6**: `--make_cogs` builds COGs for the 9 methods × 2 data types with
    the COG driver: `OVERVIEW_RESAMPLING`, `OVERVIEW_COUNT=5`,
    `BLOCKSIZE=512`, `OVERVIEWS=IGNORE_EXISTING`, `COMPRESS=ZSTD`,
    `LEVEL=22`. The manifest marks which
    COG matches EE's default when no method is given (CUBIC for Float32,
    NEAREST for Int16).
*   **P0-7**: Built and tested against `//third_party/py/osgeo`
    with unit tests and a reduced-size integration test.
*   **P0-8**: A README covering usage, outputs, disk and time budget, and
    asset provenance.

### P1: Analysis

*   **P1-1**: `stats.csv` with one row per data type × method × level × panel
    (and sub-region). Columns: min, max, mean, std, valid count, nodata count,
    count above +1000, count below −1000, number of distinct values (capped).
    NaN counts as nodata in Float32.
*   **P1-2**: Previews and contact sheets.
    *   Full-grid PNG previews of the overview levels.
    *   Per-panel contact sheets: methods × levels.
    *   Colors: grayscale for [−1000, 1000], red above +1000, blue below
        −1000, magenta for nodata.
*   **P1-3**: `--verify` runs the behavior assertions from the manifest. It
    writes a JSON report and exits non-zero on failure. Use it after GDAL
    upgrades.
*   **P1-4**: A free-disk-space check before writing; `--methods`,
    `--dtypes` and `--panels` to run subsets.
*   **P1-5**: NumPy reference implementations of NEAREST, AVERAGE, RMS and
    MODE, both direct and cascaded. They show which strategy GDAL actually
    used.

### P2: Extensions

*   **P2-1**: Additional data types: UInt8, UInt16, Int32, Float64.
*   **P2-2**: Optional panels: native-resolution cat, NAIP chip, 1/f noise.
*   **P2-3**: Figures for the EE documentation, after brand review.

--------------------------------------------------------------------------------

## 8. Validation & Success Metrics

| Metric | Target |
| --- | --- |
| Coverage | All 15 required categories map to at least one panel (§5.5), and every ★ addition is included. |
| Value contract | 100 % of valid pixels are within ±1000; nodata appears only in row 13; guard pixels equal `guard_value`. Checked automatically. |
| Determinism | Two runs with the same seed give identical pixel checksums (SHA-256 of the raw arrays). |
| COG validity | Every COG passes [`validate_cloud_optimized_geotiff.py`](gdal/swig/python/samples/validate_cloud_optimized_geotiff.py) and has exactly 5 overviews with the §2.2 dimensions. |
| Behavior | `--verify` passes every Source-basis assertion. Expected-basis results are recorded. |
| Performance (targets, to be measured) | With ZSTD level 22 on every TIFF: base grids in under 5 minutes; the full `--make_cogs` run plus analysis in under 30 minutes on a 24-core workstation. |

--------------------------------------------------------------------------------

## 9. Risks & Mitigations

| Risk | Mitigation |
| --- | --- |
| With 100 px guards, panels start at different offsets inside the 8×–32× blocks, which can confound comparisons between panels | The manifest records each panel's offset at every level; expectations are computed from absolute coordinates; the analysis uses only overview pixels fully inside a panel. |
| Small test grids are computed with a different strategy than the full grid (§6.3) | Tests force the strategy with `GDAL_OVR_CHUNK_MAX_SIZE` (confirmed in Phase 0); final checks always use the full grid. |
| NaN nodata: `==` comparisons silently fail, and NaN spreads if the tag is missing | Tests and analysis use `isnan`-aware comparisons; Phase 0 confirms that GDAL masks NaN when the tag is NaN. |
| Disk space: the workstation has about 45 GB free (96 % used) | Worst case, uncompressed, is about 9 GB for everything (PLAN §13). The script checks free space first and supports subsets. |
| GDAL version skew (workstation Python GDAL 3.13.2; GDAL closest to v3.12.0beta1) | Always run through blaze with `//third_party/py/osgeo`; record the GDAL version in every output. |
| ZSTD level 22 is the slowest level and uses a large window and match tables. libtiff does not tell zstd the tile size ([`tif_zstd.c` L213](tiff/libtiff/tif_zstd.c#L213)), so each compressing thread may need hundreds of MB | Phase 0 measures write time and peak RSS per job; lower `--jobs` / `--num_threads` if memory is tight. Level 22 stays fixed. |
| Asset licensing and brand | The cat is CC0 and is used from `//third_party/py/skimage` without copying. Landsat is USGS public domain. Roboto is Apache-2.0. The EE logo is Google-owned. Internal use only; brand review before any external publication. |
| The upscaled cat is smooth, so it says little about fine detail | Documented; optional native-resolution panel (P2-2). |

--------------------------------------------------------------------------------

## 10. Decisions & Open Questions

### 10.1 Decisions

| # | Decision | Choice | Status | Alternative |
| --- | --- | --- | --- | --- |
| D1 | Guard width | **Literal 100 px** guards and margins; right and bottom margins widened to 104 px (6848 × 15840) | ✅ Decided (user) | 128 px guard (7040 × 16256, every panel block-aligned) |
| D2 | How to package the code (the package path contains `-` and names that start with digits, so it cannot be imported normally) | **Two self-contained binaries** (`make_resample_grids.py`, `analyze_resample_grids.py`); tests load them with `importlib` | Proposed | Move the code to a path without hyphens; or several modules loaded with `importlib` |
| D3 | Asset sourcing | **Depend on `//third_party/py/skimage`** for the cat (no third-party copy). **Copy** the EE logo and a 1024² Landsat chip into `assets/`, with `ASSETS.md` provenance | Proposed | Copy the cat too (lighter build, but needs a third-party policy check) |
| D4 | Satellite image | **Landsat 8 band 8 (15 m panchromatic), Grand Bahama** (USGS public domain) | Proposed | NAIP 2 m RGB, Louisiana |
| D5 | Nodata | **Int16: −9999. Float32: NaN** | ✅ Decided (user) | −9999 for both |
| D6 | Data types | **Int16 and Float32** (EE's `int16` and `float`) | ✅ Decided (user) | Int32 or Float64 (P2-1) |
| D7 | Local reference path | **GDAL COG driver only** | ✅ Decided (user) | — |
| D8 | Georeferencing | **EPSG:3857, origin (0, 0), 10 m** | Proposed | UTM or EPSG:4326 |
| D9 | TIFF compression | **ZSTD level 22 for every TIFF** (base grids, COGs, asset chip, test files) | ✅ Decided (user) | — |

### 10.2 Open Questions

1.  Should UInt8/UInt16 grids (common for classifications and DNs) be added
    before launch?
2.  Will the EE logo or "Google" text be shown outside Google? If so, brand
    review is required.
