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
###############################################################################
# Copyright (c) 2007, Mateusz Loskot <mateusz@loskot.net>
# Copyright (c) 2009-2014, Even Rouault <even dot rouault at mines-paris . org>
# Copyright (c) 2013, Kyle Shannon <kyle at pobox dot com>
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
###############################################################################

"""Test OGR handling of GeoJSON files.

This is a rewrite of:

  http://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_geojson.py.

Tests are numbered to match the autotest test when there is a match.

TODO(schwehr): Use the json module to convert python to json strings.
"""
import json
import os


from osgeo import gdal
from osgeo import ogr
from osgeo import osr
import unittest
from autotest2.gcore import gcore_util
from autotest2.ogr import ogr_util

DRIVER = ogr_util.GEOJSON_DRIVER
EXT = '.geojson'

DEFAULT_LAYER_NAME = 'OGRGeoJSON'


def setUpModule():
  ogr_util.SetupTestEnv()


def CreateField(layer, name, field_type=ogr.OFTString):
  field_definition = ogr.FieldDefn(name, field_type)
  layer.CreateField(field_definition)
  field_definition.Destroy()


@ogr_util.SkipIfDriverMissing(DRIVER)
class OgrGeoJsonTest(ogr_util.DriverTestCase):

  def setUp(self):
    super(OgrGeoJsonTest, self).setUp(DRIVER, EXT)

  def test02Point(self):
    # TODO(schwehr): Make this type of block a helper function.
    filepath = ogr_util.GetTestFilePath('point.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbPoint, 0,
                    (100.0, 100.0, 0.0, 0.0))

  def test03Line(self):
    filepath = ogr_util.GetTestFilePath('linestring.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbLineString, 0,
                    (100.0, 101.0, 0.0, 1.0))

  def test04Polygon(self):
    filepath = ogr_util.GetTestFilePath('polygon.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbPolygon, 0,
                    (100.0, 101.0, 0.0, 1.0))

  def test05GeometryCollection(self):
    filepath = ogr_util.GetTestFilePath('geometrycollection.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbGeometryCollection, 0,
                    (100.0, 102.0, 0.0, 1.0))

  def test06MultiPoint(self):
    filepath = ogr_util.GetTestFilePath('multipoint.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiPoint, 0,
                    (100.0, 101.0, 0.0, 1.0))

  def test07MultiLineString(self):
    filepath = ogr_util.GetTestFilePath('multilinestring.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiLineString, 0,
                    (100.0, 103.0, 0.0, 3.0))

  def test08MultiPolygon(self):
    filepath = ogr_util.GetTestFilePath('multipolygon.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiPolygon, 0,
                    (100.0, 103.0, 0.0, 3.0))

  # TODO(schwehr): Implement test 9.
  # TODO(schwehr): Implement test 10.

  def test11SrsName(self):
    filepath = ogr_util.GetTestFilePath('srs_name.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbGeometryCollection, 0,
                    (100.0, 102.0, 0.0, 1.0))

    srs = layer.GetSpatialRef()
    self.assertEqual(int(srs.GetAuthorityCode('GEOGCS')), 4269)
    self.assertEqual(int(srs.GetAuthorityCode('PROJCS')), 26915)

    # TODO(schwehr): Why 26915 versus 26916?
    feature = layer.GetNextFeature()
    geometry = feature.GetGeometryRef().GetGeometryRef(0)
    srs2 = geometry.GetSpatialReference()
    self.assertEqual(int(srs2.GetAuthorityCode('GEOGCS')), 4269)
    self.assertEqual(int(srs2.GetAuthorityCode('PROJCS')), 26916)

  # Test 12 was rewritten as apps/ogrinfo_test.py.

  @ogr_util.SkipIfDriverMissing(ogr_util.SHAPEFILE_DRIVER)
  def test13CopyToStdout(self):
    gdal.ErrorReset()
    layer_name = 'gjpoint'
    filepath = ogr_util.GetTestFilePath(layer_name + '.shp')
    dst = self.driver.CreateDataSource('/vsistdout/')
    layer = dst.CreateLayer(layer_name)

    CreateField(layer, 'FID', ogr.OFTReal)
    CreateField(layer, 'NAME')

    dst_feature = ogr.Feature(feature_def=layer.GetLayerDefn())
    src = ogr.Open(filepath)
    src_layer = src.GetLayer(0)
    for feature in ogr_util.Features(src_layer):
      dst_feature.SetFrom(feature)
      layer.CreateFeature(dst_feature)

    # Unable to capture stdout from GDAL's C++ code.  There will be noise
    # on the console.

    self.assertEqual(gdal.GetLastErrorNo(), 0)

  # TODO(schwehr): Rework test 14 to know which features generate geometry.

  def test14DegenerateGeometries(self):
    # TODO(schwehr): Rename file to have semantic meaning.
    filepath = ogr_util.GetTestFilePath('ogr_geojson_14.geojson')
    # This is intentionally reading degenerate geometries and will not
    # pass CheckOpen.
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      src = ogr.Open(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    src_layer = src.GetLayer(0)
    # TODO(schwehr): Explain how there is one more geometry than GDAL 1.
    self.assertEqual(src_layer.GetFeatureCount(), 27)

    with gcore_util.TestTemporaryDirectory(prefix='geojson14') as tmpdir:
      dst_filepath = os.path.join(tmpdir, 'out_14.geojson')
      dst = self.driver.CreateDataSource(dst_filepath)
      dst_layer = dst.CreateLayer('lyr')
      for feature in ogr_util.Features(src_layer):
        geometry = feature.GetGeometryRef()
        dst_feature = ogr.Feature(feature_def=dst_layer.GetLayerDefn())
        dst_feature.SetGeometry(geometry)
        dst_layer.CreateFeature(dst_feature)

      dst = None

      # Do some minimalistic checking.
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        dst = ogr.Open(dst_filepath)
      self.assertEqual(dst.GetLayerCount(), 1)
      dst_layer = dst.GetLayer(0)
    # TODO(schwehr): Explain how there is one more geometry than with GDAL 1.
    self.assertEqual(src_layer.GetFeatureCount(), 27)

  def test15ExportToJson(self):
    feature_defn = ogr.FeatureDefn()
    feature_defn.AddFieldDefn(ogr.FieldDefn('stringfield'))
    field_defn = ogr.FieldDefn('boolfield', ogr.OFTInteger)
    field_defn.SetSubType(ogr.OFSTBoolean)
    feature_defn.AddFieldDefn(field_defn)

    feature = ogr.Feature(feature_defn)
    feature.SetField('stringfield', 'bar')
    feature.SetField('boolfield', True)
    feature.SetFID(0)

    geometry = ogr.CreateGeometryFromWkt('POINT(1 2)')
    feature.SetGeometry(geometry)

    expected = {
        'geometry': {'type': 'Point', 'coordinates': [1.0, 2.0]},
        'id': 0,
        'properties': {'boolfield': True, 'stringfield': 'bar'},
        'type': 'Feature'
    }

    result = json.loads(feature.ExportToJson())
    self.assertEqual(result, expected)

  def test16EsriPointFile(self):
    filepath = ogr_util.GetTestFilePath('esripoint.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName('OGRGeoJSON')
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, 'OGRGeoJSON', 1, ogr.wkbPoint, 4, (2, 2, 49, 49))

    self.assertEqual(
        int(layer.GetSpatialRef().GetAuthorityCode('GEOGCS')), 4326)

    feature = layer.GetNextFeature()
    geom = ogr.CreateGeometryFromWkt('POINT(2 49)')

    self.CheckFeatureGeometry(feature, geom)

    self.assertEqual(feature.GetFID(), 1)
    self.assertEqual(feature.GetFieldAsInteger('fooInt'), 2)
    self.assertEqual(feature.GetFieldAsDouble('fooDouble'), 3.4)
    self.assertEqual(feature.GetFieldAsString('fooString'), '56')

  def test17EsriLinestring(self):
    filepath = ogr_util.GetTestFilePath('esrilinestring.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbLineString, 0,
                    (2, 3, 49, 50))

    feature = layer.GetNextFeature()
    geom = ogr.CreateGeometryFromWkt('LINESTRING (2 49,3 50)')
    self.CheckFeatureGeometry(feature, geom)

    self.assertEqual(feature.GetFID(), 0)

  def test18EsriPolygon(self):
    filepath = ogr_util.GetTestFilePath('esripolygon.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbPolygon, 0,
                    (-3, 3, 49, 50))

    feature = layer.GetNextFeature()
    geom = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON (((2 49,2 50,3 50,3 49,2 49),'
        '(2.1 49.1,2.1 49.9,2.9 49.9,2.9 49.1,2.1 49.1)),'
        '((-2 49,-2 50,-3 50,-3 49,-2 49)))')
    self.CheckFeatureGeometry(feature, geom)

  def test19EsriMultiPoint(self):
    filepath = ogr_util.GetTestFilePath('esrimultipoint.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiPoint, 4,
                    (2, 3, 49, 50))

    feature = layer.GetNextFeature()
    geom = ogr.CreateGeometryFromWkt('MULTIPOINT (2 49,3 50)')
    self.CheckFeatureGeometry(feature, geom)

  def test20WithoutExtension(self):
    filenames = (
        'point.geojson', 'geometrycollection.geojson',
        'multilinestring.geojson', 'srs_name.geojson',
        'esrilinestring.json', 'esripolygon.json')

    for filename in filenames:
      filepath = ogr_util.GetTestFilePath(filename)
      data = open(filepath, 'rb').read()

      dst_filepath = os.path.join('/vsimem/', filename)
      with gcore_util.GdalUnlinkWhenDone(dst_filepath):
        dst = gdal.VSIFOpenL(dst_filepath, 'wb')
        gdal.VSIFWriteL(data, 1, len(data), dst)
        gdal.VSIFCloseL(dst)

        with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
          src = ogr.Open(dst_filepath)
          self.assertIsNotNone(src)

  def testGeoJSON21GeocouchSpatialList(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features":['
        '  {"type": "Feature",'
        '   "geometry": {"type":"Point","coordinates":[1,2]},'
        '   "properties": {"_id":"aid", "_rev":"arev", "type":"Feature",'
        '   "properties": {"intvalue" : 2, "floatvalue" : 3.2, '
        '   "strvalue" : "foo"}}}]}')
    self.assertIsNotNone(src)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)

    feature = layer.GetNextFeature()
    geom = ogr.CreateGeometryFromWkt('POINT (1 2)')
    self.CheckFeatureGeometry(feature, geom)

    self.assertEqual(feature.GetFieldAsString('_id'), 'aid')
    self.assertEqual(feature.GetFieldAsString('_rev'), 'arev')
    self.assertEqual(feature.GetFieldAsInteger('intvalue'), 2)

  def test22SeveralFeatures(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features":['
        '  {"type": "Feature",'
        '   "geometry": {"type":"Point","coordinates":[1,2]},'
        '   "properties": {"_id":"aid", "_rev":"arev", "type":"Feature",'
        '     "properties":{"intvalue" : 2, "floatvalue" : 3.2, '
        '                   "strvalue" : "foo"}}},'
        '  {"type": "Feature",'
        '   "geometry": {"type":"Point","coordinates":[3,4]},'
        '   "properties": {"_id":"aid2", "_rev":"arev2", "type":"Feature",'
        '     "properties":{"intvalue" : 3.5, "str2value" : "bar"}}}]}')
    self.assertIsNotNone(src)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)

    feature = layer.GetNextFeature()
    geom = ogr.CreateGeometryFromWkt('POINT (1 2)')
    self.CheckFeatureGeometry(feature, geom)
    self.assertEqual(feature.GetFieldAsString('_id'), 'aid')
    self.assertEqual(feature.GetFieldAsString('_rev'), 'arev')
    self.assertEqual(feature.GetFieldAsInteger('intvalue'), 2)

    feature = layer.GetNextFeature()
    geom = ogr.CreateGeometryFromWkt('POINT (3 4)')
    self.CheckFeatureGeometry(feature, geom)
    self.assertEqual(feature.GetFieldAsString('_id'), 'aid2')
    self.assertEqual(feature.GetFieldAsString('_rev'), 'arev2')
    self.assertEqual(feature.GetFieldAsInteger('intvalue'), 3)
    self.assertEqual(feature.GetFieldAsDouble('intvalue'), 3.5)

  def test23BBoxAndTestSrs(self):
    filepath = '/vsimem/geojson_23.json'
    dst = self.driver.CreateDataSource(filepath)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4322)
    layer = dst.CreateLayer('foo', srs=srs, options=['WRITE_BBOX=YES'])

    feature = ogr.Feature(layer.GetLayerDefn())
    feature.SetGeometry(ogr.CreateGeometryFromWkt('POINT(1 10)'))
    layer.CreateFeature(feature)

    feature = ogr.Feature(layer.GetLayerDefn())
    feature.SetGeometry(ogr.CreateGeometryFromWkt('POINT(2 20)'))
    layer.CreateFeature(feature)

    dst = None

    with gcore_util.GdalUnlinkWhenDone(filepath):
      src = self.CheckOpen(filepath)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetSpatialRef().ExportToWkt(), srs.ExportToWkt())

      src = gdal.VSIFOpenL(filepath, 'rb')
      data = gdal.VSIFReadL(1, 10000, src).decode('ascii')
      gdal.VSIFCloseL(src)
      # TODO(schwehr): Why is this not [ 1.0, 10.0, 2.0, 20.0 ]?
      self.assertIn('"bbox": [ 2.0, 20.0, 2.0, 20.0 ]', data)
      self.assertIn('"bbox": [ 1.0, 10.0, 1.0, 10.0 ]', data)

  def test24AlternateForms(self):
    content = (
        'loadGeoJSON({"layerFoo": { "type": "Feature",'
        '  "geometry": {'
        '    "type": "Point",'
        '    "coordinates": [2, 49]'
        '    },'
        '  "name": "bar"'
        '},'
        '"layerBar": { "type": "FeatureCollection", "features" : [  {'
        '  "type": "Feature",'
        '  "geometry": {'
        '    "type": "Point",'
        '    "coordinates": [2, 49]'
        '    },'
        '  "other_name": "baz"'
        '}]}})')

    geom = ogr.CreateGeometryFromWkt('POINT (2 49)')

    src1 = ogr.Open(content)
    self.assertIsNotNone(src1)

    filepath = '/vsimem/ogr_geojson_24.js'
    gdal.FileFromMemBuffer(filepath, content)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      src2 = ogr.Open(filepath)
    self.assertIsNotNone(src2)

    for src in src1, src2:
      layer = src.GetLayerByName('layerFoo')
      self.assertIsNotNone(layer)
      feature = layer.GetNextFeature()
      self.assertEqual(feature.GetFieldAsString('name'), 'bar')
      self.CheckFeatureGeometry(feature, geom)

      layer = src.GetLayerByName('layerBar')
      self.assertIsNotNone(layer)
      feature = layer.GetNextFeature()
      self.assertEqual(feature.GetFieldAsString('other_name'), 'baz')
      self.CheckFeatureGeometry(feature, geom)

  def test25Topo1(self):
    filepath = ogr_util.GetTestFilePath('topojson1.topojson')
    src = self.CheckOpen(filepath)

    layer = src.GetLayer(0)
    self.assertEqual(layer.GetName(), 'a_layer')
    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature,
                              'LINESTRING (100 1000,110 1000,110 1100)')

    layer = src.GetLayer(1)
    self.assertEqual(layer.GetName(), 'TopoJSON')

    expected = [
        (None, None, 'POINT EMPTY'),
        (None, None, 'POINT EMPTY'),
        (None, None, 'POINT EMPTY'),
        (None, None, 'POINT (100 1010)'),
        (None, None, 'LINESTRING EMPTY'),
        (None, None, 'LINESTRING EMPTY'),
        (None, None, 'LINESTRING EMPTY'),
        (None, None, 'LINESTRING EMPTY'),
        (None, None, 'LINESTRING EMPTY'),
        (None, None, 'LINESTRING EMPTY'),
        (None, None, 'LINESTRING EMPTY'),
        (None, None, 'LINESTRING EMPTY'),
        (None,
         None if int(gdal.VersionInfo()) < 2000000 else '0',
         'LINESTRING EMPTY'),
        (None, 'foo', 'LINESTRING EMPTY'),
        ('1', None, 'LINESTRING (100 1000,110 1000,110 1100)'),
        ('2', None, 'LINESTRING (110 1100,110 1000,100 1000)'),
        (None, None, 'POLYGON EMPTY'),
        (None, None, 'POLYGON EMPTY'),
        (None, None, 'POLYGON EMPTY'),
        (None, None, 'POLYGON ((100 1000,110 1000,110 1100,100 1100,100 1000),'
         '(101 1010,101 1090,109 1090,109 1010,101 1010))'),
        (None, None, 'POLYGON ((110 1100,110 1000,100 1000,100 1100,110 1100),'
         '(101 1010,109 1010,109 1090,101 1090,101 1010))'),
        (None, None, 'MULTIPOINT EMPTY'),
        (None, None, 'MULTIPOINT EMPTY'),
        (None, None, 'MULTIPOINT EMPTY'),
        (None, None, 'MULTIPOINT EMPTY'),
        (None, None, 'MULTIPOINT (100 1010,101 1020)'),
        (None, None, 'MULTIPOLYGON EMPTY'),
        (None, None, 'MULTIPOLYGON EMPTY'),
        (None, None, 'MULTIPOLYGON EMPTY'),
        (None, None, 'MULTIPOLYGON (((110 1100,110 1000,100 1000,100 1100,110 '
         '1100)),((101 1010,109 1010,109 1090,101 1090,101 1010)))'),
        (None, None, 'MULTILINESTRING EMPTY'),
        (None, None, 'MULTILINESTRING EMPTY'),
        (None, None, 'MULTILINESTRING ((100 1000,110 1000,110 1100))'),
        (None, None, 'MULTILINESTRING ((100 1000,110 1000,110 1100,100 1100,'
         '100 1000))'),
        (None, None, 'MULTILINESTRING ((100 1000,110 1000,110 1100,100 1100,'
         '100 1000),(101 1010,101 1090,109 1090,109 1010,101 1010))'),
    ]
    self.assertEqual(layer.GetFeatureCount(), len(expected))
    for feature_num, feature in enumerate(ogr_util.Features(layer)):
      id_val, name, geom_str = expected[feature_num]
      self.assertEqual(feature.GetField('id'), id_val)
      self.assertEqual(feature.GetField('name'), name)
      self.assertEqual(feature.GetGeometryRef().ExportToWkt(), geom_str)

  def test25Topo2(self):
    filepath = ogr_util.GetTestFilePath('topojson2.topojson')
    src = self.CheckOpen(filepath)
    layer = src.GetLayer(0)
    self.assertEqual(layer.GetName(), 'a_layer')
    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature,
                              'LINESTRING (100 1000,110 1000,110 1100)')

    layer = src.GetLayer(1)
    self.assertEqual(layer.GetName(), 'TopoJSON')
    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature,
                              'LINESTRING (100 1000,110 1000,110 1100)')

  def test26EightByte64Bit(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features":['
        '{"type": "Feature", "id": 1,'
        ' "geometry": {"type":"Point","coordinates":[1,2]},'
        ' "properties": { "intvalue" : 1, "int64" : 1234567890123, '
        '                        "intlist" : [1] }},'
        '{"type": "Feature", "id": 1234567890123,'
        ' "geometry": {"type":"Point","coordinates":[3,4]},'
        ' "properties": { "intvalue" : 1234567890123, '
        '                        "intlist" : [1, 1234567890123] }},'
        ' ]}')
    self.assertIsNotNone(src)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)

    self.assertIsNotNone(layer.GetMetadataItem(ogr.OLMD_FID64))

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetFID(), 1)
    self.assertEqual(feature.GetField('intvalue'), 1)
    self.assertEqual(feature.GetField('int64'), 1234567890123)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetFID(), 1234567890123)
    self.assertEqual(feature.GetField('intvalue'), 1234567890123)
    self.assertEqual(feature.GetField('intlist'), [1, 1234567890123])

  def test26VsiMem64Bit(self):
    filepath = '/vsimem/ogr_geojson_26.json'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      layer = dst.CreateLayer('test')
      self.assertEqual(
          layer.CreateField(ogr.FieldDefn('int64', ogr.OFTInteger64)), 0)
      self.assertEqual(
          layer.CreateField(ogr.FieldDefn('int64list', ogr.OFTInteger64List)),
          0)
      feature = ogr.Feature(layer.GetLayerDefn())
      self.assertEqual(feature.SetFID(1234567890123), 0)
      feature.SetField(0, 1234567890123)
      feature.SetFieldInteger64List(1, [1234567890123])
      self.assertEqual(layer.CreateFeature(feature), 0)
      dst = None

      src = gdal.VSIFOpenL(filepath, 'rb')
      data = gdal.VSIFReadL(1, 10000, src).decode('ascii')
      gdal.VSIFCloseL(src)

      self.assertIn(
          '{ "type": "Feature", "id": 1234567890123, "properties": { '
          '"int64": 1234567890123, "int64list": [ 1234567890123 ] }, '
          '"geometry": null }',
          data)

  def test27StringsFor64Bit(self):
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      src = ogr.Open(
          '{"type": "FeatureCollection", "features":['
          '{"type": "Feature",'
          ' "geometry": {"type":"Point","coordinates":[1,2]},'
          ' "properties": { "intvalue" : 1 }},'
          '{"type": "Feature",'
          ' "geometry": {"type":"Point","coordinates":[3,4]},'
          ' "properties": { "intvalue" : 12345678901231234567890123 }},'
          ' ]}')
    self.assertIsNotNone(self)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('intvalue'), 1)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('intvalue'), 9223372036854775807)

  def test28EsriPointWithZ(self):
    filepath = ogr_util.GetTestFilePath('esrizpoint.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbPoint, 4,
                    (2, 2, 49, 49, 1, 1))

    self.assertEqual(
        int(layer.GetSpatialRef().GetAuthorityCode('GEOGCS')), 4326)

    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature, 'POINT(2 49 1)')

    if int(gdal.VersionInfo()) >= 2000000:
      self.assertEqual(feature.GetFID(), 1)
    self.assertEqual(feature.GetFieldAsInteger('fooInt'), 2)
    self.assertEqual(feature.GetFieldAsDouble('fooDouble'), 3.4)
    self.assertEqual(feature.GetFieldAsString('fooString'), '56')

  def test29EsriLineStringWithZ(self):
    filepath = ogr_util.GetTestFilePath('esrizlinestring.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbLineString, 0,
                    (2, 3, 49, 50, 1, 2))

    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature, 'LINESTRING (2 49 1,3 50 2)')

  def test30EsriMultiPointWithZ(self):
    filepath = ogr_util.GetTestFilePath('esrizmultipoint.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiPoint, 4,
                    (2, 3, 49, 50, 1, 2))

    self.assertEqual(
        int(layer.GetSpatialRef().GetAuthorityCode('GEOGCS')), 4326)

    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature, 'MULTIPOINT (2 49 1,3 50 2)')

  def test31EsriPolygonWithZ(self):
    filepath = ogr_util.GetTestFilePath('esrizpolygon.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbPolygon, 0,
                    (2, 3, 49, 50, 1, 4))

    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature,
                              'POLYGON ((2 49 1,2 50 2,3 50 3,3 49 4,2 49 1))')

  def test32EsriMultiPointWithM_WithoutZ(self):
    filepath = ogr_util.GetTestFilePath('esrihasmnozmultipoint.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiPoint, 4,
                    (2, 3, 49, 50))

    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature, 'MULTIPOINT (2 49,3 50)')

  def test33EsriMultiPointWithHasZ_WithoutZ(self):
    filepath = ogr_util.GetTestFilePath('esriinvalidhaszmultipoint.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiPoint, 4,
                    (2, 3, 49, 50))

    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature, 'MULTIPOINT (2 49,3 50)')

  def test34EsriMultiPointWithoutHasZWithZWithMWithHasM(self):
    filepath = ogr_util.GetTestFilePath('esrizmmultipoint.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)
    self.assertIsNotNone(layer)
    self.CheckLayer(layer, DEFAULT_LAYER_NAME, 1, ogr.wkbMultiPoint, 4,
                    (2, 3, 49, 50))

    feature = layer.GetNextFeature()
    self.CheckFeatureGeometry(feature, 'MULTIPOINT (2 49 1,3 50 2)')

  def test35HugeCoordinates(self):
    # https://trac.osgeo.org/gdal/ticket/5377

    filepath = '/vsimem/ogr_geojson_35.json'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      layer = dst.CreateLayer('foo')

      # TODO(schwehr): Simplify the rest of this block.

      feature = ogr.Feature(layer.GetLayerDefn())
      geometry = ogr.Geometry(ogr.wkbPoint)
      large_negative = -1.79769313486231571e+308
      geometry.AddPoint_2D(large_negative, large_negative)
      layer.CreateFeature(feature)

      # POINT (-1.79769313486232e+308 -1.79769313486232e+308)
      self.assertIn('-1.797', geometry.ExportToWkt())

      feature = ogr.Feature(layer.GetLayerDefn())
      geometry = ogr.Geometry(ogr.wkbPoint)
      geometry.AddPoint(float('-inf'), float('inf'), float('inf'))
      feature.SetGeometry(geometry)
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'POINT (-inf inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      geometry = ogr.Geometry(ogr.wkbLineString)
      geometry.AddPoint_2D(0, 0)
      geometry.AddPoint_2D(float('-inf'), float('inf'))
      feature.SetGeometry(geometry)
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'LINESTRING (0 0,-inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      geometry = ogr.Geometry(ogr.wkbPolygon)
      geometry2 = ogr.Geometry(ogr.wkbLinearRing)
      geometry2.AddPoint_2D(0, 0)
      geometry2.AddPoint_2D(float('-inf'), float('inf'))
      geometry.AddGeometry(geometry2)
      feature.SetGeometry(geometry)
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'POLYGON ((0 0,-inf inf))')
      self.assertEqual(geometry2.ExportToWkt(), 'LINEARRING (0 0,-inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      geometry = ogr.Geometry(ogr.wkbMultiPoint)
      geometry2 = ogr.Geometry(ogr.wkbPoint)
      geometry2.AddPoint_2D(0, 0)
      # TODO(schwehr): Does this next line wipe out what was just done?
      geometry2 = ogr.Geometry(ogr.wkbPoint)
      geometry2.AddPoint_2D(float('-inf'), float('inf'))
      geometry.AddGeometry(geometry2)
      feature.SetGeometry(geometry)
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'MULTIPOINT (-inf inf)')
      self.assertEqual(geometry2.ExportToWkt(), 'POINT (-inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      geometry = ogr.Geometry(ogr.wkbMultiLineString)
      geometry2 = ogr.Geometry(ogr.wkbLineString)
      geometry2.AddPoint_2D(0, 0)
      # TODO(schwehr): Does this next line wipe out what was just done?
      geometry2 = ogr.Geometry(ogr.wkbLineString)
      geometry2.AddPoint_2D(float('-inf'), float('inf'))
      geometry.AddGeometry(geometry2)
      feature.SetGeometry(geometry)
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'MULTILINESTRING ((-inf inf))')
      self.assertEqual(geometry2.ExportToWkt(), 'LINESTRING (-inf inf)')

      # TODO(schwehr): What?  Intermixed geometry 2 and 3.
      feature = ogr.Feature(layer.GetLayerDefn())
      geometry = ogr.Geometry(ogr.wkbMultiPolygon)
      geometry2 = ogr.Geometry(ogr.wkbPolygon)
      geometry3 = ogr.Geometry(ogr.wkbLinearRing)
      geometry3.AddPoint_2D(0, 0)
      geometry2.AddGeometry(geometry3)
      # TODO(schwehr): Does this next line wipe out what was just done?
      geometry2 = ogr.Geometry(ogr.wkbPolygon)
      # TODO(schwehr): Does this next line wipe out what was just done?
      geometry3 = ogr.Geometry(ogr.wkbLinearRing)
      geometry3.AddPoint_2D(float('-inf'), float('inf'))
      geometry2.AddGeometry(geometry3)
      geometry.AddGeometry(geometry2)

      self.assertEqual(geometry.ExportToWkt(), 'MULTIPOLYGON (((-inf inf)))')
      self.assertEqual(geometry2.ExportToWkt(), 'POLYGON ((-inf inf))')
      self.assertEqual(geometry3.ExportToWkt(), 'LINEARRING (-inf inf)')

      feature.SetGeometry(geometry)
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        layer.CreateFeature(feature)

      dst = None

      src = gdal.VSIFOpenL(filepath, 'rb')
      data = gdal.VSIFReadL(1, 10000, src).decode('ascii')
      gdal.VSIFCloseL(src)

    if gcore_util.IsGdal2():
      # TODO(schwehr): Why are these broken with GDAL 2.x?
      return

    self.assertIn('-1.79', data)
    self.assertIn('e+308', data)
    self.assertIn('"type": "Point", "coordinates": null }', data)
    self.assertIn('"type": "LineString", "coordinates": null }', data)
    self.assertIn('"type": "Polygon", "coordinates": null }', data)
    self.assertIn('"type": "MultiPoint", "coordinates": null }', data)
    self.assertIn('"type": "MultiLineString", "coordinates": null }', data)
    self.assertIn('"type": "MultiPolygon", "coordinates": null }', data)

  def test36Utf8BomIllegal(self):
    filepath = ogr_util.GetTestFilePath('point_with_utf8bom.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)

    # TODO(schwehr): Test the contents of the data source.

  def test37Boolean(self):
    src = ogr.Open(
        '{"type": "FeatureCollection","features": ['
        '  { "type": "Feature", "properties": { "bool" : false, '
        '      "not_bool": false, '
        '      "bool_list" : [false, true], "notbool_list" : [false, 3]}, '
        '        "geometry": null  },'
        '  { "type": "Feature", "properties": { "bool" : true, "not_bool": 2, '
        '      "bool_list" : [true] }, "geometry": null },'
        ']}')
    layer = src.GetLayer(0)
    feature_defn = layer.GetLayerDefn()
    bool_defn = feature_defn.GetFieldIndex('bool')
    self.assertEqual(
        feature_defn.GetFieldDefn(bool_defn).GetType(), ogr.OFTInteger)

    self.assertEqual(
        feature_defn.GetFieldDefn(bool_defn).GetSubType(),
        ogr.OFSTBoolean)

    not_bool_defn = feature_defn.GetFieldIndex('not_bool')
    self.assertEqual(
        feature_defn.GetFieldDefn(not_bool_defn).GetSubType(),
        ogr.OFSTNone)

    bool_list_defn = feature_defn.GetFieldIndex('bool_list')
    self.assertEqual(
        feature_defn.GetFieldDefn(bool_list_defn).GetType(),
        ogr.OFTIntegerList)
    self.assertEqual(
        feature_defn.GetFieldDefn(bool_list_defn).GetSubType(),
        ogr.OFSTBoolean)

    not_bool_list_defn = feature_defn.GetFieldIndex('notbool_list')
    self.assertEqual(
        feature_defn.GetFieldDefn(not_bool_list_defn).GetSubType(),
        ogr.OFSTNone)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('bool'), 0)
    self.assertEqual(feature.GetField('bool_list'), [0, 1])

    filepath = '/vsimem/ogr_geojson_37.json'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      layer = dst.CreateLayer('test')
      for i in range(feature_defn.GetFieldCount()):
        layer.CreateField(feature_defn.GetFieldDefn(i))
      dst_feature = ogr.Feature(layer.GetLayerDefn())
      dst_feature.SetFrom(feature)
      layer.CreateFeature(dst_feature)
      dst = None

      src = gdal.VSIFOpenL(filepath, 'rb')
      data = gdal.VSIFReadL(1, 10000, src).decode('ascii')
      gdal.VSIFCloseL(src)

    self.assertIn(
        '"bool": false, "not_bool": 0, "bool_list": [ false, true ], '
        '"notbool_list": [ 0, 3 ]',
        data)

  def test38DateTimeTypes(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '  { "type": "Feature", "properties": { '
        '      "dt": "2014-11-20 12:34:56+0100", "dt2": "2014/11/20", '
        '      "date":"2014/11/20", "time":"12:34:56", '
        '      "no_dt": "2014-11-20 12:34:56+0100", '
        '      "no_dt2": "2014-11-20 12:34:56+0100" }, "geometry": null },'
        '  { "type": "Feature", "properties": { "dt": "2014/11/20", '
        '      "dt2": "2014/11/20T12:34:56Z", "date":"2014-11-20", '
        '      "time":"12:34:56", "no_dt": "foo", "no_dt2": 1 },'
        '  "geometry": null }'
        ']}')

    layer = src.GetLayer(0)
    feature_defn = layer.GetLayerDefn()

    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('dt')).GetType(),
        ogr.OFTDateTime)
    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('dt2')).GetType(),
        ogr.OFTDateTime)
    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('date')).GetType(),
        ogr.OFTDate)
    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('time')).GetType(),
        ogr.OFTTime)
    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('no_dt')).GetType(),
        ogr.OFTString)
    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('no_dt2')).GetType(),
        ogr.OFTString)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('dt'), '2014/11/20 12:34:56+01')
    self.assertEqual(feature.GetField('dt2'), '2014/11/20 00:00:00')
    self.assertEqual(feature.GetField('date'), '2014/11/20')
    self.assertEqual(feature.GetField('time'), '12:34:56')

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('dt'), '2014/11/20 00:00:00')
    self.assertEqual(feature.GetField('dt2'), '2014/11/20 12:34:56+00')
    self.assertEqual(feature.GetField('date'), '2014/11/20')
    self.assertEqual(feature.GetField('time'), '12:34:56')

  def test39TopObjectLevel(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '{ "type": "Feature", "id" : "foo", "properties": { "bar" : "baz" }, '
        '"geometry": null },'
        '] }')
    layer = src.GetLayer(0)
    feature_defn = layer.GetLayerDefn()
    self.assertEqual(feature_defn.GetFieldDefn(0).GetName(), 'id')
    self.assertEqual(feature_defn.GetFieldDefn(0).GetType(), ogr.OFTString)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('id'), 'foo')
    self.assertEqual(feature.GetField('bar'), 'baz')

  def test39TopObjectLevelPropertiesIdHasPrecedence(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '  { "type": "Feature", "id" : "foo", "properties": { "id" : 6 }, '
        '    "geometry": null },'
        ']}')
    layer = src.GetLayer(0)
    feature_defn = layer.GetLayerDefn()
    self.assertEqual(feature_defn.GetFieldDefn(0).GetName(), 'id')
    self.assertEqual(feature_defn.GetFieldDefn(0).GetType(), ogr.OFTInteger)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('id'), 6)

  def test39TopObjectLevelPropertiesIdHasPrecedencePart2(self):
    ds = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '  { "type": "Feature", "id" : "foo", "properties": { "id" : "baz" }, '
        '    "geometry": null },'
        ']}')
    layer = ds.GetLayer(0)
    feature_defn = layer.GetLayerDefn()
    self.assertEqual(feature_defn.GetFieldDefn(0).GetName(), 'id')
    self.assertEqual(feature_defn.GetFieldDefn(0).GetType(), ogr.OFTString)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('id'), 'baz')

  def test40NestedAttributes(self):
    src = gdal.OpenEx(
        '{'
        '  "type": "FeatureCollection",'
        '  "features" :'
        '  ['
        '    {'
        '      "type": "Feature",'
        '      "geometry": {'
        '        "type": "Point",'
        '        "coordinates": [ 2, 49 ]'
        '      },'
        '      "properties": {'
        '        "a_property": 1,'
        '        "some_object": {'
        '          "a_property": 1,'
        '          "another_property": 2'
        '        }'
        '      }'
        '    },'
        '    {'
        '      "type": "Feature",'
        '      "geometry": {'
        '        "type": "Point",'
        '        "coordinates": [ 2, 49 ]'
        '      },'
        '      "properties": {'
        '        "a_property": "foo",'
        '        "some_object": {'
        '          "a_property": 1,'
        '          "another_property": 2.34'
        '        }'
        '      }'
        '    }'
        '  ]'
        '}',
        gdal.OF_VECTOR,
        open_options=['FLATTEN_NESTED_ATTRIBUTES=YES',
                      'NESTED_ATTRIBUTE_SEPARATOR=.'])
    layer = src.GetLayer()
    layer.GetNextFeature()
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('a_property'), 'foo')
    self.assertEqual(feature.GetField('some_object.a_property'), 1)
    self.assertEqual(feature.GetField('some_object.another_property'), 2.34)

  def test41CreateGeometryFromJsonDefaultWgs84(self):
    geometry = ogr.CreateGeometryFromJson(
        '{ "type": "Point", "coordinates" : [ 2, 49] }')
    self.assertIsNotNone(geometry)
    self.assertEqual(geometry.ExportToWkt(), 'POINT (2 49)')

    srs = geometry.GetSpatialReference()
    self.assertIn('WGS 84', srs.ExportToWkt())

  def test41CreateGeometryFromJsonExplicitCrs(self):
    geometry = ogr.CreateGeometryFromJson(
        '{ "type": "Point", "coordinates" : [ 2, 49], '
        '"crs": { "type": "name", "properties": { '
        '"name": "urn:ogc:def:crs:EPSG::4322" } } }')

    srs = geometry.GetSpatialReference()
    self.assertIn('4322', srs.ExportToWkt())

  def test42EsriFeatureServiceScrolling(self):
    result_offset_0 = (
        '{ "type":"FeatureCollection",'
        '  "properties" : {'
        '    "exceededTransferLimit" : true'
        '  },'
        '  "features" :'
        '  ['
        '    {'
        '      "type": "Feature",'
        '      "geometry": {'
        '        "type": "Point",'
        '        "coordinates": [ 2, 49 ]'
        '      },'
        '      "properties": {'
        '        "id": 1,'
        '        "a_property": 1,'
        '      }'
        '    } ] }')

    basepath = '/vsimem/geojson/test.json'
    limit = '?resultRecordCount=1'

    filepath = basepath + limit
    gdal.FileFromMemBuffer(filepath, result_offset_0)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      src = ogr.Open(filepath)
      layer = src.GetLayer(0)
      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertEqual(feature.GetFID(), 1)
      self.assertIsNone(layer.GetNextFeature())

    limit = '?resultRecordCount=10'
    filepath = basepath + limit
    gdal.FileFromMemBuffer(filepath, result_offset_0)
    src = ogr.Open(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      layer = src.GetLayer(0)
      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertIsNone(layer.GetNextFeature())

    filepath1 = basepath + '?'
    limit = '?resultRecordCount=1'
    offset = '&resultOffset=0'
    filepath2 = basepath + limit + offset

    gdal.FileFromMemBuffer(filepath1, result_offset_0)
    gdal.FileFromMemBuffer(filepath2, result_offset_0)

    src = ogr.Open(filepath1)
    layer = src.GetLayer(0)
    feature = layer.GetNextFeature()
    self.assertIsNotNone(feature)
    self.assertEqual(feature.GetFID(), 1)
    self.assertIsNone(layer.GetNextFeature())
    layer.ResetReading()
    feature = layer.GetNextFeature()
    self.assertIsNotNone(feature)
    self.assertEqual(feature.GetFID(), 1)

    # TODO(schwehr): Add the rest of the original test.

  def test43FeatureWithoutGeometry(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features":['
        '{"type": "Feature", "properties": {"foo": "bar"}}]}')

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetFieldAsString('foo'), 'bar')


if __name__ == '__main__':
  unittest.main()
