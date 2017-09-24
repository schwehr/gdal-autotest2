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
# Copyright (c) 2008-2013, Even Rouault <even dot rouault at mines-paris.org>
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
"""Tests for the HDF5 driver.

Format is described here:

http://www.gdal.org/frmt_hdf5.html

Rewrite of hdf5.py:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gdrivers/hdf5.py
"""

import os

from osgeo import gdal
import unittest
from autotest2.gdrivers import gdrivers_util

DRIVER = 'hdf5'
EXT = '.h5'


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class Hdf5Test(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(Hdf5Test, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDriver(self):
    self.CheckDriver()

  def testWithSubdatasets(self):
    filename = 'groups.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(filepath)
    self.CheckShape(512, 512, 0)

    self.assertEqual(self.src.GetMetadataDomainList(), ['', 'SUBDATASETS'])
    expected = {
        'SUBDATASET_1_DESC': '[2x10] //MyGroup/Group_A/dset2 (32-bit integer)',
        'SUBDATASET_1_NAME': 'HDF5:"groups.h5"://MyGroup/Group_A/dset2',
        'SUBDATASET_2_DESC': '[3x3] //MyGroup/dset1 (32-bit integer)',
        'SUBDATASET_2_NAME': 'HDF5:"groups.h5"://MyGroup/dset1'
    }
    subdatasets = self.src.GetMetadata('SUBDATASETS')
    for k in expected:
      self.assertIn(k, subdatasets)
      if 'DESC' in k:
        self.assertEqual(subdatasets[k], expected[k])
      else:
        # The NAMES contain the path, so just check from // to the end.
        self.assertEqual(subdatasets[k].split(':')[-1],
                         expected[k].split(':')[-1])

  def testSubdataset1(self):
    filename = 'groups.h5'
    filepath = 'hdf5:' + self.getTestFilePath(filename) + '://MyGroup/dset1'
    self.CheckOpen(filepath, check_driver='hdf5image')
    self.CheckShape(3, 3, 1)
    self.CheckBand(1, 18, gdal.GDT_Int32, None, 1, 3)

  def testSubdataset2(self):
    filename = 'groups.h5'
    filepath = (
        'hdf5:' + self.getTestFilePath(filename) + '://MyGroup/Group_A/dset2')
    self.CheckOpen(filepath, check_driver='hdf5image')
    self.CheckShape(10, 2, 1)
    self.CheckBand(1, 110, gdal.GDT_Int32, None, 1, 10)

  def testArray(self):
    filename = 'u8be.h5'
    filepath = 'hdf5:' + self.getTestFilePath(filename) + '://TestArray'
    self.CheckOpen(filepath, check_driver='hdf5image')
    self.CheckShape(5, 6, 1)
    self.CheckBand(1, 135, gdal.GDT_Byte, None, 0, 9)

  def testMetadata(self):
    filename = 'metadata.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(filepath)
    metadata = self.src.GetMetadata()
    key = ('Group_with_spaces_and_underscores_Dataset_with_spaces_and_'
           'underscores_attribute_with_spaces_and_underscores')
    self.assertIn(key, metadata)
    self.assertEqual(metadata[key].strip(), '0.1')

    metadata_list = self.src.GetMetadata_List()
    self.assertEqual(len(metadata), len(metadata_list))
    list_keys = [item.split('=', 1)[0] for item in metadata_list]
    self.assertEqual(sorted(metadata.keys()), sorted(list_keys))

# TODO(schwehr): Convert the rest of the tests.
# TODO(schwehr): Generate tests with each cell type of grid for hdf5.

if __name__ == '__main__':
  unittest.main()
