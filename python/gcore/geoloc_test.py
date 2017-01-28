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

"""Test geolocation warper.

Rewrite of:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/geoloc.py
"""

import contextlib
import os


from osgeo import gdal
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util

EXT = '.vrt'


@contextlib.contextmanager
def PushDir(path):
  orig_path = os.getcwd()
  os.chdir(path)
  yield
  os.chdir(orig_path)


@gdrivers_util.SkipIfDriverMissing(gdrivers_util.VRT_DRIVER)
@gdrivers_util.SkipIfDriverMissing(gdrivers_util.GTIFF_DRIVER)
class GeolocTest(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(GeolocTest, self).setUp(gdrivers_util.VRT_DRIVER, EXT)

  def testGeoloc01WarpSst(self):
    filepath = gcore_util.GetTestFilePath('warpsst.vrt')
    with PushDir(os.path.dirname(filepath)):
      self.CheckOpen(filepath)
    self.CheckGeoTransform((-90.30271148, 0.15466423, 0, 33.87552642, 0,
                            -0.15466423))
    # TODO(schwehr): The changing checksum of the band with GDAL updates implies
    # that this test is brittle and needs to be reworked.
    self.CheckBand(1, 62319, gdal.GDT_Int16)


if __name__ == '__main__':
  unittest.main()
