#!/usr/bin/env python
# Copyright 2017 Google Inc. All Rights Reserved.
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
# Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2012, Even Rouault <even dot rouault at mines-paris.org>
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

"""Tests for the PNG driver.

Format is described here:

http://www.gdal.org/frmt_various.html

Rewrite of png.py:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/png.py
http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/png_profile.py
"""

import os

import unittest
from autotest2.gdrivers import gdrivers_util

DRIVER = gdrivers_util.PNG_DRIVER
EXT = '.png'


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestReadPng(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestReadPng, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDriver(self):
    self.CheckDriver()

  def testInfo(self):
    for base in ('test', 'byte_with_xmp', 'idat_broken', 'rgba16', 'tbbn2c16'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()

# TODO(schwehr): Rewrite all the remaining tests.


if __name__ == '__main__':
  unittest.main()
