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
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2014, Even Rouault <even dot rouault at mines-paris dot org>
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
###############################################################################
"""Test Thin Plate Spline transformer in alg/gdal_tps.cpp.

Rewrite of:

https://trac.osgeo.org/gdal/browser/trunk/autotest/alg/tps.py
"""

import unittest

from osgeo import gdal
from osgeo import osr

from autotest2.gcore import gcore_util


class TransformGeolocTest(unittest.TestCase):

  def testGroundControlPoints(self):
    # https://trac.osgeo.org/gdal/ticket/5586
    driver = gdal.GetDriverByName('MEM')

    filepath = 'tps.mem'
    with gcore_util.GdalUnlinkWhenDone(filepath):
      datasource = driver.Create('tps.mem', 2, 2)

      # An set of ground control points that will generate an error.
      gcp_list = [
          gdal.GCP(0, 0, 0, 0, 0),
          gdal.GCP(0, 50, 0, 0, 50),
          gdal.GCP(50, 0, 0, 50, 0),
          gdal.GCP(50, 50, 0, 50, 50),
          gdal.GCP(0 * 25, 0 * 25, 0, 25, 25)
      ]
      datasource.SetGCPs(gcp_list, osr.GetUserInputAsWKT('WGS84'))

      utm_wkt = osr.GetUserInputAsWKT('+proj=utm +zone=11 +datum=WGS84')

      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        transformer = gdal.Transformer(
            datasource, None, ['DST_SRS=' + utm_wkt, 'METHOD=GCP_TPS'])

      self.assertIsNotNone(transformer)

      # TODO(schwehr): The error observed is 3 (CPLE_FileIO), but
      # expected 1 (CPLE_AppDefined).
      self.assertNotEqual(gdal.GetLastErrorType(), gdal.CPLE_None)

      err_msg = gdal.GetLastErrorMsg()
      self.assertIn('problem inverting', err_msg)
      self.assertIn('interpolation matrix', err_msg)


if __name__ == '__main__':
  unittest.main()
