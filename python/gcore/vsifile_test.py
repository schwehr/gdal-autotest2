#!/usr/bin/env python
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
# Copyright (c) 2011-2013, Even Rouault <even dot rouault at mines-paris . org>
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

"""Test VSI file primitives.

Rewrite of:

https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/vsifile.py
"""

import os
import unittest

from osgeo import gdal
import gflags as flags
import unittest

FLAGS = flags.FLAGS


class VsiFileTest(unittest.TestCase):

  def GenericVerify(self, filename):
    """Write to a destination and try the basic VSI operations.

    Most VSI filesystem drivers support these basic IO operations:
    close, open, read, seek, stat, tell, truncate, unlink and write.

    This starts by by exercising the write side and then closes the file.
    It then reads the file and verifies that the write operations give
    the expected results.

    Args:
      filename: Where to write the test file.
    """
    dst = gdal.VSIFOpenL(filename, 'wb+')
    self.assertIsNotNone(dst)
    test_str = '0123456789'
    self.assertEqual(gdal.VSIFWriteL(test_str, 1, len(test_str), dst),
                     len(test_str))
    self.assertEqual(gdal.VSIFTruncateL(dst, 5), 0)
    self.assertEqual(gdal.VSIFTellL(dst), 10)
    self.assertEqual(gdal.VSIFSeekL(dst, 0, 2), 0)
    self.assertEqual(gdal.VSIFTellL(dst), 5)
    self.assertEqual(gdal.VSIFWriteL('XX', 1, 2, dst), 2)
    self.assertNotEqual(gdal.VSIFCloseL(dst), -1)

    stat = gdal.VSIStatL(
        filename,
        gdal.VSI_STAT_EXISTS_FLAG | gdal.VSI_STAT_NATURE_FLAG |
        gdal.VSI_STAT_SIZE_FLAG)
    self.assertEqual(stat.size, 7)

    src = gdal.VSIFOpenL(filename, 'rb')
    self.assertEqual(gdal.VSIFReadL(1, 7, src).decode('ascii'), '01234XX')

    # Cannot write to read mode file.
    self.assertEqual(gdal.VSIFWriteL('a', 1, 1, src), 0)

    self.assertEqual(gdal.Unlink(filename), 0)
    self.assertIsNone(gdal.VSIStatL(filename, gdal.VSI_STAT_EXISTS_FLAG))

  def testVsimem(self):
    self.GenericVerify('/vsimem/vsifile_1.bin')

  def testRealFile(self):
    with does_not_exist.TemporaryDirectory(base_path=somewhere) as temp_dir:
      self.GenericVerify(os.path.join(temp_dir, 'vsifile_2.bin'))

  # TODO(schwehr): Implement the equivalent of test vsifile_4 to vsifile_8.

  def testReadDir09(self):
    dirpath = '/vsimem/vsidir_9'

    # ReadDir will return None until there is something in the directory.
    self.assertIsNone(gdal.ReadDir(dirpath))
    gdal.Mkdir(dirpath, 123)  # vsimem ignores the mode.
    self.assertIsNone(gdal.ReadDir(dirpath))

    subdir = os.path.join(dirpath, 'subdir')
    gdal.Mkdir(subdir, 123)  # vsimem ignores the mode.
    self.assertEqual(gdal.ReadDir(dirpath), ['subdir'])

    filename = os.path.join(dirpath, 'file')
    dst = gdal.VSIFOpenL(filename, 'wb+')
    test_str = '0123456789'
    self.assertEqual(gdal.VSIFWriteL(test_str, 1, len(test_str), dst),
                     len(test_str))
    self.assertNotEqual(gdal.VSIFCloseL(dst), -1)
    self.assertEqual(
        sorted(gdal.ReadDir(dirpath)),
        [os.path.basename(filename), 'subdir'])

    filename2 = os.path.join(dirpath, 'file2')
    dst = gdal.VSIFOpenL(filename2, 'wb+')
    test_str = 'abc'
    self.assertEqual(gdal.VSIFWriteL(test_str, 1, len(test_str), dst),
                     len(test_str))
    self.assertNotEqual(gdal.VSIFCloseL(dst), -1)
    self.assertEqual(
        sorted(gdal.ReadDir(dirpath)),
        [os.path.basename(filename),
         os.path.basename(filename2),
         'subdir'])

    gdal.Unlink(filename)
    self.assertEqual(
        sorted(gdal.ReadDir(dirpath)),
        [os.path.basename(filename2), 'subdir'])
    gdal.Unlink(filename2)
    gdal.Unlink(subdir)
    self.assertIsNone(gdal.ReadDir(dirpath))

  # TODO(schwehr): Implement the equivalent of test vsifile_10.


if __name__ == '__main__':
  unittest.main()
