#!/usr/bin/env python
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
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2010-2014, Even Rouault <even.rouault at mines-paris dot org>
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

"""Test VSI handling of zip files.

Rewrite of:

https://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/vsizip.py
"""
import os
import unittest


from osgeo import gdal
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util


class ZipTest(unittest.TestCase):

  # Test 1.
  def testWriteMultipleFiles(self):
    zip_actual = '/vsimem/test.zip'
    # Cannot use os.path.join with a leading '/' on zip_actual.
    zip_path = '/vsizip' + zip_actual

    subdir1_path = os.path.join(zip_path, 'subdir1/')
    subdir2_path = os.path.join(zip_path, 'subdir2')
    subdir3_path = os.path.join(zip_path, 'subdir3')

    file1_str = 'abcd'
    file2_str = 'efghi'
    file1_path = os.path.join(subdir3_path, file1_str)
    file2_path = os.path.join(subdir3_path, file2_str)
    file3_path = os.path.join(zip_path, 'that_wont_work')

    zip_file = gdal.VSIFOpenL(zip_path, 'wb')
    self.assertIsNotNone(zip_file)

    try:
      subdir = gdal.VSIFOpenL(subdir1_path, 'wb')
      self.assertIsNotNone(subdir)
      self.assertNotEqual(gdal.VSIFCloseL(subdir), -1)

      self.assertNotEqual(
          gdal.Mkdir(subdir2_path, 0), -1)

      # Create 1st file.
      file1 = gdal.VSIFOpenL(file1_path, 'wb')
      self.assertIsNotNone(file1)
      self.assertEqual(gdal.VSIFWriteL(file1_str, 1, len(file1_str), file1), 4)
      self.assertNotEqual(gdal.VSIFCloseL(file1), -1)

      # Test that we cannot read a zip file being written.
      gdal.ErrorReset()
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        file1 = gdal.VSIFOpenL(file1_path, 'rb')
      self.assertEqual(
          gdal.GetLastErrorMsg(), 'Cannot read a zip file being written')
      self.assertIsNone(file1)
      gdal.ErrorReset()

      # Create 2nd file.
      file2 = gdal.VSIFOpenL(file2_path, 'wb')
      self.assertIsNotNone(file2)
      self.assertEqual(
          gdal.VSIFWriteL(file2_str, 1, len(file2_str), file2), len(file2_str))
      # Do not close so that we fail to write another file.

      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        file3 = gdal.VSIFOpenL(file3_path, 'wb')
      self.assertEqual(gdal.GetLastErrorMsg(),
                       'Cannot create that_wont_work while another file is '
                       'being written in the .zip')
      self.assertIsNone(file3)
      gdal.ErrorReset()

      self.assertNotEqual(gdal.VSIFCloseL(file2), -1)
      self.assertNotEqual(gdal.VSIFCloseL(zip_file), -1)

      # Try to access the now closed zip file from vsimem.
      file1 = gdal.VSIFOpenL(file1_path, 'rb')
      self.assertIsNotNone(file1)
      self.assertEqual(gdal.VSIFReadL(1, 4, file1).decode('ASCII'), file1_str)
      self.assertNotEqual(gdal.VSIFCloseL(file1), -1)

    finally:
      self.assertEqual(gdal.Unlink(zip_actual), 0)

  # Test2.
  def testWriteAfterClosingAZipWithOneFile(self):
    zip_actual = '/vsimem/test2.zip'
    zip_path = '/vsizip' + zip_actual

    file1_path = os.path.join(zip_path, 'foo.bar')
    file2_path = os.path.join(zip_path, 'bar.baz')

    file1_str = '12345'
    file2_str = '67890'

    try:
      # Create a zip file with one component file and verify it appears.
      file1 = gdal.VSIFOpenL(file1_path, 'wb')
      self.assertIsNotNone(file1)
      self.assertEqual(gdal.VSIFWriteL(file1_str, 1, 5, file1), len(file1_str))
      self.assertNotEqual(gdal.VSIFCloseL(file1), -1)

      self.assertEqual(gdal.ReadDir(zip_path), ['foo.bar'])

      # Make sure it can add a second file after closing the first.
      file2 = gdal.VSIFOpenL(file2_path, 'wb')
      self.assertIsNotNone(file2)
      self.assertEqual(
          gdal.VSIFWriteL(file2_str, 1, len(file2_str), file2), len(file2_str))

      # It cannot read the directory listing while file 2 is open for write.
      gdal.ErrorReset()
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        self.assertIsNone(gdal.ReadDir(zip_path))
      self.assertEqual(
          gdal.GetLastErrorMsg(), 'Cannot read a zip file being written')

      self.assertNotEqual(gdal.VSIFCloseL(file2), -1)

      # Now that the 2nd file is closed, it can read the contents.
      self.assertEqual(
          gdal.ReadDir(zip_path), ['foo.bar', 'bar.baz'])

      # Verify that both files inside the zip contain the expected text.
      file1 = gdal.VSIFOpenL(file1_path, 'rb')
      self.assertIsNotNone(file1_path)
      self.assertEqual(
          gdal.VSIFReadL(1, len(file1_str), file1).decode('ASCII'), file1_str)
      self.assertNotEqual(gdal.VSIFCloseL(file1), -1)

      file2 = gdal.VSIFOpenL(file2_path, 'rb')
      self.assertIsNotNone(file2_path)
      self.assertEqual(
          gdal.VSIFReadL(1, len(file2_str), file2).decode('ASCII'), file2_str)
      self.assertNotEqual(gdal.VSIFCloseL(file2), -1)

    finally:
      self.assertEqual(gdal.Unlink(zip_actual), 0)

  # Test opening in write mode a file inside a zip archive whose
  # content has been listed before (testcase for fix of r22625).
  # Test3.
  def testWriteAfterList(self):
    zip_actual = '/vsimem/test3.zip'
    zip_path = '/vsizip' + zip_actual

    file_strs = ['foo', 'bar', 'baz']
    file_paths = [os.path.join(zip_path, filename) for filename in file_strs]

    try:
      zip_file = gdal.VSIFOpenL(zip_path, 'wb')
      self.assertIsNotNone(zip_file)
      for i in range(2):
        dst = gdal.VSIFOpenL(file_paths[i], 'wb')
        self.assertIsNotNone(dst)
        self.assertEqual(
            gdal.VSIFWriteL(file_strs[i], 1, len(file_strs[i]), dst),
            len(file_strs[i]))
        self.assertNotEqual(gdal.VSIFCloseL(dst), -1)
      self.assertNotEqual(gdal.VSIFCloseL(zip_file), -1)

      self.assertEqual(gdal.ReadDir(zip_path), file_strs[:2])

      dst = gdal.VSIFOpenL(file_paths[2], 'wb')
      self.assertIsNotNone(dst)
      self.assertEqual(
          gdal.VSIFWriteL(file_strs[2], 1, len(file_strs[2]), dst),
          len(file_strs[2]))
      self.assertNotEqual(gdal.VSIFCloseL(dst), -1)

      self.assertEqual(gdal.ReadDir(zip_path), file_strs)

    finally:
      self.assertEqual(gdal.Unlink(zip_actual), 0)

  # Test 4.
  def testReadRecursive(self):
    zip_path = '/vsizip/' + gcore_util.GetTestFilePath('testzip.zip')

    expected = ['subdir/', 'subdir/subdir/', 'subdir/subdir/uint16.tif',
                'subdir/subdir/test_rpc.txt', 'subdir/test_rpc.txt',
                'test_rpc.txt', 'uint16.tif']
    self.assertEqual(gdal.ReadDirRecursive(zip_path), expected)

  # Test 5.
  def testReadDeepRecursive(self):
    zip_actual = '/vsimem/bigdepthzip.zip'
    zip_path = '/vsizip' + zip_actual

    zip_file = gdal.VSIFOpenL(zip_path, 'wb')
    self.assertIsNotNone(zip_file)

    try:
      size = 1000
      # A long path with 1001 'a' components.
      name = 'a/' * size + 'a'
      full_name = os.path.join(zip_path, name)
      dst = gdal.VSIFOpenL(full_name, 'wb')
      self.assertIsNotNone(dst)
      self.assertNotEqual(gdal.VSIFCloseL(dst), -1)
      self.assertNotEqual(gdal.VSIFCloseL(zip_file), -1)

      tree = gdal.ReadDirRecursive(zip_path)
      self.assertEqual(len(tree), size + 1)
      self.assertEqual(tree[10], 'a/a/a/a/a/a/a/a/a/a/a/')
      self.assertEqual(tree[-1], name)

    finally:
      self.assertEqual(gdal.Unlink(zip_actual), 0)

  # Test 6.  #4785.
  def testWrite2FilesWithSameName(self):
    zip_actual = '/vsimem/test6.zip'
    zip_path = '/vsizip' + zip_actual

    zip_file = gdal.VSIFOpenL(zip_path, 'wb')
    self.assertIsNotNone(zip_file)

    try:
      file_str = '12345'
      file_path = os.path.join(zip_path, 'foo.bar')
      dst = gdal.VSIFOpenL(file_path, 'wb')
      self.assertIsNotNone(dst)
      self.assertEqual(
          gdal.VSIFWriteL(file_str, 1, len(file_str), dst),
          len(file_str))
      self.assertNotEqual(gdal.VSIFCloseL(dst), -1)

      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        dst = gdal.VSIFOpenL(file_path, 'wb')
      self.assertIsNone(dst)
      self.assertIn('foo.bar already exists', gdal.GetLastErrorMsg())
      gdal.ErrorReset()
      self.assertNotEqual(gdal.VSIFCloseL(zip_file), -1)
      self.assertEqual(gdal.GetLastErrorMsg(), '')

    finally:
      self.assertEqual(gdal.Unlink(zip_actual), 0)

    # Close each time.
    dst = gdal.VSIFOpenL(file_path, 'wb')
    self.assertIsNotNone(dst)
    try:
      self.assertEqual(
          gdal.VSIFWriteL(file_str, 1, len(file_str), dst),
          len(file_str))
      self.assertNotEqual(gdal.VSIFCloseL(dst), -1)

      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        dst = gdal.VSIFOpenL(file_path, 'wb')
      self.assertIsNone(dst)
      self.assertIn('foo.bar already exists', gdal.GetLastErrorMsg())

    finally:
      self.assertEqual(gdal.Unlink(zip_actual), 0)

  # Test 7.  #5361
  def testUtf8Filenames(self):
    zip_path = '/vsizip/' + gcore_util.GetTestFilePath('cp866_plus_utf8.zip')

    self.assertEqual(gdal.ReadDir(zip_path),
                     [u'\u0430\u0431\u0432\u0433\u0434\u0435',
                      u'\u0436\u0437\u0438\u0439\u043a\u043b'])

  # Test 8.
  def testZip64(self):
    # Really needs the two vsizip modifiers.
    zip_file = os.path.join(
        '/vsizip/vsizip/' + gcore_util.GetTestFilePath('zero.bin.zip.zip'),
        'zero.bin.zip')
    self.assertEqual(gdal.VSIStatL(zip_file).size, 5000 * 1000 * 1000 + 1)

  # Test 9.
  @unittest.skip('Fails if on a Content-Addressed Storage (CAS) filesystem.')
  def testZip64SparseFile(self):
    # Uses zero_stored.bin.zip.{start,end} for the contents of the large zip
    # in sparse mode.
    # TODO(schwehr): The main file should be renamed to end in .xml or .vrt.
    # TODO(schwehr): May need to copy all three files to a temp dir for
    #   Content Addressed Filesystems (CAS) filesystems that do not support
    #   relative links.
    zip_path = (
        '/vsizip/vsisparse/' +
        gcore_util.GetTestFilePath('zero_stored.bin.xml.zip'))

    file1_path = os.path.join(zip_path, 'zero.bin')
    file2_path = os.path.join(zip_path, 'hello.txt')
    self.assertEqual(gdal.ReadDir(zip_path), ['zero.bin', 'hello.txt'])
    self.assertEqual(gdal.VSIStatL(file1_path).size, 5000 * 1000 * 1000 + 1)
    self.assertEqual(gdal.VSIStatL(file2_path).size, 6)

    src = gdal.VSIFOpenL(file1_path, 'rb')
    self.assertIsNotNone(src)
    self.assertIsNotNone(src)
    gdal.VSIFSeekL(src, 5000 * 1000 * 1000, 0)
    self.assertEqual(gdal.VSIFReadL(1, 1, src).decode('ascii'), '\x03')
    self.assertNotEqual(gdal.VSIFCloseL(src), -1)

    src = gdal.VSIFOpenL(file2_path, 'rb')
    self.assertIsNotNone(src)
    self.assertEqual(gdal.VSIFReadL(1, 6, src).decode('ascii'), 'HELLO\n')
    self.assertNotEqual(gdal.VSIFCloseL(src), -1)

  # Test 10. #5361
  @unittest.skip('TODO(schwehr): Why does this not work?')
  def testRecodeFilenames(self):
    zip_path = '/vsizip/' + gcore_util.GetTestFilePath('cp866.zip')
    with gdrivers_util.ConfigOption('CPL_ZIP_ENCODING', 'CP866'):
      self.assertEqual(
          gdal.ReadDir(zip_path),
          [u'\u0430\u0431\u0432\u0433\u0434\u0435',
           u'\u0436\u0437\u0438\u0439\u043a\u043b'])

  # Test 11. #5361
  def testDoNothingWithUtf8Names(self):
    zip_path = '/vsizip/' + gcore_util.GetTestFilePath('utf8.zip')
    self.assertEqual(
        gdal.ReadDir(zip_path),
        [u'\u0430\u0431\u0432\u0433\u0434\u0435',
         u'\u0436\u0437\u0438\u0439\u043a\u043b'])


if __name__ == '__main__':
  unittest.main()
