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

"""Tests for `gdal raster calc`.

Ported from upstream autotest/utilities/test_gdalalg_raster_calc.py.

Upstream copyright:
  Copyright (c) 2025, ISciences LLC
  SPDX-License-Identifier: MIT
"""

from collections.abc import Callable
import functools
import math
import os
import xml.etree.ElementTree as ET

from absl.testing import absltest
from absl.testing import parameterized
import numpy as np
from osgeo import gdal

from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.utilities import utilities_util


def setUpModule() -> None:
  utilities_util.setup_test_env()


def _get_calc_alg() -> gdal.Algorithm:
  reg = gdal.GetGlobalAlgorithmRegistry()
  raster = reg.InstantiateAlg("raster")
  return raster.InstantiateSubAlgorithm("calc")


def _has_vrt_expression_dialect(dialect: str) -> bool:
  vrt = gdal.GetDriverByName("VRT")
  if vrt is None:
    return False
  dialects = vrt.GetMetadataItem("ExpressionDialects") or ""
  return dialect in dialects


def _has_muparser_user_data() -> bool:
  vrt = gdal.GetDriverByName("VRT")
  if vrt is None:
    return False
  return vrt.GetMetadataItem("MUPARSER_HAS_DEFINE_FUN_USER_DATA") is not None


class GdalalgRasterCalcTest(gdaltest.TestCase, parameterized.TestCase):

  def _require_muparser(self) -> None:
    if not _has_vrt_expression_dialect("muparser"):
      self.skipTest("muparser not available")

  @parameterized.parameters("tif", "vrt")
  def test_basic_1(self, output_format: str) -> None:
    self._require_muparser()
    infile = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    with utilities_util.vsimem_dir("raster_calc_basic_1") as vsimem:
      outfile = f"{vsimem}/out.{output_format}"
      calc = _get_calc_alg()
      calc["input"] = [infile]
      calc["output"] = outfile
      calc["calc"] = ["2 + X / (1 + sum(X[1], X[2], X[3]))"]
      self.assertTrue(calc.Run())

      with gdal.Open(infile) as src, gdal.Open(outfile) as dst:
        srcval = src.ReadAsMaskedArray().astype("float64")
        expected = np.apply_along_axis(
            lambda x: 2 + x / (1 + x.sum()), 0, srcval
        )
        np.testing.assert_array_equal(expected, dst.ReadAsArray())
        self.assertEqual(src.GetGeoTransform(), dst.GetGeoTransform())
        self.assertIsSame(src.GetSpatialRef(), dst.GetSpatialRef())

  @parameterized.parameters("tif", "vrt")
  def test_basic_2(self, output_format: str) -> None:
    self._require_muparser()
    infile = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_calc_basic_2") as vsimem:
      outfile = f"{vsimem}/out.{output_format}"
      calc = _get_calc_alg()
      calc["input"] = [infile]
      calc["output"] = outfile
      calc["calc"] = ["X > 128 ? X + 3 : nan"]
      self.assertTrue(calc.Run())

      with gdal.Open(infile) as src, gdal.Open(outfile) as dst:
        srcval = src.ReadAsArray().astype("float64")
        expected = np.where(srcval > 128, srcval + 3, float("nan"))
        np.testing.assert_array_equal(expected, dst.ReadAsArray())
        self.assertEqual(src.GetGeoTransform(), dst.GetGeoTransform())
        self.assertIsSame(src.GetSpatialRef(), dst.GetSpatialRef())

  @parameterized.parameters(
      ("muparser", True),
      ("muparser", False),
      ("builtin", True),
      ("builtin", False),
  )
  def test_nodata(self, dialect: str, propagate_nodata: bool) -> None:
    if dialect == "muparser":
      self._require_muparser()
      if not _has_muparser_user_data():
        self.skipTest("muparser does not support isnodata function")

    with utilities_util.vsimem_dir("raster_calc_nodata") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"

      with gdal.GetDriverByName("GTiff").Create(
          input_1, 2, 2, eType=gdal.GDT_Int16
      ) as ds:
        ds.GetRasterBand(1).SetNoDataValue(-9)
        ds.WriteArray(np.array([[1, 2], [-9, 4]]))

      with gdal.GetDriverByName("GTiff").Create(
          input_2, 2, 2, eType=gdal.GDT_Int16
      ) as ds:
        ds.GetRasterBand(1).SetNoDataValue(-999)
        ds.WriteArray(np.array([[1, -999], [3, 4]]))

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["calc"] = (
          "(isnodata(A) ? 0 : A) + (isnodata(B) ? 0 : B)"
          if dialect == "muparser"
          else "sum"
      )
      calc["dialect"] = dialect
      calc["nodata"] = 255
      calc["output-format"] = "stream"
      if propagate_nodata:
        calc["propagate-nodata"] = True

      self.assertTrue(calc.Run())
      out_ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(out_ds.RasterCount, 1)

      result = out_ds.ReadAsArray()
      if propagate_nodata:
        np.testing.assert_array_equal(result, [[2, 255], [255, 8]])
      elif dialect == "builtin":
        np.testing.assert_array_equal(result, [[2, 2], [3, 8]])

  @parameterized.parameters(gdal.GDT_Int16, gdal.GDT_Float32)
  def test_nan_result(self, output_type: int) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_nan") as vsimem:
      src_tif = f"{vsimem}/src.tif"
      with gdal.GetDriverByName("GTiff").Create(
          src_tif, 1, 1, eType=gdal.GDT_Float32
      ) as ds:
        ds.GetRasterBand(1).SetNoDataValue(-999)
        ds.GetRasterBand(1).Fill(-999)

      calc = _get_calc_alg()
      calc["input"] = [src_tif]
      calc["calc"] = "X + nan"
      calc["output-format"] = "stream"
      calc["output-data-type"] = output_type
      self.assertTrue(calc.Run())

      result = utilities_util.get_arg_dataset(
          calc, "output"
      ).ReadAsMaskedArray()
      if output_type == gdal.GDT_Int16:
        np.testing.assert_array_equal(result, np.ma.masked_array([[0]], False))
      else:
        self.assertTrue(np.isnan(result))

  def test_nodata_variable(self) -> None:
    self._require_muparser()
    if not _has_muparser_user_data():
      self.skipTest("muparser does not support isnodata function")

    with utilities_util.vsimem_dir("raster_calc_nodata_var") as vsimem:
      src_tif = f"{vsimem}/src.tif"
      with gdal.GetDriverByName("GTiff").Create(
          src_tif, 1, 1, eType=gdal.GDT_Float32
      ) as ds:
        ds.GetRasterBand(1).SetNoDataValue(-999)
        ds.GetRasterBand(1).Fill(-999)

      calc = _get_calc_alg()
      calc["input"] = [src_tif]
      calc["calc"] = "isnodata(X) ? NODATA : 5"
      calc["output-format"] = "stream"
      calc["nodata"] = -802
      self.assertTrue(calc.Run())

      result = utilities_util.get_arg_dataset(
          calc, "output"
      ).ReadAsMaskedArray()
      self.assertTrue(np.all(result.mask))

  def test_creation_options(self) -> None:
    self._require_muparser()
    infile = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_calc_co") as vsimem:
      outfile = f"{vsimem}/out.tif"
      calc = _get_calc_alg()
      calc["input"] = [infile]
      calc["output"] = outfile
      calc["creation-option"] = ["COMPRESS=LZW"]
      calc["calc"] = ["X[1] + 3"]
      self.assertTrue(calc.Run())

      with gdal.Open(outfile) as dst:
        self.assertEqual(
            dst.GetMetadata("IMAGE_STRUCTURE")["COMPRESSION"], "LZW"
        )

  def test_output_format(self) -> None:
    self._require_muparser()
    infile = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_calc_of") as vsimem:
      outfile = f"{vsimem}/out.unknown"
      calc = _get_calc_alg()
      calc["input"] = [infile]
      calc["output"] = outfile
      calc["output-format"] = "GTiff"
      calc["calc"] = ["X + 3"]
      self.assertTrue(calc.Run())

      with gdal.Open(outfile) as dst:
        self.assertEqual(dst.GetDriver().GetName(), "GTiff")

  def test_output_type(self) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_type") as vsimem:
      src_tif = f"{vsimem}/src.tif"
      with gdal.GetDriverByName("GTiff").Create(
          src_tif, 5, 5, eType=gdal.GDT_Int32
      ) as src_ds:
        src_ds.GetRasterBand(1).Fill(500)

      calc = _get_calc_alg()
      calc["input"] = src_tif
      calc["output"] = ""
      calc["output-format"] = "MEM"
      calc["calc"] = "X > 256 ? 100 : 50"
      calc["output-data-type"] = "Byte"
      self.assertTrue(calc.Run())

      dst_ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(dst_ds.GetRasterBand(1).DataType, gdal.GDT_Byte)
      self.assertTrue(np.all(dst_ds.ReadAsArray() == 100))
      self.assertTrue(calc.Finalize())

  def test_invalid_nodata_for_output_type(self) -> None:
    with utilities_util.vsimem_dir("raster_calc_bad_nd") as vsimem:
      src_tif = f"{vsimem}/src.tif"
      with gdal.GetDriverByName("GTiff").Create(
          src_tif, 1, 1, eType=gdal.GDT_Int16
      ) as ds:
        ds.GetRasterBand(1).SetNoDataValue(-9)
        ds.GetRasterBand(1).Fill(-9)

      calc = _get_calc_alg()
      calc["input"] = src_tif
      calc["output-format"] = "stream"
      calc["calc"] = "X"
      calc["output-data-type"] = "Byte"
      calc["nodata"] = -9
      with self.assertRaisesRegex(
          RuntimeError, "Byte cannot represent NoData value -9"
      ):
        calc.Run()

  def test_output_nodata_taken_from_source(self) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_prop_nd") as vsimem:
      src_tif = f"{vsimem}/src.tif"
      with gdal.GetDriverByName("GTiff").Create(src_tif, 1, 1) as ds:
        ds.GetRasterBand(1).SetNoDataValue(255)
        ds.GetRasterBand(1).Fill(255)

      calc = _get_calc_alg()
      calc["input"] = src_tif
      calc["output-format"] = "stream"
      calc["calc"] = "X"
      calc["propagate-nodata"] = True
      self.assertTrue(calc.Run())

      ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(ds.GetRasterBand(1).GetNoDataValue(), 255)
      with self.assertRaisesRegex(RuntimeError, "no valid pixels"):
        ds.GetRasterBand(1).ComputeRasterMinMax(False)

  def test_overwrite(self) -> None:
    self._require_muparser()
    infile = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_calc_overwrite") as vsimem:
      outfile = f"{vsimem}/out.tif"
      gdal.CopyFile(infile, outfile)

      calc = _get_calc_alg()
      calc["input"] = [infile]
      calc["output"] = outfile
      calc["calc"] = ["X + 3"]
      with self.assertRaisesRegex(RuntimeError, "already exists"):
        calc.Run()

      calc["overwrite"] = True
      self.assertTrue(calc.Run())

  @parameterized.parameters("X + 3", "X[1] + 3")
  def test_basic_named_source(self, expr: str) -> None:
    self._require_muparser()
    infile = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_calc_named") as vsimem:
      outfile = f"{vsimem}/out.tif"
      calc = _get_calc_alg()
      calc["input"] = [f"X={infile}"]
      calc["output"] = outfile
      calc["calc"] = [expr]
      self.assertTrue(calc.Run())

      with gdal.Open(infile) as src, gdal.Open(outfile) as dst:
        np.testing.assert_array_equal(
            src.ReadAsArray() + 3.0, dst.ReadAsArray()
        )

  def test_several_inputs_same_name(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    uint16_tif = utilities_util.get_testdata_path("../gcore/data/uint16.tif")
    with utilities_util.vsimem_dir("raster_calc_dup_name") as vsimem:
      calc = _get_calc_alg()
      calc["input"] = [f"A={byte_tif}", f"A={uint16_tif}"]
      calc["output"] = f"{vsimem}/out.vrt"
      calc["calc"] = "A"
      with self.assertRaisesRegex(
          RuntimeError, "An input with name 'A' has already been provided"
      ):
        calc.Run()

  @parameterized.parameters("muparser", "builtin")
  def test_several_inputs_no_name(self, dialect: str) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    uint16_tif = utilities_util.get_testdata_path("../gcore/data/uint16.tif")
    with utilities_util.vsimem_dir("raster_calc_no_name") as vsimem:
      calc = _get_calc_alg()
      calc["input"] = [byte_tif, uint16_tif]
      calc["output"] = f"{vsimem}/out.vrt"
      calc["calc"] = "sum"
      calc["dialect"] = dialect

      if dialect == "muparser":
        with self.assertRaisesRegex(RuntimeError, "Inputs must be named"):
          calc.Run()
      else:
        self.assertTrue(calc.Run())

  @parameterized.parameters(
      ("_pi", "Name '_pi' is illegal because it starts with a '_'"),
      ("0ko", "Name '0ko' is illegal because it starts with a '0'"),
      ("ko-", "Name 'ko-' is illegal because character '-' is not allowed"),
      ("ok", None),
      ("ok_", None),
      ("o0123456789", None),
  )
  def test_name(self, name: str, expected_error_msg: str | None) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    calc = _get_calc_alg()
    calc["input"] = f"{name}={byte_tif}"
    calc["output-format"] = "MEM"
    calc["calc"] = name

    if expected_error_msg:
      with self.assertRaisesRegex(RuntimeError, expected_error_msg):
        calc.Run()
    else:
      self._require_muparser()
      self.assertTrue(calc.Run())

  def test_multiple_calcs(self) -> None:
    self._require_muparser()
    infile = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_calc_multi") as vsimem:
      outfile = f"{vsimem}/out.tif"
      calc = _get_calc_alg()
      calc["input"] = [infile]
      calc["output"] = outfile
      calc["calc"] = ["X + 3", "sqrt(X)"]
      self.assertTrue(calc.Run())

      with gdal.Open(infile) as src, gdal.Open(outfile) as dst:
        src_dat = src.ReadAsArray()
        dst_dat = dst.ReadAsArray()

      np.testing.assert_array_equal(src_dat + 3.0, dst_dat[0])
      np.testing.assert_array_equal(
          np.sqrt(src_dat.astype(np.float64)), dst_dat[1]
      )

  @parameterized.parameters(
      ("(A+B) / (A - B + 3)", lambda a, b: (a + b) / (a - b + 3)),
      ("A[2] + B", lambda a, b: a[1] + b),
  )
  def test_multiple_inputs(
      self,
      expr: str,
      expected_fn: Callable[[np.ndarray, np.ndarray], np.ndarray],
  ) -> None:
    self._require_muparser()
    nx, ny, nz = 3, 5, 2
    arr_a = np.arange(nx * ny * nz, dtype=np.float32).reshape(nz, ny, nx)
    arr_b = np.sqrt(arr_a)

    with utilities_util.vsimem_dir("raster_calc_multi_in") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      outfile = f"{vsimem}/out.tif"

      with gdal.GetDriverByName("GTiff").Create(
          input_1, nx, ny, nz, eType=gdal.GDT_Float32
      ) as ds:
        ds.WriteArray(arr_a)
      with gdal.GetDriverByName("GTiff").Create(
          input_2, nx, ny, nz, eType=gdal.GDT_Float32
      ) as ds:
        ds.WriteArray(arr_b)

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["output"] = outfile
      calc["calc"] = [expr]
      self.assertTrue(calc.Run())

      with gdal.Open(outfile) as dst:
        np.testing.assert_allclose(
            dst.ReadAsArray(), expected_fn(arr_a, arr_b), rtol=1e-6
        )

  @parameterized.parameters("A+B", "sum(A, B)")
  def test_inputs_from_file(self, formula: str) -> None:
    self._require_muparser()
    with (
        utilities_util.vsimem_dir("raster_calc_file_list") as vsimem,
        gcore_util.TestTemporaryDirectory() as tmpdir,
    ):
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      outfile = f"{vsimem}/out.tif"
      input_txt = os.path.join(tmpdir, "inputs.txt")

      with gdal.GetDriverByName("GTiff").Create(input_1, 2, 2) as ds:
        ds.GetRasterBand(1).Fill(1)
      with gdal.GetDriverByName("GTiff").Create(input_2, 2, 2) as ds:
        ds.GetRasterBand(1).Fill(2)

      with open(input_txt, "w", encoding="utf-8") as txtfile:
        txtfile.write(f"A={input_1}\n")
        txtfile.write(f"B={input_2}\n")

      calc = _get_calc_alg()
      calc["input"] = [f"@{input_txt}"]
      calc["output"] = outfile
      calc["calc"] = formula
      self.assertTrue(calc.Run())

      with gdal.Open(outfile) as dst:
        self.assertTrue(np.all(dst.ReadAsArray() == 3))

  def test_different_band_counts(self) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_diff_bands") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      outfile = f"{vsimem}/out.tif"

      with gdal.GetDriverByName("GTiff").Create(input_1, 2, 2, 2) as ds:
        ds.GetRasterBand(1).Fill(1)
        ds.GetRasterBand(2).Fill(2)
      with gdal.GetDriverByName("GTiff").Create(input_2, 2, 2, 3) as ds:
        ds.GetRasterBand(1).Fill(3)
        ds.GetRasterBand(2).Fill(4)
        ds.GetRasterBand(3).Fill(5)

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["output"] = outfile
      calc["calc"] = ["A[1] + A[2] + B[1] + B[2] + B[3]"]
      self.assertTrue(calc.Run())

      with gdal.Open(outfile) as dst:
        self.assertTrue(np.all(dst.ReadAsArray() == (1 + 2 + 3 + 4 + 5)))

  def test_different_resolutions(self) -> None:
    self._require_muparser()
    xmax, ymax = 60, 60
    resolutions = [30, 20, 60]

    with utilities_util.vsimem_dir("raster_calc_diff_res") as vsimem:
      inputs = [f"{vsimem}/in_{i}.tif" for i in range(len(resolutions))]
      outfile = f"{vsimem}/out.tif"

      for res, fname in zip(resolutions, inputs):
        with gdal.GetDriverByName("GTiff").Create(
            fname, int(xmax / res), int(ymax / res), 1
        ) as ds:
          ds.GetRasterBand(1).Fill(res)
          ds.SetGeoTransform((0, res, 0, ymax, 0, -res))

      calc = _get_calc_alg()
      calc["input"] = [f"A={inputs[0]}", f"B={inputs[1]}", f"C={inputs[2]}"]
      calc["calc"] = ["A + B + C"]
      calc["output"] = outfile

      calc["no-check-extent"] = True
      with self.assertRaisesRegex(
          RuntimeError, "Inputs do not have the same dimensions"
      ):
        calc.Run()
      calc["no-check-extent"] = False

      self.assertTrue(calc.Run())

      with gdal.Open(outfile) as ds:
        gcd_res = functools.reduce(math.gcd, resolutions)
        self.assertEqual(ds.RasterXSize, xmax / gcd_res)
        self.assertEqual(ds.RasterYSize, ymax / gcd_res)
        self.assertTrue(np.all(ds.ReadAsArray() == sum(resolutions)))

  def test_error_extent_mismatch(self) -> None:
    with utilities_util.vsimem_dir("raster_calc_extent_err") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      outfile = f"{vsimem}/out.tif"

      with gdal.GetDriverByName("GTiff").Create(input_1, 2, 2) as ds:
        ds.SetGeoTransform((0, 1, 0, 2, 0, -1))
      with gdal.GetDriverByName("GTiff").Create(input_2, 2, 2) as ds:
        ds.SetGeoTransform((0, 2, 0, 4, 0, -2))

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["output"] = outfile
      calc["calc"] = ["A+B"]
      with self.assertRaisesRegex(RuntimeError, "extents are inconsistent"):
        calc.Run()

      if _has_vrt_expression_dialect("muparser"):
        calc["no-check-extent"] = True
        self.assertTrue(calc.Run())
        with gdal.Open(input_1) as src, gdal.Open(outfile) as dst:
          self.assertEqual(src.GetGeoTransform(), dst.GetGeoTransform())

  def test_error_extent_within_tolerance(self) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_extent_tol") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      outfile = f"{vsimem}/out.tif"

      with gdal.GetDriverByName("GTiff").Create(input_2, 3600, 3600) as ds:
        ds.SetGeoTransform((
            2.4999861111111112e01,
            2.7777777777777778e-04,
            0.0,
            8.0000138888888884e01,
            0.0,
            -2.7777777777777778e-04,
        ))
      with gdal.GetDriverByName("GTiff").Create(input_1, 3600, 3600) as ds:
        ds.SetGeoTransform((
            2.4999861111111112e01,
            2.7777777777777778e-04,
            0.0,
            8.0000138888888884e01,
            0.0,
            -2.7777777777778173e-04,
        ))

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["output"] = outfile
      calc["calc"] = ["A+B"]
      self.assertTrue(calc.Run())

      with gdal.Open(input_1) as src, gdal.Open(outfile) as dst:
        self.assertEqual(src.GetGeoTransform(), dst.GetGeoTransform())

  def test_error_crs_mismatch(self) -> None:
    with utilities_util.vsimem_dir("raster_calc_crs_err") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      outfile = f"{vsimem}/out.tif"

      with gdal.GetDriverByName("GTiff").Create(input_1, 2, 2) as ds:
        ds.SetProjection("EPSG:4326")
      with gdal.GetDriverByName("GTiff").Create(input_2, 2, 2) as ds:
        ds.SetProjection("EPSG:4269")

      calc = _get_calc_alg()
      calc["input"] = [f"B={input_1}", f"A={input_2}"]
      calc["output"] = outfile
      calc["calc"] = ["A+B"]
      with self.assertRaisesRegex(
          RuntimeError, "spatial reference systems are inconsistent"
      ):
        calc.Run()

      if _has_vrt_expression_dialect("muparser"):
        calc["no-check-srs"] = True
        self.assertTrue(calc.Run())
        with gdal.Open(input_1) as src, gdal.Open(outfile) as dst:
          self.assertIsSame(src.GetSpatialRef(), dst.GetSpatialRef())

  @parameterized.parameters(((2, 3),), ((2, 4),))
  def test_error_band_count_mismatch(self, bands: tuple[int, int]) -> None:
    with utilities_util.vsimem_dir("raster_calc_band_err") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      outfile = f"{vsimem}/out.tif"

      gdal.GetDriverByName("GTiff").Create(input_1, 2, 2, bands[0])
      gdal.GetDriverByName("GTiff").Create(input_2, 2, 2, bands[1])

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["output"] = outfile
      calc["calc"] = ["A+B"]
      with self.assertRaisesRegex(
          RuntimeError, "incompatible numbers of bands"
      ):
        calc.Run()

      if _has_vrt_expression_dialect("muparser"):
        calc["calc"] = ["A+B[1]"]
        self.assertTrue(calc.Run())

  @parameterized.parameters(
      ("aX + 2", "aX", 1, ["aX[1] + 2"]),
      ("aX + 2", "aX", 2, ["aX[1] + 2", "aX[2] + 2"]),
      ("aX + 2", "X", 1, ["aX + 2"]),
      ("aX + 2", "a", 1, ["aX + 2"]),
      ("2 + aX", "X", 1, ["2 + aX"]),
      ("2 + aX", "aX", 1, ["2 + aX[1]"]),
      ("B1 + B10", "B1", 1, ["B1[1] + B10"]),
      ("B1[1] + B10", "B1", 2, ["B1[1] + B10"]),
      ("B1[1] + B1", "B1", 2, ["B1[1] + B1[1]", "B1[1] + B1[2]"]),
      ("SIN(N) + N", "N", 1, ["SIN(N[1]) + N[1]"]),
      ("SUM(N,N2) + N", "N", 1, ["SUM(N[1],N2) + N[1]"]),
      ("SUM(N,N2) + N", "N2", 1, ["SUM(N,N2[1]) + N"]),
      ("A_X + X", "X", 1, ["A_X + X[1]"]),
  )
  def test_expression_rewriting(
      self, expr: str, source: str, bands: int, expected: list[str]
  ) -> None:
    with utilities_util.vsimem_dir("raster_calc_rewrite") as vsimem:
      outfile = f"{vsimem}/out.vrt"
      infile = f"{vsimem}/input.tif"
      gdal.GetDriverByName("GTiff").Create(infile, 2, 2, bands)

      calc = _get_calc_alg()
      calc["input"] = [f"{source}={infile}"]
      calc["output"] = outfile
      calc["calc"] = [expr]
      calc["no-check-expression"] = True
      self.assertTrue(calc.Run())

      xml_text = gcore_util.vsi_read_file_as_string(outfile, 1024 * 1024)
      root = ET.fromstring(xml_text)
      rewritten = [
          node.attrib["expression"]
          for node in root.findall(".//PixelFunctionArguments")
      ]
      self.assertEqual(rewritten, expected)

  def test_gdalg_json(self) -> None:
    if not utilities_util.has_driver("GDALG"):
      self.skipTest("GDALG driver not available")
    self._require_muparser()
    rgbsmall = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    with utilities_util.vsimem_dir("raster_calc_gdalg") as vsimem:
      outfile = f"{vsimem}/out.gdalg.json"
      calc = _get_calc_alg()
      calc["input"] = rgbsmall
      calc["output"] = outfile
      calc["calc"] = "X"
      self.assertTrue(calc.Run())
      self.assertTrue(calc.Finalize())

      with gdal.Open(outfile) as ds:
        self.assertEqual(ds.GetRasterBand(1).Checksum(), 21212)
        self.assertEqual(ds.GetRasterBand(2).Checksum(), 21053)
        self.assertEqual(ds.GetRasterBand(3).Checksum(), 21349)

  @parameterized.parameters(
      ("VRT", None),
      ("VRT", "vrt"),
      ("stream", None),
      ("GDALG", None),
      ("GDALG", "gdalg.json"),
      ("GTiff", None),
      ("GTiff", "tif"),
  )
  def test_invalid_formula(self, output_format: str, ext: str | None) -> None:
    if output_format != "stream" and not utilities_util.has_driver(
        output_format
    ):
      self.skipTest(f"{output_format} driver not available")
    self._require_muparser()

    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    with utilities_util.vsimem_dir("raster_calc_bad_formula") as vsimem:
      calc = _get_calc_alg()
      calc["input"] = byte_tif
      if ext:
        calc["output"] = f"{vsimem}/out.{ext}"
      else:
        calc["output-format"] = output_format
        calc["output"] = f"{vsimem}/out"
      calc["calc"] = "invalid"
      with self.assertRaisesRegex(RuntimeError, "invalid variable name"):
        calc.Run()

  def test_reference_several_bands_to_stream(self) -> None:
    self._require_muparser()
    rgbsmall = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    calc = _get_calc_alg()
    calc["input"] = rgbsmall
    calc["output-format"] = "stream"
    calc["output"] = "streamed"
    calc["calc"] = "sum(X[1], X[2], X[3])"
    self.assertTrue(calc.Run())

    out_ds = utilities_util.get_arg_dataset(calc, "output")
    self.assertEqual(out_ds.RasterCount, 1)
    self.assertEqual(out_ds.GetRasterBand(1).Checksum(), 21240)

  @parameterized.parameters("avg", "min", "max", "sum")
  def test_muparser_flatten(self, fn: str) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_flatten") as vsimem:
      in1 = f"{vsimem}/in1.tif"
      in2 = f"{vsimem}/in2.tif"
      with gdal.GetDriverByName("GTiff").Create(in1, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(10)
        ds.GetRasterBand(2).Fill(100)
      with gdal.GetDriverByName("GTiff").Create(in2, 1, 1, 3) as ds:
        ds.GetRasterBand(1).Fill(2)
        ds.GetRasterBand(2).Fill(20)
        ds.GetRasterBand(3).Fill(200)

      calc = _get_calc_alg()
      calc["input"] = [f"A={in1}", f"B={in2}"]
      calc["output-format"] = "stream"
      calc["output"] = ""
      calc["calc"] = f"{fn}(A)-{fn}(B)"
      calc["flatten"] = True
      self.assertTrue(calc.Run())

      ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(ds.RasterCount, 1)
      if fn == "sum":
        expected_val = (10 + 100) - (2 + 20 + 200)
      elif fn == "avg":
        expected_val = (10 + 100) / 2 - (2 + 20 + 200) / 3
      elif fn == "max":
        expected_val = 100 - 200
      else:
        expected_val = 10 - 2

      self.assertEqual(
          ds.GetRasterBand(1).ComputeRasterMinMax(False),
          (expected_val, expected_val),
      )

  def test_muparser_flatten_not_an_aggregate(self) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_flat_nonagg") as vsimem:
      src = f"{vsimem}/src.tif"
      with gdal.GetDriverByName("GTiff").Create(src, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(10)
        ds.GetRasterBand(2).Fill(100)

      calc = _get_calc_alg()
      calc["input"] = [src]
      calc["output-format"] = "stream"
      calc["output"] = ""
      calc["calc"] = "sin(X)"
      calc["flatten"] = True
      self.assertTrue(calc.Run())
      self.assertEqual(
          utilities_util.get_arg_dataset(calc, "output").RasterCount, 2
      )

  def test_muparser_partial_flatten(self) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_flat_partial") as vsimem:
      in1 = f"{vsimem}/in1.tif"
      with gdal.GetDriverByName("GTiff").Create(in1, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(10)
        ds.GetRasterBand(2).Fill(100)

      calc = _get_calc_alg()
      calc["input"] = [f"A={in1}"]
      calc["output-format"] = "stream"
      calc["output"] = ""
      calc["calc"] = "A / sum(A)"
      calc["flatten"] = True
      self.assertTrue(calc.Run())

      ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(ds.RasterCount, 2)
      self.assertEqual(
          ds.GetRasterBand(1).ComputeRasterMinMax(False), (10 / 110, 10 / 110)
      )
      self.assertEqual(
          ds.GetRasterBand(2).ComputeRasterMinMax(False), (100 / 110, 100 / 110)
      )

  def test_muparser_nothing_to_flatten(self) -> None:
    self._require_muparser()
    with utilities_util.vsimem_dir("raster_calc_flat_none") as vsimem:
      in1 = f"{vsimem}/in1.tif"
      in2 = f"{vsimem}/in2.tif"
      with gdal.GetDriverByName("GTiff").Create(in1, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(10)
        ds.GetRasterBand(2).Fill(100)
      with gdal.GetDriverByName("GTiff").Create(in2, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(20)
        ds.GetRasterBand(2).Fill(200)

      calc = _get_calc_alg()
      calc["input"] = [f"A={in1}", f"B={in2}"]
      calc["output-format"] = "stream"
      calc["output"] = ""
      calc["calc"] = "(A + B)"
      calc["flatten"] = True
      self.assertTrue(calc.Run())

      ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(ds.RasterCount, 2)
      self.assertEqual(ds.GetRasterBand(1).ComputeRasterMinMax(False), (30, 30))
      self.assertEqual(
          ds.GetRasterBand(2).ComputeRasterMinMax(False), (300, 300)
      )

  @parameterized.parameters("min", "max", "mode", "mean", "median")
  def test_dialect_builtin(self, expression: str) -> None:
    rgbsmall = utilities_util.get_testdata_path("../gcore/data/rgbsmall.tif")
    calc = _get_calc_alg()
    calc["input"] = rgbsmall
    calc["output-format"] = "stream"
    calc["output"] = ""
    calc["calc"] = expression
    calc["dialect"] = "builtin"
    calc["flatten"] = True
    self.assertTrue(calc.Run())

    ds = utilities_util.get_arg_dataset(calc, "output")
    self.assertEqual(ds.RasterCount, 1)
    if expression in ("min", "max", "mode"):
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Byte)
    else:
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float64)

    if expression == "mean":
      self.assertEqual(ds.GetRasterBand(1).Checksum(), 21559)

  def test_pixel_function_arg(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    calc = _get_calc_alg()
    calc["input"] = byte_tif
    calc["output-format"] = "stream"
    calc["output"] = ""
    calc["calc"] = "sum(k=1)"
    calc["dialect"] = "builtin"
    self.assertTrue(calc.Run())

    ds = utilities_util.get_arg_dataset(calc, "output")
    self.assertEqual(ds.RasterCount, 1)
    self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float64)
    self.assertEqual(ds.GetRasterBand(1).Checksum(), 4455)

  def test_builtin_with_multiple_inputs(self) -> None:
    with utilities_util.vsimem_dir("raster_calc_builtin_multi") as vsimem:
      in1 = f"{vsimem}/in1.tif"
      in2 = f"{vsimem}/in2.tif"
      with gdal.GetDriverByName("GTiff").Create(in1, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(10)
        ds.GetRasterBand(2).Fill(100)
      with gdal.GetDriverByName("GTiff").Create(in2, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(20)
        ds.GetRasterBand(2).Fill(200)

      calc = _get_calc_alg()
      calc["input"] = [f"A={in1}", f"B={in2}"]
      calc["output-format"] = "stream"
      calc["output"] = ""
      calc["calc"] = "mean"
      calc["dialect"] = "builtin"
      self.assertTrue(calc.Run())

      ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(ds.RasterCount, 2)
      self.assertEqual(ds.GetRasterBand(1).DataType, gdal.GDT_Float64)
      self.assertEqual(ds.GetRasterBand(1).ComputeRasterMinMax(False), (15, 15))
      self.assertEqual(
          ds.GetRasterBand(2).ComputeRasterMinMax(False), (150, 150)
      )

  def test_builtin_with_multiple_formula(self) -> None:
    byte_tif = utilities_util.get_testdata_path("../gcore/data/byte.tif")
    calc = _get_calc_alg()
    calc["input"] = byte_tif
    calc["output-format"] = "stream"
    calc["output"] = ""
    calc["calc"] = ["mean", "sum(k=1)"]
    calc["dialect"] = "builtin"
    self.assertTrue(calc.Run())

    ds = utilities_util.get_arg_dataset(calc, "output")
    self.assertEqual(ds.RasterCount, 2)
    self.assertEqual(ds.GetRasterBand(1).ComputeRasterMinMax(False), (74, 255))
    self.assertEqual(ds.GetRasterBand(2).ComputeRasterMinMax(False), (75, 256))

  def test_complete(self) -> None:
    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "calc", "--calc"]
    )
    self.assertEqual(code, 0)
    self.assertNotIn("mean", out)

    out, _, code = utilities_util.run_gdal_cli(
        ["completion", "gdal", "raster", "calc", "--dialect=builtin", "--calc"]
    )
    self.assertEqual(code, 0)
    self.assertIn("mean", out)

  def test_sum_builtin_one_band_two_bands(self) -> None:
    with utilities_util.vsimem_dir("raster_calc_sum_1_2") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      with gdal.GetDriverByName("GTiff").Create(input_1, 1, 1, 1) as ds:
        ds.GetRasterBand(1).Fill(1)
      with gdal.GetDriverByName("GTiff").Create(input_2, 1, 1, 2) as ds:
        ds.GetRasterBand(1).Fill(10)
        ds.GetRasterBand(2).Fill(100)

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["output-format"] = "MEM"
      calc["calc"] = "sum"
      calc["dialect"] = "builtin"
      self.assertTrue(calc.Run())

      out_ds = utilities_util.get_arg_dataset(calc, "output")
      self.assertEqual(out_ds.RasterCount, 2)
      self.assertEqual(
          out_ds.GetRasterBand(1).ComputeRasterMinMax(False), (11, 11)
      )
      self.assertEqual(
          out_ds.GetRasterBand(2).ComputeRasterMinMax(False), (101, 101)
      )

  def test_sum_builtin_two_bands_three_bands_fail(self) -> None:
    with utilities_util.vsimem_dir("raster_calc_sum_2_3") as vsimem:
      input_1 = f"{vsimem}/in1.tif"
      input_2 = f"{vsimem}/in2.tif"
      gdal.GetDriverByName("GTiff").Create(input_1, 1, 1, 2)
      gdal.GetDriverByName("GTiff").Create(input_2, 1, 1, 3)

      calc = _get_calc_alg()
      calc["input"] = [f"A={input_1}", f"B={input_2}"]
      calc["output-format"] = "MEM"
      calc["calc"] = "sum"
      calc["dialect"] = "builtin"
      with self.assertRaisesRegex(
          RuntimeError,
          r"Expression cannot operate on all bands of rasters with "
          r"incompatible numbers of bands \(source B has 3 bands but expected "
          r"to have 1 or 2 bands\)",
      ):
        calc.Run()


if __name__ == "__main__":
  absltest.main()
