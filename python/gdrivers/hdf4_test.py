# Copyright 2016 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Test HDF4 grids."""

import os


from osgeo import gdal
import unittest
from autotest2.gdrivers import gdrivers_util

DRIVER_BASE = gdrivers_util.HDF4_DRIVER
DRIVER = gdrivers_util.HDF4IMAGE_DRIVER
EXT = '.hdf'


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class Hdf4Test(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(Hdf4Test, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER_BASE, filename))

  def testReadSimple(self):
    filename_info = [
        ('byte_2.hdf', gdal.GDT_Byte),
        ('byte_3.hdf', gdal.GDT_Byte),
        ('float32_2.hdf', gdal.GDT_Float32),
        ('float32_3.hdf', gdal.GDT_Float32),
        ('float64_2.hdf', gdal.GDT_Float64),
        ('float64_3.hdf', gdal.GDT_Float64),
        ('int16_2.hdf', gdal.GDT_Int16),
        ('int16_3.hdf', gdal.GDT_Int16),
        ('int32_2.hdf', gdal.GDT_Int32),
        ('int32_3.hdf', gdal.GDT_Int32),
        ('uint16_2.hdf', gdal.GDT_UInt16),
        ('uint16_3.hdf', gdal.GDT_UInt16),
        ('uint32_2.hdf', gdal.GDT_UInt32),
        ('uint32_3.hdf', gdal.GDT_UInt32)]

    for filename, gdal_type in filename_info:
      self.CheckDriver()
      filepath = self.getTestFilePath(filename)
      self.CheckOpen(filepath)
      self.CheckGeoTransform((440720.0, 60.0, 0, 3751320.0, 0, -60.0))
      prj_expected = """PROJCS["UTM",
    GEOGCS["North_American_Datum_1927",
        DATUM["North_American_Datum_1927",
            SPHEROID["Clarke 1866",6378206.4,294.9786982139006]],
        PRIMEM["Greenwich",0],
        UNIT["degree",0.0174532925199433]],
    PROJECTION["Transverse_Mercator"],
    PARAMETER["latitude_of_origin",0],
    PARAMETER["central_meridian",-117],
    PARAMETER["scale_factor",0.9996],
    PARAMETER["false_easting",500000],
    PARAMETER["false_northing",0],
    UNIT["Meter",1]]"""
      self.CheckProjection(prj_expected)
      self.CheckBand(1, 4672, gdal_type)

  def testReadUtm(self):
    filename_info = ['utmsmall_2.hdf', 'utmsmall_3.hdf']

    for filename in filename_info:
      self.CheckDriver()
      filepath = self.getTestFilePath(filename)
      self.CheckOpen(filepath)
      self.CheckGeoTransform((440720.0, 60.0, 0, 3751320.0, 0, -60.0))
      prj_expected = """PROJCS["NAD27 / UTM zone 11N",
    GEOGCS["NAD27",
        DATUM["North_American_Datum_1927",
            SPHEROID["Clarke 1866",6378206.4,294.9786982139006,
                AUTHORITY["EPSG","7008"]],
            AUTHORITY["EPSG","6267"]],
        PRIMEM["Greenwich",0],
        UNIT["degree",0.0174532925199433],
        AUTHORITY["EPSG","4267"]],
    PROJECTION["Transverse_Mercator"],
    PARAMETER["latitude_of_origin",0],
    PARAMETER["central_meridian",-117],
    PARAMETER["scale_factor",0.9996],
    PARAMETER["false_easting",500000],
    PARAMETER["false_northing",0],
    UNIT["metre",1,
        AUTHORITY["EPSG","9001"]],
    AUTHORITY["EPSG","26711"]]"""
      self.CheckProjection(prj_expected)
      self.CheckBand(1, 50054, gdal.GDT_Byte)


if __name__ == '__main__':
  unittest.main()
