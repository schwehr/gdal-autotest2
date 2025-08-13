# Copyright 2014 Google Inc. All Rights Reserved.
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
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

"""Test the Arc/Info ASCII Grid raster driver in gdal.

Format is described here:

https://gdal.org/drivers/raster/aaigrid.html

Rewrite of aaigrid.py:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/aaigrid.py
"""
import glob
import os
import unittest

from osgeo import gdal

from autotest2.gdrivers import gdrivers_util

DRIVER = gdrivers_util.AAIGRID_DRIVER
EXT = '.asc'


class AaigridTest(gdrivers_util.DriverTestCase):

  def setUp(self):  # pytype: disable=signature-mismatch
    super().setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testSimpleRead(self):
    self.CheckDriver()
    filepath = self.getTestFilePath('pixel_per_line.asc')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((100000.0, 50, 0, 650600.0, 0, -50))
    prj_expected = """PROJCS["unnamed",
    GEOGCS["NAD83",
        DATUM["North_American_Datum_1983",
            SPHEROID["GRS 1980",6378137,298.257222101,
                AUTHORITY["EPSG","7019"]],
            TOWGS84[0,0,0,0,0,0,0],
            AUTHORITY["EPSG","6269"]],
        PRIMEM["Greenwich",0,
            AUTHORITY["EPSG","8901"]],
        UNIT["degree",0.0174532925199433,
            AUTHORITY["EPSG","9108"]],
        AUTHORITY["EPSG","4269"]],
    PROJECTION["Albers_Conic_Equal_Area"],
    PARAMETER["standard_parallel_1",61.66666666666666],
    PARAMETER["standard_parallel_2",68],
    PARAMETER["latitude_of_center",59],
    PARAMETER["longitude_of_center",-132.5],
    PARAMETER["false_easting",500000],
    PARAMETER["false_northing",500000],
    UNIT["METERS",1]]"""
    self.CheckProjection(prj_expected)
    self.CheckBand(1, 1123, gdal.GDT_Float32, -99999)

  def testComma(self):
    filepath = self.getTestFilePath('pixel_per_line_comma.asc')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((100000, 50, 0, 650600, 0, -50))
    self.CheckBand(1, 1123, gdal.GDT_Float32, -99999)

  def testCreateCopy(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckBand(1, 4672, gdal.GDT_Byte)
    self.CheckCreateCopy()

  def testCaseSensitive(self):
    filepath = self.getTestFilePath('case_sensitive.ASC')
    self.CheckOpen(filepath)
    self.CheckBand(1, 1123)
    self.CheckBandSubRegion(1, 187, 5, 5, 5, 5)
    prj_expected = """PROJCS["unnamed",
    GEOGCS["NAD83",
        DATUM["North_American_Datum_1983",
            SPHEROID["GRS 1980",6378137,298.257222101,
                AUTHORITY["EPSG","7019"]],
            TOWGS84[0,0,0,0,0,0,0],
            AUTHORITY["EPSG","6269"]],
        PRIMEM["Greenwich",0,
            AUTHORITY["EPSG","8901"]],
        UNIT["degree",0.0174532925199433,
            AUTHORITY["EPSG","9108"]],
        AUTHORITY["EPSG","4269"]],
    PROJECTION["Albers_Conic_Equal_Area"],
    PARAMETER["standard_parallel_1",61.66666666666666],
    PARAMETER["standard_parallel_2",68],
    PARAMETER["latitude_of_center",59],
    PARAMETER["longitude_of_center",-132.5],
    PARAMETER["false_easting",500000],
    PARAMETER["false_northing",500000],
    UNIT["METERS",1]]
    """
    self.CheckProjection(prj_expected)

  def testNoDataFloat(self):
    filepath = self.getTestFilePath('nodata_float.asc')
    self.CheckOpen(filepath)
    self.CheckBand(1, 278, gdal.GDT_Float32, -99999)

  def testAai06Int(self):
    filepath = self.getTestFilePath('nodata_int.asc')
    self.CheckOpen(filepath)
    self.CheckBand(1, 278, gdal.GDT_Int32, -99999)

  # TODO: b/429626570 - Use GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE.
  def testAai07NonSquareVrt(self):
    filepath = gdrivers_util.GetTestFilePath('raw/nonsquare.vrt')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckBand(1, 12481)
    self.CheckCreateCopy()

  def testAai08MemoryCopy(self):
    filepath = gdrivers_util.GetTestFilePath('gtiff/byte.tif')
    self.CheckOpen(filepath, check_driver=False)
    self.CheckBand(1, 4672)
    self.CheckCreateCopy(vsimem=True)

  def testAai09DecimalPrecision(self):
    filepath = gdrivers_util.GetTestFilePath('ehdr/float32.bil')
    self.CheckOpen(filepath, check_driver=False)
    dst = self.CheckCreateCopy(check_stats=False,
                               options=['DECIMAL_PRECISION=2'])
    assert dst is not None
    stats = dst.GetRasterBand(1).ComputeRasterMinMax()
    self.assertAlmostEqual(stats[0], -0.84)
    self.assertEqual(stats[1], 2)

  def testAai10DatatypeConfig(self):
    with gdal.config_option('AAIGRID_DATATYPE', 'Float64'):
      self.assertEqual(gdal.GetConfigOption('AAIGRID_DATATYPE'), 'Float64')
      filepath = self.getTestFilePath('float64.asc')
      xml_file = filepath + '.xml'
      if os.path.exists(xml_file):
        os.remove(xml_file)
      self.CheckOpen(filepath)

    band = self.src.GetRasterBand(1)
    self.assertEqual(band.DataType, gdal.GDT_Float64)

  # TODO: b/335317901 - Rewrite test 11.
  # TODO: b/335317901 - Rewrite test 12.
  # TODO: b/335317901 - Rewrite test 13.
  # TODO: b/335317901 - Rewrite test 14.
  # TODO: b/335317901 - Rewrite test 15.
  # TODO: b/335317901 - test_aaigrid_null
  # TODO: b/335317901 - test_aaigrid_null_float64
  # TODO: b/335317901 - test_aaigrid_write_south_up_raster
  # TODO: b/335317901 - test_aaigrid_starting_with_nan
  # TODO: b/335317901 - test_aaigrid_nodata_nan

  def testInfo(self):
    globpath = gdrivers_util.GetTestFilePath(DRIVER) + '/*.json'
    for jsonfile in sorted(glob.glob(globpath)):
      filepath = os.path.splitext(jsonfile)[0]
      with self.subTest(input=os.path.basename(filepath)):
        self.CheckOpen(filepath)
        self.CheckInfo()


if __name__ == '__main__':
  unittest.main()
