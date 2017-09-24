# -*- coding: utf-8 -*-
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
###############################################################################
# Copyright (c) 2014, Even Rouault <even dot rouault at mines-paris dot org>
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

"""Test OGR handling of FileGDB files by OpenFileGDB driver.

This is a rewrite of:

  http://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_openfilegdb.py.

Tests are numbered to match the autotest test when there is a match.
"""


from osgeo import gdal
from osgeo import ogr
from osgeo import osr
import unittest
from autotest2.ogr import ogr_util


DRIVER = ogr_util.OPENFILEGDB_DRIVER
EXT = '.gdb.zip'

OPENFILEGDB_DATALIST = [['none', ogr.wkbNone, None], [
    'point', ogr.wkbPoint, 'POINT (1 2)'
], ['multipoint', ogr.wkbMultiPoint, 'MULTIPOINT (1 2,3 4)'], [
    'linestring', ogr.wkbLineString, 'LINESTRING (1 2,3 4)',
    'MULTILINESTRING ((1 2,3 4))'
], ['multilinestring', ogr.wkbMultiLineString, 'MULTILINESTRING ((1 2,3 4))'], [
    'polygon', ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
    'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))'
], [
    'multipolygon', ogr.wkbMultiPolygon, 'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0),'
    '(0.25 0.25,0.75 0.25,0.75 0.75,0.25 0.75,0.25 0.25)),'
    '((2 0,2 1,3 1,3 0,2 0)))'
], ['point25D', ogr.wkbPoint25D, 'POINT (1 2 3)'], [
    'multipoint25D', ogr.wkbMultiPoint25D, 'MULTIPOINT (1 2 -10,3 4 -20)'
], [
    'linestring25D', ogr.wkbLineString25D, 'LINESTRING (1 2 -10,3 4 -20)',
    'MULTILINESTRING ((1 2 -10,3 4 -20))'
], [
    'multilinestring25D', ogr.wkbMultiLineString25D,
    'MULTILINESTRING ((1 2 -10,3 4 -20))'
], [
    'polygon25D', ogr.wkbPolygon25D,
    'POLYGON ((0 0 -10,0 1 -10,1 1 -10,1 0 -10,0 0 -10))',
    'MULTIPOLYGON (((0 0 -10,0 1 -10,1 1 -10,1 0 -10,0 0 -10)))'
], [
    'multipolygon25D', ogr.wkbMultiPolygon25D,
    'MULTIPOLYGON (((0 0 -10,0 1 -10,1 1 -10,1 0 -10,0 0 -10)))'
], [
    'multipatch', ogr.wkbGeometryCollection25D, 'GEOMETRYCOLLECTION '
    '(TIN (((0 0 0,0 1 0,1 0 0,0 0 0)),((0 1 0,1 0 0,1 1 0,0 1 0))),'
    'TIN (((10 0 0,10 1 0,11 0 0,10 0 0)),((10 0 0,11 0 0,10 -1 0,10 0 0))),'
    'TIN (((5 0 0,5 1 0,6 0 0,5 0 0))),'
    'MULTIPOLYGON (((100 0 0,100 1 0,101 1 0,101 0 0,100 0 0),'
    '(100.25 0.25 0,100.75 0.25 0,100.75 0.75 0,100.75 0.25 0,'
    '100.25 0.25 0))))'
], ['null_polygon', ogr.wkbPolygon, None], [
    'empty_polygon', ogr.wkbPolygon, 'POLYGON EMPTY', None
], ['empty_multipoint', ogr.wkbMultiPoint, 'MULTIPOINT EMPTY', None]]


def setUpModule():
  ogr_util.SetupTestEnv()


@ogr_util.SkipIfDriverMissing(DRIVER)
class OgrOpenFileGDBTest(ogr_util.DriverTestCase):

  def setUp(self):
    super(OgrOpenFileGDBTest, self).setUp(DRIVER, EXT)

  def testReadPoint(self):
    filepath = ogr_util.GetTestFilePath('testopenfilegdb.gdb.zip')
    self.CheckOpen(filepath)
    self.assertEqual(self.src.GetLayerCount(), 22)
    layer = self.src.GetLayerByName('point')
    self.assertEqual(layer.GetName(), 'point')
    self.assertEqual(layer.GetFeatureCount(), 5)
    self.assertEqual(layer.GetExtent(), (1.0, 1.0, 2.0, 2.0))

    layer_defn = layer.GetLayerDefn()
    self.assertEqual(layer_defn.GetFieldCount(), 12)
    self.assertEqual(layer_defn.GetGeomType(), ogr.wkbPoint)

    field_defn = layer_defn.GetFieldDefn(0)
    self.assertEqual(field_defn.GetName(), 'id')
    self.assertEqual(field_defn.GetTypeName(), 'Integer')

    field_defn = layer_defn.GetFieldDefn(7)
    self.assertEqual(field_defn.GetName(), 'guid')
    self.assertEqual(field_defn.GetTypeName(), 'String')

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField(0), 1)
    self.assertEqual(feature.GetFieldCount(), 12)
    self.assertEqual(feature.GetGeomFieldCount(), 1)
    self.assertEqual(feature.GetGeometryRef().ExportToWkt(), 'POINT (1 2)')

    geometry_ref = feature.GetGeometryRef()
    self.assertEqual(geometry_ref.GetPoint(), (1.0, 2.0, 0.0))
    self.assertEqual(geometry_ref.GetPoint_2D(), (1.0, 2.0))

  def test01Basic(self, filename='testopenfilegdb.gdb.zip', version10=True):
    filepath = ogr_util.GetTestFilePath(filename)
    self.CheckOpen(filepath)

    for data in OPENFILEGDB_DATALIST:
      layer = self.src.GetLayerByName(data[0])
      expected_geom_type = data[1]
      srs = osr.SpatialReference()
      srs.SetFromUserInput('WGS84')

      if expected_geom_type == ogr.wkbLineString:
        expected_geom_type = ogr.wkbMultiLineString
      elif expected_geom_type == ogr.wkbLineString25D:
        expected_geom_type = ogr.wkbMultiLineString25D
      elif expected_geom_type == ogr.wkbPolygon:
        expected_geom_type = ogr.wkbMultiPolygon
      elif expected_geom_type == ogr.wkbPolygon25D:
        expected_geom_type = ogr.wkbMultiPolygon25D

      self.assertEqual(expected_geom_type, layer.GetGeomType())
      geom_field_defn = layer.GetLayerDefn().GetGeomFieldDefn(0)
      if expected_geom_type != ogr.wkbNone:
        self.assertEqual(expected_geom_type, geom_field_defn.type)
      self.assertEqual(
          0,
          layer.GetLayerDefn().GetFieldDefn(
              layer.GetLayerDefn().GetFieldIndex('str')).GetWidth())
      field_index = layer.GetLayerDefn().GetFieldIndex('smallint')
      self.assertEqual(
          ogr.OFTInteger,
          layer.GetLayerDefn().GetFieldDefn(
              layer.GetLayerDefn().GetFieldIndex('smallint')).GetType())
      self.assertEqual(
          ogr.OFTReal,
          layer.GetLayerDefn().GetFieldDefn(
              layer.GetLayerDefn().GetFieldIndex('float')).GetType())
      if data[1] != ogr.wkbNone:
        self.assertEqual(layer.GetSpatialRef().IsSame(srs), 1)

      feat = layer.GetNextFeature()
      if data[1] != ogr.wkbNone:
        try:
          expected_wkt = data[3]
        except:
          expected_wkt = data[2]
        geom = feat.GetGeometryRef()
        if geom:
          geom = geom.ExportToWkt()
        if geom != expected_wkt:
          self.CheckFeatureGeometry(feat, expected_wkt)

      self.assertEqual(1, feat.GetField('id'))
      self.assertEqual(-13, feat.GetField('smallint'))
      self.assertEqual(123, feat.GetField('int'))
      self.assertEqual(1.5, feat.GetField('float'))
      self.assertEqual(4.56, feat.GetField('real'))
      self.assertEqual('2013/12/26 12:34:56', feat.GetField('adate'))
      self.assertEqual('{12345678-9ABC-DEF0-1234-567890ABCDEF}',
                       feat.GetField('guid'))
      if version10:
        self.assertEqual('<foo></foo>', feat.GetField('xml'))
      self.assertEqual('00FF7F', feat.GetField('binary'))
      self.assertEqual('123456', feat.GetField('binary2'))

      if version10:
        sql_layer = self.src.ExecuteSQL(
            'GetLayerDefinition %s' % layer.GetName())
        self.assertIsNotNone(sql_layer)
        feat = sql_layer.GetNextFeature()
        self.assertIsNotNone(feat)
        feat = sql_layer.GetNextFeature()
        self.assertIsNone(feat)
        layer.ResetReading()
        layer.TestCapability('foo')
        self.src.ReleaseResultSet(sql_layer)

        sql_layer = self.src.ExecuteSQL('GetLayerMetadata %s' % layer.GetName())
        self.assertIsNotNone(sql_layer)
        feat = sql_layer.GetNextFeature()
        self.assertIsNotNone(feat)
        self.src.ReleaseResultSet(sql_layer)

    if version10:
      sql_layer = self.src.ExecuteSQL('GetLayerDefinition foo')
      self.assertIsNone(sql_layer)

      sql_layer = self.src.ExecuteSQL('GetLayerMetadata foo')
      self.assertIsNone(sql_layer)

  # Skipped test 1_92
  # Skipped test 1_93
  # Skipped test 2_93
  # Skipped test 3_93

  def test03OpenTable(self):
    filepath = ogr_util.GetTestFilePath('testopenfilegdb.gdb.zip')
    self.CheckOpen('/vsizip/' + filepath +
                   '/testopenfilegdb.gdb/a00000009.gdbtable')
    self.assertEqual(1, self.src.GetLayerCount())
    layer = self.src.GetLayer(0)
    self.assertEqual('none', layer.GetName())

    # Try opening a system table
    layer = self.src.GetLayerByName('GDB_SystemCatalog')
    self.assertEqual('GDB_SystemCatalog', layer.GetName())
    feat = layer.GetNextFeature()
    self.assertEqual('GDB_SystemCatalog', feat.GetField('Name'))
    layer = self.src.GetLayerByName('GDB_SystemCatalog')
    self.assertEqual('GDB_SystemCatalog', layer.GetName())

  def test04AttributeIndexes(self):
    filepath = ogr_util.GetTestFilePath(
        'testopenfilegdb.gdb.zip/testopenfilegdb.gdb')
    self.CheckOpen('/vsizip/' + filepath)

    layer = self.src.GetLayerByName('point')
    tests = [
        ('id = 1', [1]),
        ('1 = id', [1]),
        ('id = 5', [5]),
        ('id = 0', []),
        ('id = 6', []),
        ('id <= 1', [1]),
        ('1 >= id', [1]),
        ('id >= 5', [5]),
        ('5 <= id', [5]),
        ('id < 1', []),
        ('1 > id', []),
        ('id >= 1', [1, 2, 3, 4, 5]),
        ('id > 0', [1, 2, 3, 4, 5]),
        ('0 < id', [1, 2, 3, 4, 5]),
        ('id <= 5', [1, 2, 3, 4, 5]),
        ('id < 6', [1, 2, 3, 4, 5]),
        ('id <> 0', [1, 2, 3, 4, 5]),
        ('id IS NOT NULL', [1, 2, 3, 4, 5]),
        ('id IS NULL', []),
        ('nullint IS NOT NULL', []),
        ('nullint IS NULL', [1, 2, 3, 4, 5]),
        ("str = 'foo_e'", []),
        ("str = 'foo_é'", [1, 2, 3, 4, 5]),
        ("str <= 'foo_é'", [1, 2, 3, 4, 5]),
        ("str >= 'foo_é'", [1, 2, 3, 4, 5]),
        ("str <> 'foo_é'", []),
        ("str < 'foo_é'", []),
        ("str > 'foo_é'", []),
        ('smallint = -13', [1, 2, 3, 4, 5]),
        ('smallint <= -13', [1, 2, 3, 4, 5]),
        ('smallint >= -13', [1, 2, 3, 4, 5]),
        ('smallint < -13', []),
        ('smallint > -13', []),
        ('int = 123', [1, 2, 3, 4, 5]),
        ('int <= 123', [1, 2, 3, 4, 5]),
        ('int >= 123', [1, 2, 3, 4, 5]),
        ('int < 123', []),
        ('int > 123', []),
        ('float = 1.5', [1, 2, 3, 4, 5]),
        ('float <= 1.5', [1, 2, 3, 4, 5]),
        ('float >= 1.5', [1, 2, 3, 4, 5]),
        ('float < 1.5', []),
        ('float > 1.5', []),
        ('real = 4.56', [1, 2, 3, 4, 5]),
        ('real <= 4.56', [1, 2, 3, 4, 5]),
        ('real >= 4.56', [1, 2, 3, 4, 5]),
        ('real < 4.56', []),
        ('real > 4.56', []),
        ("adate = '2013/12/26 12:34:56'", [1, 2, 3, 4, 5]),
        ("adate <= '2013/12/26 12:34:56'", [1, 2, 3, 4, 5]),
        ("adate >= '2013/12/26 12:34:56'", [1, 2, 3, 4, 5]),
        ("adate < '2013/12/26 12:34:56'", []),
        ("adate > '2013/12/26 12:34:56'", []),
        ("guid = '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", [1, 2, 3, 4, 5]),
        ("guid <= '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", [1, 2, 3, 4, 5]),
        ("guid >= '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", [1, 2, 3, 4, 5]),
        ("guid < '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", []),
        ("guid > '{12345678-9ABC-DEF0-1234-567890ABCDEF}'", []),
        ("guid = '{'", []),
        ("guid > '{'", [1, 2, 3, 4, 5]),
        ('NOT(id = 1)', [2, 3, 4, 5]),
        ('id = 1 OR id = -1', [1]),
        ('id = -1 OR id = 1', [1]),
        ('id = 1 OR id = 1', [1]),

        # The below 5 cases have OR condition that will have different
        # result sets. For example, a feature can match the left or right
        # side or none, but not both. This can be used as an optimization
        # when combining results sets to save detecting duplicated
        # records.

        # Begin exclusive branches.
        ('id = 1 OR id = 2', [1, 2]),
        ('id < 3 OR id > 3', [1, 2, 4, 5]),
        ('id > 3 OR id < 3', [1, 2, 4, 5]),
        ('id <= 3 OR id >= 4', [1, 2, 3, 4, 5]),
        ('id >= 4 OR id <= 3', [1, 2, 3, 4, 5]),
        # End exclusive branches.
        ('id < 3 OR id >= 3', [1, 2, 3, 4, 5]),
        ('id <= 3 OR id >= 3', [1, 2, 3, 4, 5]),
        ('id <= 5 OR id >= 1', [1, 2, 3, 4, 5]),
        ('id <= 1.5 OR id >= 2', [1, 2, 3, 4, 5]),
        ('id IS NULL OR id IS NOT NULL', [1, 2, 3, 4, 5]),
        ('float < 1.5 OR float > 1.5', []),
        ('float <= 1.5 OR float >= 1.5', [1, 2, 3, 4, 5]),
        ('float < 1.5 OR float > 2', []),
        ('float < 1 OR float > 2.5', []),
        ("str < 'foo_é' OR str > 'z'", []),
        ("adate < '2013/12/26 12:34:56' OR adate > '2014/01/01'", []),
        ('id = 1 AND id = -1', []),
        ('id = -1 AND id = 1', []),
        ('id = 1 AND id = 1', [1]),
        ('id = 1 AND id = 2', []),
        ('id <= 5 AND id >= 1', [1, 2, 3, 4, 5]),
        ('id <= 3 AND id >= 3', [3]),
        ('id = 1 AND float = 1.5', [1]),
        ('id BETWEEN 1 AND 5', [1, 2, 3, 4, 5]),
        ('id IN (1)', [1]),
        ('id IN (5,4,3,2,1)', [1, 2, 3, 4, 5]),

        # The below 12 cases cannot use the field indices of the filegdb.
        # The 3rd field of the tuple is the number of the indices used.

        # Begin no index used.
        ('fid = 1', [1], 0),
        ('fid BETWEEN 1 AND 1', [1], 0),
        ('fid IN (1)', [1], 0),
        ('fid IS NULL', [], 0),
        ('fid IS NOT NULL', [1, 2, 3, 4, 5], 0),
        ("xml <> ''", [1, 2, 3, 4, 5], 0),
        ("id = 1 AND xml <> ''", [1], 1),
        ("xml <> '' AND id = 1", [1], 1),
        ("NOT(id = 1 AND xml <> '')", [2, 3, 4, 5], 0),
        ("id = 1 OR xml <> ''", [1, 2, 3, 4, 5], 0),
        ('id = id', [1, 2, 3, 4, 5], 0),
        ('id = 1 + 0', [1], 0),  # May eventually use an index.
        # End no index used.
    ]
    for test in tests:
      if len(test) == 2:
        where_clause, fids = test
        expected_attr_index_use = 2
      else:
        where_clause, fids, expected_attr_index_use = test

      layer.SetAttributeFilter(where_clause)
      sql_layer = self.src.ExecuteSQL(
          'GetLayerAttrIndexUse %s' % layer.GetName())
      attr_index_use = int(sql_layer.GetNextFeature().GetField(0))
      self.src.ReleaseResultSet(sql_layer)
      self.assertEqual(expected_attr_index_use, attr_index_use)
      self.assertEqual(len(fids), layer.GetFeatureCount())
      for fid in fids:
        feat = layer.GetNextFeature()
        self.assertEqual(fid, feat.GetFID())
      feat = layer.GetNextFeature()
      self.assertIsNone(feat)

    layer = self.src.GetLayerByName('none')
    tests = [
        ('id = 1', [1]),
        ('id IS NULL', [6]),
        ('id IS NOT NULL', [1, 2, 3, 4, 5]),
        ('id IS NULL OR id IS NOT NULL', [1, 2, 3, 4, 5, 6]),
        ('id = 1 OR id IS NULL', [1, 6]),
        ('id IS NULL OR id = 1', [1, 6]),
    ]
    for test in tests:
      where_clause, fids = test
      expected_attr_index_use = 2

      layer.SetAttributeFilter(where_clause)
      sql_layer = self.src.ExecuteSQL(
          'GetLayerAttrIndexUse %s' % layer.GetName())
      attr_index_use = int(sql_layer.GetNextFeature().GetField(0))
      self.src.ReleaseResultSet(sql_layer)
      self.assertEqual(expected_attr_index_use, attr_index_use)
      self.assertEqual(len(fids), layer.GetFeatureCount())
      for fid in fids:
        feat = layer.GetNextFeature()
        self.assertEqual(fid, feat.GetFID())
      feat = layer.GetNextFeature()
      self.assertIsNone(feat)

    layer = self.src.GetLayerByName('big_layer')
    tests = [
        ('real = 0', 86, 1),
        ('real = 1', 85, 2),
        ('real = 2', 85, 3),
        ('real = 3', 85, 4),
        ('real >= 0', 86 + 3 * 85, None),
        ('real < 4', 86 + 3 * 85, None),
        ('real > 1 AND real < 2', 0, None),
        ('real < 0', 0, None),
    ]
    for where_clause, count, start in tests:
      layer.SetAttributeFilter(where_clause)
      self.assertEqual(count, layer.GetFeatureCount())
      for i in range(count):
        feat = layer.GetNextFeature()
        self.assertIsNotNone(feat)
        if start is not None:
          self.assertEqual(i * 4 + start, feat.GetFID())
      feat = layer.GetNextFeature()
      self.assertIsNone(feat)

  # TODO(shenhongda): Add test 5

  def test06SQL1(self):
    filepath = ogr_util.GetTestFilePath('testopenfilegdb.gdb.zip')
    self.CheckOpen(filepath)
    sql_layer = self.src.ExecuteSQL(
        'select min(id), max(id), count(id), sum(id), avg(id), min(str), '
        'min(smallint), avg(smallint), min(float), avg(float), '
        'min(real), avg(real), min(adate), avg(adate), min(guid), '
        'min(nullint), avg(nullint) from point')

    self.assertIsNotNone(sql_layer)
    feat = sql_layer.GetNextFeature()
    self.assertEqual(1, feat.GetField('MIN_id'))
    self.assertEqual(5, feat.GetField('MAX_id'))
    self.assertEqual(5, feat.GetField('COUNT_id'))
    self.assertEqual(15.0, feat.GetField('SUM_id'))
    self.assertEqual(3.0, feat.GetField('AVG_id'))
    self.assertEqual('foo_', feat.GetField('MIN_str')[0:4])
    self.assertEqual(-13, feat.GetField('MIN_smallint'))
    self.assertEqual(-13, feat.GetField('AVG_smallint'))
    self.assertEqual(1.5, feat.GetField('MIN_float'))
    self.assertEqual(1.5, feat.GetField('AVG_float'))
    self.assertEqual(4.56, feat.GetField('MIN_real'))
    self.assertEqual(4.56, feat.GetField('AVG_real'))
    self.assertEqual('2013/12/26 12:34:56', feat.GetField('MIN_adate'))
    self.assertEqual('2013/12/26 12:34:56', feat.GetField('AVG_adate'))
    self.assertEqual('{12345678-9ABC-DEF0-1234-567890ABCDEF}',
                     feat.GetField('MIN_guid'))
    self.assertFalse(feat.IsFieldSet('MIN_nullint'))
    self.assertFalse(feat.IsFieldSet('AVG_nullint'))

    self.src.ReleaseResultSet(sql_layer)

  # TODO(shenhongda): Add test 7
  # TODO(shenhongda): Add test 8
  # TODO(shenhongda): Add test 9
  # TODO(shenhongda): Add test 10
  # TODO(shenhongda): Add test 11
  # TODO(shenhongda): Add test 12
  # TODO(shenhongda): Add test 13
  # TODO(shenhongda): Add test 14
  # TODO(shenhongda): Add test 15
  # TODO(shenhongda): Add test 16

  def test17(self):
    # Read a MULTILINESTRING ZM with a dummy M array (#6528).
    filepath = ogr_util.GetTestFilePath(
        'fgdb/multilinestringzm_with_dummy_m_array.gdb.zip')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer(0)
    self.assertIsNotNone(layer)
    feature = layer.GetNextFeature()
    self.assertIsNotNone(feature)


if __name__ == '__main__':
  unittest.main()
