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
# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2014, Even Rouault <even dot rouault at mines-paris . org>
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

"""Test OGR handling of ESRI Shapefiles.

This is a rewrite of:

  http://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_shape.py.
"""

import os


from osgeo import ogr
import unittest
from autotest2.gcore import gcore_util
from autotest2.ogr import ogr_util


DRIVER = ogr_util.SHAPEFILE_DRIVER
EXT = '.shp'


def setUpModule():
  ogr_util.SetupTestEnv()


def HaveGeos():
  point1 = ogr.CreateGeometryFromWkt('POINT(10 20)')
  point2 = ogr.CreateGeometryFromWkt('POINT(30 20)')
  return point1.Union(point2) is not None


@ogr_util.SkipIfDriverMissing(DRIVER)
class OgrShapefileTest(ogr_util.DriverTestCase):

  def setUp(self):
    super(OgrShapefileTest, self).setUp(DRIVER, EXT)

  def testReadPoint(self):
    filepath = ogr_util.GetTestFilePath('shape/point/point.shp')
    self.CheckOpen(filepath)
    self.assertEqual(self.src.GetLayerCount(), 1)

    layer = self.src.GetLayer()
    self.assertEqual(layer.GetName(), 'point')
    self.assertEqual(layer.GetFeatureCount(), 1)
    self.assertEqual(layer.GetExtent(), (1.0, 1.0, 2.0, 2.0))

    layer_defn = layer.GetLayerDefn()
    self.assertEqual(layer_defn.GetFieldCount(), 1)
    self.assertEqual(layer_defn.GetGeomType(), ogr.wkbPoint)

    field_defn = layer_defn.GetFieldDefn(0)
    self.assertEqual(field_defn.GetName(), 'FID')
    self.assertEqual(field_defn.GetTypeName(), 'Integer64')

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField(0), 0.0)
    self.assertEqual(feature.GetFieldCount(), 1)
    self.assertEqual(feature.GetGeomFieldCount(), 1)
    self.assertEqual(feature.GetGeometryRef().ExportToWkt(), 'POINT (1 2)')

    geometry_ref = feature.GetGeometryRef()
    self.assertEqual(geometry_ref.GetPoint(), (1.0, 2.0, 0.0))
    self.assertEqual(geometry_ref.GetPoint_2D(), (1.0, 2.0))
    self.assertIsNone(geometry_ref.GetSpatialReference())

  # These initial tests are overly complicated.
  # TODO(schwehr): Test 01.
  # TODO(schwehr): Test 02.
  # TODO(schwehr): Test 03.
  # TODO(schwehr): Test 04.
  # TODO(schwehr): Test 05.
  # TODO(schwehr): Test 06.
  # TODO(schwehr): Test 07.
  # TODO(schwehr): Test 08.

  def test09SearchInsidePolyReturnNone(self):
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()

    layer.SetSpatialFilterRect(-10, -130, 10, -110)
    if HaveGeos():
      self.assertEqual(layer.GetFeatureCount(), 0)
    else:
      self.assertEqual(layer.GetFeatureCount(), 1)

  def test10SelectSomePolygonsByRegion(self):
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()
    layer.SetSpatialFilterRect(-400, 22, -120, 400)

    index = layer.GetLayerDefn().GetFieldIndex('FID')
    fids = [feature.GetField(index) for feature in ogr_util.Features(layer)]

    self.assertEqual(fids, [0, 4, 8])

  def test11SelectAreaAndFidReturnNone(self):
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()

    layer.SetAttributeFilter('FID = 5')
    layer.SetSpatialFilterRect(-400, 22, -120, 400)
    index = layer.GetLayerDefn().GetFieldIndex('FID')
    fids = [feature.GetField(index) for feature in ogr_util.Features(layer)]

    self.assertFalse(fids)

  def test11SelectAreaAndFidReturnsOne(self):
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()

    layer.SetAttributeFilter('FID = 4')
    layer.SetSpatialFilterRect(-400, 22, -120, 400)
    index = layer.GetLayerDefn().GetFieldIndex('FID')
    fids = [feature.GetField(index) for feature in ogr_util.Features(layer)]

    self.assertEqual(fids, [4])

  def test12Multipolygon(self):
    filepath = ogr_util.GetTestFilePath('shape/multipolygon/american-samoa.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()
    feature = layer.GetNextFeature()
    geometry = feature.GetGeometryRef()
    self.assertEqual(geometry.GetCoordinateDimension(), 2)
    self.assertEqual(geometry.GetGeometryName(), 'MULTIPOLYGON')
    self.assertEqual(geometry.GetGeometryCount(), 5)

    point_counts = [15, 11, 17, 20, 9]
    for geom_index in range(5):
      poly = geometry.GetGeometryRef(geom_index)
      self.assertEqual(poly.GetGeometryName(), 'POLYGON')
      self.assertEqual(poly.GetGeometryCount(), 1)
      self.assertEqual(poly.GetGeometryRef(0).GetPointCount(),
                       point_counts[geom_index])

  def test13SetFeature(self):
    with gcore_util.TestTemporaryDirectory(prefix='shape_setfeature') as tmpdir:
      field_settings = (
          ('real_field', ogr.OFTReal, '1.23', '7.8', 7.8),
          ('int_field', ogr.OFTInteger, '2', '3', 3),
          ('str_field', ogr.OFTString, 'original', 'new', 'new')
      )

      for field_name, field_type, original, new, result in field_settings:
        filepath = os.path.join(tmpdir, field_name + 'tmp.shp')
        dst = self.driver.CreateDataSource(filepath)
        layer = dst.CreateLayer('test_layer')
        layer.CreateField(ogr.FieldDefn(field_name, field_type))
        feature = ogr.Feature(layer.GetLayerDefn())
        feature.SetField(field_name, original)
        feature.SetGeometry(ogr.CreateGeometryFromWkt('POINT(4 5)'))
        layer.CreateFeature(feature)
        dst = None

        dst = ogr.Open(filepath, update=True)
        layer = dst.GetLayer()
        feature = layer.GetFeature(0)
        feature.SetField(field_name, new)
        new_geom_str = 'POINT (9 0)'
        feature.SetGeometry(ogr.CreateGeometryFromWkt(new_geom_str))
        self.assertEqual(layer.SetFeature(feature), 0)
        dst = None

        self.CheckOpen(filepath)
        layer = self.src.GetLayer()
        feature = layer.GetFeature(0)
        self.assertEqual(feature.GetField(0), result)
        self.assertEqual(feature.GetGeometryRef().ExportToWkt(), new_geom_str)


if __name__ == '__main__':
  unittest.main()
