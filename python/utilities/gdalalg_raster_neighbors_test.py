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

"""Tests for `gdal raster neighbors`.

Ported from upstream autotest/utilities/test_gdalalg_raster_neighbors.py.

Upstream copyright:
  Copyright (c) 2025, Even Rouault <even dot rouault at spatialys.com>
  SPDX-License-Identifier: MIT
"""

import array

from absl.testing import absltest
from absl.testing import parameterized
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_neighbors_alg() -> gdal.Algorithm:
  return gdal.GetGlobalAlgorithmRegistry()["raster"]["neighbors"]


def _make_3x3_ds(
    values: list[int], nodata: float | None = None
) -> gdal.Dataset:
  ds = gdal.GetDriverByName("MEM").Create("", 3, 3)
  ds.WriteRaster(0, 0, 3, 3, array.array("B", values))
  if nodata is not None:
    ds.GetRasterBand(1).SetNoDataValue(nodata)
  return ds


class GdalalgRasterNeighborsTest(gdaltest.TestCase, parameterized.TestCase):

  @parameterized.parameters(
      ("sharpen", 4252),
      ("edge1", 2278),
      ("edge2", 2311),
  )
  def test_named_kernels(self, kernel: str, checksum: int) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_neighbors_alg()
    alg["input"] = byte_tif
    alg["kernel"] = kernel
    alg["output-format"] = "MEM"
    alg["datatype"] = "Byte"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).DataType, gdal.GDT_Byte)
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), checksum)

  def test_manual_kernels(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg = _get_neighbors_alg()
    alg["input"] = byte_tif
    alg["kernel"] = [[0, -1, 0], [-1, 5, -1], [0, -1, 0]]
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.GetRasterBand(1).DataType, gdal.GDT_Float64)
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 4013)

    alg2 = _get_neighbors_alg()
    alg2["input"] = byte_tif
    alg2["kernel"] = [
        [[0, -1, 0], [-1, 5, -1], [0, -1, 0]],
        [[0, 0, 0], [0, 1, 0], [0, 0, 0]],
    ]
    alg2["output-format"] = "MEM"
    self.assertTrue(alg2.Run())

    out_ds2 = utilities_util.get_arg_dataset(alg2, "output")
    self.assertEqual(out_ds2.RasterCount, 2)
    self.assertEqual(out_ds2.GetRasterBand(1).DataType, gdal.GDT_Float64)
    self.assertEqual(out_ds2.GetRasterBand(1).Checksum(), 4013)
    self.assertEqual(out_ds2.GetRasterBand(2).Checksum(), 4672)

  def test_multiband_and_several_kernels(self) -> None:
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )
    alg = _get_neighbors_alg()
    alg["input"] = small_world
    alg["kernel"] = "sharpen"
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())

    out_ds = utilities_util.get_arg_dataset(alg, "output")
    self.assertEqual(out_ds.RasterCount, 3)
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 31543)
    self.assertEqual(out_ds.GetRasterBand(2).Checksum(), 35728)
    self.assertEqual(out_ds.GetRasterBand(3).Checksum(), 32040)

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    alg2 = _get_neighbors_alg()
    alg2["input"] = byte_tif
    alg2["kernel"] = ["u", "v"]
    alg2["output-format"] = "MEM"
    self.assertTrue(alg2.Run())
    out_ds2 = alg2.Output()
    self.assertEqual(out_ds2.RasterCount, 2)

  @parameterized.parameters(
      ("equal", "mean", None, 5.0),
      ("equal", "sum", None, 45.0),
      ("equal", "min", "Byte", 1),
      ("equal", "max", "Byte", 9),
      ("u", None, None, 1.0),
      ("v", None, None, 3.0),
  )
  def test_methods_and_directional_kernels(
      self,
      kernel: str,
      method: str | None,
      datatype: str | None,
      expected: float,
  ) -> None:
    alg = _get_neighbors_alg()
    alg["input"] = _make_3x3_ds([1, 2, 3, 4, 5, 6, 7, 8, 9])
    alg["kernel"] = kernel
    if method is not None:
      alg["method"] = method
    if datatype is not None:
      alg["datatype"] = datatype
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())

    out_ds = alg.Output()
    typecode = "B" if datatype == "Byte" else "d"
    ar = array.array(typecode)
    ar.frombytes(out_ds.ReadRaster(1, 1, 1, 1))
    self.assertEqual(ar[0], expected)

  def test_median_mode_and_stddev(self) -> None:
    alg_odd = _get_neighbors_alg()
    alg_odd["input"] = _make_3x3_ds([1, 2, 3, 4, 5, 60, 70, 80, 90])
    alg_odd["kernel"] = "equal"
    alg_odd["method"] = "median"
    alg_odd["output-format"] = "MEM"
    alg_odd["datatype"] = "Byte"
    self.assertTrue(alg_odd.Run())
    out_ds_odd = alg_odd.Output()
    ar_b = array.array("B")
    ar_b.frombytes(out_ds_odd.ReadRaster(1, 1, 1, 1))
    self.assertEqual(ar_b[0], 5)

    alg_even = _get_neighbors_alg()
    alg_even["input"] = _make_3x3_ds([1, 2, 3, 4, 5, 60, 70, 80, 90], nodata=90)
    alg_even["kernel"] = "equal"
    alg_even["method"] = "median"
    alg_even["output-format"] = "MEM"
    self.assertTrue(alg_even.Run())
    out_ds_even = alg_even.Output()
    ar_d = array.array("d")
    ar_d.frombytes(out_ds_even.ReadRaster(1, 1, 1, 1))
    self.assertEqual(ar_d[0], 4.5)

    alg_mode = _get_neighbors_alg()
    alg_mode["input"] = _make_3x3_ds([1, 2, 3, 4, 5, 6, 3, 8, 9])
    alg_mode["kernel"] = "equal"
    alg_mode["method"] = "mode"
    alg_mode["output-format"] = "MEM"
    alg_mode["datatype"] = "Byte"
    self.assertTrue(alg_mode.Run())
    out_ds_mode = alg_mode.Output()
    ar_mode = array.array("b")
    ar_mode.frombytes(out_ds_mode.ReadRaster(1, 1, 1, 1))
    self.assertEqual(ar_mode[0], 3)

    alg_std = _get_neighbors_alg()
    alg_std["input"] = _make_3x3_ds([1, 2, 3, 4, 5, 6, 7, 8, 9])
    alg_std["kernel"] = "equal"
    alg_std["method"] = "stddev"
    alg_std["output-format"] = "MEM"
    self.assertTrue(alg_std.Run())
    out_ds_std = alg_std.Output()
    ar_std = array.array("d")
    ar_std.frombytes(out_ds_std.ReadRaster(1, 1, 1, 1))
    self.assertAlmostEqual(ar_std[0], 2.58198881149292, places=6)

  def test_gaussian_and_unsharp_masking(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")

    alg_g3 = _get_neighbors_alg()
    alg_g3["input"] = byte_tif
    alg_g3["kernel"] = "gaussian"
    alg_g3["output-format"] = "MEM"
    self.assertTrue(alg_g3.Run())
    out_ds_g3 = alg_g3.Output()
    ar = array.array("d")
    ar.frombytes(out_ds_g3.ReadRaster(10, 10, 1, 1))
    self.assertEqual(ar[0], 114.0625)

    alg_g5 = _get_neighbors_alg()
    alg_g5["input"] = byte_tif
    alg_g5["kernel"] = "gaussian"
    alg_g5["size"] = 5
    alg_g5["output-format"] = "MEM"
    self.assertTrue(alg_g5.Run())
    out_ds_g5 = alg_g5.Output()
    ar5 = array.array("d")
    ar5.frombytes(out_ds_g5.ReadRaster(10, 10, 1, 1))
    self.assertAlmostEqual(ar5[0], 119.00390625, places=6)

    alg_usm = _get_neighbors_alg()
    alg_usm["input"] = byte_tif
    alg_usm["kernel"] = "unsharp-masking"
    alg_usm["output-format"] = "MEM"
    self.assertTrue(alg_usm.Run())
    out_ds_usm = alg_usm.Output()
    ar_usm = array.array("d")
    ar_usm.frombytes(out_ds_usm.ReadRaster(10, 10, 1, 1))
    self.assertAlmostEqual(ar_usm[0], 110.99609375, places=6)

  def test_nodata_handling(self) -> None:
    for dst_nodata, expected_nodata in (
        (None, 9.0),
        (-1, -1.0),
        ("none", None),
    ):
      alg = _get_neighbors_alg()
      alg["input"] = _make_3x3_ds([1, 2, 3, 4, 5, 6, 7, 8, 9], nodata=9)
      alg["kernel"] = "equal"
      alg["method"] = "max"
      alg["output-format"] = "MEM"
      if dst_nodata is not None:
        alg["nodata"] = dst_nodata
      self.assertTrue(alg.Run())

      out_ds = alg.Output()
      self.assertEqual(
          out_ds.GetRasterBand(1).GetNoDataValue(), expected_nodata
      )
      ar = array.array("d")
      ar.frombytes(out_ds.ReadRaster(1, 1, 1, 1))
      self.assertEqual(ar[0], 8.0)

  def test_errors(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    small_world = utilities_util.get_testdata_path(
        "../gdrivers/data/small_world.tif"
    )

    alg = _get_neighbors_alg()
    alg["input"] = byte_tif
    alg["kernel"] = "equal"
    alg["method"] = "max"
    alg["output-format"] = "MEM"
    alg["nodata"] = "-1"
    alg["datatype"] = "Byte"
    with self.assertRaisesRegex(
        RuntimeError, "Band output type Byte cannot represent NoData value -1"
    ):
      alg.Run()

    alg = _get_neighbors_alg()
    alg["input"] = small_world
    alg["kernel"] = "sharpen"
    alg["method"] = ["sum", "sum"]
    alg["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError,
        "The number of values for the 'method' argument should be one or"
        " exactly the number of values of 'kernel'",
    ):
      alg.Run()

    for arg, val, err_regex in (
        (
            "kernel",
            "[[0,0],[0,0]]",
            (
                "The number of values in the 'kernel' argument must be an odd"
                " square number"
            ),
        ),
        (
            "kernel",
            "[[0,0,0],[0,0,0]]",
            (
                "The number of values in the 'kernel' argument must be an odd"
                " square number"
            ),
        ),
        (
            "kernel",
            "[[1,1,1],[1,foo,1],[1,1,1]]",
            "Non-numeric value found in the 'kernel' argument",
        ),
        ("size", 4, "The value of 'size' must be an odd number"),
        ("method", "invalid", "Invalid value 'invalid' for string argument"),
        ("kernel", "invalid", "Valid values for 'kernel' argument are:"),
    ):
      alg = _get_neighbors_alg()
      with self.assertRaisesRegex(RuntimeError, err_regex):
        alg[arg] = val

    alg = _get_neighbors_alg()
    alg["kernel"] = "[[1,1,1],[1,1,1],[1,1,1]]"
    alg["size"] = 5
    with self.assertRaisesRegex(
        RuntimeError,
        r"Value of 'size' argument \(5\) inconsistent with the one deduced"
        r" from the kernel matrix \(3\)",
    ):
      alg.Run()

    for kernel_name, size_val, err_regex in (
        ("u", 7, "Currently only size = 3 is supported for kernel 'u'"),
        (
            "gaussian",
            7,
            "Currently only size = 3 or 5 is supported for kernel 'gaussian'",
        ),
        (
            "unsharp-masking",
            7,
            "Currently only size = 5 is supported for kernel 'unsharp-masking'",
        ),
    ):
      alg = _get_neighbors_alg()
      alg["kernel"] = kernel_name
      alg["size"] = size_val
      with self.assertRaisesRegex(RuntimeError, err_regex):
        alg.Run()

  def test_completion(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "neighbors", "--kernel"]
    )
    self.assertEqual(code, 0)
    self.assertIn("sharpen", out.split(" "))

    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "neighbors", "--kernel", "["]
    )
    self.assertEqual(code, 0)
    self.assertNotIn("sharpen", out.split(" "))

  def test_custom_kernel_zero_sum(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    kernel = [[0, -0.04, 0], [-0.04, 0.16, -0.04], [0, -0.04, 0]]

    alg = _get_neighbors_alg()
    alg["input"] = byte_tif
    alg["kernel"] = kernel
    alg["output-format"] = "MEM"
    self.assertTrue(alg.Run())

    out_ds = alg.Output()
    min_val, max_val = out_ds.GetRasterBand(1).ComputeRasterMinMax()
    self.assertAlmostEqual(min_val, -7.56, places=5)
    self.assertAlmostEqual(max_val, 12.24, places=5)

    alg_err = _get_neighbors_alg()
    alg_err["input"] = byte_tif
    alg_err["kernel"] = kernel
    alg_err["method"] = "mean"
    alg_err["output-format"] = "MEM"
    with self.assertRaisesRegex(
        RuntimeError,
        "Specifying method = 'mean' for a kernel whose sum of coefficients is"
        " zero is not allowed. Use 'sum' instead",
    ):
      alg_err.Run()


if __name__ == "__main__":
  absltest.main()
