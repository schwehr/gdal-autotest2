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
# Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

"""Test OGR handling of CSV files.

This is a partial rewrite of the ogr_csv.py test file from:
  http://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_csv.py.

Individual test cases' docs indicate which test cases are rewrites
from upstream.
"""


from osgeo import gdal
from osgeo import ogr
import unittest
from autotest2.ogr import ogr_util

EXT = '.csv'


def setUpModule():
  ogr_util.SetupTestEnv()


@ogr_util.SkipIfDriverMissing(ogr_util.CSV_DRIVER)
class OgrCsvTest(ogr_util.DriverTestCase):

  def setUp(self):
    super(OgrCsvTest, self).setUp(ogr_util.CSV_DRIVER, EXT)

  def testCsv00BasicDriverTests(self):
    filepath = ogr_util.GetTestFilePath('prime_meridian.csv')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer_by_index = src.GetLayerByIndex(0)
    layer_by_name = src.GetLayerByName('prime_meridian')
    self.assertEqual(layer_by_index.GetName(), 'prime_meridian')
    self.assertEqual(layer_by_name.GetName(), 'prime_meridian')

  def testCsv02CheckLayer(self):
    filepath = ogr_util.GetTestFilePath('prime_meridian.csv')
    self.CheckOpen(filepath)
    layer = self.src.GetLayerByName('prime_meridian')
    self.assertTrue(layer)

    # Check definition.
    layer_defn = layer.GetLayerDefn()
    self.assertEqual(layer_defn.GetName(), 'prime_meridian')
    num_fields = layer_defn.GetFieldCount()
    self.assertEqual(num_fields, 10)

    field_names = [layer_defn.GetFieldDefn(i).GetName()
                   for i in range(num_fields)]
    expected_names = [
        'PRIME_MERIDIAN_CODE',
        'PRIME_MERIDIAN_NAME',
        'GREENWICH_LONGITUDE',
        'UOM_CODE',
        'REMARKS',
        'INFORMATION_SOURCE',
        'DATA_SOURCE',
        'REVISION_DATE',
        'CHANGE_ID',
        'DEPRECATED'
        ]
    self.assertItemsEqual(field_names, expected_names)

    field_index = layer.GetLayerDefn().GetFieldIndex('PRIME_MERIDIAN_CODE')
    values = [layer.GetNextFeature().GetField(field_index)
              for _ in range(layer.GetFeatureCount())]
    expected = ['8901', '8902', '8903', '8904']
    self.assertEqual(values, expected)

  def testCsv21QuotedHeaders(self):
    """Test that headers in double quotes are read correctly."""
    filename = ogr_util.GetTestFilePath('testquoteheader1.csv')
    csv_ds = self.CheckOpen(filename)

    lyr = csv_ds.GetLayerByName('testquoteheader1')
    self.assertIsNotNone(lyr)
    lyr.ResetReading()

    expect = ['test', '2000', '2000.12']
    result = [lyr.GetLayerDefn().GetFieldDefn(i).GetNameRef() for i in range(3)]
    self.assertEqual(result, expect)

    csv_ds.Destroy()
    csv_ds = None

    filename = ogr_util.GetTestFilePath('testquoteheader2.csv')
    csv_ds = ogr.Open(filename)
    self.assertIsNotNone(csv_ds)

    lyr = csv_ds.GetLayerByName('testquoteheader2')
    self.assertIsNotNone(lyr)
    lyr.ResetReading()

    expect = ['field_1', 'field_2', 'field_3']
    result = [lyr.GetLayerDefn().GetFieldDefn(i).GetNameRef() for i in range(3)]
    self.assertEqual(result, expect)

  def testSingleColumnCsv(self):
    """Test that a csv file with a single column can be written and read from.

    This is largely copied from ogr_csv_24.
    """
    # Create an invalid CSV file.
    f = gdal.VSIFOpenL('/vsimem/invalid.csv', 'wb')
    gdal.VSIFCloseL(f)

    # A vsimem file should not prevent creating a new CSV file.
    # http://trac.osgeo.org/gdal/ticket/4824
    ds = ogr.GetDriverByName(
        ogr_util.CSV_DRIVER).CreateDataSource('/vsimem/single.csv')
    lyr = ds.CreateLayer('single')
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    feat = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(feat)
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField(0, 'bar')
    lyr.CreateFeature(feat)
    feat = None
    lyr = None

    ds = self.CheckOpen('/vsimem/single.csv')
    lyr = ds.GetLayer(0)
    self.assertEqual(1, lyr.GetLayerDefn().GetFieldCount())
    feat = lyr.GetNextFeature()
    self.assertEqual('', feat.GetField(0))
    feat = lyr.GetNextFeature()
    self.assertEqual('bar', feat.GetField(0))

    gdal.Unlink('/vsimem/single.csv')
    gdal.Unlink('/vsimem/invalid.csv')

  def testBackslashQuoteInCsv(self):
    """Make sure backslashes are not treated specially, in or out of quotes."""

    filename = ogr_util.GetTestFilePath('backslash_quote.csv')
    csv_ds = self.CheckOpen(filename)

    # Make sure column names are exactly as expected.
    lyr = csv_ds.GetLayerByName('backslash_quote')
    self.assertIsNotNone(lyr)
    lyr.ResetReading()
    expect = ['Description', 'Attr0', 'Lon', 'Lat']
    num_attrs = lyr.GetLayerDefn().GetFieldCount()
    result = [lyr.GetLayerDefn().GetFieldDefn(attr_index).GetNameRef()
              for attr_index in range(num_attrs)]
    self.assertEqual(result, expect)

    # Make sure table contents are exactly as expected.
    # pylint: disable=bad-whitespace
    expect = [
        ['three things',                      'aaa',     'bbb',     'ccc'],
        ['three more things',                 'ddd',     'eee',     'fff'],
        ['backslashes outside quotes',        '\\ggg',   'hhh\\',   'i\\ii'],
        ['double backslashes outside quotes', '\\\\jjj', 'kkk\\\\', 'l\\\\ll'],
        ['backslashes inside quotes',         '\\mmm',   'nnn\\',   'o\\oo'],
        ['double backslashes inside quotes',  '\\\\ppp', 'qqq\\\\', 'r\\\\rr'],
        ['yet another three things',          'sss',     'ttt',     'uuu'],
    ]
    num_features = lyr.GetFeatureCount()
    result = []
    # TODO(schwehr): Switch to a feature iterator / generator.
    # pylint: disable=unused-variable
    for feature_num in range(num_features):
      feat = lyr.GetNextFeature()
      result.append([feat.GetField(attr_index)
                     for attr_index in range(num_attrs)])
    self.assertEqual(result, expect)


if __name__ == '__main__':
  unittest.main()
