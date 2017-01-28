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
# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test ColorTable.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/colortable.py
"""
import unittest


from osgeo import gdal
import unittest


class ColorTableTest(unittest.TestCase):

  def testSingle4(self):
    ct = gdal.ColorTable()
    entry = (0, 1, 2, 3)
    ct.SetColorEntry(0, entry)
    self.assertEqual(ct.GetColorEntry(0), entry)
    self.assertEqual(ct.GetPaletteInterpretation(), 1)

  def testSingle3(self):
    ct = gdal.ColorTable()
    entry = [0, 1, 2]
    ct.SetColorEntry(0, tuple(entry))
    entry.append(255)
    self.assertEqual(ct.GetColorEntry(0), tuple(entry))

  def testBasic(self):
    table = [
        (255, 0, 0),
        (0, 255, 0),
        (0, 0, 255),
        (255, 255, 255, 0)
        ]

    ct = gdal.ColorTable()
    for i in range(len(table)):
      ct.SetColorEntry(i, table[i])

    self.assertEqual(ct.GetCount(), 4)

    for entry_num in range(len(table)):
      src = table[entry_num]
      dst = ct.GetColorEntry(entry_num)
      for i in range(len(src)):
        self.assertEqual(dst[i], src[i])
      if len(src) < 4:
        self.assertEqual(dst[3], 255)

  def testRamp(self):
    ct = gdal.ColorTable()
    ct.CreateColorRamp(0, (255, 0, 0),
                       255, (0, 0, 255))
    self.assertEqual(ct.GetCount(), 256)
    self.assertEqual(ct.GetColorEntry(0), (255, 0, 0, 255))
    self.assertEqual(ct.GetColorEntry(255), (0, 0, 255, 255))
    self.assertEqual(ct.GetColorEntry(100), (155, 0, 100, 255))


if __name__ == '__main__':
  unittest.main()
