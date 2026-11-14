r"""Analyzes the overview resampling test grids and their reference COGs.

Reads the output directory of make_resample_grids (manifest.json, the base
grids and cog/<dtype>/resample_grid_<dtype>_<METHOD>.tif) and writes, into
--output_dir (default: <input_dir>/analysis):

  stats.csv           One row per dtype x method x level x panel/subregion.
  previews/*.png      Full-grid previews of overview levels >= 2.
  contact/*.png       Per-panel contact sheets: methods x levels.
  verify_report.json  With --verify: the manifest's behavior assertions.

Colors: grayscale for [-1000, 1000], red above +1000, blue below -1000 and
magenta for nodata (-9999 in Int16, NaN in Float32).
"""

from collections.abc import Callable, Iterator, Sequence
import csv
import dataclasses
import functools
import json
import math
import os
import re
import sys
import time
from typing import Any

from absl import app
from absl import flags
from absl import logging
import numpy as np
from osgeo import gdal
import PIL.Image
import PIL.ImageDraw
import PIL.ImageFont

import resources

gdal.UseExceptions()

VALUE_LIMIT = 1000.0
DISTINCT_CAP = 4096
FONT_RESOURCE = "third_party/roboto/unhinted/Roboto-Bold.ttf"
METHODS = (
    "NEAREST",
    "AVERAGE",
    "BILINEAR",
    "CUBIC",
    "CUBICSPLINE",
    "LANCZOS",
    "MODE",
    "RMS",
    "GAUSS",
)
METHOD_RADIUS = {
    "NEAREST": 0,
    "AVERAGE": 0,
    "RMS": 0,
    "MODE": 0,
    "BILINEAR": 1,
    "GAUSS": 1,
    "CUBIC": 2,
    "CUBICSPLINE": 2,
    "LANCZOS": 3,
}
DTYPES = ("int16", "float32")
ROUNDING_RULES = ("half_away", "half_even", "trunc")
STATS_FIELDS = (
    "dtype",
    "method",
    "level",
    "panel_id",
    "subregion_id",
    "min",
    "max",
    "mean",
    "std",
    "valid_count",
    "nodata_count",
    "count_gt_1000",
    "count_lt_minus_1000",
    "distinct_count",
)

# Flags are defined only when run as a binary, so the integration test can
# load this module next to make_resample_grids (both have --output_dir).
_FLAGS: dict[str, Any] = {}


def define_flags() -> None:
  """Defines the command-line flags (binary only)."""
  _FLAGS["input_dir"] = flags.DEFINE_string(
      "input_dir", None, "Directory written by make_resample_grids."
  )
  _FLAGS["output_dir"] = flags.DEFINE_string(
      "output_dir",
      None,
      "Where to write results (default <input_dir>/analysis).",
  )
  _FLAGS["stats"] = flags.DEFINE_bool("stats", True, "Write stats.csv.")
  _FLAGS["previews"] = flags.DEFINE_bool(
      "previews", True, "Write full-grid previews."
  )
  _FLAGS["contact_sheets"] = flags.DEFINE_bool(
      "contact_sheets", True, "Write per-panel contact sheets."
  )
  _FLAGS["verify"] = flags.DEFINE_bool(
      "verify", False, "Evaluate manifest expectations; exit 1 on failure."
  )
  _FLAGS["rounding"] = flags.DEFINE_enum(
      "rounding",
      "auto",
      ["auto", *ROUNDING_RULES],
      "Int16 rounding for the numpy references.",
  )
  _FLAGS["panels"] = flags.DEFINE_list(
      "panels", [], "Panel IDs or rows (default all)."
  )
  _FLAGS["methods"] = flags.DEFINE_list("methods", [], "Methods (default all).")
  _FLAGS["dtypes"] = flags.DEFINE_list(
      "dtypes", [], "Data types (default all)."
  )


# ---------------------------------------------------------------------------
# Nodata, rounding
# ---------------------------------------------------------------------------


def nodata_from_manifest(manifest: dict[str, Any], dtype: str) -> float | None:
  value = manifest["grid"]["nodata"].get(dtype)
  if value is None:
    return None
  if value == "nan":
    return float("nan")
  return float(value)


def nodata_mask(arr: np.ndarray, nodata: float | None) -> np.ndarray:
  """True where arr is nodata. NaN is always nodata in float arrays."""
  if np.issubdtype(arr.dtype, np.floating):
    mask = np.isnan(arr)
  else:
    mask = np.zeros(arr.shape, dtype=bool)
  if nodata is not None and not math.isnan(nodata):
    mask |= arr == nodata
  return mask


def round_int(values: np.ndarray, rule: str) -> np.ndarray:
  if rule == "half_away":
    return np.sign(values) * np.floor(np.abs(values) + 0.5)
  if rule == "half_even":
    return np.round(values)
  if rule == "trunc":
    return np.trunc(values)
  raise ValueError(f"Unknown rounding rule {rule}")


# ---------------------------------------------------------------------------
# Numpy reference implementations (absolute coordinates)
# ---------------------------------------------------------------------------


def nearest(base: np.ndarray, level: int) -> np.ndarray:
  f = 2**level
  return base[::f, ::f]


def _blocks(a: np.ndarray, f: int) -> np.ndarray:
  """(h/f, w/f, f*f) view of f x f blocks in row-major (scan) order."""
  h, w = a.shape[0] // f * f, a.shape[1] // f * f
  b = a[:h, :w].reshape(h // f, f, w // f, f).transpose(0, 2, 1, 3)
  return b.reshape(h // f, w // f, f * f)


def _reduce(
    values: np.ndarray, mask: np.ndarray, f: int, kind: str
) -> tuple[np.ndarray, np.ndarray]:
  """Nodata-aware box mean (kind=average) or RMS of f x f blocks."""
  v = _blocks(values, f)
  m = _blocks(mask, f)
  valid = ~m
  count = valid.sum(axis=-1)
  x = np.where(valid, v, 0.0)
  if kind == "rms":
    x = x * x
  total = x.sum(axis=-1)
  with np.errstate(invalid="ignore", divide="ignore"):
    mean = total / count
  if kind == "rms":
    mean = np.sqrt(mean)
  out_mask = count == 0
  return np.where(out_mask, 0.0, mean), out_mask


def box_reference(
    base: np.ndarray,
    level: int,
    kind: str,
    strategy: str = "cascaded",
    dtype: str = "float32",
    rounding: str = "half_away",
    nodata: float | None = None,
) -> tuple[np.ndarray, np.ndarray]:
  """AVERAGE or RMS reference: (values, nodata mask) at `level`.

  Cascaded: 2 x 2 reductions per level, rounded to Int16 (or cast to
  Float32) after every level, as each stored level is. Direct: one 2^L x 2^L
  reduction.
  """
  mask = nodata_mask(base, nodata)
  values = np.where(mask, 0.0, base.astype(np.float64))

  def store(v: np.ndarray) -> np.ndarray:
    if dtype == "int16":
      return round_int(v, rounding)
    return v.astype(np.float32).astype(np.float64)

  if strategy == "direct":
    v, m = _reduce(values, mask, 2**level, kind)
    return store(v), m
  for _ in range(level):
    values, mask = _reduce(values, mask, 2, kind)
    values = store(values)
  return values, mask


def mode_blocks(
    blocks: np.ndarray, valid: np.ndarray | None = None
) -> tuple[np.ndarray, np.ndarray]:
  """MODE over the last axis; ties go to the first value to reach the top.

  Returns (values, nodata mask). NaN compares equal to NaN. Invalid entries
  are ignored; a block with no valid entries is nodata.
  """
  k = blocks.shape[-1]
  if valid is None:
    valid = np.ones(blocks.shape, dtype=bool)
  a = blocks[..., :, None]
  b = blocks[..., None, :]
  eq = a == b
  if np.issubdtype(blocks.dtype, np.floating):
    eq |= np.isnan(a) & np.isnan(b)
  eq &= valid[..., :, None] & valid[..., None, :]
  total = eq.sum(axis=-1)
  lower = np.tril(np.ones((k, k), dtype=bool))
  prefix = (eq & lower).sum(axis=-1)
  top = total.max(axis=-1)
  cand = (prefix == top[..., None]) & valid
  idx = np.argmax(cand, axis=-1)
  values = np.take_along_axis(blocks, idx[..., None], axis=-1)[..., 0]
  return values, ~valid.any(axis=-1)


def mode_reference(
    base: np.ndarray,
    level: int,
    strategy: str = "cascaded",
    nodata: float | None = None,
) -> tuple[np.ndarray, np.ndarray]:
  mask = nodata_mask(base, nodata)
  if strategy == "direct":
    return mode_blocks(_blocks(base, 2**level), ~_blocks(mask, 2**level))
  values = base
  for _ in range(level):
    values, mask = mode_blocks(_blocks(values, 2), ~_blocks(mask, 2))
  return values, mask


# ---------------------------------------------------------------------------
# Windows
# ---------------------------------------------------------------------------

Window = tuple[int, int, int, int]  # x_off, y_off, x_size, y_size


def reach(method: str, level: int) -> int:
  """Cascaded kernel reach 2r(2^L - 1) in overview pixels at `level`."""
  f = 2**level
  return -(-2 * METHOD_RADIUS.get(method, 0) * (f - 1) // f)


def panel_window(panel: dict[str, Any], level: int) -> Window:
  if level == 0:
    return (panel["x"], panel["y"], panel["width"], panel["height"])
  xs, ys, w, h = panel["inner_window"][str(level)]
  return (xs, ys, w, h)


def region_window(
    panel: dict[str, Any],
    sub: dict[str, Any] | None,
    level: int,
    shrink: int = 0,
) -> Window | None:
  """Overview pixels whose footprint lies inside the subregion (or panel)."""
  f = 2**level
  if sub is None:
    ax, ay, w, h = panel["x"], panel["y"], panel["width"], panel["height"]
  else:
    ax, ay = panel["x"] + sub["x"], panel["y"] + sub["y"]
    w, h = sub["w"], sub["h"]
  xs, ys = -(-ax // f) + shrink, -(-ay // f) + shrink
  xe, ye = (ax + w) // f - shrink, (ay + h) // f - shrink
  px, py, pw, ph = panel_window(panel, level)
  xs, ys = max(xs, px), max(ys, py)
  xe, ye = min(xe, px + pw), min(ye, py + ph)
  if xe <= xs or ye <= ys:
    return None
  return (xs, ys, xe - xs, ye - ys)


def crop(arr: np.ndarray, window: Window) -> np.ndarray:
  x, y, w, h = window
  return arr[y : y + h, x : x + w]


# ---------------------------------------------------------------------------
# Inputs
# ---------------------------------------------------------------------------


def load_manifest(input_dir: str) -> dict[str, Any]:
  with open(os.path.join(input_dir, "manifest.json")) as f:
    return json.load(f)


_COG_RE = re.compile(
    r"(int16|float32)_([A-Za-z_]+?)(?:\.tif|\.tiff)$", re.IGNORECASE
)


def discover_cogs(
    root: str, manifest: dict[str, Any] | None = None
) -> dict[tuple[str, str], str]:
  """Maps (dtype, GDAL method) to a COG path under root.

  Files are matched by `<dtype>_<method>` at the end of the name; EE enum
  names (for example CUBIC_SPLINE) are mapped through the manifest's methods
  table.
  """
  table = dict((manifest or {}).get("methods", {}))
  table.pop("ee_default", None)
  out = {}
  for dirpath, _, names in os.walk(root):
    for name in sorted(names):
      m = _COG_RE.search(name)
      if not m:
        continue
      dtype, method = m.group(1).lower(), m.group(2).upper()
      method = table.get(method, method)
      if method in METHODS:
        out[(dtype, method)] = os.path.join(dirpath, name)
  return out


class Raster:
  """Caches full-level reads of one GeoTIFF / COG."""

  def __init__(self, path: str):
    self.path = path
    self._levels: dict[int, np.ndarray] = {}

  def level(self, level: int) -> np.ndarray:
    if level not in self._levels:
      ds = gdal.Open(self.path)
      band = ds.GetRasterBand(1)
      if level:
        band = band.GetOverview(level - 1)
      self._levels[level] = band.ReadAsArray()
      ds = None
    return self._levels[level]

  def drop(self) -> None:
    self._levels.clear()


def selected(
    manifest: dict[str, Any], panels: Sequence[str]
) -> list[dict[str, Any]]:
  wanted = set(panels)
  return [
      p
      for p in manifest["panels"]
      if not wanted or p["id"] in wanted or str(p["row"]) in wanted
  ]


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------


def region_stats(arr: np.ndarray, nodata: float | None) -> dict[str, Any]:
  mask = nodata_mask(arr, nodata)
  valid = arr[~mask].astype(np.float64)
  out: dict[str, Any] = {
      "valid_count": int(valid.size),
      "nodata_count": int(mask.sum()),
      "count_gt_1000": int((valid > VALUE_LIMIT).sum()),
      "count_lt_minus_1000": int((valid < -VALUE_LIMIT).sum()),
  }
  if valid.size:
    out.update({
        "min": float(valid.min()),
        "max": float(valid.max()),
        "mean": float(valid.mean()),
        "std": float(valid.std()),
        "distinct_count": int(min(np.unique(valid).size, DISTINCT_CAP)),
    })
  else:
    out.update(
        {"min": "", "max": "", "mean": "", "std": "", "distinct_count": 0}
    )
  return out


def panel_stats_rows(
    arr: np.ndarray,
    panel: dict[str, Any],
    level: int,
    nodata: float | None,
    dtype: str,
    method: str,
) -> Iterator[dict[str, Any]]:
  regions = [("", None)] + [(s["id"], s) for s in panel["subregions"]]
  for sub_id, sub in regions:
    window = region_window(panel, sub, level)
    if window is None:
      continue
    row = {
        "dtype": dtype,
        "method": method,
        "level": level,
        "panel_id": panel["id"],
        "subregion_id": sub_id,
    }
    row.update(region_stats(crop(arr, window), nodata))
    yield row


# ---------------------------------------------------------------------------
# Colormap, previews and contact sheets
# ---------------------------------------------------------------------------


def colormap(arr: np.ndarray, nodata: float | None) -> np.ndarray:
  """RGB uint8: gray in [-1000, 1000], red above, blue below, magenta nodata."""
  mask = nodata_mask(arr, nodata)
  v = np.where(mask, 0.0, arr.astype(np.float64))
  gray = np.clip((v + VALUE_LIMIT) / (2 * VALUE_LIMIT) * 255, 0, 255)
  rgb = np.repeat(gray[..., None], 3, axis=-1)
  over = np.clip((v - VALUE_LIMIT) / 200.0, 0, 1)
  under = np.clip((-VALUE_LIMIT - v) / 200.0, 0, 1)
  hi = v > VALUE_LIMIT
  lo = v < -VALUE_LIMIT
  rgb[hi] = np.stack(
      [np.full(hi.sum(), 255.0), 160 * (1 - over[hi]), 160 * (1 - over[hi])],
      axis=-1,
  )
  rgb[lo] = np.stack(
      [160 * (1 - under[lo]), 160 * (1 - under[lo]), np.full(lo.sum(), 255.0)],
      axis=-1,
  )
  rgb[mask] = (255, 0, 255)
  return rgb.astype(np.uint8)


@functools.cache
def label_font(
    size: int = 20,
) -> PIL.ImageFont.ImageFont | PIL.ImageFont.FreeTypeFont:
  try:
    return PIL.ImageFont.truetype(
        resources.GetResourceFilename(FONT_RESOURCE), size
    )
  except (OSError, IOError, ValueError):
    return PIL.ImageFont.load_default()


def contact_sheet(
    tiles: dict[tuple[str, int], np.ndarray],
    methods: Sequence[str],
    levels: Sequence[int],
    nodata: float | None,
    title: str,
    tile_px: int = 256,
) -> PIL.Image.Image:
  """Grid of methods (rows) x levels (columns), NEAREST-upscaled tiles."""
  label_w, label_h = 150, 60
  sheet = PIL.Image.new(
      "RGB",
      (
          label_w + len(levels) * (tile_px + 4),
          label_h + len(methods) * (tile_px + 4),
      ),
      (255, 255, 255),
  )
  draw = PIL.ImageDraw.Draw(sheet)
  font = label_font(18)
  draw.text((4, 4), title, fill=(0, 0, 0), font=font)
  for j, level in enumerate(levels):
    draw.text(
        (label_w + j * (tile_px + 4) + 4, 32),
        "base" if level == 0 else f"L{level} ({2**level}x)",
        fill=(0, 0, 0),
        font=font,
    )
  for i, method in enumerate(methods):
    y = label_h + i * (tile_px + 4)
    draw.text((4, y + tile_px // 2 - 10), method, fill=(0, 0, 0), font=font)
    for j, level in enumerate(levels):
      arr = tiles.get((method, level))
      if arr is None or not arr.size:
        continue
      img = PIL.Image.fromarray(colormap(arr, nodata))
      img = img.resize((tile_px, tile_px), PIL.Image.Resampling.NEAREST)
      sheet.paste(img, (label_w + j * (tile_px + 4), y))
  return sheet


# ---------------------------------------------------------------------------
# Expectations
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class Context:
  """What the expectation evaluator needs for one dtype."""

  manifest: dict[str, Any]
  dtype: str
  nodata: float | None
  base: Raster
  cogs: dict[str, Raster]
  rounding: str = "half_away"

  @functools.cached_property
  def _ref_cache(
      self,
  ) -> dict[tuple[str, str, int], tuple[np.ndarray, np.ndarray]]:
    return {}

  def reference(
      self, kind: str, strategy: str, level: int
  ) -> tuple[np.ndarray, np.ndarray]:
    """Global reference grid (values, nodata mask) at `level`."""
    key = (kind, strategy, level)
    cache = self._ref_cache
    if key not in cache:
      base = self.base.level(0)
      if kind == "nearest":
        v = nearest(base, level)
        cache[key] = (v, nodata_mask(v, self.nodata))
      elif kind in ("average", "rms"):
        cache[key] = box_reference(
            base, level, kind, strategy, self.dtype, self.rounding, self.nodata
        )
      else:
        raise ValueError(f"No global reference for {kind}")
    return cache[key]


def _valid(arr: np.ndarray, nodata: float | None) -> np.ndarray:
  return arr[~nodata_mask(arr, nodata)].astype(np.float64)


def evaluate(
    exp: dict[str, Any],
    panel: dict[str, Any],
    ctx: Context,
    method: str,
    level: int,
) -> tuple[bool | None, str]:
  """Evaluates one expectation. Returns (passed or None if skipped, detail)."""
  raster = ctx.cogs[method]
  arr = raster.level(level)
  sub = None
  if exp.get("subregion"):
    subs = {s["id"]: s for s in panel["subregions"]}
    sub = subs.get(exp["subregion"])
    if sub is None:
      return None, "subregion not generated"
  check = exp["check"]
  shrink = 0 if check == "exceeds" else reach(method, level)
  window = region_window(panel, sub, level, shrink)
  if window is None:
    return None, "empty interior"
  tile = crop(arr, window)
  tol = float(exp.get("tolerance", 0.0)) + 1e-6
  valid = _valid(tile, ctx.nodata)
  if check == "interior_equals":
    if not valid.size:
      return None, "no valid pixels"
    err = float(np.max(np.abs(valid - exp["value"])))
    return err <= tol, f"max |v - {exp['value']}| = {err:g}"
  if check == "interior_range":
    if not valid.size:
      return None, "no valid pixels"
    lo, hi = float(valid.min()), float(valid.max())
    return (
        lo >= exp["lo"] - tol and hi <= exp["hi"] + tol,
        f"range [{lo:g}, {hi:g}]",
    )
  if check == "exceeds":
    if not valid.size:
      return None, "no valid pixels"
    lo, hi = float(valid.min()), float(valid.max())
    return lo < exp["lo"] or hi > exp["hi"], f"range [{lo:g}, {hi:g}]"
  if check == "nonnegative":
    if not valid.size:
      return None, "no valid pixels"
    return float(valid.min()) >= 0, f"min {float(valid.min()):g}"
  if check == "nodata_fraction":
    frac = float(nodata_mask(tile, ctx.nodata).mean())
    return exp["lo"] <= frac <= exp["hi"], f"nodata fraction {frac:.4f}"
  if check == "count_equals":
    want = (exp.get("count") or {}).get(str(level))
    if want is None:
      return None, "no count for level"
    got = int(np.sum(np.abs(valid - exp["value"]) <= tol))
    return got == want, f"count {got}, want {want}"
  if check == "values_subset_of_input":
    src = crop(ctx.base.level(0), panel_window(panel, 0))
    src_mask = nodata_mask(src, ctx.nodata)
    out_mask = nodata_mask(tile, ctx.nodata)
    new = ~np.isin(tile[~out_mask], np.unique(src[~src_mask]))
    bad_nodata = bool(out_mask.any() and not src_mask.any())
    n_new = int(new.sum())
    return (
        n_new == 0 and not bad_nodata,
        f"{n_new} new values" + (", new nodata" if bad_nodata else ""),
    )
  if check == "equals_reference":
    kind = exp.get("reference", "nearest")
    strategy = exp.get("strategy") or "cascaded"
    if kind == "mode":
      f = 2**level
      x, y, w, h = window
      src = ctx.base.level(0)[y * f : (y + h) * f, x * f : (x + w) * f]
      ref, ref_mask = mode_reference(src, level, strategy, ctx.nodata)
    else:
      ref_full, mask_full = ctx.reference(kind, strategy, level)
      ref, ref_mask = crop(ref_full, window), crop(mask_full, window)
    out_mask = nodata_mask(tile, ctx.nodata)
    both = ~out_mask & ~ref_mask
    diff = np.abs(tile.astype(np.float64)[both] - ref.astype(np.float64)[both])
    n_bad = int((diff > tol).sum()) + int((out_mask != ref_mask).sum())
    max_diff = float(diff.max()) if diff.size else 0.0
    return n_bad == 0, (
        f"{n_bad} px differ from {strategy} {kind}, max diff {max_diff:g}"
    )
  raise ValueError(f"Unknown check {check}")


def verify(
    panels: Sequence[dict[str, Any]],
    ctx: Context,
    methods: Sequence[str],
) -> list[dict[str, Any]]:
  results = []
  max_level = len(ctx.manifest["grid"]["overview_factors"])
  for panel in panels:
    for exp in panel["expectations"]:
      if ctx.dtype not in exp.get("dtypes", DTYPES):
        continue
      for method in exp["methods"]:
        if method not in methods or method not in ctx.cogs:
          continue
        for level in exp["levels"]:
          if level > max_level:
            continue
          passed, detail = evaluate(exp, panel, ctx, method, level)
          results.append({
              "panel": panel["id"],
              "id": exp["id"],
              "check": exp["check"],
              "dtype": ctx.dtype,
              "method": method,
              "level": level,
              "basis": exp["basis"],
              "status": (
                  "skipped" if passed is None else "pass" if passed else "fail"
              ),
              "detail": detail,
          })
  return results


def summarize(results: Sequence[dict[str, Any]]) -> dict[str, Any]:
  summary: dict[str, Any] = {}
  for basis in ("source", "expected"):
    rows = [r for r in results if r["basis"] == basis]
    summary[basis] = {
        s: sum(1 for r in rows if r["status"] == s)
        for s in ("pass", "fail", "skipped")
    }
  return summary


def measure_rounding(
    manifest: dict[str, Any], average_int16: Raster | None
) -> str:
  """Infers the Int16 rounding rule from calib_rounding_int (AVERAGE L1)."""
  panels = {p["id"]: p for p in manifest["panels"]}
  panel = panels.get("calib_rounding_int")
  if panel is None or average_int16 is None:
    return "half_away"
  arr = average_int16.level(1)
  matches = set(ROUNDING_RULES)
  for sub in panel["subregions"]:
    m = re.fullmatch(r"half_k(m?)(\d+)", sub["id"])
    if not m:
      continue
    k = -int(m.group(2)) if m.group(1) else int(m.group(2))
    window = region_window(panel, sub, 1)
    if window is None:
      continue
    got = float(np.median(crop(arr, window)))
    for rule in list(matches):
      if float(round_int(np.array(k + 0.5), rule)) != got:
        matches.discard(rule)
  for rule in ROUNDING_RULES:
    if rule in matches:
      return rule
  return "half_away"


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class AnalysisConfig:
  input_dir: str
  output_dir: str
  stats: bool = True
  previews: bool = True
  contact_sheets: bool = True
  verify: bool = False
  rounding: str = "auto"
  panels: tuple[str, ...] = ()
  methods: tuple[str, ...] = ()
  dtypes: tuple[str, ...] = ()


def _write_csv(
    path: str, fields: Sequence[str], rows: Sequence[dict[str, Any]]
) -> None:
  with open(path, "w", newline="") as f:
    writer = csv.DictWriter(f, fieldnames=list(fields))
    writer.writeheader()
    writer.writerows(rows)


def analyze(
    config: AnalysisConfig, log: Callable[[str], None] = logging.info
) -> dict[str, Any]:
  """Runs the selected analyses. Returns a summary (with verify results)."""
  manifest = load_manifest(config.input_dir)
  os.makedirs(config.output_dir, exist_ok=True)
  panels = selected(manifest, config.panels)
  cog_paths = discover_cogs(os.path.join(config.input_dir, "cog"), manifest)
  dtypes = [d for d in config.dtypes or DTYPES if d in manifest["files"]]
  methods = [
      m
      for m in config.methods or METHODS
      if any((d, m) in cog_paths for d in dtypes)
  ]
  levels = list(range(1, len(manifest["grid"]["overview_factors"]) + 1))
  stats_rows: list[dict[str, Any]] = []
  verify_rows: list[dict[str, Any]] = []
  summary: dict[str, Any] = {"dtypes": dtypes, "methods": methods}
  for dtype in dtypes:
    start = time.time()
    nodata = nodata_from_manifest(manifest, dtype)
    base = Raster(
        os.path.join(config.input_dir, manifest["files"][dtype]["path"])
    )
    cogs = {
        m: Raster(cog_paths[(dtype, m)])
        for m in methods
        if (dtype, m) in cog_paths
    }
    rounding = config.rounding
    if rounding == "auto":
      rounding = (
          measure_rounding(manifest, cogs.get("AVERAGE"))
          if dtype == "int16"
          else "half_away"
      )
    summary[f"rounding_{dtype}"] = rounding
    ctx = Context(manifest, dtype, nodata, base, cogs, rounding)
    if config.stats:
      for panel in panels:
        stats_rows.extend(
            panel_stats_rows(base.level(0), panel, 0, nodata, dtype, "base")
        )
    if config.verify:
      verify_rows.extend(verify(panels, ctx, methods))
    for method, raster in cogs.items():
      tiles_levels = levels
      for level in tiles_levels:
        arr = raster.level(level)
        if config.stats:
          for panel in panels:
            stats_rows.extend(
                panel_stats_rows(arr, panel, level, nodata, dtype, method)
            )
        if config.previews and level >= 2:
          prev_dir = os.path.join(config.output_dir, "previews")
          os.makedirs(prev_dir, exist_ok=True)
          PIL.Image.fromarray(colormap(arr, nodata)).save(
              os.path.join(prev_dir, f"{dtype}_{method}_L{level}.png")
          )
    if config.contact_sheets:
      sheet_dir = os.path.join(config.output_dir, "contact")
      os.makedirs(sheet_dir, exist_ok=True)
      for panel in panels:
        tiles = {}
        for method, raster in cogs.items():
          tiles[(method, 0)] = crop(base.level(0), panel_window(panel, 0))
          for level in levels:
            tiles[(method, level)] = crop(
                raster.level(level), panel_window(panel, level)
            )
        off = panel["block_offset"][str(levels[-1])]
        title = (
            f"{panel['id']} ({dtype})  row {panel['row']} col "
            f"{panel['col']}  offset mod {2**levels[-1]}: {off}"
        )
        contact_sheet(tiles, list(cogs), [0] + levels, nodata, title).save(
            os.path.join(sheet_dir, f"{panel['id']}_{dtype}.png")
        )
    for raster in cogs.values():
      raster.drop()
    base.drop()
    log(f"{dtype}: analyzed in {time.time() - start:.1f} s")
  if config.stats:
    _write_csv(
        os.path.join(config.output_dir, "stats.csv"), STATS_FIELDS, stats_rows
    )
  if config.verify:
    report = {
        "summary": summarize(verify_rows),
        "results": verify_rows,
        "rounding": {d: summary.get(f"rounding_{d}") for d in dtypes},
    }
    with open(os.path.join(config.output_dir, "verify_report.json"), "w") as f:
      json.dump(report, f, indent=1)
    summary["verify"] = report["summary"]
    summary["verify_failures"] = [
        r
        for r in verify_rows
        if r["status"] == "fail" and r["basis"] == "source"
    ]
  return summary


def main(argv: Sequence[str]) -> None:
  if len(argv) > 1:
    raise app.UsageError("Too many command-line arguments.")
  if not _FLAGS["input_dir"].value:
    raise app.UsageError("--input_dir is required")
  config = AnalysisConfig(
      input_dir=_FLAGS["input_dir"].value,
      output_dir=_FLAGS["output_dir"].value
      or os.path.join(_FLAGS["input_dir"].value, "analysis"),
      stats=_FLAGS["stats"].value,
      previews=_FLAGS["previews"].value,
      contact_sheets=_FLAGS["contact_sheets"].value,
      verify=_FLAGS["verify"].value,
      rounding=_FLAGS["rounding"].value,
      panels=tuple(_FLAGS["panels"].value),
      methods=tuple(m.upper() for m in _FLAGS["methods"].value),
      dtypes=tuple(d.lower() for d in _FLAGS["dtypes"].value),
  )
  start = time.time()
  summary = analyze(config)
  failures = summary.pop("verify_failures", [])
  summary["seconds"] = round(time.time() - start, 1)
  summary["source_failures_sample"] = failures[:20]
  print(json.dumps(summary, indent=1))
  if failures:
    sys.exit(1)


if __name__ == "__main__":
  define_flags()
  app.run(main)
