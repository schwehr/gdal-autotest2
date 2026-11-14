r"""Generates Int16 and Float32 test grids for GDAL overview resampling.

The grids hold 83 panels (PRD.md section 5.4), one pattern family per row,
separated by 100 px guards. Each panel is designed to show how one of the
GDAL COG driver OVERVIEW_RESAMPLING methods (NEAREST, AVERAGE, BILINEAR,
CUBIC, CUBICSPLINE, LANCZOS, MODE, RMS, GAUSS) behaves.

Outputs, written to --output_dir:

  resample_grid_int16.tif, resample_grid_float32.tif  Base grids, no overviews.
  manifest.json                                        Grid and panel metadata.
  panels.geojson                                       Panel outlines in WGS84.
  cog/<dtype>/resample_grid_<dtype>_<METHOD>.tif       With --make_cogs.

Every TIFF is written with ZSTD compression at level 22.

"""

from collections.abc import Callable, Iterable, Sequence
import concurrent.futures
import dataclasses
import functools
import hashlib
import json
import math
import os
import resource
import shutil
import sys
import time
from typing import Any
import zlib

from absl import app
from absl import flags
from absl import logging
import numpy as np
from osgeo import gdal
from osgeo import osr
import PIL.Image
import PIL.ImageDraw
import PIL.ImageFont
import scipy.ndimage
import scipy.spatial

import resources

gdal.UseExceptions()

VERSION = "0.1.0"
DEFAULT_SEED = 559677208
VALUE_LIMIT = 1000.0

# Every TIFF this project writes uses ZSTD at level 22 (PRD D9).
COMPRESS = "ZSTD"
ZSTD_LEVEL = 22

# GDAL OVERVIEW_RESAMPLING names, in PRD order.
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

# Earth Engine TiffPyramidOptions.ResamplingMethod enum -> GDAL name.
EE_TO_GDAL = {
    "NEAREST": "NEAREST",
    "AVERAGE": "AVERAGE",
    "BILINEAR": "BILINEAR",
    "CUBIC": "CUBIC",
    "CUBIC_SPLINE": "CUBICSPLINE",
    "LANCZOS": "LANCZOS",
    "MODE": "MODE",
    "RMS": "RMS",
    "GAUSS": "GAUSS",
}

# Default when resamplingMethod is RESAMPLING_METHOD_UNSPECIFIED,
EE_DEFAULT = {"int16": "NEAREST", "float32": "CUBIC"}

# Kernel radius in destination pixels (GDALGetResampleFunction).
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
GDAL_TYPES = {"int16": gdal.GDT_Int16, "float32": gdal.GDT_Float32}
NUMPY_TYPES = {"int16": np.int16, "float32": np.float32}
PREDICTORS = {"int16": "2", "float32": "3"}
BYTES_PER_PIXEL = {"int16": 2, "float32": 4}

ROWS = 14
NODATA_ROW = 13

CHECKS = (
    "interior_equals",
    "interior_range",
    "exceeds",
    "values_subset_of_input",
    "nonnegative",
    "equals_reference",
    "nodata_fraction",
    "count_equals",
)

FAMILIES = {
    0: "calibration",
    1: "categorical",
    2: "lines",
    3: "checkers",
    4: "frequency",
    5: "sparse",
    6: "steps",
    7: "diagonal",
    8: "terrain",
    9: "terraces",
    10: "slopes",
    11: "noise",
    12: "imagery",
    13: "nodata",
}

FONT_RESOURCE = "third_party/roboto/unhinted/Roboto-Bold.ttf"
CAT_RESOURCE = "third_party/py/skimage/data/chelsea.png"
PACKAGE = (
    "example-grid"
)
LOGO_RESOURCE = PACKAGE + "/assets/ee_logo.png"
SATELLITE_RESOURCE = PACKAGE + "/assets/landsat_b8_grand_bahama_1024.tif"

_OUTPUT_DIR = flags.DEFINE_string(
    "output_dir", None, "Output directory."
)
_DTYPES = flags.DEFINE_list("dtypes", list(DTYPES), "Grids to write.")
_PANEL_PX = flags.DEFINE_integer("panel_px", 1024, "Panel size in pixels.")
_GUARD_PX = flags.DEFINE_integer("guard_px", 100, "Guard between panels.")
_MARGIN_PX = flags.DEFINE_integer("margin_px", 100, "Outer margin.")
_COLUMNS = flags.DEFINE_integer("columns", 6, "Panels per row (6 or 12).")
_SMALL = flags.DEFINE_bool(
    "small", False, "Shortcut for --panel_px=256 (smoke tests)."
)
_SEED = flags.DEFINE_integer("seed", DEFAULT_SEED, "Master seed.")
_NODATA = flags.DEFINE_enum(
    "nodata", "on", ["on", "none"], "'none' drops row 13 and nodata tags."
)
_INT16_NODATA = flags.DEFINE_integer("int16_nodata", -9999, "Int16 nodata.")
_FLOAT32_NODATA = flags.DEFINE_string(
    "float32_nodata", "nan", "Float32 nodata: 'nan' or a number."
)
_GUARD_VALUE = flags.DEFINE_float("guard_value", 0.0, "Guard pixel value.")
_CRS = flags.DEFINE_string("crs", "EPSG:3857", "Output CRS.")
_PIXEL_SIZE = flags.DEFINE_float("pixel_size", 10.0, "Pixel size (CRS units).")
_ORIGIN = flags.DEFINE_list("origin", ["0", "0"], "Top-left corner x,y.")
_PANELS = flags.DEFINE_list(
    "panels", [], "Panel IDs or row numbers to generate (default: all)."
)
_MAKE_COGS = flags.DEFINE_bool(
    "make_cogs", False, "Build COG-driver references."
)
_METHODS = flags.DEFINE_list("methods", list(METHODS), "Methods for COGs.")
_COG_BLOCKSIZE = flags.DEFINE_integer("cog_blocksize", 512, "COG BLOCKSIZE.")
_OVERVIEW_COUNT = flags.DEFINE_integer("overview_count", 5, "Overview levels.")
_JOBS = flags.DEFINE_integer("jobs", 4, "COGs built in parallel.")
_NUM_THREADS = flags.DEFINE_integer("num_threads", 4, "GDAL NUM_THREADS.")
_FONT_PATH = flags.DEFINE_string("font_path", None, "Roboto Bold override.")
_LOGO_PATH = flags.DEFINE_string("logo_path", None, "EE logo PNG override.")
_SATELLITE_PATH = flags.DEFINE_string(
    "satellite_path", None, "Landsat chip override."
)
_FORCE = flags.DEFINE_bool("force", False, "Skip the free-disk check.")


def tiff_compression_options(driver: str) -> list[str]:
  """Returns the creation options for ZSTD level 22 for GTiff or COG."""
  if driver == "GTiff":
    return [f"COMPRESS={COMPRESS}", f"ZSTD_LEVEL={ZSTD_LEVEL}"]
  if driver == "COG":
    return [f"COMPRESS={COMPRESS}", f"LEVEL={ZSTD_LEVEL}"]
  raise ValueError(f"Unsupported driver: {driver}")


def ee_to_gdal(name: str) -> str:
  """Maps an EE ResamplingMethod enum name (or GDAL name) to GDAL's."""
  key = name.upper()
  if key in EE_TO_GDAL:
    return EE_TO_GDAL[key]
  if key in METHODS:
    return key
  raise ValueError(f"Unknown resampling method: {name}")


def parse_float32_nodata(text: str) -> float:
  value = float(text)
  if not math.isnan(value) and abs(value) <= VALUE_LIMIT:
    raise ValueError("--float32_nodata must be nan or outside +/-1000")
  return value


# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------


def _round_up(value: int, multiple: int) -> int:
  return -(-value // multiple) * multiple


@dataclasses.dataclass(frozen=True)
class Layout:
  """Panel placement in the grid (PLAN section 4)."""

  panel_px: int = 1024
  guard_px: int = 100
  margin_px: int = 100
  columns: int = 6
  rows: int = ROWS
  overview_count: int = 5

  @property
  def pitch(self) -> int:
    return self.panel_px + self.guard_px

  @property
  def align(self) -> int:
    return 2**self.overview_count

  @property
  def raw_width(self) -> int:
    return (
        2 * self.margin_px
        + self.columns * self.panel_px
        + (self.columns - 1) * self.guard_px
    )

  @property
  def raw_height(self) -> int:
    return (
        2 * self.margin_px
        + self.rows * self.panel_px
        + (self.rows - 1) * self.guard_px
    )

  @property
  def width(self) -> int:
    return _round_up(self.raw_width, self.align)

  @property
  def height(self) -> int:
    return _round_up(self.raw_height, self.align)

  def panel_origin(self, row: int, col: int) -> tuple[int, int]:
    return (
        self.margin_px + col * self.pitch,
        self.margin_px + row * self.pitch,
    )

  def block_offset(self, row: int, col: int, level: int) -> tuple[int, int]:
    x0, y0 = self.panel_origin(row, col)
    factor = 2**level
    return (x0 % factor, y0 % factor)

  def inner_window(
      self, row: int, col: int, level: int
  ) -> tuple[int, int, int, int]:
    """Overview pixels whose 2^L footprint is inside the panel."""
    x0, y0 = self.panel_origin(row, col)
    factor = 2**level
    xs = -(-x0 // factor)
    ys = -(-y0 // factor)
    xe = (x0 + self.panel_px) // factor
    ye = (y0 + self.panel_px) // factor
    return (xs, ys, xe - xs, ye - ys)

  def overview_shape(self, level: int) -> tuple[int, int]:
    return (self.width >> level, self.height >> level)

  def validate(self) -> None:
    if self.width % self.align or self.height % self.align:
      raise ValueError("Grid size is not a multiple of 2^overview_count")
    if self.panel_px < 64 or self.panel_px % 8:
      raise ValueError("panel_px must be a multiple of 8 and >= 64")
    if self.guard_px < 0 or self.margin_px < 0:
      raise ValueError("guard and margin must be >= 0")
    if self.columns not in (6, 12):
      raise ValueError("columns must be 6 or 12")


def slot_for(row: int, col: int, columns: int) -> tuple[int, int]:
  """Maps a catalog (row, col) to a grid slot. 12 columns pack row pairs."""
  if columns == 6:
    return row, col
  return row // 2, col + 6 * (row % 2)


def rows_for(columns: int, nodata: bool) -> int:
  catalog_rows = ROWS if nodata else ROWS - 1
  return catalog_rows if columns == 6 else -(-catalog_rows // 2)


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Subregion:
  """A rectangle inside a panel, in panel-relative pixels."""

  id: str
  x: int
  y: int
  w: int
  h: int
  params: dict[str, Any] = dataclasses.field(default_factory=dict)

  def to_json(self) -> dict[str, Any]:
    out: dict[str, Any] = {
        "id": self.id,
        "x": self.x,
        "y": self.y,
        "w": self.w,
        "h": self.h,
    }
    if self.params:
      out["params"] = self.params
    return out


@dataclasses.dataclass(frozen=True)
class Expectation:
  """A behavior assertion evaluated by analyze_resample_grids --verify."""

  id: str
  check: str
  methods: tuple[str, ...]
  levels: tuple[int, ...] = (1, 2, 3, 4, 5)
  subregion: str | None = None
  dtypes: tuple[str, ...] = DTYPES
  value: float | None = None
  lo: float | None = None
  hi: float | None = None
  tolerance: float = 0.0
  count: dict[str, int] | None = None
  reference: str | None = None
  strategy: str | None = None
  basis: str = "source"

  def __post_init__(self) -> None:
    if self.check not in CHECKS:
      raise ValueError(f"Unknown check {self.check}")
    if self.basis not in ("source", "expected"):
      raise ValueError(f"Unknown basis {self.basis}")

  def to_json(self) -> dict[str, Any]:
    out: dict[str, Any] = {
        "id": self.id,
        "check": self.check,
        "methods": list(self.methods),
        "levels": list(self.levels),
        "dtypes": list(self.dtypes),
        "tolerance": self.tolerance,
        "basis": self.basis,
    }
    for key in (
        "subregion",
        "value",
        "lo",
        "hi",
        "count",
        "reference",
        "strategy",
    ):
      val = getattr(self, key)
      if val is not None:
        out[key] = val
    return out


@dataclasses.dataclass
class PanelResult:
  values: np.ndarray
  nodata_mask: np.ndarray | None = None
  params: dict[str, Any] = dataclasses.field(default_factory=dict)
  subregions: list[Subregion] = dataclasses.field(default_factory=list)
  expectations: list[Expectation] = dataclasses.field(default_factory=list)


class Assets:
  """Lazily loaded external inputs (font, cat, logo, satellite chip)."""

  def __init__(
      self,
      font_path: str | None = None,
      logo_path: str | None = None,
      satellite_path: str | None = None,
      cat_path: str | None = None,
  ):
    self._font_path = font_path
    self._logo_path = logo_path
    self._satellite_path = satellite_path
    self._cat_path = cat_path

  @staticmethod
  def _resource(path: str | None, resource_name: str) -> str:
    if path:
      return path
    return resources.GetResourceFilename(resource_name)

  @functools.cached_property
  def font_path(self) -> str:
    return self._resource(self._font_path, FONT_RESOURCE)

  def font(self, size: int) -> PIL.ImageFont.FreeTypeFont:
    return PIL.ImageFont.truetype(self.font_path, size)

  @functools.cached_property
  def cat(self) -> np.ndarray:
    """Chelsea the cat (skimage, CC0) as 8-bit grayscale."""
    path = self._resource(self._cat_path, CAT_RESOURCE)
    with PIL.Image.open(path) as img:
      return np.asarray(img.convert("L"), dtype=np.float64)

  @functools.cached_property
  def logo(self) -> np.ndarray:
    """EE logo as float RGBA in [0, 255]."""
    path = self._resource(self._logo_path, LOGO_RESOURCE)
    with PIL.Image.open(path) as img:
      return np.asarray(img.convert("RGBA"), dtype=np.float64)

  @functools.cached_property
  def satellite(self) -> np.ndarray:
    path = self._resource(self._satellite_path, SATELLITE_RESOURCE)
    ds = gdal.Open(path)
    try:
      return ds.GetRasterBand(1).ReadAsArray().astype(np.float64)
    finally:
      ds = None


@dataclasses.dataclass(frozen=True)
class PanelContext:
  size: int
  origin: tuple[int, int]
  rng: np.random.Generator
  assets: Assets
  overview_count: int = 5


@dataclasses.dataclass(frozen=True)
class PanelSpec:
  panel_id: str
  row: int
  col: int
  family: str
  generator: Callable[[PanelContext], PanelResult]
  designed_integer: bool
  uses_nodata: bool
  label: str


PANELS: dict[str, PanelSpec] = {}


def register_panel(
    panel_id: str,
    row: int,
    col: int,
    *,
    designed_integer: bool,
    label: str,
) -> Callable[
    [Callable[[PanelContext], PanelResult]],
    Callable[[PanelContext], PanelResult],
]:
  """Decorator that adds a panel generator to PANELS."""

  def decorator(
      fn: Callable[[PanelContext], PanelResult],
  ) -> Callable[[PanelContext], PanelResult]:
    if panel_id in PANELS:
      raise ValueError(f"Duplicate panel {panel_id}")
    for spec in PANELS.values():
      if (spec.row, spec.col) == (row, col):
        raise ValueError(f"Slot {(row, col)} used by {spec.panel_id}")
    PANELS[panel_id] = PanelSpec(
        panel_id=panel_id,
        row=row,
        col=col,
        family=FAMILIES[row],
        generator=fn,
        designed_integer=designed_integer,
        uses_nodata=row == NODATA_ROW,
        label=label,
    )
    return fn

  return decorator


def panel_rng(seed: int, panel_id: str) -> np.random.Generator:
  seq = np.random.SeedSequence([seed, zlib.crc32(panel_id.encode("utf-8"))])
  return np.random.Generator(np.random.PCG64(seq))


def make_context(
    spec: PanelSpec,
    layout: Layout,
    seed: int,
    assets: Assets,
    origin: tuple[int, int] | None = None,
) -> PanelContext:
  if origin is None:
    row, col = slot_for(spec.row, spec.col, layout.columns)
    origin = layout.panel_origin(row, col)
  return PanelContext(
      size=layout.panel_px,
      origin=origin,
      rng=panel_rng(seed, spec.panel_id),
      assets=assets,
      overview_count=layout.overview_count,
  )


def run_panel(spec: PanelSpec, ctx: PanelContext) -> PanelResult:
  """Runs a generator and applies the shared checks and post-processing."""
  result = spec.generator(ctx)
  values = np.asarray(result.values, dtype=np.float64)
  if values.shape != (ctx.size, ctx.size):
    raise ValueError(f"{spec.panel_id}: shape {values.shape}")
  mask = result.nodata_mask
  if mask is not None:
    if not spec.uses_nodata:
      raise ValueError(f"{spec.panel_id}: nodata outside row {NODATA_ROW}")
    mask = np.asarray(mask, dtype=bool)
    values = np.where(mask, 0.0, values)
  if not np.all(np.isfinite(values)):
    raise ValueError(f"{spec.panel_id}: non-finite values")
  values = np.clip(values, -VALUE_LIMIT, VALUE_LIMIT)
  if spec.designed_integer and not np.all(values == np.round(values)):
    raise ValueError(f"{spec.panel_id}: designed-integer panel has fractions")
  result.values = values
  result.nodata_mask = mask
  result.expectations = standard_expectations(spec) + result.expectations
  return result


def round_half_away(values: np.ndarray) -> np.ndarray:
  return np.sign(values) * np.floor(np.abs(values) + 0.5)


def to_dtype(
    values: np.ndarray,
    mask: np.ndarray | None,
    dtype: str,
    int16_nodata: int = -9999,
    float32_nodata: float = float("nan"),
) -> np.ndarray:
  """Converts clipped float64 panel values to the grid data type."""
  if dtype == "int16":
    out = round_half_away(values).astype(np.int16)
    if mask is not None:
      out[mask] = int16_nodata
    return out
  if dtype == "float32":
    out = values.astype(np.float32)
    if mask is not None:
      out[mask] = np.float32(float32_nodata)
    return out
  raise ValueError(dtype)


def standard_expectations(spec: PanelSpec) -> list[Expectation]:
  """Assertions that apply to every panel (read from GDAL source)."""
  # Phase 0: MODE never outputs NaN, even for all-nodata blocks, so on the
  # nodata row its "no new values" property is only an expectation.
  mode_basis = "expected" if spec.uses_nodata else "source"
  return [
      Expectation(
          id="nearest_is_stride",
          check="equals_reference",
          methods=("NEAREST",),
          reference="nearest",
      ),
      Expectation(
          id="mode_no_new_values",
          check="values_subset_of_input",
          methods=("MODE",),
          basis=mode_basis,
      ),
      Expectation(
          id="nearest_no_new_values",
          check="values_subset_of_input",
          methods=("NEAREST",),
      ),
      Expectation(
          id="rms_nonnegative",
          check="nonnegative",
          methods=("RMS",),
      ),
      Expectation(
          id="average_within_input_range",
          check="interior_range",
          methods=("AVERAGE",),
          lo=-VALUE_LIMIT,
          hi=VALUE_LIMIT,
          tolerance=1e-3,
      ),
      Expectation(
          id="average_cascaded_reference",
          check="equals_reference",
          methods=("AVERAGE",),
          reference="average",
          strategy="cascaded",
          tolerance=1e-3,
      ),
  ]


# ---------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------


def grid_xy(n: int, m: int | None = None) -> tuple[np.ndarray, np.ndarray]:
  """Integer pixel indices (x, y) for an m x n (rows x cols) array."""
  m = n if m is None else m
  y, x = np.mgrid[0:m, 0:n]
  return x, y


def centers(n: int, m: int | None = None) -> tuple[np.ndarray, np.ndarray]:
  """Pixel-center coordinates (x + 0.5, y + 0.5)."""
  x, y = grid_xy(n, m)
  return x + 0.5, y + 0.5


def pm(mask: np.ndarray, hi: float = 1000.0, lo: float = -1000.0) -> np.ndarray:
  return np.where(mask, hi, lo).astype(np.float64)


def quadrants(n: int) -> list[tuple[int, int, int, int]]:
  """(x, y, w, h) of the four quadrants: TL, TR, BL, BR."""
  h = n // 2
  return [
      (0, 0, h, h),
      (h, 0, n - h, h),
      (0, h, h, n - h),
      (h, h, n - h, n - h),
  ]


def normalize(values: np.ndarray, percentile: float = 99.9) -> np.ndarray:
  """Scales values to about +/-1000 by a high percentile of |x|."""
  centered = values - np.mean(values)
  scale = np.percentile(np.abs(centered), percentile)
  if scale <= 0:
    return np.zeros_like(values)
  return np.clip(centered / scale * VALUE_LIMIT, -VALUE_LIMIT, VALUE_LIMIT)


def draw_mask(
    n: int,
    draw_fn: Callable[[PIL.ImageDraw.ImageDraw], None],
    m: int | None = None,
) -> np.ndarray:
  """Rasterizes PIL drawing calls (fill=255) into a boolean mask."""
  img = PIL.Image.new("L", (n, n if m is None else m), 0)
  draw_fn(PIL.ImageDraw.Draw(img))
  return np.asarray(img) > 127


def peaks_function(n: int) -> np.ndarray:
  """MATLAB peaks on [-3, 3]^2, sampled at pixel centers."""
  cx, cy = centers(n)
  x = cx / n * 6.0 - 3.0
  y = cy / n * 6.0 - 3.0
  return (
      3 * (1 - x) ** 2 * np.exp(-(x**2) - (y + 1) ** 2)
      - 10 * (x / 5 - x**3 - y**5) * np.exp(-(x**2) - y**2)
      - 1.0 / 3.0 * np.exp(-((x + 1) ** 2) - y**2)
  )


def scaled_peaks(n: int) -> np.ndarray:
  z = peaks_function(n)
  return z * (VALUE_LIMIT / np.max(np.abs(z)))


def nearest_survivors(
    points: Iterable[tuple[int, int]],
    origin: tuple[int, int],
    levels: Iterable[int],
) -> dict[str, int]:
  """Counts local points whose absolute position NEAREST samples at level L."""
  pts = list(points)
  out = {}
  for level in levels:
    f = 2**level
    out[str(level)] = sum(
        1
        for x, y in pts
        if (origin[0] + x) % f == 0 and (origin[1] + y) % f == 0
    )
  return out


def levels_for(ctx: PanelContext) -> tuple[int, ...]:
  return tuple(range(1, ctx.overview_count + 1))


KERNEL_METHODS = ("BILINEAR", "CUBIC", "CUBICSPLINE", "LANCZOS", "GAUSS")


def _res(
    values: np.ndarray,
    params: dict[str, Any] | None = None,
    subregions: list[Subregion] | None = None,
    expectations: list[Expectation] | None = None,
    mask: np.ndarray | None = None,
) -> PanelResult:
  return PanelResult(
      values=np.asarray(values, dtype=np.float64),
      nodata_mask=mask,
      params=params or {},
      subregions=subregions or [],
      expectations=expectations or [],
  )


def flat_expectations(sub_id: str, value: float) -> list[Expectation]:
  """A flat region keeps its value (RMS keeps |value|)."""
  out = [
      Expectation(
          id=f"{sub_id}_flat_{m.lower()}",
          check="interior_equals",
          methods=(m,),
          subregion=sub_id,
          value=value,
      )
      for m in ("NEAREST", "AVERAGE", "MODE")
  ]
  out.append(
      Expectation(
          id=f"{sub_id}_flat_rms",
          check="interior_equals",
          methods=("RMS",),
          subregion=sub_id,
          value=abs(value),
      )
  )
  out.append(
      Expectation(
          id=f"{sub_id}_flat_kernels",
          check="interior_equals",
          methods=KERNEL_METHODS,
          subregion=sub_id,
          value=value,
          tolerance=0.5,
          basis="expected",
      )
  )
  return out


def overshoot_expectations() -> list[Expectation]:
  return [
      Expectation(
          id="cubic_lanczos_overshoot",
          check="exceeds",
          methods=("CUBIC", "LANCZOS"),
          levels=(1, 2),
          lo=-VALUE_LIMIT,
          hi=VALUE_LIMIT,
          basis="expected",
      ),
      Expectation(
          id="cubicspline_no_overshoot",
          check="interior_range",
          methods=("CUBICSPLINE", "BILINEAR", "GAUSS"),
          lo=-VALUE_LIMIT,
          hi=VALUE_LIMIT,
          tolerance=0.5,
          basis="expected",
      ),
  ]


def _cells(n: int, count: int = 8) -> int:
  return n // count


def _fill_rect(
    arr: np.ndarray, x: int, y: int, w: int, h: int, value: float
) -> None:
  arr[max(y, 0) : max(y + h, 0), max(x, 0) : max(x + w, 0)] = value


def _cascade_average(
    tile: np.ndarray, levels: int, integer: bool
) -> list[np.ndarray]:
  """Cascaded 2x2 box means of a periodic tile (numpy reference)."""
  out = []
  cur = np.asarray(tile, dtype=np.float64)
  for _ in range(levels):
    cur = cur.reshape(cur.shape[0] // 2, 2, cur.shape[1] // 2, 2).mean(
        axis=(1, 3)
    )
    if integer:
      cur = round_half_away(cur)
    else:
      cur = cur.astype(np.float32).astype(np.float64)
    out.append(cur)
  return out


# ---------------------------------------------------------------------------
# Row 0: calibration
# ---------------------------------------------------------------------------

CALIB_VALUES = (1000, -1000, 333, -777)


@register_panel(
    "calib_constants",
    0,
    0,
    designed_integer=True,
    label="Constants +1000/-1000/+333/-777",
)
def _calib_constants(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  subs, exps = [], []
  names = ("q_pos1000", "q_neg1000", "q_pos333", "q_neg777")
  for (x, y, w, h), val, name in zip(quadrants(n), CALIB_VALUES, names):
    v[y : y + h, x : x + w] = val
    subs.append(Subregion(name, x, y, w, h, {"value": val}))
    exps += flat_expectations(name, float(val))
  exps.append(
      Expectation(
          id="rms_sign_loss",
          check="interior_equals",
          methods=("RMS",),
          subregion="q_neg777",
          value=777.0,
      )
  )
  return _res(v, {"values": list(CALIB_VALUES)}, subs, exps)


@register_panel(
    "calib_full_pos", 0, 1, designed_integer=True, label="Full panel +1000"
)
def _calib_full_pos(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  subs = [Subregion("all", 0, 0, n, n)]
  return _res(
      np.full((n, n), 1000.0),
      {"value": 1000},
      subs,
      flat_expectations("all", 1000.0),
  )


def _rounding_motifs(k: int) -> dict[str, np.ndarray]:
  half = np.array([[k, k], [k + 1, k + 1]])
  quarter = np.array([[k, k], [k, k + 1]])
  three = np.array([[k, k + 1], [k + 1, k + 1]])
  m4 = np.array([[k] * 4, [k + 1] * 4, [k] * 4, [k] * 4])
  return {
      "half": half,
      "quarter": quarter,
      "three_quarter": three,
      "half_t": half.T,
      "quarter_t": quarter.T,
      "three_quarter_t": three.T,
      "m4_casc": m4,
      "m4_casc_t": m4.T,
  }


def _kname(k: int) -> str:
  return f"k{k}" if k >= 0 else f"km{-k}"


@register_panel(
    "calib_rounding_int",
    0,
    2,
    designed_integer=True,
    label="Int16 rounding probes",
)
def _calib_rounding_int(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  c = _cells(n)
  v = np.zeros((n, n))
  subs, exps = [], []
  table = {}
  for col, k in enumerate(range(-4, 4)):
    for row, (name, motif) in enumerate(_rounding_motifs(k).items()):
      tile = np.tile(motif, (4 // motif.shape[0], 4 // motif.shape[1]))
      v[row * c : (row + 1) * c, col * c : (col + 1) * c] = np.tile(
          tile, (c // 4, c // 4)
      )
      sid = f"{name}_{_kname(k)}"
      subs.append(
          Subregion(sid, col * c, row * c, c, c, {"motif": motif.tolist()})
      )
      for dtype, integer in (("int16", True), ("float32", False)):
        levels = _cascade_average(tile, 2, integer)
        for level, arr in enumerate(levels, start=1):
          if np.all(arr == arr.flat[0]):
            exps.append(
                Expectation(
                    id=f"{sid}_avg_l{level}_{dtype}",
                    check="interior_equals",
                    methods=("AVERAGE",),
                    levels=(level,),
                    dtypes=(dtype,),
                    subregion=sid,
                    value=float(arr.flat[0]),
                    tolerance=0.0 if integer else 1e-4,
                )
            )
      table[sid] = float(np.mean(tile))
  return _res(
      v, {"block_means": table, "rounding": "half_away_from_zero"}, subs, exps
  )


MODE_A, MODE_B, MODE_C, MODE_D = 100, -200, 300, -400


def _mode_motifs() -> dict[str, tuple[np.ndarray, int]]:
  a, b, c, d = MODE_A, MODE_B, MODE_C, MODE_D
  return {
      "tie_rows": (np.array([[a, a], [b, b]]), a),
      "tie_cols": (np.array([[a, b], [a, b]]), a),
      "tie_diag": (np.array([[a, b], [b, a]]), b),
      "distinct": (np.array([[a, b], [c, d]]), a),
      "majority": (np.array([[b, a], [a, a]]), a),
      "tie_rows_t": (np.array([[b, b], [a, a]]), b),
  }


@register_panel(
    "calib_mode_ties", 0, 3, designed_integer=True, label="MODE tie probes"
)
def _calib_mode_ties(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  c = _cells(n)
  v = np.zeros((n, n))
  subs, exps = [], []
  a, b, cc, d = MODE_A, MODE_B, MODE_C, MODE_D
  # 4x4 motif where cascaded MODE (a) differs from direct MODE (b).
  m4 = np.block([
      [np.full((2, 2), b), np.array([[a, b], [cc, d]])],
      [np.array([[a, cc], [d, b]]), np.array([[a, a], [cc, d]])],
  ])
  motifs = dict(_mode_motifs())
  row = 0
  for name, (motif, expected) in motifs.items():
    for col in range(8):
      # Columns alternate the value roles so each probe appears twice.
      rolled = motif if col % 2 == 0 else -motif
      exp_val = expected if col % 2 == 0 else -expected
      sid = f"{name}_{col}"
      v[row * c : (row + 1) * c, col * c : (col + 1) * c] = np.tile(
          rolled, (c // 2, c // 2)
      )
      subs.append(
          Subregion(sid, col * c, row * c, c, c, {"motif": rolled.tolist()})
      )
      exps.append(
          Expectation(
              id=f"{sid}_mode_l1",
              check="interior_equals",
              methods=("MODE",),
              levels=(1,),
              subregion=sid,
              value=float(exp_val),
              basis="expected",
          )
      )
    row += 1
  for r in range(row, 8):
    sid = f"m4_casc_{r}"
    v[r * c : (r + 1) * c, :] = np.tile(m4, (c // 4, n // 4))
    subs.append(Subregion(sid, 0, r * c, n, c, {"motif": m4.tolist()}))
    exps.append(
        Expectation(
            id=f"{sid}_mode_l2_cascaded",
            check="interior_equals",
            methods=("MODE",),
            levels=(2,),
            subregion=sid,
            value=float(a),
            basis="expected",
        )
    )
  exps.append(
      Expectation(
          id="mode_cascaded_reference",
          check="equals_reference",
          methods=("MODE",),
          reference="mode",
          strategy="cascaded",
          basis="expected",
      )
  )
  return _res(
      v,
      {
          "values": [a, b, cc, d],
          "tie_rule": "first value to reach the top count",
      },
      subs,
      exps,
  )


def _impulse_points(n: int) -> list[tuple[int, int]]:
  c = _cells(n)
  points = []
  for j in range(8):
    for i in range(8):
      points.append((c * i + c // 2 + i, c * j + c // 2 + j))
  return points


def _impulse(ctx: PanelContext, value: float) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  pts = _impulse_points(n)
  for x, y in pts:
    v[y, x] = value
  levels = levels_for(ctx)
  survivors = nearest_survivors(pts, ctx.origin, levels)
  exps = [
      Expectation(
          id="nearest_impulse_survivors",
          check="count_equals",
          methods=("NEAREST",),
          levels=levels,
          value=value,
          count=survivors,
      ),
      Expectation(
          id="average_impulse_l1",
          check="count_equals",
          methods=("AVERAGE",),
          levels=(1,),
          value=value / 4,
          count={"1": len(pts)},
      ),
  ]
  return _res(
      v,
      {
          "value": value,
          "points": [list(p) for p in pts],
          "nearest_survivors": survivors,
      },
      [],
      exps,
  )


@register_panel(
    "calib_impulse_pos",
    0,
    4,
    designed_integer=True,
    label="Impulses +1000, offsets 0-7",
)
def _calib_impulse_pos(ctx: PanelContext) -> PanelResult:
  return _impulse(ctx, 1000.0)


@register_panel(
    "calib_impulse_neg",
    0,
    5,
    designed_integer=True,
    label="Impulses -1000, offsets 0-7",
)
def _calib_impulse_neg(ctx: PanelContext) -> PanelResult:
  return _impulse(ctx, -1000.0)


# ---------------------------------------------------------------------------
# Row 1: categorical
# ---------------------------------------------------------------------------

CLASS_CODES = (-1000, -750, -500, -250, 250, 500, 750, 1000)
NLCD_CODES = (11, 21, 22, 23, 24, 31, 41, 42, 43, 52, 71, 81, 82, 90, 95)


def voronoi_labels(n: int, count: int, rng: np.random.Generator) -> np.ndarray:
  seeds = rng.uniform(0, n, size=(max(count, 2), 2))
  tree = scipy.spatial.cKDTree(seeds)
  cx, cy = centers(n)
  _, idx = tree.query(np.column_stack([cx.ravel(), cy.ravel()]))
  return idx.reshape(n, n)


def _voronoi(ctx: PanelContext, count: int) -> tuple[np.ndarray, int]:
  labels = voronoi_labels(ctx.size, count, ctx.rng)
  codes = ctx.rng.choice(np.array(CLASS_CODES), size=labels.max() + 1)
  return codes[labels].astype(np.float64), count


@register_panel(
    "cat_voronoi_large",
    1,
    0,
    designed_integer=True,
    label="Voronoi ~150 px, 8 classes",
)
def _cat_voronoi_large(ctx: PanelContext) -> PanelResult:
  count = max(8, round(40 * (ctx.size / 1024) ** 2))
  v, count = _voronoi(ctx, count)
  return _res(v, {"cells": count, "classes": list(CLASS_CODES)})


@register_panel(
    "cat_voronoi_fine",
    1,
    1,
    designed_integer=True,
    label="Voronoi 4-16 px, 8 classes",
)
def _cat_voronoi_fine(ctx: PanelContext) -> PanelResult:
  count = ctx.size * ctx.size // 100
  v, count = _voronoi(ctx, count)
  return _res(
      v, {"cells": count, "mean_spacing_px": 10, "classes": list(CLASS_CODES)}
  )


def smooth_field(n: int, sigma: float, rng: np.random.Generator) -> np.ndarray:
  field = scipy.ndimage.gaussian_filter(
      rng.standard_normal((n, n)), sigma, mode="wrap"
  )
  return (field - field.mean()) / (field.std() + 1e-12)


def quantize_codes(field: np.ndarray, codes: Sequence[int]) -> np.ndarray:
  edges = np.quantile(field, np.linspace(0, 1, len(codes) + 1)[1:-1])
  return np.asarray(codes, dtype=np.float64)[np.digitize(field, edges)]


@register_panel(
    "cat_landcover_codes",
    1,
    2,
    designed_integer=True,
    label="NLCD-style land cover codes",
)
def _cat_landcover_codes(ctx: PanelContext) -> PanelResult:
  field = smooth_field(ctx.size, ctx.size / 32, ctx.rng)
  return _res(quantize_codes(field, NLCD_CODES), {"codes": list(NLCD_CODES)})


@register_panel(
    "cat_thin_features",
    1,
    3,
    designed_integer=True,
    label="Thin roads (24) and rivers (11)",
)
def _cat_thin_features(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  background = (41, 42, 43, 52, 71, 81, 82)
  v = quantize_codes(smooth_field(n, n / 16, ctx.rng), background)
  step = n / 6

  def roads(draw: PIL.ImageDraw.ImageDraw) -> None:
    for i, w in enumerate(range(1, 6)):
      y = int(step * (i + 0.5))
      draw.line([(0, y), (n, y + n // 8)], fill=255, width=w)

  def rivers(draw: PIL.ImageDraw.ImageDraw) -> None:
    ys = np.arange(0, n + 1, 2)
    for i, w in enumerate(range(1, 6)):
      x0 = step * (i + 0.5)
      xs = x0 + 0.04 * n * np.sin(2 * np.pi * ys / (n / 3) + i)
      draw.line(list(zip(xs.tolist(), ys.tolist())), fill=255, width=w)

  v[draw_mask(n, rivers)] = 11
  v[draw_mask(n, roads)] = 24
  return _res(
      v,
      {
          "road_code": 24,
          "river_code": 11,
          "widths": [1, 2, 3, 4, 5],
          "background": list(background),
      },
  )


@register_panel(
    "cat_majority_dither",
    1,
    4,
    designed_integer=True,
    label="Majority dither 10/25/40/50 %",
)
def _cat_majority_dither(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v = np.full((n, n), 250.0)
  subs = []
  for (x, y, w, h), p in zip(quadrants(n), (0.10, 0.25, 0.40, 0.50)):
    sub = ctx.rng.random((h, w)) < p
    v[y : y + h, x : x + w][sub] = -750.0
    subs.append(
        Subregion(f"minority_{int(p * 100)}", x, y, w, h, {"fraction": p})
    )
  return _res(v, {"class_a": 250, "class_b": -750}, subs)


@register_panel(
    "cat_edge_phase",
    1,
    5,
    designed_integer=True,
    label="Class edges at offsets 0-7",
)
def _cat_edge_phase(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  c = _cells(n)
  half = n // 2
  v = np.full((n, n), -500.0)
  subs = []
  for p in range(8):
    x = p * c
    v[:half, x + c // 2 + p : x + c] = 500.0
    subs.append(Subregion(f"vertical_p{p}", x, 0, c, half, {"offset": p}))
    v[half + (half // 2) + p :, x : x + c] = 500.0
    subs.append(
        Subregion(f"horizontal_p{p}", x, half, c, n - half, {"offset": p})
    )
  return _res(v, {"classes": [-500, 500]}, subs)


# ---------------------------------------------------------------------------
# Row 2: lines
# ---------------------------------------------------------------------------

LINE_SPACINGS = (2, 3, 4, 5, 8, 16, 32)


def _line_bands(n: int) -> tuple[np.ndarray, list[Subregion]]:
  x, y = grid_xy(n)
  band = n // 8
  on = np.zeros((n, n), dtype=bool)
  subs = []
  for i, s in enumerate(LINE_SPACINGS):
    rows = slice(i * band, (i + 1) * band)
    on[rows] = (y[rows] % s) == 0
    subs.append(Subregion(f"spacing_{s}", 0, i * band, n, band, {"spacing": s}))
  del x
  return on, subs


@register_panel(
    "lines_horizontal",
    2,
    0,
    designed_integer=True,
    label="1 px horizontal lines",
)
def _lines_horizontal(ctx: PanelContext) -> PanelResult:
  on, subs = _line_bands(ctx.size)
  return _res(pm(on), {"spacings": list(LINE_SPACINGS)}, subs)


@register_panel(
    "lines_vertical", 2, 1, designed_integer=True, label="1 px vertical lines"
)
def _lines_vertical(ctx: PanelContext) -> PanelResult:
  on, subs = _line_bands(ctx.size)
  subs = [Subregion(s.id, s.y, s.x, s.h, s.w, s.params) for s in subs]
  return _res(pm(on.T), {"spacings": list(LINE_SPACINGS)}, subs)


@register_panel(
    "lines_crosshatch", 2, 2, designed_integer=True, label="Crosshatch 4-32 px"
)
def _lines_crosshatch(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  x, y = grid_xy(n)
  on = np.zeros((n, n), dtype=bool)
  subs = []
  for (qx, qy, w, h), s in zip(quadrants(n), (4, 8, 16, 32)):
    sl = np.s_[qy : qy + h, qx : qx + w]
    on[sl] = ((x[sl] % s) == 0) | ((y[sl] % s) == 0)
    subs.append(Subregion(f"spacing_{s}", qx, qy, w, h, {"spacing": s}))
  return _res(pm(on), {"spacings": [4, 8, 16, 32]}, subs)


@register_panel(
    "lines_widths",
    2,
    3,
    designed_integer=True,
    label="Isolated lines 1-8 px wide",
)
def _lines_widths(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  pitch = n // 16
  v = np.full((n, n), -1000.0)
  half = n // 2
  for i in range(8):
    w = i + 1
    y = pitch * i + pitch // 2
    v[y : y + w, :] = 1000.0
    x = pitch * i + pitch // 2
    v[half:, x : x + w] = 1000.0
  return _res(v, {"widths": list(range(1, 9)), "pitch": pitch})


LINE_ANGLES = (5, 15, 30, 45, 60, 75)


@register_panel(
    "lines_angled",
    2,
    4,
    designed_integer=True,
    label="Aliased 1 px lines 5-75 deg",
)
def _lines_angled(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  band = n // 6
  v = np.full((n, n), -1000.0)
  subs = []
  for i, deg in enumerate(LINE_ANGLES):
    h = band if i < 5 else n - 5 * band
    t = math.tan(math.radians(deg))

    def lines(draw: PIL.ImageDraw.ImageDraw, t: float = t) -> None:
      for x0 in range(-4 * n, 4 * n, 32):
        draw.line([(x0, 0), (x0 + n / t, n)], fill=255, width=1)

    m = draw_mask(n, lines, h)
    v[i * band : i * band + h][m] = 1000.0
    subs.append(Subregion(f"angle_{deg}", 0, i * band, n, h, {"angle": deg}))
  return _res(v, {"angles": list(LINE_ANGLES), "spacing": 32}, subs)


@register_panel(
    "lines_crossing_diagonal",
    2,
    5,
    designed_integer=True,
    label="Crossing diagonal line grids",
)
def _lines_crossing_diagonal(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  x, y = grid_xy(n)
  on = np.zeros((n, n), dtype=bool)
  subs = []
  for (qx, qy, w, h), s in zip(quadrants(n), (4, 8, 16, 32)):
    sl = np.s_[qy : qy + h, qx : qx + w]
    on[sl] = (((x[sl] + y[sl]) % s) == 0) | (((x[sl] - y[sl]) % s) == 0)
    subs.append(Subregion(f"spacing_{s}", qx, qy, w, h, {"spacing": s}))
  return _res(pm(on), {"spacings": [4, 8, 16, 32]}, subs)


# ---------------------------------------------------------------------------
# Row 3: checkers and stripes
# ---------------------------------------------------------------------------


def _checker(x: np.ndarray, y: np.ndarray, c: int) -> np.ndarray:
  return ((x // c + y // c) % 2) == 1


def _checker_quadrants(
    n: int, sizes: Sequence[int]
) -> tuple[np.ndarray, list[Subregion]]:
  x, y = grid_xy(n)
  on = np.zeros((n, n), dtype=bool)
  subs = []
  for (qx, qy, w, h), c in zip(quadrants(n), sizes):
    sl = np.s_[qy : qy + h, qx : qx + w]
    on[sl] = _checker(x[sl], y[sl], c)
    subs.append(Subregion(f"cell_{c}", qx, qy, w, h, {"cell": c}))
  return pm(on), subs


@register_panel(
    "checker_fine", 3, 0, designed_integer=True, label="Checkers 1-4 px"
)
def _checker_fine(ctx: PanelContext) -> PanelResult:
  v, subs = _checker_quadrants(ctx.size, (1, 2, 3, 4))
  exps = [
      Expectation(
          id="checker1_average_zero",
          check="interior_equals",
          methods=("AVERAGE",),
          subregion="cell_1",
          value=0.0,
      ),
      Expectation(
          id="checker1_rms_1000",
          check="interior_equals",
          methods=("RMS",),
          subregion="cell_1",
          value=1000.0,
      ),
      Expectation(
          id="checker1_nearest_constant",
          check="interior_equals",
          methods=("NEAREST",),
          subregion="cell_1",
          value=-1000.0,
      ),
      Expectation(
          id="checker1_mode_diag_tie",
          check="interior_equals",
          methods=("MODE",),
          subregion="cell_1",
          value=1000.0,
          basis="expected",
      ),
      Expectation(
          id="checker2_average_zero_l2",
          check="interior_equals",
          methods=("AVERAGE",),
          levels=(2, 3, 4, 5),
          subregion="cell_2",
          value=0.0,
      ),
  ]
  return _res(v, {"cells": [1, 2, 3, 4]}, subs, exps)


@register_panel(
    "checker_coarse", 3, 1, designed_integer=True, label="Checkers 5-32 px"
)
def _checker_coarse(ctx: PanelContext) -> PanelResult:
  v, subs = _checker_quadrants(ctx.size, (5, 8, 16, 32))
  return _res(v, {"cells": [5, 8, 16, 32]}, subs)


def _stripes(
    n: int, periods: Sequence[int]
) -> tuple[np.ndarray, list[Subregion]]:
  x, _ = grid_xy(n)
  on = np.zeros((n, n), dtype=bool)
  subs = []
  for (qx, qy, w, h), p in zip(quadrants(n), periods):
    sl = np.s_[qy : qy + h, qx : qx + w]
    on[sl] = (np.floor(2 * x[sl] / p) % 2) == 1
    subs.append(Subregion(f"period_{p}", qx, qy, w, h, {"period": p}))
  return on, subs


@register_panel(
    "stripes_vertical",
    3,
    2,
    designed_integer=True,
    label="Vertical stripes 2-6 px",
)
def _stripes_vertical(ctx: PanelContext) -> PanelResult:
  on, subs = _stripes(ctx.size, (2, 3, 4, 6))
  exps = [
      Expectation(
          id="stripes2_average_zero",
          check="interior_equals",
          methods=("AVERAGE",),
          subregion="period_2",
          value=0.0,
      )
  ]
  return _res(pm(on), {"periods": [2, 3, 4, 6]}, subs, exps)


@register_panel(
    "stripes_horizontal",
    3,
    3,
    designed_integer=True,
    label="Horizontal stripes 2-6 px",
)
def _stripes_horizontal(ctx: PanelContext) -> PanelResult:
  on, subs = _stripes(ctx.size, (2, 3, 4, 6))
  subs = [Subregion(s.id, s.y, s.x, s.h, s.w, s.params) for s in subs]
  exps = [
      Expectation(
          id="stripes2_average_zero",
          check="interior_equals",
          methods=("AVERAGE",),
          subregion="period_2",
          value=0.0,
      )
  ]
  return _res(pm(on.T), {"periods": [2, 3, 4, 6]}, subs, exps)


CHECKER_PAIRS = ((-1000, 1000), (0, 1000), (-1000, 0), (500, 1000))


@register_panel(
    "checker_offset",
    3,
    4,
    designed_integer=True,
    label="1 px checkers, offset value pairs",
)
def _checker_offset(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  x, y = grid_xy(n)
  on = _checker(x, y, 1)
  v = np.zeros((n, n))
  subs, exps = [], []
  for (qx, qy, w, h), (lo, hi) in zip(quadrants(n), CHECKER_PAIRS):
    sl = np.s_[qy : qy + h, qx : qx + w]
    v[sl] = np.where(on[sl], hi, lo)
    sid = f"pair_{lo}_{hi}".replace("-", "m")
    subs.append(Subregion(sid, qx, qy, w, h, {"pair": [lo, hi]}))
    exps.append(
        Expectation(
            id=f"{sid}_average",
            check="interior_equals",
            methods=("AVERAGE",),
            subregion=sid,
            value=(lo + hi) / 2,
        )
    )
    exps.append(
        Expectation(
            id=f"{sid}_rms",
            check="interior_equals",
            methods=("RMS",),
            subregion=sid,
            value=math.sqrt((lo * lo + hi * hi) / 2),
            tolerance=0.5,
        )
    )
  return _res(v, {"pairs": [list(p) for p in CHECKER_PAIRS]}, subs, exps)


@register_panel(
    "stripes_duty_cycle",
    3,
    5,
    designed_integer=True,
    label="Stripes 1 on / 3 or 7 off",
)
def _stripes_duty_cycle(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  x, _ = grid_xy(n)
  half = n // 2
  on = np.zeros((n, n), dtype=bool)
  on[:half] = (x[:half] % 4) < 1
  on[half:] = (x[half:] % 8) < 1
  subs = [
      Subregion("on1_off3", 0, 0, n, half, {"period": 4}),
      Subregion("on1_off7", 0, half, n, n - half, {"period": 8}),
  ]
  exps = [
      Expectation(
          id="duty4_average_l2",
          check="interior_equals",
          methods=("AVERAGE",),
          levels=(2, 3, 4, 5),
          subregion="on1_off3",
          value=-500.0,
      ),
      Expectation(
          id="duty8_average_l3",
          check="interior_equals",
          methods=("AVERAGE",),
          levels=(3, 4, 5),
          subregion="on1_off7",
          value=-750.0,
      ),
  ]
  return _res(pm(on), {"periods": [4, 8]}, subs, exps)


# ---------------------------------------------------------------------------
# Row 4: frequency response
# ---------------------------------------------------------------------------


def zone_plate(n: int) -> tuple[np.ndarray, float]:
  """Zone plate reaching 0.5 cycles/px at r = n/2; zero beyond."""
  cx, cy = centers(n)
  r = np.hypot(cx - n / 2, cy - n / 2)
  big_r = n / 2
  v = VALUE_LIMIT * np.cos(np.pi * r * r / (2 * big_r))
  return np.where(r <= big_r, v, 0.0), big_r


def zone_plate_frequency(r: float, big_r: float) -> float:
  """Local frequency (cycles/px) of zone_plate at radius r."""
  return r / (2 * big_r)


@register_panel(
    "freq_zone_plate",
    4,
    0,
    designed_integer=False,
    label="Zone plate to Nyquist",
)
def _freq_zone_plate(ctx: PanelContext) -> PanelResult:
  v, big_r = zone_plate(ctx.size)
  return _res(v, {"nyquist_radius": big_r})


def chirp(length: int, f0: float = 1 / 64, f1: float = 0.5) -> np.ndarray:
  t = np.arange(length) + 0.5
  phase = 2 * np.pi * (f0 * t + (f1 - f0) * t * t / (2 * length))
  return VALUE_LIMIT * np.sin(phase)


@register_panel(
    "freq_chirp_xy",
    4,
    1,
    designed_integer=False,
    label="x and y chirps, 64 to 2 px",
)
def _freq_chirp_xy(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  v = np.zeros((n, n))
  v[:half, :] = chirp(n)[None, :]
  v[half:, :] = chirp(n - half)[:, None]
  subs = [
      Subregion("x_chirp", 0, 0, n, half),
      Subregion("y_chirp", 0, half, n, n - half),
  ]
  return _res(v, {"period_start": 64, "period_end": 2}, subs)


@register_panel(
    "freq_siemens_star",
    4,
    2,
    designed_integer=True,
    label="Siemens star, 36 cycles",
)
def _freq_siemens_star(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, cy = centers(n)
  theta = np.arctan2(cy - n / 2, cx - n / 2)
  return _res(pm(np.sin(36 * theta) >= 0), {"cycles": 36})


def sine_grating(w: int, h: int, period: float, angle_deg: float) -> np.ndarray:
  cx, cy = centers(w, h)
  a = math.radians(angle_deg)
  return VALUE_LIMIT * np.sin(
      2 * np.pi * (cx * math.cos(a) + cy * math.sin(a)) / period
  )


GRATING_PERIODS = (2.5, 3, 4, 5, 6, 8, 12, 16)


@register_panel(
    "freq_sine_gratings",
    4,
    3,
    designed_integer=False,
    label="Sine gratings 2.5-16 px, 0/90 deg",
)
def _freq_sine_gratings(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  s = n // 4
  v = np.zeros((n, n))
  subs = []
  i = 0
  for angle in (0, 90):
    for p in GRATING_PERIODS:
      r, c = divmod(i, 4)
      v[r * s : (r + 1) * s, c * s : (c + 1) * s] = sine_grating(s, s, p, angle)
      subs.append(
          Subregion(
              f"p{p}_a{angle}".replace(".", "_"),
              c * s,
              r * s,
              s,
              s,
              {"period": p, "angle": angle},
          )
      )
      i += 1
  return _res(v, {"periods": list(GRATING_PERIODS), "angles": [0, 90]}, subs)


@register_panel(
    "freq_sine_gratings_oblique",
    4,
    4,
    designed_integer=False,
    label="Sine gratings at 30/45/60 deg",
)
def _freq_sine_gratings_oblique(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  sw, sh = n // 4, n // 6
  v = np.zeros((n, n))
  subs = []
  i = 0
  for angle in (30, 45, 60):
    for p in GRATING_PERIODS:
      r, c = divmod(i, 4)
      h = sh if r < 5 else n - 5 * sh
      v[r * sh : r * sh + h, c * sw : (c + 1) * sw] = sine_grating(
          sw, h, p, angle
      )
      subs.append(
          Subregion(
              f"p{p}_a{angle}".replace(".", "_"),
              c * sw,
              r * sh,
              sw,
              h,
              {"period": p, "angle": angle},
          )
      )
      i += 1
  return _res(
      v, {"periods": list(GRATING_PERIODS), "angles": [30, 45, 60]}, subs
  )


USAF_WIDTHS = (1, 2, 3, 4, 6, 8, 12, 16)


@register_panel(
    "freq_usaf1951",
    4,
    5,
    designed_integer=True,
    label="USAF-1951 style bars 1-16 px",
)
def _freq_usaf1951(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  s = n // 4
  v = np.full((n, n), -1000.0)
  subs = []
  placed = []
  i = 0
  for orient in ("vertical", "horizontal"):
    for w in USAF_WIDTHS:
      r, c = divmod(i, 4)
      i += 1
      size = 5 * w
      if size + 8 > s:
        continue
      x0 = c * s + (s - size) // 2
      y0 = r * s + (s - size) // 2
      for b in range(3):
        if orient == "vertical":
          _fill_rect(v, x0 + 2 * b * w, y0, w, size, 1000.0)
        else:
          _fill_rect(v, x0, y0 + 2 * b * w, size, w, 1000.0)
      sid = f"{orient}_w{w}"
      subs.append(
          Subregion(
              sid, c * s, r * s, s, s, {"bar_width": w, "orientation": orient}
          )
      )
      placed.append(sid)
  return _res(v, {"bar_widths": list(USAF_WIDTHS), "placed": placed}, subs)


# ---------------------------------------------------------------------------
# Row 5: sparse posts and holes
# ---------------------------------------------------------------------------

SQUARE_SIZES = (1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33)
DISK_DIAMETERS = (1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64)


def _slot_features(
    n: int, slot: int, sizes: Sequence[int], offsets: Sequence[int]
) -> list[tuple[int, int, int, int]]:
  """(x, y, size, offset) for size x offset features that fit on slots."""
  per_row = n // slot
  out = []
  k = 0
  for off in offsets:
    for size in sizes:
      if k >= per_row * per_row:
        return out
      if size + off + 2 > slot:
        continue
      r, c = divmod(k, per_row)
      out.append((c * slot + off, r * slot + off, size, off))
      k += 1
  return out


def _squares(ctx: PanelContext, value: float) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  feats = _slot_features(n, 64, SQUARE_SIZES, range(8))
  for x, y, size, _ in feats:
    v[y : y + size, x : x + size] = value
  return _res(
      v,
      {
          "value": value,
          "features": [list(f) for f in feats],
          "feature_fields": ["x", "y", "size", "offset"],
      },
  )


@register_panel(
    "sparse_posts_square",
    5,
    0,
    designed_integer=True,
    label="+1000 square posts 1-33 px",
)
def _sparse_posts_square(ctx: PanelContext) -> PanelResult:
  return _squares(ctx, 1000.0)


@register_panel(
    "sparse_holes_square",
    5,
    1,
    designed_integer=True,
    label="-1000 square holes 1-33 px",
)
def _sparse_holes_square(ctx: PanelContext) -> PanelResult:
  return _squares(ctx, -1000.0)


def _disks(ctx: PanelContext, value: float) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  x, y = grid_xy(n)
  feats = _slot_features(n, _cells(n), DISK_DIAMETERS, range(4))
  for fx, fy, d, _ in feats:
    r = d / 2
    cxx, cyy = fx + r, fy + r
    sl = np.s_[fy : fy + d + 1, fx : fx + d + 1]
    inside = (x[sl] + 0.5 - cxx) ** 2 + (y[sl] + 0.5 - cyy) ** 2 <= r * r
    v[sl][inside] = value
  return _res(
      v,
      {
          "value": value,
          "features": [list(f) for f in feats],
          "feature_fields": ["x", "y", "diameter", "offset"],
      },
  )


@register_panel(
    "sparse_posts_disk",
    5,
    2,
    designed_integer=True,
    label="+1000 disks 1-64 px",
)
def _sparse_posts_disk(ctx: PanelContext) -> PanelResult:
  return _disks(ctx, 1000.0)


@register_panel(
    "sparse_holes_disk",
    5,
    3,
    designed_integer=True,
    label="-1000 disks 1-64 px",
)
def _sparse_holes_disk(ctx: PanelContext) -> PanelResult:
  return _disks(ctx, -1000.0)


@register_panel(
    "sparse_on_slope",
    5,
    4,
    designed_integer=False,
    label="Posts and holes on a ramp",
)
def _sparse_on_slope(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, _ = centers(n)
  v = -500.0 + 1000.0 * cx / n
  c = _cells(n)
  sizes = (1, 2, 4, 8, 16)
  for i, size in enumerate(sizes):
    for j in range(8):
      if size + 2 > c:
        continue
      x = j * c + c // 2
      y = i * (n // len(sizes)) + c // 2
      v[y : y + size, x : x + size] = 1000.0 if j % 2 == 0 else -1000.0
  return _res(v, {"sizes": list(sizes), "ramp": [-500, 500]})


@register_panel(
    "sparse_dipoles",
    5,
    5,
    designed_integer=True,
    label="Post/hole dipoles, gap 0-16 px",
)
def _sparse_dipoles(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  c = _cells(n)
  gaps = (0, 1, 2, 3, 4, 6, 8, 16)
  for row, size in enumerate((1, 2, 4, 8, 1, 2, 4, 8)):
    for col, gap in enumerate(gaps):
      if 2 * size + gap + 2 > c:
        continue
      x = col * c + (c - 2 * size - gap) // 2
      y = row * c + c // 2
      vertical = row >= 4
      if vertical:
        x, y = col * c + c // 2, row * c + (c - 2 * size - gap) // 2
        v[y : y + size, x : x + size] = 1000.0
        v[y + size + gap : y + 2 * size + gap, x : x + size] = -1000.0
      else:
        v[y : y + size, x : x + size] = 1000.0
        v[y : y + size, x + size + gap : x + 2 * size + gap] = -1000.0
  return _res(v, {"gaps": list(gaps), "sizes": [1, 2, 4, 8]})


# ---------------------------------------------------------------------------
# Row 6: steps
# ---------------------------------------------------------------------------


@register_panel(
    "step_horizontal",
    6,
    0,
    designed_integer=True,
    label="Horizontal step -1000/+1000",
)
def _step_horizontal(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  v = np.full((n, n), -1000.0)
  v[half:] = 1000.0
  subs = [Subregion("low", 0, 0, n, half), Subregion("high", 0, half, n, half)]
  exps = (
      flat_expectations("low", -1000.0)
      + flat_expectations("high", 1000.0)
      + overshoot_expectations()
  )
  return _res(v, {"edge_row": half}, subs, exps)


@register_panel(
    "step_vertical",
    6,
    1,
    designed_integer=True,
    label="Vertical step -1000/+1000",
)
def _step_vertical(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  v = np.full((n, n), -1000.0)
  v[:, half:] = 1000.0
  subs = [Subregion("low", 0, 0, half, n), Subregion("high", half, 0, half, n)]
  exps = (
      flat_expectations("low", -1000.0)
      + flat_expectations("high", 1000.0)
      + overshoot_expectations()
  )
  return _res(v, {"edge_col": half}, subs, exps)


@register_panel(
    "step_both",
    6,
    2,
    designed_integer=True,
    label="Square and quadrant checker corners",
)
def _step_both(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half, q = n // 2, n // 4
  v = np.full((n, n), -1000.0)
  v[q + q // 2 : q + q // 2 + q, q // 2 : q // 2 + q] = 1000.0
  v[:q, half + q :] = 1000.0
  v[q:half, half : half + q] = 1000.0
  v[half + q :, half : half + q] = 1000.0
  v[half : half + q, half + q :] = 1000.0
  return _res(v, {}, [], overshoot_expectations())


@register_panel(
    "step_phase", 6, 3, designed_integer=True, label="Step edges at offsets 0-7"
)
def _step_phase(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  c = _cells(n)
  half = n // 2
  v = np.full((n, n), -1000.0)
  subs = []
  for p in range(8):
    x = p * c
    v[:half, x + c // 2 + p : x + c] = 1000.0
    subs.append(Subregion(f"vertical_p{p}", x, 0, c, half, {"offset": p}))
    v[half + half // 2 + p :, x : x + c] = 1000.0
    subs.append(
        Subregion(f"horizontal_p{p}", x, half, c, n - half, {"offset": p})
    )
  return _res(v, {}, subs, overshoot_expectations())


BAR_WIDTHS = (1, 2, 3, 4, 6, 8, 12, 16)


@register_panel(
    "step_bars", 6, 4, designed_integer=True, label="Bars 1-16 px on -1000"
)
def _step_bars(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  pitch = n // 8
  half = n // 2
  v = np.full((n, n), -1000.0)
  for i, w in enumerate(BAR_WIDTHS):
    x = i * pitch + pitch // 2 - w // 2
    v[:half, x : x + w] = 1000.0
    y = half + (i * pitch) // 2 + pitch // 4 - w // 2
    if y + w <= n:
      v[y : y + w, :] = 1000.0
  return _res(v, {"widths": list(BAR_WIDTHS)}, [], overshoot_expectations())


STEP_HEIGHTS = (1, 2, 3, 5, 10, 50, 200)


@register_panel(
    "step_small", 6, 5, designed_integer=True, label="Small steps 1-200"
)
def _step_small(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  w = n // 7
  half = n // 2
  v = np.zeros((n, n))
  subs = []
  for i, h in enumerate(STEP_HEIGHTS):
    x1 = n if i == 6 else (i + 1) * w
    v[half:, i * w : x1] = h
    subs.append(Subregion(f"h{h}", i * w, 0, x1 - i * w, n, {"height": h}))
  return _res(v, {"heights": list(STEP_HEIGHTS)}, subs)


# ---------------------------------------------------------------------------
# Row 7: diagonal steps
# ---------------------------------------------------------------------------


def half_plane(
    n: int, angle_deg: float, m: int | None = None, supersample: int = 1
) -> np.ndarray:
  """Fraction of each pixel on the + side of a line through the center."""
  m = n if m is None else m
  a = math.radians(angle_deg)
  total = np.zeros((m, n))
  y, x = np.mgrid[0:m, 0:n].astype(np.float64)
  for i in range(supersample):
    for j in range(supersample):
      sx = x + (i + 0.5) / supersample - n / 2
      sy = y + (j + 0.5) / supersample - m / 2
      total += (sx * math.sin(a) - sy * math.cos(a)) > 0
  return total / (supersample * supersample)


@register_panel(
    "diag_step_45", 7, 0, designed_integer=True, label="45 deg step"
)
def _diag_step_45(ctx: PanelContext) -> PanelResult:
  v = pm(half_plane(ctx.size, 45) > 0.5)
  return _res(v, {"angle": 45}, [], overshoot_expectations())


@register_panel(
    "diag_step_5deg", 7, 1, designed_integer=True, label="5 deg slanted edge"
)
def _diag_step_5deg(ctx: PanelContext) -> PanelResult:
  v = pm(half_plane(ctx.size, 95) > 0.5)
  return _res(v, {"angle_from_vertical": 5}, [], overshoot_expectations())


@register_panel(
    "diag_step_30_60", 7, 2, designed_integer=True, label="30 and 60 deg steps"
)
def _diag_step_30_60(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  v = np.zeros((n, n))
  v[:, :half] = pm(half_plane(half, 30, n) > 0.5)
  v[:, half:] = pm(half_plane(n - half, 60, n) > 0.5)
  subs = [
      Subregion("angle_30", 0, 0, half, n),
      Subregion("angle_60", half, 0, n - half, n),
  ]
  return _res(v, {"angles": [30, 60]}, subs, overshoot_expectations())


@register_panel(
    "diag_diamond", 7, 3, designed_integer=True, label="Diamond +1000 on -1000"
)
def _diag_diamond(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, cy = centers(n)
  big_r = 3 * n / 8
  v = pm(np.abs(cx - n / 2) + np.abs(cy - n / 2) <= big_r)
  return _res(v, {"radius": big_r}, [], overshoot_expectations())


@register_panel(
    "diag_bars_45", 7, 4, designed_integer=True, label="45 deg bars 1-16 px"
)
def _diag_bars_45(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, cy = centers(n)
  t = (cx - cy) / math.sqrt(2)
  pitch = n / 12
  on = np.zeros((n, n), dtype=bool)
  for i, w in enumerate(BAR_WIDTHS):
    center = (i - 3.5) * pitch
    on |= np.abs(t - center) < w / 2
  return _res(
      pm(on), {"widths": list(BAR_WIDTHS)}, [], overshoot_expectations()
  )


@register_panel(
    "diag_edge_antialiased",
    7,
    5,
    designed_integer=False,
    label="22.5 deg edge: anti-aliased vs aliased",
)
def _diag_edge_antialiased(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  v = np.zeros((n, n))
  v[:, :half] = -1000.0 + 2000.0 * half_plane(half, 67.5, n, supersample=8)
  v[:, half:] = pm(half_plane(n - half, 67.5, n) > 0.5)
  subs = [
      Subregion("antialiased", 0, 0, half, n),
      Subregion("aliased", half, 0, n - half, n),
  ]
  return _res(v, {"angle_from_vertical": 22.5, "supersample": 8}, subs)


# ---------------------------------------------------------------------------
# Row 8: terrain
# ---------------------------------------------------------------------------

PEAK_SIGMAS = (1, 2, 4, 8, 16, 32, 64, 150)


def _peak_centers(n: int) -> list[tuple[float, float]]:
  return [
      (n / 8, n / 8),
      (3 * n / 8, n / 8),
      (5 * n / 8, n / 8),
      (7 * n / 8, n / 8),
      (n / 4, 3 * n / 8),
      (3 * n / 4, 3 * n / 8),
      (n / 4, 3 * n / 4),
      (3 * n / 4, 3 * n / 4),
  ]


def gaussian_peaks(n: int) -> np.ndarray:
  cx, cy = centers(n)
  out = np.zeros((n, n))
  for (px, py), s in zip(_peak_centers(n), PEAK_SIGMAS):
    px, py = math.floor(px) + 0.5, math.floor(py) + 0.5
    g = np.exp(-((cx - px) ** 2 + (cy - py) ** 2) / (2 * s * s))
    out = np.maximum(out, g)
  return VALUE_LIMIT * out


@register_panel(
    "terrain_mountains",
    8,
    0,
    designed_integer=False,
    label="Gaussian peaks sigma 1-150",
)
def _terrain_mountains(ctx: PanelContext) -> PanelResult:
  return _res(gaussian_peaks(ctx.size), {"sigmas": list(PEAK_SIGMAS)})


@register_panel(
    "terrain_pits",
    8,
    1,
    designed_integer=False,
    label="Gaussian pits sigma 1-150",
)
def _terrain_pits(ctx: PanelContext) -> PanelResult:
  return _res(-gaussian_peaks(ctx.size), {"sigmas": list(PEAK_SIGMAS)})


@register_panel(
    "terrain_ridges_valleys",
    8,
    2,
    designed_integer=False,
    label="Ridges and valleys sigma 1-32",
)
def _terrain_ridges_valleys(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  sigmas = (1, 2, 4, 8, 16, 32)
  subs = []
  for (qx, qy, w, h), angle in zip(quadrants(n), (0, 30, 60, 90)):
    cx, cy = centers(w, h)
    a = math.radians(angle)
    d = (cx - w / 2) * math.cos(a) + (cy - h / 2) * math.sin(a)
    q = np.zeros((h, w))
    for i, s in enumerate(sigmas):
      pos = (i - 2.5) * w / 7
      sign = 1.0 if i % 2 == 0 else -1.0
      q += sign * VALUE_LIMIT * np.exp(-((d - pos) ** 2) / (2 * s * s))
    v[qy : qy + h, qx : qx + w] = q
    subs.append(Subregion(f"angle_{angle}", qx, qy, w, h, {"angle": angle}))
  return _res(v, {"sigmas": list(sigmas)}, subs)


def _unit_xy(n: int) -> tuple[np.ndarray, np.ndarray]:
  cx, cy = centers(n)
  return cx / n * 2 - 1, cy / n * 2 - 1


@register_panel(
    "terrain_saddle", 8, 3, designed_integer=False, label="Saddle x^2 - y^2"
)
def _terrain_saddle(ctx: PanelContext) -> PanelResult:
  x, y = _unit_xy(ctx.size)
  return _res(VALUE_LIMIT * (x * x - y * y))


@register_panel(
    "terrain_monkey_saddle",
    8,
    4,
    designed_integer=False,
    label="Monkey saddle x^3 - 3xy^2",
)
def _terrain_monkey_saddle(ctx: PanelContext) -> PanelResult:
  x, y = _unit_xy(ctx.size)
  z = x**3 - 3 * x * y * y
  return _res(VALUE_LIMIT * z / np.max(np.abs(z)))


@register_panel(
    "terrain_peaks", 8, 5, designed_integer=False, label="MATLAB peaks"
)
def _terrain_peaks(ctx: PanelContext) -> PanelResult:
  return _res(scaled_peaks(ctx.size))


# ---------------------------------------------------------------------------
# Row 9: terraces
# ---------------------------------------------------------------------------


def cone(n: int, m: int | None = None) -> np.ndarray:
  m = n if m is None else m
  cx, cy = centers(n, m)
  r = np.hypot(cx - n / 2, cy - m / 2)
  return np.clip(
      VALUE_LIMIT - 2 * VALUE_LIMIT * r / (min(n, m) / 2),
      -VALUE_LIMIT,
      VALUE_LIMIT,
  )


def terraces(z: np.ndarray, step: float = 100.0) -> np.ndarray:
  return np.clip(step * np.floor(z / step), -VALUE_LIMIT, VALUE_LIMIT)


def soft_terraces(
    z: np.ndarray, step: float = 100.0, sharpness: float = 12.0
) -> np.ndarray:
  m = np.round(z / step)
  return step * (m - 0.5 + 0.5 * np.tanh(sharpness * (z / step - m)))


@register_panel(
    "terrace_cone", 9, 0, designed_integer=True, label="Cone, 20 terraces"
)
def _terrace_cone(ctx: PanelContext) -> PanelResult:
  return _res(terraces(cone(ctx.size)), {"step": 100})


@register_panel(
    "terrace_peaks",
    9,
    1,
    designed_integer=True,
    label="Peaks, 100-unit terraces",
)
def _terrace_peaks(ctx: PanelContext) -> PanelResult:
  return _res(terraces(scaled_peaks(ctx.size)), {"step": 100})


@register_panel(
    "terrace_spiral",
    9,
    2,
    designed_integer=True,
    label="Archimedean spiral terraces",
)
def _terrace_spiral(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, cy = centers(n)
  dx, dy = cx - n / 2, cy - n / 2
  r = np.hypot(dx, dy)
  theta = np.arctan2(dy, dx)
  spacing = max(n / 16, 8)
  level = np.floor(r / spacing - theta / (2 * np.pi)) % 20
  return _res(-1000.0 + 100.0 * level, {"spacing": spacing, "step": 100})


@register_panel(
    "terrace_sinuous",
    9,
    3,
    designed_integer=True,
    label="Meandering and elliptical terraces",
)
def _terrace_sinuous(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, cy = centers(n)
  x, y = cx / n, cy / n
  z = 600 * np.sin(2 * np.pi * (1.5 * x + 0.3 * np.sin(2 * np.pi * 2 * y)))
  z += 400 * np.cos(2 * np.pi * np.hypot((x - 0.5) * 2, (y - 0.5)) * 1.5)
  return _res(terraces(z), {"step": 100})


@register_panel(
    "terrace_fine",
    9,
    4,
    designed_integer=True,
    label="Circular terraces 2-4 px",
)
def _terrace_fine(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, cy = centers(n)
  r = np.hypot(cx - n / 2, cy - n / 2)
  v = np.zeros((n, n))
  subs = []
  for (qx, qy, w, h), width in zip(quadrants(n), (2, 3, 4, 2.5)):
    sl = np.s_[qy : qy + h, qx : qx + w]
    v[sl] = -1000.0 + 100.0 * (np.floor(r[sl] / width) % 20)
    subs.append(
        Subregion(
            f"width_{width}".replace(".", "_"), qx, qy, w, h, {"width": width}
        )
    )
  return _res(v, {"widths": [2, 3, 4, 2.5]}, subs)


@register_panel(
    "terrace_soft_vs_sharp",
    9,
    5,
    designed_integer=False,
    label="Sharp vs tanh-smoothed risers",
)
def _terrace_soft_vs_sharp(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  z = cone(half, n)
  v = np.zeros((n, n))
  v[:, :half] = terraces(z)
  v[:, half:] = soft_terraces(cone(n - half, n))
  subs = [
      Subregion("sharp", 0, 0, half, n),
      Subregion("soft", half, 0, n - half, n),
  ]
  return _res(v, {"step": 100, "sharpness": 12}, subs)


# ---------------------------------------------------------------------------
# Row 10: slopes
# ---------------------------------------------------------------------------


@register_panel("slope_x", 10, 0, designed_integer=False, label="Ramp in x")
def _slope_x(ctx: PanelContext) -> PanelResult:
  cx, _ = centers(ctx.size)
  return _res(-1000.0 + 2000.0 * cx / ctx.size)


@register_panel("slope_y", 10, 1, designed_integer=False, label="Ramp in y")
def _slope_y(ctx: PanelContext) -> PanelResult:
  _, cy = centers(ctx.size)
  return _res(-1000.0 + 2000.0 * cy / ctx.size)


@register_panel(
    "slope_diagonal", 10, 2, designed_integer=False, label="Ramp along x + y"
)
def _slope_diagonal(ctx: PanelContext) -> PanelResult:
  cx, cy = centers(ctx.size)
  return _res(-1000.0 + 1000.0 * (cx + cy) / ctx.size)


@register_panel(
    "slope_gentle",
    10,
    3,
    designed_integer=False,
    label="Gentle ramps 0.1 and 0.37 per px",
)
def _slope_gentle(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  cx, _ = centers(n)
  v = np.zeros((n, n))
  v[:half] = 0.1 * (cx[:half] - n / 2)
  v[half:] = 0.37 * (cx[half:] - n / 2)
  subs = [
      Subregion("slope_0_1", 0, 0, n, half, {"slope": 0.1}),
      Subregion("slope_0_37", 0, half, n, n - half, {"slope": 0.37}),
  ]
  return _res(v, {"slopes": [0.1, 0.37]}, subs)


@register_panel(
    "slope_sawtooth",
    10,
    4,
    designed_integer=False,
    label="Sawtooth 8/32/128 px",
)
def _slope_sawtooth(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  x, _ = grid_xy(n)
  band = n // 3
  v = np.zeros((n, n))
  subs = []
  for i, p in enumerate((8, 32, 128)):
    y1 = n if i == 2 else (i + 1) * band
    v[i * band : y1] = -1000.0 + 2000.0 * ((x[i * band : y1] % p) + 0.5) / p
    subs.append(
        Subregion(f"period_{p}", 0, i * band, n, y1 - i * band, {"period": p})
    )
  return _res(v, {"periods": [8, 32, 128]}, subs)


@register_panel(
    "slope_cone_bowl", 10, 5, designed_integer=False, label="Cone and bowl"
)
def _slope_cone_bowl(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  v = np.zeros((n, n))
  v[:, :half] = cone(half, n)
  v[:, half:] = -cone(n - half, n)
  subs = [
      Subregion("cone", 0, 0, half, n),
      Subregion("bowl", half, 0, n - half, n),
  ]
  exps = [
      Expectation(
          id="bowl_rms_nonnegative",
          check="nonnegative",
          methods=("RMS",),
          subregion="bowl",
      )
  ]
  return _res(v, {}, subs, exps)


# ---------------------------------------------------------------------------
# Row 11: noise (vectorized numpy)
# ---------------------------------------------------------------------------


def _fade(t: np.ndarray) -> np.ndarray:
  return t * t * t * (t * (t * 6 - 15) + 10)


def perlin(n: int, scale: float, rng: np.random.Generator) -> np.ndarray:
  """2D gradient (Perlin) noise with lattice spacing `scale` pixels."""
  cells = int(math.ceil(n / scale)) + 2
  angles = rng.uniform(0, 2 * np.pi, size=(cells, cells))
  gx, gy = np.cos(angles), np.sin(angles)
  c = (np.arange(n) + 0.5) / scale
  i = np.floor(c).astype(np.int64)
  f = c - i
  xi, yi = i[None, :], i[:, None]
  xf, yf = f[None, :], f[:, None]

  def dot(ox: int, oy: int) -> np.ndarray:
    return gx[yi + oy, xi + ox] * (xf - ox) + gy[yi + oy, xi + ox] * (yf - oy)

  u, v = _fade(xf), _fade(yf)
  top = dot(0, 0) * (1 - u) + dot(1, 0) * u
  bottom = dot(0, 1) * (1 - u) + dot(1, 1) * u
  return top * (1 - v) + bottom * v


_F2 = 0.5 * (math.sqrt(3.0) - 1.0)
_G2 = (3.0 - math.sqrt(3.0)) / 6.0
_GRAD2 = np.array(
    [[1, 1], [-1, 1], [1, -1], [-1, -1], [1, 0], [-1, 0], [0, 1], [0, -1]],
    dtype=np.float64,
)


def simplex(n: int, scale: float, rng: np.random.Generator) -> np.ndarray:
  """2D simplex noise with feature size about `scale` pixels."""
  perm = rng.permutation(256)
  perm = np.concatenate([perm, perm])
  cx, cy = centers(n)
  x, y = cx / scale, cy / scale
  s = (x + y) * _F2
  i = np.floor(x + s).astype(np.int64)
  j = np.floor(y + s).astype(np.int64)
  t = (i + j) * _G2
  x0 = x - (i - t)
  y0 = y - (j - t)
  i1 = (x0 > y0).astype(np.int64)
  j1 = 1 - i1
  x1 = x0 - i1 + _G2
  y1 = y0 - j1 + _G2
  x2 = x0 - 1 + 2 * _G2
  y2 = y0 - 1 + 2 * _G2
  ii = np.mod(i, 256)
  jj = np.mod(j, 256)
  g0 = perm[ii + perm[jj]] % 8
  g1 = perm[ii + i1 + perm[jj + j1]] % 8
  g2 = perm[ii + 1 + perm[jj + 1]] % 8
  total = np.zeros_like(x)
  for g, dx, dy in ((g0, x0, y0), (g1, x1, y1), (g2, x2, y2)):
    tt = 0.5 - dx * dx - dy * dy
    contrib = tt**4 * (_GRAD2[g, 0] * dx + _GRAD2[g, 1] * dy)
    total += np.where(tt > 0, contrib, 0.0)
  return total


def fbm(
    n: int,
    scale: float,
    rng: np.random.Generator,
    noise: Callable[[int, float, np.random.Generator], np.ndarray],
    octaves: int = 6,
) -> np.ndarray:
  total = np.zeros((n, n))
  for o in range(octaves):
    total += 0.5**o * noise(n, max(scale / 2**o, 1.0), rng)
  return total


NOISE_SCALES = (8, 32, 128)


def _smooth_noise_panel(
    ctx: PanelContext,
    noise: Callable[[int, float, np.random.Generator], np.ndarray],
) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  subs = []
  quads = quadrants(n)
  for (qx, qy, w, h), scale in zip(quads[:3], NOISE_SCALES):
    v[qy : qy + h, qx : qx + w] = normalize(noise(w, scale, ctx.rng))[:h, :w]
    subs.append(Subregion(f"scale_{scale}", qx, qy, w, h, {"scale": scale}))
  qx, qy, w, h = quads[3]
  v[qy : qy + h, qx : qx + w] = normalize(fbm(w, 128, ctx.rng, noise))[:h, :w]
  subs.append(Subregion("fbm", qx, qy, w, h, {"scale": 128, "octaves": 6}))
  return _res(v, {"scales": list(NOISE_SCALES), "fbm_octaves": 6}, subs)


@register_panel(
    "noise_uniform",
    11,
    0,
    designed_integer=False,
    label="Uniform noise +/-1000",
)
def _noise_uniform(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  return _res(ctx.rng.uniform(-1000, 1000, size=(n, n)))


@register_panel(
    "noise_biased",
    11,
    1,
    designed_integer=False,
    label="Biased noise quadrants",
)
def _noise_biased(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  subs = []
  specs = (
      ("uniform_pos", lambda s: ctx.rng.uniform(0, 1000, s)),
      ("uniform_neg", lambda s: ctx.rng.uniform(-1000, 0, s)),
      ("normal_pos500", lambda s: ctx.rng.normal(500, 150, s)),
      ("normal_neg500", lambda s: ctx.rng.normal(-500, 150, s)),
  )
  for (qx, qy, w, h), (name, fn) in zip(quadrants(n), specs):
    v[qy : qy + h, qx : qx + w] = fn((h, w))
    subs.append(Subregion(name, qx, qy, w, h))
  exps = [
      Expectation(
          id="negative_quadrant_rms_positive",
          check="nonnegative",
          methods=("RMS",),
          subregion="uniform_neg",
      )
  ]
  return _res(np.clip(v, -1000, 1000), {}, subs, exps)


@register_panel(
    "noise_gaussian",
    11,
    2,
    designed_integer=False,
    label="Gaussian noise sigma 300",
)
def _noise_gaussian(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  return _res(
      np.clip(ctx.rng.normal(0, 300, size=(n, n)), -1000, 1000), {"sigma": 300}
  )


@register_panel(
    "noise_perlin",
    11,
    3,
    designed_integer=False,
    label="Perlin noise 8/32/128 px + fBm",
)
def _noise_perlin(ctx: PanelContext) -> PanelResult:
  return _smooth_noise_panel(ctx, perlin)


@register_panel(
    "noise_simplex",
    11,
    4,
    designed_integer=False,
    label="Simplex noise 8/32/128 px + fBm",
)
def _noise_simplex(ctx: PanelContext) -> PanelResult:
  return _smooth_noise_panel(ctx, simplex)


@register_panel(
    "noise_salt_pepper",
    11,
    5,
    designed_integer=True,
    label="Salt and pepper 1/5/20/50 %",
)
def _noise_salt_pepper(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v = np.zeros((n, n))
  subs = []
  for (qx, qy, w, h), p in zip(quadrants(n), (0.01, 0.05, 0.20, 0.50)):
    hit = ctx.rng.random((h, w)) < p
    sign = np.where(ctx.rng.random((h, w)) < 0.5, 1000.0, -1000.0)
    v[qy : qy + h, qx : qx + w] = np.where(hit, sign, 0.0)
    subs.append(
        Subregion(f"density_{int(p * 100)}", qx, qy, w, h, {"density": p})
    )
  return _res(v, {"densities": [0.01, 0.05, 0.2, 0.5]}, subs)


# ---------------------------------------------------------------------------
# Row 12: imagery and text
# ---------------------------------------------------------------------------


@register_panel(
    "img_cat", 12, 0, designed_integer=False, label="Chelsea the cat (skimage)"
)
def _img_cat(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  gray = ctx.assets.cat
  h = round(gray.shape[0] * n / gray.shape[1])
  img = PIL.Image.fromarray(gray.astype(np.float32), mode="F")
  scaled = np.asarray(
      img.resize((n, h), PIL.Image.Resampling.LANCZOS), dtype=np.float64
  )
  v = np.zeros((n, n))
  y0 = (n - h) // 2
  v[y0 : y0 + h] = scaled / 255.0 * 2000.0 - 1000.0
  return _res(
      np.clip(v, -1000, 1000),
      {"source": "skimage chelsea.png", "height": h, "y0": y0},
  )


# 256 px caps make "Google" wider than a 1024 px panel, so the largest is 192.
TEXT_CAP_HEIGHTS = (192, 128, 64, 32, 16, 8)
TEXT = "Google"


def font_for_cap_height(
    assets: Assets, cap: int, scale: int = 1
) -> tuple[PIL.ImageFont.FreeTypeFont, int]:
  """Returns a font whose 'G' is `cap * scale` px tall, and that height."""
  target = cap * scale
  size = max(1, round(target * 100 / 73))
  best = None
  for s in range(max(1, size - 4), size + 5):
    font = assets.font(s)
    b = font.getbbox("G")
    err = abs((b[3] - b[1]) - target)
    if best is None or err < best[0]:
      best = (err, s, b[3] - b[1])
  assert best is not None
  return assets.font(best[1]), int(best[2])


def render_text(
    assets: Assets, n: int, scale: int = 1
) -> tuple[np.ndarray, list[dict[str, Any]]]:
  """Coverage in [0, 1] of 'Google' lines, rendered at n * scale."""
  size = n * scale
  img = PIL.Image.new("L", (size, size), 0)
  draw = PIL.ImageDraw.Draw(img)
  y = 8 * scale
  placed = []
  for cap in TEXT_CAP_HEIGHTS:
    font, measured = font_for_cap_height(assets, cap, scale)
    left, top, right, bottom = draw.textbbox(
        (0, 0), TEXT, font=font, anchor="ls"
    )
    width = right - left
    if width + 16 * scale > size or y - top + bottom > size - 4 * scale:
      continue
    baseline = y - top
    draw.text((8 * scale, baseline), TEXT, font=font, fill=255, anchor="ls")
    placed.append({
        "cap_height": cap,
        "measured_cap_height": measured / scale,
        "baseline": baseline // scale,
        "x": 8,
    })
    y = baseline + bottom + max(cap // 4, 4) * scale
  return np.asarray(img, dtype=np.float64) / 255.0, placed


@register_panel(
    "img_text_google_binary",
    12,
    1,
    designed_integer=True,
    label="'Google' text, binary",
)
def _img_text_google_binary(ctx: PanelContext) -> PanelResult:
  cov, placed = render_text(ctx.assets, ctx.size)
  return _res(pm(cov >= 0.5), {"lines": placed, "font": "Roboto Bold"})


@register_panel(
    "img_text_google_aa",
    12,
    2,
    designed_integer=False,
    label="'Google' text, anti-aliased, inverted",
)
def _img_text_google_aa(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cov, placed = render_text(ctx.assets, n, scale=4)
  cov = cov.reshape(n, 4, n, 4).mean(axis=(1, 3))
  return _res(
      1000.0 - 2000.0 * cov,
      {"lines": placed, "font": "Roboto Bold", "supersample": 4},
  )


@register_panel(
    "img_ee_logo", 12, 3, designed_integer=False, label="Earth Engine logo"
)
def _img_ee_logo(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  rgba = ctx.assets.logo
  border = 12
  side = n - 2 * border
  resampled = False
  if rgba.shape[0] > side or rgba.shape[1] > side:
    img = PIL.Image.fromarray(rgba.astype(np.uint8), mode="RGBA")
    img.thumbnail((side, side), PIL.Image.Resampling.LANCZOS)
    rgba = np.asarray(img, dtype=np.float64)
    resampled = True
  lum = 0.299 * rgba[..., 0] + 0.587 * rgba[..., 1] + 0.114 * rgba[..., 2]
  alpha = rgba[..., 3] / 255.0
  logo = alpha * (lum / 255.0 * 2000.0 - 1000.0)
  v = np.zeros((n, n))
  h, w = logo.shape
  y0, x0 = (n - h) // 2, (n - w) // 2
  v[y0 : y0 + h, x0 : x0 + w] = logo
  return _res(
      v,
      {
          "source": "assets/ee_logo.png",
          "x": x0,
          "y": y0,
          "width": w,
          "height": h,
          "resampled": resampled,
      },
  )


@register_panel(
    "img_landsat",
    12,
    4,
    designed_integer=False,
    label="Landsat 8 B8, Grand Bahama",
)
def _img_landsat(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  chip = ctx.assets.satellite
  y0 = max((chip.shape[0] - n) // 2, 0)
  x0 = max((chip.shape[1] - n) // 2, 0)
  chip = chip[y0 : y0 + n, x0 : x0 + n]
  valid = chip[chip > 0]
  lo, hi = np.percentile(valid, [2, 98])
  v = np.zeros((n, n))
  v[: chip.shape[0], : chip.shape[1]] = -1000 + 2000 * (chip - lo) / (hi - lo)
  return _res(
      np.clip(v, -1000, 1000),
      {
          "source": "assets/landsat_b8_grand_bahama_1024.tif",
          "p2": float(lo),
          "p98": float(hi),
          "crop_x": x0,
          "crop_y": y0,
      },
  )


# ---------------------------------------------------------------------------
# Row 13: nodata
# ---------------------------------------------------------------------------


def nodata_expectations() -> list[Expectation]:
  return [
      Expectation(
          id="nearest_copies_nodata",
          check="equals_reference",
          methods=("NEAREST",),
          reference="nearest",
      ),
  ]


@register_panel(
    "nodata_holes_on_ramp",
    13,
    0,
    designed_integer=False,
    label="Nodata holes 1-64 px on a ramp",
)
def _nodata_holes_on_ramp(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, _ = centers(n)
  v = -1000.0 + 2000.0 * cx / n
  mask = np.zeros((n, n), dtype=bool)
  c = _cells(n)
  sizes = (1, 2, 3, 4, 8, 16, 32, 64)
  placed = []
  for r, size in enumerate(sizes):
    for off in range(8):
      if size + off + 2 > c:
        continue
      x, y = off * c + off + 2, r * c + off + 2
      mask[y : y + size, x : x + size] = True
      placed.append([x, y, size, off])
  return _res(
      v,
      {"holes": placed, "hole_fields": ["x", "y", "size", "offset"]},
      [],
      nodata_expectations(),
      mask,
  )


@register_panel(
    "nodata_random_masks",
    13,
    1,
    designed_integer=False,
    label="Random nodata 5/25/50/95 %",
)
def _nodata_random_masks(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v = normalize(smooth_field(n, n / 16, ctx.rng)) * 0.8
  mask = np.zeros((n, n), dtype=bool)
  subs, exps = [], []
  for (qx, qy, w, h), p in zip(quadrants(n), (0.05, 0.25, 0.50, 0.95)):
    mask[qy : qy + h, qx : qx + w] = ctx.rng.random((h, w)) < p
    sid = f"nodata_{int(p * 100)}"
    subs.append(Subregion(sid, qx, qy, w, h, {"fraction": p}))
    exps.append(
        Expectation(
            id=f"{sid}_average_l1_fraction",
            check="nodata_fraction",
            methods=("AVERAGE",),
            levels=(1,),
            subregion=sid,
            lo=max(0.0, p**4 - 0.05),
            hi=min(1.0, p**4 + 0.05),
        )
    )
  return _res(
      v,
      {"fractions": [0.05, 0.25, 0.5, 0.95]},
      subs,
      exps + nodata_expectations(),
      mask,
  )


@register_panel(
    "nodata_lines_checker",
    13,
    2,
    designed_integer=True,
    label="1 px nodata lines and checker",
)
def _nodata_lines_checker(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  x, y = grid_xy(n)
  v = np.full((n, n), 500.0)
  mask = np.zeros((n, n), dtype=bool)
  q = quadrants(n)
  subs = []
  names = ("h_lines", "v_lines", "checker", "checker_shifted")
  for (qx, qy, w, h), name in zip(q, names):
    sl = np.s_[qy : qy + h, qx : qx + w]
    if name == "h_lines":
      mask[sl] = (y[sl] % 8) == 0
    elif name == "v_lines":
      mask[sl] = (x[sl] % 8) == 0
    elif name == "checker":
      mask[sl] = ((x[sl] + y[sl]) % 2) == 0
    else:
      mask[sl] = ((x[sl] + y[sl]) % 2) == 1
    subs.append(Subregion(name, qx, qy, w, h))
  exps = [
      Expectation(
          id=f"{name}_average_rms_masked",
          check="interior_equals",
          methods=("AVERAGE", "RMS"),
          subregion=name,
          value=500.0,
      )
      for name in ("checker", "checker_shifted", "h_lines", "v_lines")
  ]
  return _res(
      v,
      {"value": 500, "line_spacing": 8},
      subs,
      exps + nodata_expectations(),
      mask,
  )


@register_panel(
    "nodata_step_gaps",
    13,
    3,
    designed_integer=True,
    label="Nodata gaps 1-16 px in a step",
)
def _nodata_step_gaps(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  half = n // 2
  v = np.full((n, n), -1000.0)
  v[:, half:] = 1000.0
  mask = np.zeros((n, n), dtype=bool)
  band = n // 8
  gaps = (1, 2, 3, 4, 6, 8, 12, 16)
  subs = []
  for i, g in enumerate(gaps):
    x0 = half - g // 2
    mask[i * band : (i + 1) * band, x0 : x0 + g] = True
    subs.append(Subregion(f"gap_{g}", 0, i * band, n, band, {"gap": g}))
  return _res(v, {"gaps": list(gaps)}, subs, nodata_expectations(), mask)


@register_panel(
    "nodata_coastline",
    13,
    4,
    designed_integer=False,
    label="Terrain coastline, nodata ocean",
)
def _nodata_coastline(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  cx, _ = centers(n)
  field = smooth_field(n, n / 12, ctx.rng) + (cx / n - 0.5) * 4
  mask = field <= 0
  v = np.clip(field / np.max(field), 0, 1) * 1000.0
  return _res(
      v,
      {"land_fraction": float(1 - mask.mean())},
      [],
      nodata_expectations(),
      mask,
  )


@register_panel(
    "nodata_categorical",
    13,
    5,
    designed_integer=True,
    label="Nodata patches in classes",
)
def _nodata_categorical(ctx: PanelContext) -> PanelResult:
  n = ctx.size
  v, _ = _voronoi(ctx, max(8, round(40 * (n / 1024) ** 2)))
  x, y = grid_xy(n)
  mask = np.zeros((n, n), dtype=bool)
  for _ in range(12):
    px, py = ctx.rng.uniform(0, n, 2)
    r = ctx.rng.uniform(4, n / 12)
    mask |= (x - px) ** 2 + (y - py) ** 2 <= r * r
  for size in (1, 2, 4, 8):
    px, py = ctx.rng.integers(0, n - size, 2)
    mask[py : py + size, px : px + size] = True
  return _res(
      v, {"classes": list(CLASS_CODES)}, [], nodata_expectations(), mask
  )


# ---------------------------------------------------------------------------
# Configuration, disk check
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class GridConfig:
  """Everything needed to write the grids (built from flags or by tests)."""

  output_dir: str
  layout: Layout = Layout()
  dtypes: tuple[str, ...] = DTYPES
  seed: int = DEFAULT_SEED
  nodata: bool = True
  int16_nodata: int = -9999
  float32_nodata: float = float("nan")
  guard_value: float = 0.0
  crs: str = "EPSG:3857"
  pixel_size: float = 10.0
  origin: tuple[float, float] = (0.0, 0.0)
  panels: tuple[str, ...] = ()
  make_cogs: bool = False
  methods: tuple[str, ...] = METHODS
  cog_blocksize: int = 512
  jobs: int = 4
  num_threads: int = 4
  font_path: str | None = None
  logo_path: str | None = None
  satellite_path: str | None = None
  force: bool = False
  cog_config: tuple[tuple[str, str], ...] = ()
  argv: tuple[str, ...] = ()

  def nodata_value(self, dtype: str) -> float | None:
    if not self.nodata:
      return None
    return float(self.int16_nodata) if dtype == "int16" else self.float32_nodata

  def base_path(self, dtype: str) -> str:
    return os.path.join(self.output_dir, f"resample_grid_{dtype}.tif")

  def cog_path(self, dtype: str, method: str) -> str:
    return os.path.join(
        self.output_dir, "cog", dtype, f"resample_grid_{dtype}_{method}.tif"
    )


def make_layout(
    panel_px: int,
    guard_px: int,
    margin_px: int,
    columns: int,
    overview_count: int,
    nodata: bool,
) -> Layout:
  layout = Layout(
      panel_px=panel_px,
      guard_px=guard_px,
      margin_px=margin_px,
      columns=columns,
      rows=rows_for(columns, nodata),
      overview_count=overview_count,
  )
  layout.validate()
  return layout


def selected_specs(config: GridConfig) -> list[PanelSpec]:
  """Panels to generate, in (row, col) order."""
  wanted = set(config.panels)
  out = []
  for spec in sorted(PANELS.values(), key=lambda s: (s.row, s.col)):
    if spec.uses_nodata and not config.nodata:
      continue
    if wanted and spec.panel_id not in wanted and str(spec.row) not in wanted:
      continue
    out.append(spec)
  return out


def estimate_bytes(config: GridConfig) -> int:
  """Worst-case (uncompressed) bytes for the selected outputs (PLAN 13)."""
  pixels = config.layout.width * config.layout.height
  ovr = sum(4.0**-level for level in range(1, config.layout.overview_count + 1))
  total = 0.0
  for dtype in config.dtypes:
    base = pixels * BYTES_PER_PIXEL[dtype]
    total += base
    if config.make_cogs:
      total += len(config.methods) * base * (1 + ovr)
  return int(total)


def check_disk(config: GridConfig) -> None:
  needed = estimate_bytes(config)
  free = shutil.disk_usage(config.output_dir).free
  logging.info(
      "Estimated worst case %.2f GB, free %.2f GB", needed / 1e9, free / 1e9
  )
  if free < 1.2 * needed and not config.force:
    raise RuntimeError(
        f"Not enough free disk in {config.output_dir}: need 1.2 x "
        f"{needed / 1e9:.2f} GB, have {free / 1e9:.2f} GB (use --force)"
    )


# ---------------------------------------------------------------------------
# Base grid writer
# ---------------------------------------------------------------------------


def gtiff_options(dtype: str, num_threads: int) -> list[str]:
  return [
      "TILED=YES",
      "BLOCKXSIZE=512",
      "BLOCKYSIZE=512",
      *tiff_compression_options("GTiff"),
      f"PREDICTOR={PREDICTORS[dtype]}",
      "BIGTIFF=IF_SAFER",
      f"NUM_THREADS={num_threads}",
  ]


def _srs(crs: str) -> osr.SpatialReference:
  srs = osr.SpatialReference()
  srs.SetFromUserInput(crs)
  srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
  return srs


def geotransform(config: GridConfig) -> list[float]:
  ox, oy = config.origin
  px = config.pixel_size
  return [ox, px, 0.0, oy, 0.0, -px]


def layout_json(layout: Layout) -> dict[str, Any]:
  return {
      "width": layout.width,
      "height": layout.height,
      "raw_width": layout.raw_width,
      "raw_height": layout.raw_height,
      "panel_px": layout.panel_px,
      "guard_px": layout.guard_px,
      "margin_px": layout.margin_px,
      "columns": layout.columns,
      "rows": layout.rows,
      "overview_factors": [2**lv for lv in range(1, layout.overview_count + 1)],
  }


def sha256_band(path: str, strip_rows: int = 512) -> str:
  """SHA-256 of the band's native-endian raw bytes (NaN-safe)."""
  ds = gdal.Open(path)
  band = ds.GetRasterBand(1)
  digest = hashlib.sha256()
  for y in range(0, ds.RasterYSize, strip_rows):
    rows = min(strip_rows, ds.RasterYSize - y)
    digest.update(
        np.ascontiguousarray(
            band.ReadAsArray(0, y, ds.RasterXSize, rows)
        ).tobytes()
    )
  ds = None
  return digest.hexdigest()


def _json_default(obj: Any) -> Any:
  if isinstance(obj, np.integer):
    return int(obj)
  if isinstance(obj, np.floating):
    return float(obj)
  if isinstance(obj, np.ndarray):
    return obj.tolist()
  raise TypeError(f"Not JSON serializable: {type(obj)}")


def _nodata_json(value: float | None) -> Any:
  if value is None:
    return None
  if math.isnan(value):
    return "nan"
  return value


def panel_entry(
    spec: PanelSpec,
    result: PanelResult,
    layout: Layout,
    origin: tuple[int, int],
    slot: tuple[int, int],
) -> dict[str, Any]:
  """The manifest record for one panel."""
  levels = range(1, layout.overview_count + 1)
  values = result.values
  valid = values if result.nodata_mask is None else values[~result.nodata_mask]
  distinct = np.unique(valid)
  return {
      "id": spec.panel_id,
      "row": spec.row,
      "col": spec.col,
      "slot": list(slot),
      "family": spec.family,
      "label": spec.label,
      "x": origin[0],
      "y": origin[1],
      "width": layout.panel_px,
      "height": layout.panel_px,
      "block_offset": {
          str(lv): list(layout.block_offset(slot[0], slot[1], lv))
          for lv in levels
      },
      "inner_window": {
          str(lv): list(layout.inner_window(slot[0], slot[1], lv))
          for lv in levels
      },
      "designed_integer": spec.designed_integer,
      "uses_nodata": spec.uses_nodata,
      "params": result.params,
      "input_stats": {
          "min": float(valid.min()) if valid.size else None,
          "max": float(valid.max()) if valid.size else None,
          "distinct": int(min(distinct.size, 4096)),
          "nodata_count": int(values.size - valid.size),
      },
      "subregions": [s.to_json() for s in result.subregions],
      "expectations": [e.to_json() for e in result.expectations],
  }


def write_grids(config: GridConfig, assets: Assets) -> list[dict[str, Any]]:
  """Writes the base grids; returns the manifest panel entries."""
  layout = config.layout
  srs = _srs(config.crs)
  driver = gdal.GetDriverByName("GTiff")
  datasets = {}
  for dtype in config.dtypes:
    ds = driver.Create(
        config.base_path(dtype),
        layout.width,
        layout.height,
        1,
        GDAL_TYPES[dtype],
        options=gtiff_options(dtype, config.num_threads),
    )
    ds.SetGeoTransform(geotransform(config))
    ds.SetSpatialRef(srs)
    band = ds.GetRasterBand(1)
    nodata = config.nodata_value(dtype)
    if nodata is not None:
      band.SetNoDataValue(nodata)
    band.Fill(config.guard_value)
    datasets[dtype] = ds
  entries = []
  for spec in selected_specs(config):
    slot = slot_for(spec.row, spec.col, layout.columns)
    origin = layout.panel_origin(*slot)
    ctx = make_context(spec, layout, config.seed, assets, origin)
    start = time.time()
    result = run_panel(spec, ctx)
    for dtype, ds in datasets.items():
      arr = to_dtype(
          result.values,
          result.nodata_mask,
          dtype,
          config.int16_nodata,
          config.float32_nodata,
      )
      ds.GetRasterBand(1).WriteArray(arr, origin[0], origin[1])
    entries.append(panel_entry(spec, result, layout, origin, slot))
    logging.info("Panel %-28s %.2f s", spec.panel_id, time.time() - start)
  for dtype, ds in datasets.items():
    ds.SetMetadata({
        "RESAMPLE_GRID_SEED": str(config.seed),
        "RESAMPLE_GRID_VERSION": VERSION,
        "RESAMPLE_GRID_LAYOUT": json.dumps(layout_json(layout)),
        "GDAL_VERSION": gdal.VersionInfo("RELEASE_NAME"),
        "RESAMPLE_GRID_DTYPE": dtype,
    })
    ds.FlushCache()
  datasets.clear()
  return entries


# ---------------------------------------------------------------------------
# Manifest and GeoJSON
# ---------------------------------------------------------------------------


def methods_json() -> dict[str, Any]:
  out: dict[str, Any] = dict(EE_TO_GDAL)
  out["ee_default"] = dict(EE_DEFAULT)
  return out


def build_manifest(
    config: GridConfig,
    entries: list[dict[str, Any]],
    files: dict[str, Any],
    timing: dict[str, float],
) -> dict[str, Any]:
  grid = layout_json(config.layout)
  grid.update({
      "crs": config.crs,
      "geotransform": geotransform(config),
      "nodata": {d: _nodata_json(config.nodata_value(d)) for d in DTYPES},
      "guard_value": config.guard_value,
  })
  return {
      "schema_version": 1,
      "generator": {
          "name": "make_resample_grids",
          "version": VERSION,
          "argv": list(config.argv),
          "seed": config.seed,
          "gdal_version": gdal.VersionInfo("RELEASE_NAME"),
          "numpy_version": np.__version__,
          "compression": {"compress": COMPRESS, "zstd_level": ZSTD_LEVEL},
      },
      "grid": grid,
      "methods": methods_json(),
      "files": files,
      "timing_seconds": timing,
      "panels": entries,
  }


def write_json(path: str, data: Any) -> None:
  with open(path, "w") as f:
    json.dump(data, f, indent=1, default=_json_default, allow_nan=False)
    f.write("\n")


def ring_area(ring: Sequence[Sequence[float]]) -> float:
  """Signed shoelace area; positive means counter-clockwise."""
  area = 0.0
  for (x1, y1), (x2, y2) in zip(ring[:-1], ring[1:]):
    area += x1 * y2 - x2 * y1
  return area / 2


def build_geojson(
    config: GridConfig, entries: list[dict[str, Any]]
) -> dict[str, Any]:
  """Panels and subregions as WGS84 polygons (RFC 7946)."""
  src = _srs(config.crs)
  dst = _srs("EPSG:4326")
  transform = osr.CoordinateTransformation(src, dst)
  gt = geotransform(config)

  def polygon(x: float, y: float, w: float, h: float) -> list[list[float]]:
    corners = [(x, y + h), (x + w, y + h), (x + w, y), (x, y), (x, y + h)]
    ring = []
    for px, py in corners:
      gx = gt[0] + px * gt[1]
      gy = gt[3] + py * gt[5]
      lon, lat, _ = transform.TransformPoint(gx, gy)
      ring.append([round(lon, 9), round(lat, 9)])
    return ring

  features = []
  for e in entries:
    off32 = e["block_offset"].get(str(config.layout.overview_count))
    features.append({
        "type": "Feature",
        "geometry": {
            "type": "Polygon",
            "coordinates": [polygon(e["x"], e["y"], e["width"], e["height"])],
        },
        "properties": {
            "id": e["id"],
            "kind": "panel",
            "row": e["row"],
            "col": e["col"],
            "family": e["family"],
            "label": e["label"],
            "block_offset_32": off32,
        },
    })
    for s in e["subregions"]:
      features.append({
          "type": "Feature",
          "geometry": {
              "type": "Polygon",
              "coordinates": [
                  polygon(e["x"] + s["x"], e["y"] + s["y"], s["w"], s["h"])
              ],
          },
          "properties": {
              "id": f"{e['id']}/{s['id']}",
              "kind": "subregion",
              "row": e["row"],
              "col": e["col"],
              "family": e["family"],
              "label": s["id"],
              "block_offset_32": off32,
          },
      })
  return {"type": "FeatureCollection", "features": features}


# ---------------------------------------------------------------------------
# COG-driver references
# ---------------------------------------------------------------------------


def cog_options(
    method: str, overview_count: int, blocksize: int, num_threads: int
) -> list[str]:
  return [
      f"OVERVIEW_RESAMPLING={method}",
      f"OVERVIEW_COUNT={overview_count}",
      "OVERVIEWS=IGNORE_EXISTING",
      f"BLOCKSIZE={blocksize}",
      *tiff_compression_options("COG"),
      "PREDICTOR=YES",
      "BIGTIFF=IF_SAFER",
      f"NUM_THREADS={num_threads}",
  ]


@dataclasses.dataclass(frozen=True)
class CogJob:
  src: str
  dst: str
  dtype: str
  method: str
  options: tuple[str, ...]
  config: tuple[tuple[str, str], ...] = ()


class CogBuildError(RuntimeError):
  pass


def build_cog(job: CogJob) -> dict[str, Any]:
  """Builds one COG (thread-safe: config options are thread-local)."""
  gdal.UseExceptions()
  start = time.time()
  try:
    for key, value in job.config:
      gdal.SetThreadLocalConfigOption(key, value)
    os.makedirs(os.path.dirname(job.dst), exist_ok=True)
    gdal.Translate(
        job.dst, job.src, format="COG", creationOptions=list(job.options)
    )
  except Exception as e:  # pylint: disable=broad-exception-caught
    raise CogBuildError(
        f"COG build failed for {job.dst} (dtype={job.dtype}, "
        f"method={job.method}): {e}"
    ) from e
  finally:
    for key, _ in job.config:
      gdal.SetThreadLocalConfigOption(key, None)
  return {
      "path": job.dst,
      "dtype": job.dtype,
      "method": job.method,
      "bytes": os.path.getsize(job.dst),
      "seconds": round(time.time() - start, 2),
  }


def cog_jobs(config: GridConfig) -> list[CogJob]:
  jobs = []
  for dtype in config.dtypes:
    for method in config.methods:
      jobs.append(
          CogJob(
              src=config.base_path(dtype),
              dst=config.cog_path(dtype, method),
              dtype=dtype,
              method=method,
              options=tuple(
                  cog_options(
                      method,
                      config.layout.overview_count,
                      config.cog_blocksize,
                      config.num_threads,
                  )
              ),
              config=config.cog_config,
          )
      )
  return jobs


def run_cog_jobs(
    jobs: Sequence[CogJob],
    workers: int,
    fn: Callable[[CogJob], dict[str, Any]] = build_cog,
) -> list[dict[str, Any]]:
  """Runs jobs in parallel; raises CogBuildError listing every failure."""
  results, errors = [], []
  if workers <= 1:
    for job in jobs:
      try:
        results.append(fn(job))
      except Exception as e:  # pylint: disable=broad-exception-caught
        errors.append(f"{job.dtype}/{job.method} ({job.dst}): {e}")
  else:
    # The embedded Python has no sys.executable, so multiprocessing
    # cannot spawn workers, and forking after GDAL started its worker threads
    # is unsafe. gdal.Translate releases the GIL, so threads run in parallel.
    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
      futures = {ex.submit(fn, job): job for job in jobs}
      for fut in concurrent.futures.as_completed(futures):
        job = futures[fut]
        try:
          results.append(fut.result())
        except Exception as e:  # pylint: disable=broad-exception-caught
          errors.append(f"{job.dtype}/{job.method} ({job.dst}): {e}")
  if errors:
    raise CogBuildError("; ".join(errors))
  order = {(j.dtype, j.method): i for i, j in enumerate(jobs)}
  return sorted(results, key=lambda r: order[(r["dtype"], r["method"])])


def check_overviews(
    path: str,
    layout: Layout,
    dtype: str,
    nodata: float | None,
    blocksize: int = 512,
) -> list[str]:
  """Returns a list of problems with a COG (empty if it is fine)."""
  problems = []
  ds = gdal.Open(path)
  band = ds.GetRasterBand(1)
  if (ds.RasterXSize, ds.RasterYSize) != (layout.width, layout.height):
    problems.append(f"size {ds.RasterXSize}x{ds.RasterYSize}")
  if band.DataType != GDAL_TYPES[dtype]:
    problems.append(f"data type {gdal.GetDataTypeName(band.DataType)}")
  if band.GetOverviewCount() != layout.overview_count:
    problems.append(f"overview count {band.GetOverviewCount()}")
  if list(band.GetBlockSize()) != [blocksize, blocksize]:
    problems.append(f"block size {band.GetBlockSize()}")
  for i in range(band.GetOverviewCount()):
    ovr = band.GetOverview(i)
    want = layout.overview_shape(i + 1)
    if (ovr.XSize, ovr.YSize) != want:
      problems.append(
          f"overview {i + 1} size {ovr.XSize}x{ovr.YSize} != {want}"
      )
    if list(ovr.GetBlockSize()) != [blocksize, blocksize]:
      problems.append(f"overview {i + 1} block size {ovr.GetBlockSize()}")
  got = band.GetNoDataValue()
  if nodata is None:
    if got is not None:
      problems.append(f"unexpected nodata {got}")
  elif got is None or not (
      (math.isnan(nodata) and math.isnan(got)) or got == nodata
  ):
    problems.append(f"nodata {got} != {nodata}")
  md = ds.GetMetadata("IMAGE_STRUCTURE")
  if md.get("LAYOUT") != "COG":
    problems.append(f"LAYOUT={md.get('LAYOUT')}")
  if md.get("COMPRESSION") != COMPRESS:
    problems.append(f"COMPRESSION={md.get('COMPRESSION')}")
  ds = None
  return problems


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------


def generate(config: GridConfig) -> dict[str, Any]:
  """Writes grids, manifest, GeoJSON and (optionally) COGs."""
  os.makedirs(config.output_dir, exist_ok=True)
  check_disk(config)
  assets = Assets(config.font_path, config.logo_path, config.satellite_path)
  timing: dict[str, float] = {}
  start = time.time()
  entries = write_grids(config, assets)
  timing["grids"] = round(time.time() - start, 2)
  files: dict[str, Any] = {}
  for dtype in config.dtypes:
    path = config.base_path(dtype)
    files[dtype] = {
        "path": os.path.basename(path),
        "sha256": sha256_band(path),
        "bytes": os.path.getsize(path),
    }
  files["cogs"] = []
  manifest = build_manifest(config, entries, files, timing)
  write_json(os.path.join(config.output_dir, "manifest.json"), manifest)
  write_json(
      os.path.join(config.output_dir, "panels.geojson"),
      build_geojson(config, entries),
  )
  if config.make_cogs:
    start = time.time()
    results = run_cog_jobs(cog_jobs(config), config.jobs)
    timing["cogs"] = round(time.time() - start, 2)
    problems = []
    for r in results:
      bad = check_overviews(
          r["path"],
          config.layout,
          r["dtype"],
          config.nodata_value(r["dtype"]),
          config.cog_blocksize,
      )
      problems += [f"{r['path']}: {p}" for p in bad]
      r["path"] = os.path.relpath(r["path"], config.output_dir)
      r["ee_default"] = EE_DEFAULT[r["dtype"]] == r["method"]
    if problems:
      raise CogBuildError("; ".join(problems))
    files["cogs"] = results
    manifest = build_manifest(config, entries, files, timing)
    write_json(os.path.join(config.output_dir, "manifest.json"), manifest)
  return manifest


def config_from_flags(argv: Sequence[str]) -> GridConfig:
  """Validates flags and builds a GridConfig."""
  if not _OUTPUT_DIR.value:
    raise app.UsageError("--output_dir is required")
  if (
      not -32768 <= _INT16_NODATA.value <= 32767
      or abs(_INT16_NODATA.value) <= VALUE_LIMIT
  ):
    raise app.UsageError("--int16_nodata must fit Int16 and be outside +/-1000")
  try:
    f32_nodata = parse_float32_nodata(_FLOAT32_NODATA.value)
  except ValueError as e:
    raise app.UsageError(str(e)) from e
  dtypes = tuple(d.lower() for d in _DTYPES.value)
  if not dtypes or set(dtypes) - set(DTYPES):
    raise app.UsageError(f"--dtypes must be a subset of {DTYPES}")
  try:
    methods = tuple(ee_to_gdal(m) for m in _METHODS.value)
  except ValueError as e:
    raise app.UsageError(str(e)) from e
  known = set(PANELS) | {str(r) for r in range(ROWS)}
  unknown = set(_PANELS.value) - known
  if unknown:
    raise app.UsageError(f"Unknown --panels: {sorted(unknown)}")
  nodata = _NODATA.value == "on"
  panel_px = 256 if _SMALL.value else _PANEL_PX.value
  try:
    layout = make_layout(
        panel_px,
        _GUARD_PX.value,
        _MARGIN_PX.value,
        _COLUMNS.value,
        _OVERVIEW_COUNT.value,
        nodata,
    )
  except ValueError as e:
    raise app.UsageError(str(e)) from e
  return GridConfig(
      output_dir=_OUTPUT_DIR.value,
      layout=layout,
      dtypes=dtypes,
      seed=_SEED.value,
      nodata=nodata,
      int16_nodata=_INT16_NODATA.value,
      float32_nodata=f32_nodata,
      guard_value=_GUARD_VALUE.value,
      crs=_CRS.value,
      pixel_size=_PIXEL_SIZE.value,
      origin=(float(_ORIGIN.value[0]), float(_ORIGIN.value[1])),
      panels=tuple(_PANELS.value),
      make_cogs=_MAKE_COGS.value,
      methods=methods,
      cog_blocksize=_COG_BLOCKSIZE.value,
      jobs=_JOBS.value,
      num_threads=_NUM_THREADS.value,
      font_path=_FONT_PATH.value,
      logo_path=_LOGO_PATH.value,
      satellite_path=_SATELLITE_PATH.value,
      force=_FORCE.value,
      argv=tuple(argv),
  )


def main(argv: Sequence[str]) -> None:
  config = config_from_flags(sys.argv if argv is None else argv)
  start = time.time()
  manifest = generate(config)
  self_rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024
  child_rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss / 1024
  print(
      json.dumps(
          {
              "output_dir": config.output_dir,
              "seconds": round(time.time() - start, 1),
              "timing_seconds": manifest["timing_seconds"],
              "peak_rss_mb": {
                  "self": round(self_rss),
                  "max_child": round(child_rss),
              },
              "files": {
                  k: v for k, v in manifest["files"].items() if k != "cogs"
              },
              "cogs": [
                  (c["path"], c["bytes"], c["seconds"])
                  for c in manifest["files"]["cogs"]
              ],
          },
          indent=1,
      )
  )


if __name__ == "__main__":
  app.run(main)
