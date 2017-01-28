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
"""Tests for grivers_util.py.

Assumes that gdal must be built with geotiff support.
"""
import os
import unittest

import mock
from osgeo import gdal
from autotest2.gdrivers import gdrivers_util


class DriversTest(unittest.TestCase):

  def testHasDrivers(self):
    """Have at least one driver."""
    self.assertTrue(gdrivers_util.drivers)


class GdriversDataTest(unittest.TestCase):

  def testFindFile(self):
    filepath = gdrivers_util.GetTestFilePath('byte.jp2')
    self.assertTrue(os.path.isfile(filepath), 'File missing: ' + filepath)


class ConfigContextManagerTest(unittest.TestCase):

  def testWithInitialValue(self):
    gdal.SetConfigOption('NONSENSE', '1')
    with gdrivers_util.ConfigOption('NONSENSE', '2'):
      self.assertEqual(gdal.GetConfigOption('NONSENSE'), '2')
    self.assertEqual(gdal.GetConfigOption('NONSENSE'), '1')

  def testNoInitialValue(self):
    option = 'THIS DOES NOT EXIST'
    with gdrivers_util.ConfigOption(option, '3'):
      self.assertEqual(gdal.GetConfigOption(option), '3')
    self.assertIsNone(gdal.GetConfigOption(option))

  def testNoInitialValueWithDefault(self):
    option = 'ALSO DOES NOT EXIST'
    with gdrivers_util.ConfigOption(option, '4', '5'):
      self.assertEqual(gdal.GetConfigOption(option), '4')
    self.assertEqual(gdal.GetConfigOption(option), '5')


class DriverTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    drivers = gdrivers_util.drivers
    driver = 'aaigrid' if 'aaigrid' in drivers else drivers[0]
    super(DriverTest, self).setUp(driver, '.dummy.ext')

  def testCheckDriver(self):
    self.driver_name = 'junk'
    self.assertRaises(AssertionError, self.CheckDriver)

  def testCheckOpen(self):
    self.assertRaises(AssertionError, self.CheckOpen, '/missing')
    self.assertRaises(AssertionError, self.CheckOpen, 'missing_relative')
    if 'aaigrid' in gdrivers_util.drivers:
      filepath = gdrivers_util.GetTestFilePath('pixel_per_line.asc')
      self.CheckOpen(filepath)
      self.driver_name = 'junk driver'
      self.assertRaises(AssertionError, self.CheckOpen, filepath)

  def testCheckGeoTransform(self):
    class DummySrc(object):

      def GetGeoTransform(self):
        return tuple(range(6))

    self.src = DummySrc()
    self.assertRaises(AssertionError, self.CheckGeoTransform, (1, 3))
    self.assertRaises(AssertionError, self.CheckGeoTransform, [1, 3] * 7)

    gt = list(range(6))
    self.CheckGeoTransform(gt)
    gt[4] = 4.0000001
    self.CheckGeoTransform(gt)
    gt[4] = 4.2
    self.assertRaises(AssertionError, self.CheckGeoTransform, gt)

  def testCheckBand(self):
    band_num = 1
    band = mock.Mock()
    checksum = 1234
    band.Checksum = mock.MagicMock(return_value=checksum)
    data_type = gdal.GDT_Float64
    band.DataType = data_type
    nodata = 4321
    band.GetNoDataValue = mock.MagicMock(return_value=nodata)

    src = mock.Mock()
    src.GetRasterBand = mock.MagicMock(return_value=band)

    self.src = src
    self.CheckBand(band_num, checksum, data_type, nodata)

    self.assertRaises(AssertionError, self.CheckBand, band_num, checksum + 1,
                      data_type, nodata)
    self.assertRaises(AssertionError, self.CheckBand, band_num, checksum,
                      gdal.GDT_Byte, nodata)
    self.assertRaises(AssertionError, self.CheckBand, band_num, checksum,
                      data_type, nodata - 1)


if __name__ == '__main__':
  unittest.main()
