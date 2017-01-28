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
# Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2014, Even Rouault <even dot rouault at mines-paris dot org>
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

"""Test geometry handling.

This is a rewrite of:

  http://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_geom.py.
"""

import pickle
import unittest


from osgeo import ogr
import unittest


class OgrGeomTest(unittest.TestCase):

  def testGeomArea(self):
    geom_wkt = (
        'MULTIPOLYGON( '
        '((0 0, 1 1, 1 0, 0 0)),'
        '((0 0, 10 0, 10 10, 0 10),'
        '(1 1, 1 2, 2 2, 2 1))'
        ' )')
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    self.assertAlmostEqual(geom.Area(), 99.5)

  # Test Area calculation for a LinearRing.  Exercises special case of
  # getGeometryType value.
  def testAreaLinearRing(self):
    geom = ogr.Geometry(type=ogr.wkbLinearRing)
    geom.AddPoint_2D(0, 0)
    geom.AddPoint_2D(10, 0)
    geom.AddPoint_2D(10, 10)
    geom.AddPoint_2D(0, 10)
    geom.AddPoint_2D(0, 0)

    self.assertAlmostEqual(geom.Area(), 100.0)

  def testAreaGeometryCollection(self):
    geom_wkt = (
        'GEOMETRYCOLLECTION( '
        'POLYGON((0 0, 1 1, 1 0, 0 0)), '
        'MULTIPOLYGON(((0 0, 1 1, 1 0, 0 0))), '
        'LINESTRING(0 0, 1 1), '
        'POINT(0 0), '
        'GEOMETRYCOLLECTION EMPTY )')
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    self.assertAlmostEqual(geom.Area(), 1.0)

  def testAreaLinearRingBigOffset(self):
    geom = ogr.Geometry(type=ogr.wkbLinearRing)
    offset = 1.0e+11
    geom.AddPoint_2D(0 + offset, 0 + offset)
    geom.AddPoint_2D(10 + offset, 0 + offset)
    geom.AddPoint_2D(10 + offset, 10 + offset)
    geom.AddPoint_2D(0 + offset, 10 + offset)
    geom.AddPoint_2D(0 + offset, 0 + offset)

    self.assertAlmostEqual(geom.GetArea(), 100.0)

  def testIsEmpty(self):
    geom = ogr.CreateGeometryFromWkt('LINESTRING EMPTY')
    self.assertTrue(geom.IsEmpty())
    geom = ogr.CreateGeometryFromWkt('POINT(1 2)')
    self.assertFalse(geom.IsEmpty())

  def testPickle(self):
    geom_wkt = (
        'MULTIPOLYGON('
        '((0 0, 1 1, 1 0, 0 0)), '
        '((0 0, 10 0, 10 10, 0 10), '
        '(1 1, 1 2, 2 2, 2 1))'
        ' )')
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    pickled_geom = pickle.dumps(geom)
    result = pickle.loads(pickled_geom)

    self.assertTrue(result.Equal(geom))

  # TODO(schwehr): Check for geos on this test.
  def testBoundaryPoint(self):
    geom_wkt = 'POINT(1 1)'
    geom = ogr.CreateGeometryFromWkt(geom_wkt)

    boundary = geom.Boundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbGeometryCollection)

  # TODO(schwehr): Check for geos on this test.
  def testBoundaryMultipoint(self):
    geom_wkt = 'MULTIPOINT((0 0), (1 1))'
    geom = ogr.CreateGeometryFromWkt(geom_wkt)

    boundary = geom.Boundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbGeometryCollection)

  # TODO(schwehr): Check for geos on this test.
  def testBoundaryLinestring(self):
    # TODO(schwehr): Explain why the first has a count of 0 and the 2nd has 2.
    geom_wkt = 'LINESTRING(0 0, 1 0, 1 1, 0 1, 0 0)'
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    boundary = geom.GetBoundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbMultiPoint)
    self.assertEqual(boundary.GetGeometryCount(), 0)

    geom_wkt = 'LINESTRING(0 0, 1 1, 2 2, 3 2, 4 2)'
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    boundary = geom.GetBoundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbMultiPoint)
    self.assertEqual(boundary.GetGeometryCount(), 2)

  # TODO(schwehr): Check for geos on this test.
  def testBoundaryPolygon(self):
    geom_wkt = 'POLYGON((0 0, 1 1, 1 0, 0 0))'
    geom = ogr.CreateGeometryFromWkt(geom_wkt)

    boundary = geom.GetBoundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbLineString)

  # TODO(schwehr): Check for geos on this test.
  def testBuildPolygonFromEdges(self):

    link_collection = ogr.Geometry(type=ogr.wkbGeometryCollection)

    # TODO(schwehr): Make a simplier example that fits nicely in 80 columns.
    wkt_lines = (
        'LINESTRING(-87.601595 30.999522, -87.599623 31.000059, '
        '-87.599219 31.00017)',
        'LINESTRING(-87.601595 30.999522, -87.604349 30.999493, '
        '-87.606935 30.99952)',
        'LINESTRING(-87.59966 31.000756, -87.599851 31.000805, '
        '-87.599992 31.000805, -87.600215 31.000761, -87.600279 31.000723, '
        '-87.600586 31.000624, -87.601256 31.000508, -87.602501 31.000447, '
        '-87.602801 31.000469, -87.603108 31.000579, -87.603331 31.000716, '
        '-87.603523 31.000909, -87.603766 31.001233, -87.603913 31.00136)',
        'LINESTRING(-87.606134 31.000182, -87.605885 31.000325, '
        '-87.605343 31.000716, -87.60466 31.001117, -87.604468 31.0012, '
        '-87.603913 31.00136)',
        'LINESTRING(-87.599219 31.00017, -87.599289 31.0003, '
        '-87.599398 31.000426, -87.599564 31.000547, -87.599609 31.000701, '
        '-87.59966 31.000756)',
        'LINESTRING(-87.606935 30.99952, -87.606713 30.999799, '
        '-87.6064 30.999981, -87.606134 31.000182)')

    for wkt_line in wkt_lines:
      geom = ogr.CreateGeometryFromWkt(wkt_line)
      link_collection.AddGeometry(geom)

    polygon = ogr.BuildPolygonFromEdges(link_collection)
    self.assertIsNotNone(polygon)
    # TODO(schwehr): Do something to actually check the polygon.

# TODO(schwehr): Port the rest of the tests from autotest.


if __name__ == '__main__':
  unittest.main()
