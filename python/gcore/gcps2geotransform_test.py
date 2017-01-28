#!/usr/bin/env python
# Copyright 2014 Google Inc. All Rights Reserved.
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
# Copyright (c) 2013 Frank Warmerdam
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

"""Test GDALGCPsToGeoTransform function.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/gcps2geotransform.py
"""

import unittest


from osgeo import gdal
import unittest


# TODO(schwehr): Merge with tiff_read_test and put in a common unit location.
# Unit default transform.
EMPTY_GEOTRANSFORM = (0., 1., 0., 0., 0., 1.)


def ToGcps(src_rows):
  gcp_list = []
  for row in src_rows:
    gcp = gdal.GCP()
    gcp.GCPPixel, gcp.GCPLine, gcp.GCPX, gcp.GCPY = row
    gcp_list.append(gcp)
  return gcp_list


class Gcps2Gt(unittest.TestCase):

  def testGcp00Single(self):
    gcp = gdal.GCP()

    expected_empty = [
        0, 'GCP',
        [2, 'Id', [1, '']],
        [2, 'Pixel', [1, '0.000000000000000E+00']],
        [2, 'Line', [1, '0.000000000000000E+00']],
        [2, 'X', [1, '0.000000000000000E+00']],
        [2, 'Y', [1, '0.000000000000000E+00']]
    ]
    self.assertEqual(gcp.serialize(), expected_empty)

    gcp.GCPPixel = 100.
    gcp.GCPine = 200.
    gcp.GCPX = 410000
    gcp.GCPY = 368000

    expected = [
        0,
        'GCP',
        [2, 'Id', [1, '']],
        [2, 'Pixel', [1, '1.000000000000000E+02']],
        [2, 'Line', [1, '0.000000000000000E+00']],
        [2, 'X', [1, '4.100000000000000E+05']],
        [2, 'Y', [1, '3.680000000000000E+05']]
    ]
    self.assertEqual(gcp.serialize(), expected)

    gcp.Id = 'SomeName'

    self.assertEqual(gcp.serialize()[2][2][1], 'SomeName')

  def testGcp01ExactToGeoTransform(self):
    gt = gdal.GCPsToGeoTransform(ToGcps((
        (0., 0., 400000, 370000),
        (100., 0., 410000, 370000),
        (100., 200., 410000, 368000)
    )))
    self.assertEqual(gt, (400000., 100., 0., 370000., 0., -10.))

  def testGcp02NonExact(self):
    gt = gdal.GCPsToGeoTransform(ToGcps((
        (0., 0., 400000, 370000),
        (100., 0., 410000, 370000),
        (100., 200., 410000, 368000),
        (0., 200., 400000, 368000.01)
    )))
    for col, val in enumerate((4e5, 100., 0., 370000.0025, -5e-05, -9.999975)):
      self.assertAlmostEqual(gt[col], val)

  def testGcp03bApproxOkFalse(self):
    # GCPsToGeoTransform does not support name named arguments.
    # pylint:disable=invalid-name
    bApproxOK = 0
    gt = gdal.GCPsToGeoTransform(ToGcps((
        (0., 0., 400000, 370000),
        (100., 0., 410000, 370000),
        (100., 200., 410000, 368000),
        (0., 200., 400000, 360000)
    )), bApproxOK)
    self.assertIsNone(gt)

  def testGcp04SinglePointReturnsNone(self):
    gt = gdal.GCPsToGeoTransform(ToGcps([(0., 0., 400000, 370000)]))
    self.assertIsNone(gt)

  def testGcp05OffsetAndScaleNoRot(self):
    gt = gdal.GCPsToGeoTransform(ToGcps((
        (0., 0., 400000, 370000),
        (100., 200., 410000, 368000)
    )))
    self.assertEqual(gt, (400000., 100., 0., 370000., 0., -10.))

  def testGcp06SpecialCase4PtsInOrder(self):
    gt = gdal.GCPsToGeoTransform(ToGcps((
        (400000, 370000, 400000, 370000),
        (410000, 370000, 410000, 370000),
        (410000, 368000, 410000, 368000),
        (400000, 368000, 400000, 368000)
    )))
    self.assertEqual(gt, EMPTY_GEOTRANSFORM)

  def testGcp07HardWithoutNormalization(self):
    # 2nd and 3rd rows swapped from test 6.
    gt = gdal.GCPsToGeoTransform(ToGcps((
        (400000, 370000, 400000, 370000),
        (410000, 368000, 410000, 368000),
        (410000, 370000, 410000, 370000),
        (400000, 368000, 400000, 368000),
    )))
    for got, expected in zip(gt, EMPTY_GEOTRANSFORM):
      # TODO(schwehr): Should this exactly equal EMPTY_GEOTRANSFORM?
      # Likely there was a chance post gdal 1.10.0 to improve this.
      self.assertAlmostEqual(got, expected, places=1)

  def testGcp08MessyRealWorld(self):
    gt = gdal.GCPsToGeoTransform(ToGcps((
        (0.01, 0.04, -87.05528672907, 39.22759504228),
        (0.01, 2688.02, -86.97079900719, 39.27075713986),
        (4031.99, 2688.04, -87.05960736744, 39.37569137000),
        (1988.16, 1540.80, -87.055069186699924, 39.304963106777514),
        (1477.41, 2400.83, -87.013419295885001, 39.304705030894979),
        (1466.02, 2376.92, -87.013906298363295, 39.304056190007913),
    )))
    gt_expected = (-87.056612873288, -2.232795668658e-05, 3.178617809303e-05,
                   39.227856615716, 2.6091510188921e-05, 1.596921026218e-05)
    for got, expected in zip(gt, gt_expected):
      self.assertAlmostEqual(got, expected)


if __name__ == '__main__':
  unittest.main()
