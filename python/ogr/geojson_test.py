# Copyright 2015 Google Inc. All Rights Reserved.
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

https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_geojson.py

Tests are numbered to match the autotest test when there is a match.

TODO(schwehr): Use the json module to convert python to json strings.
"""

from collections.abc import Iterator
import contextlib
import json
import locale
import math
import os
import struct
from typing import Any

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

DRIVER = ogr_util.GEOJSON_DRIVER
EXT = '.geojson'

DEFAULT_LAYER_NAME = 'OGRGeoJSON'


def setUpModule():
  ogr_util.SetupTestEnv()
  gdal.DontUseExceptions()


@contextlib.contextmanager
def _enable_exceptions() -> Iterator[None]:
  with (
      gdal.ExceptionMgr(useExceptions=True),
      ogr.ExceptionMgr(useExceptions=True),
      osr.ExceptionMgr(useExceptions=True),
  ):
    yield


def CreateField(layer, name, field_type=ogr.OFTString):
  field_definition = ogr.FieldDefn(name, field_type)
  layer.CreateField(field_definition)
  field_definition.Destroy()


class OgrGeoJsonTest(ogr_util.DriverTestCase, parameterized.TestCase):

  def setUp(self):  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)

  def _verify_shape_copy_to_geojson(
      self,
      shape_name: str,
      fids: list[int],
      names: list[str],
      compress: str | None = None,
  ) -> None:
    """Copies a shapefile to GeoJSON (optionally compressed) and verifies it."""
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_copy/') as tmpdir:
      if compress == '/vsigzip/':
        dst_name = f'/vsigzip/{tmpdir}/{shape_name}.geojson.gz'
      else:
        dst_name = f'{tmpdir}/{shape_name}.geojson'

      with self.driver.CreateDataSource(dst_name) as dst:
        self.assertIsNotNone(dst)
        lyr = dst.CreateLayer(shape_name)
        self.assertIsNotNone(lyr)
        self.assertEqual(
            lyr.GetDataset().GetDescription(), dst.GetDescription()
        )
        CreateField(lyr, 'FID', ogr.OFTReal)
        CreateField(lyr, 'NAME', ogr.OFTString)

        src_path = ogr_util.GetTestFilePath(f'{shape_name}.shp')
        with ogr.Open(src_path) as shp_ds:
          shp_lyr = shp_ds.GetLayer(0)
          orig_feats = list(ogr_util.Features(shp_lyr))

        dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
        for feat in orig_feats:
          dst_feat.SetFrom(feat)
          self.assertEqual(lyr.CreateFeature(dst_feat), 0)

      with ogr.Open(dst_name) as ds:
        self.assertIsNotNone(ds)
        lyr = ds.GetLayer(0)
        self.assertIsNotNone(lyr)
        self.CheckFeaturesAgainstList(lyr, 'FID', fids)
        lyr.ResetReading()
        self.CheckFeaturesAgainstList(lyr, 'NAME', names)
        lyr.ResetReading()
        for orig_feat in orig_feats:
          feat = lyr.GetNextFeature()
          self.assertIsNotNone(feat)
          self.CheckFeatureGeometry(
              feat, orig_feat.GetGeometryRef(), max_error=0.001
          )

  def test02Point(self):
    # TODO(schwehr): Make this type of block a helper function.
    filepath = ogr_util.GetTestFilePath('point.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('point')
    self.CheckLayer(
        layer, 'point', 1, ogr.wkbPoint, 0, (100.0, 100.0, 0.0, 0.0)
    )

  def test03Line(self):
    filepath = ogr_util.GetTestFilePath('linestring.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('linestring')
    self.CheckLayer(
        layer, 'linestring', 1, ogr.wkbLineString, 0, (100.0, 101.0, 0.0, 1.0)
    )

  def test04Polygon(self):
    filepath = ogr_util.GetTestFilePath('polygon.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('polygon')
    self.CheckLayer(
        layer, 'polygon', 1, ogr.wkbPolygon, 0, (100.0, 101.0, 0.0, 1.0)
    )

  def test05GeometryCollection(self):
    filepath = ogr_util.GetTestFilePath('geometrycollection.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('geometrycollection')
    self.CheckLayer(
        layer,
        'geometrycollection',
        1,
        ogr.wkbGeometryCollection,
        0,
        (100.0, 102.0, 0.0, 1.0),
    )

  def test06MultiPoint(self):
    filepath = ogr_util.GetTestFilePath('multipoint.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('multipoint')
    self.CheckLayer(
        layer, 'multipoint', 1, ogr.wkbMultiPoint, 0, (100.0, 101.0, 0.0, 1.0)
    )

  def test07MultiLineString(self):
    filepath = ogr_util.GetTestFilePath('multilinestring.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('multilinestring')
    self.CheckLayer(
        layer,
        'multilinestring',
        1,
        ogr.wkbMultiLineString,
        0,
        (100.0, 103.0, 0.0, 3.0),
    )

  def test08MultiPolygon(self):
    filepath = ogr_util.GetTestFilePath('multipolygon.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('multipolygon')
    self.CheckLayer(
        layer,
        'multipolygon',
        1,
        ogr.wkbMultiPolygon,
        0,
        (100.0, 103.0, 0.0, 3.0),
    )

  def test09ShapeToGeoJson(self):
    tests = [
        ('gjpoint', [1], ['Point 1']),
        ('gjline', [1], ['Line 1']),
        ('gjpoly', [1], ['Polygon 1']),
        ('gjmultipoint', [1], ['MultiPoint 1']),
        ('gjmultiline', [2], ['MultiLine 1']),
        ('gjmultipoly', [2], ['MultiPoly 1']),
    ]
    for shape_name, fids, names in tests:
      self._verify_shape_copy_to_geojson(shape_name, fids, names)

  def test10ShapeToGzipGeoJson(self):
    tests = [
        ('gjpoint', [1], ['Point 1']),
        ('gjline', [1], ['Line 1']),
        ('gjpoly', [1], ['Polygon 1']),
        ('gjmultipoint', [1], ['MultiPoint 1']),
        ('gjmultiline', [2], ['MultiLine 1']),
        ('gjmultipoly', [2], ['MultiPoly 1']),
    ]
    for shape_name, fids, names in tests:
      self._verify_shape_copy_to_geojson(
          shape_name, fids, names, compress='/vsigzip/'
      )

  def test11SrsName(self):
    filepath = ogr_util.GetTestFilePath('srs_name.geojson')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayerByName('srs_name')
    self.CheckLayer(
        layer,
        'srs_name',
        1,
        ogr.wkbGeometryCollection,
        0,
        (100.0, 102.0, 0.0, 1.0),
    )

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
    with gdal.quiet_errors():
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

      dst = None  # pylint: disable=unused-variable

      # Do some minimalistic checking.
      with gdal.quiet_errors():
        dst = ogr.Open(dst_filepath)
      self.assertEqual(dst.GetLayerCount(), 1)
      dst_layer = dst.GetLayer(0)
      # TODO(schwehr): Explain how there is one more geometry than with GDAL 1.
      self.assertEqual(dst_layer.GetFeatureCount(), 27)

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
        'type': 'Feature',
    }

    result = json.loads(feature.ExportToJson())
    self.assertEqual(result, expected)
    self.assertEqual(feature.ExportToJson(as_object=True), expected)

  def test20WithoutExtension(self):
    filenames = (
        'point.geojson',
        'geometrycollection.geojson',
        'multilinestring.geojson',
        'srs_name.geojson',
        'esrilinestring.json',
        'esripolygon.json',
    )

    for filename in filenames:
      filepath = ogr_util.GetTestFilePath(filename)
      with open(filepath, 'rb') as f:
        data = f.read()

      dst_filepath = os.path.join('/vsimem/', filename)
      with gcore_util.GdalUnlinkWhenDone(dst_filepath):
        dst = gdal.VSIFOpenL(dst_filepath, 'wb')
        gdal.VSIFWriteL(data, 1, len(data), dst)
        gdal.VSIFCloseL(dst)

        with gdal.quiet_errors():
          src = ogr.Open(dst_filepath)
          self.assertIsNotNone(src)

  def testGeoJSON21GeocouchSpatialList(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features":['
        '  {"type": "Feature",'
        '   "geometry": {"type":"Point","coordinates":[1,2]},'
        '   "properties": {"_id":"aid", "_rev":"arev", "type":"Feature",'
        '   "properties": {"intvalue" : 2, "floatvalue" : 3.2, '
        '   "strvalue" : "foo"}}}]}'
    )
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
        '     "properties":{"intvalue" : 3.5, "str2value" : "bar"}}}]}'
    )
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

    self.assertEqual(layer.GetExtent(), (1.0, 2.0, 10.0, 20.0))
    self.assertEqual(layer.GetExtent(geom_field=0), (1.0, 2.0, 10.0, 20.0))
    with gdal.quiet_errors():
      self.assertIsNone(layer.GetExtent(geom_field=1, can_return_null=True))

    dst = None  # pylint: disable=unused-variable

    with gcore_util.GdalUnlinkWhenDone(filepath):
      src = self.CheckOpen(filepath)
      layer = src.GetLayer(0)
      srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
      self.assertIsSame(layer.GetSpatialRef(), srs)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('"bbox": [ 1, 10, 2, 20 ]', data)
      self.assertIn('"bbox": [ 1.0, 10.0, 1.0, 10.0 ]', data)
      self.assertIn('"bbox": [ 2.0, 20.0, 2.0, 20.0 ]', data)

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
        '}]}})'
    )

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
        ' ]}'
    )
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
          layer.CreateField(ogr.FieldDefn('int64', ogr.OFTInteger64)), 0
      )
      self.assertEqual(
          layer.CreateField(ogr.FieldDefn('int64list', ogr.OFTInteger64List)), 0
      )
      feature = ogr.Feature(layer.GetLayerDefn())
      self.assertEqual(feature.SetFID(1234567890123), 0)
      feature.SetField(0, 1234567890123)
      feature.SetFieldInteger64List(1, [1234567890123])
      self.assertEqual(layer.CreateFeature(feature), 0)
      dst = None  # pylint: disable=unused-variable

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)

      self.assertIn(
          '{ "type": "Feature", "id": 1234567890123, "properties": { '
          '"int64": 1234567890123, "int64list": [ 1234567890123 ] }, '
          '"geometry": null }',
          data,
      )

  def test27StringsFor64Bit(self):
    with gdal.quiet_errors():
      src = ogr.Open(
          '{"type": "FeatureCollection", "features":['
          '{"type": "Feature",'
          ' "geometry": {"type":"Point","coordinates":[1,2]},'
          ' "properties": { "intvalue" : 1 }},'
          '{"type": "Feature",'
          ' "geometry": {"type":"Point","coordinates":[3,4]},'
          ' "properties": { "intvalue" : 12345678901231234567890123 }},'
          ' ]}'
      )
    self.assertIsNotNone(src)
    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('intvalue'), 1)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('intvalue'), 9223372036854775807)

  def test35HugeCoordinates(self):
    # https://trac.osgeo.org/gdal/ticket/5377

    filepath = '/vsimem/ogr_geojson_35.json'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      layer = dst.CreateLayer('foo')

      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetFID(1)
      geometry = ogr.Geometry(ogr.wkbPoint)
      large_negative = -1.79769313486231571e308
      geometry.AddPoint_2D(large_negative, large_negative)
      feature.SetGeometry(geometry)
      layer.CreateFeature(feature)

      # POINT (-1.79769313486232e+308 -1.79769313486232e+308)
      self.assertIn('-1.797', geometry.ExportToWkt())

      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetFID(2)
      geometry = ogr.Geometry(ogr.wkbPoint)
      geometry.AddPoint(float('-inf'), float('inf'), float('inf'))
      feature.SetGeometry(geometry)
      with gdal.quiet_errors():
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'POINT (-inf inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetFID(3)
      geometry = ogr.Geometry(ogr.wkbLineString)
      geometry.AddPoint_2D(0, 0)
      geometry.AddPoint_2D(float('-inf'), float('inf'))
      feature.SetGeometry(geometry)
      with gdal.quiet_errors():
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'LINESTRING (0 0,-inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetFID(4)
      geometry = ogr.Geometry(ogr.wkbPolygon)
      geometry2 = ogr.Geometry(ogr.wkbLinearRing)
      geometry2.AddPoint_2D(0, 0)
      geometry2.AddPoint_2D(float('-inf'), float('inf'))
      geometry.AddGeometry(geometry2)
      feature.SetGeometry(geometry)
      with gdal.quiet_errors():
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'POLYGON ((0 0,-inf inf))')
      self.assertEqual(geometry2.ExportToWkt(), 'LINEARRING (0 0,-inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetFID(5)
      geometry = ogr.Geometry(ogr.wkbMultiPoint)
      geometry2 = ogr.Geometry(ogr.wkbPoint)
      geometry2.AddPoint_2D(float('-inf'), float('inf'))
      geometry.AddGeometry(geometry2)
      feature.SetGeometry(geometry)
      with gdal.quiet_errors():
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'MULTIPOINT (-inf inf)')
      self.assertEqual(geometry2.ExportToWkt(), 'POINT (-inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetFID(6)
      geometry = ogr.Geometry(ogr.wkbMultiLineString)
      geometry2 = ogr.Geometry(ogr.wkbLineString)
      geometry2.AddPoint_2D(float('-inf'), float('inf'))
      geometry.AddGeometry(geometry2)
      feature.SetGeometry(geometry)
      with gdal.quiet_errors():
        layer.CreateFeature(feature)

      self.assertEqual(geometry.ExportToWkt(), 'MULTILINESTRING ((-inf inf))')
      self.assertEqual(geometry2.ExportToWkt(), 'LINESTRING (-inf inf)')

      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetFID(7)
      geometry = ogr.Geometry(ogr.wkbMultiPolygon)
      geometry2 = ogr.Geometry(ogr.wkbPolygon)
      geometry3 = ogr.Geometry(ogr.wkbLinearRing)
      geometry3.AddPoint_2D(float('-inf'), float('inf'))
      geometry2.AddGeometry(geometry3)
      geometry.AddGeometry(geometry2)

      self.assertEqual(geometry.ExportToWkt(), 'MULTIPOLYGON (((-inf inf)))')
      self.assertEqual(geometry2.ExportToWkt(), 'POLYGON ((-inf inf))')
      self.assertEqual(geometry3.ExportToWkt(), 'LINEARRING (-inf inf)')

      feature.SetGeometry(geometry)
      with gdal.quiet_errors():
        layer.CreateFeature(feature)

      dst = None  # pylint: disable=unused-variable

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('-1.79', data)
      self.assertIn('e+308', data)
      for ident in range(2, 8):
        self.assertIn(
            f'{{ "type": "Feature", "id": {ident}, "properties": {{ }},'
            ' "geometry": null }',
            data,
        )

  def test36Utf8BomIllegal(self):
    filepath = ogr_util.GetTestFilePath('point_with_utf8bom.json')
    src = self.CheckOpen(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayer(0)
    self.CheckLayer(
        layer,
        'point_with_utf8bom',
        1,
        ogr.wkbPoint,
        0,
        (100.0, 100.0, 0.0, 0.0),
    )

  def test37Boolean(self):
    src = ogr.Open(
        '{"type": "FeatureCollection","features": ['
        '  { "type": "Feature", "properties": { "bool" : false, '
        '      "not_bool": false, '
        '      "bool_list" : [false, true], "notbool_list" : [false, 3]}, '
        '        "geometry": null  },'
        '  { "type": "Feature", "properties": { "bool" : true, "not_bool": 2, '
        '      "bool_list" : [true] }, "geometry": null },'
        ']}'
    )
    layer = src.GetLayer(0)
    feature_defn = layer.GetLayerDefn()
    bool_defn = feature_defn.GetFieldIndex('bool')
    self.assertEqual(
        feature_defn.GetFieldDefn(bool_defn).GetType(), ogr.OFTInteger
    )

    self.assertEqual(
        feature_defn.GetFieldDefn(bool_defn).GetSubType(), ogr.OFSTBoolean
    )

    not_bool_defn = feature_defn.GetFieldIndex('not_bool')
    self.assertEqual(
        feature_defn.GetFieldDefn(not_bool_defn).GetSubType(), ogr.OFSTNone
    )

    bool_list_defn = feature_defn.GetFieldIndex('bool_list')
    self.assertEqual(
        feature_defn.GetFieldDefn(bool_list_defn).GetType(), ogr.OFTIntegerList
    )
    self.assertEqual(
        feature_defn.GetFieldDefn(bool_list_defn).GetSubType(), ogr.OFSTBoolean
    )

    not_bool_list_defn = feature_defn.GetFieldIndex('notbool_list')
    self.assertEqual(
        feature_defn.GetFieldDefn(not_bool_list_defn).GetSubType(), ogr.OFSTJSON
    )

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
      dst = None  # pylint: disable=unused-variable

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)

    self.assertIn('{ "bool": false, "not_bool": 0, "bool_list":', data)

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
        ']}'
    )

    layer = src.GetLayer(0)
    feature_defn = layer.GetLayerDefn()

    self.assertEqual(
        feature_defn.GetFieldDefn(feature_defn.GetFieldIndex('dt')).GetType(),
        ogr.OFTDateTime,
    )
    self.assertEqual(
        feature_defn.GetFieldDefn(feature_defn.GetFieldIndex('dt2')).GetType(),
        ogr.OFTDateTime,
    )
    self.assertEqual(
        feature_defn.GetFieldDefn(feature_defn.GetFieldIndex('date')).GetType(),
        ogr.OFTDate,
    )
    self.assertEqual(
        feature_defn.GetFieldDefn(feature_defn.GetFieldIndex('time')).GetType(),
        ogr.OFTTime,
    )
    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('no_dt')
        ).GetType(),
        ogr.OFTString,
    )
    self.assertEqual(
        feature_defn.GetFieldDefn(
            feature_defn.GetFieldIndex('no_dt2')
        ).GetType(),
        ogr.OFTString,
    )

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

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_38/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      # dummy NATIVE_DATA so that input values are not copied directly.
      gdal.VectorTranslate(
          out_path, src, options='-lco NATIVE_DATA=dummy -of GeoJSON'
      )
      data = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      self.assertIn(
          '"dt": "2014-11-20T12:34:56+01:00", "dt2": "2014-11-20T00:00:00",'
          ' "date": "2014-11-20", "time": "12:34:56"',
          data,
      )

    ds_str = gdal.OpenEx(
        '{"type": "FeatureCollection", "features": ['
        '{ "type": "Feature", "properties": { '
        '"dt": "2014-11-20 12:34:56+0100", "dt2": "2014\\/11\\/20", '
        '"date":"2014\\/11\\/20", "time":"12:34:56", '
        '"no_dt": "2014-11-20 12:34:56+0100", '
        '"no_dt2": "2014-11-20 12:34:56+0100" }, "geometry": null }'
        '] }',
        open_options=['DATE_AS_STRING=YES'],
    )
    lyr_str = ds_str.GetLayer(0)
    defn_str = lyr_str.GetLayerDefn()
    for i in range(defn_str.GetFieldCount()):
      self.assertEqual(defn_str.GetFieldDefn(i).GetType(), ogr.OFTString)

  def test39TopObjectLevel(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '{ "type": "Feature", "id" : "foo", "properties": { "bar" : "baz" }, '
        '"geometry": null },'
        '] }'
    )
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
        ']}'
    )
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
        ']}'
    )
    layer = ds.GetLayer(0)
    feature_defn = layer.GetLayerDefn()
    self.assertEqual(feature_defn.GetFieldDefn(0).GetName(), 'id')
    self.assertEqual(feature_defn.GetFieldDefn(0).GetType(), ogr.OFTString)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('id'), 'baz')

  def test39TopObjectLevelExtendedIds(self):
    # id and properties.ID (#6538)
    ds = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '{ "type": "Feature", "id" : 1, "properties": { "ID": 2 }, '
        '"geometry": null }] }'
    )
    lyr = ds.GetLayer(0)
    defn = lyr.GetLayerDefn()
    self.assertEqual(defn.GetFieldDefn(0).GetName(), 'ID')
    self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTInteger)
    feat = lyr.GetNextFeature()
    self.assertEqual(feat.GetFID(), 1)
    self.assertEqual(feat.GetField('ID'), 2)

    # Test handling of duplicated id
    gdal.ErrorReset()
    with gdal.quiet_errors():
      ds = ogr.Open(
          '{"type": "FeatureCollection", "features": ['
          '{ "type": "Feature", "id" : 1, "properties": { "foo": "bar" }, '
          '"geometry": null },'
          '{ "type": "Feature", "id" : 1, "properties": { "foo": "baz" }, '
          '"geometry": null },'
          '{ "type": "Feature", "id" : 2, "properties": { "foo": "baw" }, '
          '"geometry": null }] }'
      )
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    lyr = ds.GetLayer(0)
    for expected_fid, expected_foo in [(1, 'bar'), (2, 'baz'), (3, 'baw')]:
      feat = lyr.GetNextFeature()
      self.assertEqual(feat.GetFID(), expected_fid)
      self.assertEqual(feat.GetField('foo'), expected_foo)

    # Negative id
    ds = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '{ "type": "Feature", "id" : -1, "properties": { "foo": "bar" }, '
        '"geometry": null }] }'
    )
    lyr = ds.GetLayer(0)
    defn = lyr.GetLayerDefn()
    self.assertEqual(defn.GetFieldDefn(0).GetName(), 'id')
    self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTInteger)
    self.assertEqual(lyr.GetNextFeature().GetField('id'), -1)

    # Negative id 64bit
    for first_id, second_id in [(-1234567890123, -2), (-2, -1234567890123)]:
      ds = ogr.Open(
          '{"type": "FeatureCollection", "features": ['
          f'{{ "type": "Feature", "id" : {first_id}, '
          '"properties": { "foo": "bar" }, "geometry": null },'
          f'{{ "type": "Feature", "id" : {second_id}, '
          '"properties": { "foo": "baz" }, "geometry": null }] }}'
      )
      lyr = ds.GetLayer(0)
      defn = lyr.GetLayerDefn()
      self.assertEqual(defn.GetFieldDefn(0).GetName(), 'id')
      self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTInteger64)
      self.assertEqual(lyr.GetNextFeature().GetField('id'), first_id)

    # Positive and then negative id
    ds = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '{ "type": "Feature", "id" : 1, "properties": { "foo": "baz" }, '
        '"geometry": null },'
        '{ "type": "Feature", "id" : -1, "properties": { "foo": "bar" }, '
        '"geometry": null }] }'
    )
    lyr = ds.GetLayer(0)
    defn = lyr.GetLayerDefn()
    self.assertEqual(defn.GetFieldDefn(0).GetName(), 'id')
    self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTInteger)
    self.assertEqual(lyr.GetNextFeature().GetField('id'), 1)

    # Mix of int and string id
    ds = ogr.Open(
        '{"type": "FeatureCollection", "features": ['
        '{ "type": "Feature", "id" : -2, "properties": { "foo": "baz" }, '
        '"geometry": null },'
        '{ "type": "Feature", "id" : "str", "properties": { "foo": "bar" }, '
        '"geometry": null },'
        '{ "type": "Feature", "id" : -3, "properties": { "foo": "baz" }, '
        '"geometry": null }] }'
    )
    lyr = ds.GetLayer(0)
    defn = lyr.GetLayerDefn()
    self.assertEqual(defn.GetFieldDefn(0).GetName(), 'id')
    self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTString)
    self.assertEqual(lyr.GetNextFeature().GetField('id'), '-2')

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
        open_options=[
            'FLATTEN_NESTED_ATTRIBUTES=YES',
            'NESTED_ATTRIBUTE_SEPARATOR=.',
        ],
    )
    layer = src.GetLayer()
    layer.GetNextFeature()
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('a_property'), 'foo')
    self.assertEqual(feature.GetField('some_object.a_property'), 1)
    self.assertEqual(feature.GetField('some_object.another_property'), 2.34)

  def test41CreateGeometryFromJsonDefaultWgs84(self):
    geometry = ogr.CreateGeometryFromJson(
        '{ "type": "Point", "coordinates" : [ 2, 49] }'
    )
    self.assertIsNotNone(geometry)
    self.assertEqual(geometry.ExportToWkt(), 'POINT (2 49)')

    srs = geometry.GetSpatialReference()
    self.assertIn('WGS 84', srs.ExportToWkt())

  def test41CreateGeometryFromJsonExplicitCrs(self):
    geometry = ogr.CreateGeometryFromJson(
        '{ "type": "Point", "coordinates" : [ 2, 49], '
        '"crs": { "type": "name", "properties": { '
        '"name": "urn:ogc:def:crs:EPSG::4322" } } }'
    )

    srs = geometry.GetSpatialReference()
    self.assertIn('4322', srs.ExportToWkt())

    geometry_null_crs = ogr.CreateGeometryFromJson(
        '{ "type": "Point", "coordinates" : [ 2, 49], "crs": null }'
    )
    self.assertFalse(geometry_null_crs.GetSpatialReference())

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
        '    } ] }'
    )

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

    with (
        gcore_util.GdalUnlinkWhenDone(filepath1),
        gcore_util.GdalUnlinkWhenDone(filepath2),
    ):
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

  def test43FeatureWithoutGeometry(self):
    src = ogr.Open(
        '{"type": "FeatureCollection", "features":['
        '{"type": "Feature", "properties": {"foo": "bar"}}]}'
    )

    layer = src.GetLayerByName(DEFAULT_LAYER_NAME)

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetFieldAsString('foo'), 'bar')

  def _check_topo_geometry(
      self, actual: ogr.Feature | ogr.Geometry, expected: ogr.Geometry
  ) -> None:
    """Checks 2D topological geometry equality, matching upstream ogrtest."""
    geom = (
        actual.GetGeometryRef() if isinstance(actual, ogr.Feature) else actual
    )
    self.assertIsNotNone(geom)
    self.assertEqual(geom.GetGeometryName(), expected.GetGeometryName())
    if geom.Within(expected) and expected.Within(geom):
      return
    self.CheckFeatureGeometry(geom, expected)

  def _compare_layers(self, lyr: ogr.Layer, lyr_ref: ogr.Layer) -> None:
    defn = lyr.GetLayerDefn()
    defn_ref = lyr_ref.GetLayerDefn()
    self.assertEqual(defn.GetFieldCount(), defn_ref.GetFieldCount())
    for i in range(defn.GetFieldCount()):
      fld = defn.GetFieldDefn(i)
      fld_ref = defn_ref.GetFieldDefn(i)
      self.assertEqual(fld.GetNameRef(), fld_ref.GetNameRef())
      self.assertEqual(fld.GetType(), fld_ref.GetType())
      # Promoted JSON fields have OFSTJSON while JSON-encoded reference
      # strings have OFSTNone; upstream ogrtest.compare_layers checks values.
      if fld.GetSubType() != ogr.OFSTJSON:
        self.assertEqual(fld.GetSubType(), fld_ref.GetSubType())
      self.assertEqual(fld.GetWidth(), fld_ref.GetWidth())
      self.assertEqual(fld.GetPrecision(), fld_ref.GetPrecision())

    lyr.ResetReading()
    lyr_ref.ResetReading()
    while True:
      feat = lyr.GetNextFeature()
      feat_ref = lyr_ref.GetNextFeature()
      if feat is None or feat_ref is None:
        self.assertIsNone(feat)
        self.assertIsNone(feat_ref)
        break
      self.assertEqual(feat.GetFID(), feat_ref.GetFID())
      for i in range(defn.GetFieldCount()):
        self.assertEqual(feat.IsFieldNull(i), feat_ref.IsFieldNull(i))
        self.assertEqual(feat.IsFieldSet(i), feat_ref.IsFieldSet(i))
        self.assertEqual(feat.GetField(i), feat_ref.GetField(i))
      ref_geom = feat_ref.GetGeometryRef()
      if ref_geom is None:
        self.assertIsNone(feat.GetGeometryRef())
      else:
        self._check_topo_geometry(feat, ref_geom)

  def test_ogr_geojson_44(self):
    with _enable_exceptions(), self.assertRaises(Exception):
      ogr.Open('{"type": "FeatureCollection", "features":[ null ]}')

  def test_ogr_geojson_45(self):
    content = (
        '{"type": "FeatureCollection", "foo": "bar", "bar": "baz",'
        ' "features":[ { "type": "Feature", '
        '"foo": ["bar", "baz", 1.0, true, false,[],{}], '
        '"properties": { "myprop": "myvalue" }, "geometry": null } ]}'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_45/') as tmpdir:
      mem_file = f'{tmpdir}/ogr_geojson_45.json'
      for i in range(2):
        if i == 0:
          ds = gdal.OpenEx(
              content, gdal.OF_VECTOR, open_options=['NATIVE_DATA=YES']
          )
        else:
          gdal.FileFromMemBuffer(mem_file, content)
          ds = gdal.OpenEx(
              mem_file, gdal.OF_VECTOR, open_options=['NATIVE_DATA=YES']
          )
        with ds:
          lyr = ds.GetLayer(0)
          self.assertEqual(
              lyr.GetMetadataItem('NATIVE_DATA', 'NATIVE_DATA'),
              '{ "foo": "bar", "bar": "baz" }',
          )
          self.assertEqual(
              lyr.GetMetadataItem('NATIVE_MEDIA_TYPE', 'NATIVE_DATA'),
              'application/vnd.geo+json',
          )
          f = lyr.GetNextFeature()
          native_data = f.GetNativeData()
          if i == 0:
            expected = (
                (
                    '{ "type": "Feature", "foo": [ "bar", "baz", 1.000000,'
                    ' true, false, [ ], { } ], "properties": { "myprop":'
                    ' "myvalue" }, "geometry": null }'
                ),
                (
                    '{ "type": "Feature", "foo": [ "bar", "baz", 1.0, true,'
                    ' false, [ ], { } ], "properties": { "myprop": "myvalue" },'
                    ' "geometry": null }'
                ),
            )
          else:
            expected = (
                (
                    '{"type":"Feature","foo":["bar","baz",1.0,true,false,[],{}],'
                    '"properties":{"myprop":"myvalue"},"geometry":null}'
                ),
            )
          self.assertIn(native_data, expected)
          self.assertEqual(f.GetNativeMediaType(), 'application/vnd.geo+json')

      out_file = f'{tmpdir}/ogr_geojson_45_write.json'
      with self.driver.CreateDataSource(out_file) as ds:
        lyr = ds.CreateLayer(
            'test',
            options=[
                (
                    'NATIVE_DATA={ "type": "ignored", "bbox": [ 0, 0, 0, 0 ],'
                    ' "foo": "bar", "bar": "baz", "features": "ignored" }'
                ),
                'NATIVE_MEDIA_TYPE=application/vnd.geo+json',
            ],
        )
        f = ogr.Feature(lyr.GetLayerDefn())
        json_geom = (
            '{ "type": "GeometryCollection", "foo_gc": "bar_gc", '
            '"geometries" : ['
            '{ "type": "Point", "foo_point": "bar_point", '
            '"coordinates": [0,1,2, 3] },'
            '{ "type": "LineString", "foo_linestring": "bar_linestring", '
            '"coordinates": [[0,1,2, 4]] },'
            '{ "type": "MultiPoint", "foo_multipoint": "bar_multipoint", '
            '"coordinates": [[0,1,2, 5]] },'
            '{ "type": "MultiLineString", '
            '"foo_multilinestring": "bar_multilinestring", '
            '"coordinates": [[[0,1,2, 6]]] },'
            '{ "type": "Polygon", "foo_polygon": "bar_polygon", '
            '"coordinates": [[[0,1,2, 7]]] },'
            '{ "type": "MultiPolygon", "foo_multipolygon": "bar_multipolygon", '
            '"coordinates": [[[[0,1,2, 8]]]] }'
            '] }'
        )
        f.SetNativeData(
            '{ "type": "ignored", "bbox": "ignored", "properties" : "ignored",'
            f' "foo_feature": "bar_feature", "geometry": {json_geom} }}'
        )
        f.SetNativeMediaType('application/vnd.geo+json')
        with gdal.quiet_errors():  # warns on 4D coordinates
          f.SetGeometry(ogr.CreateGeometryFromJson(json_geom))
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(out_file, max_size=10000)
      for expected_substr in (
          '"bbox": [ 0, 1, 2, 0, 1, 2 ],',
          '"foo": "bar"',
          '"bar": "baz"',
          '"foo_feature": "bar_feature"',
          '"foo_gc": "bar_gc"',
          '"foo_point": "bar_point"',
          '"foo_linestring": "bar_linestring"',
          '"foo_multipoint": "bar_multipoint"',
          '"foo_multilinestring": "bar_multilinestring"',
          '"foo_polygon": "bar_polygon"',
          '"foo_multipolygon": "bar_multipolygon"',
          '3',
          '4',
          '5',
          '6',
          '7',
          '8',
      ):
        self.assertIn(expected_substr, data)

      # Native support with string and numeric id.
      for id_json in ('"foobarbaz"', '1234657890123'):
        src_ds = gdal.OpenEx(
            '{\n'
            '"type": "FeatureCollection",\n'
            '"features": [\n'
            '{ "type": "Feature",\n'
            f'  "id": {id_json},\n'
            '  "properties": {},\n'
            '  "geometry": null\n'
            '}\n'
            ']\n'
            '}\n',
            open_options=['NATIVE_DATA=YES'],
        )
        id_out = f'{tmpdir}/out.json'
        gdal.VectorTranslate(id_out, src_ds, format='GeoJSON')
        got = gcore_util.vsi_read_file_as_string(id_out, max_size=10000)
        expected = (
            '{\n'
            '"type": "FeatureCollection",\n'
            '"features": [\n'
            f'{{ "type": "Feature", "id": {id_json}, "properties": {{ }},'
            ' "geometry": null }\n'
            ']\n'
            '}\n'
        )
        self.assertEqual(json.loads(got), json.loads(expected))

  def test_ogr_geojson_46(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_46/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_46.json'
      with self.driver.CreateDataSource(filepath) as ds:
        lyr = ds.CreateLayer('test')
        lyr.CreateField(ogr.FieldDefn('myprop'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['myprop'] = '{ "a": "b" }'
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('{ "myprop": { "a": "b" } }', data)

  def test_ogr_geojson_47(self):
    # Update from inline definition not supported.
    with gdal.quiet_errors():
      ds = ogr.Open('{"type": "FeatureCollection", "features":[]}', update=1)
    self.assertIsNone(ds)

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_47/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_47.json'
      gdal.FileFromMemBuffer(
          filepath,
          '{"type": "FeatureCollection", "foo": "bar",'
          ' "features":[ { "type": "Feature", "bar": "baz", "properties":'
          ' { "myprop": "myvalue" }, "geometry": null } ]}',
      )

      with ogr.Open(filepath, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        f.SetField('myprop', 'another_value')
        lyr.SetFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('"foo": "bar"', data)
      self.assertIn('"bar": "baz"', data)
      self.assertNotIn('crs', data)
      self.assertIn('"myprop": "another_value"', data)

      # Test append support.
      with ogr.Open(filepath, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(1 2)'))
        lyr.CreateFeature(f)
        self.assertEqual(f.GetFID(), 1)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(2 3)'))
        lyr.CreateFeature(f)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetFID(), 0)

      with ogr.Open(filepath, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(4 5)'))
        lyr.CreateFeature(f)
        f.SetField('myprop', 'value_of_point_4_5')
        lyr.SetFeature(f)

      with ogr.Open(filepath) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 4)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('"foo": "bar"', data)
      self.assertIn('"bar": "baz"', data)
      self.assertNotIn('crs', data)
      self.assertIn('"myprop": "another_value"', data)
      self.assertIn('"myprop": "value_of_point_4_5"', data)
      self.assertNotIn('id', data)

      # Appending to empty array, array ending with null, trailing key, and
      # OGR_GEOJSON_REWRITE_IN_PLACE=YES.
      for initial_json in (
          '{ "type": "FeatureCollection", "features": []}',
          '{ "type": "FeatureCollection", "features": [ null ]}',
          '{ "type": "FeatureCollection", "features": [], "something": "else"}',
      ):
        gdal.FileFromMemBuffer(filepath, initial_json)
        with ogr.Open(filepath, update=1) as ds:
          lyr = ds.GetLayer(0)
          lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
        with ogr.Open(filepath) as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('something', data)

      with gdaltest.config_option('OGR_GEOJSON_REWRITE_IN_PLACE', 'YES'):
        gdal.FileFromMemBuffer(
            filepath,
            '{ "type": "FeatureCollection", "bbox": [0,0,0,0], "features":'
            ' [ { "type": "Feature", "geometry": { "type": "Point",'
            ' "coordinates": [0,0]} } ]}',
        )
        with ogr.Open(filepath, update=1) as ds:
          lyr = ds.GetLayer(0)
          lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
        with ogr.Open(filepath) as ds:
          self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 2)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('bbox', data)

  def test_ogr_geojson_48(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_48/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_48.json'
      gdal.FileFromMemBuffer(
          filepath,
          '{ "type": "Feature", "bar": "baz", "bbox": [2,49,2,49],'
          ' "properties": { "myprop": "myvalue" },'
          ' "geometry": {"type": "Point", "coordinates": [ 2, 49]} }',
      )
      with ogr.Open(filepath, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        f.SetField('myprop', 'another_value')
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (3 50)'))
        lyr.SetFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('"bar": "baz"', data)
      self.assertIn('"bbox": [ 3.0, 50.0, 3.0, 50.0 ]', data)
      self.assertNotIn('crs', data)
      self.assertNotIn('FeatureCollection', data)
      self.assertIn('"myprop": "another_value"', data)

  @parameterized.parameters(
      (True, True),
      (True, False),
      (False, True),
      (False, False),
  )
  def test_ogr_geojson_update_feature(
      self,
      check_after_update_before_reopen: bool,
      sync_to_disk_after_update: bool,
  ):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_upd_feat/') as tmpdir:
      filename = f'{tmpdir}/test.json'
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('test')
        lyr.CreateField(ogr.FieldDefn('int64list', ogr.OFTInteger64List))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['int64list'] = [123456790123, -123456790123]
        lyr.CreateFeature(f)

      with ogr.Open(filename, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetFID(0)
        f['int64list'] = [-123456790123, 123456790123]
        lyr.UpdateFeature(f, [0], [], False)
        if sync_to_disk_after_update:
          lyr.SyncToDisk()
        if check_after_update_before_reopen:
          lyr.ResetReading()
          f = lyr.GetNextFeature()
          self.assertEqual(f['int64list'], [-123456790123, 123456790123])

      with ogr.Open(filename) as ds:
        f = ds.GetLayer(0).GetNextFeature()
        self.assertEqual(f['int64list'], [-123456790123, 123456790123])

  def test_ogr_geojson_49(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_49/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_49.json'
      gdal.FileFromMemBuffer(
          filepath,
          '{ "type": "Feature", "properties": { "foo": ["bar"] },'
          ' "geometry": null }',
      )
      with gdal.OpenEx(filepath, open_options=['ARRAY_AS_STRING=YES']) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
        )
        f = lyr.GetNextFeature()
        self.assertEqual(f['foo'], '[ "bar" ]')

  def test_ogr_geojson_50(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_50/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_50.json'
      with self.driver.CreateDataSource(filepath) as ds:
        lyr = ds.CreateLayer('test')
        lyr.CreateField(ogr.FieldDefn('val', ogr.OFTReal))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['val'] = 1.23456789012456
        lyr.CreateFeature(f)
        f = ogr.Feature(lyr.GetLayerDefn())
        f['val'] = 5268.813
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertTrue('1.23456789012456' in data or '5268.813 ' in data)
      gdal.Unlink(filepath)

      with self.driver.CreateDataSource(filepath) as ds:
        lyr = ds.CreateLayer('test', options=['SIGNIFICANT_FIGURES=17'])
        lyr.CreateField(ogr.FieldDefn('val', ogr.OFTReal))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt('POINT (0.0000123456789012456 0)')
        )
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertTrue('1.23456789012456' in data or '-5' in data)
      gdal.Unlink(filepath)

      with self.driver.CreateDataSource(filepath) as ds:
        lyr = ds.CreateLayer(
            'test',
            options=['COORDINATE_PRECISION=15', 'SIGNIFICANT_FIGURES=17'],
        )
        lyr.CreateField(ogr.FieldDefn('val', ogr.OFTReal))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['val'] = 1.23456789012456
        f.SetGeometry(
            ogr.CreateGeometryFromWkt('POINT (0.0000123456789012456 0)')
        )
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('0.00001234', data)
      self.assertIn('1.23456789012456', data)

  def test_ogr_geojson_51(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_51/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_51.json'
      with self.driver.CreateDataSource(filepath) as ds:
        lyr = ds.CreateLayer('test')
        lyr.CreateField(ogr.FieldDefn('id', ogr.OFTInteger))
        wkts = [
            'POINT EMPTY',
            'LINESTRING EMPTY',
            'POLYGON EMPTY',
            'MULTIPOINT EMPTY',
            'MULTILINESTRING EMPTY',
            'MULTIPOLYGON EMPTY',
            'GEOMETRYCOLLECTION EMPTY',
        ]
        for idx, wkt in enumerate(wkts, 1):
          f = ogr.Feature(lyr.GetLayerDefn())
          f['id'] = idx
          f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
          lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn('{ "id": 1 }, "geometry": null', data)
      self.assertIn(
          '{ "id": 2 }, "geometry": { "type": "LineString", "coordinates":'
          ' [ ] } }',
          data,
      )
      self.assertIn(
          '{ "id": 3 }, "geometry": { "type": "Polygon", "coordinates": [ ]'
          ' } }',
          data,
      )
      self.assertIn(
          '{ "id": 4 }, "geometry": { "type": "MultiPoint", "coordinates":'
          ' [ ] } }',
          data,
      )
      self.assertIn(
          '{ "id": 5 }, "geometry": { "type": "MultiLineString",'
          ' "coordinates": [ ] } }',
          data,
      )
      self.assertIn(
          '{ "id": 6 }, "geometry": { "type": "MultiPolygon", "coordinates":'
          ' [ ] } }',
          data,
      )
      self.assertIn(
          '{ "id": 7 }, "geometry": { "type": "GeometryCollection",'
          ' "geometries": [ ] } }',
          data,
      )

      with ogr.Open(filepath) as ds:
        for f in ds.GetLayer(0):
          if f.GetFID() >= 2:
            self.assertTrue(f.GetGeometryRef().IsEmpty())

  def test_ogr_geojson_52(self):
    filepath = ogr_util.GetTestFilePath('geojson/nullvalues.geojson')
    with self.CheckOpen(filepath) as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      lyr = ds.GetLayerByName('nullvalues')
      self.assertIsNotNone(lyr)
      defn = lyr.GetLayerDefn()
      self.assertEqual(defn.GetFieldDefn(0).GetNameRef(), 'int')
      self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTInteger)
      self.assertEqual(defn.GetFieldDefn(1).GetNameRef(), 'string')
      self.assertEqual(defn.GetFieldDefn(1).GetType(), ogr.OFTString)
      self.assertEqual(defn.GetFieldDefn(2).GetNameRef(), 'double')
      self.assertEqual(defn.GetFieldDefn(2).GetType(), ogr.OFTReal)

  def test_ogr_geojson_53(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_53/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_53.json'
      with self.driver.CreateDataSource(filepath) as ds:
        lyr = ds.CreateLayer('test')
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT ZM (1 2 3 4)'))
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filepath, max_size=10000)
      self.assertIn(
          '{ "type": "Point", "coordinates": [ 1.0, 2.0, 3.0 ] }', data
      )

  def test_ogr_geojson_54(self):
    with ogr.Open(
        '{\n'
        '   "type": "FeatureCollection",\n'
        '  "features": [\n'
        '      { "type": "Feature", "properties": { "int": null,'
        ' "string": null, "double": null, "dt" : null, "boolean": null,'
        ' "null": null }, "geometry": null },\n'
        '      { "type": "Feature", "properties": { "int": 168,'
        ' "string": "string", "double": 1.23, "dt" : "2016-05-18T12:34:56Z",'
        ' "boolean": true }, "geometry": null }\n'
        '  ]\n'
        '}\n'
    ) as ds:
      defn = ds.GetLayer(0).GetLayerDefn()
      self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTInteger)
      self.assertEqual(defn.GetFieldDefn(1).GetType(), ogr.OFTString)
      self.assertEqual(defn.GetFieldDefn(2).GetType(), ogr.OFTReal)
      self.assertEqual(defn.GetFieldDefn(3).GetType(), ogr.OFTDateTime)
      self.assertEqual(defn.GetFieldDefn(4).GetType(), ogr.OFTInteger)
      self.assertEqual(defn.GetFieldDefn(4).GetSubType(), ogr.OFSTBoolean)
      self.assertEqual(defn.GetFieldDefn(4).GetWidth(), 1)
      self.assertEqual(defn.GetFieldDefn(5).GetType(), ogr.OFTString)

  def test_ogr_geojson_55(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_55/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      gdal.VectorTranslate(
          out_path,
          '{"type": "FeatureCollection", "features": [{ "type": "Feature",'
          ' "id": 123, "properties": {}, "geometry": { "type": "Point",'
          ' "coordinates": [2.123456789, 49] } },{ "type": "Feature", "id":'
          ' 124, "properties": {}, "geometry": { "type": "Point",'
          ' "coordinates": [3, 50] } }]}',
          options=(
              '-f GeoJSON -lco RFC7946=YES -lco WRITE_BBOX=YES -preserve_fid'
          ),
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      expected = {
          'type': 'FeatureCollection',
          'bbox': [2.1234568, 49.0, 3.0, 50.0],
          'features': [
              {
                  'type': 'Feature',
                  'id': 123,
                  'properties': {},
                  'bbox': [2.1234568, 49.0, 2.1234568, 49.0],
                  'geometry': {
                      'type': 'Point',
                      'coordinates': [2.1234568, 49.0],
                  },
              },
              {
                  'type': 'Feature',
                  'id': 124,
                  'properties': {},
                  'bbox': [3.0, 50.0, 3.0, 50.0],
                  'geometry': {'type': 'Point', 'coordinates': [3.0, 50.0]},
              },
          ],
      }
      self.assertEqual(json.loads(got), expected)
      gdal.Unlink(out_path)

      # Test polygon winding order.
      gdal.VectorTranslate(
          out_path,
          '{"type": "FeatureCollection", "features": ['
          '{ "type": "Feature", "geometry": { "type": "Polygon",'
          ' "coordinates": [[[2,49],[3,49],[3,50],[2,50],[2,49]],'
          '[[2.1,49.1],[2.1,49.9],[2.9,49.9],[2.9,49.1],[2.1,49.1]]] } },'
          '{ "type": "Feature", "geometry": { "type": "Polygon",'
          ' "coordinates": [[[2,49],[2,50],[3,50],[3,49],[2,49]],'
          '[[2.1,49.1],[2.9,49.1],[2.9,49.9],[2.1,49.9],[2.1,49.1]]] } }'
          ']}',
          format='GeoJSON',
          layerCreationOptions=['RFC7946=YES', 'WRITE_BBOX=YES'],
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      ring_outer = [
          [2.0, 49.0],
          [3.0, 49.0],
          [3.0, 50.0],
          [2.0, 50.0],
          [2.0, 49.0],
      ]
      ring_inner = [
          [2.1, 49.1],
          [2.1, 49.9],
          [2.9, 49.9],
          [2.9, 49.1],
          [2.1, 49.1],
      ]
      expected_winding = {
          'type': 'FeatureCollection',
          'bbox': [2.0, 49.0, 3.0, 50.0],
          'features': [
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [2.0, 49.0, 3.0, 50.0],
                  'geometry': {
                      'type': 'Polygon',
                      'coordinates': [ring_outer, ring_inner],
                  },
              },
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [2.0, 49.0, 3.0, 50.0],
                  'geometry': {
                      'type': 'Polygon',
                      'coordinates': [ring_outer, ring_inner],
                  },
              },
          ],
      }
      self.assertEqual(json.loads(got), expected_winding)
      gdal.Unlink(out_path)

      # Test foreign member filtering under RFC7946.
      src_ds = gdal.OpenEx(
          '{\n'
          '"type": "FeatureCollection",\n'
          '"coordinates": "should not be found in output",\n'
          '"geometries": "should not be found in output",\n'
          '"geometry": "should not be found in output",\n'
          '"properties": "should not be found in output",\n'
          '"valid": "should be in output",\n'
          '"crs": "should not be found in output",\n'
          '"bbox": [0,0,0,0],\n'
          '"features": [\n'
          '{ "type": "Feature",\n'
          '  "id": ["not expected as child of features"],\n'
          '  "coordinates": "should not be found in output",\n'
          '  "geometries": "should not be found in output",\n'
          '  "features": "should not be found in output",\n'
          '  "valid": "should be in output",\n'
          '  "properties": { "foo": "bar" },\n'
          '  "geometry": {\n'
          '    "type": "Point",\n'
          '    "bbox": [0,0,0,0],\n'
          '    "geometry": "should not be found in output",\n'
          '    "properties": "should not be found in output",\n'
          '    "features": "should not be found in output",\n'
          '    "valid": "should be in output",\n'
          '    "coordinates": [2,49]\n'
          '  }\n'
          '}\n'
          ']\n'
          '}\n',
          open_options=['NATIVE_DATA=YES'],
      )
      gdal.VectorTranslate(
          out_path,
          src_ds,
          format='GeoJSON',
          layerCreationOptions=['RFC7946=YES'],
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      expected_foreign = {
          'type': 'FeatureCollection',
          'valid': 'should be in output',
          'bbox': [2.0, 49.0, 2.0, 49.0],
          'features': [{
              'type': 'Feature',
              'valid': 'should be in output',
              'properties': {
                  'id': ['not expected as child of features'],
                  'foo': 'bar',
              },
              'geometry': {
                  'type': 'Point',
                  'coordinates': [2.0, 49.0],
                  'valid': 'should be in output',
              },
          }],
      }
      self.assertEqual(json.loads(got), expected_foreign)

  def test_ogr_geojson_56(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_56/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      # Offsetting longitudes beyond antimeridian.
      gdal.VectorTranslate(
          out_path,
          '{"type": "FeatureCollection", "features": ['
          '{ "type": "Feature", "geometry":'
          ' { "type": "Point", "coordinates": [182, 49] } },'
          '{ "type": "Feature", "geometry":'
          ' { "type": "Point", "coordinates": [-183, 50] } },'
          '{ "type": "Feature", "geometry":'
          ' { "type": "LineString", "coordinates": [[-183, 51],[-182, 48]] } },'
          '{ "type": "Feature", "geometry":'
          ' { "type": "LineString", "coordinates": [[182, 52],[183, 47]] } },'
          '{ "type": "Feature", "geometry":'
          ' { "type": "Polygon", "coordinates":'
          ' [[[-183, 51],[-183, 48],[-182, 48],[-183, 48],[-183, 51]]] } },'
          '{ "type": "Feature", "geometry":'
          ' { "type": "Polygon", "coordinates":'
          ' [[[183, 51],[183, 48],[182, 48],[183, 48],[183, 51]]] } }'
          ']}',
          format='GeoJSON',
          layerCreationOptions=['RFC7946=YES', 'WRITE_BBOX=YES'],
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      expected1 = {
          'type': 'FeatureCollection',
          'bbox': [-178.0, 47.0, 178.0, 52.0],
          'features': [
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [-178.0, 49.0, -178.0, 49.0],
                  'geometry': {'type': 'Point', 'coordinates': [-178.0, 49.0]},
              },
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [177.0, 50.0, 177.0, 50.0],
                  'geometry': {'type': 'Point', 'coordinates': [177.0, 50.0]},
              },
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [177.0, 48.0, 178.0, 51.0],
                  'geometry': {
                      'type': 'LineString',
                      'coordinates': [[177.0, 51.0], [178.0, 48.0]],
                  },
              },
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [-178.0, 47.0, -177.0, 52.0],
                  'geometry': {
                      'type': 'LineString',
                      'coordinates': [[-178.0, 52.0], [-177.0, 47.0]],
                  },
              },
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [177.0, 48.0, 178.0, 51.0],
                  'geometry': {
                      'type': 'Polygon',
                      'coordinates': [[
                          [177.0, 51.0],
                          [177.0, 48.0],
                          [178.0, 48.0],
                          [177.0, 48.0],
                          [177.0, 51.0],
                      ]],
                  },
              },
              {
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [-178.0, 48.0, -177.0, 51.0],
                  'geometry': {
                      'type': 'Polygon',
                      'coordinates': [[
                          [-177.0, 51.0],
                          [-177.0, 48.0],
                          [-178.0, 48.0],
                          [-177.0, 48.0],
                          [-177.0, 51.0],
                      ]],
                  },
              },
          ],
      }
      self.assertEqual(json.loads(got), expected1)
      gdal.Unlink(out_path)

      # Geometries across the antimeridian.
      gdal.VectorTranslate(
          out_path,
          '{"type": "FeatureCollection", "features": [{ "type": "Feature",'
          ' "geometry": { "type": "LineString", "coordinates": [[179,'
          ' 51],[-179, 48]] } },{ "type": "Feature", "geometry": { "type":'
          ' "LineString", "coordinates": [[-179, 52],[179, 47]] } },{ "type":'
          ' "Feature", "geometry": { "type": "MultiLineString", "coordinates":'
          ' [ [ [ 179.0, 51.0 ], [ 180.0, 49.5 ] ], [ [ -180.0, 49.5 ], ['
          ' -179.0, 48.0 ] ] ] } },{ "type": "Feature", "geometry": { "type":'
          ' "Polygon", "coordinates": [[[177, 51],[-175, 51],[-175, 48],[177,'
          ' 48],[177, 51]]] } },{ "type": "Feature", "geometry": { "type":'
          ' "MultiPolygon", "coordinates": [ [ [ [ 177.0, 51.0 ], [ 177.0, 48.0'
          ' ], [ 180.0, 48.0 ], [ 180.0, 51.0 ], [ 177.0, 51.0 ] ] ], [ [ ['
          ' -180.0, 51.0 ], [ -180.0, 48.0 ], [ -175.0, 48.0 ], [ -175.0, 51.0'
          ' ], [ -180.0, 51.0 ] ] ] ] } },{ "type": "Feature", "geometry": {'
          ' "type": "MultiPolygon", "coordinates": [ [ [ [ 172, 52 ], [ 173, 52'
          ' ], [ 173, 51 ], [ 172, 51 ], [ 172, 52 ] ] ], [ [ [ -162, 52 ], ['
          ' -163, 52 ], [ -163, 51 ], [ -162, 51 ], [ -162, 52 ] ] ] ] } }]}',
          format='GeoJSON',
          layerCreationOptions=['RFC7946=YES', 'WRITE_BBOX=YES'],
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      j_got = json.loads(got)
      self.assertEqual(j_got['bbox'], [172.0, 47.0, -162.0, 52.0])
      expected_features = [
          (
              [179.0, 48.0, -179.0, 51.0],
              {
                  'type': 'MultiLineString',
                  'coordinates': [
                      [[179.0, 51.0], [180.0, 49.5]],
                      [[-180.0, 49.5], [-179.0, 48.0]],
                  ],
              },
          ),
          (
              [179.0, 47.0, -179.0, 52.0],
              {
                  'type': 'MultiLineString',
                  'coordinates': [
                      [[-179.0, 52.0], [-180.0, 49.5]],
                      [[180.0, 49.5], [179.0, 47.0]],
                  ],
              },
          ),
          (
              [179.0, 48.0, -179.0, 51.0],
              {
                  'type': 'MultiLineString',
                  'coordinates': [
                      [[179.0, 51.0], [180.0, 49.5]],
                      [[-180.0, 49.5], [-179.0, 48.0]],
                  ],
              },
          ),
          (
              [177.0, 48.0, -175.0, 51.0],
              {
                  'type': 'MultiPolygon',
                  'coordinates': [
                      [[
                          [177.0, 51.0],
                          [177.0, 48.0],
                          [180.0, 48.0],
                          [180.0, 51.0],
                          [177.0, 51.0],
                      ]],
                      [[
                          [-180.0, 51.0],
                          [-180.0, 48.0],
                          [-175.0, 48.0],
                          [-175.0, 51.0],
                          [-180.0, 51.0],
                      ]],
                  ],
              },
          ),
          (
              [177.0, 48.0, -175.0, 51.0],
              {
                  'type': 'MultiPolygon',
                  'coordinates': [
                      [[
                          [177.0, 51.0],
                          [177.0, 48.0],
                          [180.0, 48.0],
                          [180.0, 51.0],
                          [177.0, 51.0],
                      ]],
                      [[
                          [-180.0, 51.0],
                          [-180.0, 48.0],
                          [-175.0, 48.0],
                          [-175.0, 51.0],
                          [-180.0, 51.0],
                      ]],
                  ],
              },
          ),
          (
              [172, 51, -162, 52],
              {
                  'type': 'MultiPolygon',
                  'coordinates': [
                      [[[172, 52], [173, 52], [173, 51], [172, 51], [172, 52]]],
                      [[
                          [-162, 52],
                          [-163, 52],
                          [-163, 51],
                          [-162, 51],
                          [-162, 52],
                      ]],
                  ],
              },
          ),
      ]
      for i, (exp_bbox, exp_geom) in enumerate(expected_features):
        self.assertEqual(j_got['features'][i]['bbox'], exp_bbox)
        self._check_topo_geometry(
            ogr.CreateGeometryFromJson(
                json.dumps(j_got['features'][i]['geometry'])
            ),
            ogr.CreateGeometryFromJson(json.dumps(exp_geom)),
        )
      gdal.Unlink(out_path)

      # Geometries that defeat antimeridian heuristics.
      mp_geoms = [
          [
              [[[172, 52], [173, 52], [173, 51], [172, 51], [172, 52]]],
              [[[-162, 52], [-163, 52], [-163, 51], [-162, 51], [-162, 52]]],
              [[[-80, 10], [-60, 10], [-60, 11], [-80, 10]]],
          ],
          [
              [[[172, 52], [173, 52], [173, 51], [172, 51], [172, 52]]],
              [[[-162, 52], [-163, 52], [-163, 51], [-162, 51], [-162, 52]]],
              [[[80, 10], [60, 10], [60, 11], [80, 10]]],
          ],
          [
              [[[172, 52], [173, 52], [173, 51], [172, 51], [172, 52]]],
              [[[-162, 52], [-163, 52], [-163, 51], [-162, 51], [-162, 52]]],
              [[[10, 10], [120, 10], [120, 11], [10, 10]]],
          ],
          [
              [[[172, 52], [173, 52], [173, 51], [172, 51], [172, 52]]],
              [[[-162, 52], [-163, 52], [-163, 51], [-162, 51], [-162, 52]]],
              [[[-91, 10], [-80, 10], [-80, 11], [-91, 10]]],
          ],
      ]
      src_fc = {
          'type': 'FeatureCollection',
          'features': [
              {
                  'type': 'Feature',
                  'geometry': {'type': 'MultiPolygon', 'coordinates': coords},
              }
              for coords in mp_geoms
          ],
      }
      gdal.VectorTranslate(
          out_path,
          json.dumps(src_fc),
          format='GeoJSON',
          layerCreationOptions=['RFC7946=YES', 'WRITE_BBOX=YES'],
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      j_got = json.loads(got)
      self.assertEqual(j_got['bbox'], [-163.0, 10.0, 173.0, 52.0])
      for i, coords in enumerate(mp_geoms):
        self.assertEqual(
            j_got['features'][i]['bbox'], [-163.0, 10.0, 173.0, 52.0]
        )
        self._check_topo_geometry(
            ogr.CreateGeometryFromJson(
                json.dumps(j_got['features'][i]['geometry'])
            ),
            ogr.CreateGeometryFromJson(
                json.dumps({'type': 'MultiPolygon', 'coordinates': coords})
            ),
        )

  def test_ogr_geojson_56_world(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_56_world/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      gdal.VectorTranslate(
          out_path,
          '{"type": "FeatureCollection", "features": ['
          '{ "type": "Feature", "geometry": {"type":"Polygon","coordinates":'
          '[[[-180,-90.0],[180,-90.0],[180,90.0],[-180,90.0],[-180,-90.0]]]} }'
          ']}',
          format='GeoJSON',
          layerCreationOptions=['RFC7946=YES', 'WRITE_BBOX=YES'],
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      expected = {
          'type': 'FeatureCollection',
          'bbox': [-180.0, -90.0, 180.0, 90.0],
          'features': [{
              'type': 'Feature',
              'properties': {},
              'bbox': [-180.0, -90.0, 180.0, 90.0],
              'geometry': {
                  'type': 'Polygon',
                  'coordinates': [[
                      [-180.0, -90.0],
                      [180.0, -90.0],
                      [180.0, 90.0],
                      [-180.0, 90.0],
                      [-180.0, -90.0],
                  ]],
              },
          }],
      }
      self.assertEqual(json.loads(got), expected)

  def test_ogr_geojson_56_next(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_56_next/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      gdal.VectorTranslate(
          out_path,
          '{"type": "FeatureCollection", "features": ['
          '{ "type": "Feature", "geometry": {"type":"Polygon","coordinates":'
          '[[[-180,50],[179.5,50.0],[179.5,40],[-180,45],[-180,50]]]} }'
          ']}',
          format='GeoJSON',
          layerCreationOptions=['RFC7946=YES', 'WRITE_BBOX=YES'],
      )
      got = json.loads(
          gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      )
      expected1 = {
          'type': 'FeatureCollection',
          'bbox': [179.5, 40.0, 180.0, 50.0],
          'features': [{
              'type': 'Feature',
              'properties': {},
              'bbox': [179.5, 40.0, 180.0, 50.0],
              'geometry': {
                  'type': 'Polygon',
                  'coordinates': [[
                      [179.5, 40.0],
                      [180.0, 45.0],
                      [180.0, 50.0],
                      [179.5, 50.0],
                      [179.5, 40.0],
                  ]],
              },
          }],
      }
      expected2 = {
          'type': 'FeatureCollection',
          'bbox': [179.5, 40.0, 180.0, 50.0],
          'features': [{
              'type': 'Feature',
              'properties': {},
              'bbox': [179.5, 40.0, 180.0, 50.0],
              'geometry': {
                  'type': 'Polygon',
                  'coordinates': [[
                      [180.0, 45.0],
                      [180.0, 50.0],
                      [179.5, 50.0],
                      [179.5, 40.0],
                      [180.0, 45.0],
                  ]],
              },
          }],
      }
      self.assertIn(got, (expected1, expected2))
      gdal.Unlink(out_path)

      # WRAPDATELINE=NO
      gdal.VectorTranslate(
          out_path,
          '{"type":"LineString","coordinates":[[179,50],[-179,50]]}',
          format='GeoJSON',
          layerCreationOptions=[
              'RFC7946=YES',
              'WRITE_BBOX=YES',
              'WRAPDATELINE=NO',
          ],
      )
      got = json.loads(
          gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      )
      self.assertEqual(
          got,
          {
              'type': 'FeatureCollection',
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [-179.0, 50.0, 179.0, 50.0],
                  'geometry': {
                      'type': 'LineString',
                      'coordinates': [[179.0, 50.0], [-179.0, 50.0]],
                  },
              }],
              'bbox': [-179.0, 50.0, 179.0, 50.0],
          },
      )
      gdal.Unlink(out_path)

      # Line geometry with one longitude at +/- 180deg (#8645)
      for coords, exp_ml in [
          (
              [[-179, 0], [-180, 0], [179, 0]],
              [[[-179.0, 0.0], [-180.0, 0.0]], [[180.0, 0.0], [179.0, 0.0]]],
          ),
          (
              [[179, 0], [180, 0], [-179, 0]],
              [[[179.0, 0.0], [180.0, 0.0]], [[-180.0, 0.0], [-179.0, 0.0]]],
          ),
      ]:
        gdal.VectorTranslate(
            out_path,
            json.dumps({
                'type': 'FeatureCollection',
                'features': [{
                    'type': 'Feature',
                    'geometry': {'type': 'LineString', 'coordinates': coords},
                }],
            }),
            format='GeoJSON',
            layerCreationOptions=['RFC7946=YES', 'WRITE_BBOX=YES'],
        )
        got = json.loads(
            gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
        )
        self.assertEqual(
            got,
            {
                'type': 'FeatureCollection',
                'bbox': [179.0, 0.0, -179.0, 0.0],
                'features': [{
                    'type': 'Feature',
                    'properties': {},
                    'bbox': [179.0, 0.0, -179.0, 0.0],
                    'geometry': {
                        'type': 'MultiLineString',
                        'coordinates': exp_ml,
                    },
                }],
            },
        )
        gdal.Unlink(out_path)

  def _mem_ds_from_wkts(
      self, proj4_or_wkt: str, wkts: list[str]
  ) -> gdal.Dataset:
    src_ds = gdal.GetDriverByName('MEM').Create('', 0, 0, 0)
    sr = osr.SpatialReference()
    sr.SetFromUserInput(proj4_or_wkt)
    lyr = src_ds.CreateLayer('test', srs=sr)
    for wkt in wkts:
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(f)
    return src_ds

  def test_ogr_geojson_57(self):
    eqc_proj = (
        '+proj=eqc +lat_ts=0 +lat_0=0 +lon_0=0 +x_0=0 +y_0=0 '
        '+datum=WGS84 +units=m +no_defs'
    )
    north_polar_proj = (
        '+proj=stere +lat_0=90 +lat_ts=71 +lon_0=0 +k=1 +x_0=0 +y_0=0 '
        '+datum=WGS84 +units=m +no_defs'
    )
    south_polar_proj = (
        '+proj=stere +lat_0=-90 +lat_ts=-71 +lon_0=0 +k=1 +x_0=0 +y_0=0 '
        '+datum=WGS84 +units=m +no_defs'
    )
    utm60_proj = '+proj=utm +zone=60 +datum=WGS84 +units=m +no_defs'

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_57/') as tmpdir:
      out_path = f'{tmpdir}/out.json'

      def _translate_and_read(src_ds: gdal.Dataset) -> dict[str, Any]:
        gdal.VectorTranslate(
            out_path,
            src_ds,
            format='GeoJSON',
            layerCreationOptions=[
                'WRITE_NAME=NO',
                'RFC7946=YES',
                'WRITE_BBOX=YES',
            ],
        )
        got = gcore_util.vsi_read_file_as_string(out_path, max_size=20000)
        gdal.Unlink(out_path)
        return json.loads(got)

      # Standard case: WGS 84 / Plate Carree.
      src_ds = self._mem_ds_from_wkts(
          eqc_proj,
          [
              'POLYGON((2000000 2000000,2000000 -2000000,-2000000 -2000000,'
              '-2000000 2000000,2000000 2000000))'
          ],
      )
      # Modern PROJ implements ellipsoidal EQC (lat +/-18.0814781 for WGS84);
      # older PROJ used spherical EQC (lat +/-17.9663057).
      self.assertEqual(
          _translate_and_read(src_ds),
          {
              'type': 'FeatureCollection',
              'bbox': [-17.9663057, -18.0814781, 17.9663057, 18.0814781],
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [-17.9663057, -18.0814781, 17.9663057, 18.0814781],
                  'geometry': {
                      'type': 'Polygon',
                      'coordinates': [[
                          [17.9663057, 18.0814781],
                          [-17.9663057, 18.0814781],
                          [-17.9663057, -18.0814781],
                          [17.9663057, -18.0814781],
                          [17.9663057, 18.0814781],
                      ]],
                  },
              }],
          },
      )

      # Arctic Polar Stereographic.
      src_ds = self._mem_ds_from_wkts(
          north_polar_proj,
          [
              (
                  'POLYGON((2000000 2000000,2000000 -2000000,-2000000 -2000000,'
                  '-2000000 2000000,2000000 2000000))'
              ),
              (
                  'POLYGON((-2000000 -2000000,-1000000 -2000000,-1000000'
                  ' 2000000,-2000000 2000000,-2000000 -2000000))'
              ),
          ],
      )
      got = _translate_and_read(src_ds)
      self.assertEqual(got['bbox'], [-180.0, 64.3861643, 180.0, 90.0])
      self._check_topo_geometry(
          ogr.CreateGeometryFromJson(
              json.dumps(got['features'][0]['geometry'])
          ),
          ogr.CreateGeometryFromJson(
              json.dumps({
                  'type': 'Polygon',
                  'coordinates': [[
                      [135.0, 64.3861643],
                      [180.0, 71.7425119],
                      [180.0, 90.0],
                      [-180.0, 90.0],
                      [-180.0, 71.7425119],
                      [-135.0, 64.3861643],
                      [-45.0, 64.3861643],
                      [45.0, 64.3861643],
                      [135.0, 64.3861643],
                  ]],
              })
          ),
      )
      self._check_topo_geometry(
          ogr.CreateGeometryFromJson(
              json.dumps(got['features'][1]['geometry'])
          ),
          ogr.CreateGeometryFromJson(
              json.dumps({
                  'type': 'Polygon',
                  'coordinates': [[
                      [-45.0, 64.3861643],
                      [-26.5650512, 69.6286694],
                      [-153.4349488, 69.6286694],
                      [-135.0, 64.3861643],
                      [-45.0, 64.3861643],
                  ]],
              })
          ),
      )

      # Slice of spherical cap (west hemisphere).
      src_ds = self._mem_ds_from_wkts(
          north_polar_proj,
          [
              'POLYGON((-2000000 2000000,0 0,-2000000 -2000000,-2000000'
              ' 2000000))'
          ],
      )
      self.assertEqual(
          _translate_and_read(src_ds),
          {
              'type': 'FeatureCollection',
              'bbox': [-135.0, 64.3861643, -45.0, 90.0],
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [-135.0, 64.3861643, -45.0, 90.0],
                  'geometry': {
                      'type': 'Polygon',
                      'coordinates': [[
                          [-135.0, 64.3861643],
                          [-45.0, 64.3861643],
                          [-45.0, 90.0],
                          [-135.0, 90.0],
                          [-135.0, 64.3861643],
                      ]],
                  },
              }],
          },
      )

      # Slice of spherical cap (east hemisphere).
      src_ds = self._mem_ds_from_wkts(
          north_polar_proj,
          [
              'MULTIPOLYGON(((2000000 2000000,0 0,2000000 -2000000,2000000'
              ' 2000000)))'
          ],
      )
      self.assertEqual(
          _translate_and_read(src_ds),
          {
              'type': 'FeatureCollection',
              'bbox': [45.0, 64.3861643, 135.0, 90.0],
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [45.0, 64.3861643, 135.0, 90.0],
                  'geometry': {
                      'type': 'MultiPolygon',
                      'coordinates': [[[
                          [135.0, 64.3861643],
                          [135.0, 90.0],
                          [45.0, 90.0],
                          [45.0, 64.3861643],
                          [135.0, 64.3861643],
                      ]]],
                  },
              }],
          },
      )

      # Slice of spherical cap crossing the antimeridian.
      src_ds = self._mem_ds_from_wkts(
          north_polar_proj,
          ['POLYGON((100000 100000,-100000 100000,0 0,100000 100000))'],
      )
      got = _translate_and_read(src_ds)
      exp_cap_variants = [
          {
              'type': 'FeatureCollection',
              'bbox': [135.0, 88.6984598, -135.0, 90.0],
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [135.0, 88.6984598, -135.0, 90.0],
                  'geometry': {
                      'type': 'MultiPolygon',
                      'coordinates': [
                          [[
                              [180.0, 89.0796531],
                              [180.0, 90.0],
                              [135.0, 88.6984598],
                              [180.0, 89.0796531],
                          ]],
                          [[
                              [-180.0, 90.0],
                              [-180.0, 89.0796531],
                              [-135.0, 88.6984598],
                          ]],
                      ],
                  },
              }],
          },
          {
              'type': 'FeatureCollection',
              'bbox': [135.0, 88.6984598, -135.0, 90.0],
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [135.0, 88.6984598, -135.0, 90.0],
                  'geometry': {
                      'type': 'MultiPolygon',
                      'coordinates': [
                          [[
                              [-135.0, 88.6984598],
                              [-180.0, 90.0],
                              [-180.0, 89.0796531],
                              [-135.0, 88.6984598],
                          ]],
                          [[
                              [180.0, 90.0],
                              [135.0, 88.6984598],
                              [180.0, 89.0796531],
                              [180.0, 90.0],
                          ]],
                      ],
                  },
              }],
          },
          {
              'type': 'FeatureCollection',
              'bbox': [135.0, 88.6984598, -135.0, 90.0],
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [135.0, 88.6984598, -135.0, 90.0],
                  'geometry': {
                      'type': 'MultiPolygon',
                      'coordinates': [
                          [[
                              [135.0, 88.6984598],
                              [180.0, 89.0796531],
                              [180.0, 90.0],
                              [135.0, 88.6984598],
                          ]],
                          [[
                              [-135.0, 88.6984598],
                              [-180.0, 90.0],
                              [-180.0, 89.0796531],
                              [-135.0, 88.6984598],
                          ]],
                      ],
                  },
              }],
          },
      ]
      self.assertIn(got, exp_cap_variants)

      # Antarctic Polar Stereographic.
      src_ds = self._mem_ds_from_wkts(
          south_polar_proj,
          [
              'MULTIPOLYGON(((2000000 2000000,2000000 -2000000,'
              '-2000000 -2000000,-2000000 2000000,2000000 2000000)))'
          ],
      )
      got = _translate_and_read(src_ds)
      self.assertEqual(got['bbox'], [-180.0, -90.0, 180.0, -64.3861643])
      self._check_topo_geometry(
          ogr.CreateGeometryFromJson(
              json.dumps(got['features'][0]['geometry'])
          ),
          ogr.CreateGeometryFromJson(
              json.dumps({
                  'type': 'Polygon',
                  'coordinates': [[
                      [45.0, -64.3861643],
                      [-45.0, -64.3861643],
                      [-135.0, -64.3861643],
                      [-180.0, -71.7425119],
                      [-180.0, -90.0],
                      [180.0, -90.0],
                      [180.0, -71.7425119],
                      [135.0, -64.3861643],
                      [45.0, -64.3861643],
                  ]],
              })
          ),
      )

      # UTM zone 60N crossing antimeridian.
      src_ds = self._mem_ds_from_wkts(
          utm60_proj,
          [
              (
                  'POLYGON((670000 4000000,850000 4000000,850000 4100000,'
                  '670000 4100000,670000 4000000))'
              ),
              'MULTILINESTRING((670000 4000000,850000 4100000))',
              'LINESTRING(670000 0,850000 0)',
          ],
      )
      got = _translate_and_read(src_ds)
      self.assertEqual(
          got['bbox'], [178.5275649, 0.0, -179.0681936, 37.0308258]
      )
      expected_utm_geoms = [
          {
              'type': 'MultiPolygon',
              'coordinates': [
                  [[
                      [180.0, 36.1071354],
                      [180.0, 36.1071354],
                      [180.0, 37.0082839],
                      [180.0, 37.0082839],
                      [178.9112998, 37.0308258],
                      [178.8892102, 36.1298163],
                      [180.0, 36.1071354],
                  ]],
                  [[
                      [-180.0, 37.0082839],
                      [-180.0, 36.1071354],
                      [-180.0, 36.1071354],
                      [-179.1135277, 36.0816324],
                      [-179.0681936, 36.9810434],
                      [-180.0, 37.0082839],
                  ]],
              ],
          },
          {
              'type': 'MultiLineString',
              'coordinates': [
                  [[178.8892102, 36.1298163], [180.0, 36.5995612]],
                  [[-180.0, 36.5995612], [-179.0681936, 36.9810434]],
              ],
          },
          {
              'type': 'MultiLineString',
              'coordinates': [
                  [[178.5275649, 0.0], [180.0, 0.0]],
                  [[-180.0, 0.0], [-179.8562277, 0.0]],
              ],
          },
      ]
      for i, exp_geom in enumerate(expected_utm_geoms):
        self._check_topo_geometry(
            ogr.CreateGeometryFromJson(
                json.dumps(got['features'][i]['geometry'])
            ),
            ogr.CreateGeometryFromJson(json.dumps(exp_geom)),
        )

      # Polygon on west of antimeridian in UTM zone 60N.
      src_ds = self._mem_ds_from_wkts(
          utm60_proj,
          [
              'POLYGON((670000 4000000,700000 4000000,700000 4100000,'
              '670000 4100000,670000 4000000))'
          ],
      )
      self.assertEqual(
          _translate_and_read(src_ds),
          {
              'type': 'FeatureCollection',
              'bbox': [178.8892102, 36.1240958, 179.2483693, 37.0308258],
              'features': [{
                  'type': 'Feature',
                  'properties': {},
                  'bbox': [178.8892102, 36.1240958, 179.2483693, 37.0308258],
                  'geometry': {
                      'type': 'Polygon',
                      'coordinates': [[
                          [178.8892102, 36.1298163],
                          [179.2223914, 36.1240958],
                          [179.2483693, 37.0249155],
                          [178.9112998, 37.0308258],
                          [178.8892102, 36.1298163],
                      ]],
                  },
              }],
          },
      )

  def test_ogr_geojson_58(self):
    with ogr.Open(
        '{ "type": "FeatureCollection", "name": "layer_name", "features": []}'
    ) as ds:
      self.assertIsNotNone(ds)
      self.assertIsNotNone(ds.GetLayerByName('layer_name'))

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_58/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_58.json'
      with self.driver.CreateDataSource(filepath) as ds:
        ds.CreateLayer('foo')
      with ogr.Open(filepath) as ds:
        self.assertIsNotNone(ds.GetLayerByName('foo'))

  def test_ogr_geojson_empty_name(self):
    with ogr.Open(
        '{ "type": "FeatureCollection", "name": "", "features": []}'
    ) as ds:
      self.assertEqual(ds.GetLayer(0).GetName(), 'OGRGeoJSON')

  def test_ogr_geojson_59(self):
    with ogr.Open(
        '{ "type": "FeatureCollection", "description": "my_description",'
        ' "features": []}'
    ) as ds:
      self.assertEqual(
          ds.GetLayer(0).GetMetadataItem('DESCRIPTION'), 'my_description'
      )

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_59/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_59.json'
      with self.driver.CreateDataSource(filepath) as ds:
        ds.CreateLayer('foo', options=['DESCRIPTION=my desc'])
      with ogr.Open(filepath) as ds:
        lyr = ds.GetLayerByName('foo')
        self.assertEqual(lyr.GetMetadataItem('DESCRIPTION'), 'my desc')

  def test_ogr_geojson_60(self):
    with gdal.OpenEx(
        '{ "type": "FeatureCollection", "features": [\n'
        '{ "type": "Feature", "properties" : { "foo" : "bar" } },\n'
        '{ "type": "Feature", "properties" : { "foo": null } },\n'
        '{ "type": "Feature", "properties" : {  } } ] }'
    ) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetNextFeature()['foo'], 'bar')
      self.assertTrue(lyr.GetNextFeature().IsFieldNull('foo'))
      self.assertFalse(lyr.GetNextFeature().IsFieldSet('foo'))

      with gcore_util.VsimemCleanupDir('/vsimem/geojson_60/') as tmpdir:
        out_path = f'{tmpdir}/ogr_geojson_60.json'
        gdal.VectorTranslate(out_path, ds, format='GeoJSON')
        data = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
        self.assertIn('"properties": { "foo": "bar" }', data)
        self.assertIn('"properties": { "foo": null }', data)
        self.assertIn('"properties": { }', data)

  def test_ogr_geojson_61(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_61/') as tmpdir:
      filepath = f'{tmpdir}/ogr_geojson_61.json'
      gdal.FileFromMemBuffer(
          filepath, '{ "type": "FeatureCollection", "features": ['
      )
      with _enable_exceptions(), self.assertRaises(Exception):
        gdal.OpenEx(filepath)

      with _enable_exceptions(), self.assertRaises(Exception):
        gdal.OpenEx('{ "type": "Point", "x" : { "coordinates" : null } } ')

      gdal.FileFromMemBuffer(
          filepath,
          '{ "type": "FeatureCollection", "features": [ { "type": "Feature",'
          ' "properties": {"": 1}, "geometry": null }] }',
      )
      with gdal.OpenEx(filepath) as ds:
        f = ds.GetLayer(0).GetNextFeature()
        self.assertEqual(f.GetField(''), 1)

  def test_ogr_geojson_62(self):
    for bad_crs_json in (
        (
            '{ "type": "FeatureCollection", "crs": { "type":"name" },'
            ' "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"name",'
            ' "properties":null }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"name",'
            ' "properties":1 }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"name",'
            ' "properties":{"name":null} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"name",'
            ' "properties":{"name":1} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"name",'
            ' "properties":{"name":"x"} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"EPSG" },'
            ' "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"EPSG",'
            ' "properties":null }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"EPSG",'
            ' "properties":1 }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"EPSG",'
            ' "properties":{"code":null} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"link" },'
            ' "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"link",'
            ' "properties":null }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"link",'
            ' "properties":1 }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"link",'
            ' "properties":{"href":null} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"OGC" },'
            ' "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"OGC",'
            ' "properties":null }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"OGC",'
            ' "properties":1 }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"OGC",'
            ' "properties":{"urn":null} }, "features":[] }'
        ),
    ):
      self.assertIsNotNone(gdal.OpenEx(bad_crs_json))

    for quiet_crs_json in (
        (
            '{ "type": "FeatureCollection", "crs": { "type":"EPSG",'
            ' "properties":{"code":1} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"EPSG",'
            ' "properties":{"code":"x"} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"link",'
            ' "properties":{"href":1} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"link",'
            ' "properties":{"href": "1"} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"OGC",'
            ' "properties":{"urn":1} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"OGC",'
            ' "properties":{"urn":"x"} }, "features":[] }'
        ),
    ):
      with gdal.quiet_errors():
        self.assertIsNotNone(gdal.OpenEx(quiet_crs_json))

    with gdal.OpenEx(
        '{ "type": "FeatureCollection", "crs": { "type":"name", "properties":'
        '{"name": "urn:ogc:def:crs:EPSG::32631"} }, "features":[] }'
    ) as ds:
      srs = ds.GetLayer(0).GetSpatialRef()
      self.assertEqual(srs.GetAuthorityCode(None), '32631')
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [1, 2])

    with gdal.OpenEx(
        '{ "type": "FeatureCollection", "crs": { "type":"name", "properties":'
        '{"name": "urn:ogc:def:crs:OGC:1.3:CRS84"} }, "features":[] }'
    ) as ds:
      srs = ds.GetLayer(0).GetSpatialRef()
      self.assertEqual(srs.GetAuthorityCode(None), '4326')
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [2, 1])

    for crs_32631 in (
        (
            '{ "type": "FeatureCollection", "crs": { "type":"EPSG",'
            ' "properties":{"code": 32631} }, "features":[] }'
        ),
        (
            '{ "type": "FeatureCollection", "crs": { "type":"OGC",'
            ' "properties":{"urn": "urn:ogc:def:crs:EPSG::32631"} },'
            ' "features":[] }'
        ),
    ):
      with gdal.OpenEx(crs_32631) as ds:
        srs = ds.GetLayer(0).GetSpatialRef()
        self.assertIn('32631', srs.ExportToWkt())

  def test_ogr_geojson_63(self):
    ref_path = ogr_util.GetTestFilePath('geojson/test_type_promotion_ref.json')
    test_path = ogr_util.GetTestFilePath('geojson/test_type_promotion.json')
    with ogr.Open(ref_path) as ds_ref, ogr.Open(test_path) as ds:
      self._compare_layers(ds.GetLayer(0), ds_ref.GetLayer(0))

  def test_ogr_geojson_64(self):
    for in_wkt, exp_wkt in [
        ('POINT ZM(1 2 3 4)', 'POINT Z(1 2 3)'),
        ('POINT M(1 2 3)', 'POINT (1 2)'),
        ('LINESTRING ZM(1 2 3 4,5 6 7 8)', 'LINESTRING Z(1 2 3,5 6 7)'),
        ('LINESTRING M(1 2 3,4 5 6)', 'LINESTRING (1 2,4 5)'),
    ]:
      g = ogr.CreateGeometryFromWkt(in_wkt)
      self.CheckFeatureGeometry(
          ogr.CreateGeometryFromJson(g.ExportToJson()),
          ogr.CreateGeometryFromWkt(exp_wkt),
      )

  def test_ogr_geojson_65(self):
    with ogr.Open(
        '{\n'
        '"type": "FeatureCollection",\n'
        '"crs": { "type": "name", "properties":'
        ' { "name": "urn:ogc:def:crs:EPSG::32631" } },\n'
        '"features": [{\n'
        '"type": "Feature",\n'
        '"geometry": {"type": "Point", "coordinates": [500000,4500000]},\n'
        '"properties": {}}]}\n'
    ) as ds:
      f = ds.GetLayer(0).GetNextFeature()
      srs = f.GetGeometryRef().GetSpatialReference()
      self.assertEqual(int(srs.GetAuthorityCode('PROJCS')), 32631)

  def test_ogr_geojson_66(self):
    with ogr.Open(
        '{\n'
        '"type": "FeatureCollection",\n'
        '"features": [\n'
        '{ "type": "Feature", "geometry": null, "properties": null },\n'
        '{ "type": "Feature", "geometry": null, "properties": [] }\n'
        ']}\n'
    ) as ds:
      self.assertEqual(ds.GetLayer(0).GetLayerDefn().GetFieldCount(), 0)

  def test_ogr_geojson_67(self):
    filepath = ogr_util.GetTestFilePath('geojson/grenada.geojson')
    with ogr.Open(filepath) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.GetDriver().GetName(), 'GeoJSON')
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 1)

  def test_ogr_geojson_id_field_and_id_type(self):
    poly_path = ogr_util.GetTestFilePath('shape/poly/poly.shp')
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_id/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      out2_path = f'{tmpdir}/out2.json'

      gdal.VectorTranslate(
          out_path,
          poly_path,
          options=(
              '-f GeoJSON -lco ID_TYPE=String -preserve_fid -limit 1 -fid 2'
          ),
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      self.assertIn(
          '"id": "2", "properties": { "AREA": 261752.781, "EAS_ID": 171,'
          ' "PRFEDEA": "35043414" }',
          got,
      )

      gdal.VectorTranslate(
          out_path,
          poly_path,
          options=(
              '-f GeoJSON -lco ID_TYPE=Integer -preserve_fid -limit 1 -fid 2'
          ),
      )
      got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
      self.assertIn(
          '"id": 2, "properties": { "AREA": 261752.781, "EAS_ID": 171,'
          ' "PRFEDEA": "35043414" }',
          got,
      )

      for lco, expected_snippet in [
          (
              ['ID_FIELD=EAS_ID'],
              (
                  '"id": 168, "properties": { "AREA": 215229.266,'
                  ' "PRFEDEA": "35043411" }'
              ),
          ),
          (
              ['ID_FIELD=EAS_ID', 'ID_TYPE=String'],
              (
                  '"id": "168", "properties": { "AREA": 215229.266,'
                  ' "PRFEDEA": "35043411" }'
              ),
          ),
      ]:
        gdal.VectorTranslate(
            out_path,
            poly_path,
            format='GeoJSON',
            layerCreationOptions=lco,
            limit=1,
        )
        got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
        self.assertIn(expected_snippet, got)

        for reexport_lco, reexport_expected in [
            (None, expected_snippet),
            (
                ['ID_TYPE=String'],
                (
                    '"id": "168", "properties": { "AREA": 215229.266,'
                    ' "PRFEDEA": "35043411" }'
                ),
            ),
            (
                ['ID_TYPE=Integer'],
                (
                    '"id": 168, "properties": { "AREA": 215229.266,'
                    ' "PRFEDEA": "35043411" }'
                ),
            ),
        ]:
          with gdal.OpenEx(out_path, open_options=['NATIVE_DATA=YES']) as src:
            kwargs: dict[str, Any] = {'format': 'GeoJSON'}
            if reexport_lco:
              kwargs['layerCreationOptions'] = reexport_lco
            gdal.VectorTranslate(out2_path, src, **kwargs)
          got2 = gcore_util.vsi_read_file_as_string(out2_path, max_size=10000)
          gdal.Unlink(out2_path)
          self.assertIn(reexport_expected, got2)

      for lco, expected_snippet in [
          (
              ['ID_FIELD=PRFEDEA'],
              (
                  '"id": "35043411", "properties": { "AREA": 215229.266,'
                  ' "EAS_ID": 168 }'
              ),
          ),
          (
              ['ID_FIELD=PRFEDEA', 'ID_TYPE=Integer'],
              (
                  '"id": 35043411, "properties": { "AREA": 215229.266,'
                  ' "EAS_ID": 168 }'
              ),
          ),
          (
              ['ID_GENERATE=YES'],
              (
                  '"id": 0, "properties": { "AREA": 215229.266, "EAS_ID": 168,'
                  ' "PRFEDEA": "35043411" }'
              ),
          ),
          (
              ['ID_GENERATE=YES', 'ID_TYPE=Integer'],
              (
                  '"id": 0, "properties": { "AREA": 215229.266, "EAS_ID": 168,'
                  ' "PRFEDEA": "35043411" }'
              ),
          ),
          (
              ['ID_GENERATE=YES', 'ID_TYPE=String'],
              (
                  '"id": "0", "properties": { "AREA": 215229.266, "EAS_ID":'
                  ' 168, "PRFEDEA": "35043411" }'
              ),
          ),
      ]:
        gdal.VectorTranslate(
            out_path,
            poly_path,
            format='GeoJSON',
            layerCreationOptions=lco,
            limit=1,
        )
        got = gcore_util.vsi_read_file_as_string(out_path, max_size=10000)
        self.assertIn(expected_snippet, got)

  def test_ogr_geojson_geom_export_failure(self):
    g = ogr.CreateGeometryFromWkt('POINT EMPTY')
    self.assertIsNone(g.ExportToJson())

    g = ogr.CreateGeometryFromWkt('GEOMETRYCOLLECTION(TIN EMPTY)')
    with gdal.quiet_errors():
      self.assertEqual(
          json.loads(g.ExportToJson()),
          {'type': 'GeometryCollection', 'geometries': None},
      )

    g = ogr.Geometry(ogr.wkbLineString)
    g.AddPoint_2D(float('nan'), 0)
    with gdal.quiet_errors():
      self.assertIsNone(g.ExportToJson())

    g = ogr.Geometry(ogr.wkbPolygon)
    lr1 = ogr.Geometry(ogr.wkbLinearRing)
    for x, y in [(0, 0), (0, 1), (1, 1), (0, 0)]:
      lr1.AddPoint_2D(x, y)
    g.AddGeometry(lr1)
    lr2 = ogr.Geometry(ogr.wkbLinearRing)
    for x, y in [(0, 0), (float('nan'), 1), (1, 1), (0, 0)]:
      lr2.AddPoint_2D(x, y)
    g.AddGeometry(lr2)
    with gdal.quiet_errors():
      self.assertIsNone(g.ExportToJson())

  def test_ogr_geojson_starting_with_crs(self):
    with ogr.Open(
        '{\n'
        '"crs": { "type": "name", "properties":'
        ' { "name": "urn:ogc:def:crs:EPSG::32631" } },\n'
        '"type": "FeatureCollection",\n'
        '"features": [{\n'
        '"type": "Feature",\n'
        '"geometry": {"type": "Point", "coordinates": [500000,4500000]},\n'
        '"properties": {}}]}\n'
    ) as ds:
      self.assertIsNotNone(ds)

  def test_ogr_geojson_append_flush(self):
    with gcore_util.TestTemporaryDirectory(prefix='geojson_flush') as tmpdir:
      tmpfilename = os.path.join(tmpdir, 'ogr_geojson_append_flush.json')
      with open(tmpfilename, 'w') as f:
        f.write(
            '{\n'
            '"type": "FeatureCollection",\n'
            '"features": [\n'
            '{ "type": "Feature", "properties":'
            ' { "x": 1, "y": 2, "z": 3, "w": 4 },'
            ' "geometry": { "type": "Point", "coordinates": [ 0, 0 ] } } ] }'
        )
      ds = ogr.Open(tmpfilename, update=1)
      lyr = ds.GetLayer(0)
      f = ogr.Feature(lyr.GetLayerDefn())
      f['x'] = 10
      lyr.CreateFeature(f)
      lyr.SyncToDisk()

      ds2 = ogr.Open(tmpfilename, update=1)
      lyr2 = ds2.GetLayer(0)
      lyr2.GetNextFeature()
      f2 = lyr2.GetNextFeature()
      self.assertIsNotNone(f2)
      self.assertEqual(f2['x'], 10)

  def test_ogr_geojson_empty_geometrycollection(self):
    g = ogr.CreateGeometryFromJson(
        '{"type": "GeometryCollection", "geometries": []}'
    )
    self.assertEqual(g.ExportToWkt(), 'GEOMETRYCOLLECTION EMPTY')

  def test_ogr_geojson_read_fields_with_different_case(self):
    with ogr.Open(
        '{\n'
        '"type": "FeatureCollection",\n'
        '"features": [\n'
        '{ "type": "Feature", "id": "my_id", "geometry": null, "properties":\n'
        '{ "ID": "MY_ID", "x": "foo", "X": "FOO"} }\n'
        ']}\n'
    ) as ds:
      f = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(f.GetField(0), 'my_id')
      self.assertEqual(f.GetField(1), 'MY_ID')
      self.assertEqual(f.GetField(2), 'foo')
      self.assertEqual(f.GetField(3), 'FOO')

  def test_ogr_geojson_clip_geometries_rfc7946(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_clip/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      gdal.VectorTranslate(
          out_path,
          '{"type": "FeatureCollection", "features": ['
          '{ "type": "Feature", "geometry": {"type":"Polygon","coordinates":'
          '[[[-220,-20],[-220,30],[16,30],[16,-20],[-220,-20]]]} },'
          '{ "type": "Feature", "geometry": {"type":"Polygon","coordinates":'
          '[[[220,40],[220,70],[-16,70],[-16,40],[220,40]]]} },'
          '{ "type": "Feature", "geometry": {"type":"Polygon","coordinates":'
          '[[[170,-40],[170,-70],[-16,70],[-16,-40],[170,-40]]]} }'
          ']}',
          options='-f GeoJSON -lco RFC7946=YES',
      )
      expected_wkts = [
          (
              'MULTIPOLYGON (((-180 30,-180 -20,16 -20,16 30,-180 30)),'
              '((140 -20,180 -20,180 30,140 30,140 -20)))'
          ),
          (
              'MULTIPOLYGON (((180 40,180 70,-16 70,-16 40,180 40)),'
              '((-180 70,-180 40,-140 40,-140 70,-180 70)))'
          ),
          'POLYGON ((170 -40,-16 -40,-16 70,170 -70,170 -40))',
      ]
      with ogr.Open(out_path) as ds:
        lyr = ds.GetLayer(0)
        for exp_wkt in expected_wkts:
          self._check_topo_geometry(
              lyr.GetNextFeature(), ogr.CreateGeometryFromWkt(exp_wkt)
          )

  def test_ogr_geojson_non_finite(self):
    json_content = (
        '{\n'
        '  "type": "FeatureCollection",\n'
        '  "features": [\n'
        '      { "type": "Feature", "properties": { "inf_prop": infinity,'
        ' "minus_inf_prop": -infinity, "nan_prop": nan },'
        ' "geometry": null }\n'
        '  ]\n'
        '}\n'
    )
    with gdal.quiet_errors():
      ds = ogr.Open(json_content)
    self.assertIsNotNone(ds)
    with ds:
      lyr = ds.GetLayer(0)
      f = lyr.GetNextFeature()
      for i in range(3):
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(i).GetType(), ogr.OFTReal
        )
      self.assertEqual(f['inf_prop'], float('inf'))
      self.assertEqual(f['minus_inf_prop'], float('-inf'))
      self.assertTrue(math.isnan(f['nan_prop']))

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_non_finite/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      with gdal.quiet_errors():
        gdal.VectorTranslate(out_path, json_content, options='-f GeoJSON')
      with ogr.Open(out_path) as ds:
        self.assertEqual(ds.GetLayer(0).GetLayerDefn().GetFieldCount(), 0)
      gdal.Unlink(out_path)

      gdal.VectorTranslate(
          out_path,
          json_content,
          options='-f GeoJSON -lco WRITE_NON_FINITE_VALUES=YES',
      )
      with ogr.Open(out_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
        f = lyr.GetNextFeature()
        self.assertEqual(f['inf_prop'], float('inf'))
        self.assertEqual(f['minus_inf_prop'], float('-inf'))
        self.assertTrue(math.isnan(f['nan_prop']))

  def test_ogr_geojson_json_string_autodetect(self):
    json_content = (
        '{\n'
        '  "type": "FeatureCollection",\n'
        '  "features": [\n'
        '      { "type": "Feature", "properties": { "jsonish": "[5]" },'
        ' "geometry": null }\n'
        '  ]\n'
        '}\n'
    )
    with gdal.quiet_errors(), ogr.Open(json_content) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
      )
      self.assertEqual(lyr.GetNextFeature()['jsonish'], '[5]')

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_autodetect/') as tmpdir:
      out_path = f'{tmpdir}/out.json'
      with gdal.quiet_errors():
        gdal.VectorTranslate(out_path, json_content, options='-f GeoJSON')
      with ogr.Open(out_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTIntegerList
        )
      gdal.Unlink(out_path)

      with gdal.quiet_errors():
        gdal.VectorTranslate(
            out_path,
            json_content,
            options='-f GeoJSON -lco AUTODETECT_JSON_STRINGS=FALSE',
        )
      with ogr.Open(out_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
        )
        self.assertEqual(lyr.GetNextFeature()['jsonish'], '[5]')

  def test_ogr_geojson_random_reading_with_id(self):
    json_content = (
        '{\n'
        '  "type": "FeatureCollection",\n'
        '  "features": [\n'
        '      { "type": "Feature", "id": 1, "properties": { "a": "a" },'
        ' "geometry": null },\n'
        '      { "type": "Feature", "id": 2, "properties": { "a": "bc" },'
        ' "geometry": null }\n'
        '  ]\n'
        '}\n'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_rand_id/') as tmpdir:
      filename = f'{tmpdir}/temp.json'
      gdal.FileFromMemBuffer(filename, json_content)
      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f1_ref = lyr.GetNextFeature()
        f2_ref = lyr.GetNextFeature()
        self.assertTrue(lyr.GetFeature(1).Equal(f1_ref))
        self.assertTrue(lyr.GetFeature(2).Equal(f2_ref))
        self.assertIsNone(lyr.GetFeature(3))

  def test_ogr_geojson_random_reading_without_id(self):
    json_content = (
        '{\n'
        '  "type": "FeatureCollection",\n'
        '  "features": [\n'
        '      { "type": "Feature", "properties": { "a": "a" },'
        ' "geometry": null },\n'
        '      { "type": "Feature", "properties": { "a": "bc" },'
        ' "geometry": null }\n'
        '  ]\n'
        '}\n'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_rand_noid/') as tmpdir:
      filename = f'{tmpdir}/temp.json'
      gdal.FileFromMemBuffer(filename, json_content)
      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f1_ref = lyr.GetNextFeature()
        f2_ref = lyr.GetNextFeature()
        self.assertTrue(lyr.GetFeature(0).Equal(f1_ref))
        self.assertTrue(lyr.GetFeature(1).Equal(f2_ref))
        self.assertIsNone(lyr.GetFeature(2))

  def test_ogr_geojson_single_feature_random_reading_with_id(self):
    json_content = (
        '{ "type": "Feature", "id": 1, "properties": { "a": "a" },'
        ' "geometry": null }\n'
    )
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_single_id/') as tmpdir:
      filename = f'{tmpdir}/temp.json'
      gdal.FileFromMemBuffer(filename, json_content)
      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f1_ref = lyr.GetNextFeature()
        self.assertTrue(lyr.GetFeature(1).Equal(f1_ref))

  def test_ogr_geojson_3d_geom_type(self):
    for coords1, coords2 in [
        ('[1,2,3]', '[1,2,4]'),
        ('[1,2,3]', '[1,2]'),
        ('[1,2]', '[1,2,4]'),
    ]:
      with ogr.Open(
          '{"type": "FeatureCollection", "features":['
          '{"type": "Feature", "geometry": {"type":"Point",'
          f'"coordinates":{coords1}}}, "properties": null}},'
          '{"type": "Feature", "geometry": {"type":"Point",'
          f'"coordinates":{coords2}}}, "properties": null}}'
          ']}'
      ) as ds:
        self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbPoint25D)

  def test_ogr_geojson_update_in_loop(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_upd_loop/') as tmpdir:
      filename = f'{tmpdir}/temp.json'
      cases = [
          (
              (
                  '{"type": "FeatureCollection", "name": "test", "features":'
                  '[{ "type": "Feature", "properties": { "foo": 1 },'
                  ' "geometry": null },'
                  ' { "type": "Feature", "properties": { "foo": 2 },'
                  ' "geometry": null }]}'
              ),
              [0, 1],
          ),
          (
              (
                  '{"type": "FeatureCollection", "name": "test", "features":'
                  '[{ "type": "Feature", "id": 0, "properties": { "foo": 1 },'
                  ' "geometry": null },'
                  ' { "type": "Feature", "properties": { "foo": 2 }, "id": 1,'
                  ' "geometry": null }]}'
              ),
              [0, 1],
          ),
          (
              (
                  '{"type": "FeatureCollection", "name": "test", "features":'
                  '[{ "type": "Feature", "id": 1, "properties": { "foo": 1 },'
                  ' "geometry": null },'
                  ' { "type": "Feature", "properties": { "foo": 2 }, "id": 3,'
                  ' "geometry": null }]}'
              ),
              [1, 3],
          ),
      ]
      for content, expected_fids in cases:
        gdal.FileFromMemBuffer(filename, content)
        with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.GA_Update) as ds:
          layer = ds.GetLayer()
          fids = []
          for feature in layer:
            fids.append(feature.GetFID())
            layer.SetFeature(feature)
          self.assertEqual(fids, expected_fids)

  def test_ogr_geojson_starting_with_coordinates(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_start_coords/') as tmpdir:
      filename = f'{tmpdir}/temp.json'
      gdal.FileFromMemBuffer(
          filename,
          '{ "coordinates": [' + (' ' * 10000) + '2,49], "type": "Point"}',
      )
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertIsNotNone(ds)

  def test_ogr_geojson_starting_with_geometry_coordinates(self):
    with gcore_util.VsimemCleanupDir(
        '/vsimem/geojson_start_geom_coords/'
    ) as tmpdir:
      filename = f'{tmpdir}/temp.json'
      gdal.FileFromMemBuffer(
          filename,
          '{ "geometry": {"coordinates": ['
          + (' ' * 10000)
          + '2,49], "type": "Point"}, "type": "Feature", "properties": {} }',
      )
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertIsNotNone(ds)

  @parameterized.parameters(
      ('{"type":"Point","coordinates":[', '2,49]}'),
      ('{"type":"LineString","coordinates":[[', '2,49],[3,50]]}'),
      ('{"type":"Polygon","coordinates":[[[', '0,0],[0,1],[1,1],[0,0]]]}'),
      ('{"type":"MultiPoint","coordinates":[[', '2,49]]}'),
      ('{"type":"MultiLineString","coordinates":[[[', '2,49],[3,50]]]}'),
      (
          '{"type":"MultiPolygon","coordinates":[[[[',
          '0,0],[0,1],[1,1],[0,0]]]]}',
      ),
      ('{"type":"GeometryCollection","geometries":[', ']}'),
  )
  def test_ogr_geojson_starting_with_geometry_type(self, start: str, end: str):
    with gcore_util.VsimemCleanupDir(
        '/vsimem/geojson_start_geom_type/'
    ) as tmpdir:
      filename = f'{tmpdir}/temp.json'
      gdal.FileFromMemBuffer(
          filename,
          '{ "geometry":'
          + start
          + (' ' * 10000)
          + end
          + ', "type":"Feature","properties":{}}',
      )
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertIsNotNone(ds)

  def test_ogr_geojson_write_float32(self):
    def _cast_as_float(x: float) -> float:
      return struct.unpack('f', struct.pack('f', x))[0]

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_float32/') as tmpdir:
      filename = f'{tmpdir}/test_ogr_geojson_write_float32.json'
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('foo')
        fld_defn = ogr.FieldDefn('float32', ogr.OFTReal)
        fld_defn.SetSubType(ogr.OFSTFloat32)
        lyr.CreateField(fld_defn)

        fld_defn = ogr.FieldDefn('float32list', ogr.OFTRealList)
        fld_defn.SetSubType(ogr.OFSTFloat32)
        lyr.CreateField(fld_defn)

        f = ogr.Feature(lyr.GetLayerDefn())
        f['float32'] = _cast_as_float(0.35)
        f['float32list'] = [
            _cast_as_float(123.0),
            _cast_as_float(0.35),
            _cast_as_float(0.15),
            _cast_as_float(0.12345678),
            _cast_as_float(1.2345678e-15),
            _cast_as_float(1.2345678e15),
            _cast_as_float(0.123456789),
        ]
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filename, max_size=10000)
      data = data.replace('e+0', 'e+').replace('e-0', 'e-')
      self.assertIn('"float32": 0.35,', data)
      self.assertIn(
          '"float32list": [ 123.0, 0.35, 0.15, 0.12345678, 1.2345678e-15,'
          ' 1.2345678e+15, 0.12345679 ]',
          data,
      )

  def test_ogr_geojson_write_float_exponential_without_dot(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_float_exp/') as tmpdir:
      filename = (
          f'{tmpdir}/test_ogr_geojson_write_float_exponential_without_dot.json'
      )
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('foo')
        fld_defn = ogr.FieldDefn('float32', ogr.OFTReal)
        fld_defn.SetSubType(ogr.OFSTFloat32)
        lyr.CreateField(fld_defn)
        lyr.CreateField(ogr.FieldDefn('float64', ogr.OFTReal))

        f = ogr.Feature(lyr.GetLayerDefn())
        f['float32'] = 1e-7
        f['float64'] = 1e-8
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filename, max_size=10000)
      self.assertIsNotNone(json.loads(data))

  def test_ogr_geojson_feature_starting_with_big_properties(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_big_props/') as tmpdir:
      filename = (
          f'{tmpdir}/test_ogr_geojson_feature_starting_with_big_properties.json'
      )
      gdal.FileFromMemBuffer(
          filename,
          '{"properties":{"foo":"%s"},"type":"Feature","geometry":null}'
          % ('x' * 10000),
      )
      with ogr.Open(filename) as ds:
        self.assertIsNotNone(ds)

  def test_ogr_geojson_export_geometry_axis_order(self):
    # EPSG:4326 and lat,long data order.
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    sr.SetAxisMappingStrategy(osr.OAMS_AUTHORITY_COMPLIANT)
    g = ogr.CreateGeometryFromWkt('POINT (49 2)')
    g.AssignSpatialReference(sr)
    before_wkt = g.ExportToWkt()
    self.assertEqual(
        json.loads(g.ExportToJson()),
        {'type': 'Point', 'coordinates': [2.0, 49.0]},
    )
    self.assertEqual(g.ExportToWkt(), before_wkt)

    # EPSG:4326 and long,lat data order.
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    sr.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    g = ogr.CreateGeometryFromWkt('POINT (2 49)')
    g.AssignSpatialReference(sr)
    self.assertEqual(
        json.loads(g.ExportToJson()),
        {'type': 'Point', 'coordinates': [2.0, 49.0]},
    )

    # CRS84 with long,lat CRS and data order.
    sr = osr.SpatialReference()
    sr.SetFromUserInput('OGC:CRS84')
    g = ogr.CreateGeometryFromWkt('POINT (2 49)')
    g.AssignSpatialReference(sr)
    self.assertEqual(
        json.loads(g.ExportToJson()),
        {'type': 'Point', 'coordinates': [2.0, 49.0]},
    )

    # Projected CRS with easting, northing order.
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(32631)
    g = ogr.CreateGeometryFromWkt('POINT (2 49)')
    g.AssignSpatialReference(sr)
    self.assertEqual(
        json.loads(g.ExportToJson()),
        {'type': 'Point', 'coordinates': [2.0, 49.0]},
    )

    # Projected CRS with northing, easting order.
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(2393)
    sr.SetAxisMappingStrategy(osr.OAMS_AUTHORITY_COMPLIANT)
    g = ogr.CreateGeometryFromWkt('POINT (49 2)')
    g.AssignSpatialReference(sr)
    self.assertEqual(
        json.loads(g.ExportToJson()),
        {'type': 'Point', 'coordinates': [2.0, 49.0]},
    )

    # No CRS.
    g = ogr.CreateGeometryFromWkt('POINT (2 49)')
    self.assertEqual(
        json.loads(g.ExportToJson()),
        {'type': 'Point', 'coordinates': [2.0, 49.0]},
    )

  def test_ogr_geojson_sparse_fields(self):
    filepath = ogr_util.GetTestFilePath('geojson/sparse_fields.geojson')
    with ogr.Open(filepath) as ds:
      defn = ds.GetLayer(0).GetLayerDefn()
      field_names = [
          defn.GetFieldDefn(i).GetName() for i in range(defn.GetFieldCount())
      ]
      self.assertEqual(
          field_names,
          ['C', 'B', 'A', 'D', 'E_prev', 'E', 'E_next', 'F', 'X'],
      )

  @parameterized.parameters(
      'point.geojson',
      'geojson/featurecollection_point.json',
  )
  def test_ogr_geojson_crs_4326(self, relpath: str):
    with ogr.Open(ogr_util.GetTestFilePath(relpath)) as ds:
      srs = ds.GetLayer(0).GetSpatialRef()
      self.assertEqual(srs.GetAuthorityCode(None), '4326')
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [2, 1])

  @parameterized.parameters(
      'geojson/pointz.json',
      'geojson/featurecollection_pointz.json',
  )
  def test_ogr_geojson_crs_4979(self, relpath: str):
    with ogr.Open(ogr_util.GetTestFilePath(relpath)) as ds:
      srs = ds.GetLayer(0).GetSpatialRef()
      self.assertEqual(srs.GetAuthorityCode(None), '4979')
      self.assertEqual(srs.GetDataAxisToSRSAxisMapping(), [2, 1, 3])

  def test_ogr_geojson_write_rfc7946_from_3d_crs(self):
    srs_4979 = osr.SpatialReference()
    srs_4979.ImportFromEPSG(4979)
    srs_4979.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    srs_4326_5773 = osr.SpatialReference()
    srs_4326_5773.SetFromUserInput('EPSG:4326+5773')
    srs_4326_5773.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    ct = osr.CoordinateTransformation(srs_4979, srs_4326_5773)
    lon, lat, z = ct.TransformPoint(2, 49, 100)

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_3d_crs/') as tmpdir:
      filename = f'{tmpdir}/out.geojson'
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('out', srs=srs_4326_5773, options=['RFC7946=YES'])
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt(f'POINT({lon:.17g} {lat:.17g} {z:.17g})')
        )
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filename, max_size=10000)
      self.assertIn('"coordinates": [ 2.0, 49.0, 100.0', data)

  def test_ogr_geojson_feature_large(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_large/') as tmpdir:
      filename = f'{tmpdir}/test_ogr_geojson_feature_large.json'
      coords = ','.join(['[0,0]' for _ in range(10 * 1024)])
      gdal.FileFromMemBuffer(
          filename,
          '{"type":"FeatureCollection","features":[{"type":"Feature",'
          '"properties":{},"geometry":{"type":"LineString",'
          f'"coordinates":[{coords}]}}}}]}}',
      )
      self.assertIsNotNone(ogr.Open(filename))
      with gdaltest.config_option('OGR_GEOJSON_MAX_OBJ_SIZE', '0'):
        self.assertIsNotNone(ogr.Open(filename))
      with (
          gdaltest.config_option('OGR_GEOJSON_MAX_OBJ_SIZE', '0.1'),
          gdal.quiet_errors(),
      ):
        self.assertIsNone(ogr.Open(filename))

  # No vsicurl/webserver in google3:
  # - test_ogr_geojson_read_from_http
  # - test_ogr_geojson_read_from_http_with_GDAL_HTTP_HEADERS
  # - test_ogr_geojson_read_from_http_with_GDAL_HTTP_HEADERS_Accept
  # - test_ogr_geojson_read_from_http_with_GDAL_HTTP_HEADERS_overriding_Accept

  def test_ogr_geojson_ogr2ogr_nln_with_input_dataset_having_name(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_nln/') as tmpdir:
      filename = f'{tmpdir}/test_ogr_geojson_feature_large.geojson'
      gdal.VectorTranslate(
          filename,
          '{"type":"FeatureCollection","name":"to_be_overriden","features":[]}',
          layerName='newName',
      )
      with ogr.Open(filename) as ds:
        self.assertEqual(ds.GetLayer(0).GetName(), 'newName')

  @parameterized.parameters(True, False)
  def test_ogr_geojson_ids_0_1_null_unset(self, read_from_file: bool):
    connection_name = ogr_util.GetTestFilePath(
        'geojson/ids_0_1_null_unset.json'
    )
    if not read_from_file:
      with open(connection_name, 'rb') as f:
        connection_name = f.read().decode('ascii')
    with ogr.Open(connection_name) as ds:
      self.assertIsNotNone(ds)
      lyr = ds.GetLayer(0)
      f = lyr.GetNextFeature()
      self.assertEqual(f.GetFID(), 0)
      self.assertEqual(f['id'], 0)
      self.assertEqual(f['seq'], 0)
      f = lyr.GetNextFeature()
      self.assertEqual(f.GetFID(), 1)
      self.assertEqual(f['id'], 1)
      self.assertEqual(f['seq'], 1)
      f = lyr.GetNextFeature()
      self.assertEqual(f.GetFID(), 2)
      self.assertIsNone(f['id'])
      self.assertEqual(f['seq'], 2)
      f = lyr.GetNextFeature()
      self.assertEqual(f.GetFID(), 3)
      self.assertFalse(f.IsFieldSet('id'))
      self.assertEqual(f['seq'], 3)
      self.assertIsNone(lyr.GetNextFeature())

      for i in range(4):
        f = lyr.GetFeature(i)
        self.assertEqual(f.GetFID(), i)
        self.assertEqual(f['seq'], i)

  @parameterized.parameters(True, False)
  def test_ogr_geojson_ids_0_1_null_1_null(self, read_from_file: bool):
    connection_name = ogr_util.GetTestFilePath(
        'geojson/ids_0_1_null_1_null.json'
    )
    if not read_from_file:
      with open(connection_name, 'rb') as f:
        connection_name = f.read().decode('ascii')
    with gdal.quiet_errors():
      gdal.ErrorReset()
      with ogr.Open(connection_name) as ds:
        self.assertIsNotNone(ds)
        if not read_from_file:
          self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetFID(), 0)
        self.assertEqual(f['id'], 0)
        self.assertEqual(f['seq'], 0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetFID(), 1)
        self.assertEqual(f['id'], 1)
        self.assertEqual(f['seq'], 1)
        f = lyr.GetNextFeature()
        if read_from_file:
          self.assertEqual(gdal.GetLastErrorType(), gdal.CE_Warning)
        self.assertEqual(f.GetFID(), 2)
        self.assertIsNone(f['id'])
        self.assertEqual(f['seq'], 2)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetFID(), 3)
        self.assertEqual(f['id'], 1)
        self.assertEqual(f['seq'], 3)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetFID(), 4)
        self.assertIsNone(f['id'])
        self.assertEqual(f['seq'], 4)
        self.assertIsNone(lyr.GetNextFeature())

        gdal.ErrorReset()
        for i in range(5):
          f = lyr.GetFeature(i)
          self.assertEqual(f.GetFID(), i)
          self.assertEqual(f['seq'], i)
        self.assertEqual(gdal.GetLastErrorType(), gdal.CE_None)

  # No test_ogrsf binary in google3 - test_ogr_geojson_test_ogrsf
  # No test_ogrsf binary in google3 - test_ogr_geojson_test_ogrsf_update

  @parameterized.parameters(
      (['a_string', 42.0, {'a_field': 'a_value'}],),
      (['a_string', 42, {'a_field': 'a_value'}],),
      (['a_string', {'a_field': 'a_value'}, 42],),
      ([42, 'a_string', {'a_field': 'a_value'}],),
      ([42, {'a_field': 'a_value'}, 'a_string'],),
      ([{'a_field': 'a_value'}, 42, 'a_string'],),
      ([{'a_field': 'a_value'}, 'a_string', 42],),
  )
  def test_ogr_geojson_mixed_type_promotion(self, properties: list[object]):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_mixed/') as tmpdir:
      filename = f'{tmpdir}/temp.json'
      jdata = {
          'type': 'FeatureCollection',
          'features': [
              {'type': 'Feature', 'properties': {'prop0': prop_val}}
              for prop_val in properties
          ],
      }
      gdal.FileFromMemBuffer(filename, json.dumps(jdata))
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertIsNotNone(ds)
        fld_def = ds.GetLayer(0).GetLayerDefn().GetFieldDefn(0)
        self.assertEqual(fld_def.GetTypeName(), 'String')
        self.assertEqual(fld_def.GetSubType(), ogr.OFSTJSON)

  def test_ogr_geojson_coordinate_precision(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_coord_prec/') as tmpdir:
      filename = f'{tmpdir}/test_ogr_geojson_coordinate_precision.json'
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(
            'foo', options=['COORDINATE_PRECISION=1', 'WRITE_BBOX=YES']
        )
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1.23456789 2.3456789)'))
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_string(filename, max_size=10000)
      self.assertIn('"bbox": [ 1.2, 2.3, 1.2, 2.3 ]', data)
      self.assertIn('"coordinates": [ 1.2, 2.3 ]', data)
      self.assertNotIn('3456', data)

      with ogr.Open(filename) as ds:
        prec = (
            ds.GetLayer(0)
            .GetLayerDefn()
            .GetGeomFieldDefn(0)
            .GetCoordinatePrecision()
        )
        self.assertEqual(prec.GetXYResolution(), 1e-1)

  def test_ogr_geojson_field_types(self):
    test_data = (
        '{"type":"FeatureCollection","name":"My Collection","features":['
        '{ "type": "Feature", "properties": { "prop0": 42 },'
        ' "geometry": { "type": "Point", "coordinates": [ 102.0, 0.5 ] } },'
        '{ "type": "Feature", "properties": { "prop0": "42" },'
        ' "geometry": { "type": "Point", "coordinates": [ 102.0, 0.5 ] } },'
        '{ "type": "Feature", "properties": { "prop0": "astring" },'
        ' "geometry": { "type": "Point", "coordinates": [ 102.0, 0.5 ] } },'
        '{ "type": "Feature", "properties": { "prop0": { "nested": 75 } },'
        ' "geometry": { "type": "Point", "coordinates": [ 102.0, 0.5 ] } },'
        '{ "type": "Feature", "properties": { "prop0": { "a": "b" } },'
        ' "geometry": { "type": "Point", "coordinates": [ 102.0, 0.5 ] } }'
        ']}'
    )
    with (
        gcore_util.VsimemCleanupDir('/vsimem/geojson_field_types/') as tmpdir,
        gdal.OpenEx(
            test_data, gdal.OF_VECTOR, open_options=['NATIVE_DATA=TRUE']
        ) as srcds,
    ):
      filename = f'{tmpdir}/test_ogr_geojson_field_types.json'
      gdal.VectorTranslate(
          filename, srcds, options='-f GeoJSON -lco NATIVE_DATA=TRUE'
      )
      data = gcore_util.vsi_read_file_as_string(filename, max_size=10000)
      self.assertIn('{ "prop0": "42" }', data)
      self.assertIn('{ "prop0": "astring" }', data)
      self.assertIn('{ "prop0": { "nested": 75 } }', data)
      self.assertIn('{ "prop0": 42 }', data)
      self.assertIn('{ "prop0": { "a": "b" } }', data)

  def test_ogr_geojson_open_with_non_c_locale(self):
    original_locale = locale.setlocale(locale.LC_ALL)
    try:
      try:
        locale.setlocale(locale.LC_ALL, '')
      except locale.Error:
        self.skipTest('cannot set default locale')
      if locale.localeconv()['decimal_point'] == '.':
        self.skipTest('cannot test, as decimal_point is dot')
      self.test02Point()
    finally:
      locale.setlocale(locale.LC_ALL, original_locale)

  def test_ogr_geojson_write_geometry_validity_fixing_rfc7946(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_fix_rfc7946/') as tmpdir:
      filename = f'{tmpdir}/valid_rfc7946.geojson'
      with self.driver.CreateDataSource(filename) as ds:
        srs = osr.SpatialReference()
        srs.ImportFromEPSG(32632)
        lyr = ds.CreateLayer('foo', srs=srs, options=['RFC7946=YES'])
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'CURVEPOLYGON (COMPOUNDCURVE ((318049.787'
                ' 5688446.432,318056.628 5688447.908),CIRCULARSTRING'
                ' (318056.628 5688447.908,318055.204 5688448.31,318054.25'
                ' 5688449.44),(318054.25 5688449.44,318049.487'
                ' 5688471.421,318049.381 5688471.91,318046.45'
                ' 5688471.278,318051.318 5688448.807),CIRCULARSTRING'
                ' (318051.318 5688448.807,318051.039 5688447.306,318049.787'
                ' 5688446.432)))'
            )
        )
        lyr.CreateFeature(f)

      with ogr.Open(filename) as ds:
        f = ds.GetLayer(0).GetNextFeature()
        self.assertTrue(f.GetGeometryRef().IsValid())
        self.assertIn(
            '((6.3889058 51.3181847,', f.GetGeometryRef().ExportToWkt()
        )

  def test_ogr_geojson_write_geometry_validity_fixing(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_fix_geom/') as tmpdir:
      filename = f'{tmpdir}/valid.geojson'
      with self.driver.CreateDataSource(filename) as ds:
        srs = osr.SpatialReference()
        srs.ImportFromEPSG(4326)
        lyr = ds.CreateLayer('foo', srs=srs, options=['COORDINATE_PRECISION=7'])
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'CURVEPOLYGON ((6.38889863954163 51.3181827925179,'
                '6.38899594962816 51.3181982385094,'
                '6.38899341741945 51.3181982023144,'
                '6.38899088736512 51.3181982771017,'
                '6.3889883718375 51.3181984625056,'
                '6.3889858831369 51.3181987576155,'
                '6.38898343343252 51.3181991609928,'
                '6.38898103470357 51.3181996706617,'
                '6.38897869867934 51.3182002841327,'
                '6.38897643678313 51.3182009984033,'
                '6.38897426007491 51.3182018099849,'
                '6.38897217919885 51.3182027149033,'
                '6.38897020433018 51.3182037087387,'
                '6.38896834512585 51.3182047866288,'
                '6.38896661067703 51.3182059433019,'
                '6.38896500946527 51.3182071731038,'
                '6.3889635493198 51.3182084700195,'
                '6.38896223738132 51.3182098277102,'
                '6.38896108006473 51.3182112395339,'
                '6.38888158690724 51.3184071643413,'
                '6.3888798178826 51.3184115229661,'
                '6.38883812537253 51.3184049086286,'
                '6.38891937389822 51.3182046159251,'
                '6.38891977479799 51.3182034182797,'
                '6.38892004565215 51.3182022064019,'
                '6.38892018522285 51.318200985823,'
                '6.38892019287242 51.3181997621242,'
                '6.38892006856587 51.3181985408984,'
                '6.38891981287105 51.3181973277249,'
                '6.38891942695666 51.3181961281515,'
                '6.38891891258675 51.3181949476556,'
                '6.38891827211159 51.3181937916355,'
                '6.38891750845846 51.3181926653737,'
                '6.38891662511696 51.3181915740195,'
                '6.38891562612488 51.3181905225562,'
                '6.3889145160472 51.3181895157905,'
                '6.38891329995705 51.3181885583257,'
                '6.38891198341239 51.318187654536,'
                '6.38891057242995 51.3181868085505,'
                '6.38890907345788 51.3181860242377,'
                '6.388907493347 51.3181853051815,'
                '6.38890583931839 51.3181846546672,'
                '6.38890411893121 51.3181840756684,'
                '6.38890234004799 51.3181835708295,'
                '6.38890051079796 51.3181831424602,'
                '6.38889863954163 51.3181827925179))'
            )
        )
        lyr.CreateFeature(f)

      with ogr.Open(filename) as ds:
        f = ds.GetLayer(0).GetNextFeature()
        self.assertTrue(f.GetGeometryRef().IsValid())

  def test_ogr_geojson_arrow_stream_mixed_timezone(self):
    # Omit GetArrowStreamAsPyArrow checks since pyarrow is not a dependency in
    # google3; verify WriteArrow C Data Interface path.
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_arrow_mix/') as tmpdir:
      filename = f'{tmpdir}/mixed_tz.geojson'
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
        lyr.CreateField(ogr.FieldDefn('datetime', ogr.OFTDateTime))
        for dt in (
            None,
            '2022-05-31T12:34:56.789Z',
            None,
            '2022-05-31T12:34:56.789+01:00',
            '2022-05-31T12:34:56.789-01:00',
        ):
          f = ogr.Feature(lyr.GetLayerDefn())
          if dt is not None:
            f.SetField('datetime', dt)
          lyr.CreateFeature(f)

      with (
          ogr.Open(filename) as ds,
          ogr.GetDriverByName('MEM').CreateDataSource('') as mem_ds,
      ):
        mem_lyr = mem_ds.CreateLayer('test', geom_type=ogr.wkbPoint)
        mem_lyr.WriteArrow(ds.GetLayer(0))
        self.assertEqual(mem_lyr.GetFeatureCount(), 5)
        self.assertIsNone(mem_lyr.GetNextFeature()['datetime'])
        self.assertEqual(
            mem_lyr.GetNextFeature()['datetime'], '2022/05/31 12:34:56.789+00'
        )
        self.assertIsNone(mem_lyr.GetNextFeature()['datetime'])
        self.assertEqual(
            mem_lyr.GetNextFeature()['datetime'], '2022/05/31 11:34:56.789+00'
        )
        self.assertEqual(
            mem_lyr.GetNextFeature()['datetime'], '2022/05/31 13:34:56.789+00'
        )

  @parameterized.parameters(
      (
          '2022-05-31T12:34:56.789+05:00',
          '2022-05-31T13:34:56.789+05:00',
          '2022/05/31 12:34:56.789+05',
      ),
      (
          '2022-05-31T12:34:56.789-05:00',
          '2022-05-31T13:34:56.789-05:00',
          '2022/05/31 12:34:56.789-05',
      ),
      (
          '2022-05-31T12:34:56.789Z',
          '2022-05-31T13:34:56.789',
          '2022/05/31 12:34:56.789',
      ),
  )
  def test_ogr_geojson_arrow_stream_timezone_write_arrow(
      self, dt1: str, dt2: str, expected_first: str
  ):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_arrow_tz/') as tmpdir:
      filename = f'{tmpdir}/tz.geojson'
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
        lyr.CreateField(ogr.FieldDefn('datetime', ogr.OFTDateTime))
        for dt in (dt1, dt2):
          f = ogr.Feature(lyr.GetLayerDefn())
          f.SetField('datetime', dt)
          lyr.CreateFeature(f)

      with (
          ogr.Open(filename) as ds,
          ogr.GetDriverByName('MEM').CreateDataSource('') as mem_ds,
      ):
        mem_lyr = mem_ds.CreateLayer('test', geom_type=ogr.wkbPoint)
        mem_lyr.WriteArrow(ds.GetLayer(0))
        f = mem_lyr.GetNextFeature()
        self.assertEqual(f['datetime'], expected_first)

  def test_ogr_geojson_foreign_members_collection(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_fm_coll/') as tmpdir:
      filename = f'{tmpdir}/test_ogr_geojson_foreign_members_collection.geojson'
      with self.driver.CreateDataSource(filename) as ds:
        brace_err_re = (
            r'Value of FOREIGN_MEMBERS_COLLECTION should start with \{'
            r' and end with \}'
        )
        for opt, err_re in [
            ('FOREIGN_MEMBERS_COLLECTION=invalid', brace_err_re),
            ('FOREIGN_MEMBERS_COLLECTION={invalid', brace_err_re),
            (
                'FOREIGN_MEMBERS_COLLECTION={invalid}',
                'Value of FOREIGN_MEMBERS_COLLECTION is invalid JSON',
            ),
        ]:
          with (
              _enable_exceptions(),
              self.assertRaisesRegex(Exception, err_re),
          ):
            ds.CreateLayer('test', geom_type=ogr.wkbNone, options=[opt])

        ds.CreateLayer(
            'test',
            geom_type=ogr.wkbNone,
            options=['FOREIGN_MEMBERS_COLLECTION={"foo":"bar"}'],
        )

      data = gcore_util.vsi_read_file_as_string(filename, max_size=10000)
      self.assertIn(
          '{\n"type": "FeatureCollection",\n"foo":"bar",\n"name": "test",',
          data,
      )

  def test_ogr_geojson_foreign_members_feature(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_fm_feat/') as tmpdir:
      filename = f'{tmpdir}/test_ogr_geojson_foreign_members_feature.geojson'
      with self.driver.CreateDataSource(filename) as ds:
        brace_err_re = (
            r'Value of FOREIGN_MEMBERS_FEATURE should start with \{'
            r' and end with \}'
        )
        for opt, err_re in [
            ('FOREIGN_MEMBERS_FEATURE=invalid', brace_err_re),
            ('FOREIGN_MEMBERS_FEATURE={invalid', brace_err_re),
            (
                'FOREIGN_MEMBERS_FEATURE={invalid}',
                'Value of FOREIGN_MEMBERS_FEATURE is invalid JSON',
            ),
        ]:
          with (
              _enable_exceptions(),
              self.assertRaisesRegex(Exception, err_re),
          ):
            ds.CreateLayer('test', geom_type=ogr.wkbNone, options=[opt])

        lyr = ds.CreateLayer(
            'test',
            geom_type=ogr.wkbNone,
            options=['FOREIGN_MEMBERS_FEATURE={"foo":"bar"}'],
        )
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

      data = gcore_util.vsi_read_file_as_string(filename, max_size=10000)
      self.assertIn(
          '{\n"type": "FeatureCollection",\n"name": "test",\n"features": [\n'
          '{ "type": "Feature", "properties": { }, "geometry": null,'
          ' "foo":"bar"}\n]\n}',
          data,
      )

  def test_ogr_json_getextent3d(self):
    jdata = (
        '{\n'
        '  "type": "FeatureCollection",\n'
        '  "features": [\n'
        '    {"type": "Feature", "properties": {"foo": "bar"},'
        ' "geometry": {"type": "%s", "coordinates": %s}},\n'
        '    {"type": "Feature", "properties": {"foo": "baz"},'
        ' "geometry": {"type": "%s", "coordinates": %s}}\n'
        '  ]\n'
        '}'
    )
    inf = float('inf')
    ninf = float('-inf')

    with gcore_util.VsimemCleanupDir('/vsimem/geojson_ext3d/') as tmpdir:
      filename = f'{tmpdir}/test.json'

      # 3D points.
      gdal.FileFromMemBuffer(
          filename, jdata % ('Point', '[1, 1, 1]', 'Point', '[2, 2, 2]')
      )
      gdal.ErrorReset()
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)
        self.assertEqual(lyr.GetExtent(), (1.0, 2.0, 1.0, 2.0))
        self.assertEqual(lyr.GetExtent3D(), (1.0, 2.0, 1.0, 2.0, 1.0, 2.0))

      # 2D points + attribute & spatial filters.
      gdal.FileFromMemBuffer(
          filename, jdata % ('Point', '[1, 1]', 'Point', '[2, 2]')
      )
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        lyr = ds.GetLayer(0)
        self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent3D))
        self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent))
        self.assertEqual(lyr.GetExtent(), (1.0, 2.0, 1.0, 2.0))
        self.assertEqual(lyr.GetExtent3D(), (1.0, 2.0, 1.0, 2.0, inf, ninf))

        lyr.SetAttributeFilter("foo = 'baz'")
        self.assertFalse(lyr.TestCapability(ogr.OLCFastGetExtent3D))
        self.assertFalse(lyr.TestCapability(ogr.OLCFastGetExtent))
        self.assertEqual(lyr.GetExtent(), (2.0, 2.0, 2.0, 2.0))
        self.assertEqual(lyr.GetExtent3D(), (2.0, 2.0, 2.0, 2.0, inf, ninf))

        lyr.SetAttributeFilter(None)
        self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent3D))
        self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent))
        self.assertEqual(lyr.GetExtent(), (1.0, 2.0, 1.0, 2.0))
        self.assertEqual(lyr.GetExtent3D(), (1.0, 2.0, 1.0, 2.0, inf, ninf))

        lyr.SetSpatialFilterRect(1.5, 1.5, 2.5, 2.5)
        self.assertFalse(lyr.TestCapability(ogr.OLCFastGetExtent3D))
        self.assertFalse(lyr.TestCapability(ogr.OLCFastGetExtent))
        self.assertEqual(lyr.GetExtent(), (2.0, 2.0, 2.0, 2.0))
        self.assertEqual(lyr.GetExtent3D(), (2.0, 2.0, 2.0, 2.0, inf, ninf))

      # Mixed 2D/3D points.
      gdal.FileFromMemBuffer(
          filename, jdata % ('Point', '[1, 1, 1]', 'Point', '[2, 2]')
      )
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetExtent(), (1.0, 2.0, 1.0, 2.0))
        self.assertEqual(lyr.GetExtent3D(), (1.0, 2.0, 1.0, 2.0, 1.0, 1.0))

      # Mixed geometry types.
      gdal.FileFromMemBuffer(
          filename,
          jdata
          % ('Point', '[1, 1, 1]', 'LineString', '[[2, 2, 2], [3, 3, 3]]'),
      )
      with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
        self.assertEqual(gdal.GetLastErrorMsg(), '')
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetGeomFieldDefn(0).GetType(), ogr.wkbUnknown
        )

      # Polygon, polygon with hole, mixed 2D series.
      for json_doc, exp_ext, exp_ext3d in [
          (
              (
                  '{"type": "FeatureCollection", "features": [{"type":'
                  ' "Feature", "properties": {}, "geometry": {"type":'
                  ' "Polygon", "coordinates": [[[1, 1], [2, 1], [2, 2], [1, 2],'
                  ' [1, 1]]]}}]}'
              ),
              (1.0, 2.0, 1.0, 2.0),
              (1.0, 2.0, 1.0, 2.0, inf, ninf),
          ),
          (
              (
                  '{"type": "FeatureCollection", "features": [{"type":'
                  ' "Feature", "properties": {}, "geometry": {"type":'
                  ' "Polygon", "coordinates": [[[1, 1], [2, 1], [2, 2], [1, 2],'
                  ' [1, 1]], [[1.5, 1.5], [1.5, 1.6], [1.6, 1.6], [1.6, 1.5],'
                  ' [1.5, 1.5]]]}}]}'
              ),
              (1.0, 2.0, 1.0, 2.0),
              (1.0, 2.0, 1.0, 2.0, inf, ninf),
          ),
          (
              (
                  '{"type": "FeatureCollection", "features": ['
                  '{"type": "Feature", "properties": {}, "geometry":'
                  ' {"type": "Point", "coordinates": [1, 1]}},'
                  '{"type": "Feature", "properties": {}, "geometry":'
                  ' {"type": "LineString", "coordinates": [[2, 2], [3, 3]]}},'
                  '{"type": "Feature", "properties": {}, "geometry":'
                  ' {"type": "Polygon", "coordinates":'
                  ' [[[4, 4], [5, 4], [5, 5], [4, 5], [4, 4]]]}},'
                  '{"type": "Feature", "properties": {}, "geometry":'
                  ' {"type": "Polygon", "coordinates":'
                  ' [[[6, 6], [7, 6], [7, 7], [6, 7], [6, 6]],'
                  ' [[6.5, 6.5], [6.5, 6.6], [6.6, 6.6], [6.6, 6.5],'
                  ' [6.5, 6.5]]]}}]}'
              ),
              (1.0, 7.0, 1.0, 7.0),
              (1.0, 7.0, 1.0, 7.0, inf, ninf),
          ),
          (
              (
                  '{"type": "FeatureCollection", "features": [{"type":'
                  ' "Feature", "properties": {}, "geometry": {"type": "Point",'
                  ' "coordinates": [1, 1, 1]}},{"type": "Feature",'
                  ' "properties": {}, "geometry": {"type": "LineString",'
                  ' "coordinates": [[2, 2, 2], [3, 3, 3]]}},{"type": "Feature",'
                  ' "properties": {}, "geometry": {"type": "Polygon",'
                  ' "coordinates": [[[4, 4, 4], [5, 4, 4], [5, 5, 5], [4, 5,'
                  ' 5], [4, 4, 4]]]}},{"type": "Feature", "properties": {},'
                  ' "geometry": {"type": "Polygon", "coordinates": [[[6, 6, 6],'
                  ' [7, 6, 6], [7, 7, 7], [6, 7, 7], [6, 6, 6]], [[6.5, 6.5,'
                  ' 6.5], [6.5, 6.6, 6.5], [6.6, 6.6, 6.5], [6.6, 6.5, 6.5],'
                  ' [6.5, 6.5, 6.5]]]}}]}'
              ),
              (1.0, 7.0, 1.0, 7.0),
              (1.0, 7.0, 1.0, 7.0, 1.0, 7.0),
          ),
          (
              (
                  '{"type": "FeatureCollection", "features": [{"type":'
                  ' "Feature", "properties": {}, "geometry": {"type":'
                  ' "GeometryCollection", "geometries": [{"type": "Point",'
                  ' "coordinates": [6, 7]}, {"type": "Polygon", "coordinates":'
                  ' [[[3, 4, 2], [5, 4, 4], [5, 5, 5], [4, 5, 5], [3, 4,'
                  ' 2]]]}]}}]}'
              ),
              (3.0, 6.0, 4.0, 7.0),
              (3.0, 6.0, 4.0, 7.0, 2.0, 5.0),
          ),
      ]:
        gdal.FileFromMemBuffer(filename, json_doc)
        with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          lyr = ds.GetLayer(0)
          self.assertEqual(lyr.GetExtent(), exp_ext)
          self.assertEqual(lyr.GetExtent3D(), exp_ext3d)

  def test_ogr_geojson_geom_coord_precision(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_gprec/') as tmpdir:
      filename = f'{tmpdir}/test.geojson'
      with self.driver.Create(filename, 0, 0, 0, gdal.GDT_Unknown) as ds:
        geom_fld = ogr.GeomFieldDefn('geometry', ogr.wkbUnknown)
        prec = ogr.CreateGeomCoordinatePrecision()
        prec.Set(1e-5, 1e-3, 0)
        geom_fld.SetCoordinatePrecision(prec)
        lyr = ds.CreateLayerFromGeomFieldDefn('test', geom_fld)
        prec = lyr.GetLayerDefn().GetGeomFieldDefn(0).GetCoordinatePrecision()
        self.assertEqual(prec.GetXYResolution(), 1e-5)
        self.assertEqual(prec.GetZResolution(), 1e-3)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt('POINT(1.23456789 2.34567891 9.87654321)')
        )
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_bytes(filename, max_size=10000)
      self.assertIn(b'"xy_coordinate_resolution"', data)
      self.assertIn(b'"z_coordinate_resolution"', data)
      self.assertIn(b'"coordinates": [ 1.23457, 2.34568, 9.877 ]', data)

      # Append feature.
      with ogr.Open(filename, update=1) as ds:
        lyr = ds.GetLayer(0)
        prec = lyr.GetLayerDefn().GetGeomFieldDefn(0).GetCoordinatePrecision()
        self.assertEqual(prec.GetXYResolution(), 1e-5)
        self.assertEqual(prec.GetZResolution(), 1e-3)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt('POINT(2.23456789 3.34567891 8.87654321)')
        )
        lyr.CreateFeature(f)

      data = gcore_util.vsi_read_file_as_bytes(filename, max_size=10000)
      self.assertIn(b'"coordinates": [ 1.23457, 2.34568, 9.877 ]', data)
      self.assertIn(b'"coordinates": [ 2.23457, 3.34568, 8.877 ]', data)

      # Modify existing feature.
      with ogr.Open(filename, update=1) as ds:
        lyr = ds.GetLayer(0)
        prec = lyr.GetLayerDefn().GetGeomFieldDefn(0).GetCoordinatePrecision()
        self.assertEqual(prec.GetXYResolution(), 1e-5)
        self.assertEqual(prec.GetZResolution(), 1e-3)
        f = lyr.GetNextFeature()
        f.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'POINT(-2.23456789 -3.34567891 -8.87654321)'
            )
        )
        lyr.SetFeature(f)

      data = gcore_util.vsi_read_file_as_bytes(filename, max_size=10000)
      self.assertIn(b'"coordinates": [ -2.23457, -3.34568, -8.877 ]', data)
      self.assertIn(b'"coordinates": [ 2.23457, 3.34568, 8.877 ]', data)

      with ogr.Open(filename) as ds:
        prec = (
            ds.GetLayer(0)
            .GetLayerDefn()
            .GetGeomFieldDefn(0)
            .GetCoordinatePrecision()
        )
        self.assertEqual(prec.GetXYResolution(), 1e-5)
        self.assertEqual(prec.GetZResolution(), 1e-3)

  def test_ogr_geojson_geom_coord_precision_rfc7946(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_gprec_rfc/') as tmpdir:
      filename = f'{tmpdir}/test.geojson'
      with self.driver.Create(filename, 0, 0, 0, gdal.GDT_Unknown) as ds:
        geom_fld = ogr.GeomFieldDefn('geometry', ogr.wkbUnknown)
        prec = ogr.CreateGeomCoordinatePrecision()
        prec.Set(1e-3, 1e-3, 0)
        geom_fld.SetCoordinatePrecision(prec)
        srs = osr.SpatialReference()
        srs.SetFromUserInput('EPSG:32631+3855')
        geom_fld.SetSpatialRef(srs)
        lyr = ds.CreateLayerFromGeomFieldDefn('test', geom_fld, ['RFC7946=YES'])
        prec = lyr.GetLayerDefn().GetGeomFieldDefn(0).GetCoordinatePrecision()
        self.assertAlmostEqual(prec.GetXYResolution(), 8.983152841195214e-09)
        self.assertEqual(prec.GetZResolution(), 1e-3)

      with ogr.Open(filename) as ds:
        prec = (
            ds.GetLayer(0)
            .GetLayerDefn()
            .GetGeomFieldDefn(0)
            .GetCoordinatePrecision()
        )
        self.assertAlmostEqual(prec.GetXYResolution(), 8.983152841195214e-09)
        self.assertEqual(prec.GetZResolution(), 1e-3)

  def test_ogr_geojson_open_with_feature_type_non_jsonfg(self):
    filepath = ogr_util.GetTestFilePath('geojson/featuretype.json')
    with gdal.OpenEx(filepath) as ds:
      self.assertEqual(ds.GetDriver().GetDescription(), 'GeoJSON')

  def test_ogr_geojson_open_jsonfg_with_geojson(self):
    filepath = ogr_util.GetTestFilePath('jsonfg/crs_none.json')
    with gdal.OpenEx(filepath, allowed_drivers=['GeoJSON']) as ds:
      self.assertEqual(ds.GetDriver().GetDescription(), 'GeoJSON')

    if gdal.GetDriverByName('JSONFG'):
      with gdal.OpenEx(filepath) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), 'JSONFG')
      with gdal.OpenEx(filepath, allowed_drivers=['GeoJSON', 'JSONFG']) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), 'JSONFG')

  def test_ogr_geojson_identify_jsonfg_with_geojson(self):
    filepath = ogr_util.GetTestFilePath('jsonfg/crs_none.json')
    drv = gdal.IdentifyDriverEx(filepath, allowed_drivers=['GeoJSON'])
    self.assertEqual(drv.GetDescription(), 'GeoJSON')

    if gdal.GetDriverByName('JSONFG'):
      drv = gdal.IdentifyDriverEx(filepath)
      self.assertEqual(drv.GetDescription(), 'JSONFG')
      drv = gdal.IdentifyDriverEx(
          filepath, allowed_drivers=['GeoJSON', 'JSONFG']
      )
      self.assertEqual(drv.GetDescription(), 'JSONFG')

  def test_ogr_geojson_feature_with_type_topology_property(self):
    filepath = ogr_util.GetTestFilePath(
        'geojson/feature_with_type_Topology_property.json'
    )
    with gdal.OpenEx(filepath) as ds:
      self.assertEqual(ds.GetDriver().GetDescription(), 'GeoJSON')

  def test_ogr_geojson_force_opening(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_force/') as tmpdir:
      filename = f'{tmpdir}/test.json'
      gdal.FileFromMemBuffer(
          filename,
          b'{'
          + b' ' * (1000 * 1000)
          + b' "type": "FeatureCollection", "features":[]}',
      )
      with _enable_exceptions(), self.assertRaises(Exception):
        gdal.OpenEx(filename)

      with gdal.OpenEx(filename, allowed_drivers=['GeoJSON']) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), 'GeoJSON')

      drv = gdal.IdentifyDriverEx(
          'http://example.com', allowed_drivers=['GeoJSON']
      )
      self.assertEqual(drv.GetDescription(), 'GeoJSON')

  def test_ogr_geojson_force_opening_stacta(self):
    filepath = ogr_util.GetTestFilePath('stacta/test.json')
    if gdal.GetDriverByName('STACTA'):
      with gdal.OpenEx(filepath) as ds:
        self.assertEqual(ds.GetDriver().GetDescription(), 'STACTA')

    with gdal.OpenEx(filepath, allowed_drivers=['GeoJSON']) as ds:
      self.assertEqual(ds.GetDriver().GetDescription(), 'GeoJSON')

  @parameterized.parameters(
      (
          [],
          [
              ogr.OFTString,
              ogr.OFTInteger,
              ogr.OFTReal,
              ogr.OFTInteger,
              ogr.OFTString,
              ogr.OFTString,
              (ogr.OFTString, ogr.OFSTNone),
              ogr.OFTString,
          ],
          [],
          None,
      ),
      (
          [
              'OGR_SCHEMA={"layers": [{"name": "test_point", "fields":'
              ' [{ "name": "str", "type": "Integer" }]}]}'
          ],
          [
              ogr.OFTInteger,
              ogr.OFTInteger,
              ogr.OFTReal,
              ogr.OFTInteger,
              ogr.OFTString,
              ogr.OFTString,
              ogr.OFTString,
              ogr.OFTString,
          ],
          [],
          None,
      ),
      (
          [
              'OGR_SCHEMA={ "layers": [{"name": "test_point",'
              ' "schemaType": "Full", "fields": [{ "name": "json_str",'
              ' "subType": "JSON", "newName": "json_str" },'
              ' {"name": "uuid_str", "subType": "UUID" }]}]}'
          ],
          [
              (ogr.OFTString, ogr.OFSTJSON),
              (ogr.OFTString, ogr.OFSTUUID),
          ],
          ['json_str'],
          None,
      ),
      (
          [
              'OGR_SCHEMA={ "layers": [{"name": "test_point", "fields":'
              ' [{ "name": "real", "width": 7, "precision": 3 }]}]}'
          ],
          [
              ogr.OFTString,
              ogr.OFTInteger,
              ogr.OFTReal,
              ogr.OFTInteger,
              ogr.OFTString,
              ogr.OFTString,
              (ogr.OFTString, ogr.OFSTNone),
              ogr.OFTString,
          ],
          [],
          None,
      ),
      (
          [
              'OGR_SCHEMA={ "layers": [{"name": "test_point", "fields":'
              ' [{ "name": "int", "subType": "Boolean" }, { "name": "real",'
              ' "type": "Integer", "subType": "Int16" }]}]}'
          ],
          [
              ogr.OFTString,
              (ogr.OFTInteger, ogr.OFSTBoolean),
              (ogr.OFTInteger, ogr.OFSTInt16),
              ogr.OFTInteger,
              ogr.OFTString,
              ogr.OFTString,
              ogr.OFTString,
              ogr.OFTString,
          ],
          [],
          None,
      ),
      (
          [
              'OGR_SCHEMA={ "layers": [{"name": "test_point", "fields":'
              ' [{ "name": "int_str", "type": "Integer" },'
              ' { "name": "real_str", "type": "Real" }]}]}'
          ],
          [
              ogr.OFTString,
              ogr.OFTInteger,
              ogr.OFTReal,
              ogr.OFTInteger,
              ogr.OFTInteger,
              ogr.OFTReal,
              ogr.OFTString,
              ogr.OFTString,
          ],
          [],
          None,
      ),
      (
          [
              'OGR_SCHEMA={ "layers": [{"name": "test_point", "fields":'
              ' [{ "name": "str", "type": "xxxxx" }]}]}'
          ],
          [],
          [],
          'Unsupported field type: xxxxx for field str',
      ),
      (
          [
              'OGR_SCHEMA={ "layers": [{"name": "test_point", "fields":'
              ' [{ "name": "xxxxx", "type": "String",'
              ' "newName": "new_str" }]}]}'
          ],
          [],
          [],
          'Field xxxxx not found',
      ),
      (
          [
              'OGR_SCHEMA={ "layers": [{"name": "xxxxx", "fields":'
              ' [{ "name": "str", "type": "String" }]}]}'
          ],
          [],
          [],
          'Layer xxxxx not found',
      ),
  )
  def test_ogr_geojson_schema_override(
      self,
      open_options: list[str],
      expected_field_types: list[object],
      expected_field_names: list[str],
      expected_warning: str | None,
  ):
    json_data = {
        'type': 'FeatureCollection',
        'features': [{
            'type': 'Feature',
            'properties': {
                'str': '1',
                'int': 2,
                'real': 3.4,
                'bool': 1,
                'int_str': '2',
                'real_str': '3.4',
                'json_str': '{"foo": 1}',
                'uuid_str': '123e4567-e89b-12d3-a456-426614174000',
            },
            'geometry': {'type': 'Point', 'coordinates': [1, 2]},
        }],
    }
    with gcore_util.TestTemporaryDirectory(prefix='geojson_schema') as tmpdir:
      json_file = os.path.join(tmpdir, 'test_point.json')
      with open(json_file, 'w') as f:
        json.dump(json_data, f)

      gdal.ErrorReset()
      with gdal.quiet_errors():
        if not expected_field_types:
          ds = gdal.OpenEx(
              json_file,
              gdal.OF_VECTOR | gdal.OF_READONLY,
              open_options=open_options,
              allowed_drivers=['GeoJSON'],
          )
          self.assertIsNotNone(expected_warning)
          self.assertIn(expected_warning, gdal.GetLastErrorMsg())
          self.assertIsNone(ds)
          return

        with gdal.OpenEx(
            json_file,
            gdal.OF_VECTOR | gdal.OF_READONLY,
            open_options=open_options,
            allowed_drivers=['GeoJSON'],
        ) as ds:
          self.assertIsNotNone(ds)
          lyr = ds.GetLayer(0)
          self.assertEqual(lyr.GetFeatureCount(), 1)
          self.assertLen(
              expected_field_types, lyr.GetLayerDefn().GetFieldCount()
          )

          names = expected_field_names or [
              'str',
              'int',
              'real',
              'bool',
              'int_str',
              'real_str',
              'json_str',
              'uuid_str',
          ]
          feat = lyr.GetNextFeature()
          for i, name in enumerate(names):
            expected_spec = expected_field_types[i]
            if isinstance(expected_spec, tuple):
              expected_type, expected_subtype = expected_spec
              self.assertEqual(
                  feat.GetFieldDefnRef(i).GetSubType(), expected_subtype
              )
            else:
              expected_type = expected_spec
            self.assertEqual(feat.GetFieldDefnRef(i).GetType(), expected_type)
            self.assertEqual(feat.GetFieldDefnRef(i).GetName(), name)

          if open_options and 'precision' in open_options[0]:
            self.assertEqual(feat.GetFieldDefnRef(2).GetWidth(), 7)
            self.assertEqual(feat.GetFieldDefnRef(2).GetPrecision(), 3)

          if 'int' in names:
            int_sub_type = feat.GetFieldDefnRef('int').GetSubType()
            expected_int = 1 if int_sub_type == ogr.OFSTBoolean else 2
            self.assertEqual(feat.GetFieldAsInteger('int'), expected_int)
          if 'str' in names:
            self.assertEqual(feat.GetFieldAsString('str'), '1')
          if 'real_str' in names:
            self.assertEqual(feat.GetFieldAsDouble('real_str'), 3.4)
          if 'int_str' in names:
            self.assertEqual(feat.GetFieldAsInteger('int_str'), 2)

  @parameterized.parameters(None, 'AUTO', 'ALL', 'NONE', 'STAC')
  def test_ogr_geojson_foreign_members(
      self, foreign_members_option: str | None
  ):
    open_options = {}
    if foreign_members_option:
      open_options['FOREIGN_MEMBERS'] = foreign_members_option
    filepath = ogr_util.GetTestFilePath('geojson/stac_item.json')
    with gdal.OpenEx(filepath, gdal.OF_VECTOR, open_options=open_options) as ds:
      lyr = ds.GetLayer(0)
      f = lyr.GetNextFeature()
      if foreign_members_option is None or foreign_members_option in (
          'AUTO',
          'STAC',
      ):
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 39)
        self.assertEqual(f['stac_version'], '1.0.0')
        self.assertEqual(f['assets.thumbnail.title'], 'Thumbnail image')
        self.assertEqual(
            json.loads(f['assets.thumbnail.alternate']),
            {
                's3': {
                    'storage:platform': 'AWS',
                    'storage:requester_pays': True,
                    'href': 's3://example/thumb_small.jpeg',
                }
            },
        )
      elif foreign_members_option == 'ALL':
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 35)
        self.assertEqual(f['stac_version'], '1.0.0')
        self.assertNotEqual(f['assets'], '')
      else:
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 29)

  @parameterized.parameters(
      ({'type': 'Point'},),
      ({'type': 'Point', 'coordinates': None},),
      ({'type': 'Point', 'coordinates': 'invalid'},),
      ({'type': 'Point', 'coordinates': []},),
      ({'type': 'Point', 'coordinates': [1]},),
      ({'type': 'Point', 'coordinates': [None, 2]},),
      ({'type': 'Point', 'coordinates': ['invalid', 2]},),
      ({'type': 'MultiPoint'},),
      ({'type': 'MultiPoint', 'coordinates': None},),
      ({'type': 'MultiPoint', 'coordinates': 'invalid'},),
      ({'type': 'MultiPoint', 'coordinates': ['invalid']},),
      ({'type': 'MultiPoint', 'coordinates': [['invalid', 2]]},),
      ({'type': 'LineString'},),
      ({'type': 'LineString', 'coordinates': None},),
      ({'type': 'LineString', 'coordinates': 'invalid'},),
      ({'type': 'LineString', 'coordinates': ['invalid']},),
      ({'type': 'LineString', 'coordinates': [['invalid', 2]]},),
      ({'type': 'MultiLineString'},),
      ({'type': 'MultiLineString', 'coordinates': None},),
      ({'type': 'MultiLineString', 'coordinates': 'invalid'},),
      ({'type': 'MultiLineString', 'coordinates': ['invalid']},),
      ({'type': 'MultiLineString', 'coordinates': [['invalid']]},),
      ({'type': 'MultiLineString', 'coordinates': [[['invalid', 2]]]},),
      ({'type': 'Polygon'},),
      ({'type': 'Polygon', 'coordinates': None},),
      ({'type': 'Polygon', 'coordinates': 'invalid'},),
      ({'type': 'Polygon', 'coordinates': ['invalid']},),
      ({'type': 'Polygon', 'coordinates': [['invalid']]},),
      ({'type': 'Polygon', 'coordinates': [[['invalid', 2]]]},),
      ({'type': 'MultiPolygon'},),
      ({'type': 'MultiPolygon', 'coordinates': None},),
      ({'type': 'MultiPolygon', 'coordinates': 'invalid'},),
      ({'type': 'MultiPolygon', 'coordinates': ['invalid']},),
      ({'type': 'MultiPolygon', 'coordinates': [['invalid']]},),
      ({'type': 'MultiPolygon', 'coordinates': [[['invalid']]]},),
      ({'type': 'MultiPolygon', 'coordinates': [[[['invalid', 2]]]]},),
  )
  def test_ogr_geojson_invalid_geoms(self, geojson_obj: dict[str, object]):
    with gdal.quiet_errors():
      gdal.ErrorReset()
      ogr.Open(json.dumps(geojson_obj))
      self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_geojson_sqlite_dialect_id_property(self):
    if ogr.GetDriverByName('SQLite') is None:
      self.skipTest('SQLite driver missing')

    j = {
        'type': 'FeatureCollection',
        'name': 'test',
        'features': [{
            'type': 'Feature',
            'properties': {'id': 5, 'foo': 'bar'},
            'geometry': None,
        }],
    }
    with ogr.Open(json.dumps(j)) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFIDColumn(), 'id')
      self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'id')

      for sql in (
          'SELECT * FROM test',
          'SELECT * FROM test WHERE id = 5',
          'SELECT * FROM test WHERE ROWID = 5',
      ):
        with ds.ExecuteSQL(sql, dialect='SQLite') as sql_lyr:
          self.assertEqual(sql_lyr.GetFeatureCount(), 1)
          f = sql_lyr.GetNextFeature()
          self.assertEqual(f['id'], 5)
          self.assertEqual(f['foo'], 'bar')

  def test_ogr_geojson_invalid_number(self):
    with gcore_util.VsimemCleanupDir('/vsimem/geojson_bad_num/') as tmpdir:
      filename = f'{tmpdir}/out.json'
      gdal.FileFromMemBuffer(
          filename,
          '{"type": "FeatureCollection", "features": ['
          '{"type": "Feature", "properties":'
          ' {"foo": 12345678123456781234567812345678}, "geometry": null}]}',
      )
      with ogr.Open(filename) as ds:
        f = ds.GetLayer(0).GetNextFeature()
        self.assertEqual(f['foo'], float(12345678123456781234567812345678))

      for content, err_msg in [
          (
              (
                  '{"type": "FeatureCollection", "features": [{"type":'
                  ' "Feature", "properties": {"foo":'
                  ' 123456781234567-81234567812345678}, "geometry": null}]}'
              ),
              'Unrecognized number: 123456781234567-81234567812345678',
          ),
          (
              (
                  '{"type": "FeatureCollection", "features": [{"type":'
                  ' "Feature", "properties": {"foo":'
                  ' 12345678.1234567-81234567812345678}, "geometry": null}]}'
              ),
              'Unrecognized number: 12345678.1234567-81234567812345678',
          ),
          (
              '{"":{"":"","features":[{"":4a},0]}}',
              'Unrecognized number: 4a',
          ),
      ]:
        gdal.FileFromMemBuffer(filename, content)
        with (
            _enable_exceptions(),
            self.assertRaisesRegex(Exception, err_msg),
        ):
          ogr.Open(filename)

  def test_ogr_geojson_export_to_json_geom_collection_with_curve(self):
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION('
        'GEOMETRYCOLLECTION(CIRCULARSTRING EMPTY),POINT (0 0))'
    )
    self.assertEqual(
        json.loads(g.ExportToJson()),
        {
            'type': 'GeometryCollection',
            'geometries': [
                {
                    'type': 'GeometryCollection',
                    'geometries': [{'type': 'LineString', 'coordinates': []}],
                },
                {'type': 'Point', 'coordinates': [0.0, 0.0]},
            ],
        },
    )


if __name__ == '__main__':
  googletest.main()
