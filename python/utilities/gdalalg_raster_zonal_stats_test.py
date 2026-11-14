# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tests for `gdal raster zonal-stats`.

Ported from upstream autotest/utilities/test_gdalalg_raster_zonal_stats.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

from collections.abc import Iterator, Sequence
import contextlib
import math
import struct
import sys

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_zonal_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  return reg.InstantiateAlg("raster").InstantiateSubAlgorithm("zonal-stats")


def _have_geos() -> bool:
  return ogr.GetGEOSVersionMajor() > 0


def _have_fractional_pixels() -> bool:
  return _have_geos() and (
      ogr.GetGEOSVersionMajor(),
      ogr.GetGEOSVersionMinor(),
      ogr.GetGEOSVersionMicro(),
  ) >= (3, 14, 1)


def _wkt_ds(
    wkts: str | Sequence[str],
    *,
    geom_type: int | None = None,
    epsg: int | None = None,
) -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").CreateVector("")
  srs = osr.SpatialReference(epsg=epsg) if epsg else None
  if srs is not None:
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
  lyr = ds.CreateLayer(
      "polys",
      srs=srs,
      geom_type=geom_type if geom_type is not None else ogr.wkbUnknown,
  )
  wkt_list = [wkts] if isinstance(wkts, str) else list(wkts)
  for idx, wkt in enumerate(wkt_list):
    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt(wkt)
    assert geom is not None
    feat.SetGeometry(geom)
    feat.SetFID(idx + 1)
    lyr.CreateFeature(feat)
  return ds


def _make_polyrast() -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").Create("", 20, 20, 1, eType=gdal.GDT_Int16)
  ds.SetSpatialRef(osr.SpatialReference(epsg=27700))
  ds.SetGeoTransform((478000, 200, 0, 4766000, 0, -200))
  ds.WriteRaster(0, 0, 20, 20, struct.pack("@400h", *range(400)))
  return ds


def _field_names(feat: ogr.Feature) -> list[str]:
  return [
      feat.GetFieldDefnRef(i).GetName() for i in range(feat.GetFieldCount())
  ]


def _all_features(lyr: ogr.Layer) -> list[ogr.Feature]:
  features = []
  while True:
    feat = lyr.GetNextFeature()
    if feat is None:
      break
    features.append(feat)
  return features


class GdalalgRasterZonalStatsTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    self.rgbsmall_tif = utilities_util.get_testdata_path(
        "../gcore/data/gtiff/rgbsmall_NONE_tiled.tif"
    )
    self.poly_shp = utilities_util.get_testdata_path("../ogr/data/poly.shp")

  @contextlib.contextmanager
  def _error_raised(self, err_type: int, match: str = "") -> Iterator[None]:
    errors: list[tuple[int, int, str]] = []

    def handler(lvl: int, num: int, msg: str) -> None:
      errors.append((lvl, num, msg))

    gdal.PushErrorHandler(handler)
    try:
      yield
    finally:
      gdal.PopErrorHandler()

    if err_type == gdal.CE_None:
      non_debug = [e for e in errors if e[0] != gdal.CE_Debug]
      self.assertEqual(non_debug, [])
    else:
      self.assertTrue(
          any(lvl == err_type and match in msg for lvl, _, msg in errors),
          msg=f"Expected {err_type} matching {match!r}, got {errors}",
      )

  def _skip_for_strategy_or_pixels(
      self, strategy: str, pixels: str = "default"
  ) -> None:
    if strategy == "raster" and not _have_geos():
      self.skipTest("--strategy raster requires GEOS")
    if pixels == "fractional" and not _have_fractional_pixels():
      self.skipTest("--pixels fractional requires GEOS >= 3.14.1")

  def test_zones_dataset_empty(self) -> None:
    zonal = _get_zonal_alg()
    zonal["input"] = gdal.Open(self.rgbsmall_tif)
    zonal["zones"] = gdal.GetDriverByName("MEM").Create("", 1, 1, 0)
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = "sum"
    with self.assertRaisesRegex(
        RuntimeError, "Zones dataset has no band or layer"
    ):
      zonal.Run()

  def test_no_and_non_invertible_geotransform(self) -> None:
    zonal = _get_zonal_alg()
    zonal["input"] = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    zonal["zones"] = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = "sum"
    with self.assertRaisesRegex(RuntimeError, "Dataset has no geotransform"):
      zonal.Run()

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    src_ds.SetGeoTransform([0] * 6)
    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["zones"] = gdal.GetDriverByName("MEM").Create("", 1, 1, 1)
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = "sum"
    with self.assertRaisesRegex(
        RuntimeError, "Dataset geotransform cannot be inverted"
    ):
      zonal.Run()

  @parameterized.product(
      strategy=("raster", "feature"),
      pixels=("default", "all-touched", "fractional"),
      band=(1, 2, (3, 1)),
  )
  def test_polygon_zones_basic(
      self, strategy: str, pixels: str, band: int | tuple[int, ...]
  ) -> None:
    self._skip_for_strategy_or_pixels(strategy, pixels)

    src_ds = gdal.Open(self.rgbsmall_tif)
    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["zones"] = _wkt_ds([
        (
            "POLYGON((-44.8151 -22.9973, -44.7357 -22.9979,"
            "-44.7755 -22.9563,-44.8151 -22.9973))"
        ),
        (
            "POLYGON((-44.7671 -23.0347,-44.7683 -23.0678,"
            "-44.7183 -23.0686,-44.7191 -23.0339,-44.7671 -23.0347))"
        ),
    ])
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["strategy"] = strategy
    zonal["pixels"] = pixels
    stats = ["sum", "mean"]
    if band == 1:
      stats.extend(["max", "max_center_x", "max_center_y"])
    zonal["stat"] = stats
    zonal["band"] = list(band) if isinstance(band, tuple) else band
    zonal["chunk-size"] = "2k"
    zonal["layer-creation-option"] = {"FID": "my_fid"}
    self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    self.assertEqual(out_ds.GetLayer(0).GetFIDColumn(), "my_fid")
    results = _all_features(out_ds.GetLayer(0))
    self.assertLen(results, 2)

    if band == 1:
      if pixels == "fractional":
        self.assertAlmostEqual(results[0]["sum"], 7130.21435281425, places=4)
        self.assertAlmostEqual(results[1]["sum"], 12886.7218339441, places=4)
      elif pixels == "all-touched":
        self.assertEqual(results[0]["sum"], 8251)
        self.assertEqual(results[1]["sum"], 15131)
      else:
        self.assertEqual(results[0]["sum"], 7148)
        self.assertEqual(results[1]["sum"], 12497)
        inv_gt = gdal.InvGeoTransform(src_ds.GetGeoTransform())
        band1 = src_ds.GetRasterBand(1)
        for feat in results:
          px = gdal.ApplyGeoTransform(
              inv_gt, feat["max_center_x"], feat["max_center_y"]
          )
          raw = band1.ReadRaster(
              int(px[0]), int(px[1]), 1, 1, buf_type=gdal.GDT_Int32
          )
          assert raw is not None
          self.assertEqual(struct.unpack("@i", raw)[0], feat["max"])
    elif band == 2:
      if pixels == "fractional":
        self.assertAlmostEqual(results[0]["sum"], 9743.76203341247, places=4)
        self.assertAlmostEqual(results[1]["sum"], 18396.5491179946, places=4)
      elif pixels == "all-touched":
        self.assertEqual(results[0]["sum"], 11232)
        self.assertEqual(results[1]["sum"], 21693)
      else:
        self.assertEqual(results[0]["sum"], 9764)
        self.assertEqual(results[1]["sum"], 17917)
    else:
      self.assertEqual(results[0].GetFieldCount(), 4)
      self.assertEqual(
          _field_names(results[0]),
          ["sum_band_3", "mean_band_3", "sum_band_1", "mean_band_1"],
      )
      if pixels == "fractional":
        self.assertAlmostEqual(
            results[0]["sum_band_1"], 7130.21435281425, places=4
        )
        self.assertAlmostEqual(
            results[1]["sum_band_1"], 12886.7218339441, places=4
        )
      elif pixels == "all-touched":
        self.assertEqual(results[0]["sum_band_1"], 8251)
        self.assertEqual(results[1]["sum_band_1"], 15131)
      else:
        self.assertEqual(results[0]["sum_band_1"], 7148)
        self.assertEqual(results[1]["sum_band_1"], 12497)

  @parameterized.product(
      strategy=("raster", "feature"),
      pixels=("all-touched", "fractional"),
  )
  def test_polygon_zones_weighted(self, strategy: str, pixels: str) -> None:
    self._skip_for_strategy_or_pixels(strategy, pixels)

    values = [1, 2, 3, 4, 9, 10, 11, 12, 5, 6, 7, 8]
    weights = [0.3] * 4 + [0.1] * 4 + [0.2] * 4
    nx, ny = 4, 3

    values_ds = gdal.GetDriverByName("MEM").Create("", nx, ny)
    values_ds.SetGeoTransform((0, 1, 0, ny, 0, -1))
    values_ds.WriteRaster(0, 0, nx, ny, bytes(values))

    weights_ds = gdal.GetDriverByName("MEM").Create(
        "", nx, ny, eType=gdal.GDT_Float64
    )
    weights_ds.SetGeoTransform((0, 1, 0, ny, 0, -1))
    weights_ds.WriteRaster(0, 0, nx, ny, struct.pack("@12d", *weights))

    zonal = _get_zonal_alg()
    zonal["input"] = values_ds
    zonal["weights"] = weights_ds
    zonal["zones"] = _wkt_ds(
        ["POLYGON ((0.5 0.5, 3.5 0.5, 3.5 2.5, 0.5 2.5, 0.5 0.5))"]
    )
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["strategy"] = strategy
    zonal["pixels"] = pixels
    zonal["stat"] = [
        "mean",
        "weighted_mean",
        "weighted_sum",
        "weighted_variance",
        "weighted_stdev",
    ]
    self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    feat = out_ds.GetLayer(0).GetNextFeature()
    if pixels == "all-touched":
      w_sum = sum(v * w for v, w in zip(values, weights))
      self.assertAlmostEqual(feat["mean"], sum(values) / len(values))
      self.assertAlmostEqual(feat["weighted_mean"], w_sum / sum(weights))
      self.assertAlmostEqual(feat["weighted_sum"], w_sum)
      self.assertAlmostEqual(feat["weighted_variance"], 10.13889, places=4)
      self.assertAlmostEqual(feat["weighted_stdev"], 3.184162, places=4)
    else:
      cov = [0.25, 0.5, 0.5, 0.25, 0.5, 1.0, 1.0, 0.5, 0.25, 0.5, 0.5, 0.25]
      mean_val = sum(v * c for v, c in zip(values, cov)) / sum(cov)
      w_cov = [w * c for w, c in zip(weights, cov)]
      w_sum = sum(v * wc for v, wc in zip(values, w_cov))
      self.assertAlmostEqual(feat["mean"], mean_val)
      self.assertAlmostEqual(feat["weighted_mean"], w_sum / sum(w_cov))
      self.assertAlmostEqual(feat["weighted_sum"], w_sum)

  @parameterized.parameters((1,), (2,), ((3, 1),))
  def test_raster_zones(self, band: int | tuple[int, ...]) -> None:
    src_ds = gdal.Open(self.rgbsmall_tif)
    nx, ny = src_ds.RasterXSize, src_ds.RasterYSize

    band1_raw = src_ds.GetRasterBand(1).ReadRaster(0, 0, nx, ny)
    assert band1_raw is not None
    zones = [int(round(v / 51.0)) for v in band1_raw]
    zones_ds = gdal.GetDriverByName("MEM").Create("", nx, ny)
    zones_ds.SetGeoTransform(src_ds.GetGeoTransform())
    zones_ds.WriteRaster(0, 0, nx, ny, bytes(zones))

    weights = [i / 1000.0 for i in range(nx * ny)]
    weights_ds = gdal.GetDriverByName("MEM").Create(
        "", nx, ny, eType=gdal.GDT_Float32
    )
    weights_ds.SetGeoTransform(src_ds.GetGeoTransform())
    weights_ds.WriteRaster(0, 0, nx, ny, struct.pack(f"@{nx * ny}f", *weights))
    w_raw = weights_ds.ReadRaster(0, 0, nx, ny)
    assert w_raw is not None
    w_vals = struct.unpack(f"@{nx * ny}f", w_raw)

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["weights"] = weights_ds
    zonal["zones"] = zones_ds
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = [
        "sum",
        "mean",
        "weighted_mean",
        "max",
        "max_center_y",
        "max_center_x",
    ]
    zonal["band"] = list(band) if isinstance(band, tuple) else band
    zonal["chunk-size"] = "2k"
    self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    results = _all_features(out_ds.GetLayer(0))
    unique_zones = sorted(set(zones))
    self.assertLen(results, len(unique_zones))

    bands = [band] if isinstance(band, int) else list(band)
    inv_gt = gdal.InvGeoTransform(src_ds.GetGeoTransform())
    for b_idx in bands:
      src_raw = src_ds.GetRasterBand(b_idx).ReadRaster(0, 0, nx, ny)
      assert src_raw is not None
      src_vals = list(src_raw)

      def stat_field(name: str, band_num: int = b_idx) -> str:
        return name if len(bands) == 1 else f"{name}_band_{band_num}"

      for i, zval in enumerate(unique_zones):
        self.assertEqual(results[i]["value"], zval)
        idxs = [k for k, zv in enumerate(zones) if zv == zval]
        self.assertEqual(
            results[i][stat_field("sum")], sum(src_vals[k] for k in idxs)
        )
        expected_wm = sum(src_vals[k] * w_vals[k] for k in idxs) / sum(
            w_vals[k] for k in idxs
        )
        self.assertAlmostEqual(
            results[i][stat_field("weighted_mean")], expected_wm, places=4
        )
        expected_max = max(src_vals[k] for k in idxs)
        self.assertEqual(results[i][stat_field("max")], expected_max)
        px = gdal.ApplyGeoTransform(
            inv_gt,
            results[i][stat_field("max_center_x")],
            results[i][stat_field("max_center_y")],
        )
        self.assertEqual(
            src_vals[int(px[1]) * nx + int(px[0])],
            results[i][stat_field("max")],
        )

  @parameterized.product(
      strategy=("raster", "feature"),
      stat=(
          "center_x",
          "center_y",
          "count",
          "coverage",
          "max",
          "mean",
          "min",
          "minority",
          "mode",
          "stdev",
          "sum",
          ("frac", "unique"),
          "variance",
          "variety",
          "values",
          "max_center_x",
          "max_center_y",
          "min_center_x",
          "min_center_y",
          "weighted_mean",
          "weighted_sum",
          "weighted_stdev",
          "weighted_variance",
          "weights",
      ),
  )
  def test_polygon_zones_all_stats(
      self, strategy: str, stat: str | tuple[str, ...]
  ) -> None:
    self._skip_for_strategy_or_pixels(strategy, "fractional")

    nodata = 99
    values = [
        1,
        1,
        1,
        1,
        1,
        1,
        1,
        2,
        3,
        1,
        1,
        4,
        5,
        6,
        1,
        1,
        0,
        nodata,
        7,
        1,
        1,
        1,
        1,
        1,
        1,
    ]
    weights = [w for row_w in (0.3, 0.1, 0.2, 0.4, 0.5) for w in [row_w] * 5]

    ds = gdal.GetDriverByName("MEM").Create("", 5, 5)
    ds.SetGeoTransform((-1, 1, 0, 4, 0, -1))
    ds.GetRasterBand(1).SetNoDataValue(nodata)
    ds.WriteRaster(0, 0, 5, 5, bytes(values))

    weights_ds = gdal.GetDriverByName("MEM").Create(
        "", 5, 5, eType=gdal.GDT_Float64
    )
    weights_ds.SetGeoTransform((0, 1, 0, 5, 0, -1))
    weights_ds.WriteRaster(0, 0, 5, 5, struct.pack("@25d", *weights))

    zonal = _get_zonal_alg()
    zonal["input"] = ds
    zonal["weights"] = weights_ds
    zonal["zones"] = _wkt_ds(
        ["POLYGON ((0.5 0.5, 2.5 0.5, 2.5 2.5, 0.5 2.5, 0.5 0.5))"]
    )
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["pixels"] = "fractional"
    zonal["strategy"] = strategy
    zonal["stat"] = list(stat) if isinstance(stat, tuple) else stat
    self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    feat = out_ds.GetLayer(0).GetNextFeature()
    exp_values = [1, 2, 3, 4, 5, 6, 0, 7]
    exp_cov = [0.25, 0.5, 0.25, 0.5, 1.0, 0.5, 0.25, 0.25]
    exp_weights = [0.2, 0.2, 0.2, 0.4, 0.4, 0.4, 0.5, 0.5]

    if stat == "center_x":
      self.assertEqual(
          list(feat["center_x"]), [0.5, 1.5, 2.5, 0.5, 1.5, 2.5, 0.5, 2.5]
      )
    elif stat == "center_y":
      self.assertEqual(
          list(feat["center_y"]), [2.5, 2.5, 2.5, 1.5, 1.5, 1.5, 0.5, 0.5]
      )
    elif stat == "coverage":
      for got, exp in zip(feat["coverage"], exp_cov):
        self.assertAlmostEqual(got, exp, places=5)
    elif stat == "count":
      self.assertEqual(feat["count"], 3.5)
    elif stat == ("frac", "unique"):
      frac_map = dict(zip(feat["unique"], feat["frac"]))
      self.assertEqual(set(frac_map.keys()), {0, 1, 2, 3, 4, 5, 6, 7})
      self.assertAlmostEqual(frac_map[0], 0.25 / 3.5)
      self.assertAlmostEqual(frac_map[1], 0.25 / 3.5)
      self.assertAlmostEqual(frac_map[2], 0.5 / 3.5)
      self.assertAlmostEqual(frac_map[3], 0.25 / 3.5)
      self.assertAlmostEqual(frac_map[4], 0.5 / 3.5)
      self.assertAlmostEqual(frac_map[5], 1.0 / 3.5)
      self.assertAlmostEqual(frac_map[6], 0.5 / 3.5)
      self.assertAlmostEqual(frac_map[7], 0.25 / 3.5)
    elif stat == "max":
      self.assertEqual(feat["max"], 7)
    elif stat == "max_center_x":
      self.assertEqual(feat["max_center_x"], 2.5)
    elif stat == "max_center_y":
      self.assertEqual(feat["max_center_y"], 0.5)
    elif stat == "mean":
      self.assertAlmostEqual(feat["mean"], 13.75 / 3.5)
    elif stat == "min":
      self.assertEqual(feat["min"], 0)
    elif stat == "minority":
      self.assertEqual(feat["minority"], 0)
    elif stat == "min_center_x":
      self.assertEqual(feat["min_center_x"], 0.5)
    elif stat == "min_center_y":
      self.assertEqual(feat["min_center_y"], 0.5)
    elif stat == "mode":
      self.assertEqual(feat["mode"], 5)
    elif stat == "stdev":
      self.assertAlmostEqual(feat["stdev"], 1.9807749, places=5)
    elif stat == "sum":
      self.assertEqual(feat["sum"], 13.75)
    elif stat == "variety":
      self.assertEqual(feat["variety"], 8)
    elif stat == "values":
      self.assertEqual(list(feat["values"]), exp_values)
    elif stat == "variance":
      self.assertAlmostEqual(feat["variance"], 3.923469, places=5)
    elif stat == "weighted_mean":
      w_tot = sum(w * c for w, c in zip(exp_weights, exp_cov))
      w_sum = sum(
          v * w * c for v, w, c in zip(exp_values, exp_weights, exp_cov)
      )
      self.assertAlmostEqual(feat["weighted_mean"], w_sum / w_tot)
    elif stat == "weighted_stdev":
      self.assertAlmostEqual(feat["weighted_stdev"], 2.032634, places=5)
    elif stat == "weighted_variance":
      self.assertAlmostEqual(feat["weighted_variance"], 4.1316, places=4)
    elif stat == "weighted_sum":
      w_sum = sum(
          v * w * c for v, w, c in zip(exp_values, exp_weights, exp_cov)
      )
      self.assertAlmostEqual(feat["weighted_sum"], w_sum)
    elif stat == "weights":
      for got, exp in zip(feat["weights"], exp_weights):
        self.assertAlmostEqual(got, exp)

  def test_weighted_stats_nodata(self) -> None:
    nx, ny = 3, 4
    vals = bytes(range(nx * ny))
    values_ds = gdal.GetDriverByName("MEM").Create("", nx, ny)
    values_ds.SetGeoTransform((0, 1, 0, ny, 0, -1))
    values_ds.WriteRaster(0, 0, nx, ny, vals)

    weights_ds = gdal.GetDriverByName("MEM").Create("", nx, ny)
    weights_ds.SetGeoTransform((0, 1, 0, ny, 0, -1))
    weights_ds.GetRasterBand(1).SetNoDataValue(6)
    weights_ds.WriteRaster(0, 0, nx, ny, vals)

    zonal = _get_zonal_alg()
    zonal["input"] = values_ds
    zonal["weights"] = weights_ds
    zonal["zones"] = _wkt_ds(
        [f"POLYGON ((0 0, {nx} 0, {nx} {ny}, 0 {ny}, 0 0))"]
    )
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = [
        "weighted_mean",
        "weighted_sum",
        "weighted_stdev",
        "weighted_variance",
        "weights",
    ]
    self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertTrue(math.isnan(feat["weighted_mean"]))
    self.assertTrue(math.isnan(feat["weighted_sum"]))
    self.assertTrue(math.isnan(feat["weighted_stdev"]))
    self.assertTrue(math.isnan(feat["weighted_variance"]))
    got_weights = feat["weights"]
    self.assertLen(got_weights, nx * ny)
    for idx, val in enumerate(got_weights):
      if idx == 6:
        self.assertTrue(math.isnan(val))
      else:
        self.assertEqual(val, float(idx))

  @parameterized.parameters(
      "weights",
      "weighted_mean",
      "weighted_sum",
      "weighted_stdev",
      "weighted_variance",
  )
  def test_missing_weights(self, stat: str) -> None:
    zonal = _get_zonal_alg()
    zonal["input"] = _make_polyrast()
    zonal["zones"] = self.poly_shp
    zonal["stat"] = stat
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    with self.assertRaisesRegex(RuntimeError, "requires weights"):
      zonal.Run()

  @parameterized.parameters("raster", "feature")
  def test_non_polygon_geometry(self, strategy: str) -> None:
    self._skip_for_strategy_or_pixels(strategy)
    zonal = _get_zonal_alg()
    zonal["input"] = self.byte_tif
    zonal["zones"] = _wkt_ds(["LINESTRING (3 3, 8 8)"])
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["strategy"] = strategy
    zonal["stat"] = "sum"
    with self.assertRaisesRegex(RuntimeError, "Non-polygonal geometry"):
      zonal.Run()

  def test_output_format_detection(self) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    with utilities_util.vsimem_dir("zonal_stats_csv") as tmp:
      zonal = _get_zonal_alg()
      zonal["input"] = _make_polyrast()
      zonal["output"] = f"{tmp}/out.csv"
      zonal["stat"] = "mean"
      zonal["zones"] = self.poly_shp
      self.assertTrue(zonal.Run())
      out_ds = zonal.Output()
      self.assertEqual(out_ds.GetDriver().GetName(), "CSV")

  @parameterized.parameters("raster", "feature")
  def test_polygon_zones_include_fields(self, strategy: str) -> None:
    self._skip_for_strategy_or_pixels(strategy)
    polyrast = _make_polyrast()

    zonal = _get_zonal_alg()
    zonal["input"] = polyrast
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["strategy"] = strategy
    zonal["stat"] = "sum"
    zonal["include-field"] = "does_not_exist"
    zonal["zones"] = self.poly_shp
    with self.assertRaisesRegex(RuntimeError, "Field .* not found"):
      zonal.Run()

    zonal["include-field"] = ["PRFEDEA", "EAS_ID"]
    self.assertTrue(zonal.Run())
    out_ds = zonal.Output()
    feat = out_ds.GetLayer(0).GetNextFeature()
    self.assertEqual(_field_names(feat), ["PRFEDEA", "EAS_ID", "sum"])
    self.assertEqual(feat["PRFEDEA"], "35043411")
    self.assertEqual(feat["EAS_ID"], 168)
    self.assertEqual(feat["sum"], 369.0)

  def test_invalid_inputs_and_chunk_size(self) -> None:
    zonal = _get_zonal_alg()
    zonal["input"] = self.byte_tif
    zonal["zones"] = self.byte_tif
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = "sum"
    zonal["include-field"] = "id"
    with self.assertRaisesRegex(RuntimeError, "Cannot include fields"):
      zonal.Run()

    zonal = _get_zonal_alg()
    zonal["input"] = self.byte_tif
    zonal["zones"] = self.byte_tif
    zonal["zones-band"] = 2
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = "sum"
    with self.assertRaisesRegex(RuntimeError, "Invalid zones band: 2"):
      zonal.Run()

    polyrast = _make_polyrast()
    zonal = _get_zonal_alg()
    zonal["input"] = polyrast
    zonal["zones"] = self.poly_shp
    zonal["stat"] = "sum"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["band"] = 2
    with self.assertRaisesRegex(RuntimeError, "Value of 'band' should be"):
      zonal.Run()

    zonal = _get_zonal_alg()
    zonal["input"] = polyrast
    zonal["zones"] = self.poly_shp
    zonal["zones-layer"] = "does_not_exist"
    zonal["stat"] = "sum"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError, "Specified zones layer .* not found"
    ):
      zonal.Run()

    zonal = _get_zonal_alg()
    with self.assertRaisesRegex(RuntimeError, "Failed to parse memory size"):
      zonal["chunk-size"] = "2 gigs"
    with self.assertRaisesRegex(RuntimeError, "too large"):
      zonal["chunk-size"] = "1e100 mb"
    with self.assertRaisesRegex(RuntimeError, "must have a unit"):
      zonal["chunk-size"] = "512"

  def test_large_chunk_size(self) -> None:
    if sys.maxsize <= 1 << 32:
      self.skipTest("only works on 64 bit")
    zonal = _get_zonal_alg()
    zonal["input"] = self.byte_tif
    zonal["zones"] = self.byte_tif
    zonal["stat"] = "sum"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["chunk-size"] = "10000GB"
    self.assertTrue(zonal.Run())

  @parameterized.parameters(
      gdal.GDT_Int64,
      gdal.GDT_UInt64,
      gdal.GDT_CFloat16,
      gdal.GDT_CFloat32,
      gdal.GDT_CFloat64,
  )
  def test_unsupported_type(self, dtype: int) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 3, 3, eType=dtype)
    src_ds.SetGeoTransform((0, 1, 0, 3, 0, -1))
    src_ds.GetRasterBand(1).Fill(1)

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["zones"] = _wkt_ds("POLYGON ((0 0, 3 0, 3 3, 0 0))")
    zonal["stat"] = "sum"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    with self.assertRaisesRegex(RuntimeError, "Source data type"):
      zonal.Run()

  def test_invalid_weight_band(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create(
        "", 3, 3, eType=gdal.GDT_Float32
    )
    src_ds.SetGeoTransform((0, 1, 0, 3, 0, -1))
    src_ds.GetRasterBand(1).Fill(1)

    weight_ds = gdal.GetDriverByName("MEM").Create(
        "", 3, 3, eType=gdal.GDT_Float32
    )
    weight_ds.SetGeoTransform((0, 1, 0, 3, 0, -1))
    weight_ds.GetRasterBand(1).Fill(1)

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["weights"] = weight_ds
    zonal["weights-band"] = 2
    zonal["zones"] = _wkt_ds("POLYGON ((0 0, 3 0, 3 3, 0 0))")
    zonal["stat"] = "weighted_mean"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    with self.assertRaisesRegex(RuntimeError, "invalid weights band"):
      zonal.Run()

  @parameterized.parameters(
      gdal.GDT_Int64,
      gdal.GDT_UInt64,
      gdal.GDT_CFloat16,
      gdal.GDT_CFloat32,
      gdal.GDT_CFloat64,
  )
  def test_unsupported_weight_type(self, dtype: int) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create(
        "", 3, 3, eType=gdal.GDT_Float32
    )
    src_ds.SetGeoTransform((0, 1, 0, 3, 0, -1))
    src_ds.GetRasterBand(1).Fill(1)

    weight_ds = gdal.GetDriverByName("MEM").Create("", 3, 3, eType=dtype)
    weight_ds.SetGeoTransform((0, 1, 0, 3, 0, -1))
    weight_ds.GetRasterBand(1).Fill(1)

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["weights"] = weight_ds
    zonal["zones"] = _wkt_ds("POLYGON ((0 0, 3 0, 3 3, 0 0))")
    zonal["stat"] = "weighted_mean"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    with self.assertRaisesRegex(RuntimeError, "Weights data type"):
      zonal.Run()

  @parameterized.product(
      srs_case=(
          (4326, None, None, False),
          (4326, None, 4326, False),
          (None, None, 4326, False),
          (None, None, None, False),
          (4326, None, 4269, True),
          (4326, 4269, None, True),
          (4326, 4269, 4326, True),
      ),
      zones_typ=("raster", "vector"),
  )
  def test_srs_mismatch(
      self,
      srs_case: tuple[int | None, int | None, int | None, bool],
      zones_typ: str,
  ) -> None:
    raster_srs, weights_srs, zones_srs, warn = srs_case

    src_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    src_ds.SetGeoTransform((0, 1, 0, 10, 0, -1))
    if raster_srs:
      src_ds.SetSpatialRef(osr.SpatialReference(epsg=raster_srs))

    weights_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    weights_ds.SetGeoTransform((0, 1, 0, 10, 0, -1))
    if weights_srs:
      weights_ds.SetSpatialRef(osr.SpatialReference(epsg=weights_srs))

    if zones_typ == "vector":
      zones_ds = _wkt_ds(["POLYGON ((2 2, 8 2, 8 8, 2 2))"], epsg=zones_srs)
    else:
      zones_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
      zones_ds.SetGeoTransform((0, 1, 0, 10, 0, -1))
      if zones_srs:
        zones_ds.SetSpatialRef(osr.SpatialReference(epsg=zones_srs))

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["weights"] = weights_ds
    zonal["zones"] = zones_ds
    zonal["stat"] = "weighted_mean"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"

    err_type = gdal.CE_Warning if warn else gdal.CE_None
    with self._error_raised(err_type, match="do not have the same SRS"):
      self.assertTrue(zonal.Run())

  @parameterized.parameters("raster", "feature")
  def test_polygon_zone_outside_raster(self, strategy: str) -> None:
    self._skip_for_strategy_or_pixels(strategy)
    zonal = _get_zonal_alg()
    zonal["input"] = self.byte_tif
    zonal["zones"] = _wkt_ds(
        ["POLYGON ((2 2, 8 2, 8 8, 2 2))", "POLYGON EMPTY"]
    )
    zonal["strategy"] = strategy
    zonal["stat"] = ["sum", "mode"]
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    results = _all_features(out_ds.GetLayer(0))
    self.assertLen(results, 2)
    self.assertEqual(results[0]["sum"], 0)
    self.assertIsNone(results[0]["mode"])
    self.assertEqual(results[1]["sum"], 0)
    self.assertIsNone(results[1]["mode"])

  @parameterized.parameters(None, 99)
  def test_raster_values_partially_outside(
      self, src_nodata: int | None
  ) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    src_ds.SetGeoTransform((0, 1, 0, 10, 0, -1))
    if src_nodata is not None:
      src_ds.GetRasterBand(1).SetNoDataValue(src_nodata)
    src_ds.WriteRaster(0, 0, 10, 10, bytes([1] * 100))

    zones = [1 + (i // 20) for i in range(100)]
    zones_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    zones_ds.SetGeoTransform((2, 1, 0, 12, 0, -1))
    zones_ds.WriteRaster(0, 0, 10, 10, bytes(zones))

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["zones"] = zones_ds
    zonal["stat"] = ["count", "sum"]
    zonal["output"] = ""
    zonal["output-format"] = "MEM"

    with self._error_raised(
        gdal.CE_Warning, match="Source raster does not fully cover zones raster"
    ):
      self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    results = _all_features(out_ds.GetLayer(0))
    self.assertLen(results, 5)
    for i in range(1, 6):
      self.assertEqual(results[i - 1]["value"], i)
      if src_nodata is not None:
        self.assertEqual(results[i - 1]["count"], 16 if i > 1 else 0)
      else:
        self.assertEqual(results[i - 1]["count"], 20)
        self.assertEqual(results[i - 1]["sum"], 16 if i > 1 else 0)

  @parameterized.parameters(None, 99)
  def test_raster_zones_entirely_outside(self, src_nodata: int | None) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    src_ds.SetGeoTransform((0, 1, 0, 10, 0, -1))
    src_ds.GetRasterBand(1).Fill(1)
    if src_nodata is not None:
      src_ds.GetRasterBand(1).SetNoDataValue(src_nodata)

    zones = [1 + (i // 20) for i in range(100)]
    zones_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    zones_ds.SetGeoTransform((1000, 1, 0, 1000, 0, -1))
    zones_ds.WriteRaster(0, 0, 10, 10, bytes(zones))

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["zones"] = zones_ds
    zonal["stat"] = ["sum", "mode"]
    zonal["output"] = ""
    zonal["output-format"] = "MEM"

    with self._error_raised(
        gdal.CE_Warning, match="Source raster does not intersect zones raster"
    ):
      self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    results = _all_features(out_ds.GetLayer(0))
    self.assertLen(results, 5)
    for feat in results:
      self.assertEqual(feat["sum"], 0)
      if src_nodata is not None:
        self.assertIsNone(feat["mode"])
      else:
        self.assertEqual(feat["mode"], 0)

  def test_raster_weights_partially_outside(self) -> None:
    src_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    src_ds.SetGeoTransform((0, 1, 0, 10, 0, -1))
    src_ds.WriteRaster(0, 0, 10, 10, bytes([1] * 100))

    zones = [1 + (i // 20) for i in range(100)]
    zones_ds = gdal.GetDriverByName("MEM").Create("", 10, 10)
    zones_ds.SetGeoTransform((0, 1, 0, 10, 0, -1))
    zones_ds.WriteRaster(0, 0, 10, 10, bytes(zones))

    weights = [i / 100.0 for i in range(100)]
    weights_ds = gdal.GetDriverByName("MEM").Create(
        "", 10, 10, eType=gdal.GDT_Float32
    )
    weights_ds.SetGeoTransform((2, 1, 0, 12, 0, -1))
    weights_ds.WriteRaster(0, 0, 10, 10, struct.pack("@100f", *weights))
    raw_w = weights_ds.ReadRaster(0, 0, 10, 10)
    assert raw_w is not None
    w_f32 = struct.unpack("@100f", raw_w)
    weights_adj = [0.0] * 100
    for row in range(8):
      for col in range(2, 10):
        weights_adj[row * 10 + col] = w_f32[(row + 2) * 10 + (col - 2)]

    zonal = _get_zonal_alg()
    zonal["input"] = src_ds
    zonal["zones"] = zones_ds
    zonal["weights"] = weights_ds
    zonal["stat"] = "weighted_sum"
    zonal["output"] = ""
    zonal["output-format"] = "MEM"

    with self._error_raised(
        gdal.CE_Warning,
        match="Weighting raster does not fully cover zones raster",
    ):
      self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    results = _all_features(out_ds.GetLayer(0))
    self.assertLen(results, 5)
    for feat in results:
      zone = feat["value"]
      expected = sum(
          weights_adj[idx] for idx, zv in enumerate(zones) if zv == zone
      )
      self.assertAlmostEqual(feat["weighted_sum"], expected, places=5)

  @parameterized.parameters("raster", "feature")
  def test_vector_zones_weights_resampled(self, strategy: str) -> None:
    self._skip_for_strategy_or_pixels(strategy)

    values = [1, 2, 3, 4]
    values_ds = gdal.GetDriverByName("MEM").Create("", 2, 2)
    values_ds.SetGeoTransform((0, 1, 0, 2, 0, -1))
    values_ds.WriteRaster(0, 0, 2, 2, bytes(values))

    weights = [float(i) for i in range(1, 17)]
    weights_ds = gdal.GetDriverByName("MEM").Create(
        "", 4, 4, eType=gdal.GDT_Float32
    )
    weights_ds.SetGeoTransform((0, 0.5, 0, 2, 0, -0.5))
    weights_ds.WriteRaster(0, 0, 4, 4, struct.pack("@16f", *weights))

    zonal = _get_zonal_alg()
    zonal["zones"] = _wkt_ds(["POLYGON ((0 0, 2 0, 2 2, 0 2, 0 0))"])
    zonal["input"] = values_ds
    zonal["weights"] = weights_ds
    zonal["strategy"] = strategy
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    zonal["stat"] = ["weighted_mean", "weights"]

    with self._error_raised(gdal.CE_Warning, match="Resampled weight"):
      self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    feat = out_ds.GetLayer(0).GetNextFeature()
    weights_agg = [3.5, 5.5, 11.5, 13.5]
    expected_wmean = sum(v * w for v, w in zip(values, weights_agg)) / sum(
        weights_agg
    )
    self.assertAlmostEqual(feat["weighted_mean"], expected_wmean, places=5)

  def test_pipeline_usage(self) -> None:
    if not utilities_util.has_driver("CSV"):
      self.skipTest("CSV driver not available")
    pipeline = gdal.Algorithm("pipeline")
    with utilities_util.vsimem_dir("zonal_stats_pipe") as tmp:
      src_fname = f"{tmp}/polyrast.tif"
      out_fname = f"{tmp}/out.csv"
      gdal.GetDriverByName("GTiff").CreateCopy(src_fname, _make_polyrast())
      self.assertTrue(
          pipeline.ParseRunAndFinalize([
              "read",
              src_fname,
              "!",
              "zonal-stats",
              "--zones",
              self.poly_shp,
              "--stat",
              "sum",
              "!",
              "write",
              out_fname,
          ])
      )
      with gdal.OpenEx(out_fname) as dst:
        self.assertEqual(dst.GetLayer(0).GetFeatureCount(), 10)

  @parameterized.parameters("raster", "feature")
  def test_null_geometry(self, strategy: str) -> None:
    self._skip_for_strategy_or_pixels(strategy)
    zones = gdal.GetDriverByName("MEM").CreateVector("")
    zones_lyr = zones.CreateLayer("zones")
    zones_lyr.CreateField(ogr.FieldDefn("zone_id", ogr.OFTInteger))
    zones_lyr.CreateFeature(ogr.Feature(zones_lyr.GetLayerDefn()))

    zonal = _get_zonal_alg()
    zonal["input"] = self.byte_tif
    zonal["zones"] = zones
    zonal["strategy"] = strategy
    zonal["stat"] = ["sum", "mode"]
    zonal["output"] = ""
    zonal["output-format"] = "MEM"
    self.assertTrue(zonal.Run())

    out_ds = zonal.Output()
    results = _all_features(out_ds.GetLayer(0))
    self.assertLen(results, 1)
    self.assertEqual(results[0]["sum"], 0)
    self.assertIsNone(results[0]["mode"])


if __name__ == "__main__":
  absltest.main()
