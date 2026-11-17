# Copyright 2017 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2006, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2014, Even Rouault <even dot rouault at mines-paris.org>
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
"""Test OGR handling of GML files.

This is a rewrite of:

https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml_geom.py
https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml_read.py

and includes tests from:
https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gml.py
"""


from collections.abc import Mapping, Sequence
import os
import shutil
from typing import Any
import unittest

from absl.testing import parameterized
from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

DRIVER = ogr_util.GML_DRIVER
EXT = '.gml'


def setUpModule():
  ogr_util.SetupTestEnv()


class OgrGmlTest(ogr_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)
    ogr.UseExceptions()

  def assertGeomEqual(self, result, expect):
    self.assertTrue(expect.Equals(result))

  def _assert_geom_spatially_equal(self, actual: Any, expect_wkt: str) -> None:
    expect = ogr.CreateGeometryFromWkt(expect_wkt)
    self.assertIsNotNone(actual)
    self.assertEqual(actual.GetGeometryName(), expect.GetGeometryName())
    self.assertTrue(actual.Within(expect) and expect.Within(actual))

  def _copy_testdata(self, rel_paths: Sequence[str]) -> str:
    tempdir = self.create_tempdir().full_path
    for rel_path in rel_paths:
      src = ogr_util.GetTestFilePath(rel_path)
      dst = os.path.join(tempdir, os.path.basename(rel_path))
      shutil.copyfile(src, dst)
    return tempdir

  def _touch_newer(self, newer_path: str, older_path: str) -> None:
    older_mtime = os.stat(older_path).st_mtime
    newer_mtime = os.stat(newer_path).st_mtime
    if newer_mtime <= older_mtime:
      os.utime(newer_path, (older_mtime + 10, older_mtime + 10))

  def _verify_fme_gml(self, ds: Any) -> None:
    expected_results = [
        (ogr.wkbMultiPoint, 'MULTIPOINT (2 49)'),
        (ogr.wkbMultiPolygon, 'MULTIPOLYGON (((2 49,3 49,3 50,2 50,2 49)))'),
        (ogr.wkbMultiLineString, 'MULTILINESTRING ((2 49,3 50))'),
    ]
    for layer_idx, (expected_type, expected_wkt) in enumerate(expected_results):
      layer = ds.GetLayer(layer_idx)
      self.assertEqual(layer.GetGeomType(), expected_type)
      for _ in range(2):
        feat = layer.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), expected_wkt)

  def _check_wfs_joint_layer(self, ds: Any) -> None:
    layer = ds.GetLayer(0)
    self.assertTrue(layer.GetName().startswith('join_table1_table2'))
    expected_fields = [
        ('table1.gml_id', ogr.OFTString),
        ('table1.foo', ogr.OFTInteger),
        ('table1.bar', ogr.OFTInteger),
        ('table2.gml_id', ogr.OFTString),
        ('table2.bar', ogr.OFTInteger),
        ('table2.baz', ogr.OFTString),
    ]
    layer_defn = layer.GetLayerDefn()
    self.assertEqual(layer_defn.GetFieldCount(), len(expected_fields))
    for i, (field_name, field_type) in enumerate(expected_fields):
      fld_defn = layer_defn.GetFieldDefn(i)
      self.assertEqual(fld_defn.GetName(), field_name)
      self.assertEqual(fld_defn.GetType(), field_type)
    self.assertEqual(layer_defn.GetGeomFieldCount(), 2)
    self.assertEqual(
        layer_defn.GetGeomFieldDefn(0).GetName(), 'table1.geometry'
    )
    self.assertEqual(
        layer_defn.GetGeomFieldDefn(1).GetName(), 'table2.geometry'
    )

    feat = layer.GetNextFeature()
    self.assertEqual(feat.GetField('table1.gml_id'), 'table1-1')
    self.assertEqual(feat.GetField('table1.foo'), 1)
    self.assertFalse(feat.IsFieldSet('table1.bar'))
    self.assertEqual(feat.GetField('table2.gml_id'), 'table2-1')
    self.assertEqual(feat.GetField('table2.bar'), 2)
    self.assertEqual(feat.GetField('table2.baz'), 'foo')
    self.assertIsNone(feat.GetGeomFieldRef(0))
    self.assertEqual(feat.GetGeomFieldRef(1).ExportToWkt(), 'POINT (2 49)')

    feat = layer.GetNextFeature()
    self.assertEqual(feat.GetField('table1.gml_id'), 'table1-2')
    self.assertFalse(feat.IsFieldSet('table1.foo'))
    self.assertEqual(feat.GetField('table1.bar'), 2)
    self.assertEqual(feat.GetField('table2.gml_id'), 'table2-2')
    self.assertEqual(feat.GetField('table2.bar'), 2)
    self.assertEqual(feat.GetField('table2.baz'), 'bar')
    self.assertEqual(feat.GetGeomFieldRef(0).ExportToWkt(), 'POINT (3 50)')
    self.assertEqual(feat.GetGeomFieldRef(1).ExportToWkt(), 'POINT (2 50)')

  def _copy_min_example(self) -> str:
    return self._copy_testdata([
        'gml/min_example/ft1_schema.xsd',
        'gml/min_example/ft2_schema.xsd',
        'gml/min_example/minimal_example.xsd',
        'gml/min_example/minimal_example.gml',
    ])

  def _read_testdata_bytes(self, rel_path: str) -> bytes:
    with open(ogr_util.GetTestFilePath(rel_path), 'rb') as f:
      return f.read()

  def testPoint(self):
    expect = ogr.CreateGeometryFromWkt('POINT (31 29 16)')
    result = ogr.CreateGeometryFromGML(
        '<gml:Point xmlns:foo="http://bar">'
        '<gml:pos>31 29 16</gml:pos></gml:Point>'
    )
    self.assertGeomEqual(result, expect)

    result = ogr.CreateGeometryFromGML(expect.ExportToGML())
    self.assertGeomEqual(result, expect)

  def testPosListLine2D(self):
    expect = ogr.CreateGeometryFromWkt('LINESTRING (31 42,53 64,55 76)')
    result = ogr.CreateGeometryFromGML(
        '<LineString xmlns:foo="http://bar">'
        '<posList xmlns:foo="http://bar">31 42 53 64 55 76</posList>'
        '</LineString>'
    )
    self.assertGeomEqual(result, expect)

    result = ogr.CreateGeometryFromGML(expect.ExportToGML())
    self.assertGeomEqual(result, expect)

  def testPosListLine3D(self):
    expect = ogr.CreateGeometryFromWkt('LINESTRING (31 42 1,53 64 2,55 76 3)')
    result = ogr.CreateGeometryFromGML(
        '<LineString><posList xmlns:foo="http://bar" srsDimension="3">'
        '31 42 1 53 64 2 55 76 3</posList></LineString>'
    )
    self.assertGeomEqual(result, expect)

    result = ogr.CreateGeometryFromGML(expect.ExportToGML())
    self.assertGeomEqual(result, expect)

  def testPosListLine3D_2(self):
    # GML 3.x "posList" element for a 3D linestring, but with srsDimension
    # set on LineString, not posList.
    expect = ogr.CreateGeometryFromWkt('LINESTRING (31 42 1,53 64 2,55 76 3)')
    result = ogr.CreateGeometryFromGML(
        '<LineString srsDimension="3">'
        '<posList>31 42 1 53 64 2 55 76 3</posList></LineString>'
    )
    self.assertGeomEqual(result, expect)

    result = ogr.CreateGeometryFromGML(expect.ExportToGML())
    self.assertGeomEqual(result, expect)

  def testPolygon(self):
    expect = ogr.CreateGeometryFromWkt(
        'POLYGON ((0 0,4 0,4 4,0 4,0 0),(1 1,2 1,2 2,1 2,1 1))'
    )
    gml = """
        <gml:Polygon xmlns:foo="http://bar">
          <gml:exterior xmlns:foo="http://bar">
            <gml:LinearRing xmlns:foo="http://bar">
              <gml:pos xmlns:foo="http://bar">0 0</gml:pos>
              <gml:pos>4 0</gml:pos>
              <gml:pos>4 4</gml:pos>
              <gml:pos>0 4</gml:pos>
              <gml:pos>0 0</gml:pos>
            </gml:LinearRing>
          </gml:exterior>
          <gml:interior xmlns:foo="http://bar">
            <gml:LinearRing xmlns:foo="http://bar">
              <gml:pos>1 1</gml:pos>
              <gml:pos>2 1</gml:pos>
              <gml:pos>2 2</gml:pos>
              <gml:pos>1 2</gml:pos>
              <gml:pos>1 1</gml:pos>
            </gml:LinearRing>
          </gml:interior>
        </gml:Polygon>"""
    result = ogr.CreateGeometryFromGML(gml)
    self.assertGeomEqual(result, expect)

    result = ogr.CreateGeometryFromGML(expect.ExportToGML())
    self.assertGeomEqual(result, expect)

  @parameterized.named_parameters(
      dict(testcase_name='use_gfs', use_gfs=True),
      dict(testcase_name='no_gfs', use_gfs=False),
  )
  def test_ogr_gml_1(self, use_gfs: bool) -> None:
    files = ['gml/ionic_wfs.gml']
    if use_gfs:
      files.append('gml/ionic_wfs.gfs')
    tempdir = self._copy_testdata(files)
    gml_path = os.path.join(tempdir, 'ionic_wfs.gml')

    with ogr.Open(gml_path) as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      layer = ds.GetLayerByName('GEM')
      self.assertEqual(layer.GetDataset().GetDescription(), ds.GetDescription())

      feat = layer.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('Name'), 'Aartselaar')
      expected_wkt = (
          'POLYGON ((44038 511549,44015 511548,43994 511522,43941 511539,'
          '43844 511514,43754 511479,43685 511521,43594 511505,43619 511452,'
          '43645 511417,4363 511387,437 511346,43749 511298,43808 511229,'
          '43819 511205,4379 511185,43728 511167,43617 511175,43604 511151,'
          '43655 511125,43746 511143,43886 511154,43885 511178,43928 511186,'
          '43977 511217,4404 511223,44008 511229,44099 51131,44095 511335,'
          '44106 51135,44127 511379,44124 511435,44137 511455,44105 511467,'
          '44098 511484,44086 511499,4407 511506,44067 511535,44038 511549))'
      )
      self.CheckFeatureGeometry(feat, expected_wkt)
      self.assertIsNone(layer.GetNextFeature())

  def test_ogr_gml_3(self) -> None:
    tempdir = self._copy_testdata(['gml/rnf_eg.gml'])
    with ogr.Open(os.path.join(tempdir, 'rnf_eg.gml')) as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      layer = ds.GetLayerByName('RoadSegment')
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('ngd_id'), 817792)
      self.assertEqual(feat.GetField('type'), 'HWY')
      expected_wkt = (
          'LINESTRING (-63.500411040289066 46.240122507771368,'
          '-63.501009714909742 46.240344881690326,'
          '-63.502170462373471 46.241041855639622,'
          '-63.505862621395394 46.24195250605576,'
          '-63.506719184531178 46.242002742901576,'
          '-63.507197272602212 46.241931577811606,'
          '-63.508403092799554 46.241752283460158,'
          '-63.509946573455622 46.241745397977233)'
      )
      self.CheckFeatureGeometry(feat, expected_wkt)
      self.assertIsNone(layer.GetNextFeature())

  def test_ogr_gml_4(self) -> None:
    tempdir = self._copy_testdata(['gml/bom.gml'])
    with ogr.Open(os.path.join(tempdir, 'bom.gml')) as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      layer = ds.GetLayerByName('CartographicText')
      self.assertEqual(layer.GetFeatureCount(), 3)

      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('featureCode'), 10198)
      self.assertEqual(feat.GetField('anchorPosition'), 8)
      self.CheckFeatureGeometry(feat, 'POINT (347243.85 461299.5)')

      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('featureCode'), 10069)
      self.CheckFeatureGeometry(feat, 'POINT (347251.45 461250.85)')

  def test_ogr_gml_5(self) -> None:
    tempdir = self._copy_testdata(['gml/ticket_2349_test_1.gml'])
    with ogr.Open(os.path.join(tempdir, 'ticket_2349_test_1.gml')) as ds:
      layer = ds.GetLayerByName('MyPolyline')
      layer.SetAttributeFilter('height > 300')
      layer.GetNextFeature()

  def test_ogr_gml_6(self) -> None:
    filenames = [
        'test_point1.gml',
        'test_point2.gml',
        'test_point3.gml',
        'test_point4.gml',
    ]
    tempdir = self._copy_testdata([f'gml/{name}' for name in filenames])
    for filename in filenames:
      fids: list[int] = []
      with ogr.Open(os.path.join(tempdir, filename)) as ds:
        layer = ds.GetLayer()
        feat = layer.GetNextFeature()
        while feat is not None:
          fid = feat.GetFID()
          self.assertGreaterEqual(fid, 0)
          self.assertNotIn(fid, fids)
          fids.append(fid)
          feat = layer.GetNextFeature()

  def test_ogr_gml_7(self) -> None:
    tempdir = self._copy_testdata(['gml/test_point.gml', 'gml/test_point.xsd'])
    with gdal.config_option('GML_EXPOSE_FID', 'FALSE'):
      with ogr.Open(os.path.join(tempdir, 'test_point.gml')) as ds:
        layer = ds.GetLayer()
        layer_defn = layer.GetLayerDefn()
        self.assertEqual(layer.GetFeatureCount(), 5)
        self.assertEqual(
            layer_defn.GetFieldDefn(0).GetFieldTypeName(
                layer_defn.GetFieldDefn(0).GetType()
            ),
            'Real',
        )
        self.assertEqual(
            layer_defn.GetFieldDefn(1).GetFieldTypeName(
                layer_defn.GetFieldDefn(1).GetType()
            ),
            'Integer',
        )
        self.assertEqual(
            layer_defn.GetFieldDefn(2).GetFieldTypeName(
                layer_defn.GetFieldDefn(2).GetType()
            ),
            'String',
        )

  def test_ogr_gml_8(self) -> None:
    tempdir = self._copy_testdata(['gml/utf8.gml'])
    with ogr.Open(os.path.join(tempdir, 'utf8.gml')) as ds:
      layer = ds.GetLayer()
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetFieldAsString('name'), 'Āliamanu')

  def test_ogr_gml_9(self) -> None:
    tempdir = self.create_tempdir().full_path
    filepath = os.path.join(tempdir, 'broken_utf8.gml')
    with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
      layer = ds.CreateLayer('test')
      layer.CreateField(ogr.FieldDefn('test', ogr.OFTString))
      feat = ogr.Feature(layer.GetLayerDefn())
      feat.SetFieldBinaryFromHexString('test', '80626164')  # \x80bad
      with gdal.quiet_errors():
        self.assertEqual(layer.CreateFeature(feat), 0)

    with ogr.Open(filepath) as ds:
      layer = ds.GetLayerByName('test')
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('test'), '?bad')

  def test_ogr_gml_10(self) -> None:
    tempdir = self.create_tempdir().full_path
    filepath = os.path.join(tempdir, 'fields.gml')
    with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
      layer = ds.CreateLayer('test')
      str_fld = ogr.FieldDefn('string', ogr.OFTString)
      str_fld.SetWidth(100)
      layer.CreateField(str_fld)
      real_fld = ogr.FieldDefn('real', ogr.OFTReal)
      real_fld.SetWidth(4)
      real_fld.SetPrecision(2)
      layer.CreateField(real_fld)
      layer.CreateField(ogr.FieldDefn('float', ogr.OFTReal))
      int_fld = ogr.FieldDefn('integer', ogr.OFTInteger)
      int_fld.SetWidth(5)
      layer.CreateField(int_fld)
      layer.CreateField(ogr.FieldDefn('date', ogr.OFTDate))
      layer.CreateField(ogr.FieldDefn('time', ogr.OFTTime))
      layer.CreateField(ogr.FieldDefn('datetime', ogr.OFTDateTime))

      feat = ogr.Feature(layer.GetLayerDefn())
      feat.SetField('string', 'test string of length 24')
      feat.SetField('real', 12.34)
      feat.SetField('float', 1234.5678)
      feat.SetField('integer', '1234')
      feat.SetField('date', '2019/11/06')
      feat.SetField('time', '12:34:56')
      feat.SetField('datetime', '2019/11/06 12:34:56+00')
      self.assertEqual(layer.CreateFeature(feat), 0)

    with ogr.Open(filepath) as ds:
      layer = ds.GetLayerByName('test')
      feat = layer.GetNextFeature()
      layer_defn = layer.GetLayerDefn()

      self.assertEqual(
          feat.GetFieldDefnRef(feat.GetFieldIndex('string')).GetType(),
          ogr.OFTString,
      )
      self.assertEqual(
          feat.GetFieldDefnRef(feat.GetFieldIndex('real')).GetType(),
          ogr.OFTReal,
      )
      self.assertEqual(
          feat.GetFieldDefnRef(feat.GetFieldIndex('float')).GetType(),
          ogr.OFTReal,
      )
      self.assertEqual(
          feat.GetFieldDefnRef(feat.GetFieldIndex('integer')).GetType(),
          ogr.OFTInteger,
      )
      self.assertEqual(
          feat.GetFieldDefnRef(feat.GetFieldIndex('date')).GetType(),
          ogr.OFTDate,
      )
      self.assertEqual(
          feat.GetFieldDefnRef(feat.GetFieldIndex('time')).GetType(),
          ogr.OFTTime,
      )
      self.assertEqual(
          feat.GetFieldDefnRef(feat.GetFieldIndex('datetime')).GetType(),
          ogr.OFTDateTime,
      )

      self.assertEqual(feat.GetField('string'), 'test string of length 24')
      self.assertEqual(feat.GetFieldAsDouble('real'), 12.34)
      self.assertEqual(feat.GetField('float'), 1234.5678)
      self.assertEqual(feat.GetField('integer'), 1234)
      self.assertEqual(feat.GetField('date'), '2019/11/06')
      self.assertEqual(feat.GetField('time'), '12:34:56')
      self.assertEqual(feat.GetField('datetime'), '2019/11/06 12:34:56+00')

      self.assertEqual(
          layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('string')).GetWidth(),
          100,
      )
      real_defn = layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('real'))
      self.assertEqual(real_defn.GetWidth(), 4)
      self.assertEqual(real_defn.GetPrecision(), 2)
      self.assertEqual(
          layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('integer')).GetWidth(),
          5,
      )

  def test_ogr_gml_11(self) -> None:
    tempdir = self._copy_testdata([
        'gml/testgeometryelementpath.gml',
        'gml/testgeometryelementpath.gfs',
    ])
    gml_path = os.path.join(tempdir, 'testgeometryelementpath.gml')
    gfs_path = os.path.join(tempdir, 'testgeometryelementpath.gfs')
    self._touch_newer(gfs_path, gml_path)

    with ogr.Open(gml_path) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetGeometryColumn(), 'location1container|location1')
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('attrib1'), 'attrib1_value')
      self.assertEqual(feat.GetField('attrib2'), 'attrib2_value')
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (3 50)')

  def test_ogr_gml_12(self) -> None:
    zip_path = ogr_util.GetTestFilePath('gml/testgeometryelementpath.zip')
    with ogr.Open(f'/vsizip/{zip_path}/testgeometryelementpath.gml') as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetGeometryColumn(), 'location1container|location1')
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('attrib1'), 'attrib1_value')
      self.assertEqual(feat.GetField('attrib2'), 'attrib2_value')
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (3 50)')

  def test_ogr_gml_13(self) -> None:
    tempdir = self._copy_testdata(['gml/testlistfields.gml'])
    gml_path = os.path.join(tempdir, 'testlistfields.gml')
    for _ in range(2):
      with ogr.Open(gml_path) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetFieldAsStringList(feat.GetFieldIndex('attrib1')),
            ['value1', 'value2'],
        )
        self.assertEqual(feat.GetField(feat.GetFieldIndex('attrib2')), 'value3')
        self.assertEqual(
            feat.GetFieldAsIntegerList(feat.GetFieldIndex('attrib3')),
            [4, 5],
        )
        self.assertEqual(
            feat.GetFieldAsDoubleList(feat.GetFieldIndex('attrib4')),
            [6.1, 7.1],
        )

  # test_ogr_gml_14 omitted: requires CURL/HTTP network access unavailable in Google3.
  # test_ogr_gml_15 omitted: upstream runs the external test_ogrsf CLI binary.

  def test_ogr_gml_city_gml(self) -> None:
    tempdir = self._copy_testdata(['gml/citygml.gml'])
    with ogr.Open(os.path.join(tempdir, 'citygml.gml')) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetGeomType(), ogr.wkbMultiPolygon25D)
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('Name_'), 'aname')
      self.assertEqual(feat.GetField('a_int_attr'), 2)
      self.assertEqual(feat.GetField('a_double_attr'), 3.45)
      self.assertEqual(
          feat.GetGeometryRef().ExportToIsoWkt(),
          'MULTIPOLYGON Z (((0 0 0,0.0 0.5 0,0 1 0,1 1 0,1 0 0,0 0 0)))',
      )

  def test_gml_read_compound_crs_lat_long(self) -> None:
    tempdir = self._copy_testdata(['gml/citygml_compound_crs.gml'])
    with ogr.Open(os.path.join(tempdir, 'citygml_compound_crs.gml')) as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      layer = ds.GetLayer(0)
      sr = osr.SpatialReference()
      sr.ImportFromEPSG(6668)  # JGD2011
      self.assertTrue(
          sr.IsSame(
              layer.GetSpatialRef(),
              options=['IGNORE_DATA_AXIS_TO_SRS_AXIS_MAPPING=YES'],
          )
      )
      expected_wkt = (
          'POLYHEDRALSURFACE Z (((139.812484938717 35.7092130413279 0.15,'
          '139.812489071491 35.7091641446533 0.15,'
          '139.812444202746 35.7091610722245 0.15,'
          '139.812439721473 35.7092112956502 0.15,'
          '139.812436111402 35.7092517484017 0.15,'
          '139.812481422309 35.7092546406366 0.15,'
          '139.812484938717 35.7092130413279 0.15)),'
          '((139.812484938717 35.7092130413279 0.15,'
          '139.812481422309 35.7092546406366 0.15,'
          '139.812481422309 35.7092546406366 12.08,'
          '139.812484938717 35.7092130413279 12.08,'
          '139.812484938717 35.7092130413279 0.15)),'
          '((139.812481422309 35.7092546406366 0.15,'
          '139.812436111402 35.7092517484017 0.15,'
          '139.812436111402 35.7092517484017 12.08,'
          '139.812481422309 35.7092546406366 12.08,'
          '139.812481422309 35.7092546406366 0.15)),'
          '((139.812436111402 35.7092517484017 0.15,'
          '139.812439721473 35.7092112956502 0.15,'
          '139.812439721473 35.7092112956502 12.08,'
          '139.812436111402 35.7092517484017 12.08,'
          '139.812436111402 35.7092517484017 0.15)),'
          '((139.812439721473 35.7092112956502 0.15,'
          '139.812444202746 35.7091610722245 0.15,'
          '139.812444202746 35.7091610722245 12.08,'
          '139.812439721473 35.7092112956502 12.08,'
          '139.812439721473 35.7092112956502 0.15)),'
          '((139.812444202746 35.7091610722245 0.15,'
          '139.812489071491 35.7091641446533 0.15,'
          '139.812489071491 35.7091641446533 12.08,'
          '139.812444202746 35.7091610722245 12.08,'
          '139.812444202746 35.7091610722245 0.15)),'
          '((139.812489071491 35.7091641446533 0.15,'
          '139.812484938717 35.7092130413279 0.15,'
          '139.812484938717 35.7092130413279 12.08,'
          '139.812489071491 35.7091641446533 12.08,'
          '139.812489071491 35.7091641446533 0.15)),'
          '((139.812484938717 35.7092130413279 12.08,'
          '139.812481422309 35.7092546406366 12.08,'
          '139.812436111402 35.7092517484017 12.08,'
          '139.812439721473 35.7092112956502 12.08,'
          '139.812444202746 35.7091610722245 12.08,'
          '139.812489071491 35.7091641446533 12.08,'
          '139.812484938717 35.7092130413279 12.08)))'
      )
      feat = layer.GetNextFeature()
      self.CheckFeatureGeometry(feat, expected_wkt)

  def test_ogr_gml_city_gml_lod2_with_xlink_href(self) -> None:
    tempdir = self._copy_testdata(['gml/citygml_lod2_713_5322.xml'])
    gdal.ErrorReset()
    with ogr.Open(os.path.join(tempdir, 'citygml_lod2_713_5322.xml')) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(gdal.GetLastErrorMsg(), '')
      self.assertTrue(layer.GetSpatialRef().IsProjected())
      self.assertEqual(layer.GetGeomType(), ogr.wkbPolyhedralSurfaceZ)

      feat = layer.GetNextFeature()
      expected_wkt = (
          'POLYHEDRALSURFACE Z (((713235.347 5322980.429 584.22,'
          '713235.909 5322980.781 584.22,713237.154 5322981.561 584.22,'
          '713237.154 5322981.561 587.655,713235.909 5322980.781 588.03,'
          '713235.347 5322980.429 587.86,713235.347 5322980.429 584.22)),'
          '((713236.374 5322982.678 587.658,713237.154 5322981.561 587.655,'
          '713237.154 5322981.561 584.22,713236.374 5322982.678 584.22,'
          '713234.768 5322984.981 584.22,713234.768 5322984.981 587.666,'
          '713236.374 5322982.678 587.658)),'
          '((713234.612 5322981.452 584.22,713235.347 5322980.429 584.22,'
          '713235.347 5322980.429 587.86,713234.612 5322981.452 587.853,'
          '713234.612 5322981.452 588.21,713234.585 5322981.49 588.21,'
          '713234.585 5322981.49 584.22,713234.612 5322981.452 584.22)),'
          '((713229.269 5322983.856 587.663,713231.399 5322985.337 588.33,'
          '713233.571 5322986.847 587.65,713235.231 5322988.002 587.13,'
          '713235.231 5322988.002 584.22,713233.571 5322986.847 584.22,'
          '713231.399 5322985.337 584.22,713229.269 5322983.856 584.22,'
          '713229.269 5322983.856 587.663)),'
          '((713235.231 5322988.002 584.22,713235.231 5322988.002 587.13,'
          '713236.493 5322986.18 587.126,713236.493 5322986.18 584.22,'
          '713235.231 5322988.002 584.22)),'
          '((713232.071 5322979.751 584.22,713232.071 5322979.751 587.663,'
          '713229.269 5322983.856 587.663,713229.269 5322983.856 584.22,'
          '713232.071 5322979.751 584.22)),'
          '((713235.909 5322980.781 584.22,713235.347 5322980.429 584.22,'
          '713234.612 5322981.452 584.22,713234.585 5322981.49 584.22,'
          '713234.205 5322981.227 584.22,713232.071 5322979.751 584.22,'
          '713229.269 5322983.856 584.22,713231.399 5322985.337 584.22,'
          '713233.571 5322986.847 584.22,713235.231 5322988.002 584.22,'
          '713236.493 5322986.18 584.22,713234.82 5322985.017 584.22,'
          '713234.768 5322984.981 584.22,713236.374 5322982.678 584.22,'
          '713237.154 5322981.561 584.22,713235.909 5322980.781 584.22)),'
          '((713232.071 5322979.751 584.22,713234.205 5322981.227 584.22,'
          '713234.585 5322981.49 584.22,713234.585 5322981.49 588.21,'
          '713234.205 5322981.227 588.33,713232.071 5322979.751 587.663,'
          '713232.071 5322979.751 584.22)),'
          '((713235.181 5322981.849 588.032,713234.612 5322981.452 588.21,'
          '713234.612 5322981.452 587.853,713235.181 5322981.849 588.032)),'
          '((713235.347 5322980.429 587.86,713235.909 5322980.781 588.03,'
          '713235.181 5322981.849 588.032,713234.612 5322981.452 587.853,'
          '713235.347 5322980.429 587.86)),'
          '((713234.768 5322984.981 584.22,713234.82 5322985.017 584.22,'
          '713236.493 5322986.18 584.22,713236.493 5322986.18 587.126,'
          '713234.82 5322985.017 587.65,713234.768 5322984.981 587.666,'
          '713234.768 5322984.981 584.22)),'
          '((713234.205 5322981.227 588.33,713234.585 5322981.49 588.21,'
          '713234.612 5322981.452 588.21,713235.181 5322981.849 588.032,'
          '713235.909 5322980.781 588.03,713237.154 5322981.561 587.655,'
          '713236.374 5322982.678 587.658,713234.768 5322984.981 587.666,'
          '713234.82 5322985.017 587.65,713236.493 5322986.18 587.126,'
          '713235.231 5322988.002 587.13,713233.571 5322986.847 587.65,'
          '713231.399 5322985.337 588.33,713234.205 5322981.227 588.33)),'
          '((713232.071 5322979.751 587.663,713234.205 5322981.227 588.33,'
          '713231.399 5322985.337 588.33,713229.269 5322983.856 587.663,'
          '713232.071 5322979.751 587.663)))'
      )
      self.CheckFeatureGeometry(feat, expected_wkt)
      feat = layer.GetNextFeature()
      self.assertFalse(feat.GetGeometryRef().IsEmpty())

  def test_ogr_gml_17(self) -> None:
    tempdir = self._copy_testdata(['gml/gnis_pop_100.gml'])
    with ogr.Open(os.path.join(tempdir, 'gnis_pop_100.gml')) as ds:
      layer = ds.GetLayer(0)
      sr = layer.GetSpatialRef()
      self.assertIn('GEOGCS["WGS 84"', sr.ExportToWkt())
      self.assertEqual(layer.GetExtent(), (-80.17, 76.58, -13.32, 51.0))
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2.09 34.12)')

  def test_ogr_gml_18(self) -> None:
    tempdir = self._copy_testdata(['gml/gnis_pop_110.gml'])
    with ogr.Open(os.path.join(tempdir, 'gnis_pop_110.gml')) as ds:
      layer = ds.GetLayer(0)
      sr = layer.GetSpatialRef()
      self.assertIn('GEOGCS["WGS 84"', sr.ExportToWkt())
      self.assertEqual(sr.GetDataAxisToSRSAxisMapping(), [2, 1])
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2.09 34.12)')

  def test_ogr_gml_19(self) -> None:
    tempdir = self._copy_testdata(['gml/gnis_pop_110.gml'])
    with gdal.config_option('GML_INVERT_AXIS_ORDER_IF_LAT_LONG', 'NO'):
      with ogr.Open(os.path.join(tempdir, 'gnis_pop_110.gml')) as ds:
        layer = ds.GetLayer(0)
        sr = layer.GetSpatialRef()
        self.assertIn('GEOGCS["WGS 84"', sr.ExportToWkt())
        self.assertEqual(sr.GetDataAxisToSRSAxisMapping(), [1, 2])
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(), 'POINT (34.12 2.09)'
        )

  def test_ogr_gml_20(self) -> None:
    tempdir = self._copy_testdata(['gml/archsites.gml', 'gml/archsites.xsd'])
    gml_path = os.path.join(tempdir, 'archsites.gml')
    with ogr.Open(gml_path) as ds:
      layer = ds.GetLayer(0)
      layer_defn = layer.GetLayerDefn()

      self.assertNotEqual(layer_defn.GetFieldIndex('gml_id'), -1)

      cat_defn = layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('cat'))
      self.assertEqual(
          cat_defn.GetFieldTypeName(cat_defn.GetType()), 'Integer64'
      )

      str1_defn = layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('str1'))
      self.assertEqual(
          str1_defn.GetFieldTypeName(str1_defn.GetType()), 'String'
      )

      self.assertEqual(layer.GetGeometryColumn(), 'the_geom')
      self.assertEqual(layer_defn.GetGeomType(), ogr.wkbPoint)

    self.assertFalse(os.path.exists(os.path.join(tempdir, 'archsites.gfs')))

  @parameterized.named_parameters(
      ('gml3', 'GML3', 'expected_gml_gml3'),
      ('gml3_deegree', 'GML3Deegree', 'expected_gml_gml3degree'),
      ('gml32', 'GML3.2', 'expected_gml_gml32'),
  )
  def test_ogr_gml_21(self, frmt: str, base_filename: str) -> None:
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    tempdir = self.create_tempdir().full_path
    filename = os.path.join(tempdir, f'{base_filename}.gml')

    with ogr.GetDriverByName('GML').CreateDataSource(
        filename, options=[f'FORMAT={frmt}']
    ) as ds:
      layer = ds.CreateLayer('firstlayer', srs=sr)
      layer.CreateField(ogr.FieldDefn('string_field', ogr.OFTString))

      feat = ogr.Feature(layer.GetLayerDefn())
      feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (2 49)'))
      layer.CreateFeature(feat)

      feat = ogr.Feature(layer.GetLayerDefn())
      feat.SetField(0, 'foo')
      feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (3 48)'))
      layer.CreateFeature(feat)

    with ogr.Open(filename) as ds:
      layer = ds.GetLayer(0)
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

    for ext in ('.gml', '.xsd'):
      actual_path = os.path.join(tempdir, f'{base_filename}{ext}')
      expected_path = ogr_util.GetTestFilePath(f'gml/{base_filename}{ext}')
      with open(actual_path, 'rt', encoding='utf-8') as actual_f:
        actual_lines = [line.strip() for line in actual_f]
      with open(expected_path, 'rt', encoding='utf-8') as expected_f:
        expected_lines = [line.strip() for line in expected_f]
      self.assertEqual(actual_lines, expected_lines)

  def test_ogr_gml_22(self) -> None:
    tempdir = self._copy_testdata(['gml/paris_typical_strike_demonstration.xml'])
    xml_path = os.path.join(tempdir, 'paris_typical_strike_demonstration.xml')
    with ogr.Open(xml_path) as ds:
      self.assertIsNotNone(ds.GetLayerByName('RouteGeometry'))
      layer = ds.GetLayerByName('RouteInstruction')
      self.assertIsNotNone(layer)
      self.assertEqual(layer.GetFeatureCount(), 9)

  def test_ogr_gml_23(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/global_geometry.xml', 'gml/global_geometry.xsd']
    )
    with ogr.Open(os.path.join(tempdir, 'global_geometry.xml')) as ds:
      layer = ds.GetLayer(0)
      sr = layer.GetSpatialRef()
      self.assertIn('GEOGCS["WGS 84"', sr.ExportToWkt())
      self.assertEqual(sr.GetDataAxisToSRSAxisMapping(), [2, 1])
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')
      self.assertEqual(layer.GetExtent(), (2.0, 3.0, 49.0, 50.0))

  def test_ogr_gml_24(self) -> None:
    tempdir = self._copy_testdata(['gml/global_geometry.xml'])
    with ogr.Open(os.path.join(tempdir, 'global_geometry.xml')) as ds:
      layer = ds.GetLayer(0)
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')
      self.assertEqual(layer.GetExtent(), (2.0, 3.0, 49.0, 50.0))

  def test_ogr_gml_25(self) -> None:
    tempdir = self._copy_testdata(['gml/curveProperty.xml'])
    with gdal.config_option('GML_FACE_HOLE_NEGATIVE', 'YES'):
      with ogr.Open(os.path.join(tempdir, 'curveProperty.xml')) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(),
            'POLYGON ((14 21,6 21,6 9,14 9,22 9,22 21,14 21))',
        )

  def test_ogr_gml_26(self) -> None:
    tempdir = self.create_tempdir().full_path
    out_gml = os.path.join(tempdir, 'ogr_gml_26.gml')
    src_shp = ogr_util.GetTestFilePath('shape/poly/poly.shp')
    gdal.VectorTranslate(
        out_gml,
        src_shp,
        options=['-f', 'GML', '-dsco', 'FORMAT=GML2', '-zfield', 'eas_id'],
    )
    with open(out_gml, 'rt', encoding='utf-8') as f:
      content = f.read()
    self.assertIn(
        '<gml:coord><gml:X>478315.53125</gml:X><gml:Y>4762880.5</gml:Y>'
        '<gml:Z>158</gml:Z></gml:coord>',
        content,
    )
    with ogr.Open(out_gml) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetGeomType(), ogr.wkbPolygon25D)

  def test_ogr_gml_27(self) -> None:
    tempdir = self.create_tempdir().full_path
    out_gml = os.path.join(tempdir, 'ogr_gml_27.gml')
    src_shp = ogr_util.GetTestFilePath('shape/poly/poly.shp')
    gdal.VectorTranslate(
        out_gml,
        src_shp,
        options=['-f', 'GML', '-dsco', 'FORMAT=GML3', '-zfield', 'eas_id'],
    )
    with open(out_gml, 'rt', encoding='utf-8') as f:
      content = f.read()
    self.assertIn(
        '<gml:lowerCorner>478315.53125 4762880.5 158</gml:lowerCorner>',
        content,
    )
    with ogr.Open(out_gml) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetGeomType(), ogr.wkbPolygon25D)

  def test_ogr_gml_28(self) -> None:
    tempdir = self.create_tempdir().full_path
    out_gml = os.path.join(tempdir, 'ogr_gml_28.gml')
    src_dbf = ogr_util.GetTestFilePath('idlink.dbf')
    gdal.VectorTranslate(out_gml, src_dbf, format='GML')

    # Try with .xsd
    with ogr.Open(out_gml) as ds:
      self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbNone)

    os.unlink(os.path.join(tempdir, 'ogr_gml_28.xsd'))

    # Without .xsd (generates .gfs)
    with ogr.Open(out_gml) as ds:
      self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbNone)

    # With .gfs
    with ogr.Open(out_gml) as ds:
      self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbNone)

  def test_ogr_gml_29(self) -> None:
    tempdir = self._copy_testdata(['gml/testfmegml.gml', 'gml/testfmegml.xsd'])
    with ogr.Open(os.path.join(tempdir, 'testfmegml.gml')) as ds:
      self._verify_fme_gml(ds)

  def test_ogr_gml_30(self) -> None:
    field1 = ' ' * 2048
    geom = '0 1 ' * 512

    data = f"""<FeatureCollection xmlns:gml="http://www.opengis.net/gml">
      <gml:featureMember>
        <layer1>
          <geometry>
            <gml:LineString>
              <gml:posList>{geom}</gml:posList>
            </gml:LineString>
          </geometry>
          <field1>A{field1}Z</field1>
        </layer1>
      </gml:featureMember>
    </FeatureCollection>"""

    filepath = '/vsimem/ogr_gml_30.gml'
    with gcore_util.GdalUnlinkWhenDone(filepath):
      gdal.FileFromMemBuffer(filepath, data)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        field1_val = feat.GetField(0)
        geom_wkt = feat.GetGeometryRef().ExportToWkt()

      self.assertLen(field1_val, 2050)
      self.assertLen(geom_wkt, 2060)

  def test_ogr_gml_31(self) -> None:
    tempdir = self._copy_testdata(['gml/testfmegml.gml', 'gml/testfmegml.xsd'])
    gml_path = os.path.join(tempdir, 'testfmegml.gml')

    with gdal.config_option('GML_READ_MODE', 'SEQUENTIAL_LAYERS'):
      with ogr.Open(gml_path) as ds:
        self._verify_fme_gml(ds)

      with ogr.Open(gml_path) as ds:
        layer = ds.GetLayer(1)
        layer.GetNextFeature()
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetFID(), 1)

        layer = ds.GetLayer(0)
        layer.GetNextFeature()
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetFID(), 1)

  def test_ogr_gml_32(self) -> None:
    tempdir = self._copy_testdata(['gml/testfmegml.gml'])
    gml_path = os.path.join(tempdir, 'testfmegml.gml')
    gfs_path = os.path.join(tempdir, 'testfmegml.gfs')

    with ogr.Open(gml_path) as ds:
      layer = ds.GetLayer(1)
      layer.GetNextFeature()
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetFID(), 1)

    with open(gfs_path, 'rt', encoding='utf-8') as f:
      self.assertIn('<SequentialLayers>true</SequentialLayers>', f.read())

  def test_ogr_gml_33(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/testfmegml_interleaved.gml', 'gml/testfmegml_interleaved.gfs']
    )
    gml_path = os.path.join(tempdir, 'testfmegml_interleaved.gml')
    gfs_path = os.path.join(tempdir, 'testfmegml_interleaved.gfs')
    self._touch_newer(gfs_path, gml_path)

    with gdal.config_option('GML_READ_MODE', 'INTERLEAVED_LAYERS'):
      with ogr.Open(gml_path) as ds:
        read_sequence = [
            (0, 1),
            (0, None),
            (1, 3),
            (2, 5),
            (2, None),
            (0, 2),
            (1, 4),
            (1, None),
            (2, 6),
            (2, None),
            (0, None),
            (1, None),
            (2, None),
        ]
        for layer_idx, expected_fid in read_sequence:
          feat = ds.GetLayer(layer_idx).GetNextFeature()
          fid = None if feat is None else feat.GetFID()
          self.assertEqual(fid, expected_fid)

  def test_ogr_gml_34(self) -> None:
    filepath = '/vsimem/ogr_gml_34.gml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_34.xsd'),
    ):
      drv = ogr.GetDriverByName('GML')
      with drv.CreateDataSource(filepath) as ds:
        layer = ds.CreateLayer('test')
        layer.CreateField(ogr.FieldDefn('name', ogr.OFTString))
        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetField(0, '\xc4\x80liamanu<&')
        layer.CreateFeature(feat)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetFieldAsString('name'), '\xc4\x80liamanu<&')

  def test_ogr_gml_35_huge_resolver_disabled_without_have_sqlite(
      self,
  ) -> None:
    # //third_party/gdal/BUILD defines SQLITE_ENABLED but not HAVE_SQLITE,
    # so GMLHugeFileResolver compiles without SQLite support.
    tempdir = self._copy_testdata(['gml/GmlTopo-sample.xml'])
    xml_path = os.path.join(tempdir, 'GmlTopo-sample.xml')
    with gdal.config_option('GML_SKIP_RESOLVE_ELEMS', 'HUGE'):
      with gdal.quiet_errors(), self.assertRaisesRegex(RuntimeError, 'SQLite'):
        ogr.Open(xml_path)

  @parameterized.product(
      face_hole_negative=('NO', 'YES'),
      skip_resolve_as_open_option=(True, False),
  )
  def test_ogr_gml_36(
      self, face_hole_negative: str, skip_resolve_as_open_option: bool
  ) -> None:
    tempdir = self._copy_testdata(['gml/GmlTopo-sample.xml'])
    xml_path = os.path.join(tempdir, 'GmlTopo-sample.xml')
    if face_hole_negative == 'NO':
      expected_wkt = (
          'MULTIPOLYGON (((-0.1 0.6,-0.0 0.7,0.2 0.7,0.3 0.6,0.5 0.6,0.5 0.8,'
          '0.7 0.8,0.8 0.6,0.9 0.6,0.9 0.4,0.7 0.3,0.7 0.2,0.9 0.1,0.9 -0.1,'
          '0.6 -0.2,0.3 -0.2,0.2 -0.2,-0.1 0.0,-0.1 0.1,-0.1 0.2,0.1 0.3,'
          '0.1 0.4,-0.0 0.4,-0.1 0.5,-0.1 0.6)))'
      )
    else:
      expected_wkt = (
          'POLYGON ((-0.1 0.6,-0.0 0.7,0.2 0.7,0.3 0.6,0.5 0.6,0.5 0.8,'
          '0.7 0.8,0.8 0.6,0.9 0.6,0.9 0.4,0.7 0.3,0.7 0.2,0.9 0.1,0.9 -0.1,'
          '0.6 -0.2,0.3 -0.2,0.2 -0.2,-0.1 0.0,-0.1 0.1,-0.1 0.2,0.1 0.3,'
          '0.1 0.4,-0.0 0.4,-0.1 0.5,-0.1 0.6),'
          '(0.2 0.2,0.2 0.4,0.4 0.4,0.5 0.2,0.5 0.1,0.5 0.0,0.2 0.0,0.2 0.2),'
          '(0.6 0.1,0.8 0.1,0.8 -0.1,0.6 -0.1,0.6 0.1))'
      )

    gdal.ErrorReset()
    if skip_resolve_as_open_option:
      with gdal.config_option('GML_FACE_HOLE_NEGATIVE', face_hole_negative):
        with gdal.OpenEx(
            xml_path, open_options=['SKIP_RESOLVE_ELEMS=NONE']
        ) as ds:
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          feat = ds.GetLayerByName('Suolo').GetNextFeature()
          self._assert_geom_spatially_equal(
              feat.GetGeometryRef(), expected_wkt
          )
    else:
      with gdal.config_options({
          'GML_SKIP_RESOLVE_ELEMS': 'NONE',
          'GML_FACE_HOLE_NEGATIVE': face_hole_negative,
      }):
        with ogr.Open(xml_path) as ds:
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          feat = ds.GetLayerByName('Suolo').GetNextFeature()
          self._assert_geom_spatially_equal(
              feat.GetGeometryRef(), expected_wkt
          )

    with gdal.config_option('GML_FACE_HOLE_NEGATIVE', face_hole_negative):
      with ogr.Open(xml_path) as ds:
        feat = ds.GetLayerByName('Suolo').GetNextFeature()
        self._assert_geom_spatially_equal(feat.GetGeometryRef(), expected_wkt)

  @parameterized.product(
      resolver=('HUGE', 'NONE'),
      skip_resolve_as_open_option=(True, False),
  )
  def test_ogr_gml_38(
      self, resolver: str, skip_resolve_as_open_option: bool
  ) -> None:
    tempdir = self._copy_testdata(['gml/sample_gml_face_hole_negative_no.xml'])
    xml_path = os.path.join(tempdir, 'sample_gml_face_hole_negative_no.xml')
    if resolver == 'HUGE':
      # HAVE_SQLITE is not defined in //third_party/gdal/BUILD.
      with gdal.quiet_errors(), self.assertRaisesRegex(RuntimeError, 'SQLite'):
        if skip_resolve_as_open_option:
          gdal.OpenEx(xml_path, open_options=['SKIP_RESOLVE_ELEMS=HUGE'])
        else:
          with gdal.config_option('GML_SKIP_RESOLVE_ELEMS', 'HUGE'):
            ogr.Open(xml_path)
      return

    expected_wkt = (
        'MULTIPOLYGON (((0.9 0.6,0.9 0.4,0.7 0.3,0.7 0.2,0.9 0.1,0.9 -0.1,'
        '0.6 -0.2,0.3 -0.2,0.2 -0.2,-0.1 0.0,-0.1 0.1,-0.1 0.2,0.1 0.3,'
        '0.1 0.4,-0.0 0.4,-0.1 0.5,-0.1 0.6,-0.0 0.7,0.2 0.7,0.3 0.6,0.5 0.6,'
        '0.5 0.8,0.7 0.8,0.8 0.6,0.9 0.6),(0.6 0.1,0.6 -0.1,0.8 -0.1,'
        '0.8 0.1,0.6 0.1),(0.2 0.4,0.2 0.2,0.2 0.0,0.5 0.0,0.5 0.1,0.5 0.2,'
        '0.4 0.4,0.2 0.4)))'
    )
    gdal.ErrorReset()
    if skip_resolve_as_open_option:
      with gdal.OpenEx(xml_path, open_options=[f'SKIP_RESOLVE_ELEMS={resolver}']) as ds:
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        self.CheckFeatureGeometry(
            ds.GetLayerByName('Suolo').GetNextFeature(), expected_wkt
        )
    else:
      with gdal.config_option('GML_SKIP_RESOLVE_ELEMS', resolver):
        with ogr.Open(xml_path) as ds:
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          self.CheckFeatureGeometry(
              ds.GetLayerByName('Suolo').GetNextFeature(), expected_wkt
          )

  @parameterized.parameters(True, False)
  def test_ogr_gml_huge_resolver_same_nested_property_name(
      self, skip_resolve_as_open_option: bool
  ) -> None:
    tempdir = self._copy_testdata(['gml/same_nested_property_name.gml'])
    gml_path = os.path.join(tempdir, 'same_nested_property_name.gml')

    with ogr.Open(gml_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(feat['gml_id'], 'test.0')
      self.assertEqual(feat['test'], 'foo')
      self.assertEqual(feat['bar|test'], 'bar')

    # HAVE_SQLITE is not defined in //third_party/gdal/BUILD.
    with gdal.quiet_errors(), self.assertRaisesRegex(RuntimeError, 'SQLite'):
      if skip_resolve_as_open_option:
        gdal.OpenEx(gml_path, open_options=['SKIP_RESOLVE_ELEMS=HUGE'])
      else:
        with gdal.config_option('GML_SKIP_RESOLVE_ELEMS', 'HUGE'):
          ogr.Open(gml_path)

  def test_ogr_gml_40(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/testLookForSimpleType.xml', 'gml/testLookForSimpleType.xsd']
    )
    with ogr.Open(os.path.join(tempdir, 'testLookForSimpleType.xml')) as ds:
      layer_defn = ds.GetLayer(0).GetLayerDefn()
      fld_defn = layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('CITYNAME'))
      self.assertEqual(fld_defn.GetWidth(), 26)

  def test_ogr_gml_42_validate_schema_unsupported_without_libxml2(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/expected_gml_gml32.gml', 'gml/expected_gml_gml32.xsd']
    )
    with ogr.Open(os.path.join(tempdir, 'expected_gml_gml32.gml')) as ds:
      with gdal.quiet_errors(), self.assertRaisesRegex(
          RuntimeError, 'not implemented due to missing libxml2 support'
      ):
        ds.ExecuteSQL('SELECT ValidateSchema()')

  # test_ogr_gml_43 omitted: disabled upstream (testing.deegree.org times out) and CURL is disabled.

  def test_ogr_gml_44(self) -> None:
    xsd_content = """<?xml version="1.0" encoding="UTF-8"?>
<xs:schema targetNamespace="http://ogr.maptools.org/" xmlns:ogr="http://ogr.maptools.org/" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:gml="http://www.opengis.net/gml" elementFormDefault="qualified" version="1.0">
<xs:import namespace="http://www.opengis.net/gml" schemaLocation="http://schemas.opengeospatial.net/gml/2.1.2/feature.xsd"/><xs:element name="FeatureCollection" type="ogr:FeatureCollectionType" substitutionGroup="gml:_FeatureCollection"/>
<xs:complexType name="FeatureCollectionType">
  <xs:complexContent>
    <xs:extension base="gml:AbstractFeatureCollectionType">
      <xs:attribute name="lockId" type="xs:string" use="optional"/>
      <xs:attribute name="scope" type="xs:string" use="optional"/>
    </xs:extension>
  </xs:complexContent>
</xs:complexType>
<xs:element name="test_point" type="ogr:test_point_Type" substitutionGroup="gml:_Feature"/>
<xs:complexType name="test_point_Type">
  <xs:complexContent>
    <xs:extension base="gml:AbstractFeatureType">
      <xs:sequence>
<xs:element name="geometryProperty" type="gml:GeometryPropertyType" nillable="true" minOccurs="1" maxOccurs="1"/>
    <xs:element name="dbl" nillable="true" minOccurs="0" maxOccurs="1">
      <xs:simpleType>
        <xs:restriction base="xs:decimal">
          <xs:totalDigits value="32"/>
          <xs:fractionDigits value="3"/>
        </xs:restriction>
      </xs:simpleType>
    </xs:element>
      </xs:sequence>
    </xs:extension>
  </xs:complexContent>
</xs:complexType>
</xs:schema>"""
    tempdir = self._copy_testdata(['gml/test_point.gml'])
    gml_path = os.path.join(tempdir, 'test_point.gml')
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_44'):
      xsd_path = '/vsimem/ogr_gml_44/ogr_gml_44.xsd'
      gdal.FileFromMemBuffer(xsd_path, xsd_content)
      with ogr.Open(f'{gml_path},xsd={xsd_path}') as ds:
        self.assertEqual(ds.GetLayer(0).GetLayerDefn().GetFieldCount(), 2)

  def test_ogr_gml_45(self) -> None:
    # Note: schema validation via ValidateSchema() requires HAVE_LIBXML2.
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_45'):
      filepath = '/vsimem/ogr_gml_45/ogr_gml_45.gml'
      with ogr.GetDriverByName('GML').CreateDataSource(
          filepath, options=['PREFIX=foo', 'TARGET_NAMESPACE=http://bar/']
      ) as ds:
        layer = ds.CreateLayer('test')
        layer.CreateField(ogr.FieldDefn('str', ogr.OFTString))
        layer.CreateField(ogr.FieldDefn('int', ogr.OFTInteger))
        layer.CreateField(ogr.FieldDefn('dbl', ogr.OFTReal))

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetField('str', 'str')
        feat.SetField('int', 1)
        feat.SetField('dbl', 2.34)
        layer.CreateFeature(feat)

      content = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('xmlns:foo="http://bar/"', content)
      self.assertIn('<foo:FeatureCollection', content)
      with ogr.Open(filepath) as ds:
        feat = ds.GetLayer(0).GetNextFeature()
        self.assertEqual(feat['str'], 'str')
        self.assertEqual(feat['int'], 1)
        self.assertEqual(feat['dbl'], 2.34)

  @parameterized.product(
      frmt=('GML2', 'GML3', 'GML3Deegree', 'GML3.2'),
      wkt=(
          '',
          'POINT (0 1)',
          'LINESTRING (0 1,2 3)',
          'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
          'MULTIPOINT (0 1)',
          'MULTILINESTRING ((0 1,2 3))',
          'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))',
          'GEOMETRYCOLLECTION (POINT (0 1))',
      ),
  )
  def test_ogr_gml_46(self, frmt: str, wkt: str) -> None:
    # Note: ValidateSchema() skipped because HAVE_LIBXML2 is disabled in Google3.
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_46'):
      filepath = '/vsimem/ogr_gml_46/ogr_gml_46.gml'
      with ogr.GetDriverByName('GML').CreateDataSource(
          filepath, options=[f'FORMAT={frmt}']
      ) as ds:
        if wkt:
          geom = ogr.CreateGeometryFromWkt(wkt)
          geom_type = geom.GetGeometryType()
          srs = osr.SpatialReference()
          srs.ImportFromEPSG(4326)
        else:
          geom = None
          geom_type = ogr.wkbNone
          srs = None

        layer = ds.CreateLayer('test', geom_type=geom_type, srs=srs)
        layer.CreateField(ogr.FieldDefn('str', ogr.OFTString))
        layer.CreateField(ogr.FieldDefn('int', ogr.OFTInteger))
        layer.CreateField(ogr.FieldDefn('dbl', ogr.OFTReal))

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetField('str', 'str')
        feat.SetField('int', 1)
        feat.SetField('dbl', 2.34)
        if geom is not None:
          feat.SetGeometry(geom)
        layer.CreateFeature(feat)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat['str'], 'str')
        self.assertEqual(feat['int'], 1)
        self.assertEqual(feat['dbl'], 2.34)
        if wkt:
          self.CheckFeatureGeometry(feat, wkt)
        else:
          self.assertIsNone(feat.GetGeometryRef())

  @parameterized.parameters('wfs10', 'wfs11', 'wfs20')
  def test_ogr_gml_validate_wfs(self, stem: str) -> None:
    # Note: ValidateSchema() requires HAVE_LIBXML2 + OGC schemas zip; verify opening.
    tempdir = self._copy_testdata([f'gml/{stem}.xml', f'gml/{stem}.xsd'])
    with ogr.Open(os.path.join(tempdir, f'{stem}.xml')) as ds:
      self.assertGreaterEqual(ds.GetLayerCount(), 1)
      with gdal.quiet_errors(), self.assertRaisesRegex(
          RuntimeError, 'not implemented due to missing libxml2 support'
      ):
        ds.ExecuteSQL('SELECT ValidateSchema()')

  def test_ogr_gml_48(self) -> None:
    tempdir = self._copy_testdata([
        'gml/schema_with_geom_in_complextype.xml',
        'gml/schema_with_geom_in_complextype.xsd',
    ])
    with ogr.Open(
        os.path.join(tempdir, 'schema_with_geom_in_complextype.xml')
    ) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetGeomType(), ogr.wkbUnknown)
      self.assertEqual(layer.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString)

  def test_ogr_gml_49(self) -> None:
    filepath = '/vsimem/ogr_gml_49.gml'
    gml_content = """\
<ogr:FeatureCollection xmlns:gml="http://www.opengis.net/gml" xmlns:ogr="http://ogr.maptools.org/">
  <gml:featureMember>
    <ogr:test>
      <ogr:geometry><gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>2,49 2,50 3,50 3,49 2,49</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon></ogr:geometry>
      <ogr:otherGeometry><gml:Point><gml:pos>-2 -49</gml:pos></gml:Point></ogr:otherGeometry>
    </ogr:test>
  </gml:featureMember>
</ogr:FeatureCollection>
"""
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_49.gfs'),
    ):
      gdal.FileFromMemBuffer(filepath, gml_content)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().GetGeometryType(), ogr.wkbPolygon
        )

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().GetGeometryType(), ogr.wkbPolygon
        )

  def test_ogr_gml_50(self) -> None:
    filepath = '/vsimem/ogr_gml_50.gml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_50.xsd'),
    ):
      with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
        layer = ds.CreateLayer('listlayer')
        layer.CreateField(ogr.FieldDefn('stringlist', ogr.OFTStringList))
        layer.CreateField(ogr.FieldDefn('intlist', ogr.OFTIntegerList))
        layer.CreateField(ogr.FieldDefn('reallist', ogr.OFTRealList))
        feat = ogr.Feature(feature_def=layer.GetLayerDefn())
        feat.SetFieldStringList(0, ['a', 'b'])
        feat.SetFieldIntegerList(1, [2, 3])
        feat.SetFieldDoubleList(2, [4.56, 5.67])
        layer.CreateFeature(feat)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        layer_defn = layer.GetLayerDefn()
        self.assertEqual(
            feat.GetFieldAsStringList(layer_defn.GetFieldIndex('stringlist')),
            ['a', 'b'],
        )
        self.assertEqual(
            feat.GetFieldAsIntegerList(layer_defn.GetFieldIndex('intlist')),
            [2, 3],
        )
        self.assertEqual(
            feat.GetFieldAsDoubleList(layer_defn.GetFieldIndex('reallist')),
            [4.56, 5.67],
        )

  @parameterized.parameters(False, True)
  def test_ogr_gml_read_FeaturePropertyList_in_gfs(
      self, use_gfs_template: bool
  ) -> None:
    files = ['gml/testfeaturepropertylist.gml']
    if not use_gfs_template:
      files.append('gml/testfeaturepropertylist.gfs')
    tempdir = self._copy_testdata(files)
    gml_path = os.path.join(tempdir, 'testfeaturepropertylist.gml')
    if use_gfs_template:
      open_options = [
          'GFS_TEMPLATE='
          + ogr_util.GetTestFilePath('gml/testfeaturepropertylist.gfs')
      ]
    else:
      self._touch_newer(
          os.path.join(tempdir, 'testfeaturepropertylist.gfs'), gml_path
      )
      open_options = []

    with gdal.OpenEx(gml_path, open_options=open_options) as ds:
      self.assertEqual(ds.GetLayerCount(), 2)
      layer0 = ds.GetLayer(0)
      self.assertEqual(layer0.GetFeatureCount(), 1)
      feat = layer0.GetNextFeature()
      self.assertEqual(feat['gml_id'], 'FA')
      self.assertEqual(feat['name'], 'Feature A')
      self.assertEqual(feat['roleInline_href'], ['#FB1_1', '#FB1_2'])

      layer1 = ds.GetLayer(1)
      self.assertEqual(layer1.GetFeatureCount(), 3)
      feat = layer1.GetNextFeature()
      self.assertEqual(feat['gml_id'], 'FB1_1')
      self.assertEqual(feat['name'], 'Feature B 1_1')
      self.assertEqual(layer1.GetNextFeature()['gml_id'], 'FB1_2')
      self.assertEqual(layer1.GetNextFeature()['gml_id'], 'FB2')

  @parameterized.parameters(False, True)
  def test_ogr_gml_read_FeatureProperty_in_gfs(
      self, use_gfs_template: bool
  ) -> None:
    files = ['gml/testfeaturepropertylist2.xml']
    if not use_gfs_template:
      files.append('gml/testfeaturepropertylist2.gfs')
    tempdir = self._copy_testdata(files)
    xml_path = os.path.join(tempdir, 'testfeaturepropertylist2.xml')
    if use_gfs_template:
      open_options = [
          'GFS_TEMPLATE='
          + ogr_util.GetTestFilePath('gml/testfeaturepropertylist2.gfs')
      ]
    else:
      self._touch_newer(
          os.path.join(tempdir, 'testfeaturepropertylist2.gfs'), xml_path
      )
      open_options = []

    with gdal.OpenEx(xml_path, open_options=open_options) as ds:
      self.assertEqual(ds.GetLayerCount(), 3)
      layer0 = ds.GetLayer(0)
      self.assertEqual(layer0.GetFeatureCount(), 1)
      feat = layer0.GetNextFeature()
      self.assertEqual(feat['gml_id'], 'FT1_1')
      self.assertEqual(feat['roleFt2_href'], ['#FT2_1', '#FT2_2'])
      self.assertEqual(feat['roleOt1_href'], ['#OT1_1', '#OT1_2', '#OT1_3'])

      layer1 = ds.GetLayer(1)
      self.assertEqual(layer1.GetFeatureCount(), 2)
      self.assertEqual(layer1.GetNextFeature()['gml_id'], 'FT2_1')
      self.assertEqual(layer1.GetNextFeature()['gml_id'], 'FT2_2')

      layer2 = ds.GetLayer(2)
      self.assertEqual(layer2.GetFeatureCount(), 4)
      for expected_id in ('OT1_1', 'OT1_2', 'OT1_3', 'OT1_4'):
        self.assertEqual(layer2.GetNextFeature()['gml_id'], expected_id)

  @parameterized.parameters('GML2', 'GML3')
  def test_ogr_gml_51(self, frmt: str) -> None:
    tempdir = self.create_tempdir().full_path
    out_gml = os.path.join(tempdir, 'ogr_gml_51.gml')
    src_shp = ogr_util.GetTestFilePath('shape/poly/poly.shp')
    gdal.VectorTranslate(
        out_gml,
        src_shp,
        format='GML',
        datasetCreationOptions=[
            f'FORMAT={frmt}',
            'WRITE_FEATURE_BOUNDED_BY=no',
            'STRIP_PREFIX=YES',
        ],
    )
    with open(out_gml, 'rt', encoding='utf-8') as f:
      content = f.read()
    self.assertIn('<FeatureCollection', content)
    if frmt == 'GML3':
      self.assertIn('<featureMember>', content)
    self.assertIn('<poly', content)
    self.assertIn('<AREA>215229.266</AREA>', content)
    self.assertNotIn(
        '<gml:boundedBy><gml:Envelope><gml:lowerCorner>479647', content
    )

    with ogr.Open(out_gml) as ds:
      self.assertIsNotNone(ds.GetLayer(0).GetNextFeature())

  def test_ogr_gml_52(self) -> None:
    tempdir = self._copy_testdata(['gml/fake_mtkgml.xml'])
    xml_path = os.path.join(tempdir, 'fake_mtkgml.xml')
    for _ in range(2):
      with ogr.Open(xml_path) as ds:
        layer_a = ds.GetLayerByName('A')
        self.assertEqual(layer_a.GetGeomType(), ogr.wkbPoint25D)
        srs = layer_a.GetSpatialRef()
        self.assertIsNotNone(srs)
        self.assertIn('3067', srs.ExportToWkt())
        self.assertEqual(layer_a.GetExtent(), (280000, 280000, 7000000, 7000000))

        feat = layer_a.GetNextFeature()
        self.assertEqual(feat.GetField('gid'), '1')
        self.assertEqual(feat.GetField('regular_attribute'), 5)
        self.assertEqual(feat.GetField('foo_href'), 'some_ref')
        self.assertEqual(feat.GetField('teksti'), 'En francais !')
        self.assertEqual(feat.GetField('teksti_kieli'), 'fr')
        self.CheckFeatureGeometry(feat, 'POINT (280000 7000000 0)')

        layer_b = ds.GetLayerByName('B')
        self.assertEqual(layer_b.GetGeomType(), ogr.wkbPolygon25D)
        self.assertIsNotNone(layer_b.GetSpatialRef())
        self.CheckFeatureGeometry(
            layer_b.GetNextFeature(),
            'POLYGON ((280000 7000000 0,281000 7000000 0,281000 7001000 0,'
            '280000 7001000 0,280000 7000000 0))',
        )

        layer_c = ds.GetLayerByName('C')
        self.assertEqual(layer_c.GetGeomType(), ogr.wkbLineString25D)
        self.CheckFeatureGeometry(
            layer_c.GetNextFeature(),
            'LINESTRING (280000 7000000 0,281000 7000000 0,281000 7001000 0,'
            '280000 7001000 0,280000 7000000 0)',
        )

  def test_ogr_gml_53(self) -> None:
    xsd_path = ogr_util.GetTestFilePath('gml/archsites.xsd')
    with ogr.ExceptionMgr(useExceptions=False):
      self.assertIsNone(ogr.Open(xsd_path))

  def test_ogr_gml_54(self) -> None:
    tempdir = self._copy_testdata(['gml/empty.gml'])
    empty_path = os.path.join(tempdir, 'empty.gml')
    # First without .gfs, then with .gfs.
    for _ in range(2):
      with ogr.Open(empty_path) as ds:
        self.assertIsNotNone(ds)

  def test_ogr_gml_55(self) -> None:
    tempdir = self._copy_testdata([
        'gml/ogr_gml_55.gml',
        'gml/ogr_gml_55.xsd',
        'gml/ogr_gml_55_included1.xsd',
        'gml/ogr_gml_55_included2.xsd',
    ])
    gml_path = os.path.join(tempdir, 'ogr_gml_55.gml')
    with ogr.Open(gml_path) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(
          layer.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
      )
    self.assertFalse(os.path.exists(os.path.join(tempdir, 'ogr_gml_55.gfs')))

  def test_ogr_gml_56(self) -> None:
    tempdir = self._copy_testdata(['gml/ogr_gml_56.gml'])
    gml_path = os.path.join(tempdir, 'ogr_gml_56.gml')
    registry_path = ogr_util.GetTestFilePath('gml/ogr_gml_56_registry.xml')

    with gdal.config_option('GML_REGISTRY', registry_path):
      with ogr.Open(gml_path) as ds:
        main_layer = ds.GetLayerByName('mainFeature')
        self.assertIsNotNone(main_layer.GetSpatialRef())
        feat = main_layer.GetNextFeature()
        self.assertEqual(
            feat.GetFieldAsString(
                feat.GetFieldIndex('subFeatureProperty_href')
            ),
            '#subFeature.0',
        )
        self.assertEqual(
            feat.GetFieldAsStringList(
                feat.GetFieldIndex('subFeatureRepeatedProperty_href')
            ),
            ['#subFeatureRepeated.0', '#subFeatureRepeated.1'],
        )
        self.assertEqual(
            feat.GetGeomFieldRef(0).ExportToWkt(),
            'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
        )
        self.assertEqual(feat.GetGeomFieldRef(1).ExportToWkt(), 'POINT (10 10)')

        sub_layer = ds.GetLayerByName('subFeature')
        self.assertEqual(sub_layer.GetLayerDefn().GetGeomFieldCount(), 0)
        feat = sub_layer.GetNextFeature()
        self.assertEqual(
            feat.GetFieldAsStringList(
                feat.GetFieldIndex('subFeatureRepeatedProperty_href')
            ),
            ['#subFeatureRepeated.2'],
        )
        self.assertEqual(feat.GetField('foo'), 'bar')

        rep_layer = ds.GetLayerByName('subFeatureRepeated')
        feat = rep_layer.GetNextFeature()
        self.assertEqual(feat.GetField('gml_id'), 'subFeatureRepeated.2')
        self.assertEqual(feat.GetField('bar'), 'baz')
        self.assertEqual(
            rep_layer.GetNextFeature().GetField('gml_id'),
            'subFeatureRepeated.0',
        )
        self.assertEqual(
            rep_layer.GetNextFeature().GetField('gml_id'),
            'subFeatureRepeated.1',
        )

    self.assertFalse(os.path.exists(os.path.join(tempdir, 'ogr_gml_56.gfs')))

  @parameterized.named_parameters(
      dict(
          testcase_name='no_srs',
          options=[],
          first_geom_epsg=None,
          second_geom_epsg=None,
          expected_first_srs_code=None,
          expected_second_srs_code=None,
      ),
      dict(
          testcase_name='same_srs',
          options=[],
          first_geom_epsg=32630,
          second_geom_epsg=32630,
          expected_first_srs_code='32630',
          expected_second_srs_code='32630',
      ),
      dict(
          testcase_name='different_srs',
          options=[],
          first_geom_epsg=32630,
          second_geom_epsg=32631,
          expected_first_srs_code=None,
          expected_second_srs_code='32631',
      ),
      dict(
          testcase_name='gml32_no_srs',
          options=['FORMAT=GML3.2'],
          first_geom_epsg=None,
          second_geom_epsg=None,
          expected_first_srs_code=None,
          expected_second_srs_code=None,
      ),
  )
  def test_ogr_gml_57(
      self,
      options: Sequence[str],
      first_geom_epsg: int | None,
      second_geom_epsg: int | None,
      expected_first_srs_code: str | None,
      expected_second_srs_code: str | None,
  ) -> None:
    filepath = '/vsimem/ogr_gml_57.gml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_57.xsd'),
    ):
      with ogr.GetDriverByName('GML').CreateDataSource(
          filepath, options=options
      ) as ds:
        self.assertEqual(
            ds.TestCapability(ogr.ODsCCreateGeomFieldAfterCreateLayer), 1
        )
        layer = ds.CreateLayer('myLayer', geom_type=ogr.wkbNone)
        self.assertEqual(layer.TestCapability(ogr.OLCCreateGeomField), 1)

        first_geom_defn = ogr.GeomFieldDefn('first_geometry', ogr.wkbPoint)
        if first_geom_epsg is not None:
          sr = osr.SpatialReference()
          sr.ImportFromEPSG(first_geom_epsg)
          first_geom_defn.SetSpatialRef(sr)
        layer.CreateGeomField(first_geom_defn)

        second_geom_defn = ogr.GeomFieldDefn(
            'second_geometry', ogr.wkbLineString
        )
        if second_geom_epsg is not None:
          sr = osr.SpatialReference()
          sr.ImportFromEPSG(second_geom_epsg)
          second_geom_defn.SetSpatialRef(sr)
        layer.CreateGeomField(second_geom_defn)

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetGeomFieldDirectly(0, ogr.CreateGeometryFromWkt('POINT (0 1)'))
        feat.SetGeomFieldDirectly(
            1, ogr.CreateGeometryFromWkt('LINESTRING (2 3,4 5)')
        )
        layer.CreateFeature(feat)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        if expected_first_srs_code is not None:
          self.assertIn(
              expected_first_srs_code,
              feat.GetGeomFieldRef(0).GetSpatialReference().ExportToWkt(),
          )
        if expected_second_srs_code is not None:
          self.assertIn(
              expected_second_srs_code,
              feat.GetGeomFieldRef(1).GetSpatialReference().ExportToWkt(),
          )
        self.assertEqual(feat.GetGeomFieldRef(0).ExportToWkt(), 'POINT (0 1)')
        self.assertEqual(
            feat.GetGeomFieldRef(1).ExportToWkt(),
            'LINESTRING (2 3,4 5)',
        )

  def test_ogr_gml_58(self) -> None:
    tempdir = self._copy_testdata(['gml/inspire_cadastralparcel.xml'])
    with ogr.Open(os.path.join(tempdir, 'inspire_cadastralparcel.xml')) as ds:
      layer = ds.GetLayer(0)
      layer_defn = layer.GetLayerDefn()
      self.assertEqual(layer_defn.GetGeomFieldCount(), 2)
      self.assertEqual(layer_defn.GetGeomFieldDefn(0).GetName(), 'geometry')
      self.assertEqual(
          layer_defn.GetGeomFieldDefn(0).GetType(), ogr.wkbMultiPolygon
      )
      self.assertEqual(
          layer_defn.GetGeomFieldDefn(1).GetName(), 'referencePoint'
      )
      self.assertEqual(layer_defn.GetGeomFieldDefn(1).GetType(), ogr.wkbPoint)

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'CadastralParcel-01'),
          ('areaValue', 10.0),
          ('areaValue_uom', 'm2'),
          ('beginLifespanVersion', '2000-01-01T00:00:00.0Z'),
          ('endLifespanVersion', '2001-01-01T00:00:00.0Z'),
          ('inspireId_localId', 'CadastralParcel-01-localId'),
          ('inspireId_namespace', 'namespace'),
          ('label', 'label'),
          ('nationalCadastralReference', 'nationalCadastralReference'),
          ('validFrom', '2002-01-01T00:00:00.0Z'),
          ('validTo', '2003-01-01T00:00:00.0Z'),
          ('basicPropertyUnit_href', ['#BPU.1', '#BPU.2']),
          ('administrativeUnit_href', '#AU.1'),
          ('zoning_href', '#CZ.1'),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)
      self.assertEqual(
          feat.GetGeomFieldRef(0).ExportToWkt(),
          'MULTIPOLYGON (((2 49,2 50,3 50,3 49)))',
      )
      self.assertEqual(feat.GetGeomFieldRef(1).ExportToWkt(), 'POINT (2.5 49.5)')

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'CadastralParcel-02'),
          ('areaValue', None),
          ('areaValue_uom', None),
          ('beginLifespanVersion', '2000-01-01T00:00:00.0Z'),
          ('endLifespanVersion', None),
          ('inspireId_localId', 'CadastralParcel-02-localId'),
          ('inspireId_namespace', 'namespace'),
          ('label', 'label'),
          ('nationalCadastralReference', 'nationalCadastralReference'),
          ('validFrom', None),
          ('validTo', None),
          ('basicPropertyUnit_href', None),
          ('administrativeUnit_href', None),
          ('zoning_href', None),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)
      self.assertEqual(
          feat.GetGeomFieldRef(0).ExportToWkt(),
          'MULTIPOLYGON (((2 49,2 50,3 50,3 49)))',
      )
      self.assertIsNone(feat.GetGeomFieldRef(1))

  def test_ogr_gml_58a(self) -> None:
    tempdir = self._copy_testdata(['gml/inspire_basicpropertyunit.xml'])
    with ogr.Open(os.path.join(tempdir, 'inspire_basicpropertyunit.xml')) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetLayerDefn().GetGeomFieldCount(), 0)

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'BasicPropertyUnit-01'),
          ('inspireId_localId', 'BasicPropertyUnit-01-localId'),
          ('inspireId_namespace', 'namespace'),
          ('nationalCadastralReference', 'nationalCadastralReference'),
          ('areaValue', 10.0),
          ('areaValue_uom', 'm2'),
          ('validFrom', '2000-01-01T00:00:00.0Z'),
          ('validTo', '2001-01-01T00:00:00.0Z'),
          ('beginLifespanVersion', '2002-01-01T00:00:00.0Z'),
          ('endLifespanVersion', '2003-01-01T00:00:00.0Z'),
          ('administrativeUnit_href', '#AU.1'),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'BasicPropertyUnit-02'),
          ('inspireId_localId', 'BasicPropertyUnit-02-localId'),
          ('inspireId_namespace', 'namespace'),
          ('nationalCadastralReference', 'nationalCadastralReference'),
          ('areaValue', None),
          ('areaValue_uom', None),
          ('validFrom', '2000-01-01T00:00:00.0Z'),
          ('validTo', None),
          ('beginLifespanVersion', '2002-01-01T00:00:00.0Z'),
          ('endLifespanVersion', None),
          ('administrativeUnit_href', None),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)

  def test_ogr_gml_58b(self) -> None:
    tempdir = self._copy_testdata(['gml/inspire_cadastralboundary.xml'])
    with ogr.Open(os.path.join(tempdir, 'inspire_cadastralboundary.xml')) as ds:
      layer = ds.GetLayer(0)
      layer_defn = layer.GetLayerDefn()
      self.assertEqual(layer_defn.GetGeomFieldCount(), 1)
      self.assertEqual(layer_defn.GetGeomFieldDefn(0).GetName(), 'geometry')
      self.assertEqual(
          layer_defn.GetGeomFieldDefn(0).GetType(), ogr.wkbLineString
      )

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'CadastralBoundary-01'),
          ('beginLifespanVersion', '2000-01-01T00:00:00.0Z'),
          ('endLifespanVersion', '2001-01-01T00:00:00.0Z'),
          ('estimatedAccuracy', 1.0),
          ('estimatedAccuracy_uom', 'm'),
          ('inspireId_localId', 'CadastralBoundary-01-localId'),
          ('inspireId_namespace', 'namespace'),
          ('validFrom', '2002-01-01T00:00:00.0Z'),
          ('validTo', '2003-01-01T00:00:00.0Z'),
          ('parcel_href', ['#Parcel.1', '#Parcel.2']),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)
      self.assertEqual(
          feat.GetGeomFieldRef(0).ExportToWkt(), 'LINESTRING (2 49,3 50)'
      )

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'CadastralBoundary-02'),
          ('beginLifespanVersion', '2000-01-01T00:00:00.0Z'),
          ('endLifespanVersion', None),
          ('estimatedAccuracy', None),
          ('estimatedAccuracy_uom', None),
          ('inspireId_localId', 'CadastralBoundary-02-localId'),
          ('inspireId_namespace', 'namespace'),
          ('validFrom', None),
          ('validTo', None),
          ('parcel_href', None),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)
      self.assertEqual(
          feat.GetGeomFieldRef(0).ExportToWkt(), 'LINESTRING (2 49,3 50)'
      )

  def test_ogr_gml_58c(self) -> None:
    tempdir = self._copy_testdata(['gml/inspire_cadastralzoning.xml'])
    with ogr.Open(os.path.join(tempdir, 'inspire_cadastralzoning.xml')) as ds:
      layer = ds.GetLayer(0)
      layer_defn = layer.GetLayerDefn()
      self.assertEqual(layer_defn.GetGeomFieldCount(), 2)
      self.assertEqual(layer_defn.GetGeomFieldDefn(0).GetName(), 'geometry')
      self.assertEqual(
          layer_defn.GetGeomFieldDefn(0).GetType(), ogr.wkbMultiPolygon
      )
      self.assertEqual(
          layer_defn.GetGeomFieldDefn(1).GetName(), 'referencePoint'
      )
      self.assertEqual(layer_defn.GetGeomFieldDefn(1).GetType(), ogr.wkbPoint)

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'CadastralZoning-01'),
          ('beginLifespanVersion', '2000-01-01T00:00:00.0Z'),
          ('endLifespanVersion', '2001-01-01T00:00:00.0Z'),
          ('estimatedAccuracy', 1.0),
          ('estimatedAccuracy_uom', 'm'),
          ('inspireId_localId', 'CadastralZoning-01-localId'),
          ('inspireId_namespace', 'namespace'),
          ('label', 'label'),
          ('level', '3'),
          ('levelName', ['English', 'Francais', 'Deutsch']),
          ('levelName_locale', ['en', 'fr', 'de']),
          ('name_language', ['language']),
          ('name_nativeness', ['nativeness']),
          ('name_nameStatus', ['nameStatus']),
          ('name_pronunciation', None),
          ('name_spelling_text', ['text']),
          ('name_spelling_script', ['script']),
          ('nationalCadastalZoningReference', 'nationalCadastalZoningReference'),
          ('validFrom', '2002-01-01T00:00:00.0Z'),
          ('validTo', '2003-01-01T00:00:00.0Z'),
          ('upperLevelUnit_href', '#ulu.1'),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)
      self.assertEqual(
          feat.GetGeomFieldRef(0).ExportToWkt(),
          'MULTIPOLYGON (((2 49,2 50,3 50,3 49)))',
      )
      self.assertEqual(feat.GetGeomFieldRef(1).ExportToWkt(), 'POINT (2.5 49.5)')

      feat = layer.GetNextFeature()
      expected = [
          ('gml_id', 'CadastralZoning-02'),
          ('beginLifespanVersion', '2000-01-01T00:00:00.0Z'),
          ('endLifespanVersion', None),
          ('estimatedAccuracy', None),
          ('estimatedAccuracy_uom', None),
          ('inspireId_localId', None),
          ('inspireId_namespace', None),
          ('label', 'label'),
          ('level', '3'),
          ('levelName', ['English']),
          ('levelName_locale', ['en']),
          ('name_language', None),
          ('name_nativeness', None),
          ('name_nameStatus', None),
          ('name_pronunciation', None),
          ('name_spelling_text', None),
          ('name_spelling_script', None),
          ('nationalCadastalZoningReference', 'nationalCadastalZoningReference'),
          ('validFrom', None),
          ('validTo', None),
          ('upperLevelUnit_href', None),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)
      self.assertEqual(
          feat.GetGeomFieldRef(0).ExportToWkt(),
          'MULTIPOLYGON (((2 49,2 50,3 50,3 49)))',
      )
      self.assertIsNone(feat.GetGeomFieldRef(1))

  def test_ogr_gml_59(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/testcondition.gml', 'gml/testcondition.gfs']
    )
    gml_path = os.path.join(tempdir, 'testcondition.gml')
    gfs_path = os.path.join(tempdir, 'testcondition.gfs')
    self._touch_newer(gfs_path, gml_path)

    with ogr.Open(gml_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      expected = [
          ('name_en', 'English name'),
          ('name_fr', 'Nom francais'),
          ('name_others_lang', ['de']),
          ('name_others', ['Deutsche name']),
      ]
      for key, val in expected:
        self.assertEqual(feat.GetField(key), val)

  def test_ogr_gml_60(self) -> None:
    tempdir = self._copy_testdata(['gml/wfs_200_multiplelayers.gml'])
    gml_path = os.path.join(tempdir, 'wfs_200_multiplelayers.gml')
    for _ in range(2):
      with ogr.Open(gml_path) as ds:
        road_layer = ds.GetLayerByName('road')
        self.assertEqual(road_layer.GetFeatureCount(), 1)
        self.assertEqual(road_layer.GetNextFeature().GetField('gml_id'), 'road.21')

        pop_layer = ds.GetLayerByName('popplace')
        self.assertEqual(pop_layer.GetFeatureCount(), 1)
        self.assertEqual(
            pop_layer.GetNextFeature().GetField('gml_id'), 'popplace.BACMK'
        )

  def test_ogr_gml_61(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/gmlsubfeature.gml', 'gml/gmlsubfeature.gfs']
    )
    gml_path = os.path.join(tempdir, 'gmlsubfeature.gml')
    gfs_path = os.path.join(tempdir, 'gmlsubfeature.gfs')
    self._touch_newer(gfs_path, gml_path)

    with ogr.Open(gml_path) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetFeatureCount(), 2)

      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('gml_id'), 'Object.1')
      self.assertEqual(feat.GetField('foo'), 'bar')
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          'POLYGON ((2 48,2 49,3 49,3 48,2 48))',
      )

      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetField('gml_id'), 'Object.2')
      self.assertEqual(feat.GetField('foo'), 'baz')
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          'POLYGON ((2 -48,2 -49,3 -49,3 -48,2 -48))',
      )

  def test_ogr_gml_62(self) -> None:
    tempdir = self._copy_testdata(['gml/gmlattributes.gml'])
    with ogr.Open(os.path.join(tempdir, 'gmlattributes.gml')) as ds:
      self.assertEqual(ds.GetLayer(0).GetLayerDefn().GetFieldCount(), 1)

  def test_ogr_gml_62bis(self) -> None:
    tempdir = self._copy_testdata(['gml/gmlattributes.gml'])
    gml_path = os.path.join(tempdir, 'gmlattributes.gml')
    # First run creates .gfs under GML_ATTRIBUTES_TO_OGR_FIELDS=YES; second uses .gfs.
    for iteration in range(2):
      if iteration == 0:
        with gdal.config_option('GML_ATTRIBUTES_TO_OGR_FIELDS', 'YES'):
          ds = ogr.Open(gml_path)
      else:
        ds = ogr.Open(gml_path)
      with ds:
        layer = ds.GetLayer(0)
        self.assertEqual(layer.GetLayerDefn().GetFieldCount(), 4)
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetField('element_attr1'), '1')
        self.assertEqual(feat.GetField('element2_attr1'), 'a')
        self.assertEqual(feat.GetField('element2'), 'foo')
        self.assertFalse(feat.IsFieldSet('element3_attr1'))

        feat = layer.GetNextFeature()
        self.assertFalse(feat.IsFieldSet('element_attr1'))
        self.assertFalse(feat.IsFieldSet('element2_attr1'))
        self.assertFalse(feat.IsFieldSet('element2'))
        self.assertEqual(feat.GetField('element3_attr1'), 1)

        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetField('element_attr1'), 'a')
        self.assertFalse(feat.IsFieldSet('element2_attr1'))
        self.assertFalse(feat.IsFieldSet('element2'))
        self.assertFalse(feat.IsFieldSet('element3_attr1'))

  def test_ogr_gml_63(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/ruian_st_v1.xml.gz', 'gml/ruian_ob_v1.xml.gz']
    )
    with ogr.Open(os.path.join(tempdir, 'ruian_st_v1.xml.gz')) as ds:
      self.assertEqual(ds.GetLayerCount(), 14)
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetName(), 'Staty')
      self.assertEqual(layer.GetGeometryColumn(), 'DefinicniBod')

    with ogr.Open(os.path.join(tempdir, 'ruian_ob_v1.xml.gz')) as ds:
      num_layers = ds.GetLayerCount()
      self.assertEqual(num_layers, 11)
      num_features = sum(
          ds.GetLayer(i).GetFeatureCount() for i in range(num_layers)
      )
      self.assertEqual(num_features, 7)

  def test_ogr_gml_64(self) -> None:
    # Note: XERCES parser is not built in Google3; test EXPAT parser.
    tempdir = self._copy_testdata(['gml/rnf_eg.gml'])
    with gdal.config_option('GML_PARSER', 'EXPAT'):
      with ogr.Open(os.path.join(tempdir, 'rnf_eg.gml')) as ds:
        self.assertIsNotNone(ds.GetLayer(0).GetNextFeature())

  def test_ogr_gml_billion_laugh(self) -> None:
    # Note: XERCES parser is not built in Google3; test EXPAT parser.
    gml_path = ogr_util.GetTestFilePath('gml/billionlaugh.gml')
    with (
        gdal.config_option('GML_PARSER', 'EXPAT'),
        self.assertRaisesRegex(RuntimeError, 'File probably corrupted'),
    ):
      with gdal.OpenEx(gml_path) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), 'GML')
        for layer in ds:
          for _ in layer:
            pass

  @parameterized.named_parameters(
      dict(
          testcase_name='geometry',
          option='SRSDIMENSION_LOC=GEOMETRY',
          expected=(
              '<ogr:geometryProperty><gml:MultiSurface'
              ' srsDimension="3"><gml:surfaceMember><gml:Polygon>'
              '<gml:exterior><gml:LinearRing><gml:posList>'
              '0 1 2 3 4 5 6 7 8 0 1 2</gml:posList>'
              '</gml:LinearRing></gml:exterior></gml:Polygon>'
              '</gml:surfaceMember></gml:MultiSurface>'
              '</ogr:geometryProperty>'
          ),
      ),
      dict(
          testcase_name='poslist',
          option='SRSDIMENSION_LOC=POSLIST',
          expected=(
              '<ogr:geometryProperty><gml:MultiSurface>'
              '<gml:surfaceMember><gml:Polygon><gml:exterior>'
              '<gml:LinearRing><gml:posList srsDimension="3">'
              '0 1 2 3 4 5 6 7 8 0 1 2</gml:posList>'
              '</gml:LinearRing></gml:exterior></gml:Polygon>'
              '</gml:surfaceMember></gml:MultiSurface>'
              '</ogr:geometryProperty>'
          ),
      ),
      dict(
          testcase_name='geometry_poslist',
          option='SRSDIMENSION_LOC=GEOMETRY,POSLIST',
          expected=(
              '<ogr:geometryProperty><gml:MultiSurface'
              ' srsDimension="3"><gml:surfaceMember><gml:Polygon>'
              '<gml:exterior><gml:LinearRing><gml:posList'
              ' srsDimension="3">0 1 2 3 4 5 6 7 8 0 1 2</gml:posList>'
              '</gml:LinearRing></gml:exterior></gml:Polygon>'
              '</gml:surfaceMember></gml:MultiSurface>'
              '</ogr:geometryProperty>'
          ),
      ),
  )
  def test_ogr_gml_65(self, option: str, expected: str) -> None:
    filepath = '/vsimem/ogr_gml_65.gml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_65.xsd'),
    ):
      with ogr.GetDriverByName('GML').CreateDataSource(
          filepath, options=['FORMAT=GML3', option]
      ) as ds:
        layer = ds.CreateLayer('lyr')
        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'MULTIPOLYGON (((0 1 2,3 4 5,6 7 8,0 1 2)))'
            )
        )
        layer.CreateFeature(feat)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn(expected, data)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(),
            'MULTIPOLYGON (((0 1 2,3 4 5,6 7 8,0 1 2)))',
        )

  @parameterized.named_parameters(
      dict(
          testcase_name='compoundcurve',
          layer_name='compoundcurve',
          geom_type=ogr.wkbCompoundCurve,
          input_wkts=[
              'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0))',
              'COMPOUNDCURVE ((0 0,1 1,2 0))',
          ],
          expected_wkts=[
              'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0))',
              'COMPOUNDCURVE ((0 0,1 1,2 0))',
          ],
      ),
      dict(
          testcase_name='curvepolygon',
          layer_name='curvepolygon',
          geom_type=ogr.wkbCurvePolygon,
          input_wkts=[
              'CURVEPOLYGON (COMPOUNDCURVE (CIRCULARSTRING (0 0,1 0,0 0)))',
              'POLYGON ((0 0,0 1,1 1,0 0))',
          ],
          expected_wkts=[
              'CURVEPOLYGON (CIRCULARSTRING (0 0,0.5 0.5,1 0,0.5 -0.5,0 0))',
              'CURVEPOLYGON ((0 0,0 1,1 1,0 0))',
          ],
      ),
      dict(
          testcase_name='multicurve',
          layer_name='multicurve',
          geom_type=ogr.wkbMultiCurve,
          input_wkts=[
              'MULTICURVE (CIRCULARSTRING (0 0,0.5 0.5,1 0,0.5 -0.5,0 0))',
              'MULTICURVE ((0 0,0 1,1 1,0 0))',
          ],
          expected_wkts=[
              'MULTICURVE (CIRCULARSTRING (0 0,0.5 0.5,1 0,0.5 -0.5,0 0))',
              'MULTICURVE ((0 0,0 1,1 1,0 0))',
          ],
      ),
      dict(
          testcase_name='multisurface',
          layer_name='multisurface',
          geom_type=ogr.wkbMultiSurface,
          input_wkts=[
              (
                  'MULTISURFACE (CURVEPOLYGON (COMPOUNDCURVE ('
                  'CIRCULARSTRING (0 0,1 0,0 0))))'
              ),
              'MULTISURFACE (((0 0,0 1,1 1,0 0)))',
          ],
          expected_wkts=[
              (
                  'MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0,0.5 0.5,'
                  '1 0,0.5 -0.5,0 0)))'
              ),
              'MULTISURFACE (((0 0,0 1,1 1,0 0)))',
          ],
      ),
      dict(
          testcase_name='circularstring',
          layer_name='circularstring',
          geom_type=ogr.wkbCircularString,
          input_wkts=[
              'CIRCULARSTRING (0 0,1 1,2 0)',
          ],
          expected_wkts=[
              'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0))',
          ],
      ),
      dict(
          testcase_name='compoundcurve25d',
          layer_name='compoundcurvez',
          geom_type=ogr.wkbCompoundCurveZ,
          input_wkts=[
              'COMPOUNDCURVE Z (CIRCULARSTRING Z (0 0 10,1 1 20,2 0 30))',
          ],
          expected_wkts=[
              'COMPOUNDCURVE Z (CIRCULARSTRING Z (0 0 10,1 1 20,2 0 30))',
          ],
      ),
  )
  def test_ogr_gml_66(
      self,
      layer_name: str,
      geom_type: int,
      input_wkts: Sequence[str],
      expected_wkts: Sequence[str],
  ) -> None:
    filepath = f'/vsimem/ogr_gml_66_{layer_name}.gml'
    with gcore_util.GdalUnlinkWhenDone(filepath):
      with ogr.GetDriverByName('GML').CreateDataSource(
          filepath, options=['FORMAT=GML3']
      ) as ds:
        layer = ds.CreateLayer(layer_name, geom_type=geom_type)
        for wkt in input_wkts:
          feat = ogr.Feature(layer.GetLayerDefn())
          feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
          layer.CreateFeature(feat)
          feat = None

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        for idx, expected_wkt in enumerate(expected_wkts):
          feat = layer.GetNextFeature()
          self.assertIsNotNone(
              feat,
              f'feature {idx} of {layer_name}',
          )
          self.assertEqual(
              feat.GetGeometryRef().ExportToWkt(),
              expected_wkt,
              f'feature {idx} of {layer_name}',
          )

  def test_ogr_gml_67(self) -> None:
    filepath = '/vsimem/ogr_gml_67.gml'
    with gcore_util.GdalUnlinkWhenDone(filepath):
      with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
        layer = ds.CreateLayer('test')
        b1_fld_defn = ogr.FieldDefn('b1', ogr.OFTInteger)
        b1_fld_defn.SetSubType(ogr.OFSTBoolean)
        layer.CreateField(b1_fld_defn)
        b2_fld_defn = ogr.FieldDefn('b2', ogr.OFTInteger)
        b2_fld_defn.SetSubType(ogr.OFSTBoolean)
        layer.CreateField(b2_fld_defn)
        bool_list_fld_defn = ogr.FieldDefn('bool_list', ogr.OFTIntegerList)
        bool_list_fld_defn.SetSubType(ogr.OFSTBoolean)
        layer.CreateField(bool_list_fld_defn)
        short_fld_defn = ogr.FieldDefn('short', ogr.OFTInteger)
        short_fld_defn.SetSubType(ogr.OFSTInt16)
        layer.CreateField(short_fld_defn)
        float_fld_defn = ogr.FieldDefn('float', ogr.OFTReal)
        float_fld_defn.SetSubType(ogr.OFSTFloat32)
        layer.CreateField(float_fld_defn)
        int64_fld_defn = ogr.FieldDefn('int64', ogr.OFTInteger64)
        layer.CreateField(int64_fld_defn)
        int64list_fld_defn = ogr.FieldDefn('int64list', ogr.OFTInteger64List)
        layer.CreateField(int64list_fld_defn)

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetField(0, 1)
        feat.SetField(1, 0)
        feat.SetFieldIntegerList(2, [1, 0])
        feat.SetField(3, -32768)
        feat.SetField(4, 1.23)
        feat.SetField(5, 1)
        feat.SetFieldInteger64List(6, [1])
        layer.CreateFeature(feat)

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetFID(1234567890123)
        feat.SetField(5, 1234567890123)
        feat.SetFieldInteger64List(6, [1, 1234567890123])
        layer.CreateFeature(feat)

      for iteration in range(3):
        with ogr.Open(filepath) as ds:
          layer = ds.GetLayer(0)
          layer_defn = layer.GetLayerDefn()

          b1_defn = layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('b1'))
          self.assertEqual(b1_defn.GetType(), ogr.OFTInteger, iteration)
          self.assertEqual(b1_defn.GetSubType(), ogr.OFSTBoolean, iteration)

          bool_list_defn = layer_defn.GetFieldDefn(
              layer_defn.GetFieldIndex('bool_list')
          )
          self.assertEqual(
              bool_list_defn.GetType(), ogr.OFTIntegerList, iteration
          )
          self.assertEqual(
              bool_list_defn.GetSubType(), ogr.OFSTBoolean, iteration
          )

          if iteration == 0:
            short_defn = layer_defn.GetFieldDefn(
                layer_defn.GetFieldIndex('short')
            )
            self.assertEqual(short_defn.GetType(), ogr.OFTInteger)
            self.assertEqual(short_defn.GetSubType(), ogr.OFSTInt16)

            float_defn = layer_defn.GetFieldDefn(
                layer_defn.GetFieldIndex('float')
            )
            self.assertEqual(float_defn.GetType(), ogr.OFTReal)
            self.assertEqual(float_defn.GetSubType(), ogr.OFSTFloat32)

          int64_defn = layer_defn.GetFieldDefn(
              layer_defn.GetFieldIndex('int64')
          )
          self.assertEqual(int64_defn.GetType(), ogr.OFTInteger64, iteration)

          int64list_defn = layer_defn.GetFieldDefn(
              layer_defn.GetFieldIndex('int64list')
          )
          self.assertEqual(
              int64list_defn.GetType(), ogr.OFTInteger64List, iteration
          )

          feat = layer.GetNextFeature()
          self.assertEqual(feat.GetField('b1'), 1)
          self.assertEqual(feat.GetField('b2'), 0)
          self.assertEqual(feat.GetFieldAsString('bool_list'), '(2:1,0)')
          self.assertEqual(feat.GetField('short'), -32768)
          self.assertEqual(feat.GetField('float'), 1.23)

          feat = layer.GetNextFeature()
          self.assertEqual(feat.GetFID(), 1234567890123)
          self.assertEqual(feat.GetField('int64'), 1234567890123)
          self.assertEqual(feat.GetField('int64list'), [1, 1234567890123])

        # Try-except needed because the XSD file does not exist in each
        # iteration.
        try:
          gdal.Unlink(filepath.replace('.gml', '.xsd'))
        except RuntimeError:
          pass

  def test_ogr_gml_68(self) -> None:
    tempdir = self._copy_testdata([
        'gml/choicepolygonmultipolygon.gml',
        'gml/choicepolygonmultipolygon.xsd',
    ])
    with ogr.Open(
        os.path.join(tempdir, 'choicepolygonmultipolygon.gml')
    ) as ds:
      expected_results = [
          'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))',
          'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)),((10 0,10 1,11 1,11 0,10 0)))',
      ]
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetGeomType(), ogr.wkbMultiPolygon)
      for expected_wkt in expected_results:
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), expected_wkt)

  def test_ogr_gml_69(self) -> None:
    filepath = '/vsimem/ogr_gml_69.gml'
    with gcore_util.GdalUnlinkWhenDone(filepath):
      with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
        layer = ds.CreateLayer('test', geom_type=ogr.wkbNone)

        field_not_nullable_defn = ogr.FieldDefn(
            'field_not_nullable', ogr.OFTString
        )
        field_not_nullable_defn.SetNullable(0)
        layer.CreateField(field_not_nullable_defn)
        field_nullable_defn = ogr.FieldDefn('field_nullable', ogr.OFTString)
        layer.CreateField(field_nullable_defn)
        geomfield_not_nullable_defn = ogr.GeomFieldDefn(
            'geomfield_not_nullable', ogr.wkbPoint
        )
        geomfield_not_nullable_defn.SetNullable(0)
        layer.CreateGeomField(geomfield_not_nullable_defn)
        geomfield_nullable_defn = ogr.GeomFieldDefn(
            'geomfield_nullable', ogr.wkbPoint
        )
        layer.CreateGeomField(geomfield_nullable_defn)

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetField('field_not_nullable', 'not_null')
        feat.SetGeomFieldDirectly(
            'geomfield_not_nullable',
            ogr.CreateGeometryFromWkt('POINT(0 0)'),
        )
        layer.CreateFeature(feat)

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetField('field_not_nullable', 'not_null')
        gdal.PushErrorHandler('CPLQuietErrorHandler')
        with self.assertRaisesRegex(
            RuntimeError, 'geomfield_not_nullable.*NULL'
        ):
          layer.CreateFeature(feat)
        gdal.PopErrorHandler()

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
        gdal.PushErrorHandler('CPLQuietErrorHandler')
        with self.assertRaisesRegex(RuntimeError, 'field_not_nullable.*NULL'):
          layer.CreateFeature(feat)
        gdal.PopErrorHandler()

      with gdal.OpenEx(filepath, open_options=['EMPTY_AS_NULL=NO']) as ds:
        layer = ds.GetLayerByName('test')
        layer_defn = layer.GetLayerDefn()
        self.assertEqual(
            layer_defn.GetFieldDefn(
                layer_defn.GetFieldIndex('field_not_nullable')
            ).IsNullable(),
            0,
        )
        self.assertEqual(
            layer_defn.GetFieldDefn(
                layer_defn.GetFieldIndex('field_nullable')
            ).IsNullable(),
            1,
        )
        self.assertEqual(
            layer_defn.GetGeomFieldDefn(
                layer_defn.GetGeomFieldIndex('geomfield_not_nullable')
            ).IsNullable(),
            0,
        )
        self.assertEqual(
            layer_defn.GetGeomFieldDefn(
                layer_defn.GetGeomFieldIndex('geomfield_nullable')
            ).IsNullable(),
            1,
        )

  def test_ogr_gml_70(self) -> None:
    filepath = '/vsimem/ogr_gml_70.gml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_70.xsd'),
    ):
      with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
        layer = ds.CreateLayer('test', geom_type=ogr.wkbNone)
        field_defn = ogr.FieldDefn('field_string', ogr.OFTString)
        field_defn.SetDefault("'a'")
        field_defn.SetNullable(0)
        layer.CreateField(field_defn)

        feat = ogr.Feature(layer.GetLayerDefn())
        layer.CreateFeature(feat)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayerByName('test')
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetField('field_string'), 'a')

  def test_ogr_gml_71(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/wfsjointlayer.gml', 'gml/wfsjointlayer.xsd']
    )
    gfs_path = os.path.join(tempdir, 'wfsjointlayer.gfs')
    self.assertFalse(os.path.exists(gfs_path))
    with ogr.Open(os.path.join(tempdir, 'wfsjointlayer.gml')) as ds:
      self._check_wfs_joint_layer(ds)
    self.assertFalse(os.path.exists(gfs_path))

  def test_ogr_gml_71bis(self) -> None:
    tempdir = self._copy_testdata(['gml/wfsjointlayer.gml'])
    gml_path = os.path.join(tempdir, 'wfsjointlayer.gml')
    xsd_path = ogr_util.GetTestFilePath(
        'gml/wfsjointlayer_not_understood.xsd'
    )
    with gdal.OpenEx(gml_path, open_options=[f'XSD={xsd_path}']) as ds:
      self._check_wfs_joint_layer(ds)
    self.assertTrue(os.path.exists(os.path.join(tempdir, 'wfsjointlayer.gfs')))

  def test_ogr_gml_71ter(self) -> None:
    tempdir = self._copy_testdata(['gml/wfsjointlayer.gml'])
    gml_path = os.path.join(tempdir, 'wfsjointlayer.gml')
    # Without .xsd nor .gfs
    with ogr.Open(gml_path) as ds:
      self._check_wfs_joint_layer(ds)
    self.assertTrue(os.path.exists(os.path.join(tempdir, 'wfsjointlayer.gfs')))
    # With .gfs
    with ogr.Open(gml_path) as ds:
      self._check_wfs_joint_layer(ds)

  @parameterized.named_parameters(
      dict(
          testcase_name='creation_options_override',
          creation_options=['NAME=name', 'DESCRIPTION=description'],
          metadata={'NAME': 'ignored', 'DESCRIPTION': 'ignored'},
      ),
      dict(
          testcase_name='set_metadata_only',
          creation_options=[],
          metadata={'NAME': 'name', 'DESCRIPTION': 'description'},
      ),
  )
  def test_ogr_gml_72(
      self,
      creation_options: Sequence[str],
      metadata: Mapping[str, str],
  ) -> None:
    filepath = '/vsimem/ogr_gml_72.gml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_72.xsd'),
    ):
      with ogr.GetDriverByName('GML').CreateDataSource(
          filepath, options=creation_options
      ) as ds:
        ds.SetMetadata(metadata)

      with ogr.Open(filepath) as ds:
        self.assertEqual(
            ds.GetMetadata(),
            {'NAME': 'name', 'DESCRIPTION': 'description'},
        )

  def test_ogr_gml_73(self) -> None:
    tempdir = self._copy_testdata(['gml/cswresults.xml'])
    with ogr.Open(os.path.join(tempdir, 'cswresults.xml')) as ds:
      for i in range(3):
        layer = ds.GetLayer(i)
        self.assertIn('4326', layer.GetSpatialRef().ExportToWkt())
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(),
            'POLYGON ((-180 -90,-180 90,180 90,180 -90,-180 -90))',
        )

  def test_ogr_gml_74(self) -> None:
    # With .xsd
    tempdir_xsd = self._copy_testdata([
        'gml/expected_gml_gml32.gml',
        'gml/expected_gml_gml32.xsd',
    ])
    with gdal.OpenEx(
        os.path.join(tempdir_xsd, 'expected_gml_gml32.gml'),
        open_options=['FORCE_SRS_DETECTION=YES'],
    ) as ds:
      layer = ds.GetLayer(0)
      self.assertIsNotNone(layer.GetSpatialRef())
      self.assertEqual(layer.GetFeatureCount(), 2)

    # Without .xsd or .gfs, then with generated .gfs
    tempdir = self._copy_testdata(['gml/expected_gml_gml32.gml'])
    gml_path = os.path.join(tempdir, 'expected_gml_gml32.gml')
    for _ in range(2):
      with gdal.OpenEx(
          gml_path, open_options=['FORCE_SRS_DETECTION=YES']
      ) as ds:
        layer = ds.GetLayer(0)
        self.assertIsNotNone(layer.GetSpatialRef())
        self.assertEqual(layer.GetFeatureCount(), 2)

  def test_ogr_gml_75(self) -> None:
    filepath = '/vsimem/ogr_gml_75.xml'
    with gcore_util.GdalUnlinkWhenDone(filepath):
      gdal.FileFromMemBuffer(
          filepath,
          """<?xml version="1.0" encoding="UTF-8"?>
<Capabilities xmlns="http://www.opengis.net/wmts/1.0"
  xmlns:ows="http://www.opengis.net/ows/1.1"
  xmlns:xlink="http://www.w3.org/1999/xlink"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
  xmlns:gml="http://www.opengis.net/gml"
  xsi:schemaLocation="http://www.opengis.net/wmts/1.0 http://somewhere"
  version="1.0.0">
  <ows:OperationsMetadata>
    <ows:Operation name="GetCapabilities">
      <ows:DCP>
        <ows:HTTP>
          <ows:Get xlink:href="http://foo"/>
        </ows:HTTP>
      </ows:DCP>
    </ows:Operation>
    <ows:Operation name="GetTile">
      <ows:DCP>
        <ows:HTTP>
          <ows:Get xlink:href="http://foo"/>
        </ows:HTTP>
      </ows:DCP>
    </ows:Operation>
  </ows:OperationsMetadata>
</Capabilities>""",
      )

      # The XML is not a valid GML file, so it should fail to open.
      with self.assertRaisesRegex(
          RuntimeError, 'not recognized as being in a supported file format'
      ):
        ogr.Open(filepath)

  @unittest.skip(
      'Upstream @pytest.mark.slow: parses 2.2 GB sparse GML files; exceeds'
      ' Forge timeout.'
  )
  def test_ogr_gml_76(self) -> None:
    tempdir = self._copy_testdata([
        'gml/huge_attribute_gml_sparse.xml',
        'gml/huge_attribute_start.gml',
        'gml/huge_attribute_end.gml',
        'gml/huge_geom_gml_sparse.xml',
        'gml/huge_geom_start.gml',
        'gml/huge_geom_end.gml',
    ])
    with gdal.quiet_errors(), ogr.ExceptionMgr(useExceptions=False):
      for name in (
          'huge_attribute_gml_sparse.xml',
          'huge_geom_gml_sparse.xml',
      ):
        ds = ogr.Open('/vsisparse/' + os.path.join(tempdir, name))
        if ds is not None:
          layer = ds.GetLayer(0)
          layer.GetNextFeature()
          ds = None

  def test_ogr_gml_77(self) -> None:
    filepath = '/vsimem/ogr_gml_77.xml'
    gml_content = """\
<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <ogr:featureMember>
    <ogr:point gml:id="point.0">
      <ogr:geometryProperty><gml:Point srsName="http://www.opengis.net/def/crs/EPSG/0/4326"><gml:pos>49 2</gml:pos></gml:Point></ogr:geometryProperty>
      <ogr:id>1</ogr:id>
    </ogr:point>
  </ogr:featureMember>
</ogr:FeatureCollection>
"""
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_77.gfs'),
    ):
      gdal.FileFromMemBuffer(filepath, gml_content)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual(
            layer.GetSpatialRef().GetDataAxisToSRSAxisMapping(), [2, 1]
        )
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

      with gdal.OpenEx(filepath, open_options=['SWAP_COORDINATES=YES']) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

      with gdal.OpenEx(filepath, open_options=['SWAP_COORDINATES=NO']) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (49 2)')

  def test_ogr_gml_78(self) -> None:
    filepath = '/vsimem/ogr_gml_78.xml'
    gml_content = """\
<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <ogr:featureMember>
    <ogr:point gml:id="point.0">
      <ogr:geometryProperty><gml:Point srsName="EPSG:4326"><gml:pos>2 49</gml:pos></gml:Point></ogr:geometryProperty>
      <ogr:id>1</ogr:id>
    </ogr:point>
  </ogr:featureMember>
</ogr:FeatureCollection>
"""
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_78.gfs'),
    ):
      gdal.FileFromMemBuffer(filepath, gml_content)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual(
            layer.GetSpatialRef().GetDataAxisToSRSAxisMapping(), [2, 1]
        )
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

      with gdal.OpenEx(filepath, open_options=['SWAP_COORDINATES=YES']) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (49 2)')

      with gdal.OpenEx(filepath, open_options=['SWAP_COORDINATES=NO']) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

  def test_ogr_gml_SWAP_COORDINATES_no_srs(self) -> None:
    xml_body = """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <ogr:featureMember>
    <ogr:point gml:id="point.0">
      <ogr:geometryProperty><gml:Point><gml:pos>2 49</gml:pos></gml:Point></ogr:geometryProperty>
      <ogr:id>1</ogr:id>
    </ogr:point>
  </ogr:featureMember>
</ogr:FeatureCollection>
"""
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_gml_swap_coords_no_srs'
    ):
      filepath = '/vsimem/ogr_gml_swap_coords_no_srs/test.xml'
      gdal.FileFromMemBuffer(filepath, xml_body)

      with ogr.Open(filepath) as ds:
        feat = ds.GetLayer(0).GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

      with gdal.OpenEx(
          filepath, open_options=['SWAP_COORDINATES=YES']
      ) as ds:
        feat = ds.GetLayer(0).GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (49 2)')

      with gdal.OpenEx(
          filepath, open_options=['SWAP_COORDINATES=NO']
      ) as ds:
        feat = ds.GetLayer(0).GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

  def test_ogr_gml_79(self) -> None:
    filepath = '/vsimem/ogr_gml_79.xml'
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)

    tests = [
        ('SHORT', 'EPSG:4326', '2 49'),
        ('OGC_URN', 'urn:ogc:def:crs:EPSG::4326', '49 2'),
        ('OGC_URL', 'http://www.opengis.net/def/crs/EPSG/0/4326', '49 2'),
    ]
    for srsname_format, expected_srsname, expected_coords in tests:
      with (
          gcore_util.GdalUnlinkWhenDone(filepath),
          gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_79.xsd'),
      ):
        with ogr.GetDriverByName('GML').CreateDataSource(
            filepath,
            options=[
                'FORMAT=GML3',
                f'SRSNAME_FORMAT={srsname_format}',
            ],
        ) as ds:
          layer = ds.CreateLayer('firstlayer', srs=sr)
          feat = ogr.Feature(layer.GetLayerDefn())
          geom = ogr.CreateGeometryFromWkt('POINT (2 49)')
          feat.SetGeometry(geom)
          layer.CreateFeature(feat)

        data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)

        self.assertIn(expected_srsname, data, srsname_format)
        self.assertIn(expected_coords, data, srsname_format)

  def test_ogr_gml_80(self) -> None:
    filepath = '/vsimem/ogr_gml_80.xml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_80.xsd'),
    ):
      with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
        layer = ds.CreateLayer('test', geom_type=ogr.wkbNone)
        layer.CreateField(ogr.FieldDefn('int_field', ogr.OFTInteger))

        feat = ogr.Feature(layer.GetLayerDefn())
        feat['int_field'] = 4
        layer.CreateFeature(feat)

        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetFieldNull('int_field')
        layer.CreateFeature(feat)

        feat = ogr.Feature(layer.GetLayerDefn())
        layer.CreateFeature(feat)

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat['int_field'], 4)

        feat = layer.GetNextFeature()
        self.assertIsNone(feat['int_field'])

        feat = layer.GetNextFeature()
        self.assertFalse(feat.IsFieldSet('int_field'))

  def test_ogr_gml_81(self) -> None:
    tempdir = self._copy_testdata(['gml/test_xsi_nil_gfs.gml'])
    with ogr.Open(os.path.join(tempdir, 'test_xsi_nil_gfs.gml')) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(feat.GetField('intval'), 1)

  def test_ogr_gml_gml2_write_geometry_error(self) -> None:
    filepath = '/vsimem/ogr_gml_83.gml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/ogr_gml_83.xsd'),
    ):
      with ogr.GetDriverByName('GML').CreateDataSource(
          filepath, options=['FORMAT=GML2']
      ) as ds:
        layer = ds.CreateLayer('test')
        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'GEOMETRYCOLLECTION(POINT(0 0), TIN EMPTY)'
            )
        )
        gdal.PushErrorHandler('CPLQuietErrorHandler')
        with self.assertRaisesRegex(
            RuntimeError, 'Export of geometry to GML failed'
        ):
          layer.CreateFeature(feat)
        gdal.PopErrorHandler()

  def test_ogr_gml_srsname_only_on_top_bounded_by(self) -> None:
    xml_body = """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ out.xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:boundedBy>
    <gml:Envelope srsName="EPSG:27700">
      <gml:lowerCorner>0 0</gml:lowerCorner>
      <gml:upperCorner>1 1</gml:upperCorner>
    </gml:Envelope>
  </gml:boundedBy>
  <gml:featureMember>
    <ogr:poly fid="poly.0">
      <ogr:geometryProperty><gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:coordinates>0,0 0,1 1,1 1,0 0,0</gml:coordinates></gml:LinearRing></gml:outerBoundaryIs></gml:Polygon></ogr:geometryProperty>
    </ogr:poly>
  </gml:featureMember>
</<ogr:FeatureCollection>"""
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_top_bounded_by'):
      filepath = '/vsimem/ogr_gml_top_bounded_by/test.xml'
      gdal.FileFromMemBuffer(filepath, xml_body)
      # First open generates .gfs; second reads .gfs
      for _ in range(2):
        with ogr.Open(filepath) as ds:
          layer = ds.GetLayer(0)
          self.assertIn('27700', layer.GetSpatialRef().ExportToWkt())

  def test_ogr_gml_featuretype_suffix_in_xsd(self) -> None:
    tempdir = self._copy_testdata(['gml/arcgis-world-wfs.gml'])
    gml_path = os.path.join(tempdir, 'arcgis-world-wfs.gml')
    xsd_path = ogr_util.GetTestFilePath('gml/arcgis-world-wfs.xsd')
    with ogr.Open(f'{gml_path},xsd={xsd_path}') as ds:
      layer_defn = ds.GetLayer(0).GetLayerDefn()
      for i in range(2, 4):
        self.assertEqual(layer_defn.GetFieldDefn(i).GetType(), ogr.OFTReal)

  def test_ogr_gml_standalone_geom(self) -> None:
    tempdir = self._copy_testdata(['gml/standalone_geometry.gml'])
    with ogr.Open(os.path.join(tempdir, 'standalone_geometry.gml')) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          'POLYGON ((2 49,3 49,3 48,2 48,2 49))',
      )

  def test_ogr_gml_unique(self) -> None:
    tempdir = self.create_tempdir()
    for gml_format in ('GML2', 'GML3', 'GML3.2'):
      for constraint_met in (True, False):
        filepath = os.path.join(tempdir.full_path, 'test_ogr_gml_unique.gml')
        with ogr.GetDriverByName('GML').CreateDataSource(
            filepath, options=[f'FORMAT={gml_format}']
        ) as ds:
          layer = ds.CreateLayer('test', geom_type=ogr.wkbNone)
          field_defn = ogr.FieldDefn('field_not_unique', ogr.OFTString)
          layer.CreateField(field_defn)
          field_defn = ogr.FieldDefn('field_unique', ogr.OFTString)
          field_defn.SetUnique(True)
          layer.CreateField(field_defn)

          feat = ogr.Feature(layer.GetLayerDefn())
          feat['field_unique'] = 'foo'
          layer.CreateFeature(feat)
          feat = ogr.Feature(layer.GetLayerDefn())
          feat['field_unique'] = 'bar' if constraint_met else 'foo'
          layer.CreateFeature(feat)
          feat = None

        with gdal.OpenEx(filepath) as ds:
          layer = ds.GetLayerByName('test')
          layer_defn = layer.GetLayerDefn()
          self.assertEqual(
              layer_defn.GetFieldDefn(
                  layer_defn.GetFieldIndex('field_not_unique')
              ).IsUnique(),
              0,
              f'format={gml_format}, constraint_met={constraint_met}',
          )
          self.assertEqual(
              layer_defn.GetFieldDefn(
                  layer_defn.GetFieldIndex('field_unique')
              ).IsUnique(),
              1,
              f'format={gml_format}, constraint_met={constraint_met}',
          )

  def test_ogr_gml_write_gfs_no(self) -> None:
    src_bytes = self._read_testdata_bytes('gml/expected_gml_gml32.gml')
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_write_gfs_no'):
      filepath = '/vsimem/ogr_gml_write_gfs_no/test.gml'
      gfs_path = '/vsimem/ogr_gml_write_gfs_no/test.gfs'
      gdal.FileFromMemBuffer(filepath, src_bytes)

      with gdal.OpenEx(filepath) as ds:
        self.assertIsNotNone(ds)
      self.assertIsNotNone(gdal.VSIStatL(gfs_path))
      gdal.Unlink(gfs_path)

      with gdal.OpenEx(filepath, open_options=['WRITE_GFS=NO']) as ds:
        self.assertIsNotNone(ds)
      self.assertIsNone(gdal.VSIStatL(gfs_path))

  def test_ogr_gml_write_gfs_yes(self) -> None:
    gml_bytes = self._read_testdata_bytes('gml/expected_gml_gml32.gml')
    xsd_bytes = self._read_testdata_bytes('gml/expected_gml_gml32.xsd')
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_write_gfs_yes'):
      filepath = '/vsimem/ogr_gml_write_gfs_yes/test.gml'
      gfs_path = '/vsimem/ogr_gml_write_gfs_yes/test.gfs'
      gdal.FileFromMemBuffer(filepath, gml_bytes)
      gdal.FileFromMemBuffer(
          '/vsimem/ogr_gml_write_gfs_yes/test.xsd', xsd_bytes
      )

      with gdal.OpenEx(filepath) as ds:
        self.assertIsNotNone(ds)
      self.assertIsNone(gdal.VSIStatL(gfs_path))

      with gdal.OpenEx(filepath, open_options=['WRITE_GFS=YES']) as ds:
        self.assertIsNotNone(ds)
      self.assertIsNotNone(gdal.VSIStatL(gfs_path))

  def test_ogr_gml_no_gfs_rewriting(self) -> None:
    src_bytes = self._read_testdata_bytes('gml/expected_gml_gml32.gml')
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_no_gfs_rewriting'):
      filepath = '/vsimem/ogr_gml_no_gfs_rewriting/test.gml'
      gfs_path = '/vsimem/ogr_gml_no_gfs_rewriting/test.gfs'
      gdal.FileFromMemBuffer(filepath, src_bytes)

      with gdal.OpenEx(filepath) as ds:
        self.assertIsNotNone(ds)
      self.assertIsNotNone(gdal.VSIStatL(gfs_path))

      f = gdal.VSIFOpenL(gfs_path, 'rb+')
      self.assertIsNotNone(f)
      data = gdal.VSIFReadL(1, 10000, f)
      gdal.VSIFSeekL(f, 0, 0)
      data += b'<!-- mycomment -->'
      gdal.VSIFWriteL(data, 1, len(data), f)
      gdal.VSIFCloseL(f)

      with gdal.OpenEx(filepath) as ds:
        self.assertIsNotNone(ds)

      reloaded = gcore_util.vsi_read_file_as_bytes(gfs_path, max_size=10000)
      self.assertIn(b'<!-- mycomment -->', reloaded)

  def test_ogr_gml_aixm_elevated_surface(self) -> None:
    tempdir = self._copy_testdata(['gml/aixm_ElevatedSurface.xml'])
    with ogr.Open(os.path.join(tempdir, 'aixm_ElevatedSurface.xml')) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetExtent(), (2, 3, 49, 50))
      feat = layer.GetNextFeature()
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(), 'POLYGON ((2 49,3 49,3 50,2 49))'
      )

  def test_ogr_gml_aixm_elevated_curve(self) -> None:
    tempdir = self._copy_testdata(['gml/aixm_ElevatedCurve.xml'])
    with ogr.Open(os.path.join(tempdir, 'aixm_ElevatedCurve.xml')) as ds:
      layer = ds.GetLayer('GuidanceLine')
      feat = layer.GetNextFeature()
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(), 'LINESTRING (2 49,3 50)'
      )

      layer = ds.GetLayer('VerticalStructure')
      feat = layer.GetNextFeature()
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(), 'LINESTRING (2 45,3 46)'
      )
      self.assertEqual(feat['elevation'], 75)
      self.assertEqual(feat['elevation_uom'], 'M')
      self.assertEqual(feat['verticalDatum'], 'EGM_96')
      self.assertEqual(feat['verticalAccuracy'], 30)
      self.assertEqual(feat['verticalAccuracy_uom'], 'M')

  def test_ogr_gml_aixm_vertical_structure_mix_point_and_line(self) -> None:
    tempdir = self._copy_testdata(
        ['gml/aixm_VerticalStructure_mix_point_and_line.xml']
    )
    with ogr.Open(
        os.path.join(tempdir, 'aixm_VerticalStructure_mix_point_and_line.xml')
    ) as ds:
      layer = ds.GetLayer('VerticalStructure')
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')
      self.assertEqual(feat['elevation'], 10)
      self.assertEqual(feat['elevation_uom'], 'M')
      self.assertEqual(feat['verticalAccuracy'], 0.1)

      feat = layer.GetNextFeature()
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(), 'LINESTRING (2 45,3 46)'
      )
      self.assertEqual(feat['elevation'], 0)

  @parameterized.parameters('GML2', 'GML3', 'GML3.2')
  def test_ogr_gml_srs_name_in_xsd(self, gml_format: str) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_srs_name_in_xsd'):
      filename = '/vsimem/ogr_gml_srs_name_in_xsd/test.gml'
      xsdfilename = '/vsimem/ogr_gml_srs_name_in_xsd/test.xsd'
      with ogr.GetDriverByName('GML').CreateDataSource(
          filename, options=[f'FORMAT={gml_format}']
      ) as ds:
        srs = osr.SpatialReference()
        srs.ImportFromEPSG(4326)
        layer = ds.CreateLayer('test', srs=srs, geom_type=ogr.wkbMultiPolygon)
        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt('MULTIPOLYGON (((2 49,2 50,3 50,2 49)))')
        )
        layer.CreateFeature(feat)

      data = gcore_util.vsi_read_file_as_bytes(xsdfilename, max_size=10000)
      if gml_format == 'GML2':
        self.assertIn(b'<!-- srsName="EPSG:4326" -->', data)
      else:
        self.assertIn(b'<!-- srsName="urn:ogc:def:crs:EPSG::4326" -->', data)

      with ogr.Open(filename) as ds:
        layer = ds.GetLayer(0)
        srs = layer.GetSpatialRef()
        self.assertEqual(srs.GetAuthorityCode(None), '4326')
        self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [2, 1])
        feat = layer.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(),
            'MULTIPOLYGON (((2 49,2 50,3 50,2 49)))',
        )

  def test_ogr_gml_too_nested(self) -> None:
    tempdir = self._copy_testdata(['gml/too_nested.gml'])
    gml_path = os.path.join(tempdir, 'too_nested.gml')
    gfs_path = os.path.join(tempdir, 'too_nested.gfs')

    with gdal.quiet_errors(), ogr.ExceptionMgr(useExceptions=False):
      with ogr.Open(gml_path) as ds:
        layer = ds.GetLayer(0)
        self.assertIsNone(layer.GetNextFeature())

    self.assertFalse(os.path.exists(gfs_path))

    with gdal.config_option('OGR_GML_NESTING_LEVEL', 'UNLIMITED'):
      with ogr.Open(gml_path) as ds:
        layer = ds.GetLayer(0)
        self.assertIsNotNone(layer.GetNextFeature())

  def test_ogr_gml_first_feature_without_geometry(self) -> None:
    src_bytes = self._read_testdata_bytes(
        'gml/first_feature_without_geometry.gml'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_first_no_geom'):
      filepath = '/vsimem/ogr_gml_first_no_geom/first_feature_without_geometry.gml'
      gdal.FileFromMemBuffer(filepath, src_bytes)
      with ogr.Open(filepath) as ds:
        self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbPoint)

  def test_ogr_gml_force_srs_detection_multiple_geom_fields(self) -> None:
    tempdir = self._copy_testdata([
        'gml/multiple_geometry_fields_srs_detection.gml',
        'gml/multiple_geometry_fields_srs_detection.xsd',
    ])
    with gdal.OpenEx(
        os.path.join(tempdir, 'multiple_geometry_fields_srs_detection.gml'),
        open_options=['FORCE_SRS_DETECTION=YES'],
    ) as ds:
      layer_defn = ds.GetLayer(0).GetLayerDefn()
      self.assertIsNone(layer_defn.GetGeomFieldDefn(0).GetSpatialRef())
      self.assertEqual(
          layer_defn.GetGeomFieldDefn(1).GetSpatialRef().GetAuthorityCode(None),
          '32631',
      )
      self.assertEqual(
          layer_defn.GetGeomFieldDefn(2).GetSpatialRef().GetAuthorityCode(None),
          '32632',
      )
      self.assertIsNone(layer_defn.GetGeomFieldDefn(3).GetSpatialRef())

  def test_ogr_gml_read_feature_with_gml_description(self) -> None:
    tempdir = self._copy_testdata([
        'gml/feature_with_gml_description.gml',
        'gml/feature_with_gml_description.xsd',
    ])
    with gdal.OpenEx(
        os.path.join(tempdir, 'feature_with_gml_description.gml')
    ) as ds:
      self.assertEqual(
          ds.GetMetadataItem('DESCRIPTION'), 'toplevel description'
      )
      self.assertEqual(ds.GetMetadataItem('NAME'), 'toplevel name')
      layer = ds.GetLayer(0)
      layer_defn = layer.GetLayerDefn()
      self.assertEqual(
          [
              layer_defn.GetFieldDefn(i).GetName()
              for i in range(layer_defn.GetFieldCount())
          ],
          [
              'gml_id',
              'description',
              'identifier',
              'name',
              'bar',
              'not_found_in_gml',
          ],
      )
      feat = layer.GetNextFeature()
      self.assertEqual(feat['gml_id'], 'foo.0')
      self.assertEqual(feat['description'], 'gml_description')
      self.assertEqual(feat['identifier'], 'gml_identifier')
      self.assertEqual(feat['name'], 'gml_name')
      self.assertEqual(feat['bar'], 1)

  def test_ogr_gml_read_srsDimension_3_on_top_gml_Envelope(self) -> None:
    tempdir = self._copy_testdata([
        'gml/global_srsDimension_3.gml',
        'gml/global_srsDimension_3.xsd',
    ])
    with gdal.OpenEx(
        os.path.join(tempdir, 'global_srsDimension_3.gml')
    ) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(
          feat.GetGeometryRef().ExportToIsoWkt(),
          'LINESTRING Z (1 2 3,4 5 6,7 8 9,10 11 12)',
      )

  def test_ogr_gml_read_boundedby_only(self) -> None:
    tempdir = self._copy_testdata(['gml/only_boundedby.gml'])
    gml_path = os.path.join(tempdir, 'only_boundedby.gml')
    gfs_path = os.path.join(tempdir, 'only_boundedby.gfs')

    self.assertFalse(os.path.exists(gfs_path))
    with gdal.OpenEx(gml_path) as ds:
      self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbNone)
    self.assertTrue(os.path.exists(gfs_path))

    os.unlink(gfs_path)
    for _ in range(2):
      with gdal.OpenEx(gml_path, open_options=['USE_BBOX=YES']) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual(layer.GetLayerDefn().GetGeomFieldCount(), 1)
        self.assertEqual(layer.GetGeomType(), ogr.wkbPolygon)
        self.assertEqual(layer.GetGeometryColumn(), 'boundedBy')
        self.assertEqual(layer.GetExtent(), (0, 2, 1, 3))
        feat = layer.GetNextFeature()
        self.assertEqual(feat['fid'], 'fid1')
        self.assertEqual(
            feat.GetGeometryRef().ExportToIsoWkt(),
            'POLYGON ((0 1,2 1,2 3,0 3,0 1))',
        )
        feat = layer.GetNextFeature()
        self.assertEqual(feat['fid'], 'fid2')
        self.assertIsNone(feat.GetGeometryRef())
      self.assertTrue(os.path.exists(gfs_path))

  def test_ogr_gml_read_boundedby_only_gml_null_only(self) -> None:
    tempdir = self._copy_testdata(['gml/only_boundedby_only_null.gml'])
    gml_path = os.path.join(tempdir, 'only_boundedby_only_null.gml')
    gfs_path = os.path.join(tempdir, 'only_boundedby_only_null.gfs')
    for _ in range(2):
      with gdal.OpenEx(gml_path, open_options=['USE_BBOX=YES']) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual(layer.GetLayerDefn().GetGeomFieldCount(), 0)
        feat = layer.GetNextFeature()
        self.assertEqual(feat['fid'], 'fid1')
        self.assertIsNone(feat.GetGeometryRef())
      self.assertTrue(os.path.exists(gfs_path))

  def test_ogr_gml_read_bbox_and_several_geom_elements(self) -> None:
    tempdir = self._copy_testdata(['gml/bbox_and_several_geom_elements.gml'])
    gml_path = os.path.join(tempdir, 'bbox_and_several_geom_elements.gml')
    gfs_path = os.path.join(tempdir, 'bbox_and_several_geom_elements.gfs')
    for _ in range(2):
      with gdal.OpenEx(gml_path, open_options=['USE_BBOX=YES']) as ds:
        layer0 = ds.GetLayer(0)
        self.assertEqual(layer0.GetGeometryColumn(), 'geom1')
        self.assertEqual(layer0.GetGeomType(), ogr.wkbMultiPolygon)
        feat0 = layer0.GetNextFeature()
        self.assertEqual(
            feat0.GetGeometryRef().GetGeometryType(),
            ogr.wkbMultiPolygon,
        )
        layer1 = ds.GetLayer(1)
        self.assertEqual(layer1.GetGeometryColumn(), 'geom2')
        self.assertEqual(layer1.GetGeomType(), ogr.wkbPoint)
        feat1 = layer1.GetNextFeature()
        self.assertEqual(
            feat1.GetGeometryRef().GetGeometryType(),
            ogr.wkbPoint,
        )
      self.assertTrue(os.path.exists(gfs_path))

  def test_ogr_gml_read_boundedby_invalid(self) -> None:
    tempdir = self._copy_testdata(['gml/only_boundedby_invalid.gml'])
    with gdal.quiet_errors():
      with gdal.OpenEx(
          os.path.join(tempdir, 'only_boundedby_invalid.gml'),
          open_options=['USE_BBOX=YES'],
      ) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

  def test_ogr_gml_read_boundedby_repeated(self) -> None:
    tempdir = self._copy_testdata(['gml/only_boundedby_repeated.gml'])
    with gdal.OpenEx(
        os.path.join(tempdir, 'only_boundedby_repeated.gml'),
        open_options=['USE_BBOX=YES'],
    ) as ds:
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  def test_ogr_gml_field_comment(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_field_comment'):
      filepath = '/vsimem/ogr_gml_field_comment/test.gml'
      with ogr.GetDriverByName('GML').CreateDataSource(filepath) as ds:
        layer = ds.CreateLayer('test', geom_type=ogr.wkbNone)
        for field_type in (
            ogr.OFTInteger,
            ogr.OFTInteger64,
            ogr.OFTReal,
            ogr.OFTString,
            ogr.OFTIntegerList,
            ogr.OFTInteger64List,
            ogr.OFTRealList,
            ogr.OFTStringList,
            ogr.OFTDateTime,
            ogr.OFTDate,
            ogr.OFTTime,
        ):
          field_defn = ogr.FieldDefn(
              f'field{layer.GetLayerDefn().GetFieldCount()}', field_type
          )
          field_defn.SetComment('comment <>')
          layer.CreateField(field_defn)

      with gdal.OpenEx(filepath) as ds:
        layer_defn = ds.GetLayer(0).GetLayerDefn()
        for i in range(1, layer_defn.GetFieldCount()):
          self.assertEqual(layer_defn.GetFieldDefn(i).GetComment(), 'comment <>')

  def test_ogr_gml_field_comment_from_gfs(self) -> None:
    tempdir = self._copy_testdata(['gml/testcomment.gml', 'gml/testcomment.gfs'])
    gml_path = os.path.join(tempdir, 'testcomment.gml')
    gfs_path = os.path.join(tempdir, 'testcomment.gfs')
    self._touch_newer(gfs_path, gml_path)
    with gdal.OpenEx(gml_path) as ds:
      layer_defn = ds.GetLayer(0).GetLayerDefn()
      self.assertEqual(
          layer_defn.GetFieldDefn(layer_defn.GetFieldIndex('name')).GetComment(),
          'my comment',
      )

  def test_ogr_gml_ignore_old_gfs(self) -> None:
    tempdir = self._copy_testdata(['gml/ionic_wfs.gml'])
    gml_path = os.path.join(tempdir, 'ionic_wfs.gml')
    gfs_path = os.path.join(tempdir, 'ionic_wfs.gfs')
    with open(
        ogr_util.GetTestFilePath('gml/ionic_wfs.gfs'), 'rt', encoding='utf-8'
    ) as f_in:
      gfs_contents = f_in.read().replace('<Width>0</Width>', '<Width>22</Width>')
    with open(gfs_path, 'wt', encoding='utf-8') as f_out:
      f_out.write(gfs_contents)
    self._touch_newer(gfs_path, gml_path)

    with ogr.Open(gml_path) as ds:
      self.assertEqual(
          ds.GetLayer(0).GetLayerDefn().GetFieldDefn(1).GetWidth(), 22
      )

    os.utime(gfs_path, (1, 1))
    with ogr.Open(gml_path) as ds:
      self.assertEqual(
          ds.GetLayer(0).GetLayerDefn().GetFieldDefn(1).GetWidth(), 10
      )

  def test_ogr_gml_geom_coord_precision(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_coord_precision'):
      filepath = '/vsimem/ogr_gml_coord_precision/test.gml'
      xsd_path = '/vsimem/ogr_gml_coord_precision/test.xsd'
      with gdal.GetDriverByName('GML').Create(
          filepath, 0, 0, 0, gdal.GDT_Unknown
      ) as ds:
        geom_fld = ogr.GeomFieldDefn('my_geom_field', ogr.wkbUnknown)
        prec = ogr.CreateGeomCoordinatePrecision()
        prec.Set(1e-5, 1e-3, 0)
        geom_fld.SetCoordinatePrecision(prec)
        geom_fld.SetNullable(False)
        layer = ds.CreateLayerFromGeomFieldDefn('test', geom_fld)
        layer_prec = (
            layer.GetLayerDefn().GetGeomFieldDefn(0).GetCoordinatePrecision()
        )
        self.assertEqual(layer_prec.GetXYResolution(), 1e-5)
        self.assertEqual(layer_prec.GetZResolution(), 1e-3)

        for wkt in (
            'POINT(1.23456789 2.34567891)',
            'POINT(1.23456789 2.34567891 9.87654321)',
            'LINESTRING(1.23456789 2.34567891 9.87654321,'
            '1.23456789 2.34567891 9.87654321)',
        ):
          feat = ogr.Feature(layer.GetLayerDefn())
          feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
          layer.CreateFeature(feat)

      data = gcore_util.vsi_read_file_as_bytes(filepath, max_size=10000)
      self.assertIn(
          b'\n  <gml:boundedBy><gml:Envelope srsDimension="3">'
          b'<gml:lowerCorner>1.23457 2.34568 0.0</gml:lowerCorner>'
          b'<gml:upperCorner>1.23457 2.34568 9.877</gml:upperCorner>'
          b'</gml:Envelope></gml:boundedBy>',
          data,
      )
      self.assertIn(
          b'\n      <gml:boundedBy><gml:Envelope srsDimension="3">'
          b'<gml:lowerCorner>1.23457 2.34568 9.877</gml:lowerCorner>'
          b'<gml:upperCorner>1.23457 2.34568 9.877</gml:upperCorner>'
          b'</gml:Envelope></gml:boundedBy>',
          data,
      )
      self.assertIn(b'<gml:pos>1.23457 2.34568</gml:pos>', data)
      self.assertIn(b'<gml:pos>1.23457 2.34568 9.877</gml:pos>', data)
      self.assertIn(
          b'<gml:posList srsDimension="3">'
          b'1.23457 2.34568 9.877 1.23457 2.34568 9.877</gml:posList>',
          data,
      )

      xsd_data = gcore_util.vsi_read_file_as_bytes(xsd_path, max_size=10000)
      self.assertIn(b'<ogr:xy_coordinate_resolution>', xsd_data)
      self.assertIn(b'<ogr:z_coordinate_resolution>', xsd_data)

      with ogr.Open(filepath) as ds:
        reopened_fld = ds.GetLayer(0).GetLayerDefn().GetGeomFieldDefn(0)
        self.assertEqual(reopened_fld.GetName(), 'my_geom_field')
        self.assertFalse(reopened_fld.IsNullable())
        reopened_prec = reopened_fld.GetCoordinatePrecision()
        self.assertEqual(reopened_prec.GetXYResolution(), 1e-5)
        self.assertEqual(reopened_prec.GetZResolution(), 1e-3)

  def test_ogr_gml_geom_link_to_immediate_child(self) -> None:
    with gdal.OpenEx(
        ogr_util.GetTestFilePath('gml/link_to_immediate_child.gml'),
        open_options=['WRITE_GFS=NO'],
    ) as ds:
      self.assertIsNotNone(ds)

  @parameterized.product(
      use_create_geom_field=(False, True),
      has_srs=(False, True),
  )
  def test_ogr_gml_ogr2ogr_from_layer_with_name_geom_field(
      self, use_create_geom_field: bool, has_srs: bool
  ) -> None:
    with gdal.GetDriverByName('MEM').Create(
        '', 0, 0, 0, gdal.GDT_Unknown
    ) as ds:
      if has_srs:
        srs = osr.SpatialReference()
        srs.ImportFromEPSG(4326)
        srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
      else:
        srs = None
      if use_create_geom_field:
        layer = ds.CreateLayer('test', geom_type=ogr.wkbNone)
        my_geom_field = ogr.GeomFieldDefn('my_geom', ogr.wkbUnknown)
        my_geom_field.SetSpatialRef(srs)
        layer.CreateGeomField(my_geom_field)
      else:
        layer = ds.CreateLayer('test', geom_type=ogr.wkbUnknown, srs=srs)

      for wkt in ('POINT(2 49)', 'POINT(3 50)'):
        feat = ogr.Feature(layer.GetLayerDefn())
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(wkt))
        layer.CreateFeature(feat)

      with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_named_geom_field'):
        out_filename = '/vsimem/ogr_gml_named_geom_field/out.gml'
        gdal.VectorTranslate(out_filename, ds, format='GML')
        data = gcore_util.vsi_read_file_as_bytes(out_filename, max_size=10000)

    if has_srs:
      self.assertIn(
          b'<gml:boundedBy><gml:Envelope srsName="urn:ogc:def:crs:EPSG::4326">'
          b'<gml:lowerCorner>49 2</gml:lowerCorner>'
          b'<gml:upperCorner>50 3</gml:upperCorner>'
          b'</gml:Envelope></gml:boundedBy>',
          data,
      )
    else:
      self.assertIn(
          b'<gml:boundedBy><gml:Envelope>'
          b'<gml:lowerCorner>2 49</gml:lowerCorner>'
          b'<gml:upperCorner>3 50</gml:upperCorner>'
          b'</gml:Envelope></gml:boundedBy>',
          data,
      )

  @parameterized.parameters(False, True)
  def test_ogr_gml_ogr2ogr_from_layers_with_inconsistent_srs(
      self, first_layer_has_srs: bool
  ) -> None:
    with gdal.GetDriverByName('MEM').Create(
        '', 0, 0, 0, gdal.GDT_Unknown
    ) as ds:
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

      layer = ds.CreateLayer(
          'test',
          geom_type=ogr.wkbUnknown,
          srs=srs if first_layer_has_srs else None,
      )
      feat = ogr.Feature(layer.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(2 49)'))
      layer.CreateFeature(feat)

      layer = ds.CreateLayer(
          'test2',
          geom_type=ogr.wkbUnknown,
          srs=None if first_layer_has_srs else srs,
      )
      feat = ogr.Feature(layer.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(3 50)'))
      layer.CreateFeature(feat)

      with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_inconsistent_srs'):
        out_filename = '/vsimem/ogr_gml_inconsistent_srs/out.gml'
        gdal.VectorTranslate(out_filename, ds, format='GML')
        data = gcore_util.vsi_read_file_as_bytes(out_filename, max_size=10000)

    self.assertIn(b'<gml:boundedBy><gml:Null /></gml:boundedBy>', data)

  def test_ogr_gml_USE_SCHEMA_IMPORT_NO(self) -> None:
    tempdir = self._copy_min_example()
    with gdal.OpenEx(
        os.path.join(tempdir, 'minimal_example.gml'),
        open_options=['USE_SCHEMA_IMPORT=NO'],
    ) as ds:
      self.assertNotEqual(ds.GetLayerCount(), 2)

  def test_ogr_gml_USE_SCHEMA_IMPORT_YES(self) -> None:
    tempdir = self._copy_min_example()
    with gdal.OpenEx(
        os.path.join(tempdir, 'minimal_example.gml'),
        open_options=['USE_SCHEMA_IMPORT=YES'],
    ) as ds:
      self.assertEqual(ds.GetLayerCount(), 2)

  def test_ogr_gml_GML_USE_SCHEMA_IMPORT_YES(self) -> None:
    tempdir = self._copy_min_example()
    with gdal.config_option('GML_USE_SCHEMA_IMPORT', 'YES'):
      with ogr.Open(os.path.join(tempdir, 'minimal_example.gml')) as ds:
        self.assertEqual(ds.GetLayerCount(), 2)

  def test_ogr_gml_GML_USE_SCHEMA_IMPORT_NO(self) -> None:
    tempdir = self._copy_min_example()
    with gdal.config_option('GML_USE_SCHEMA_IMPORT', 'NO'):
      with ogr.Open(os.path.join(tempdir, 'minimal_example.gml')) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)

  def test_ogr_gml_get_layers_by_name_from_imported_schema(self) -> None:
    tempdir = self._copy_min_example()
    with gdal.OpenEx(
        os.path.join(tempdir, 'minimal_example.gml'),
        open_options=['USE_SCHEMA_IMPORT=YES'],
    ) as ds:
      self.assertEqual(ds.GetLayerCount(), 2)

      layer = ds.GetLayerByName('sanier')
      self.assertIsNotNone(layer)
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetFieldAsString('arokstatus'), 'Rechtsbestand')

      layer = ds.GetLayerByName('entwick')
      self.assertIsNotNone(layer)
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetFieldAsDouble('SHAPE_Leng'), 8266.56532551)

  def test_ogr_gml_get_layers_by_name_from_imported_schema_more_tests(
      self,
  ) -> None:
    tempdir = self._copy_min_example()
    with gdal.OpenEx(
        os.path.join(tempdir, 'minimal_example.gml'),
        open_options=['USE_SCHEMA_IMPORT=YES'],
    ) as ds:
      self.assertEqual(ds.GetLayerCount(), 2)

      layer = ds.GetLayerByName('entwick')
      feat = layer.GetNextFeature()
      self.assertEqual(feat.GetFieldAsDouble('SHAPE_Leng'), 8266.56532551)
      oa_nr_num = feat.GetFieldAsDouble('oa_nr')
      self.assertIsInstance(oa_nr_num, float)
      self.assertEqual(oa_nr_num, 430070)

      layer = ds.GetLayerByName('sanier')
      feat = layer.GetNextFeature()
      oa_nr_str = feat.GetFieldAsString('oa_nr')
      self.assertIsInstance(oa_nr_str, str)
      self.assertEqual(oa_nr_str, '430050')
      self.assertIsInstance(feat.GetFieldAsDateTime('dat_erst'), list)

  def test_ogr_gml_force_opening(self) -> None:
    src_bytes = self._read_testdata_bytes('nas/empty_nas.xml')
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_force_opening'):
      filepath = '/vsimem/ogr_gml_force_opening/test.gml'
      gdal.FileFromMemBuffer(filepath, src_bytes)
      with gdal.config_option('NAS_GFS_TEMPLATE', ''):
        with gdal.OpenEx(filepath, allowed_drivers=['GML']) as ds:
          self.assertEqual(ds.GetDriver().GetDescription(), 'GML')

  @parameterized.product(
      is_path=(False, True),
      spec=(
          (
              None,
              True,
              [ogr.OFTString, ogr.OFTReal, ogr.OFTInteger, ogr.OFTString],
              [],
              '',
          ),
          (
              None,
              False,
              [ogr.OFTString, ogr.OFTReal, ogr.OFTInteger, ogr.OFTInteger],
              [],
              '',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "str", "type": "Real" }]}]}',
              False,
              [ogr.OFTString, ogr.OFTReal, ogr.OFTInteger, ogr.OFTReal],
              [],
              '',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "str", "type": "Real" }]}]}',
              True,
              [ogr.OFTString, ogr.OFTReal, ogr.OFTInteger, ogr.OFTReal],
              [],
              '',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "xxxx", "type": "Real" }]}]}',
              True,
              [],
              [],
              'Field xxxx not found in layer test_point',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "str", "type": "xxxx" },'
              ' {"name": "dbl", "type": "String" }]}]}',
              True,
              [],
              [],
              'Unsupported field type: xxxx for field str',
          ),
          (
              '{"layers": [{"name": "xxxx",'
              ' "fields": [{ "name": "str", "type": "Real" }]}]}',
              True,
              [],
              [],
              'Layer xxxx not found',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "str", "type": "Real" }]},'
              '{"name": "xxxx",'
              ' "fields": [{ "name": "str", "type": "Real" }]}]}',
              True,
              [],
              [],
              'Layer xxxx not found',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "str", "newName": "new_str" }]}]}',
              True,
              [ogr.OFTString, ogr.OFTReal, ogr.OFTInteger, ogr.OFTString],
              ['fid', 'dbl', 'int', 'new_str'],
              '',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "str", "subType": "UUID" }]}]}',
              True,
              [
                  ogr.OFTString,
                  ogr.OFTReal,
                  ogr.OFTInteger,
                  (ogr.OFTString, ogr.OFSTUUID),
              ],
              ['fid', 'dbl', 'int', 'str'],
              '',
          ),
          (
              '{"layers": [{"name": "test_point", "schemaType": "Full",'
              ' "fields": [{ "name": "dbl"}, { "name": "str", "type": "String",'
              ' "newName": "new_str", "subType": "UUID" }]}]}',
              True,
              [ogr.OFTString, ogr.OFTReal, (ogr.OFTString, ogr.OFSTUUID)],
              ['fid', 'dbl', 'new_str'],
              '',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "int", "width": 7, "precision": 3 }]}]}',
              True,
              [ogr.OFTString, ogr.OFTReal, ogr.OFTInteger, ogr.OFTString],
              [],
              '',
          ),
          (
              '{"layers": [{"name": "test_point",'
              ' "fields": [{ "name": "int", "subType": "Boolean" },'
              ' { "name": "dbl", "type": "Integer", "subType": "Int16" }]}]}',
              True,
              [
                  ogr.OFTString,
                  (ogr.OFTInteger, ogr.OFSTInt16),
                  (ogr.OFTInteger, ogr.OFSTBoolean),
                  ogr.OFTString,
              ],
              ['fid', 'dbl', 'int', 'str'],
              '',
          ),
      ),
  )
  def test_ogr_gml_type_override(self, is_path: bool, spec: Any) -> None:
    (
        schema,
        with_xsd,
        expected_field_types,
        expected_field_names,
        expected_warning,
    ) = spec
    if schema is None and is_path:
      return

    files = ['gml/test_point.gml']
    if with_xsd:
      files.append('gml/test_point.xsd')
    tempdir = self._copy_testdata(files)
    gml_path = os.path.join(tempdir, 'test_point.gml')

    open_options: list[str] = []
    if is_path:
      schema_path = os.path.join(tempdir, 'ogr_schema.json')
      with open(schema_path, 'wt', encoding='utf-8') as f:
        f.write(schema)
      open_options.append(f'OGR_SCHEMA={schema_path}')
    elif schema is not None:
      open_options.append(f'OGR_SCHEMA={schema}')

    gdal.ErrorReset()
    if not expected_field_types:
      with gdal.quiet_errors(), gdal.ExceptionMgr(useExceptions=False):
        ds = gdal.OpenEx(
            gml_path,
            gdal.OF_VECTOR | gdal.OF_READONLY,
            open_options=open_options,
            allowed_drivers=['GML'],
        )
        self.assertIn(expected_warning, gdal.GetLastErrorMsg())
        self.assertIsNone(ds)
      return

    with gdal.OpenEx(
        gml_path,
        gdal.OF_VECTOR | gdal.OF_READONLY,
        open_options=open_options,
        allowed_drivers=['GML'],
    ) as gml_ds:
      self.assertEqual(gml_ds.GetLayerCount(), 1)
      feat = gml_ds.GetLayerByName('test_point').GetNextFeature()
      field_names = expected_field_names or ('fid', 'dbl', 'int', 'str')
      for i in range(len(field_names)):
        expected_spec = expected_field_types[i]
        if isinstance(expected_spec, tuple):
          expected_type, expected_subtype = expected_spec
          self.assertEqual(feat.GetFieldDefnRef(i).GetType(), expected_type)
          self.assertEqual(
              feat.GetFieldDefnRef(i).GetSubType(), expected_subtype
          )
        else:
          self.assertEqual(feat.GetFieldDefnRef(i).GetType(), expected_spec)

      if open_options and 'precision' in open_options[0]:
        self.assertEqual(feat.GetFieldDefnRef(2).GetWidth(), 7)
        self.assertEqual(feat.GetFieldDefnRef(2).GetPrecision(), 3)

      self.assertEqual(feat.GetFieldAsString('fid'), 'F0')
      self.assertEqual(feat.GetFieldAsDouble('dbl'), 1.0)
      if 'int' in field_names:
        self.assertEqual(feat.GetFieldAsInteger('int'), 1)
      if 'str' in field_names:
        self.assertEqual(feat.GetFieldAsString('str'), '1')
      if 'new_str' in field_names:
        self.assertEqual(feat.GetFieldAsString('new_str'), '1')
      if expected_warning:
        self.assertIn(expected_warning, gdal.GetLastErrorMsg())

  def test_ogr_gml_write_error(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_write_error'):
      filepath_short = '/vsimem/ogr_gml_write_error/test1.gml||maxlength=200'
      ds = ogr.GetDriverByName('GML').CreateDataSource(
          filepath_short, options=['XSISCHEMA=OFF']
      )
      with self.assertRaisesRegex(RuntimeError, 'Could not write line'):
        ds.Close()

      filepath_mid = '/vsimem/ogr_gml_write_error/test2.gml||maxlength=600'
      ds = ogr.GetDriverByName('GML').CreateDataSource(
          filepath_mid, options=['XSISCHEMA=OFF']
      )
      layer = ds.CreateLayer('test')
      feat = ogr.Feature(layer.GetLayerDefn())
      with self.assertRaisesRegex(RuntimeError, 'Could not write line'):
        layer.CreateFeature(feat)
      ds.Close()

  def test_ogr_gml_unique_fid_not_increasing_no_prefix(self) -> None:
    filepath = '/vsimem/test_fid_no_prefix.xml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/test_fid_no_prefix.gfs'),
    ):
      gdal.FileFromMemBuffer(
          filepath,
          """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ out.xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <ogr:poly fid="2"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="1"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="4"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="3"></ogr:poly>
  </gml:featureMember>
</ogr:FeatureCollection>""",
      )

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual([feat.GetFID() for feat in layer], [2, 1, 4, 3])

  def test_ogr_gml_unique_fid_not_increasing_prefix(self) -> None:
    filepath = '/vsimem/test_fid_prefix.xml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/test_fid_prefix.gfs'),
    ):
      gdal.FileFromMemBuffer(
          filepath,
          """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ out.xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <ogr:poly fid="poly.2"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.1"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.4"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.3"></ogr:poly>
  </gml:featureMember>
</ogr:FeatureCollection>""",
      )

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual([feat.GetFID() for feat in layer], [2, 1, 4, 3])

  def test_ogr_gml_unique_fid_not_unique(self) -> None:
    filepath = '/vsimem/test_fid_not_unique.xml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/test_fid_not_unique.gfs'),
    ):
      gdal.FileFromMemBuffer(
          filepath,
          """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ out.xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <ogr:poly fid="poly.2"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.1"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.2"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.3"></ogr:poly>
  </gml:featureMember>
</ogr:FeatureCollection>""",
      )

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual([feat.GetFID() for feat in layer], [2, 1, 3, 4])

  def test_ogr_gml_unique_fid_not_unique_no_numeric(self) -> None:
    filepath = '/vsimem/test_fid_no_numeric.xml'
    with (
        gcore_util.GdalUnlinkWhenDone(filepath),
        gcore_util.GdalUnlinkWhenDone('/vsimem/test_fid_no_numeric.gfs'),
    ):
      gdal.FileFromMemBuffer(
          filepath,
          """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ out.xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:featureMember>
    <ogr:poly fid="poly.2"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.1"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.2a"></ogr:poly>
  </gml:featureMember>
  <gml:featureMember>
    <ogr:poly fid="poly.3"></ogr:poly>
  </gml:featureMember>
</ogr:FeatureCollection>""",
      )

      with ogr.Open(filepath) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual([feat.GetFID() for feat in layer], [2, 1, 3, 4])

  # Test open option SKIP_CORRUPTED_FEATURES
  def test_ogr_gml_skip_corrupted_features(self):
    filename = '/vsimem/ogr_gml_skip_corrupted_features.gml'
    gml = """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation=""
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:boundedBy>
    <gml:Box>
      <gml:coord><gml:X>0</gml:X><gml:Y>-5</gml:Y></gml:coord>
      <gml:coord><gml:X>8</gml:X><gml:Y>3</gml:Y></gml:coord>
    </gml:Box>
  </gml:boundedBy>

  <gml:featureMember>
    <ogr:solids fid="solids.1">
    <boundary>
            <ClosureSurface>
              <lod2MultiSurface>
                <gml:MultiSurface>
                  <gml:surfaceMember>
                    <gml:Polygon gml:id="UUID_da310e81-cd4a-4167-94a0-3ea04f39ebd0">
                      <gml:exterior>
                        <gml:LinearRing>
                          <gml:posList srsDimension="3">646102.957 5667459.848 146.668 646102.957 5667459.848 142.56 646107.611 5667459.649 142.56 646107.611 5667459.649 146.696 646102.957 5667459.848 146.668</gml:posList>
                        </gml:LinearRing>
                      </gml:exterior>
                    </gml:Polygon>
                  </gml:surfaceMember>
                </gml:MultiSurface>
              </lod2MultiSurface>
            </ClosureSurface>
          </boundary>
      <gml:Solid>
        <gml:exterior>
          <gml:XXXX>
            <gml:surfaceMember xlink:href="#UUID_da310e81-cd4a-4167-94a0-3ea04f39ebd0"/>
          </gml:XXXX>
        </gml:exterior>
      </gml:Solid>
    </ogr:solids>
  </gml:featureMember>
</ogr:FeatureCollection>"""
    gdal.FileFromMemBuffer(filename, gml)

    message = (
        'You may set the GML_SKIP_CORRUPTED_FEATURES configuration option to'
        ' YES to skip to the next feature'
    )

    with gdal.config_option('GML_SKIP_CORRUPTED_FEATURES', 'NO'):
      with ogr.Open(filename) as ds:
        layer = ds.GetLayer(0)
        with self.assertRaisesRegex(RuntimeError, message):
          layer.GetNextFeature()

    with gdal.config_option('GML_SKIP_CORRUPTED_FEATURES', 'YES'):
      with ogr.Open(filename) as ds:
        layer = ds.GetLayer(0)
        f = layer.GetNextFeature()
        self.assertIsNone(f)

    with gdal.config_option('GML_SKIP_CORRUPTED_FEATURES', 'NO'):
      with gdal.OpenEx(
          filename, open_options=['SKIP_CORRUPTED_FEATURES=YES']
      ) as ds:
        layer = ds.GetLayer(0)
        f = layer.GetNextFeature()
        self.assertIsNone(f)

    with gdal.config_option('GML_SKIP_CORRUPTED_FEATURES', 'NO'):
      with gdal.OpenEx(
          filename, open_options=['SKIP_CORRUPTED_FEATURES=NO']
      ) as ds:
        layer = ds.GetLayer(0)
        with self.assertRaisesRegex(RuntimeError, message):
          layer.GetNextFeature()

  @parameterized.parameters(True, False)
  def test_ogr_gml_citygml3(self, skip_resolve_as_open_option: bool) -> None:
    xml_body = """<?xml version="1.0" encoding="utf-8" ?>
<CityModel>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation=""
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
  <gml:boundedBy>
    <gml:Box>
      <gml:coord><gml:X>0</gml:X><gml:Y>-5</gml:Y></gml:coord>
      <gml:coord><gml:X>8</gml:X><gml:Y>3</gml:Y></gml:coord>
    </gml:Box>
  </gml:boundedBy>
  <gml:featureMember>
    <ogr:solids fid="solids.1">
      <lod2Solid>
        <gml:Solid>
          <gml:exterior>
            <gml:Shell>
              <gml:surfaceMember xlink:href="#UUID_da310e81-cd4a-4167-94a0-3ea04f39ebd0"/>
            </gml:Shell>
          </gml:exterior>
        </gml:Solid>
      </lod2Solid>
      <boundary>
        <ClosureSurface>
          <lod2MultiSurface>
            <gml:MultiSurface>
              <gml:surfaceMember>
                <gml:Polygon gml:id="UUID_da310e81-cd4a-4167-94a0-3ea04f39ebd0">
                  <gml:exterior>
                    <gml:LinearRing>
                      <gml:posList srsDimension="3">0 0 0 1 1 1 1 0 2 0 0 0</gml:posList>
                    </gml:LinearRing>
                  </gml:exterior>
                </gml:Polygon>
              </gml:surfaceMember>
            </gml:MultiSurface>
          </lod2MultiSurface>
        </ClosureSurface>
      </boundary>
    </ogr:solids>
  </gml:featureMember>
</ogr:FeatureCollection>
</CityModel>"""
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_citygml3'):
      filename = '/vsimem/ogr_gml_citygml3/test.gml'
      gdal.FileFromMemBuffer(filename, xml_body)
      if skip_resolve_as_open_option:
        cm = gdal.OpenEx(filename, open_options=['SKIP_RESOLVE_ELEMS=NONE'])
      else:
        with gdal.config_option('GML_SKIP_RESOLVE_ELEMS', 'NONE'):
          cm = ogr.Open(filename)
      with cm as ds:
        layer = ds.GetLayer(0)
        self.assertEqual(layer.GetGeometryColumn(), 'lod2Solid')
        feat = layer.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(),
            'POLYHEDRALSURFACE Z (((0 0 0,1 1 1,1 0 2,0 0 0)))',
        )

  def test_ogr_gml_datetime(self) -> None:
    tempdir = self._copy_testdata(['gml/datetime.gml', 'gml/datetime.xsd'])
    with ogr.Open(os.path.join(tempdir, 'datetime.gml')) as ds:
      layer = ds.GetLayer(0)
      feat = layer.GetNextFeature()
      layer_defn = layer.GetLayerDefn()
      self.assertEqual(layer_defn.GetFieldDefn(1).GetType(), ogr.OFTTime)
      self.assertEqual(feat['time'], '23:59:60')
      self.assertEqual(layer_defn.GetFieldDefn(2).GetType(), ogr.OFTDate)
      self.assertEqual(feat['date'], '9999/12/31')
      self.assertEqual(layer_defn.GetFieldDefn(3).GetType(), ogr.OFTDateTime)
      self.assertEqual(feat['datetime'], '9999/12/31 23:59:60.999')
      self.assertEqual(layer_defn.GetFieldDefn(4).GetType(), ogr.OFTDateTime)
      self.assertEqual(feat['dtInTimePosition'], '9999/12/31 23:59:60.999')

    src_bytes = self._read_testdata_bytes('gml/datetime.gml')
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_datetime'):
      tmp_filename = '/vsimem/ogr_gml_datetime/out.gml'
      gdal.FileFromMemBuffer(tmp_filename, src_bytes)
      with ogr.Open(tmp_filename) as ds:
        layer = ds.GetLayer(0)
        feat = layer.GetNextFeature()
        layer_defn = layer.GetLayerDefn()
        self.assertEqual(layer_defn.GetFieldDefn(1).GetType(), ogr.OFTTime)
        self.assertEqual(feat['time'], '23:59:60')
        self.assertEqual(layer_defn.GetFieldDefn(2).GetType(), ogr.OFTDate)
        self.assertEqual(feat['date'], '9999/12/31')
        self.assertEqual(layer_defn.GetFieldDefn(3).GetType(), ogr.OFTDateTime)
        self.assertEqual(feat['datetime'], '9999/12/31 23:59:60.999')
        self.assertEqual(layer_defn.GetFieldDefn(4).GetType(), ogr.OFTDateTime)
        self.assertEqual(feat['timePosition'], '9999/12/31 23:59:60.999')

  def test_ogr_gml_multiple_geom_elements_different_last_element(
      self,
  ) -> None:
    src_bytes = self._read_testdata_bytes(
        'gml/multiple_geom_elements_different_last_element.gml'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_gml_multi_geom_diff'):
      tmpname = '/vsimem/ogr_gml_multi_geom_diff/test.xml'
      gdal.FileFromMemBuffer(tmpname, src_bytes)
      with ogr.Open(tmpname) as ds:
        layer = ds.GetLayer(0)
        self.assertEqual(layer.GetGeometryColumn(), '')
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToIsoWkt(), 'POINT (3 4)')
        feat = layer.GetNextFeature()
        self.assertEqual(feat.GetGeometryRef().ExportToIsoWkt(), 'POINT (7 8)')

  # Note: test_ogr_gml_multiple_geom_elements_same_last_element relies on
  # upstream commit 5f5f34b60a (post-ea75dfb9) to report GetGeometryColumn() ==
  # 'secondGeom' consistently; test_ogr_gml_ossfuzz_487160964 exercises the
  # invalid cross-linked file without crashing (leak fix in upstream 589cda23ee).
  def test_ogr_gml_ossfuzz_487160964(self) -> None:
    tempdir = self._copy_testdata(['gml/ossfuzz_487160964.gml'])
    with gdal.quiet_errors(), ogr.ExceptionMgr(useExceptions=False):
      ds = ogr.Open(os.path.join(tempdir, 'ossfuzz_487160964.gml'))
      if ds is not None:
        ds.Close()

  @parameterized.parameters(
      '1.wkt',
      '2.wkt',
      '3.wkt',
      '4.wkt',
      '5.wkt',
      '6.wkt',
      '7.wkt',
      '8.wkt',
      '9.wkt',
      '10.wkt',
      '11.wkt',
      '3d_1.wkt',
      '3d_broken_line.wkt',
      '3d_broken_point.wkt',
      '3d_broken_polygon.wkt',
      'db2_poly.wkt',
  )
  def test_gml_geom(self, filename: str) -> None:
    wkt_path = ogr_util.GetTestFilePath(os.path.join('wkb_wkt', filename))
    with open(wkt_path, 'rt', encoding='utf-8') as f:
      raw_wkt = f.read()
    geom_wkt = ogr.CreateGeometryFromWkt(raw_wkt)
    gml = geom_wkt.ExportToGML()
    self.assertTrue(gml)
    geom_gml = ogr.CreateGeometryFromGML(gml)
    self.CheckFeatureGeometry(geom_wkt, geom_gml, 1e-13)

  def test_gml_space_test(self) -> None:
    gml = (
        '<gml:LineString xmlns:foo="http://bar">'
        '<gml:coordinates xmlns:foo="http://bar" decimal="." cs="," ts=" ">'
        '189999.99995605,624999.99998375 200000.00005735,624999.99998375 '
        '200000.00005735,612499.99997125 195791.3593843,612499.99997125 '
        '193327.3749823,612499.99997125 189999.99995605,612499.99997125 '
        '189999.99995605,619462.31247125 189999.99995605,624999.99998375 \n'
        '</gml:coordinates></gml:LineString>'
    )
    geom = ogr.CreateGeometryFromGML(gml)
    self.assertIsNotNone(geom)
    self.assertEqual(geom.GetGeometryType(), ogr.wkbLineString)
    self.assertEqual(geom.GetPointCount(), 8)

  def test_gml_polygon(self) -> None:
    gml = (
        '<Polygon><exterior><LinearRing><posList>0 0 4 0 4 4 0 4 0 0'
        '</posList></LinearRing></exterior><interior xmlns:foo="http://bar">'
        '<LinearRing><posList xmlns:foo="http://bar">1 1 2 1 2 2 1 2 1 1'
        '</posList></LinearRing></interior></Polygon>'
    )
    geom = ogr.CreateGeometryFromGML(gml)
    self.assertEqual(
        geom.ExportToWkt(),
        'POLYGON ((0 0,4 0,4 4,0 4,0 0),(1 1,2 1,2 2,1 2,1 1))',
    )

  # Note: upstream test_gml_posList_line3d_srsName_only (commit e9fb8e57d9)
  # and test_gml_export_compound_crs (commit 7d1f90dbc8) require C++ fixes
  # landed after Google3's ea75dfb98cb snapshot.

  def _create_gml_with_srs_from_wkt(self, wkt: str, epsg: int) -> str:
    geom = ogr.CreateGeometryFromWkt(wkt)
    self.assertIsNotNone(geom)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(epsg)
    geom.AssignSpatialReference(srs)
    return geom.ExportToGML()

  @parameterized.named_parameters(
      dict(
          testcase_name='point',
          wkt='POINT(21.675 53.763)',
          wkt_file=None,
          expected_prefix='<gml:Point srsName="EPSG:4326">',
          check_no_nested_srsname=False,
      ),
      dict(
          testcase_name='point3d',
          wkt='POINT(21.675 53.763 100)',
          wkt_file=None,
          expected_prefix='<gml:Point srsName="EPSG:4326">',
          check_no_nested_srsname=False,
      ),
      dict(
          testcase_name='linestring',
          wkt=None,
          wkt_file='wkb_wkt/5.wkt',
          expected_prefix='<gml:LineString srsName="EPSG:4326">',
          check_no_nested_srsname=False,
      ),
      dict(
          testcase_name='polygon',
          wkt=None,
          wkt_file='wkb_wkt/6.wkt',
          expected_prefix='<gml:Polygon srsName="EPSG:4326">',
          check_no_nested_srsname=False,
      ),
      dict(
          testcase_name='multipoint',
          wkt=None,
          wkt_file='wkb_wkt/11.wkt',
          expected_prefix='<gml:MultiPoint srsName="EPSG:4326">',
          check_no_nested_srsname=False,
      ),
      dict(
          testcase_name='multilinestring',
          wkt=None,
          wkt_file='wkb_wkt/2.wkt',
          expected_prefix='<gml:MultiLineString srsName="EPSG:4326">',
          check_no_nested_srsname=False,
      ),
      dict(
          testcase_name='multipolygon',
          wkt=None,
          wkt_file='wkb_wkt/4.wkt',
          expected_prefix='<gml:MultiPolygon srsName="EPSG:4326">',
          check_no_nested_srsname=True,
      ),
      dict(
          testcase_name='geometrycollection',
          wkt=None,
          wkt_file='wkb_wkt/3.wkt',
          expected_prefix='<gml:MultiGeometry srsName="EPSG:4326">',
          check_no_nested_srsname=False,
      ),
  )
  def test_gml_out_srs(
      self,
      wkt: str | None,
      wkt_file: str | None,
      expected_prefix: str,
      check_no_nested_srsname: bool,
  ) -> None:
    if wkt_file is not None:
      with open(ogr_util.GetTestFilePath(wkt_file), 'rt', encoding='utf-8') as f:
        wkt = f.read()
    assert wkt is not None
    gml = self._create_gml_with_srs_from_wkt(wkt, 4326)
    self.assertTrue(gml.startswith(expected_prefix), gml)
    if check_no_nested_srsname:
      self.assertNotIn('srsName', gml[len(expected_prefix) :])

  def test_gml_Box(self) -> None:
    gml = """<gml:Box xmlns:gml="http://www.opengis.net/gml" srsName="foo">
  <gml:coord>
    <gml:X>1</gml:X>
    <gml:Y>2</gml:Y>
  </gml:coord>
  <gml:coord>
    <gml:X>3</gml:X>
    <gml:Y>4</gml:Y>
  </gml:coord>
</gml:Box>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'POLYGON ((1 2,3 2,3 4,1 4,1 2))',
    )

  def test_gml_null(self) -> None:
    gdal.ErrorReset()
    geom = ogr.CreateGeometryFromGML('<gml:null>unknown</gml:null>')
    self.assertIsNone(geom)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_gml_Envelope(self) -> None:
    gml = """<gml:Envelope xmlns:gml="http://www.opengis.net/gml" srsName="foo">
    <gml:lowerCorner>1 2</gml:lowerCorner>
    <gml:upperCorner>3 4</gml:upperCorner>
</gml:Envelope>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'POLYGON ((1 2,3 2,3 4,1 4,1 2))',
    )

  def test_gml_Curve(self) -> None:
    gml = """<gml:Curve xmlns:gml="http://www.opengis.net/gml" srsName="foo">
    <gml:segments xmlns:foo="http://bar">
        <gml:LineStringSegment xmlns:foo="http://bar">
            <gml:posList>1 2 3 4</gml:posList>
        </gml:LineStringSegment>
    </gml:segments>
</gml:Curve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(), 'LINESTRING (1 2,3 4)'
    )

  def test_gml_Curve_with_pointProperty(self) -> None:
    gml = """<gml:Curve xmlns:gml="http://www.opengis.net/gml" srsName="foo">
    <gml:segments>
        <gml:LineStringSegment>
            <gml:pos>1 2</gml:pos>
            <gml:pointProperty>
                <gml:Point>
                    <gml:pos>3 4</gml:pos>
                </gml:Point>
            </gml:pointProperty>
        </gml:LineStringSegment>
    </gml:segments>
</gml:Curve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(), 'LINESTRING (1 2,3 4)'
    )

  def test_gml_MultiCurve(self) -> None:
    gml = """<gml:MultiCurve xmlns:foo="http://bar" xmlns:gml="http://www.opengis.net/gml" srsName="foo">
    <gml:curveMember xmlns:foo="http://bar">
        <gml:LineString>
            <gml:posList>1 2 2 3</gml:posList>
        </gml:LineString>
    </gml:curveMember>
    <gml:curveMember>
        <gml:LineString>
            <gml:posList>3 4 4 5</gml:posList>
        </gml:LineString>
    </gml:curveMember>
</gml:MultiCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'MULTILINESTRING ((1 2,2 3),(3 4,4 5))',
    )

  def test_gml_MultiSurface(self) -> None:
    gml = """<gml:MultiSurface xmlns:gml="http://www.opengis.net/gml" srsName="foo">
    <gml:surfaceMember xmlns:foo="http://bar" xlink:role="main">
        <gml:Surface gml:id="id1">
            <gml:patches xmlns:foo="http://bar">
                <gml:PolygonPatch xmlns:foo="http://bar" interpolation="planar">
                    <gml:exterior>
                        <gml:LinearRing>
                            <gml:posList>1 2 3 4 5 6 1 2</gml:posList>
                        </gml:LinearRing>
                    </gml:exterior>
                    <gml:interior>
                        <gml:LinearRing>
                            <gml:posList>2 3 4 5 6 7 2 3</gml:posList>
                        </gml:LinearRing>
                    </gml:interior>
                    <gml:interior>
                        <gml:LinearRing>
                            <gml:posList>3 4 5 6 7 8 3 4</gml:posList>
                        </gml:LinearRing>
                    </gml:interior>
                </gml:PolygonPatch>
            </gml:patches>
        </gml:Surface>
    </gml:surfaceMember>
    <gml:surfaceMember>
        <gml:Surface>
            <gml:patches>
                <gml:PolygonPatch interpolation="planar">
                    <gml:exterior>
                        <gml:Ring>
                            <gml:curveMember>
                                <gml:Curve>
                                    <gml:segments>
                                        <gml:LineStringSegment>
                                            <gml:pos>4 5</gml:pos>
                                            <gml:pos>6 7</gml:pos>
                                        </gml:LineStringSegment>
                                    </gml:segments>
                                </gml:Curve>
                            </gml:curveMember>
                            <gml:curveMember>
                                <gml:Curve>
                                    <gml:segments>
                                        <gml:LineStringSegment>
                                            <gml:pos>6 7</gml:pos>
                                            <gml:pos>8 9</gml:pos>
                                        </gml:LineStringSegment>
                                    </gml:segments>
                                </gml:Curve>
                            </gml:curveMember>
                            <gml:curveMember>
                                <gml:Curve>
                                    <gml:segments>
                                        <gml:LineStringSegment>
                                            <gml:pos>8 9</gml:pos>
                                            <gml:pos>4 5</gml:pos>
                                        </gml:LineStringSegment>
                                    </gml:segments>
                                </gml:Curve>
                            </gml:curveMember>
                        </gml:Ring>
                    </gml:exterior>
                </gml:PolygonPatch>
            </gml:patches>
        </gml:Surface>
    </gml:surfaceMember>
</gml:MultiSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'MULTIPOLYGON (((1 2,3 4,5 6,1 2),(2 3,4 5,6 7,2 3),'
        '(3 4,5 6,7 8,3 4)),((4 5,6 7,8 9,4 5)))',
    )

  def test_gml_MultiSurface_surfaceMembers(self) -> None:
    gml = """<gml:MultiSurface xmlns:foo="http://bar">
          <gml:surfaceMembers xmlns:foo="http://bar">
            <gml:Surface xmlns:foo="http://bar">
              <gml:patches xmlns:foo="http://bar">
                <gml:PolygonPatch xmlns:foo="http://bar">
                  <gml:exterior xmlns:foo="http://bar">
                      <gml:LinearRing xmlns:foo="http://bar">
                          <gml:posList>1 2 3 4 5 6 1 2</gml:posList>
                      </gml:LinearRing>
                    </gml:exterior>
                    <gml:interior>
                      <gml:LinearRing>
                          <gml:posList>2 3 4 5 6 7 2 3</gml:posList>
                      </gml:LinearRing>
                    </gml:interior>
                </gml:PolygonPatch>
              </gml:patches>
            </gml:Surface>
            <gml:Surface>
              <gml:patches>
                <gml:PolygonPatch>
                  <gml:exterior>
                    <gml:LinearRing>
                      <gml:posList>3 4 5 6 7 8 3 4</gml:posList>
                    </gml:LinearRing>
                  </gml:exterior>
                </gml:PolygonPatch>
                <gml:PolygonPatch>
                  <gml:exterior>
                    <gml:LinearRing>
                      <gml:posList>30 40 50 60 70 80 30 40</gml:posList>
                    </gml:LinearRing>
                  </gml:exterior>
                </gml:PolygonPatch>
              </gml:patches>
            </gml:Surface>
          </gml:surfaceMembers>
        </gml:MultiSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'MULTIPOLYGON (((1 2,3 4,5 6,1 2),(2 3,4 5,6 7,2 3)),'
        '((3 4,5 6,7 8,3 4)),((30 40,50 60,70 80,30 40)))',
    )

  def test_gml_MultiCurve_curveMembers(self) -> None:
    gml = """<gml:MultiCurve xmlns:foo="http://bar">
          <gml:curveMembers xmlns:foo="http://bar">
            <gml:LineString xmlns:foo="http://bar">
                <gml:posList xmlns:foo="http://bar" srsDimension="2">0 0 1 1</gml:posList>
              </gml:LineString>
            </gml:curveMembers>
          </gml:MultiCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'MULTILINESTRING ((0 0,1 1))',
    )

  def test_gml_MultiGeometry_geometryMembers(self) -> None:
    gml = """<gml:MultiGeometry xmlns:foo="http://bar">
          <gml:geometryMembers xmlns:foo="http://bar">
            <gml:LineString xmlns:foo="http://bar">
                <gml:posList xmlns:foo="http://bar" srsDimension="2">0 0 1 1</gml:posList>
              </gml:LineString>
            </gml:geometryMembers>
          </gml:MultiGeometry>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'GEOMETRYCOLLECTION (LINESTRING (0 0,1 1))',
    )

  def test_gml_CompositeCurve_curveMembers(self) -> None:
    gml = """<gml:CompositeCurve xmlns:foo="http://bar">
          <gml:curveMembers xmlns:foo="http://bar">
            <gml:LineString xmlns:foo="http://bar">
                <gml:posList xmlns:foo="http://bar" srsDimension="2">0 0 1 1</gml:posList>
            </gml:LineString>
            <gml:LineString xmlns:foo="http://bar">
                <gml:posList xmlns:foo="http://bar" srsDimension="2">1 1 2 2</gml:posList>
            </gml:LineString>
          </gml:curveMembers>
        </gml:CompositeCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(), 'LINESTRING (0 0,1 1,2 2)'
    )

  def test_gml_MultiCurve_pointMembers(self) -> None:
    gml = """<gml:MultiPoint xmlns:foo="http://bar">
          <gml:pointMembers xmlns:foo="http://bar">
              <gml:Point xmlns:foo="http://bar">
                <gml:pos xmlns:foo="http://bar">0 0</gml:pos>
              </gml:Point>
              <gml:Point>
                <gml:pos>1 1</gml:pos>
              </gml:Point>
            </gml:pointMembers>
          </gml:MultiPoint>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(), 'MULTIPOINT (0 0,1 1)'
    )

  def test_gml_Solid(self) -> None:
    gml = """<gml:Solid xmlns:foo="http://bar" gml:id="UUID_cc5a9513-2d85-4f1a-869a-620400182e1f">
          <gml:exterior xmlns:foo="http://bar">
            <gml:CompositeSurface xmlns:foo="http://bar" gml:id="UUID_2c83341e-a9ce-4abe-9c40-b5208eed5588">
              <gml:surfaceMember xmlns:foo="http://bar">
                <gml:Polygon xmlns:foo="http://bar" gml:id="UUID_d8e4b04b-ce0a-441e-b940-5ab99fcf6112">
                  <gml:exterior xmlns:foo="http://bar">
                    <gml:LinearRing xmlns:foo="http://bar" gml:id="UUID_d8e4b04b-ce0a-441e-b940-5ab99fcf6112_0">
                      <gml:posList xmlns:foo="http://bar">1 2 0 3 4 0 5 6 0 1 2 0</gml:posList>
                    </gml:LinearRing>
                  </gml:exterior>
                </gml:Polygon>
              </gml:surfaceMember>
            </gml:CompositeSurface>
          </gml:exterior>
        </gml:Solid>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'POLYHEDRALSURFACE Z (((1 2 0,3 4 0,5 6 0,1 2 0)))',
    )

  def test_gml_OrientableSurface(self) -> None:
    gml = """<gml:OrientableSurface xmlns:foo="http://bar" orientation="+">
      <gml:baseSurface xmlns:foo="http://bar">
        <gml:Polygon>
          <gml:exterior>
            <gml:LinearRing>
              <gml:posList srsDimension="3">-213.475 24.989 0
              -213.475 24.989 8.0 -215.704 25.077 8.0 -215.704
              25.077 0 -213.475 24.989 0 </gml:posList>
            </gml:LinearRing>
          </gml:exterior>
        </gml:Polygon>
      </gml:baseSurface>
    </gml:OrientableSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'POLYGON ((-213.475 24.989 0,-213.475 24.989 8,-215.704 25.077 8,'
        '-215.704 25.077 0,-213.475 24.989 0))',
    )

  def test_gml_Triangle(self) -> None:
    gml = """<gml:Triangle xmlns:foo="http://bar">
                <gml:exterior>
                    <gml:LinearRing>
                        <gml:posList>0 0 0 1 1 1 0 0</gml:posList>
                     </gml:LinearRing>
                </gml:exterior>
             </gml:Triangle>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'TRIANGLE ((0 0,0 1,1 1,0 0))',
    )

    triangle = ogr.CreateGeometryFromWkt('TRIANGLE ((0 0,0 1,0 1,0 0))')
    self.assertEqual(
        triangle.ExportToGML(['FORMAT=GML3']),
        '<gml:Triangle><gml:exterior><gml:LinearRing>'
        '<gml:posList>0 0 0 1 0 1 0 0</gml:posList>'
        '</gml:LinearRing></gml:exterior></gml:Triangle>',
    )

  def test_gml_Rectangle(self) -> None:
    gml = """<gml:Rectangle xmlns:foo="http://bar">
                <gml:exterior>
                    <gml:LinearRing>
                        <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
                     </gml:LinearRing>
                </gml:exterior>
             </gml:Rectangle>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
    )

  def test_gml_PolyhedralSurface(self) -> None:
    gml_2_patches = """<gml:PolyhedralSurface>
                <gml:polygonPatches>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">1 2 3 4 5 6 7 8 9 1 2 3</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                    </gml:PolygonPatch>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">10 11 12 13 14 15 16 17 18 10 11 12</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                        <gml:interior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">19 20 21 22 23 24 25 26 27 19 20 21</gml:posList>
                            </gml:LinearRing>
                        </gml:interior>
                    </gml:PolygonPatch>
                </gml:polygonPatches>
            </gml:PolyhedralSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_2_patches).ExportToWkt(),
        'POLYHEDRALSURFACE Z (((1 2 3,4 5 6,7 8 9,1 2 3)),'
        '((10 11 12,13 14 15,16 17 18,10 11 12),'
        '(19 20 21,22 23 24,25 26 27,19 20 21)))',
    )

    gml_1_ring_each = """<gml:PolyhedralSurface>
                <gml:polygonPatches>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">1 2 3 4 5 6 7 8 9 1 2 3</gml:posList>
                            </gml:LinearRing></gml:exterior>
                        </gml:PolygonPatch>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">10 11 12 13 14 15 16 17 18 10 11 12</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                    </gml:PolygonPatch>
            </gml:polygonPatches>
            </gml:PolyhedralSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_1_ring_each).ExportToWkt(),
        'POLYHEDRALSURFACE Z (((1 2 3,4 5 6,7 8 9,1 2 3)),'
        '((10 11 12,13 14 15,16 17 18,10 11 12)))',
    )

    for empty_gml in (
        '<gml:PolyhedralSurface></gml:PolyhedralSurface>',
        '<gml:PolyhedralSurface><gml:polygonPatches></gml:polygonPatches>'
        '</gml:PolyhedralSurface>',
        '<gml:PolyhedralSurface><gml:polygonPatches><gml:PolygonPatch>'
        '<gml:exterior></gml:exterior></gml:PolygonPatch>'
        '</gml:polygonPatches></gml:PolyhedralSurface>',
    ):
      self.assertEqual(
          ogr.CreateGeometryFromGML(empty_gml).ExportToWkt(),
          'POLYHEDRALSURFACE EMPTY',
      )

    wkt_cube = (
        'POLYHEDRALSURFACE Z (((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)),'
        '((0 0 0,0 1 0,1 1 0,1 0 0,0 0 0)),'
        '((0 0 0,1 0 0,1 0 1,0 0 1,0 0 0)),'
        '((1 1 0,1 1 1,1 0 1,1 0 0,1 1 0)),'
        '((0 1 0,0 1 1,1 1 1,1 1 0,0 1 0)),'
        '((0 0 1,1 0 1,1 1 1,0 1 1,0 0 1)))'
    )
    cube = ogr.CreateGeometryFromWkt(wkt_cube)
    cube_gml = cube.ExportToGML(['FORMAT=GML3'])
    self.assertEqual(
        cube_gml,
        '<gml:PolyhedralSurface><gml:polygonPatches>'
        '<gml:PolygonPatch><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">0 0 0 0 0 1 0 1 1 0 1 0 0 0 0'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>'
        '<gml:PolygonPatch><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">0 0 0 0 1 0 1 1 0 1 0 0 0 0 0'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>'
        '<gml:PolygonPatch><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">0 0 0 1 0 0 1 0 1 0 0 1 0 0 0'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>'
        '<gml:PolygonPatch><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">1 1 0 1 1 1 1 0 1 1 0 0 1 1 0'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>'
        '<gml:PolygonPatch><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">0 1 0 0 1 1 1 1 1 1 1 0 0 1 0'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>'
        '<gml:PolygonPatch><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">0 0 1 1 0 1 1 1 1 0 1 1 0 0 1'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:PolygonPatch>'
        '</gml:polygonPatches></gml:PolyhedralSurface>',
    )
    self.assertGeomEqual(ogr.CreateGeometryFromGML(cube_gml), cube)

    empty_ps = ogr.CreateGeometryFromWkt('POLYHEDRALSURFACE EMPTY')
    self.assertEqual(
        empty_ps.ExportToGML(['FORMAT=GML3']),
        '<gml:PolyhedralSurface><gml:polygonPatches>'
        '</gml:polygonPatches></gml:PolyhedralSurface>',
    )

    gml_multi_patches = """<gml:PolyhedralSurface>
                <gml:polygonPatches>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">1 2 3 4 5 6 7 8 9 1 2 3</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                    </gml:PolygonPatch>
                    <!--- --->
                </gml:polygonPatches>
                <!--- --->
                <gml:polygonPatches>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">1 2 3 4 5 6 7 8 9 1 2 3</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                    </gml:PolygonPatch>
                </gml:polygonPatches>
            </gml:PolyhedralSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_multi_patches).ExportToWkt(),
        'GEOMETRYCOLLECTION ('
        'POLYHEDRALSURFACE (((1 2 3,4 5 6,7 8 9,1 2 3))),'
        'POLYHEDRALSURFACE (((1 2 3,4 5 6,7 8 9,1 2 3))))',
    )

    gml_curved_patch = """<gml:PolyhedralSurface>
                <gml:polygonPatches>
                    <gml:PolygonPatch interpolation="planar">
                        <gml:exterior>
                            <gml:Ring>
                                <gml:curveMember>
                                    <gml:Curve>
                                        <gml:segments>
                                            <gml:LineStringSegment interpolation="linear">
                                                <gml:pos>0 -1</gml:pos>
                                                <gml:pos>0 1</gml:pos>
                                            </gml:LineStringSegment>
                                            <gml:Arc interpolation="circularArc3Points" numArc="1">
                                                <gml:pos>0 1</gml:pos>
                                                <gml:pos>1 0</gml:pos>
                                                <gml:pos>0 -1</gml:pos>
                                            </gml:Arc>
                                        </gml:segments>
                                    </gml:Curve>
                                </gml:curveMember>
                            </gml:Ring>
                        </gml:exterior>
                    </gml:PolygonPatch>
                </gml:polygonPatches>
            </gml:PolyhedralSurface>"""
    self.assertIn(
        'POLYHEDRALSURFACE (((0 -1,0 1,',
        ogr.CreateGeometryFromGML(gml_curved_patch).ExportToWkt(),
    )

  def test_gml_Tin(self) -> None:
    gml = """<gml:Tin xmlns:foo="http://bar">
                <gml:patches xmlns:foo="http://bar">
                    <gml:Triangle xmlns:foo="http://bar">
                        <gml:exterior xmlns:foo="http://bar">
                            <gml:LinearRing xmlns:foo="http://bar">
                                <gml:posList srsDimension="3">0 0 1 0 1 1 1 1 1 1 0 1 0 0 1 </gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                     </gml:Triangle>
                 </gml:patches>
             </gml:Tin>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'TIN Z (((0 0 1,0 1 1,1 1 1,1 0 1,0 0 1)))',
    )

    expected_wkt = (
        'TIN Z (((0 0 0,0 0 1,0 1 0,0 0 0)),((0 0 0,0 1 0,1 1 0,0 0 0)))'
    )
    for root_tag, patches_tag in (
        ('TriangulatedSurface', 'patches'),
        ('TriangulatedSurface', 'trianglePatches'),
        ('Surface', 'trianglePatches'),
    ):
      gml_variant = f"""<gml:{root_tag}>
                <gml:{patches_tag}>
                    <gml:Triangle>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">0 0 0 0 0 1 0 1 0 0 0 0</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                    </gml:Triangle>
                    <gml:Triangle>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">0 0 0 0 1 0 1 1 0 0 0 0</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                    </gml:Triangle>
                </gml:{patches_tag}>
            </gml:{root_tag}>"""
      self.assertEqual(
          ogr.CreateGeometryFromGML(gml_variant).ExportToWkt(), expected_wkt
      )

    tin = ogr.CreateGeometryFromWkt(expected_wkt)
    self.assertEqual(
        tin.ExportToGML(['FORMAT=GML3']),
        '<gml:TriangulatedSurface><gml:patches>'
        '<gml:Triangle><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">0 0 0 0 0 1 0 1 0 0 0 0'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:Triangle>'
        '<gml:Triangle><gml:exterior><gml:LinearRing>'
        '<gml:posList srsDimension="3">0 0 0 0 1 0 1 1 0 0 0 0'
        '</gml:posList></gml:LinearRing></gml:exterior></gml:Triangle>'
        '</gml:patches></gml:TriangulatedSurface>',
    )

  def test_gml_Surface_with_curve_polygon_then_curve_polygon(self) -> None:
    gml = """<gml:Surface>
                <gml:patches>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
                             </gml:LinearRing>
                        </gml:exterior>
                     </gml:PolygonPatch>
                    <gml:PolygonPatch interpolation="planar">
                     <gml:exterior>
                      <gml:Ring>
                       <gml:curveMember>
                        <gml:Curve>
                         <gml:segments>
                          <gml:LineStringSegment interpolation="linear">
                           <gml:pos>0 -1</gml:pos>
                           <gml:pos>0 1</gml:pos>
                          </gml:LineStringSegment>
                          <gml:Arc interpolation="circularArc3Points" numArc="1">
                           <gml:pos>0 1</gml:pos>
                           <gml:pos>1 0</gml:pos>
                           <gml:pos>0 -1</gml:pos>
                          </gml:Arc>
                         </gml:segments>
                        </gml:Curve>
                       </gml:curveMember>
                      </gml:Ring></gml:exterior>
                     </gml:PolygonPatch>
                </gml:patches>
            </gml:Surface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToIsoWkt(),
        'MULTISURFACE (((0 0,0 1,1 1,1 0,0 0)),'
        'CURVEPOLYGON (COMPOUNDCURVE ((0 -1,0 1),CIRCULARSTRING (0 1,1 0,0 -1))))',
    )

  def test_gml_Surface_with_curve_polygon_then_polygon_then_curve_polygon(
      self,
  ) -> None:
    gml = """<gml:Surface>
                <gml:patches>
                    <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
                             </gml:LinearRing>
                        </gml:exterior>
                     </gml:PolygonPatch>
                     <gml:PolygonPatch>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
                             </gml:LinearRing>
                        </gml:exterior>
                     </gml:PolygonPatch>
                    <gml:PolygonPatch interpolation="planar">
                     <gml:exterior>
                      <gml:Ring>
                       <gml:curveMember>
                        <gml:Curve>
                         <gml:segments>
                          <gml:LineStringSegment interpolation="linear">
                           <gml:pos>0 -1</gml:pos>
                           <gml:pos>0 1</gml:pos>
                          </gml:LineStringSegment>
                          <gml:Arc interpolation="circularArc3Points" numArc="1">
                           <gml:pos>0 1</gml:pos>
                           <gml:pos>1 0</gml:pos>
                           <gml:pos>0 -1</gml:pos>
                          </gml:Arc>
                         </gml:segments>
                        </gml:Curve>
                       </gml:curveMember>
                      </gml:Ring></gml:exterior>
                     </gml:PolygonPatch>
                </gml:patches>
            </gml:Surface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToIsoWkt(),
        'MULTISURFACE (((0 0,0 1,1 1,1 0,0 0)),((0 0,0 1,1 1,1 0,0 0)),'
        'CURVEPOLYGON (COMPOUNDCURVE ((0 -1,0 1),CIRCULARSTRING (0 1,1 0,0 -1))))',
    )

  def test_gml_Surface_mix_of_triangle_and_rectangle(self) -> None:
    gml = """<gml:Surface>
                <gml:patches>
                    <gml:Triangle>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">0 0 0 0 0 1 0 1 0 0 0 0</gml:posList>
                            </gml:LinearRing>
                        </gml:exterior>
                    </gml:Triangle>
                    <gml:Rectangle>
                        <gml:exterior>
                            <gml:LinearRing>
                                <gml:posList srsDimension="3">0 0 10 0 1 10 1 1 10 1 0 10 0 0 10</gml:posList>
                             </gml:LinearRing>
                        </gml:exterior>
                     </gml:Rectangle>
                </gml:patches>
            </gml:Surface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToIsoWkt(),
        'GEOMETRYCOLLECTION Z (TRIANGLE Z ((0 0 0,0 0 1,0 1 0,0 0 0)),'
        'POLYGON Z ((0 0 10,0 1 10,1 1 10,1 0 10,0 0 10)))',
    )

  def test_gml_ConcatenatedDeduplication(self) -> None:
    gml = """<gml:Surface>
       <gml:patches>
        <gml:PolygonPatch interpolation="planar">
         <gml:exterior>
          <gml:Ring>
           <gml:curveMember>
            <gml:Curve>
             <gml:segments>
              <gml:LineStringSegment interpolation="linear">
               <gml:pos>0 -1</gml:pos>
               <gml:pos>0 1</gml:pos>
              </gml:LineStringSegment>
              <gml:Arc interpolation="circularArc3Points" numArc="1">
               <gml:pos>0 1</gml:pos>
               <gml:pos>1 0</gml:pos>
               <gml:pos>0 -1</gml:pos>
              </gml:Arc>
             </gml:segments>
            </gml:Curve>
           </gml:curveMember>
          </gml:Ring></gml:exterior>
         </gml:PolygonPatch>
        </gml:patches>
       </gml:Surface>"""
    geom = ogr.CreateGeometryFromGML(gml)
    expected_wkt = (
        'CURVEPOLYGON (COMPOUNDCURVE ((0 -1,0 1),'
        'CIRCULARSTRING (0 1,1 0,0 -1)))'
    )
    self.CheckFeatureGeometry(geom, expected_wkt)
    self.assertTrue(geom.IsValid())

  def test_gml_out_precision(self) -> None:
    geom = ogr.CreateGeometryFromWkt('POINT(93538.15 1.23456789)')
    self.assertEqual(
        geom.ExportToGML(),
        '<gml:Point><gml:coordinates>93538.15,1.23456789'
        '</gml:coordinates></gml:Point>',
    )
    geom = ogr.CreateGeometryFromWkt('POINT(93538.55 1234567.89)')
    self.assertEqual(
        geom.ExportToGML(),
        '<gml:Point><gml:coordinates>93538.55,1234567.89'
        '</gml:coordinates></gml:Point>',
    )

  def test_gml_invalid_geoms(self) -> None:
    cases = [
        ('<?xml version="1.0" encoding="UTF-8"?>', None),
        ('<!-- bla -->', None),
        ('<foo/>', None),
        (
            '<gml:Point><gml:pos>31 29 16</gml:pos>'
            '<gml:pos>31 29 16</gml:pos></gml:Point>',
            None,
        ),
        ('<gml:Point><gml:coordinates/></gml:Point>', 'POINT EMPTY'),
        ('<gml:Point><gml:coordinates>0</gml:coordinates></gml:Point>', None),
        (
            '<gml:Point><gml:coordinates>0 1</gml:coordinates></gml:Point>',
            'POINT (0 1)',
        ),
        (
            '<gml:Point><gml:coordinates>0 1 2</gml:coordinates></gml:Point>',
            'POINT (0 1 2)',
        ),
        (
            '<gml:Point><gml:coordinates>0,1 2,3</gml:coordinates></gml:Point>',
            None,
        ),
        ('<gml:Point><gml:pos>0</gml:pos></gml:Point>', None),
        ('<gml:Point><gml:pos/></gml:Point>', 'POINT EMPTY'),
        ('<gml:Point/>', None),
        ('<gml:Point><foo/></gml:Point>', None),
        ('<gml:LineString/>', None),
        ('<gml:LineString><foo/></gml:LineString>', None),
        (
            '<gml:LineString><gml:posList></gml:posList></gml:LineString>',
            'LINESTRING EMPTY',
        ),
        (
            '<gml:LineString><gml:posList>0</gml:posList></gml:LineString>',
            None,
        ),
        (
            '<gml:LineString><gml:posList srsDimension="4">0 1 2 3'
            '</gml:posList></gml:LineString>',
            None,
        ),
        (
            '<gml:LineString><gml:posList srsDimension="3">0 1 2 3'
            '</gml:posList></gml:LineString>',
            None,
        ),
        ('<gml:Point><gml:coord></gml:coord></gml:Point>', None),
        (
            '<gml:Point><gml:coord><gml:X/><gml:Y/></gml:coord></gml:Point>',
            None,
        ),
        ('<gml:Point><gml:coord><gml:X>0</gml:X></gml:coord></gml:Point>', None),
        ('<gml:Polygon/>', 'POLYGON EMPTY'),
        ('<gml:Polygon><gml:outerBoundaryIs/></gml:Polygon>', 'POLYGON EMPTY'),
        (
            '<gml:Polygon><gml:outerBoundaryIs><foo/></gml:outerBoundaryIs>'
            '</gml:Polygon>',
            None,
        ),
        (
            '<gml:Polygon><gml:outerBoundaryIs><gml:Point><gml:pos>31 29 16'
            '</gml:pos></gml:Point></gml:outerBoundaryIs></gml:Polygon>',
            None,
        ),
        (
            '<gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:posList>'
            '0 1 2 3 4 5 0 1</gml:posList></gml:LinearRing>'
            '</gml:outerBoundaryIs><gml:innerBoundaryIs/></gml:Polygon>',
            None,
        ),
        (
            '<gml:Polygon><gml:outerBoundaryIs><gml:LinearRing/>'
            '</gml:outerBoundaryIs><gml:innerBoundaryIs/></gml:Polygon>',
            None,
        ),
        (
            '<gml:Polygon><gml:outerBoundaryIs><gml:LinearRing><gml:posList>'
            '0 1 2 3 4 5 0 1</gml:posList></gml:LinearRing>'
            '</gml:outerBoundaryIs><gml:innerBoundaryIs><gml:Point><gml:pos>'
            '31 29 16</gml:pos></gml:Point></gml:innerBoundaryIs></gml:Polygon>',
            None,
        ),
        (
            '<gml:Polygon><gml:exterior><gml:CompositeCurve/></gml:exterior>'
            '</gml:Polygon>',
            None,
        ),
        (
            '<gml:Polygon><gml:exterior><gml:LinearRing><gml:posList>'
            '0 0 4 0 4 4 0 4 0 0<gml:/posList><gml:/LinearRing>'
            '<gml:/exterior><gml:interior><gml:CompositeCurve/>'
            '</gml:interior></gml:Polygon>',
            None,
        ),
        ('<gml:Ring/>', None),
        ('<gml:Ring><foo/></gml:Ring>', None),
        ('<gml:Ring><gml:curveMember/></gml:Ring>', None),
        ('<gml:Ring><gml:curveMember><foo/></gml:curveMember></gml:Ring>', None),
        (
            '<gml:Ring><gml:curveMember><gml:Point><gml:pos>31 29 16</gml:pos>'
            '</gml:Point></gml:curveMember></gml:Ring>',
            None,
        ),
        ('<gml:Box/>', None),
        ('<gml:Box><gml:pos>31 29 16</gml:pos></gml:Box>', None),
        ('<gml:MultiPolygon/>', 'MULTIPOLYGON EMPTY'),
        ('<gml:MultiPolygon><foo/></gml:MultiPolygon>', 'MULTIPOLYGON EMPTY'),
        (
            '<gml:MultiPolygon><gml:polygonMember/></gml:MultiPolygon>',
            'MULTIPOLYGON EMPTY',
        ),
        (
            '<gml:MultiPolygon><gml:polygonMember><foo/></gml:polygonMember>'
            '</gml:MultiPolygon>',
            None,
        ),
        (
            '<gml:MultiPolygon><gml:polygonMember><gml:Point><gml:pos>31 29 16'
            '</gml:pos></gml:Point></gml:polygonMember></gml:MultiPolygon>',
            None,
        ),
        (
            '<gml:MultiSurface><gml:surfaceMembers/></gml:MultiSurface>',
            'MULTIPOLYGON EMPTY',
        ),
        (
            '<gml:MultiSurface><gml:surfaceMembers><foo/></gml:surfaceMembers>'
            '</gml:MultiSurface>',
            'MULTIPOLYGON EMPTY',
        ),
        (
            '<gml:MultiSurface><gml:surfaceMembers><gml:Polygon/>'
            '</gml:surfaceMembers></gml:MultiSurface>',
            'MULTIPOLYGON EMPTY',
        ),
        ('<gml:MultiPoint/>', 'MULTIPOINT EMPTY'),
        ('<gml:MultiPoint><foo/></gml:MultiPoint>', 'MULTIPOINT EMPTY'),
        (
            '<gml:MultiPoint><gml:pointMember/></gml:MultiPoint>',
            'MULTIPOINT EMPTY',
        ),
        (
            '<gml:MultiPoint><gml:pointMember><gml:LineString><gml:posList>'
            '0 1 2 3</gml:posList></gml:LineString></gml:pointMember>'
            '</gml:MultiPoint>',
            None,
        ),
        (
            '<gml:MultiPoint><gml:pointMembers></gml:pointMembers>'
            '</gml:MultiPoint>',
            'MULTIPOINT EMPTY',
        ),
        (
            '<gml:MultiPoint><gml:pointMembers><foo/></gml:pointMembers>'
            '</gml:MultiPoint>',
            'MULTIPOINT EMPTY',
        ),
        (
            '<gml:MultiPoint><gml:pointMembers><gml:Point/>'
            '</gml:pointMembers></gml:MultiPoint>',
            None,
        ),
        ('<gml:MultiLineString/>', 'MULTILINESTRING EMPTY'),
        (
            '<gml:MultiLineString><foo/></gml:MultiLineString>',
            'MULTILINESTRING EMPTY',
        ),
        (
            '<gml:MultiLineString><gml:lineStringMember/>'
            '</gml:MultiLineString>',
            None,
        ),
        (
            '<gml:MultiLineString><gml:lineStringMember><gml:Point><gml:pos>'
            '31 29 16</gml:pos></gml:Point></gml:lineStringMember>'
            '</gml:MultiLineString>',
            None,
        ),
        ('<gml:MultiCurve/>', 'MULTILINESTRING EMPTY'),
        ('<gml:MultiCurve><foo/></gml:MultiCurve>', 'MULTILINESTRING EMPTY'),
        (
            '<gml:MultiCurve><gml:curveMember/></gml:MultiCurve>',
            'MULTILINESTRING EMPTY',
        ),
        (
            '<gml:MultiCurve><gml:curveMember><foo/></gml:curveMember>'
            '</gml:MultiCurve>',
            None,
        ),
        (
            '<gml:MultiCurve><gml:curveMember><gml:Curve/></gml:curveMember>'
            '</gml:MultiCurve>',
            None,
        ),
        (
            '<gml:MultiCurve><gml:curveMember><gml:Curve><foo/></gml:Curve>'
            '</gml:curveMember></gml:MultiCurve>',
            None,
        ),
        (
            '<gml:MultiCurve><gml:curveMember><gml:Curve><gml:Point><gml:pos>'
            '31 29 16</gml:pos></gml:Point></gml:Curve></gml:curveMember>'
            '</gml:MultiCurve>',
            None,
        ),
        (
            '<gml:MultiCurve><gml:curveMembers></gml:curveMembers>'
            '</gml:MultiCurve>',
            'MULTILINESTRING EMPTY',
        ),
        (
            '<gml:MultiCurve><gml:curveMembers><foo/></gml:curveMembers>'
            '</gml:MultiCurve>',
            None,
        ),
        (
            '<gml:MultiCurve><gml:curveMembers><gml:LineString/>'
            '</gml:curveMembers></gml:MultiCurve>',
            None,
        ),
        ('<gml:Curve/>', None),
        ('<gml:Curve><foo/></gml:Curve>', None),
        ('<gml:Curve><gml:segments/></gml:Curve>', 'LINESTRING EMPTY'),
        ('<gml:Curve><gml:segments><foo/></gml:segments></gml:Curve>', None),
        (
            '<gml:Curve><gml:segments><gml:Point><gml:pos>31 29 16</gml:pos>'
            '</gml:Point></gml:segments></gml:Curve>',
            None,
        ),
        ('<gml:Arc/>', None),
        ('<gml:Arc><gml:posList>0 0 0 1</gml:posList></gml:Arc>', None),
        ('<gml:Arc><gml:posList>0 0 0 1 1 0 2 0</gml:posList></gml:Arc>', None),
        ('<gml:ArcString/>', None),
        (
            '<gml:ArcString><gml:posList>0 0 0 1</gml:posList></gml:ArcString>',
            None,
        ),
        (
            '<gml:ArcString><gml:posList>0 0 0 1 1 0 2 0</gml:posList>'
            '</gml:ArcString>',
            None,
        ),
        ('<gml:Circle/>', None),
        ('<gml:Circle><gml:posList>0 0 0 1</gml:posList></gml:Circle>', None),
        ('<gml:segments><foo/></gml:segments>', None),
        ('<gml:segments><gml:LineStringSegment/></gml:segments>', None),
        (
            '<gml:segments><gml:LineStringSegment><foo/>'
            '</gml:LineStringSegment></gml:segments>',
            None,
        ),
        (
            '<gml:segments><gml:LineStringSegment><gml:Point><gml:pos>31 29 16'
            '</gml:pos></gml:Point></gml:LineStringSegment></gml:segments>',
            None,
        ),
        ('<gml:MultiGeometry/>', 'GEOMETRYCOLLECTION EMPTY'),
        (
            '<gml:MultiGeometry><foo/></gml:MultiGeometry>',
            'GEOMETRYCOLLECTION EMPTY',
        ),
        (
            '<gml:MultiGeometry><gml:geometryMember/></gml:MultiGeometry>',
            'GEOMETRYCOLLECTION EMPTY',
        ),
        (
            '<gml:MultiGeometry><gml:geometryMember><foo/>'
            '</gml:geometryMember></gml:MultiGeometry>',
            None,
        ),
        (
            '<gml:MultiGeometry><gml:geometryMembers><foo/>'
            '</gml:geometryMembers></gml:MultiGeometry>',
            None,
        ),
        ('<gml:Surface/>', 'POLYGON EMPTY'),
        ('<gml:Surface><foo/></gml:Surface>', 'POLYGON EMPTY'),
        ('<gml:Surface><gml:patches/></gml:Surface>', 'POLYGON EMPTY'),
        ('<gml:Surface><gml:patches><foo/></gml:patches></gml:Surface>', None),
        (
            '<gml:Surface><gml:patches><gml:PolygonPatch/></gml:patches>'
            '</gml:Surface>',
            'POLYGON EMPTY',
        ),
        ('<gml:Solid/>', 'POLYHEDRALSURFACE EMPTY'),
        ('<gml:Solid><foo/></gml:Solid>', 'POLYHEDRALSURFACE EMPTY'),
        (
            '<gml:Solid><gml:exterior/></gml:Solid>',
            'POLYHEDRALSURFACE EMPTY',
        ),
        ('<gml:Solid><gml:exterior><foo/></gml:exterior></gml:Solid>', None),
        (
            '<gml:Solid><gml:exterior><Polygon><exterior><LinearRing>'
            '<posList srsDimension="2">0 0 4 0 4 4 0 4 0 0</posList>'
            '</LinearRing></exterior></Polygon></gml:exterior><gml:interior/>'
            '</gml:Solid>',
            'POLYGON ((0 0,4 0,4 4,0 4,0 0))',
        ),
        ('<gml:OrientableCurve/>', None),
        ('<gml:OrientableCurve><foo/></gml:OrientableCurve>', None),
        ('<gml:OrientableCurve><gml:baseCurve/></gml:OrientableCurve>', None),
        (
            '<gml:OrientableCurve><gml:baseCurve><foo/></gml:baseCurve>'
            '</gml:OrientableCurve>',
            None,
        ),
        (
            '<gml:OrientableCurve><gml:baseCurve><gml:Point><gml:pos>0 0'
            '</gml:pos></gml:Point></gml:baseCurve></gml:OrientableCurve>',
            None,
        ),
        ('<gml:OrientableSurface/>', None),
        ('<gml:OrientableSurface><foo/></gml:OrientableSurface>', None),
        (
            '<gml:OrientableSurface><gml:baseSurface/></gml:OrientableSurface>',
            None,
        ),
        (
            '<gml:OrientableSurface><gml:baseSurface><foo/></gml:baseSurface>'
            '</gml:OrientableSurface>',
            None,
        ),
        ('<gmlce:SimplePolygon/>', None),
        ('<gmlce:SimplePolygon><foo/></gmlce:SimplePolygon>', None),
        (
            '<gmlce:SimplePolygon><gml:posList/></gmlce:SimplePolygon>',
            'POLYGON EMPTY',
        ),
        ('<gmlce:SimpleMultiPoint/>', None),
        ('<gmlce:SimpleMultiPoint><foo/></gmlce:SimpleMultiPoint>', None),
        (
            '<gmlce:SimpleMultiPoint><gml:posList/></gmlce:SimpleMultiPoint>',
            'MULTIPOINT EMPTY',
        ),
        ('<gml:Envelope/>', None),
        (
            '<gml:Envelope><gml:lowerCorner/><gml:upperCorner/></gml:Envelope>',
            None,
        ),
        (
            '<gml:Envelope><gml:lowerCorner>1</gml:lowerCorner>'
            '<gml:upperCorner>3 4</gml:upperCorner/></gml:Envelope>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates cs="bla">1bla2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates cs="">1bla2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates cs="0">1bla2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates ts="bla">1,2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates ts="">1,2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates ts="0">1,2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates decimal="bla">1,2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates decimal="">1,2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Point><gml:coordinates decimal="0">1,2</gml:coordinates>'
            '</gml:Point>',
            None,
        ),
        (
            '<gml:Curve><gml:segments>'
            '<gml:Arc><gml:posList>0 0 1 0 0 0</gml:posList></gml:Arc>'
            '<gml:LineStringSegment><gml:posList>-10 0 -1 0 0 0'
            '</gml:posList></gml:LineStringSegment>'
            '</gml:segments></gml:Curve>',
            'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 0,0 0),(0 0,-1 0,-10 0))',
        ),
        (
            '<gml:CompositeCurve>'
            '<gml:curveMember><gml:Curve><gml:segments><gml:LineString>'
            '<gml:posList>0 0 1 0 0 0</gml:posList></gml:LineString>'
            '</gml:segments></gml:Curve></gml:curveMember>'
            '<gml:curveMember><gml:Curve><gml:segments>'
            '<gml:Arc><gml:posList>-10 0 1 0 0 0</gml:posList></gml:Arc>'
            '<gml:LineStringSegment><gml:posList>0 0 -1 0 0 0</gml:posList>'
            '</gml:LineStringSegment></gml:segments></gml:Curve>'
            '</gml:curveMember></gml:CompositeCurve>',
            None,
        ),
        (
            '<gml:ArcByBulge><gml:bulge>2</gml:bulge>'
            '<gml:normal>-1</gml:normal></gml:ArcByBulge>',
            None,
        ),
        (
            '<gml:ArcByBulge><gml:posList>2 0</gml:posList>'
            '<gml:bulge>2</gml:bulge><gml:normal>-1</gml:normal>'
            '</gml:ArcByBulge>',
            None,
        ),
        (
            '<gml:ArcByBulge><gml:posList>2 0 -2 0</gml:posList>'
            '<gml:normal>-1</gml:normal></gml:ArcByBulge>',
            None,
        ),
        ('<ArcByBulge><bulge/></ArcByBulge>', None),
        (
            '<gml:ArcByCenterPoint><gml:radius>2</gml:radius>'
            '<gml:startAngle>90</gml:startAngle><gml:endAngle>270'
            '</gml:endAngle></gml:ArcByCenterPoint>',
            None,
        ),
        (
            '<gml:ArcByCenterPoint><gml:pos>1 2</gml:pos>'
            '<gml:startAngle>90</gml:startAngle><gml:endAngle>270'
            '</gml:endAngle></gml:ArcByCenterPoint>',
            None,
        ),
        (
            '<gml:ArcByCenterPoint><gml:pos>1 2</gml:pos>'
            '<gml:radius>2</gml:radius<gml:endAngle>270</gml:endAngle>'
            '</gml:ArcByCenterPoint>',
            None,
        ),
        (
            '<gml:ArcByCenterPoint><gml:pos>1 2</gml:pos>'
            '<gml:radius>2</gml:radius><gml:startAngle>90</gml:startAngle>'
            '</gml:ArcByCenterPoint>',
            None,
        ),
        (
            '<gml:CircleByCenterPoint><gml:radius>2</gml:radius>'
            '</gml:CircleByCenterPoint>',
            None,
        ),
        (
            '<gml:CircleByCenterPoint><gml:pos>1 2</gml:pos>'
            '</gml:CircleByCenterPoint>',
            None,
        ),
        ('<gml:PolyhedralSurface><foo/></gml:PolyhedralSurface>', None),
        (
            '<gml:PolyhedralSurface><gml:polygonPatches><foo/>'
            '</gml:polygonPatches></gml:PolyhedralSurface>',
            None,
        ),
        (
            '<gml:PolyhedralSurface><gml:polygonPatches><gml:PolygonPatch>'
            '<gml:exterior><foo/></gml:exterior></gml:PolygonPatch>'
            '</gml:polygonPatches></gml:PolyhedralSurface>',
            None,
        ),
        (
            '<gml:Triangle><gml:exterior><gml:CompositeCurve/></gml:exterior>'
            '</gml:Triangle>',
            None,
        ),
        ('<gml:TriangulatedSurface></gml:TriangulatedSurface>', None),
        (
            '<gml:TriangulatedSurface><gml:patches><gml:Triangle/>'
            '</gml:patches></gml:TriangulatedSurface>',
            None,
        ),
    ]
    with gdal.quiet_errors(), ogr.ExceptionMgr(useExceptions=False):
      for gml, expected_wkt in cases:
        geom = ogr.CreateGeometryFromGML(gml)
        if expected_wkt is None:
          self.assertIsNone(geom, gml)
        else:
          self.assertIsNotNone(geom, gml)
          self.assertEqual(geom.ExportToWkt(), expected_wkt, gml)

  def test_gml_write_gml3_geometries(self) -> None:
    gml_list = [
        '<gml:Point><gml:pos>2 3</gml:pos></gml:Point>',
        '<gml:Point><gml:pos>2 3 4</gml:pos></gml:Point>',
        '<gml:LineString><gml:posList>2 3 4 5</gml:posList></gml:LineString>',
        '<gml:Curve><gml:segments><gml:LineStringSegment>'
        '<gml:posList>2 3 4 5</gml:posList></gml:LineStringSegment>'
        '</gml:segments></gml:Curve>',
        '<gml:LineString><gml:posList srsDimension="3">2 3 10 4 5 20'
        '</gml:posList></gml:LineString>',
        '<gml:Curve><gml:segments><gml:LineStringSegment>'
        '<gml:posList srsDimension="3">2 3 10 4 5 20</gml:posList>'
        '</gml:LineStringSegment></gml:segments></gml:Curve>',
        '<gml:Polygon><gml:exterior><gml:LinearRing>'
        '<gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>'
        '</gml:LinearRing></gml:exterior></gml:Polygon>',
        '<gml:Polygon><gml:exterior><gml:LinearRing>'
        '<gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>'
        '</gml:LinearRing></gml:exterior><gml:interior><gml:LinearRing>'
        '<gml:posList>10 10 10 11 11 11 10 10</gml:posList>'
        '</gml:LinearRing></gml:interior></gml:Polygon>',
        '<gml:MultiPoint><gml:pointMember><gml:Point><gml:pos>2 3</gml:pos>'
        '</gml:Point></gml:pointMember><gml:pointMember><gml:Point>'
        '<gml:pos>4 5</gml:pos></gml:Point></gml:pointMember></gml:MultiPoint>',
        '<gml:MultiCurve><gml:curveMember><gml:LineString>'
        '<gml:posList>0 1 2 3 4 5</gml:posList></gml:LineString>'
        '</gml:curveMember><gml:curveMember><gml:LineString>'
        '<gml:posList>6 7 8 9 10 11</gml:posList></gml:LineString>'
        '</gml:curveMember></gml:MultiCurve>',
        '<gml:MultiCurve><gml:curveMember><gml:Curve><gml:segments>'
        '<gml:LineStringSegment><gml:posList>0 1 2 3 4 5</gml:posList>'
        '</gml:LineStringSegment></gml:segments></gml:Curve></gml:curveMember>'
        '<gml:curveMember><gml:Curve><gml:segments><gml:LineStringSegment>'
        '<gml:posList>6 7 8 9 10 11</gml:posList></gml:LineStringSegment>'
        '</gml:segments></gml:Curve></gml:curveMember></gml:MultiCurve>',
        '<gml:MultiSurface><gml:surfaceMember><gml:Polygon><gml:exterior>'
        '<gml:LinearRing><gml:posList>0 1 2 3 4 5 0 1</gml:posList>'
        '</gml:LinearRing></gml:exterior></gml:Polygon></gml:surfaceMember>'
        '<gml:surfaceMember><gml:Polygon><gml:exterior><gml:LinearRing>'
        '<gml:posList>6 7 8 9 10 11 6 7</gml:posList></gml:LinearRing>'
        '</gml:exterior></gml:Polygon></gml:surfaceMember></gml:MultiSurface>',
        '<gml:MultiGeometry><gml:geometryMember><gml:Point>'
        '<gml:pos>0 1</gml:pos></gml:Point></gml:geometryMember>'
        '<gml:geometryMember><gml:LineString><gml:posList>2 3 4 5'
        '</gml:posList></gml:LineString></gml:geometryMember>'
        '</gml:MultiGeometry>',
    ]
    for gml_in in gml_list:
      geom = ogr.CreateGeometryFromGML(gml_in)
      opts = ['FORMAT=GML3']
      if '<gml:Curve' in gml_in:
        opts.append('GML3_LINESTRING_ELEMENT=curve')
      self.assertEqual(geom.ExportToGML(opts), gml_in)

  def test_gml_write_gml3_srs(self) -> None:
    sr32631 = osr.SpatialReference()
    sr32631.SetFromUserInput('EPSG:32631')
    srlonglat = osr.SpatialReference()
    srlonglat.SetFromUserInput('EPSG:4326')
    srlonglat.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    srlatlong = osr.SpatialReference()
    srlatlong.SetFromUserInput('EPSG:4326')
    srlatlong.SetAxisMappingStrategy(osr.OAMS_AUTHORITY_COMPLIANT)

    cases = [
        (
            'POINT(500000 4500000)',
            sr32631,
            ['FORMAT=GML3'],
            '<gml:Point srsName="urn:ogc:def:crs:EPSG::32631">'
            '<gml:pos>500000 4500000</gml:pos></gml:Point>',
        ),
        (
            'POINT(2 49)',
            srlonglat,
            ['FORMAT=GML3'],
            '<gml:Point srsName="urn:ogc:def:crs:EPSG::4326">'
            '<gml:pos>49 2</gml:pos></gml:Point>',
        ),
        (
            'POINT(49 2)',
            srlatlong,
            ['FORMAT=GML3'],
            '<gml:Point srsName="urn:ogc:def:crs:EPSG::4326">'
            '<gml:pos>49 2</gml:pos></gml:Point>',
        ),
        (
            'POINT(2 49)',
            srlonglat,
            ['FORMAT=GML3', 'GML3_LONGSRS=NO'],
            '<gml:Point srsName="EPSG:4326"><gml:pos>2 49</gml:pos></gml:Point>',
        ),
        (
            'POINT(2 49)',
            srlonglat,
            ['FORMAT=GML3', 'SRSNAME_FORMAT=SHORT'],
            '<gml:Point srsName="EPSG:4326"><gml:pos>2 49</gml:pos></gml:Point>',
        ),
        (
            'POINT(2 49)',
            srlonglat,
            ['FORMAT=GML3', 'SRSNAME_FORMAT=OGC_URN'],
            '<gml:Point srsName="urn:ogc:def:crs:EPSG::4326">'
            '<gml:pos>49 2</gml:pos></gml:Point>',
        ),
        (
            'POINT(2 49)',
            srlonglat,
            ['FORMAT=GML3', 'SRSNAME_FORMAT=OGC_URL'],
            '<gml:Point srsName="http://www.opengis.net/def/crs/EPSG/0/4326">'
            '<gml:pos>49 2</gml:pos></gml:Point>',
        ),
    ]
    for wkt, srs, opts, expected_gml in cases:
      geom = ogr.CreateGeometryFromWkt(wkt)
      geom.AssignSpatialReference(srs)
      self.assertEqual(geom.ExportToGML(options=opts), expected_gml)

  def test_gml_nested(self) -> None:
    gml_ok = (
        '<gml:MultiGeometry><gml:geometryMember>' * 31
        + '<gml:MultiPolygon></gml:MultiPolygon>'
        + '</gml:geometryMember></gml:MultiGeometry>' * 31
    )
    self.assertIsNotNone(ogr.CreateGeometryFromGML(gml_ok))

    gml_too_deep = (
        '<gml:MultiGeometry><gml:geometryMember>' * 32
        + '<gml:MultiPolygon></gml:MultiPolygon>'
        + '</gml:geometryMember></gml:MultiGeometry>' * 32
    )
    with gdal.quiet_errors(), self.assertRaises(Exception):
      ogr.CreateGeometryFromGML(gml_too_deep)

  @parameterized.named_parameters(
      dict(
          testcase_name='SimplePolygon',
          gml='<gmlce:SimplePolygon><gml:posList>0 0 1 0 1 1 0 1'
          '</gml:posList></gmlce:SimplePolygon>',
          expected_wkt='POLYGON ((0 0,1 0,1 1,0 1,0 0))',
      ),
      dict(
          testcase_name='SimpleRectangle',
          gml='<gmlce:SimpleRectangle><gml:posList>0 0 1 0 1 1 0 1'
          '</gml:posList></gmlce:SimpleRectangle>',
          expected_wkt='POLYGON ((0 0,1 0,1 1,0 1,0 0))',
      ),
      dict(
          testcase_name='SimpleTriangle',
          gml='<gmlce:SimpleTriangle><gml:posList>0 0 1 0 1 1'
          '</gml:posList></gmlce:SimpleTriangle>',
          expected_wkt='TRIANGLE ((0 0,1 0,1 1,0 0))',
      ),
      dict(
          testcase_name='SimpleMultiPoint',
          gml='<gmlce:SimpleMultiPoint><gml:posList>0 1 2 3'
          '</gml:posList></gmlce:SimpleMultiPoint>',
          expected_wkt='MULTIPOINT (0 1,2 3)',
      ),
  )
  def test_gml_Simple_types(self, gml: str, expected_wkt: str) -> None:
    self.assertEqual(ogr.CreateGeometryFromGML(gml).ExportToWkt(), expected_wkt)

  def test_gml_CompositeCurveInRing(self) -> None:
    gml = """<gml:Surface>
    <gml:patches>
        <gml:PolygonPatch interpolation="planar">
            <gml:exterior>
                <gml:Ring>
                    <gml:curveMember>
                        <gml:CompositeCurve>
                            <gml:curveMember>
                                <gml:Curve>
                                    <gml:segments>
                                        <gml:LineStringSegment>
                                            <gml:pos>0 0</gml:pos>
                                            <gml:pos>0 1</gml:pos>
                                        </gml:LineStringSegment>
                                    </gml:segments>
                                </gml:Curve>
                            </gml:curveMember>
                            <gml:curveMember>
                                <gml:Curve>
                                    <gml:segments>
                                        <gml:LineStringSegment>
                                            <gml:pos>0 1</gml:pos>
                                            <gml:pos>1 1</gml:pos>
                                        </gml:LineStringSegment>
                                    </gml:segments>
                                </gml:Curve>
                            </gml:curveMember>
                            <gml:curveMember>
                                <gml:Curve>
                                    <gml:segments>
                                        <gml:LineStringSegment>
                                            <gml:pos>1 1</gml:pos>
                                            <gml:pos>0 0</gml:pos>
                                        </gml:LineStringSegment>
                                    </gml:segments>
                                </gml:Curve>
                            </gml:curveMember>
                        </gml:CompositeCurve>
                    </gml:curveMember>
                </gml:Ring>
            </gml:exterior>
        </gml:PolygonPatch>
    </gml:patches>
</gml:Surface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'POLYGON ((0 0,0 1,1 1,0 0))',
    )

  def test_gml_CompositeSurface_in_surfaceMembers(self) -> None:
    gml = """<gml:MultiSurface>
          <gml:surfaceMembers>
            <gml:CompositeSurface>
              <gml:surfaceMember>
                <gml:Polygon>
                  <gml:exterior>
                    <gml:LinearRing>
                      <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
                    </gml:LinearRing>
                  </gml:exterior>
                </gml:Polygon>
              </gml:surfaceMember>
              <gml:surfaceMember>
                <gml:Polygon>
                  <gml:exterior>
                    <gml:LinearRing>
                      <gml:posList>2 0 2 1 3 1 3 0 2 0</gml:posList>
                    </gml:LinearRing>
                  </gml:exterior>
                </gml:Polygon>
              </gml:surfaceMember>
            </gml:CompositeSurface>
          </gml:surfaceMembers>
        </gml:MultiSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)),((2 0,2 1,3 1,3 0,2 0)))',
    )

  def test_gml_MultiSurfaceOfSurfaceOfPolygonPatchWithInteriorRing(
      self,
  ) -> None:
    gml = """<gml:MultiSurface>
               <gml:surfaceMember>
                 <gml:Surface>
                    <gml:patches>
                      <gml:PolygonPatch>
                        <gml:exterior>
                          <gml:LinearRing>
                            <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
                          </gml:LinearRing>
                        </gml:exterior>
                      </gml:PolygonPatch>
                    </gml:patches>
                  </gml:Surface>
                </gml:surfaceMember>
                <gml:surfaceMember>
                  <gml:Surface>
                    <gml:patches>
                      <gml:PolygonPatch>
                        <gml:interior>
                          <gml:LinearRing>
                            <gml:posList>0.25 0.25 0.25 0.75 0.75 0.75 0.75 0.25 0.25 0.25</gml:posList>
                          </gml:LinearRing>
                        </gml:interior>
                      </gml:PolygonPatch>
                    </gml:patches>
                  </gml:Surface>
                </gml:surfaceMember>
               <gml:surfaceMember>
                 <gml:Surface>
                    <gml:patches>
                      <gml:PolygonPatch>
                        <gml:exterior>
                          <gml:LinearRing>
                            <gml:posList>0 0 0 -1 -1 -1 -1 0 0 0</gml:posList>
                          </gml:LinearRing>
                        </gml:exterior>
                      </gml:PolygonPatch>
                    </gml:patches>
                  </gml:Surface>
                </gml:surfaceMember>
              </gml:MultiSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0),'
        '(0.25 0.25,0.25 0.75,0.75 0.75,0.75 0.25,0.25 0.25)),'
        '((0 0,0 -1,-1 -1,-1 0,0 0)))',
    )

  def test_gml_Coordinates_ts_cs_decimal(self) -> None:
    cases = [
        ('<gml:Point><gml:coordinates>1,2</gml:coordinates></gml:Point>',
         'POINT (1 2)'),
        ('<gml:Point><gml:coordinates cs="," ts=" " decimal=".">1,2'
         '</gml:coordinates></gml:Point>', 'POINT (1 2)'),
        ('<gml:Point><gml:coordinates cs="," ts=" " decimal=".">1,2,3'
         '</gml:coordinates></gml:Point>', 'POINT (1 2 3)'),
        ('<gml:Point><gml:coordinates cs="," ts=" " decimal=".">  1,2  '
         '</gml:coordinates></gml:Point>', 'POINT (1 2)'),
        ('<gml:Point><gml:coordinates>1 2</gml:coordinates></gml:Point>',
         'POINT (1 2)'),
        ('<gml:Point><gml:coordinates cs=";">1;2</gml:coordinates>'
         '</gml:Point>', 'POINT (1 2)'),
        ('<gml:Point><gml:coordinates decimal="," cs=";">1,2;3,4'
         '</gml:coordinates></gml:Point>', 'POINT (1.2 3.4)'),
        ('<gml:Point><gml:coordinates decimal="," cs=";">1,2;3,4;5,6'
         '</gml:coordinates></gml:Point>', 'POINT (1.2 3.4 5.6)'),
        ('<gml:LineString><gml:coordinates>1,2 3,4</gml:coordinates>'
         '</gml:LineString>', 'LINESTRING (1 2,3 4)'),
        ('<gml:LineString><gml:coordinates cs="," ts=" " decimal=".">1,2 3,4'
         '</gml:coordinates></gml:LineString>', 'LINESTRING (1 2,3 4)'),
        ('<gml:LineString><gml:coordinates cs="," ts=" " decimal=".">'
         '1,2,2.5 3,4</gml:coordinates></gml:LineString>',
         'LINESTRING (1 2 2.5,3 4 0)'),
        ('<gml:LineString><gml:coordinates ts="-">1,2-3,4</gml:coordinates>'
         '</gml:LineString>', 'LINESTRING (1 2,3 4)'),
        ('<gml:LineString><gml:coordinates cs=" " ts=",">1 2,3 4'
         '</gml:coordinates></gml:LineString>', 'LINESTRING (1 2,3 4)'),
        ('<gml:LineString><gml:coordinates cs=" " ts=",">1 2 2.5,3 4'
         '</gml:coordinates></gml:LineString>', 'LINESTRING (1 2 2.5,3 4 0)'),
    ]
    for gml, expected_wkt in cases:
      self.assertEqual(
          ogr.CreateGeometryFromGML(gml).ExportToWkt(), expected_wkt, gml
      )

  def test_gml_with_xml_header_and_comments(self) -> None:
    cases = [
        (
            '<?xml version="1.0" encoding="UTF-8"?><!-- comment --><gml:Point>'
            ' <!-- comment --> <gml:coordinates>1,2</gml:coordinates>'
            '</gml:Point>',
            'POINT (1 2)',
        ),
        (
            """<gml:MultiSurface><!-- comment -->
               <gml:surfaceMember><!-- comment -->
                 <gml:Surface><!-- comment -->
                    <gml:patches><!-- comment -->
                      <gml:PolygonPatch><!-- comment -->
                        <gml:exterior><!-- comment -->
                          <gml:LinearRing><!-- comment -->
                            <gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>
                          </gml:LinearRing>
                        </gml:exterior>
                        <!-- comment -->
                        <gml:interior><!-- comment -->
                          <gml:LinearRing><!-- comment -->
                            <gml:posList>0.25 0.25 0.25 0.75 0.75 0.75 0.75 0.25 0.25 0.25</gml:posList>
                          </gml:LinearRing>
                        </gml:interior>
                      </gml:PolygonPatch>
                    </gml:patches>
                  </gml:Surface>
                </gml:surfaceMember>
              </gml:MultiSurface>""",
            'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0),'
            '(0.25 0.25,0.25 0.75,0.75 0.75,0.75 0.25,0.25 0.25)))',
        ),
    ]
    for gml, expected_wkt in cases:
      self.assertEqual(
          ogr.CreateGeometryFromGML(gml).ExportToWkt(), expected_wkt
      )

  def test_gml_srsDimension_topgeometry(self) -> None:
    gml = """<gml:Surface srsName="EPSG:25832" srsDimension="3">
    <gml:patches>
        <gml:PolygonPatch>
            <gml:exterior>
                <gml:LinearRing>
                    <gml:posList>
                        0 0 10 0 1 10 1 1 10 1 0 10 0 0 10
                    </gml:posList>
                </gml:LinearRing>
            </gml:exterior>
        </gml:PolygonPatch>
    </gml:patches>
</gml:Surface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'POLYGON ((0 0 10,0 1 10,1 1 10,1 0 10,0 0 10))',
    )

  def test_gml_Arc(self) -> None:
    gml = '<gml:Arc><gml:posList>1 0 0 1 -1 0</gml:posList></gml:Arc>'
    geom = ogr.CreateGeometryFromGML(gml)
    self.CheckFeatureGeometry(
        geom, ogr.CreateGeometryFromWkt('CIRCULARSTRING (1 0,0 1,-1 0)')
    )
    gml2 = geom.ExportToGML(['FORMAT=GML3'])
    self.assertEqual(
        gml2,
        '<gml:Curve><gml:segments><gml:ArcString>'
        '<gml:posList>1 0 0 1 -1 0</gml:posList></gml:ArcString>'
        '</gml:segments></gml:Curve>',
    )
    self.assertGeomEqual(ogr.CreateGeometryFromGML(gml2), geom)

  def test_gml_ArcByBulge(self) -> None:
    gml = (
        '<gml:ArcByBulge><gml:posList>2 0 -2 0</gml:posList>'
        '<gml:bulge>2</gml:bulge><gml:normal>-1</gml:normal></gml:ArcByBulge>'
    )
    self.CheckFeatureGeometry(
        ogr.CreateGeometryFromGML(gml),
        ogr.CreateGeometryFromWkt('CIRCULARSTRING (2 0,0 2,-2 0)'),
    )

  def test_gml_ArcByCenterPoint(self) -> None:
    expected = ogr.CreateGeometryFromWkt('CIRCULARSTRING (1 4,-1 2,1 0)')
    for gml in (
        '<gml:ArcByCenterPoint><gml:pos>1 2</gml:pos><gml:radius>2'
        '</gml:radius><gml:startAngle>90</gml:startAngle><gml:endAngle>270'
        '</gml:endAngle></gml:ArcByCenterPoint>',
        "<gml:ArcByCenterPoint><gml:pos>1 2</gml:pos><gml:radius uom='km'>"
        '0.002</gml:radius><gml:startAngle>90</gml:startAngle><gml:endAngle>270'
        '</gml:endAngle></gml:ArcByCenterPoint>',
        "<gml:ArcByCenterPoint srsName='http://www.opengis.net/def/crs/EPSG/0/25832'>"
        "<gml:pos>1 2</gml:pos><gml:radius uom='km'>0.002</gml:radius>"
        '<gml:startAngle>90</gml:startAngle><gml:endAngle>270</gml:endAngle>'
        '</gml:ArcByCenterPoint>',
        "<gml:ArcByCenterPoint srsName='http://www.opengis.net/def/crs/EPSG/0/2222'>"
        "<gml:pos>1 2</gml:pos><gml:radius uom='m'>0.6096</gml:radius>"
        '<gml:startAngle>90</gml:startAngle><gml:endAngle>270</gml:endAngle>'
        '</gml:ArcByCenterPoint>',
        "<gml:ArcByCenterPoint srsName='http://www.opengis.net/def/crs/EPSG/0/2222'>"
        "<gml:pos>1 2</gml:pos><gml:radius uom='ft'>2</gml:radius>"
        '<gml:startAngle>90</gml:startAngle><gml:endAngle>270</gml:endAngle>'
        '</gml:ArcByCenterPoint>',
    ):
      self.CheckFeatureGeometry(ogr.CreateGeometryFromGML(gml), expected)

  def test_gml_CompoundCurve_of_ArcByCenterPoint(self) -> None:
    gml = """
<Surface gml:id="Surface1" srsDimension="2" srsName="URN:OGC:DEF:CRS:OGC:1.3:CRS84">
  <patches>
    <PolygonPatch>
      <exterior>
        <Ring>
          <curveMember>
            <Curve ns:id="Curve1">
              <segments>
                <LineStringSegment>
                  <pos>-80.40 33.86</pos>
                  <pos>-80.27 33.63</pos>
                </LineStringSegment>
              </segments>
            </Curve>
          </curveMember>
          <curveMember>
            <Curve ns:id="Curve2">
              <segments>
                <ArcByCenterPoint numArc="1">
                  <pointProperty>
                    <Point ns:id="Point1">
                      <pos>-80.47 33.98</pos>
                    </Point>
                  </pointProperty>
                  <radius uom="NM">23</radius>
                  <startAngle uom="deg">295</startAngle>
                  <endAngle uom="deg">249</endAngle>
                </ArcByCenterPoint>
              </segments>
            </Curve>
          </curveMember>
          <curveMember>
            <Curve ns:id="Curve3">
              <segments>
                <LineStringSegment>
                  <pos>-80.63 33.62</pos>
                  <pos>-80.39 33.85</pos>
                </LineStringSegment>
              </segments>
            </Curve>
          </curveMember>
        </Ring>
      </exterior>
    </PolygonPatch>
  </patches>
</Surface>"""
    self.CheckFeatureGeometry(
        ogr.CreateGeometryFromGML(gml),
        'POLYGON ((-80.4 33.86,-80.27 33.63,-80.305028054229538 33.622017309598967,'
        '-80.335422529369936 33.613343178471617,-80.366464292754429 33.606448070493634,'
        '-80.398003921948742 33.601365147653873,-80.429889693662162 33.598118851265042,'
        '-80.461968286017793 33.596724788982847,-80.494085487001527 33.597189662699385,'
        '-80.52608690656875 33.599511237590342,-80.557818688893789 33.603678352435914,'
        '-80.589128223167393 33.609670971175497,-80.619864849221443 33.617460275496377,'
        '-80.63 33.62,-80.39 33.85))',
    )

  def test_gml_CircleByCenterPoint(self) -> None:
    gml = (
        '<gml:CircleByCenterPoint><gml:pos>1 2</gml:pos>'
        '<gml:radius>2</gml:radius></gml:CircleByCenterPoint>'
    )
    self.CheckFeatureGeometry(
        ogr.CreateGeometryFromGML(gml),
        ogr.CreateGeometryFromWkt('CIRCULARSTRING (-1 2,1 4,3 2,1 0,-1 2)'),
    )

  def test_gml_CircleByCenterPoint_srs_geog_uom_m_km(self) -> None:
    geom1 = ogr.CreateGeometryFromGML(
        '<gml:CircleByCenterPoint srsName="urn:ogc:def:crs:EPSG::4326">'
        '<gml:pos>49 2</gml:pos><gml:radius uom="m">2000</gml:radius>'
        '</gml:CircleByCenterPoint>'
    )
    geom1.SwapXY()
    geom2 = ogr.CreateGeometryFromGML(
        '<gml:CircleByCenterPoint srsName="URN:OGC:DEF:CRS:OGC:1.3:CRS84">'
        '<gml:pos>2 49</gml:pos><gml:radius uom="km">2</gml:radius>'
        '</gml:CircleByCenterPoint>'
    )
    self.CheckFeatureGeometry(geom1, geom2)

  def test_gml_CircleByCenterPoint_srs_geog_unnhandled_uom(self) -> None:
    with ogr.ExceptionMgr(useExceptions=False), gdal.quiet_errors():
      ogr.CreateGeometryFromGML(
          '<gml:CircleByCenterPoint gml:id="geom_id"'
          ' srsName="urn:ogc:def:crs:EPSG::4326">'
          '<gml:pos>49 2</gml:pos><gml:radius uom="unhandled">2000'
          '</gml:radius></gml:CircleByCenterPoint>'
      )
      self.assertEqual(
          gdal.GetLastErrorMsg(),
          "GML geometry id='geom_id': Unhandled distance unit 'unhandled' in"
          " attribute 'radius'",
      )

      ogr.CreateGeometryFromGML(
          '<gml:CircleByCenterPoint srsName="urn:ogc:def:crs:EPSG::4326">'
          '<gml:pos>2 49</gml:pos><gml:radius uom="unhandled">2000'
          '</gml:radius></gml:CircleByCenterPoint>'
      )
      self.assertEqual(
          gdal.GetLastErrorMsg(),
          "Unhandled distance unit 'unhandled' in attribute 'radius'",
      )

  def test_gml_CompoundCurve_of_ArcByCenterPoint_curve_in_same_segments(
      self,
  ) -> None:
    geom = ogr.CreateGeometryFromGML("""
        <aixm:Surface srsName="urn:ogc:def:crs:EPSG::4326" gml:id="ID_249">
            <gml:patches>
                <gml:PolygonPatch>
                    <gml:exterior>
                        <gml:Ring>
                            <gml:curveMember>
                                <gml:Curve gml:id="ID_250">
                                    <gml:segments>
                                        <gml:GeodesicString>
                                            <gml:posList>55.233333333333334
                                            -36.166666666666664 55.23116372807667
                                            -36.89437337916484</gml:posList>
                                        </gml:GeodesicString>
                                        <gml:ArcByCenterPoint numArc="1">
                                            <gml:posList>55.2333333333333
                                            -36.166666666666664</gml:posList>
                                            <gml:radius uom="NM">25.0</gml:radius>
                                            <gml:startAngle uom="deg">270.0</gml:startAngle>
                                            <gml:endAngle uom="deg">497.0</gml:endAngle>
                                        </gml:ArcByCenterPoint>
                                        <gml:GeodesicString>
                                            <gml:posList>54.92816350530716 -35.674116070018954
                                            55.233333333333334
                                            -36.166666666666664</gml:posList>
                                        </gml:GeodesicString>
                                    </gml:segments>
                                </gml:Curve>
                            </gml:curveMember>
                        </gml:Ring>
                    </gml:exterior>
                </gml:PolygonPatch>
            </gml:patches>
            <aixm:horizontalAccuracy uom="KM">2.0</aixm:horizontalAccuracy>
        </aixm:Surface>""")
    self.CheckFeatureGeometry(
        geom,
        'POLYGON ((55.2333333333333 -36.1666666666667,55.2311637280767 -36.8943733791648,'
        '55.2602248071013 -36.8960852160185,55.2891782700249 -36.8912782655051,'
        '55.3178697537514 -36.88292675789,55.3461587637071 -36.8710639413776,'
        '55.3739064765608 -36.8557405708675,55.4009764350458 -36.8370248014709,'
        '55.4272352367262 -36.8150019876212,55.4525532129231 -36.7897743859994,'
        '55.476805093957 -36.7614607612323,55.4998706568286 -36.7301958939182,'
        '55.5216353514589 -36.6961299915025,55.5419909016414 -36.6594280032268,'
        '55.5608358769213 -36.6202688413925,55.5780762317212 -36.5788445119267,'
        '55.5936258081681 -36.5353591583435,55.6074067992521 -36.4900280239174,'
        '55.6193501691593 -36.4430763379496,55.6293960278662 -36.3947381326996,'
        '55.6374939573672 -36.3452549984946,55.6436032872147 -36.2948747851727,'
        '55.6476933173936 -36.2438502586493,55.6497434869178 -36.19243772209,'
        '55.6497434869178 -36.1408956112433,55.6476933173936 -36.089483074684,'
        '55.6436032872147 -36.0384585481606,55.6374939573672 -35.9880783348387,'
        '55.6293960278662 -35.9385952006337,55.6193501691593 -35.8902569953837,'
        '55.6074067992521 -35.8433053094159,55.5936258081681 -35.7979741749898,'
        '55.5780762317212 -35.7544888214066,55.5608358769213 -35.7130644919408,'
        '55.5419909016414 -35.6739053301065,55.5216353514589 -35.6372033418322,'
        '55.4998706568286 -35.6031374394151,55.476805093957 -35.571872572101,'
        '55.4525532129231 -35.5435589473339,55.4272352367262 -35.5183313457121,'
        '55.4009764350458 -35.4963085318624,55.3739064765608 -35.4775927624659,'
        '55.3461587637071 -35.4622693919557,55.3178697537514 -35.4504065754433,'
        '55.2891782700249 -35.4420550678282,55.2602248071013 -35.4372481173149,'
        '55.2311508336186 -35.4360014509317,55.2020980963355 -35.4383133491681,'
        '55.1732079288891 -35.4441648063421,55.1446205685763 -35.4535197729773,'
        '55.1164744843283 -35.4663254761286,55.0889057188812 -35.4825128133848,'
        '55.0620472479757 -35.5019968161103,55.0360283592393 -35.524677177381,'
        '55.0109740532301 -35.5504388400636,54.9870044689367 -35.5791526404379,'
        '54.9642343358562 -35.6106760028906,54.9427724545944 -35.6448536812344,'
        '54.9281635053072 -35.674116070019,55.2333333333333 -36.1666666666667))',
    )

  def test_gml_Ring_starting_with_ArcByCenterPoint(self) -> None:
    geom = ogr.CreateGeometryFromGML(
        """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<ns3:Surface xmlns:ns1="http://www.opengis.net/gml/3.2" xmlns:ns2="http://www.w3.org/1999/xlink" xmlns:ns3="http://www.aixm.aero/schema/5.1" xmlns:ns4="http://www.isotc211.org/2005/gco" xmlns:ns5="http://www.isotc211.org/2005/gmd" xmlns:ns6="http://www.isotc211.org/2005/gts" xmlns:ns7="http://www.aixm.aero/schema/5.1/message" srsName="urn:ogc:def:crs:EPSG::4326" ns1:id="Ase_Proj_1561541">
    <ns1:patches>
        <ns1:PolygonPatch>
            <ns1:exterior>
                <ns1:Ring>
                    <ns1:curveMember>
                        <ns1:Curve ns1:id="Ase_Curv_1561541">
                            <ns1:segments>
<ns1:ArcByCenterPoint interpolation="circularArcCenterPointWithRadius" numArc="1">
    <ns1:posList srsName="urn:ogc:def:crs:EPSG::4326">46.5875 0.3066666666666666</ns1:posList>
    <ns1:radius uom="[nmi_i]">9.5</ns1:radius>
    <ns1:startAngle uom="deg">6.295729688631284</ns1:startAngle>
    <ns1:endAngle uom="deg">67.38797951888118</ns1:endAngle>
</ns1:ArcByCenterPoint>
<ns1:GeodesicString interpolation="geodesic">
    <ns1:posList srsName="urn:ogc:def:crs:EPSG::4326">46.64833333333333 0.5194444444444445 46.43861111111111 0.33805555555555555 46.42305555555555 0.28944444444444445</ns1:posList>
</ns1:GeodesicString>
<ns1:ArcByCenterPoint interpolation="circularArcCenterPointWithRadius" numArc="1">
    <ns1:posList srsName="urn:ogc:def:crs:EPSG::4326">46.581388888888895 0.2980555555555556</ns1:posList>
    <ns1:radius uom="[nmi_i]">9.5</ns1:radius>
    <ns1:startAngle uom="deg">-177.84615335400528</ns1:startAngle>
    <ns1:endAngle uom="deg">-120.68835384474265</ns1:endAngle>
</ns1:ArcByCenterPoint>
<ns1:GeodesicString interpolation="geodesic">
    <ns1:posList srsName="urn:ogc:def:crs:EPSG::4326">46.500277777777775 0.10055555555555556 46.54083333333333 0.10555555555555556 46.575 0.225 46.59444444444445 0.25833333333333336 46.65833333333333 0.2833333333333333 46.69555555555555 0.25555555555555554 46.745 0.33194444444444443</ns1:posList>
</ns1:GeodesicString>
                            </ns1:segments>
                        </ns1:Curve>
                    </ns1:curveMember>
                </ns1:Ring>
            </ns1:exterior>
        </ns1:PolygonPatch>
    </ns1:patches>
</ns3:Surface>"""
    )
    self.CheckFeatureGeometry(
        geom,
        ogr.CreateGeometryFromWkt(
            'POLYGON ((46.745 0.331944444444444,46.7432764927165 0.347962462754535,'
            '46.7409162535525 0.363717281627594,46.7378065348593 0.3791917846605,'
            '46.7339626150092 0.394310000163572,46.7294033759096 0.408997750371246,'
            '46.7241512079629 0.423183023082575,46.7182318974714 0.436796331344972,'
            '46.7116744971061 0.449771059387196,46.7045111801497 0.462043792829896,'
            '46.6967770793052 0.473554631563425,46.6885101109471 0.484247483569652,'
            '46.6797507857586 0.494070338341013,46.6705420067727 0.502975518511247,'
            '46.6609288558889 0.5109199085045,46.6509583699943 0.517865159176619,'
            '46.6483333333333 0.519444444444444,46.4386111111111 0.338055555555556,'
            '46.4230555555555 0.289444444444444,46.4239652827541 0.273432507330975,'
            '46.4255282239253 0.25756088338307,46.4278483647801 0.241884857622964,'
            '46.4309144976449 0.226480183207377,46.4347118090807 0.21142133567082,'
            '46.4392219496614 0.196781161237034,46.4444231204746 0.18263053185483,'
            '46.450290175977 0.169038008752016,46.4567947427662 0.156069515749848,'
            '46.4639053537626 0.143788023916768,46.4715875972233 0.132253248900933,'
            '46.4798042799442 0.121521362391196,46.4885156039408 0.111644719041597,'
            '46.4976793558324 0.102671600158084,46.5002777777778 0.100555555555556,'
            '46.5408333333333 0.105555555555556,46.575 0.225,'
            '46.5944444444444 0.258333333333333,46.6583333333333 0.283333333333333,'
            '46.6955555555556 0.255555555555556,46.745 0.331944444444444))'
        ),
    )

  def test_gml_Ring_ending_with_ArcByCenterPoint(self) -> None:
    geom = ogr.CreateGeometryFromGML(
        """<Surface srsName="urn:ogc:def:crs:EPSG::4326" gml:id="SRF15">
  <gml:patches>
    <gml:PolygonPatch>
      <gml:exterior>
        <gml:Ring>
          <gml:curveMember>
            <gml:Curve gml:id="CRV285">
              <gml:segments>
                <gml:GeodesicString>
                  <gml:pos>65.921852847164544 57.491624339427801</gml:pos>
                  <gml:pos>65.994485559282040 57.381695154985898</gml:pos>
                </gml:GeodesicString>
                <gml:GeodesicString>
                  <gml:pos>65.994485559282040 57.381695154985898</gml:pos>
                  <gml:pos>66.005296531715544 57.370309546435649</gml:pos>
                </gml:GeodesicString>
                <gml:GeodesicString>
                  <gml:pos>66.005296531715544 57.370309546435649</gml:pos>
                  <gml:pos>66.016091667389503 57.358930089809121</gml:pos>
                </gml:GeodesicString>
                <gml:GeodesicString>
                  <gml:pos>66.016091667389503 57.358930089809121</gml:pos>
                  <gml:pos>66.098645447810924 57.305543979762817</gml:pos>
                </gml:GeodesicString>
                <gml:ArcByCenterPoint numArc="1">
                  <gml:pos>66.015066666666669 57.356413888888888</gml:pos>
                  <gml:radius uom="[NMI_I]">5.183821647136702</gml:radius>
                  <gml:startAngle uom="DEG">346.133716913420812</gml:startAngle>
                  <gml:endAngle uom="DEG">246.770739243069897</gml:endAngle>
                </gml:ArcByCenterPoint>
                <gml:GeodesicString>
                  <gml:pos>65.980986631402914 57.162257469110280</gml:pos>
                  <gml:pos>65.960397363228381 57.184109890966802</gml:pos>
                </gml:GeodesicString>
                <gml:ArcByCenterPoint numArc="1">
                  <gml:pos>65.994450000000001 57.378138888888891</gml:pos>
                  <gml:radius uom="[NMI_I]">5.183821647136512</gml:radius>
                  <gml:startAngle uom="DEG">246.790586752267330</gml:startAngle>
                  <gml:endAngle uom="DEG">147.427609082272085</gml:endAngle>
                </gml:ArcByCenterPoint>
              </gml:segments>
            </gml:Curve>
          </gml:curveMember>
        </gml:Ring>
      </gml:exterior>
    </gml:PolygonPatch>
  </gml:patches>
</Surface>
"""
    )
    self.CheckFeatureGeometry(
        geom,
        ogr.CreateGeometryFromWkt(
            'POLYGON ((65.9218528471645 57.4916243394278,'
            '65.994485559282 57.3816951549859,66.0052965317155 57.3703095464356,'
            '66.0160916673895 57.3589300898091,66.0986454478109 57.3055439797628,'
            '66.0972833787257 57.2909960861013,66.0952275333989 57.2770050871946,'
            '66.0927800217659 57.2634046235748,66.0899528791643 57.2502614506486,'
            '66.0867600033801 57.2376400142709,66.0832170844948 57.2256021285702,'
            '66.079341525797 57.214206668632,66.0751523561957 57.203509279214,'
            '66.0706701346179 57.1935621012919,66.0659168469146 57.1844135176486,'
            '66.0609157958339 57.1761079188393,66.0556914846553 57.1686854906152,'
            '66.0502694951079 57.1621820236802,66.0446763602204 57.1566287467641,'
            '66.0389394327704 57.1520521834553,66.033086750019 57.1484740335078,'
            '66.0271468954283 57.145911078861,66.0211488580662 57.1443751146804,'
            '66.0151218904116 57.1438729053792,66.0090953652666 57.1444061657465,'
            '66.0030986324867 57.1459715668484,65.9971608762266 57.1485607664255,'
            '65.991310973392 57.1521604634455,65.9855773539744 57.156752476156,'
            '65.9809866314029 57.1622574691103,65.9603973632284 57.1841098909668,'
            '65.9548299773474 57.1895629087476,65.9495750702849 57.1968227388142,'
            '65.9445389220664 57.2049612750665,65.9397459320329 57.2139385813473,'
            '65.935219308136 57.2237107308994,65.9309809566457 57.234230023973,'
            '65.927051378271 57.2454452217566,65.9234495711475 57.2573017957448,'
            '65.9201929411068 57.269742191149,65.9172972196136 57.2827061032052,'
            '65.914776389716 57.2961307649312,65.9126426203217 57.3099512454202,'
            '65.9109062090733 57.3241007570179,65.9095755340619 57.3385109700802,'
            '65.9086570145768 57.353112334239,65.9081550810541 57.3678344048457,'
            '65.9080721543467 57.382606172423,65.9084086344011 57.3973563949994,'
            '65.9091628983867 57.4120139317594,65.9103313082862 57.4265080751922,'
            '65.911908227915 57.4407688820673,65.9138860493003 57.4547275015333,'
            '65.9162552283127 57.468316498379,65.9190043294021 57.4814701705531,'
            '65.9218528471645 57.4916243394278))'
        ),
    )

  def test_gml_Circle(self) -> None:
    gml = """<gml:Curve><gml:segments><gml:Circle>
             <gml:posList>-1 0 0 1 -0.707106781186547 -0.707106781186548</gml:posList>
             </gml:Circle></gml:segments></gml:Curve>"""
    self.CheckFeatureGeometry(
        ogr.CreateGeometryFromGML(gml),
        ogr.CreateGeometryFromWkt(
            'CIRCULARSTRING (-1 0,0 1,-0.707106781186547 -0.707106781186548,'
            '-0.923879532511287 -0.38268343236509,-1 0)'
        ),
    )

    geom = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,2 0,0 0)')
    gml2 = geom.ExportToGML(['FORMAT=GML3'])
    self.assertEqual(
        gml2,
        '<gml:Curve><gml:segments><gml:Circle><gml:posList>'
        '0 0 1 1 2 0</gml:posList></gml:Circle></gml:segments></gml:Curve>',
    )
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml2).ExportToWkt(),
        'CIRCULARSTRING (0 0,1 1,2 0,1 -1,0 0)',
    )

    geom3d = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0 10,2 0 10,0 0 10)')
    self.assertEqual(
        geom3d.ExportToGML(['FORMAT=GML3']),
        '<gml:Curve><gml:segments><gml:Circle><gml:posList srsDimension="3">'
        '0 0 10 1 1 10 2 0 10</gml:posList></gml:Circle></gml:segments>'
        '</gml:Curve>',
    )

    gml_patch = """<gml:PolygonPatch>
                <gml:exterior>
                    <gml:Ring>
                        <gml:curveMember>
                            <gml:Curve>
                                <gml:segments>
                                    <gml:Circle>
                                        <gml:posList>-1 0 0 1 -0.707106781186547 -0.707106781186548</gml:posList>
                                    </gml:Circle>
                                </gml:segments>
                            </gml:Curve>
                        </gml:curveMember>
                    </gml:Ring>
                </gml:exterior>
            </gml:PolygonPatch>"""
    self.CheckFeatureGeometry(
        ogr.CreateGeometryFromGML(gml_patch),
        ogr.CreateGeometryFromWkt(
            'CURVEPOLYGON (CIRCULARSTRING (-1 0,0 1,'
            '-0.707106781186547 -0.707106781186548,'
            '-0.923879532511287 -0.38268343236509,-1 0))'
        ),
    )

  def test_gml_ArcString(self) -> None:
    gml = (
        '<gml:ArcString><gml:posList srsDimension="2">-2 0 -1 -1 0 0'
        '</gml:posList></gml:ArcString>'
    )
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'CIRCULARSTRING (-2 0,-1 -1,0 0)',
    )

    gml_multi = (
        '<gml:ArcString><gml:posList srsDimension="2">'
        '-2 0 -1 -1 0 0 1 -1 2 0 0 2 -2 0</gml:posList></gml:ArcString>'
    )
    geom = ogr.CreateGeometryFromGML(gml_multi)
    self.assertEqual(
        geom.ExportToWkt(), 'CIRCULARSTRING (-2 0,-1 -1,0 0,1 -1,2 0,0 2,-2 0)'
    )
    gml2 = geom.ExportToGML(['FORMAT=GML3'])
    self.assertEqual(
        gml2,
        '<gml:Curve><gml:segments><gml:ArcString>'
        '<gml:posList>-2 0 -1 -1 0 0 1 -1 2 0 0 2 -2 0</gml:posList>'
        '</gml:ArcString></gml:segments></gml:Curve>',
    )
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml2).ExportToWkt(),
        'CIRCULARSTRING (-2 0,-1 -1,0 0,1 -1,2 0,0 2,-2 0)',
    )

  def test_gml_OGRCompoundCurve(self) -> None:
    geom = ogr.CreateGeometryFromWkt('COMPOUNDCURVE ((0 0,1 1,2 0))')
    gml = geom.ExportToGML(['FORMAT=GML3'])
    expected_gml = (
        '<gml:CompositeCurve><gml:curveMember><gml:LineString>'
        '<gml:posList>0 0 1 1 2 0</gml:posList></gml:LineString>'
        '</gml:curveMember></gml:CompositeCurve>'
    )
    self.assertEqual(gml, expected_gml)
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'LINESTRING (0 0,1 1,2 0)',
    )

    gml_arc = (
        '<gml:CompositeCurve><gml:curveMember><gml:Curve><gml:segments>'
        '<gml:ArcString><gml:posList>0 0 1 1 2 0</gml:posList></gml:ArcString>'
        '</gml:segments></gml:Curve></gml:curveMember></gml:CompositeCurve>'
    )
    geom_arc = ogr.CreateGeometryFromGML(gml_arc)
    self.assertEqual(
        geom_arc.ExportToWkt(), 'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0))'
    )
    self.assertEqual(geom_arc.ExportToGML(['FORMAT=GML3']), gml_arc)

    gml_3_arcs = """<gml:CompositeCurve>
    <gml:curveMember><gml:Curve><gml:segments><gml:Arc><gml:posList>0 0 1 1 2 0</gml:posList></gml:Arc></gml:segments></gml:Curve></gml:curveMember>
    <gml:curveMember><gml:Curve><gml:segments><gml:Arc><gml:posList>2 0 3 1 4 0</gml:posList></gml:Arc></gml:segments></gml:Curve></gml:curveMember>
    <gml:curveMember><gml:Curve><gml:segments><gml:Arc><gml:posList>4 0 5 1 6 0</gml:posList></gml:Arc></gml:segments></gml:Curve></gml:curveMember>
    </gml:CompositeCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_3_arcs).ExportToWkt(),
        'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0),'
        'CIRCULARSTRING (2 0,3 1,4 0),CIRCULARSTRING (4 0,5 1,6 0))',
    )

    gml_curve_3_arcs = """<gml:Curve><gml:segments>
            <gml:Arc><gml:posList>0 0 1 1 2 0</gml:posList></gml:Arc>
            <gml:Arc><gml:posList>2 0 3 1 4 0</gml:posList></gml:Arc>
            <gml:Arc><gml:posList>4 0 5 1 6 0</gml:posList></gml:Arc>
            </gml:segments></gml:Curve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_curve_3_arcs).ExportToWkt(),
        'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0),'
        'CIRCULARSTRING (2 0,3 1,4 0),CIRCULARSTRING (4 0,5 1,6 0))',
    )

    gml_mixed = """<gml:Curve><gml:segments>
            <gml:Arc><gml:posList>0 0 1 0 0 0</gml:posList></gml:Arc>
            <gml:LineStringSegment><gml:posList>0 0 -1 0 0 0</gml:posList></gml:LineStringSegment>
            </gml:segments></gml:Curve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_mixed).ExportToWkt(),
        'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 0,0 0),(0 0,-1 0,0 0))',
    )

    gml_nested = """<gml:CompositeCurve>
    <gml:curveMember><gml:Curve><gml:segments><gml:LineString><gml:posList>0 0 1 0 0 0</gml:posList></gml:LineString></gml:segments></gml:Curve></gml:curveMember>
    <gml:curveMember>
            <gml:Curve><gml:segments>
                <gml:Arc><gml:posList>0 0 1 0 0 0</gml:posList></gml:Arc>
                <gml:LineStringSegment><gml:posList>0 0 -1 0 0 0</gml:posList></gml:LineStringSegment>
            </gml:segments></gml:Curve>
    </gml:curveMember>
    </gml:CompositeCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_nested).ExportToWkt(),
        'COMPOUNDCURVE ((0 0,1 0,0 0),CIRCULARSTRING (0 0,1 0,0 0),'
        '(0 0,-1 0,0 0))',
    )

  def test_gml_OGRCurvePolygon_one_CircularString(self) -> None:
    gml = (
        '<gml:Polygon><gml:exterior><gml:Ring><gml:curveMember><gml:Curve>'
        '<gml:segments><gml:Arc><gml:posList>0 0 1 0 0 0</gml:posList>'
        '</gml:Arc></gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior></gml:Polygon>'
    )
    geom = ogr.CreateGeometryFromGML(gml)
    self.assertEqual(
        geom.ExportToWkt(), 'CURVEPOLYGON (CIRCULARSTRING (0 0,1 0,0 0))'
    )
    self.assertEqual(
        geom.ExportToGML(['FORMAT=GML3']),
        '<gml:Polygon><gml:exterior><gml:Ring><gml:curveMember><gml:Curve>'
        '<gml:segments><gml:Circle><gml:posList>0 0 0.5 0.5 1 0</gml:posList>'
        '</gml:Circle></gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior></gml:Polygon>',
    )
    self.assertEqual(
        geom.ExportToGML(['FORMAT=GML3', 'GMLID=gmlid']),
        '<gml:Polygon gml:id="gmlid"><gml:exterior><gml:Ring><gml:curveMember>'
        '<gml:Curve gml:id="gmlid.exterior"><gml:segments><gml:Circle>'
        '<gml:posList>0 0 0.5 0.5 1 0</gml:posList></gml:Circle>'
        '</gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior></gml:Polygon>',
    )

  def test_gml_OGRCurvePolygon_one_CircularString_with_interior_CircularString(
      self,
  ) -> None:
    gml = (
        '<gml:Polygon><gml:exterior><gml:Ring><gml:curveMember><gml:Curve>'
        '<gml:segments><gml:Arc><gml:posList>0 0 1 0 0 0</gml:posList>'
        '</gml:Arc></gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior><gml:interior><gml:Ring><gml:curveMember><gml:Curve>'
        '<gml:segments><gml:Arc><gml:posList>0.25 0 0.75 0 0.25 0'
        '</gml:posList></gml:Arc></gml:segments></gml:Curve></gml:curveMember>'
        '</gml:Ring></gml:interior></gml:Polygon>'
    )
    geom = ogr.CreateGeometryFromGML(gml)
    self.assertEqual(
        geom.ExportToWkt(),
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 0,0 0),'
        'CIRCULARSTRING (0.25 0.0,0.75 0.0,0.25 0.0))',
    )
    self.assertEqual(
        geom.ExportToGML(['FORMAT=GML3']),
        '<gml:Polygon><gml:exterior><gml:Ring><gml:curveMember><gml:Curve>'
        '<gml:segments><gml:Circle><gml:posList>0 0 0.5 0.5 1 0</gml:posList>'
        '</gml:Circle></gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior><gml:interior><gml:Ring><gml:curveMember><gml:Curve>'
        '<gml:segments><gml:Circle><gml:posList>0.25 0.0 0.5 0.25 0.75 0.0'
        '</gml:posList></gml:Circle></gml:segments></gml:Curve>'
        '</gml:curveMember></gml:Ring></gml:interior></gml:Polygon>',
    )
    self.assertEqual(
        geom.ExportToGML(['FORMAT=GML3', 'GMLID=gmlid']),
        '<gml:Polygon gml:id="gmlid"><gml:exterior><gml:Ring><gml:curveMember>'
        '<gml:Curve gml:id="gmlid.exterior"><gml:segments><gml:Circle>'
        '<gml:posList>0 0 0.5 0.5 1 0</gml:posList></gml:Circle>'
        '</gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior><gml:interior><gml:Ring><gml:curveMember>'
        '<gml:Curve gml:id="gmlid.interior.0"><gml:segments><gml:Circle>'
        '<gml:posList>0.25 0.0 0.5 0.25 0.75 0.0</gml:posList></gml:Circle>'
        '</gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:interior></gml:Polygon>',
    )

  def test_gml_OGRCurvePolygon_CompoundCurve(self) -> None:
    gml = (
        '<gml:Polygon><gml:exterior><gml:Ring><gml:curveMember>'
        '<gml:LineString><gml:posList>0 -1 0 1</gml:posList></gml:LineString>'
        '</gml:curveMember><gml:curveMember><gml:Curve><gml:segments>'
        '<gml:ArcString><gml:posList>0 1 1 0 0 -1</gml:posList></gml:ArcString>'
        '</gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior></gml:Polygon>'
    )
    geom = ogr.CreateGeometryFromGML(gml)
    self.assertEqual(
        geom.ExportToWkt(),
        'CURVEPOLYGON (COMPOUNDCURVE ((0 -1,0 1),'
        'CIRCULARSTRING (0 1,1 0,0 -1)))',
    )
    self.assertEqual(geom.ExportToGML(['FORMAT=GML3']), gml)
    self.assertEqual(
        geom.ExportToGML(['FORMAT=GML3', 'GMLID=gmlid']),
        '<gml:Polygon gml:id="gmlid"><gml:exterior><gml:Ring><gml:curveMember>'
        '<gml:LineString gml:id="gmlid.exterior.0"><gml:posList>0 -1 0 1'
        '</gml:posList></gml:LineString></gml:curveMember><gml:curveMember>'
        '<gml:Curve gml:id="gmlid.exterior.1"><gml:segments><gml:ArcString>'
        '<gml:posList>0 1 1 0 0 -1</gml:posList></gml:ArcString>'
        '</gml:segments></gml:Curve></gml:curveMember></gml:Ring>'
        '</gml:exterior></gml:Polygon>',
    )

  def test_gml_OGRCurvePolygon_LinearRing_with_interior_CircularString(
      self,
  ) -> None:
    gml = (
        '<gml:Polygon><gml:exterior><gml:LinearRing><gml:posList>'
        '-2 -2 -2 2 2 2 2 -2 -2 -2</gml:posList></gml:LinearRing>'
        '</gml:exterior><gml:interior><gml:Ring><gml:curveMember><gml:Arc>'
        '<gml:posList>0.25 0 0.75 0 0.25 0</gml:posList></gml:Arc>'
        '</gml:curveMember></gml:Ring></gml:interior></gml:Polygon>'
    )
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml).ExportToWkt(),
        'CURVEPOLYGON ((-2 -2,-2 2,2 2,2 -2,-2 -2),'
        'CIRCULARSTRING (0.25 0.0,0.75 0.0,0.25 0.0))',
    )

  def test_gml_OGRCurvePolygon_CircularString_with_interior_LinearRing(
      self,
  ) -> None:
    gml = (
        '<gml:Polygon><gml:exterior><gml:Ring><gml:curveMember><gml:Circle>'
        '<gml:posList>-1 0 1 2 3 0</gml:posList></gml:Circle>'
        '</gml:curveMember></gml:Ring></gml:exterior><gml:interior>'
        '<gml:LinearRing><gml:posList>-2 -2 -2 2 2 2 2 -2 -2 -2'
        '</gml:posList></gml:LinearRing></gml:interior></gml:Polygon>'
    )
    geom = ogr.CreateGeometryFromGML(gml)
    self.CheckFeatureGeometry(
        geom,
        'CURVEPOLYGON (CIRCULARSTRING (-1 0,1 2,3 0,1.0 -2.0,-1 0),'
        '(-2 -2,-2 2,2 2,2 -2,-2 -2))',
    )
    geom2 = ogr.CreateGeometryFromGML(geom.ExportToGML(['FORMAT=GML3']))
    expected_geom2 = ogr.CreateGeometryFromGML(
        '<gml:Polygon><gml:exterior><gml:Curve><gml:segments><gml:ArcString>'
        '<gml:posList>-1 0 1 2 3 0 1 -2 -1 0</gml:posList></gml:ArcString>'
        '</gml:segments></gml:Curve></gml:exterior><gml:interior>'
        '<gml:LineString><gml:posList>-2 -2 -2 2 2 2 2 -2 -2 -2'
        '</gml:posList></gml:LineString></gml:interior></gml:Polygon>'
    )
    self.CheckFeatureGeometry(geom2, expected_geom2)

  def test_gml_OGRMultiSurface(self) -> None:
    gml_cp = """<gml:MultiSurface>
    <gml:surfaceMember>
        <gml:Surface>
            <gml:patches>
                <gml:PolygonPatch>
                    <gml:exterior>
                        <gml:Ring><gml:curveMember><gml:Curve><gml:segments><gml:Circle><gml:posList>0 0 1 1 1 -1</gml:posList></gml:Circle></gml:segments></gml:Curve></gml:curveMember></gml:Ring>
                    </gml:exterior>
                </gml:PolygonPatch>
            </gml:patches>
        </gml:Surface>
    </gml:surfaceMember>
</gml:MultiSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_cp).ExportToWkt(),
        'MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING '
        '(0 0,1 1,1 -1,0.292893218813453 -0.707106781186548,0 0)))',
    )

    gml_poly_cp = """<gml:MultiSurface>
    <gml:surfaceMember>
        <gml:Polygon>
            <gml:exterior>
                <gml:LinearRing><gml:posList>0 0 0 1 1 1 0 0</gml:posList></gml:LinearRing>
            </gml:exterior>
        </gml:Polygon>
    </gml:surfaceMember>
    <gml:surfaceMember>
        <gml:Polygon>
            <gml:exterior>
                <gml:Ring><gml:curveMember><gml:Curve><gml:segments><gml:Circle><gml:posList>0 0 1 1 1 -1</gml:posList></gml:Circle></gml:segments></gml:Curve></gml:curveMember></gml:Ring>
            </gml:exterior>
        </gml:Polygon>
    </gml:surfaceMember>
</gml:MultiSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_poly_cp).ExportToWkt(),
        'MULTISURFACE (((0 0,0 1,1 1,0 0)),CURVEPOLYGON (CIRCULARSTRING '
        '(0 0,1 1,1 -1,0.292893218813453 -0.707106781186548,0 0)))',
    )

    gml_cp_poly = """<gml:MultiSurface>
    <gml:surfaceMember>
        <gml:Polygon>
            <gml:exterior>
                <gml:Ring><gml:curveMember><gml:Curve><gml:segments><gml:Circle><gml:posList>0 0 1 1 1 -1</gml:posList></gml:Circle></gml:segments></gml:Curve></gml:curveMember></gml:Ring>
            </gml:exterior>
        </gml:Polygon>
    </gml:surfaceMember>
    <gml:surfaceMember>
        <gml:Polygon>
            <gml:exterior>
                <gml:LinearRing><gml:posList>0 0 0 1 1 1 0 0</gml:posList></gml:LinearRing>
            </gml:exterior>
        </gml:Polygon>
    </gml:surfaceMember>
</gml:MultiSurface>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_cp_poly).ExportToWkt(),
        'MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING '
        '(0 0,1 1,1 -1,0.292893218813453 -0.707106781186548,0 0)),'
        '((0 0,0 1,1 1,0 0)))',
    )

    geom = ogr.CreateGeometryFromWkt(
        'MULTISURFACE (CURVEPOLYGON((0 0,0 1,1 1,1 0,0 0)))'
    )
    self.assertEqual(
        geom.ExportToGML(['FORMAT=GML3']),
        '<gml:MultiSurface><gml:surfaceMember><gml:Polygon><gml:exterior>'
        '<gml:LinearRing><gml:posList>0 0 0 1 1 1 1 0 0 0</gml:posList>'
        '</gml:LinearRing></gml:exterior></gml:Polygon></gml:surfaceMember>'
        '</gml:MultiSurface>',
    )

  def test_gml_OGRMultiCurve(self) -> None:
    gml_arc = (
        '<gml:MultiCurve><gml:curveMember><gml:Curve><gml:segments>'
        '<gml:ArcString><gml:posList>0 0 1 1 1 -1</gml:posList>'
        '</gml:ArcString></gml:segments></gml:Curve></gml:curveMember>'
        '</gml:MultiCurve>'
    )
    geom = ogr.CreateGeometryFromGML(gml_arc)
    self.assertEqual(
        geom.ExportToWkt(), 'MULTICURVE (CIRCULARSTRING (0 0,1 1,1 -1))'
    )
    self.assertEqual(geom.ExportToGML(['FORMAT=GML3']), gml_arc)

    gml_line_arc = """<gml:MultiCurve>
    <gml:curveMember>
        <gml:LineString><gml:posList>0 0 1 1 1 -1</gml:posList></gml:LineString>
    </gml:curveMember>
    <gml:curveMember>
        <gml:Curve>
            <gml:segments><gml:Arc><gml:posList>0 0 1 1 1 -1</gml:posList></gml:Arc></gml:segments>
        </gml:Curve>
    </gml:curveMember>
</gml:MultiCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_line_arc).ExportToWkt(),
        'MULTICURVE ((0 0,1 1,1 -1),CIRCULARSTRING (0 0,1 1,1 -1))',
    )

    gml_arc_line = """<gml:MultiCurve>
    <gml:curveMember>
        <gml:Curve>
            <gml:segments><gml:Arc><gml:posList>0 0 1 1 1 -1</gml:posList></gml:Arc></gml:segments>
        </gml:Curve>
    </gml:curveMember>
    <gml:curveMember>
        <gml:LineString><gml:posList>0 0 1 1 1 -1</gml:posList></gml:LineString>
    </gml:curveMember>
</gml:MultiCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_arc_line).ExportToWkt(),
        'MULTICURVE (CIRCULARSTRING (0 0,1 1,1 -1),(0 0,1 1,1 -1))',
    )

    gml_cc = """<gml:MultiCurve>
    <gml:curveMember>
        <gml:CompositeCurve>
            <gml:curveMember>
                <gml:Curve>
                    <gml:segments><gml:Arc><gml:posList>0 0 1 1 1 -1</gml:posList></gml:Arc></gml:segments>
                </gml:Curve>
            </gml:curveMember>
            <gml:curveMember>
                <gml:LineString><gml:posList>1 -1 1 1 1 -1</gml:posList></gml:LineString>
            </gml:curveMember>
        </gml:CompositeCurve>
    </gml:curveMember>
</gml:MultiCurve>"""
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_cc).ExportToWkt(),
        'MULTICURVE (COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,1 -1),'
        '(1 -1,1 1,1 -1)))',
    )

  def test_gml_write_gml_ns(self) -> None:
    geom = ogr.CreateGeometryFromWkt('POINT(500000 4500000)')
    self.assertEqual(
        geom.ExportToGML(options=['NAMESPACE_DECL=YES']),
        '<gml:Point xmlns:gml="http://www.opengis.net/gml">'
        '<gml:coordinates>500000,4500000</gml:coordinates></gml:Point>',
    )
    self.assertEqual(
        geom.ExportToGML(options=['FORMAT=GML3', 'NAMESPACE_DECL=YES']),
        '<gml:Point xmlns:gml="http://www.opengis.net/gml">'
        '<gml:pos>500000 4500000</gml:pos></gml:Point>',
    )
    self.assertEqual(
        geom.ExportToGML(
            options=['FORMAT=GML32', 'GMLID=foo', 'NAMESPACE_DECL=YES']
        ),
        '<gml:Point xmlns:gml="http://www.opengis.net/gml/3.2" gml:id="foo">'
        '<gml:pos>500000 4500000</gml:pos></gml:Point>',
    )

  def test_gml_read_gml_ArcByCenterPoint_projected_crs_northing_easting(
      self,
  ) -> None:
    geom = ogr.CreateGeometryFromGML(
        """<gml:Surface srsName="urn:ogc:def:crs:EPSG::2326" srsDimension="2">
    <gml:patches>
    <gml:PolygonPatch>
    <gml:exterior>
    <gml:Ring>
    <gml:curveMember>
    <gml:Curve>
    <gml:segments>
    <gml:LineStringSegment>
    <gml:posList>821502.753690919 838825.332031005 821194.727830006 839043.611480001</gml:posList>
    </gml:LineStringSegment>
    <gml:ArcByCenterPoint numArc="1">
    <gml:posList>821194.396688321 839052.616490606</gml:posList>
    <gml:radius uom="EPSG:2326">9.01109709191771</gml:radius>
    <gml:startAngle uom="degree">177.894008505116</gml:startAngle>
    <gml:endAngle uom="degree">250.98396509322</gml:endAngle>
    </gml:ArcByCenterPoint>
    <gml:LineStringSegment>
    <gml:posList>821185.877350006 839049.680380003 821502.753690919 838825.332031005</gml:posList>
    </gml:LineStringSegment>
    </gml:segments>
    </gml:Curve>
    </gml:curveMember>
    </gml:Ring>
    </gml:exterior>
    </gml:PolygonPatch>
    </gml:patches>
    </gml:Surface>"""
    )
    self.assertIsNotNone(geom)

  def test_gml_OrientableCurve(self) -> None:
    gml_pos = (
        '<gml:OrientableCurve srsName="foo" gml:id="test" orientation="+">'
        ' <gml:baseCurve> <gml:Curve> <gml:segments>'
        ' <gml:LineStringSegment> <gml:posList>0 1 2 3</gml:posList>'
        ' </gml:LineStringSegment> </gml:segments> </gml:Curve>'
        ' </gml:baseCurve> </gml:OrientableCurve>'
    )
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_pos).ExportToWkt(),
        'LINESTRING (0 1,2 3)',
    )
    gml_neg = (
        '<gml:OrientableCurve srsName="foo" gml:id="test" orientation="-">'
        ' <gml:baseCurve> <gml:Curve> <gml:segments>'
        ' <gml:LineStringSegment> <gml:posList>0 1 2 3</gml:posList>'
        ' </gml:LineStringSegment> </gml:segments> </gml:Curve>'
        ' </gml:baseCurve> </gml:OrientableCurve>'
    )
    self.assertEqual(
        ogr.CreateGeometryFromGML(gml_neg).ExportToWkt(),
        'LINESTRING (2 3,0 1)',
    )

  def test_gml_aixm_ElevatedPoint(self) -> None:
    geom = ogr.CreateGeometryFromGML(
        """<aixm:ElevatedPoint srsName="urn:ogc:def:crs:EPSG::4326">
            <gml:pos>49 2</gml:pos>
            <aixm:elevation uom="M">10</aixm:elevation> <!-- ignored -->
        </aixm:ElevatedPoint>"""
    )
    self.CheckFeatureGeometry(geom, 'POINT (49 2)')


if __name__ == '__main__':
  googletest.main()
