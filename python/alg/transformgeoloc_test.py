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
# Copyright (c) 2012, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2012, Even Rouault <even dot rouault at mines-paris dot org>
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

"""Test gdaltransformgeolocs.cpp.

Rewrite of:

https://trac.osgeo.org/gdal/browser/trunk/autotest/alg/transformgeoloc.py
"""

import unittest

import numpy
from osgeo import gdal
from osgeo import osr


class TransformGeolocTest(unittest.TestCase):

  def testInMemorySmall(self):
    driver = gdal.GetDriverByName('MEM')
    geoloc_datasource = driver.Create('geoloc_1', 2, 2, 3, gdal.GDT_Float64)

    lon = numpy.asarray([[-117.0, -116.0], [-116.5, -115.5]])
    lat = numpy.asarray([[45.0, 45.5], [44.0, 44.5]])

    geoloc_datasource.GetRasterBand(1).WriteArray(lon)
    geoloc_datasource.GetRasterBand(2).WriteArray(lat)
    # Z left as default zero.

    # Create a wgs84 to utm transformer.
    wgs84_wkt = osr.GetUserInputAsWKT('WGS84')
    utm_wkt = osr.GetUserInputAsWKT('+proj=utm +zone=11 +datum=WGS84')

    ll_utm_transformer = gdal.Transformer(
        None, None, ['SRC_SRS=' + wgs84_wkt, 'DST_SRS=' + utm_wkt])

    # Transform the geoloc dataset in place.
    status = ll_utm_transformer.TransformGeolocations(
        geoloc_datasource.GetRasterBand(1),
        geoloc_datasource.GetRasterBand(2),
        geoloc_datasource.GetRasterBand(3))

    self.assertEqual(status, 0)

    expected = numpy.asarray([[[500000., 578126.73752063],
                               [540087.07398216, 619246.88515202]],
                              [[4982950.40022729, 5038982.81207935],
                               [4871994.34702687, 4928503.38229823]],
                              [[0.0, 0.0], [0.0, 0.0]]])

    result = geoloc_datasource.ReadAsArray()
    # TODO(schwehr): Use numpy.testing.all_close(result, expected)
    # once we have numpy 1.12.
    for a, b in zip(result.flatten(), expected.flatten()):
      self.assertAlmostEqual(a, b)


if __name__ == '__main__':
  unittest.main()
