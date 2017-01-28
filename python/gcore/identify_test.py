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
# Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test driver identification methods.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/identify.py

TODO(schwehr): Create a pretty differencing tool for WKT.
"""

import os


from osgeo import gdal
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

EXT = '.tif'


class IdentTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(IdentTest, self).setUp(gdrivers_util.GTIFF_DRIVER, EXT)

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
  def testIdent1(self):
    filepath = gcore_util.GetTestFilePath('byte.tif')
    file_list = gdal.ReadDir(os.path.dirname(filepath))
    drv = gdal.IdentifyDriver(filepath, file_list)
    self.assertIsNotNone(drv)
    self.assertEqual(drv.GetDescription().lower(), gdrivers_util.GTIFF_DRIVER)

  @gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
  def testIdent1NoDirectoryList(self):
    filepath = gcore_util.GetTestFilePath('byte.tif')
    drv = gdal.IdentifyDriver(filepath)
    self.assertIsNotNone(drv)
    self.assertEqual(drv.GetDescription().lower(), gdrivers_util.GTIFF_DRIVER)

  def testIdent2WillNotRecogniseFile(self):
    filepath = gcore_util.GetTestFilePath('testserialization.asc.aux.xml')
    file_list = gdal.ReadDir(os.path.dirname(filepath))
    drv = gdal.IdentifyDriver(filepath, file_list)
    self.assertIsNone(drv)

  def testIdent3WillNotRecogniseDirectory(self):
    dirpath = '/vsimem/testIdent3'
    filename = 'testserialization.asc.aux.xml'
    filepath = os.path.join(dirpath, filename)
    self.assertEqual(0, gdal.Mkdir(dirpath, 0))
    dst = gdal.VSIFOpenL(filepath, 'wb+')
    test_str = 'foo'
    self.assertEqual(gdal.VSIFWriteL(test_str, 1, len(test_str), dst),
                     len(test_str))
    # Mkdir vsimem ignores the mode.
    drv = gdal.IdentifyDriver(dirpath, [filename])
    self.assertIsNone(drv)

  def testIdent3WillNotRecogniseDirectoryNoFileList(self):
    dirpath = '/vsimem/testIdent3NoFileList'
    filename = 'testserialization.asc.aux.xml'
    filepath = os.path.join(dirpath, filename)
    self.assertEqual(0, gdal.Mkdir(dirpath, 0))
    dst = gdal.VSIFOpenL(filepath, 'wb+')
    test_str = 'foo'
    self.assertEqual(gdal.VSIFWriteL(test_str, 1, len(test_str), dst),
                     len(test_str))
    # Mkdir vsimem ignores the mode.
    drv = gdal.IdentifyDriver(dirpath)
    self.assertIsNone(drv)


if __name__ == '__main__':
  unittest.main()
