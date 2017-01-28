# Copyright 2015 Google Inc. All Rights Reserved.
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

"""Tests the gdal_rasterize commandline application."""

import os
import subprocess
import tempfile


from osgeo import gdal

import gflags as flags
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

FLAGS = flags.FLAGS


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
class GdalRasterizeTest(gdrivers_util.DriverTestCase):
  def setUp(self):
    self._ext = '.tif'
    super(GdalRasterizeTest, self).setUp(gdrivers_util.GTIFF_DRIVER,
                                         self._ext)

  def testRasterizeShapefile(self):
    inputpath = gcore_util.GetTestFilePath('poly.shp')
    _, outputpath = tempfile.mkstemp(dir=FLAGS.test_tmpdir,
                                     suffix=self._ext)
    binary = os.path.join('TODO(schwehr): Where?',
                          'gdal_rasterize')
    cmd = [binary,
           '-burn', '1',
           '-ot', 'gtiff',
           '-tr', '1000', '1000',
           inputpath, outputpath]
    subprocess.check_call(cmd)

    # Checks some information about the output.
    self.CheckOpen(outputpath)
    self.CheckGeoTransform(
        (477815.53125, 1000.0, 0.0, 4766110.5, 0.0, -1000.0))
    self.CheckBand(1, 3, gdal_type=gdal.GDT_Float64)


if __name__ == '__main__':
  unittest.main()
