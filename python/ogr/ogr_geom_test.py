# Copyright 2016 Google Inc. All Rights Reserved.
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
# Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>
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

https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_geom.py
"""

from collections.abc import Iterator
import contextlib
import copy
import math
import pickle
import random

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest


@contextlib.contextmanager
def _disable_exceptions() -> Iterator[None]:
  with (
      gdal.ExceptionMgr(useExceptions=False),
      ogr.ExceptionMgr(useExceptions=False),
      osr.ExceptionMgr(useExceptions=False),
  ):
    yield


def _have_geos() -> bool:
  return ogr.GetGEOSVersionMajor() > 0


def _geos_version() -> tuple[int, int, int]:
  return (
      ogr.GetGEOSVersionMajor(),
      ogr.GetGEOSVersionMinor(),
      ogr.GetGEOSVersionMicro(),
  )


def _have_sfcgal() -> bool:
  point1 = ogr.CreateGeometryFromWkt('POINT (1.0 1.0 1.0)')
  point2 = ogr.CreateGeometryFromWkt('POINT (4.0 1.0 5.0)')
  with _disable_exceptions(), gdal.quiet_errors():
    return point1.Distance3D(point2) == 5.0


class OgrGeomTest(gdaltest.TestCase, parameterized.TestCase):

  def setUp(self):
    super().setUp()
    gdal.UseExceptions()

  def _check_geometry(
      self,
      actual: ogr.Geometry | None,
      expected: ogr.Geometry | str | None,
      max_error: float = 0.0001,
      *,
      context: str | None = None,
      _root_actual: ogr.Geometry | None = None,
      _root_expected: ogr.Geometry | None = None,
  ) -> None:
    context_msg = f' ({context})' if context else ''

    if isinstance(expected, str):
      expected_geom: ogr.Geometry | None = ogr.CreateGeometryFromWkt(expected)
      self.assertIsNotNone(
          expected_geom,
          f'failed to parse expected geometry as WKT{context_msg}',
      )
    else:
      expected_geom = expected

    if expected_geom is None:
      self.assertIsNone(
          actual, f'expected NULL geometry but got one{context_msg}'
      )
      return
    self.assertIsNotNone(
        actual, f'expected geometry but got NULL{context_msg}'
    )
    assert actual is not None
    assert expected_geom is not None

    self.assertEqual(
        actual.GetGeometryName(),
        expected_geom.GetGeometryName(),
        f'geometry types do not match{context_msg}',
    )
    self.assertEqual(
        actual.GetGeometryCount(),
        expected_geom.GetGeometryCount(),
        f'sub-geometry counts do not match{context_msg}',
    )
    self.assertEqual(
        actual.GetPointCount(),
        expected_geom.GetPointCount(),
        f'point counts do not match{context_msg}',
    )

    if expected_geom.Is3D():
      self.assertTrue(actual.Is3D(), 'expected Z dimension not found')
    if actual.Is3D():
      self.assertTrue(expected_geom.Is3D(), 'unexpected Z dimension')

    if expected_geom.IsMeasured():
      self.assertTrue(actual.IsMeasured(), 'expected M dimension not found')
    if actual.IsMeasured():
      self.assertTrue(expected_geom.IsMeasured(), 'unexpected M dimension')

    if _root_actual is None:
      _root_actual = actual
    if _root_expected is None:
      _root_expected = expected_geom

    if actual.GetGeometryCount() > 0:
      for i in range(actual.GetGeometryCount()):
        self._check_geometry(
            actual.GetGeometryRef(i),
            expected_geom.GetGeometryRef(i),
            max_error,
            context=context,
            _root_actual=_root_actual,
            _root_expected=_root_expected,
        )
    else:
      count = actual.GetPointCount()
      if ogr.GT_Flatten(actual.GetGeometryType()) == ogr.wkbPoint:
        count = 1
        if expected_geom.IsEmpty():
          self.assertTrue(actual.IsEmpty())
          return
        self.assertFalse(actual.IsEmpty())

      for i in range(count):
        actual_pt = [actual.GetX(i), actual.GetY(i)]
        expected_pt = [expected_geom.GetX(i), expected_geom.GetY(i)]

        if actual.Is3D() or expected_geom.Is3D():
          actual_pt.append(actual.GetZ(i))
          expected_pt.append(expected_geom.GetZ(i))

        if actual.IsMeasured() or expected_geom.IsMeasured():
          actual_pt.append(actual.GetM(i))
          expected_pt.append(expected_geom.GetM(i))

        for act_coord, exp_coord in zip(actual_pt, expected_pt, strict=True):
          self.assertAlmostEqual(
              act_coord,
              exp_coord,
              delta=max_error,
              msg=(
                  f'Error in vertex {i + 1}/{count} exceeds tolerance.'
                  f'{context_msg}\n'
                  f'  Expected: {_root_expected.ExportToWkt()}\n'
                  f'  Actual: {_root_actual.ExportToWkt()}'
              ),
          )

  def test_ogr_geom_area(self):
    geom_wkt = (
        'MULTIPOLYGON( '
        '((0 0, 1 1, 1 0, 0 0)),'
        '((0 0, 10 0, 10 10, 0 10),'
        '(1 1, 1 2, 2 2, 2 1))'
        ' )'
    )
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    self.assertAlmostEqual(geom.GetArea(), 99.5, delta=1e-11)
    self.assertAlmostEqual(geom.Area(), 99.5, delta=1e-11)

  def test_ogr_geom_area_linearring(self):
    geom = ogr.Geometry(type=ogr.wkbLinearRing)
    geom.AddPoint_2D(0, 0)
    geom.AddPoint_2D(10, 0)
    geom.AddPoint_2D(10, 10)
    geom.AddPoint_2D(0, 10)
    geom.AddPoint_2D(0, 0)

    self.assertAlmostEqual(geom.Area(), 100.0, delta=1e-11)

  def test_ogr_geom_area_geometrycollection(self):
    geom_wkt = (
        'GEOMETRYCOLLECTION( '
        'POLYGON((0 0, 1 1, 1 0, 0 0)), '
        'MULTIPOLYGON(((0 0, 1 1, 1 0, 0 0))), '
        'LINESTRING(0 0, 1 1), '
        'POINT(0 0), '
        'GEOMETRYCOLLECTION EMPTY )'
    )
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    self.assertAlmostEqual(geom.Area(), 1.0, delta=1e-11)

  def test_ogr_geom_area_linearring_big_offset(self):
    geom = ogr.Geometry(type=ogr.wkbLinearRing)
    offset = 1.0e11
    geom.AddPoint_2D(0 + offset, 0 + offset)
    geom.AddPoint_2D(10 + offset, 0 + offset)
    geom.AddPoint_2D(10 + offset, 10 + offset)
    geom.AddPoint_2D(0 + offset, 10 + offset)
    geom.AddPoint_2D(0 + offset, 0 + offset)

    self.assertAlmostEqual(geom.GetArea(), 100.0, delta=1e-11)

  def test_ogr_geom_area_triangle(self):
    geom_wkt = 'TRIANGLE((0 0,100 0,0 100,0 0))'
    geom = ogr.CreateGeometryFromWkt(geom_wkt)

    self.assertAlmostEqual(geom.GetArea(), 5000.0, delta=1e-11)
    self.assertAlmostEqual(geom.Area(), 5000.0, delta=1e-11)

  def test_ogr_geom_is_empty(self):
    geom = ogr.CreateGeometryFromWkt('LINESTRING EMPTY')
    self.assertTrue(geom.IsEmpty())
    geom = ogr.CreateGeometryFromWkt('POINT(1 2)')
    self.assertIsNotNone(geom)
    self.assertFalse(geom.IsEmpty())

  def test_ogr_geom_is_empty_triangle(self):
    geom = ogr.CreateGeometryFromWkt('TRIANGLE EMPTY')
    self.assertTrue(geom.IsEmpty())

    geom = ogr.CreateGeometryFromWkb(geom.ExportToWkb())
    self.assertTrue(geom.IsEmpty())

    geom = ogr.CreateGeometryFromWkt('TRIANGLE((0 0,100 0,0 100,0 0))')
    self.assertIsNotNone(geom)
    self.assertFalse(geom.IsEmpty())

  @googletest.skip('Empty geometries cannot be constructed')
  def test_ogr_geom_pickle(self):
    geom_str = '((0 0,1 1,1 0,0 0)),((0 0,10 0, 10 10, 0 10),(1 1,1 2,2 2,2 1))'
    geom_wkt = f'MULTIPOLYGON({geom_str})'
    geom = ogr.CreateGeometryFromWkt(geom_wkt)
    p = pickle.dumps(geom)
    with gdal.quiet_errors():
      g = pickle.loads(p)  # pylint: disable=g-unsafe-pickle-load

    self.assertTrue(geom.Equal(g))

  def test_ogr_geom_polyhedral_surface(self):
    wkt_original = (
        'POLYHEDRALSURFACE Z (((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)),'
        '((0 0 0,0 1 0,1 1 0,1 0 0,0 0 0)),'
        '((0 0 0,1 0 0,1 0 1,0 0 1,0 0 0)),'
        '((1 1 0,1 1 1,1 0 1,1 0 0,1 1 0)),'
        '((0 1 0,0 1 1,1 1 1,1 1 0,0 1 0)),'
        '((0 0 1,1 0 1,1 1 1,0 1 1,0 0 1)))'
    )
    ps = ogr.CreateGeometryFromWkt(wkt_original)

    wkb_string = ps.ExportToWkb(ogr.wkbXDR)
    geom = ogr.CreateGeometryFromWkb(wkb_string)
    self.assertEqual(geom.ExportToWkt(), wkt_original)
    self.assertEqual(geom.Clone().ExportToWkt(), wkt_original)

    polygon_wkt = ogr.ForceTo(geom.Clone(), ogr.wkbPolygon25D).ExportToWkt()
    self.assertEqual(polygon_wkt, wkt_original)

    multipolygon_wkt = ogr.ForceTo(
        geom.Clone(), ogr.wkbMultiPolygon25D
    ).ExportToWkt()
    self.assertEqual(
        multipolygon_wkt,
        'MULTIPOLYGON (((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)),'
        '((0 0 0,0 1 0,1 1 0,1 0 0,0 0 0)),'
        '((0 0 0,1 0 0,1 0 1,0 0 1,0 0 0)),'
        '((1 1 0,1 1 1,1 0 1,1 0 0,1 1 0)),'
        '((0 1 0,0 1 1,1 1 1,1 1 0,0 1 0)),'
        '((0 0 1,1 0 1,1 1 1,0 1 1,0 0 1)))',
    )

    if _have_sfcgal():
      self.assertEqual(ps.Area(), 6.0)

    self.assertEqual(ps.WkbSize(), 807)

    if _have_geos():
      geom = ogr.CreateGeometryFromWkb(wkb_string)
      # GEOS RelateNG emits an error on degenerate 2D-projected vertical faces
      # and returns 2 (truthy) when exceptions are disabled.
      with _disable_exceptions(), gdal.quiet_errors():
        self.assertTrue(ps.Contains(geom))

    self.assertFalse(ps.IsEmpty())

    ps.Empty()
    self.assertEqual(ps.ExportToWkt(), 'POLYHEDRALSURFACE Z EMPTY')

    g = ogr.CreateGeometryFromWkt(
        'POLYHEDRALSURFACE (((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)))'
    )
    self.assertNotEqual(g.Equals(g), 0)

    for wkt in [
        'MULTIPOLYGON (((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)))',
        'POLYHEDRALSURFACE (((0 0 0,0 0 1,0 1 1,0 0 0)))',
        (
            'POLYHEDRALSURFACE (((0 0 0,0 0 1,0 1 1,0 0 0)),'
            '((0 0 0,0 0 1,0 1 1,0 0 0)))'
        ),
        'POLYHEDRALSURFACE EMPTY',
    ]:
      g2 = ogr.CreateGeometryFromWkt(wkt)
      self.assertFalse(g.Equals(g2), wkt)

    with _disable_exceptions():
      self.assertNotEqual(
          g.AddGeometry(ogr.CreateGeometryFromWkt('POINT (0 0)')), 0
      )
      self.assertNotEqual(
          g.AddGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)')), 0
      )

    # Test dimension promotion.
    g = ogr.CreateGeometryFromWkt('POLYHEDRALSURFACE EMPTY')
    g.AddGeometryDirectly(
        ogr.CreateGeometryFromWkt(
            'POLYGON ZM ((0 0 1 2,0 1 1 2,1 1 1 2,0 0 1 2))'
        )
    )
    g.AddGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON ((10 10,10 11,11 11,10 10))')
    )
    self.assertEqual(
        g.ExportToIsoWkt(),
        'POLYHEDRALSURFACE ZM (((0 0 1 2,0 1 1 2,1 1 1 2,0 0 1 2)),'
        '((10 10 0 0,10 11 0 0,11 11 0 0,10 10 0 0)))',
    )

  def test_ogr_geom_tin(self):
    poly1 = ogr.CreateGeometryFromWkt('TRIANGLE ((0 0 0,0 0 1,0 1 0,0 0 0))')
    poly2 = ogr.CreateGeometryFromWkt('TRIANGLE ((0 0 0,0 1 0,1 1 0,0 0 0))')
    tin = ogr.Geometry(ogr.wkbTIN)
    tin.AddGeometry(poly1)
    tin.AddGeometry(poly2)
    polygon_wkt = [poly1.ExportToWkt(), poly2.ExportToWkt()]
    for i in range(tin.GetGeometryCount()):
      geom = tin.GetGeometryRef(i)
      self.assertEqual(polygon_wkt[i], geom.ExportToWkt())

    wkt_original = (
        'TIN Z (((0 0 0,0 0 1,0 1 0,0 0 0)),((0 0 0,0 1 0,1 1 0,0 0 0)))'
    )
    tin = ogr.CreateGeometryFromWkt(wkt_original)

    wkb_string = tin.ExportToWkb(ogr.wkbXDR)
    geom = ogr.CreateGeometryFromWkb(wkb_string)
    self.assertEqual(geom.ExportToWkt(), wkt_original)
    self.assertEqual(geom.Clone().ExportToWkt(), wkt_original)

    if _have_sfcgal():
      self.assertEqual(12.3 * tin.Area(), 12.3)

    self.assertEqual(tin.WkbSize(), 227)
    self.assertFalse(tin.IsEmpty())

    tin.Empty()
    self.assertEqual(tin.ExportToWkt(), 'TIN Z EMPTY')

    wrong_polygon = ogr.CreateGeometryFromWkt(
        'POLYGON ((0 0 0,0 1 0,1 1 0,0 0 1))'
    )
    geom_count = tin.GetGeometryCount()
    with _disable_exceptions(), gdal.quiet_errors():
      err = tin.AddGeometry(wrong_polygon)
    self.assertNotEqual(err, 0)
    self.assertEqual(tin.GetGeometryCount(), geom_count)

    if _have_geos():
      point = tin.PointOnSurface()
      self.assertEqual(point.ExportToWkt(), 'POINT EMPTY')

    tin = ogr.CreateGeometryFromWkt(wkt_original)
    tin.FlattenTo2D()
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertFalse(tin.IsValid())

    for invalid_wkt in [
        'TIN (((0 0,0 1,1 1,1 0,0 0)))',
        'TIN(((0 0,0 1,1 1,0 0),(0.1 0.1,0.1 0.2,0.2 0.2,0.1 0.1)))',
        'TIN (POLYGON((0 0,0 1,1 1,0 0)))',
    ]:
      with _disable_exceptions(), gdal.quiet_errors():
        g = ogr.CreateGeometryFromWkt(invalid_wkt)
      self.assertIsNone(g, invalid_wkt)

    # Add a POLYGON that can be cast as a TRIANGLE.
    g = ogr.Geometry(ogr.wkbTIN)
    self.assertEqual(
        g.AddGeometry(ogr.CreateGeometryFromWkt('POLYGON ((0 0,0 1,1 1,0 0))')),
        0,
    )
    self.assertEqual(g.ExportToIsoWkt(), 'TIN (((0 0,0 1,1 1,0 0)))')

  def test_ogr_geom_boundary_point(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    geom = ogr.CreateGeometryFromWkt('POINT(1 1)')
    boundary = geom.GetBoundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbGeometryCollection)
    boundary = geom.Boundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbGeometryCollection)

  def test_ogr_geom_boundary_multipoint(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    geom = ogr.CreateGeometryFromWkt('MULTIPOINT((0 0), (1 1))')
    boundary = geom.Boundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbGeometryCollection)

  def test_ogr_geom_boundary_linestring(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    # Closed ring has empty boundary (0 points); open linestring has 2.
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

  def test_ogr_geom_boundary_polygon(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    geom = ogr.CreateGeometryFromWkt('POLYGON((0 0, 1 1, 1 0, 0 0))')
    boundary = geom.GetBoundary()
    self.assertEqual(boundary.GetGeometryType(), ogr.wkbLineString)

  def test_ogr_geom_build_from_edges_1(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    link_collection = ogr.Geometry(type=ogr.wkbGeometryCollection)
    wkt_lines = (
        (
            'LINESTRING(-87.601595 30.999522, -87.599623 31.000059, '
            '-87.599219 31.00017)'
        ),
        (
            'LINESTRING(-87.601595 30.999522, -87.604349 30.999493, '
            '-87.606935 30.99952)'
        ),
        (
            'LINESTRING(-87.59966 31.000756, -87.599851 31.000805, '
            '-87.599992 31.000805, -87.600215 31.000761, -87.600279 31.000723, '
            '-87.600586 31.000624, -87.601256 31.000508, -87.602501 31.000447, '
            '-87.602801 31.000469, -87.603108 31.000579, -87.603331 31.000716, '
            '-87.603523 31.000909, -87.603766 31.001233, -87.603913 31.00136)'
        ),
        (
            'LINESTRING(-87.606134 31.000182, -87.605885 31.000325, '
            '-87.605343 31.000716, -87.60466 31.001117, -87.604468 31.0012, '
            '-87.603913 31.00136)'
        ),
        (
            'LINESTRING(-87.599219 31.00017, -87.599289 31.0003, '
            '-87.599398 31.000426, -87.599564 31.000547, -87.599609 31.000701, '
            '-87.59966 31.000756)'
        ),
        (
            'LINESTRING(-87.606935 30.99952, -87.606713 30.999799, '
            '-87.6064 30.999981, -87.606134 31.000182)'
        ),
    )

    for wkt_line in wkt_lines:
      link_collection.AddGeometry(ogr.CreateGeometryFromWkt(wkt_line))

    polygon = ogr.BuildPolygonFromEdges(link_collection)
    self.assertIsNotNone(polygon)

  def test_ogr_geom_build_from_edges_2(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    link_collection = ogr.Geometry(type=ogr.wkbMultiLineString)
    wkt_lines = (
        (
            'LINESTRING (-87.601595 30.999522,-87.599623 31.000059,'
            '-87.599219 31.00017)'
        ),
        (
            'LINESTRING (-87.601595 30.999522,-87.604349 30.999493,'
            '-87.606935 30.99952)'
        ),
        (
            'LINESTRING (-87.59966 31.000756,-87.599851 31.000805,'
            '-87.599992 31.000805,-87.600215 31.000761,-87.600279 31.000723,'
            '-87.600586 31.000624,-87.601256 31.000508,-87.602501 31.000447,'
            '-87.602801 31.000469,-87.603108 31.000579,-87.603331 31.000716,'
            '-87.603523 31.000909,-87.603766 31.001233,-87.603913 31.00136)'
        ),
        (
            'LINESTRING (-87.606134 31.000182,-87.605885 31.000325,'
            '-87.605343 31.000716,-87.60466 31.001117,-87.604468 31.0012,'
            '-87.603913 31.00136)'
        ),
        (
            'LINESTRING (-87.599219 31.00017,-87.599289 31.0003,'
            '-87.599398 31.000426,-87.599564 31.000547,-87.599609 31.000701,'
            '-87.59966 31.000756)'
        ),
        (
            'LINESTRING (-87.606935 30.99952,-87.606713 30.999799,'
            '-87.6064 30.999981,-87.606134 31.000182)'
        ),
    )

    for wkt_line in wkt_lines:
      link_collection.AddGeometry(ogr.CreateGeometryFromWkt(wkt_line))

    polygon = ogr.BuildPolygonFromEdges(link_collection)
    self.assertIsNotNone(polygon)

  def test_ogr_geom_build_from_edges_3(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    src_geom = ogr.CreateGeometryFromWkt('POINT (0 1)')
    with _disable_exceptions(), gdal.quiet_errors():
      poly = ogr.BuildPolygonFromEdges(src_geom)
    self.assertIsNone(poly)

    src_geom = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION (LINESTRING(0 1,2 3),POINT(0 1),'
        'LINESTRING(0 1,-2 3),LINESTRING(-2 3,2 3))'
    )
    with _disable_exceptions(), gdal.quiet_errors():
      poly = ogr.BuildPolygonFromEdges(src_geom)
    self.assertIsNone(poly)

  def test_ogr_geom_build_from_edges_4(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    link_collection = ogr.Geometry(type=ogr.wkbGeometryCollection)
    wkt_lines = (
        'LINESTRING EMPTY',
        'LINESTRING (1 1,1 2)',
        'LINESTRING EMPTY',
        'LINESTRING (1 2,2 2)',
        'LINESTRING (2 2,2 1)',
        'LINESTRING (2 1,1 1)',
        'LINESTRING (0 0,0 10)',
        'LINESTRING (0 10,10 10)',
        'LINESTRING (10 10,10 0)',
        'LINESTRING (10 0,0 0)',
    )

    for wkt_line in wkt_lines:
      link_collection.AddGeometry(ogr.CreateGeometryFromWkt(wkt_line))

    poly = ogr.BuildPolygonFromEdges(link_collection)
    self.assertIsNotNone(poly)
    self.assertEqual(
        poly.ExportToWkt(),
        'POLYGON ((0 0,0 10,10 10,10 0,0 0),(1 1,1 2,2 2,2 1,1 1))',
    )

  def test_ogr_geom_area_empty_linearring(self):
    geom = ogr.Geometry(type=ogr.wkbLinearRing)
    self.assertEqual(geom.GetArea(), 0)

  def test_ogr_geom_transform_to(self):
    # Somewhere in Paris suburbs...
    geom = ogr.CreateGeometryFromWkt('POINT(2 49)')

    # Input SRS is EPSG:4326
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    sr.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    geom.AssignSpatialReference(sr)

    # Output SRS is EPSG:32631
    sr2 = osr.SpatialReference()
    sr2.ImportFromEPSG(32631)
    ret = geom.TransformTo(sr2)

    self.assertEqual(ret, 0)
    self.assertAlmostEqual(geom.GetX(), 426857, delta=1)
    self.assertAlmostEqual(geom.GetY(), 5427937, delta=1)

    # Geometry without SRS
    geom = ogr.CreateGeometryFromWkt('POINT(2 49)')
    gdal.ErrorReset()
    with _disable_exceptions(), gdal.quiet_errors():
      ret = geom.TransformTo(sr2)
    self.assertNotEqual(ret, 0)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  @parameterized.parameters(
      ('POINT Z (90 -90 0)', 'POINT Z (0 0 -6356752.31424518)'),
      ('POINT ZM (90 -90 0 20)', 'POINT ZM (0 0 -6356752.31424518 20)'),
      (
          'LINESTRING Z (90 -90 0,0 0 1000000000)',
          'LINESTRING Z (0 0 -6356752.31424518,1006378137.0 0 0)',
      ),
      (
          'LINESTRING ZM (90 -90 0 20,0 0 1000000000 30)',
          'LINESTRING ZM (0 0 -6356752.31424518 20,1006378137.0 0 0 30)',
      ),
  )
  def test_ogr_geom_transform_3d_to(self, input_wkt, output_wkt):
    # Input SRS is EPSG:4979
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4979)
    sr.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    # Output SRS is EPSG:4978
    sr2 = osr.SpatialReference()
    sr2.ImportFromEPSG(4978)

    geom = ogr.CreateGeometryFromWkt(input_wkt)
    geom.AssignSpatialReference(sr)
    self.assertEqual(geom.TransformTo(sr2), 0)
    self._check_geometry(geom, output_wkt)

  def test_ogr_geom_transform(self):
    # Somewhere in Paris suburbs...
    geom = ogr.CreateGeometryFromWkt('POINT(2 49)')

    # Input SRS is EPSG:4326
    sr = osr.SpatialReference()
    sr.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    sr.ImportFromEPSG(4326)

    # Output SRS is EPSG:32631
    sr2 = osr.SpatialReference()
    sr2.ImportFromEPSG(32631)

    ct = osr.CoordinateTransformation(sr, sr2)
    geom.Transform(ct)

    self.assertAlmostEqual(geom.GetX(), 426857, delta=1)
    self.assertAlmostEqual(geom.GetY(), 5427937, delta=1)

  def test_ogr_geom_transform_geogcrs_to_wgs84(self):
    srs_4326 = osr.SpatialReference()
    srs_4326.ImportFromEPSG(4326)
    srs_4326.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    srs_7683 = osr.SpatialReference()
    srs_7683.ImportFromEPSG(7683)
    srs_7683.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    wkt = (
        'POLYGON ((44.0 43.1666666666667,44.0 43.3333333333333,'
        '44.25 43.3333333333333,44.25 43.1666666666667,'
        '44.0 43.1666666666667))'
    )

    options = osr.CoordinateTransformationOptions()
    # Setting the pipeline is not compulsory, but this enables us to be
    # independent of PROJ transformation database.
    self.assertTrue(
        options.SetOperation(
            '+proj=pipeline +step +proj=axisswap +order=2,1 +step'
            ' +proj=unitconvert +xy_in=deg +xy_out=rad +step +proj=push +v_3'
            ' +step +proj=cart +ellps=GSK2011 +step +inv +proj=cart'
            ' +ellps=WGS84 +step +proj=pop +v_3 +step +proj=unitconvert'
            ' +xy_in=rad +xy_out=deg +step +proj=axisswap +order=2,1'
        )
    )
    ct = osr.CoordinateTransformation(srs_7683, srs_4326)
    tr = ogr.GeomTransformer(ct)
    polygon = ogr.CreateGeometryFromWkt(wkt)
    polygon.AssignSpatialReference(srs_7683)
    gdal.ErrorReset()
    polygon = tr.Transform(polygon)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    self._check_geometry(
        polygon,
        'POLYGON ((44.0 43.1666661611411,44.0 43.3333328276326,'
        '44.25 43.3333328276326,44.25 43.1666661611411,'
        '44.0 43.1666661611411))',
    )

  @parameterized.parameters(
      (
          (
              'POLYGON((0 100000,100000 0,0 -100000,-100000 0,0 100000),'
              '(0 50000,50000 0,0 -50000,-50000 0,0 50000))'
          ),
          (
              'POLYGON ((90.0 89.089200825091,0.0 89.089200825091,'
              '-90 89.089200825091,-180 89.0892008251069,-180 89.5445935108883,'
              '-90 89.5445935108803,0.0 89.5445935108803,90.0 89.5445935108803,'
              '180.0 89.5445935108883,180.0 89.0892008251069,'
              '90.0 89.089200825091))'
          ),
      ),
      (
          (
              'POLYGON((50000 -100000,100000 -100000,100000 100000,'
              '-100000 100000,-100000 50000,50000 50000,50000 -100000))'
          ),
          (
              'MULTIPOLYGON (((135.0 88.7119614804959,45.0 88.7119614804959,'
              '26.565051177078 88.9817007095479,135.0 89.3559612202261,'
              '180.0 89.5445935108803,180.0 89.089200825091,'
              '135.0 88.7119614804959)),'
              '((-116.565051177078 88.9817007095479,-135 88.7119614804959,'
              '-180 89.089200825091,-180 89.5445935108803,'
              '-116.565051177078 88.9817007095479)))'
          ),
      ),
  )
  def test_ogr_geom_transform_polar_projected_to_geographic(
      self, input_wkt, output_wkt
  ):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    srs_3996 = osr.SpatialReference()
    srs_3996.ImportFromEPSG(3996)
    srs_3996.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    srs_4326 = osr.SpatialReference()
    srs_4326.ImportFromEPSG(4326)
    srs_4326.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    ct = osr.CoordinateTransformation(srs_3996, srs_4326)
    tr = ogr.GeomTransformer(ct)

    g = ogr.CreateGeometryFromWkt(input_wkt)
    g = tr.Transform(g)
    self._check_geometry(g, output_wkt)

    tr = ogr.GeomTransformer(ct, ['WRAPDATELINE=YES'])
    g = ogr.CreateGeometryFromWkt(input_wkt)
    g = tr.Transform(g)
    self._check_geometry(g, output_wkt)

  def test_ogr_geomtransfomer_default(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    src = osr.SpatialReference()
    src.ImportFromEPSG(32660)

    dst = osr.SpatialReference()
    dst.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    dst.ImportFromEPSG(4326)

    ct = osr.CoordinateTransformation(src, dst)
    geom = ogr.CreateGeometryFromWkt(
        'LINESTRING(832864.275023695 0,835092.849076364 0)'
    )
    transformer = ogr.GeomTransformer(ct)

    geom_dst = transformer.Transform(geom)
    self.assertEqual(
        geom_dst.ExportToWkt(),
        'MULTILINESTRING ((179.99 0.0,180 0),(-180 0,-179.99 0.0))',
    )

  def test_ogr_geomtransfomer_wrapdateline_with_ct(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    src = osr.SpatialReference()
    src.ImportFromEPSG(3857)

    dst = osr.SpatialReference()
    dst.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    dst.ImportFromEPSG(4326)

    ct = osr.CoordinateTransformation(src, dst)
    geom = ogr.CreateGeometryFromWkt(
        'LINESTRING(20026376.3937099 0,-20026376.3937099 0)'
    )
    transformer = ogr.GeomTransformer(ct, ['WRAPDATELINE=YES'])

    geom_dst = geom.Transform(transformer)
    self._check_geometry(
        geom_dst, 'MULTILINESTRING ((179.9 0.0,180 0),(-180 0,-179.9 0.0))'
    )

  def test_ogr_geomtransfomer_wrapdateline_no_ct(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    geom = ogr.CreateGeometryFromWkt('LINESTRING(-179 0,179 0)')
    transformer = ogr.GeomTransformer(None, ['WRAPDATELINE=YES'])

    geom_dst = geom.Transform(transformer)
    self.assertEqual(
        geom_dst.ExportToWkt(),
        'MULTILINESTRING ((-179 0,-180 0),(180 0,179 0))',
    )

  def test_ogr_geomtransfomer_wrapdateline_multipoint(self):
    transformer = ogr.GeomTransformer(None, ['WRAPDATELINE=YES'])

    geom = ogr.CreateGeometryFromWkt(
        'MULTIPOINT((-179 0),(179 0),(-182 0),(182 0))'
    )
    geom_dst = geom.Transform(transformer)
    self.assertEqual(
        geom_dst.ExportToIsoWkt(),
        'MULTIPOINT ((-179 0),(179 0),(178 0),(-178 0))',
    )

  def test_ogr_geomtransfomer_wrapdateline_geometrycollection(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    transformer = ogr.GeomTransformer(None, ['WRAPDATELINE=YES'])

    geom = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(POINT(-182 0),LINESTRING(-179 0,179 0))'
    )
    geom_dst = geom.Transform(transformer)
    self.assertEqual(
        geom_dst.ExportToIsoWkt(),
        'GEOMETRYCOLLECTION (POINT (178 0),LINESTRING (-179 0,-180 0),'
        'LINESTRING (180 0,179 0))',
    )

  def test_ogr_geom_closerings(self):
    geom = ogr.CreateGeometryFromWkt('POLYGON((0 0,0 1,1 1,1 0))')
    geom.CloseRings()
    self.assertEqual(geom.ExportToWkt(), 'POLYGON ((0 0,0 1,1 1,1 0,0 0))')

    geom.CloseRings()
    self.assertEqual(geom.ExportToWkt(), 'POLYGON ((0 0,0 1,1 1,1 0,0 0))')

  def test_ogr_geom_segmentize(self):
    # 2D
    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 0,0 10)')
    geom.Segmentize(1.00001)
    self._check_geometry(
        geom, 'LINESTRING (0 0,0 1,0 2,0 3,0 4,0 5,0 6,0 7,0 8,0 9,0 10)'
    )

    # 2D + Z
    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 0 1,0 10 1)')
    geom.Segmentize(1.00001)
    self._check_geometry(
        geom,
        'LINESTRING (0 0 1,0 1 1,0 2 1,0 3 1,0 4 1,0 5 1,0 6 1,0 7 1,0 8 1,'
        '0 9 1,0 10 1)',
    )
    self.assertEqual(str(geom), geom.ExportToIsoWkt())

    # 2D + M
    geom = ogr.CreateGeometryFromWkt('LINESTRING M(0 0 1,0 10 2)')
    geom.Segmentize(5)
    self.assertEqual(
        geom.ExportToIsoWkt(), 'LINESTRING M (0 0 1,0 5 2,0 10 2)'
    )
    self.assertEqual(str(geom), geom.ExportToIsoWkt())

    # 2D + ZM
    geom = ogr.CreateGeometryFromWkt('LINESTRING ZM(0 0 1 2,0 10 1 2)')
    geom.Segmentize(5)
    self.assertEqual(
        geom.ExportToIsoWkt(), 'LINESTRING ZM (0 0 1 2,0 5 1 2,0 10 1 2)'
    )
    self.assertEqual(str(geom), geom.ExportToIsoWkt())

    # Test distance between points <<<< segmentization threshold
    # https://github.com/OSGeo/gdal/issues/1414
    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 0,0 1)')
    geom.Segmentize(10000)
    self.assertEqual(geom.ExportToWkt(), 'LINESTRING (0 0,0 1)')

    # Check segmentize symmetry: do exact binary comparison.
    g1 = ogr.CreateGeometryFromWkt('LINESTRING (0 0,1.2 1,2 0)')
    g1.Segmentize(0.25)
    g2 = ogr.CreateGeometryFromWkt('LINESTRING (2 0,1.2 1,0 0)')
    g2.Segmentize(0.25)
    for i in range(g1.GetPointCount()):
      self.assertEqual(g1.GetPoint(i), g2.GetPoint(g1.GetPointCount() - 1 - i))

    # Test extremely small threshold
    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 0,0 1)')
    with _disable_exceptions(), gdal.quiet_errors():
      geom.Segmentize(1e-30)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    # On a CircularStringZM
    geom = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING ZM(0 0 10 100,1 1 11 101,2 0 12 102)'
    )
    geom.Segmentize(1)
    self._check_geometry(
        geom,
        'CIRCULARSTRING ZM (0 0 10 100,'
        '0.292893218813453 0.707106781186548 10.5 101,1 1 11 101,'
        '1.70710678118655 0.707106781186547 11.5 102,2 0 12 102)',
    )

    # Test that it does not crash
    geom.Segmentize(0.1)

    with self.assertRaises(RuntimeError):
      geom.Segmentize(1e-10)

  def test_ogr_geom_segmentize_issue_1341(self):
    expected_geom = (
        'LINESTRING (0 0,0.4 0.0,0.8 0.0,1.2 0.0,1.6 0.0,2 0,2.4 0.0,2.8 0.0,'
        '3.2 0.0,3.6 0.0,4 0,4.4 0.0,4.8 0.0,5.2 0.0,5.6 0.0,6 0,6.4 0.0,'
        '6.8 0.0,7.2 0.0,7.6 0.0,8 0,8.4 0.0,8.8 0.0,9.2 0.0,9.6 0.0,10 0)'
    )

    for step in (0.399999999999, 0.4, 0.400000000001):
      geom = ogr.CreateGeometryFromWkt('LINESTRING(0 0,10 0)')
      geom.Segmentize(step)
      self._check_geometry(geom, expected_geom)
      geom.Segmentize(step)
      self._check_geometry(geom, expected_geom)

  def test_ogr_geom_value(self):
    geom = ogr.CreateGeometryFromWkt('LINESTRING(2 3,5 3,5 0)')

    self._check_geometry(geom.Value(-1e-3), 'POINT (2 3)')
    self._check_geometry(geom.Value(geom.Length() / 4), 'POINT (3.5 3)')
    self._check_geometry(geom.Value(geom.Length() / 2), 'POINT (5 3)')
    self._check_geometry(geom.Value(3 * geom.Length() / 4), 'POINT (5 1.5)')
    self._check_geometry(geom.Value(geom.Length() + 1e-3), 'POINT (5 0)')

  def test_ogr_geom_flattenTo2D(self):
    geom = ogr.CreateGeometryFromWkt('POINT (1 2 3)')

    # Point is 0 dimension, LineString 1, ...
    self.assertEqual(geom.GetDimension(), 0)
    self.assertEqual(geom.GetCoordinateDimension(), 3)

    geom.FlattenTo2D()
    self.assertEqual(geom.GetCoordinateDimension(), 2)
    self.assertEqual(geom.ExportToWkt(), 'POINT (1 2)')

  def test_ogr_geom_flattenTo2D_triangle(self):
    geom = ogr.CreateGeometryFromWkt(
        'TRIANGLE ((0 0 0,100 0 100,0 100 100,0 0 0))'
    )

    self.assertEqual(geom.GetDimension(), 2)
    self.assertEqual(geom.GetCoordinateDimension(), 3)

    geom.FlattenTo2D()
    self.assertEqual(geom.GetCoordinateDimension(), 2)
    self.assertEqual(geom.ExportToWkt(), 'TRIANGLE ((0 0,100 0,0 100,0 0))')

  def test_ogr_geom_linestring_limits(self):
    geom = ogr.CreateGeometryFromWkt('LINESTRING EMPTY')
    self.assertEqual(geom.Length(), 0)

    with self.assertRaises(RuntimeError):
      geom.GetPoint(-1)
    with self.assertRaises(RuntimeError):
      geom.GetPoint(0)
    with self.assertRaises(RuntimeError):
      geom.GetPoint_2D(-1)
    with self.assertRaises(RuntimeError):
      geom.GetPoint_2D(0)
    with self.assertRaises(RuntimeError):
      geom.SetPoint(-1, 5, 6, 7)
    with self.assertRaises(RuntimeError):
      geom.SetPoint_2D(-1, 5, 6)
    with self.assertRaises(RuntimeError):
      geom.SetPoint((1 << 31) - 2, 5, 6, 7)
    with self.assertRaises(RuntimeError):
      geom.SetPoint_2D((1 << 31) - 2, 5, 6)
    with self.assertRaises(RuntimeError):
      geom.SetPoint_2D((1 << 31) - 1, 5, 6)

    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 0)')
    self.assertEqual(geom.Length(), 0)
    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 0, 1 0)')
    self.assertEqual(geom.Length(), 1)

  def test_ogr_geom_coord_round(self):
    geom = ogr.CreateGeometryFromWkt('POINT(370441.860 5591000.590)')
    self.assertEqual(geom.ExportToWkt(), 'POINT (370441.86 5591000.59)')

  def test_ogr_geom_coord_round_2(self):
    geom = ogr.CreateGeometryFromWkt('POINT(1.0 169.600374)')
    self.assertEqual(geom.ExportToWkt(), 'POINT (1.0 169.600374)')

  def test_ogr_geom_area_point(self):
    geom = ogr.CreateGeometryFromWkt('POINT(0 0)')
    with gdal.quiet_errors():
      area = geom.Area()
    self.assertEqual(area, 0)

  def test_ogr_geom_length_point(self):
    geom = ogr.CreateGeometryFromWkt('POINT(0 0)')
    with gdal.quiet_errors():
      length = geom.Length()
    self.assertEqual(length, 0)

  def test_ogr_geom_length_multilinestring(self):
    geom = ogr.CreateGeometryFromWkt('MULTILINESTRING((0 0,0 1),(0 0,0 1))')
    self.assertAlmostEqual(geom.Length(), 2, delta=0.00000000001)

  def test_ogr_geom_length_geometrycollection(self):
    geom = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION( POLYGON((0 0,0 1,1 1,1 0,0 0)),'
        ' MULTILINESTRING((0 0,0 1),(0 0,0 1)), LINESTRING(0 0,0 1),'
        ' LINESTRING(0 0,0 1), POINT(0 0), GEOMETRYCOLLECTION EMPTY )'
    )
    self.assertAlmostEqual(geom.Length(), 8, delta=0.00000000001)

  def test_ogr_geom_getpoints(self):
    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 1,2 3)')
    self.assertEqual(geom.GetPoints(), [(0.0, 1.0), (2.0, 3.0)])
    self.assertEqual(
        geom.GetPoints(nCoordDimension=3), [(0.0, 1.0, 0.0), (2.0, 3.0, 0.0)]
    )

    geom = ogr.CreateGeometryFromWkt('LINESTRING(0 1 2,3 4 5)')
    self.assertEqual(geom.GetPoints(), [(0.0, 1.0, 2.0), (3.0, 4.0, 5.0)])
    self.assertEqual(
        geom.GetPoints(nCoordDimension=2), [(0.0, 1.0), (3.0, 4.0)]
    )

    geom = ogr.CreateGeometryFromWkt('POINT(0 1)')
    self.assertEqual(geom.GetPoints(), [(0.0, 1.0)])
    self.assertEqual(geom.GetPoints(nCoordDimension=3), [(0.0, 1.0, 0.0)])

    geom = ogr.CreateGeometryFromWkt('POINT(0 1 2)')
    self.assertEqual(geom.GetPoints(), [(0.0, 1.0, 2.0)])
    self.assertEqual(geom.GetPoints(nCoordDimension=2), [(0.0, 1.0)])

  @parameterized.named_parameters(
      ('2d_compoundcurve', 'COMPOUNDCURVE(CIRCULARSTRING (0 0, 1 1, 2 0))'),
      (
          '3d_compoundcurve',
          'COMPOUNDCURVE(CIRCULARSTRINGZ (0 0 0, 1 1 0, 2 0 0))',
      ),
  )
  def test_ogr_geom_getpoints_failure(self, wkt):
    geom = ogr.CreateGeometryFromWkt(wkt)
    with self.assertRaisesRegex(RuntimeError, 'Incompatible geometry'):
      geom.GetPoints()
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertIsNone(geom.GetPoints())

  def test_ogr_geom_empty(self):
    g1 = ogr.CreateGeometryFromWkt('POLYGON((0 0,1 1,1 2,1 1,0 0))')
    g1.Empty()
    self.assertEqual(g1.ExportToWkt(), 'POLYGON EMPTY')

  def test_ogr_geom_empty_triangle(self):
    g1 = ogr.CreateGeometryFromWkt('TRIANGLE ((0 0,100 0,0 100,0 0))')
    g1.Empty()
    self.assertEqual(g1.ExportToWkt(), 'TRIANGLE EMPTY')

  def test_ogr_geom_mixed_coordinate_dimension(self):
    # First part is 3D, second part is 2D of same length.
    wkt = (
        'MULTIPOLYGON (((1 2 -4,1 3 -3,2 3 -3,2 2 -3,1 2 -6)),'
        '((-1 -2,-1 -3,-2 -3,-2 -2,-1 -2,50 60)))'
    )
    expected_wkt = (
        'MULTIPOLYGON (((1 2 -4,1 3 -3,2 3 -3,2 2 -3,1 2 -6)),'
        '((-1 -2 0,-1 -3 0,-2 -3 0,-2 -2 0,-1 -2 0,50 60 0)))'
    )
    g = ogr.CreateGeometryFromWkt(wkt)
    self.assertEqual(g.ExportToWkt(), expected_wkt)

    # First part is 3D, second part is 2D of same length, except the last point
    # which is 3D.
    wkt = (
        'MULTIPOLYGON (((1 2 -4,1 3 -3,2 3 -3,2 2 -3,1 2 -6)),'
        '((-1 -2,-1 -3,-2 -3,-2 -2,-1 -2,50 60 7)))'
    )
    expected_wkt = (
        'MULTIPOLYGON (((1 2 -4,1 3 -3,2 3 -3,2 2 -3,1 2 -6)),'
        '((-1 -2 0,-1 -3 0,-2 -3 0,-2 -2 0,-1 -2 0,50 60 7)))'
    )
    g = ogr.CreateGeometryFromWkt(wkt)
    self.assertEqual(g.ExportToWkt(), expected_wkt)

  def test_ogr_geom_getenvelope3d(self):
    g = ogr.CreateGeometryFromWkt('POINT EMPTY')
    self.assertEqual(g.GetEnvelope3D(), (0.0, 0.0, 0.0, 0.0, 0.0, 0.0))

    g = ogr.CreateGeometryFromWkt('POINT(1 2)')
    self.assertEqual(g.GetEnvelope3D(), (1.0, 1.0, 2.0, 2.0, 0.0, 0.0))

    g = ogr.CreateGeometryFromWkt('POINT(1 2 3)')
    self.assertEqual(g.GetEnvelope3D(), (1.0, 1.0, 2.0, 2.0, 3.0, 3.0))

    g = ogr.CreateGeometryFromWkt('POLYGON EMPTY')
    self.assertEqual(g.GetEnvelope3D(), (0.0, 0.0, 0.0, 0.0, 0.0, 0.0))

    g = ogr.CreateGeometryFromWkt('LINESTRING(1 2,3 4)')
    self.assertEqual(g.GetEnvelope3D(), (1, 3, 2, 4, 0, 0))

    g = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON(((1 2 3,-1 2 3,-1 -2 3,-1 2 3,1 2 3),'
        '(0.1 0.2 0.3,-0.1 0.2 0.3,-0.1 -0.2 0.3,-0.1 0.2 0.3,0.1 0.2 0.3)),'
        '((10 20 -30,-10 20 -30,-10 -20 -30,-10 20 -30,10 20 -30)))'
    )
    self.assertEqual(g.GetEnvelope3D(), (-10, 10, -20, 20, -30, 3.0))

    g = ogr.CreateGeometryFromWkt('MULTIPOLYGON EMPTY')
    self.assertEqual(g.GetEnvelope3D(), (0.0, 0.0, 0.0, 0.0, 0.0, 0.0))

    g = ogr.CreateGeometryFromWkt(
        'TRIANGLE ((0 0 0,100 0 100,0 100 100,0 0 0))'
    )
    self.assertEqual(g.GetEnvelope3D(), (0.0, 100.0, 0.0, 100.0, 0.0, 100.0))

  def test_ogr_geom_z_empty(self):
    for geom in (
        'POINT',
        'LINESTRING',
        'POLYGON',
        'MULTIPOINT',
        'MULTILINESTRING',
        'MULTIPOLYGON',
        'GEOMETRYCOLLECTION',
        'CIRCULARSTRING',
        'COMPOUNDCURVE',
        'CURVEPOLYGON',
        'MULTICURVE',
        'MULTISURFACE',
    ):
      in_wkt = geom + ' Z EMPTY'
      g1 = ogr.CreateGeometryFromWkt(in_wkt)
      self.assertEqual(g1.ExportToIsoWkt(), in_wkt)

      g2 = ogr.CreateGeometryFromWkb(g1.ExportToIsoWkb())
      self.assertEqual(g2.ExportToIsoWkt(), in_wkt)

  def test_ogr_geom_getlineargeometry(self):
    for geom_spec in (
        'POINT',
        'LINESTRING',
        'POLYGON',
        'MULTIPOINT',
        'MULTILINESTRING',
        'MULTIPOLYGON',
        'GEOMETRYCOLLECTION',
        ('CIRCULARSTRING', 'LINESTRING'),
        ('COMPOUNDCURVE', 'LINESTRING'),
        ('CURVEPOLYGON', 'POLYGON'),
        ('MULTICURVE', 'MULTILINESTRING'),
        ('MULTISURFACE', 'MULTIPOLYGON'),
    ):
      if isinstance(geom_spec, tuple):
        geom_in, geom_out = geom_spec
      else:
        geom_in = geom_out = geom_spec
      in_wkt = geom_in + ' EMPTY'
      g = ogr.CreateGeometryFromWkt(in_wkt)
      g2 = g.GetLinearGeometry()
      if geom_in != geom_out:
        self.assertTrue(g.HasCurveGeometry())
      self.assertEqual(g2.ExportToWkt(), geom_out + ' EMPTY')

    g = ogr.CreateGeometryFromWkt('GEOMETRYCOLLECTION(POINT (0 1))')
    self.assertFalse(g.HasCurveGeometry())
    self.assertFalse(g.HasCurveGeometry(True))

    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(CIRCULARSTRING (0 0,0 1,0 0))'
    )
    self.assertTrue(g.HasCurveGeometry())
    self.assertTrue(g.HasCurveGeometry(True))

  def test_ogr_geom_getdimension(self):
    for geom, dim in (
        ('POINT EMPTY', 0),
        ('LINESTRING EMPTY', 1),
        ('POLYGON EMPTY', 2),
        ('MULTIPOINT EMPTY', 0),
        ('MULTILINESTRING EMPTY', 1),
        ('MULTIPOLYGON EMPTY', 2),
        ('GEOMETRYCOLLECTION EMPTY', 0),
        ('CIRCULARSTRING EMPTY', 1),
        ('COMPOUNDCURVE EMPTY', 1),
        ('CURVEPOLYGON EMPTY', 2),
        ('MULTICURVE EMPTY', 1),
        ('TRIANGLE EMPTY', 2),
        ('MULTISURFACE EMPTY', 2),
        ('POLYHEDRALSURFACE EMPTY', 2),
        ('TIN EMPTY', 2),
    ):
      g = ogr.CreateGeometryFromWkt(geom)
      self.assertEqual(g.GetDimension(), dim)

    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(LINESTRING EMPTY, POINT (0 1))'
    )
    self.assertEqual(g.GetDimension(), 1)

    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(POINT (0 1), LINESTRING EMPTY, POLYGON EMPTY)'
    )
    self.assertEqual(g.GetDimension(), 2)

  def test_ogr_geom_triangle(self):
    wkt_original = 'TRIANGLE ((0 0,0 1,1 1,0 0))'
    geom = ogr.CreateGeometryFromWkt(wkt_original)

    wkb_bytes = geom.ExportToWkb(ogr.wkbXDR)
    geom = ogr.CreateGeometryFromWkb(wkb_bytes)
    self.assertEqual(geom.ExportToWkt(), wkt_original)

    self.assertEqual(geom.Clone().ExportToWkt(), wkt_original)

    polygon_wkt = ogr.ForceTo(geom.Clone(), ogr.wkbPolygon).ExportToWkt()
    self.assertEqual(polygon_wkt, 'POLYGON ((0 0,0 1,1 1,0 0))')

    polygon_wkt = ogr.ForceTo(geom.Clone(), ogr.wkbMultiPolygon).ExportToWkt()
    self.assertEqual(polygon_wkt, 'MULTIPOLYGON (((0 0,0 1,1 1,0 0)))')

  def test_ogr_geom_triangle_invalid_wkt(self):
    for geom_wkt in (
        'TRIANGLE (0 0)',
        'TRIANGLE ((0 0))',
        'TRIANGLE ((0 0,0 1,1 1,1 0))',
        'TRIANGLE ((0 0,0 1,1 1,1 0,0 0))',
        'TRIANGLE ((0 0,0 1,1 1,0 0),(0 0,0 1,1 1,0 0))',
    ):
      with self.assertRaises(RuntimeError):
        ogr.CreateGeometryFromWkt(geom_wkt)

  def test_ogr_geom_triangle_sfcgal(self):
    if not _have_sfcgal():
      self.skipTest('SFCGAL is not available')

    g1 = ogr.CreateGeometryFromWkt('TRIANGLE ((0 0,100 0 100,0 100 100,0 0))')
    g2 = ogr.CreateGeometryFromWkt(
        'TRIANGLE ((-1 -1,100 0 100,0 100 100,-1 -1))'
    )
    self.assertTrue(g2.Intersects(g1))

    g1 = ogr.CreateGeometryFromWkt('TRIANGLE ((0 0,1 0,0 1,0 0))')
    g2 = ogr.CreateGeometryFromWkt('TRIANGLE ((0 0,1 0,1 1,0 0))')
    g3 = g1.Intersection(g2)
    g4 = ogr.CreateGeometryFromWkt(
        'TRIANGLE ((0.5 0.5 0,0 0 0,1 0 0,0.5 0.5 0))'
    )
    self.assertTrue(g4.Equals(g3))

  def test_ogr_geom_circularstring(self):
    in_wkt = 'CIRCULARSTRING (0 0,1 1,1 -1)'

    # Test export/import Wkt
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbCircularString)
    self.assertEqual(g1.ExportToWkt(), in_wkt)

    if _have_geos():
      self.assertTrue(g1.IsValid())

    # Test GetEnvelope()
    env = g1.GetEnvelope()
    expected_env = (0.0, 2.0, -1.0, 1.0)
    for i in range(4):
      self.assertAlmostEqual(env[i], expected_env[i], delta=1e-8)

    # Test Length()
    length = g1.Length()
    self.assertAlmostEqual(length, 1.5 * math.pi, delta=1e-8)

    # Test Value()
    self._check_geometry(g1.Value(-1), 'POINT (0 0)')
    self._check_geometry(g1.Value(0), 'POINT (0 0)')
    self._check_geometry(
        g1.Value(length / 6.0), 'POINT (0.292893218813453 0.707106781186548)'
    )
    self._check_geometry(g1.Value(length / 3.0), 'POINT (1 1)')
    self._check_geometry(
        g1.Value(length / 2.0), 'POINT (1.707106781186547 0.707106781186547)'
    )
    self._check_geometry(g1.Value(2 * length / 3.0), 'POINT (2 0)')
    self._check_geometry(g1.Value(length), 'POINT (1 -1)')
    self._check_geometry(g1.Value(length + 1), 'POINT (1 -1)')

    # Test export/import Wkb
    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # With Z
    in_wkt = 'CIRCULARSTRING Z (0 0 10,1 1 20,2 0 30)'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbCircularStringZ)
    self.assertEqual(g1.ExportToWkt(), in_wkt)

    wkb = g1.ExportToWkb()
    isowkb = g1.ExportToIsoWkb()
    self.assertEqual(wkb, isowkb)
    g2 = ogr.CreateGeometryFromWkb(wkb)
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Test stroking
    with gdal.config_options(
        {'OGR_STROKE_CURVE': 'TRUE', 'OGR_ARC_STEPSIZE': '45'}
    ):
      in_wkt = 'CIRCULARSTRING (0 0,1 1,1 -1)'
      g1 = ogr.CreateGeometryFromWkt(in_wkt)

    expected_g = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,0.218168517531969 0.623489801858729,'
        '0.777479066043687 0.974927912181831,'
        '1.433883739117561 0.900968867902435,'
        '1.900968867902463 0.433883739117562,'
        '1.974927912181821 -0.222520933956316,'
        '1.623489801858719 -0.78183148246804,1 -1)'
    )
    self._check_geometry(g1, expected_g)

    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1 1,1 -1)')
    in_wkb = g1.ExportToWkb()

    with gdal.config_options(
        {'OGR_STROKE_CURVE': 'TRUE', 'OGR_ARC_STEPSIZE': '45'}
    ):
      g2 = ogr.CreateGeometryFromWkb(in_wkb)
    self._check_geometry(g2, expected_g)

    # Test HasCurveGeometry
    self.assertTrue(g1.HasCurveGeometry())
    self.assertTrue(g1.HasCurveGeometry(True))

    # Test GetLinearGeometry
    self._check_geometry(g1.GetLinearGeometry(45), expected_g)

    # Test ForceToLineString
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToLineString(g1)
    self._check_geometry(g2, expected_g)

    # Test ForceToMultiLineString
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      expected_g2 = ogr.CreateGeometryFromWkt(
          'MULTILINESTRING ((0 0,0.218168517531969 0.623489801858729,'
          '0.777479066043687 0.974927912181831,'
          '1.433883739117561 0.900968867902435,'
          '1.900968867902463 0.433883739117562,'
          '1.974927912181821 -0.222520933956316,'
          '1.623489801858719 -0.78183148246804,1 -1))'
      )
      g2 = ogr.ForceToMultiLineString(g1)
    self._check_geometry(g2, expected_g2)

    # Test GEOS operations
    if _have_geos():
      self.assertIsNotNone(g1.Intersection(g1))

    # Test CIRCULARSTRING wrapped in a GEOMETRYCOLLECTION
    g1 = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(CIRCULARSTRING (0 0,1 1,1 -1))'
    )

    if _have_geos():
      self.assertIsNotNone(g1.Intersection(g1))

    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToLineString(g1)
    self._check_geometry(g2, expected_g)

    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToMultiLineString(g1)
    self._check_geometry(g2, expected_g2)

    # Test stroking of full circle with 3 points (CCW order like PostGIS).
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1 0,0 0)')
    expected_g = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,0.116977778440514 -0.321393804843282,'
        '0.413175911166547 -0.49240387650611,0.75 -0.433012701892224,'
        '0.969846310392967 -0.171010071662835,'
        '0.969846310392967 0.171010071662835,0.75 0.433012701892224,'
        '0.413175911166547 0.49240387650611,'
        '0.116977778440514 0.321393804843282,0 0)'
    )
    self._check_geometry(g1.GetLinearGeometry(45), expected_g)
    self.assertAlmostEqual(g1.Length(), 2 * math.pi * 0.5, delta=1e-8)

    # Test stroking of full circle (well defined)
    g1 = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (0 0,0.5 0.5,1.0 0.0,0.5 -0.5,0.0 0.0)'
    )
    expected_g = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,0.049515566048791 0.216941869558781,'
        '0.188255099070638 0.390915741234018,'
        '0.388739533021848 0.48746395609092,'
        '0.611260466978166 0.48746395609092,'
        '0.811744900929369 0.390915741234018,'
        '0.950484433951232 0.216941869558781,1 0,'
        '0.950484433951232 -0.216941869558781,'
        '0.811744900929369 -0.390915741234018,'
        '0.611260466978166 -0.48746395609092,'
        '0.388739533021848 -0.48746395609092,'
        '0.188255099070638 -0.390915741234018,'
        '0.049515566048791 -0.216941869558781,0 0)'
    )
    self._check_geometry(g1.GetLinearGeometry(45), expected_g)
    self.assertAlmostEqual(g1.Length(), 2 * math.pi * 0.5, delta=1e-8)

    # Check segmentize
    g1.Segmentize(0.5)
    expected_g = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (0 0,0.146446609406726 0.353553390593274,0.5 0.5,'
        '0.853553390593274 0.353553390593274,1 0,'
        '0.853553390593274 -0.353553390593274,0.5 -0.5,'
        '0.146446609406726 -0.353553390593274,0 0)'
    )
    self._check_geometry(g1, expected_g)
    self.assertAlmostEqual(g1.Length(), 2 * math.pi * 0.5, delta=1e-8)

    # Check segmentize symmetry: do exact binary comparison
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1.2 1,2 0)')
    g1.Segmentize(0.25)
    g2 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (2 0,1.2 1,0 0)')
    g2.Segmentize(0.25)
    for i in range(g1.GetPointCount()):
      self.assertEqual(g1.GetPoint(i), g2.GetPoint(g1.GetPointCount() - 1 - i))

    # Test stroking of full circle with Z
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0 1,1 0 2,0 0 1)')
    expected_g = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0 1,0.116977778440514 -0.321393804843282 1,'
        '0.413175911166547 -0.49240387650611 1,0.75 -0.433012701892224 1,'
        '0.969846310392967 -0.171010071662835 1,'
        '0.969846310392967 0.171010071662835 1,0.75 0.433012701892224 1,'
        '0.413175911166547 0.49240387650611 1,'
        '0.116977778440514 0.321393804843282 1,0 0 1)'
    )
    self._check_geometry(g1.GetLinearGeometry(45), expected_g)

    g1.Segmentize(0.5)
    expected_g = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING Z (0 0 1,0.146446609406726 -0.353553390593274 1.25,'
        '0.5 -0.5 1.5,0.853553390593274 -0.353553390593274 1.75,1 0 2,'
        '0.853553390593274 0.353553390593274 1.75,0.5 0.5 1.5,'
        '0.146446609406727 0.353553390593274 1.25,0 0 1)'
    )
    self._check_geometry(g1, expected_g)

    # Same as above but reverse order
    g1 = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (0 0,0.5 -0.5,1.0 0.0,0.5 0.5,0.0 0.0)'
    )
    expected_g = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,0.049515566048791 -0.216941869558781,'
        '0.188255099070638 -0.390915741234018,'
        '0.388739533021848 -0.48746395609092,'
        '0.611260466978166 -0.48746395609092,'
        '0.811744900929369 -0.390915741234018,'
        '0.950484433951232 -0.216941869558781,1 0,'
        '0.950484433951232 0.216941869558781,'
        '0.811744900929369 0.390915741234018,'
        '0.611260466978166 0.48746395609092,'
        '0.388739533021848 0.48746395609092,'
        '0.188255099070638 0.390915741234018,'
        '0.049515566048791 0.216941869558781,0 0)'
    )
    self._check_geometry(g1.GetLinearGeometry(45), expected_g)
    self.assertAlmostEqual(g1.Length(), 2 * math.pi * 0.5, delta=1e-8)

    # Test stroking of a circular string with 3 colinear points
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1 1,2 2)')
    self._check_geometry(g1.GetLinearGeometry(45), 'LINESTRING (0 0,1 1,2 2)')
    length = g1.Length()
    self.assertAlmostEqual(length, 2 * math.sqrt(2), delta=1e-8)

    self._check_geometry(g1.Value(length / 4.0), 'POINT (0.5 0.5)')
    self._check_geometry(g1.Value(3.0 * length / 4.0), 'POINT (1.5 1.5)')

    g1.Segmentize(0.5)
    expected_g = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (0 0,0.166666666666667 0.166666666666667,'
        '0.333333333333333 0.333333333333333,0.5 0.5,'
        '0.666666666666667 0.666666666666667,'
        '0.833333333333333 0.833333333333333,1 1,'
        '1.166666666666667 1.166666666666667,'
        '1.333333333333333 1.333333333333333,1.5 1.5,'
        '1.666666666666667 1.666666666666667,'
        '1.833333333333333 1.833333333333333,2 2)'
    )
    self._check_geometry(g1, expected_g)
    self.assertAlmostEqual(g1.Length(), 2 * math.sqrt(2), delta=1e-8)

    # Same with Z
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0 1,1 1 2,2 2 1)')
    self._check_geometry(
        g1.GetLinearGeometry(45), 'LINESTRING (0 0 1,1 1 2,2 2 1)'
    )
    g1.Segmentize(0.5)
    expected_g = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING Z (0 0 1,'
        '0.166666666666667 0.166666666666667 1.166666666666667,'
        '0.333333333333333 0.333333333333333 1.333333333333333,0.5 0.5 1.5,'
        '0.666666666666667 0.666666666666667 1.666666666666667,'
        '0.833333333333333 0.833333333333333 1.833333333333333,1 1 2,'
        '1.166666666666667 1.166666666666667 1.833333333333333,'
        '1.333333333333333 1.333333333333333 1.666666666666667,1.5 1.5 1.5,'
        '1.666666666666667 1.666666666666667 1.333333333333333,'
        '1.833333333333333 1.833333333333333 1.166666666666667,2 2 1)'
    )
    self._check_geometry(g1, expected_g)
    self._check_geometry(g1.Value(length / 4.0), 'POINT (0.5 0.5 1.5)')
    self._check_geometry(g1.Value(3.0 * length / 4.0), 'POINT (1.5 1.5 1.5)')

    # Test GetEnvelope() in various cases
    cx = 1
    cy = 2
    r = 3
    envelope_cases = (
        (
            math.pi / 3,
            math.pi / 4,
            math.pi / 6,
            (
                cx + r * math.cos(math.pi / 3),
                cx + r * math.cos(math.pi / 6),
                cy + r * math.sin(math.pi / 6),
                cy + r * math.sin(math.pi / 3),
            ),
        ),
        (
            math.pi / 3,
            math.pi / 6,
            -math.pi / 6,
            (
                cx + r * math.cos(math.pi / 3),
                cx + r,
                cy + r * math.sin(-math.pi / 6),
                cy + r * math.sin(math.pi / 3),
            ),
        ),
        (
            math.pi / 3,
            math.pi - math.pi / 3,
            -math.pi / 6,
            (cx - r, cx + r * math.cos(-math.pi / 6), cy - r, cy + r),
        ),
        (
            math.pi / 3,
            math.pi - math.pi / 3,
            math.pi / 6,
            (cx - r, cx + r, cy - r, cy + r),
        ),
        (
            math.pi / 3,
            math.pi + math.pi / 3,
            math.pi / 3,
            (cx - r, cx + r, cy - r, cy + r),
        ),
    )
    for a0, a1, a2, expected_env in envelope_cases:
      in_wkt = (
          f'CIRCULARSTRING({cx + r * math.cos(a0):.16g}'
          f' {cy + r * math.sin(a0):.16g},'
          f'{cx + r * math.cos(a1):.16g} {cy + r * math.sin(a1):.16g},'
          f'{cx + r * math.cos(a2):.16g} {cy + r * math.sin(a2):.16g})'
      )
      g1 = ogr.CreateGeometryFromWkt(in_wkt)
      env = g1.GetEnvelope()
      for i in range(4):
        self.assertAlmostEqual(env[i], expected_env[i], delta=1e-8)

    # Error case: not enough points
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertIsNone(ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0)'))

    # EMPTY
    for in_wkt in ('CIRCULARSTRING EMPTY', 'CIRCULARSTRING Z EMPTY'):
      g1 = ogr.CreateGeometryFromWkt(in_wkt)
      self.assertEqual(g1.ExportToWkt(), in_wkt)
      self.assertEqual(g1.Length(), 0.0)
      g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
      self.assertEqual(g2.ExportToWkt(), in_wkt)

  def test_ogr_geom_compoundcurve(self):
    in_wkt = 'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,1 -1))'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbCompoundCurve)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    self.assertEqual(g1.GetGeometryCount(), 1)
    self.assertEqual(
        g1.GetGeometryRef(0).ExportToWkt(), 'CIRCULARSTRING (0 0,1 1,1 -1)'
    )

    env = g1.GetEnvelope()
    expected_env = (0.0, 2.0, -1.0, 1.0)
    for i in range(4):
      self.assertAlmostEqual(env[i], expected_env[i], delta=1e-8)

    self.assertAlmostEqual(g1.Length(), 1.5 * math.pi, delta=1e-8)

    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertTrue(g1.HasCurveGeometry())
    self.assertTrue(g1.HasCurveGeometry(True))

    # CreateGeometryFromWkt of LINESTRING
    g1 = ogr.CreateGeometryFromWkt('COMPOUNDCURVE ((0 0,0 10))')
    self.assertTrue(g1.HasCurveGeometry())
    self.assertFalse(g1.HasCurveGeometry(True))

    # With Z
    in_wkt = (
        'COMPOUNDCURVE Z (CIRCULARSTRING Z (0 0 10,1 1 20,2 0 30),'
        '(2 0 30,0 0 10))'
    )
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbCompoundCurveZ)
    self.assertEqual(g1.ExportToWkt(), in_wkt)

    env = g1.GetEnvelope()
    expected_env = (0.0, 2.0, 0.0, 1.0)
    for i in range(4):
      self.assertAlmostEqual(env[i], expected_env[i], delta=1e-8)

    env3d = g1.GetEnvelope3D()
    expected_env3d = (0.0, 2.0, 0.0, 1.0, 10, 30)
    for i in range(6):
      self.assertAlmostEqual(env3d[i], expected_env3d[i], delta=1e-8)

    # Test Value()
    self._check_geometry(g1.Value(-1e-3), 'POINT (0 0 10)')
    self._check_geometry(g1.Value(math.pi / 2.0), 'POINT (1 1 20)')
    self._check_geometry(g1.Value(math.pi), 'POINT (2 0 30)')
    self._check_geometry(g1.Value(math.pi + 1), 'POINT (1 0 20)')
    self._check_geometry(g1.Value(math.pi + 2 + 1e-3), 'POINT (0 0 10)')

    wkb = g1.ExportToWkb()
    isowkb = g1.ExportToIsoWkb()
    self.assertEqual(wkb, isowkb)
    g2 = ogr.CreateGeometryFromWkb(wkb)
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Several parts
    in_wkt = 'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,1 -1),(1 -1,0 0))'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbCompoundCurve)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    self.assertTrue(g1.Equals(g1))
    self.assertTrue(g1.Equals(g1.Clone()))
    self.assertFalse(g1.Equals(ogr.CreateGeometryFromWkt('POINT(0 0)')))
    self.assertFalse(
        g1.Equals(
            ogr.CreateGeometryFromWkt(
                'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,1 -1))'
            )
        )
    )
    self.assertFalse(
        g1.Equals(
            ogr.CreateGeometryFromWkt(
                'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,1 -1),(1 -1,0 1))'
            )
        )
    )
    self.assertAlmostEqual(
        g1.Length(), 1.5 * math.pi + math.sqrt(2), delta=1e-8
    )

    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Test stroking
    with gdal.config_options(
        {'OGR_STROKE_CURVE': 'TRUE', 'OGR_ARC_STEPSIZE': '45'}
    ):
      g1 = ogr.CreateGeometryFromWkt(
          'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,1 -1))'
      )

    expected_g = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,0.218168517531969 0.623489801858729,'
        '0.777479066043687 0.974927912181831,'
        '1.433883739117561 0.900968867902435,'
        '1.900968867902463 0.433883739117562,'
        '1.974927912181821 -0.222520933956316,'
        '1.623489801858719 -0.78183148246804,1 -1)'
    )
    self._check_geometry(g1, expected_g)

    g1 = ogr.CreateGeometryFromWkt(
        'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,1 -1))'
    )
    in_wkb = g1.ExportToWkb()

    with gdal.config_options(
        {'OGR_STROKE_CURVE': 'TRUE', 'OGR_ARC_STEPSIZE': '45'}
    ):
      g2 = ogr.CreateGeometryFromWkb(in_wkb)
    self._check_geometry(g2, expected_g)

    self._check_geometry(g1.GetLinearGeometry(45), expected_g)
    self.assertTrue(g1.HasCurveGeometry())
    self.assertTrue(g1.HasCurveGeometry(True))

    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToLineString(g1)
    self._check_geometry(g2, expected_g)

    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      expected_multiline = ogr.CreateGeometryFromWkt(
          'MULTILINESTRING ((0 0,0.218168517531969 0.623489801858729,'
          '0.777479066043687 0.974927912181831,'
          '1.433883739117561 0.900968867902435,'
          '1.900968867902463 0.433883739117562,'
          '1.974927912181821 -0.222520933956316,'
          '1.623489801858719 -0.78183148246804,1 -1))'
      )
      g2 = ogr.ForceToMultiLineString(g1)
    self._check_geometry(g2, expected_multiline)

    # Check segmentize
    g1.Segmentize(0.5)
    expected_seg = ogr.CreateGeometryFromWkt(
        'COMPOUNDCURVE (CIRCULARSTRING (0 0,'
        '0.076120467488713 0.38268343236509,'
        '0.292893218813453 0.707106781186548,'
        '0.61731656763491 0.923879532511287,1 1,'
        '1.38268343236509 0.923879532511287,'
        '1.707106781186547 0.707106781186547,'
        '1.923879532511287 0.38268343236509,2 0,'
        '1.923879532511287 -0.38268343236509,'
        '1.707106781186547 -0.707106781186547,'
        '1.38268343236509 -0.923879532511287,1 -1))'
    )
    self._check_geometry(g1, expected_seg)

    # Error cases
    for bad_wkt in (
        'COMPOUNDCURVE ((0 0))',
        'COMPOUNDCURVE (COMPOUNDCURVE EMPTY)',
        'COMPOUNDCURVE ((0 0,1 1),(2 2,3 3))',
        'COMPOUNDCURVE (EMPTY,(2 2,3 3))',
        'COMPOUNDCURVE ((2 2,3 3), EMPTY)',
    ):
      with _disable_exceptions(), gdal.quiet_errors():
        self.assertIsNone(ogr.CreateGeometryFromWkt(bad_wkt), bad_wkt)

    g = ogr.Geometry(ogr.wkbCompoundCurve)
    g.AddGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
    self.assertEqual(g.ExportToWkt(), 'COMPOUNDCURVE ((0 0,1 1))')

    with _disable_exceptions(), gdal.quiet_errors():
      g.AddGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
    self.assertEqual(g.ExportToWkt(), 'COMPOUNDCURVE ((0 0,1 1),(1 1,0 0))')

    g = ogr.Geometry(ogr.wkbCompoundCurve)
    g.AddGeometryDirectly(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
    self.assertEqual(g.ExportToWkt(), 'COMPOUNDCURVE ((0 0,1 1))')

    with _disable_exceptions(), gdal.quiet_errors():
      g.AddGeometryDirectly(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
    self.assertEqual(g.ExportToWkt(), 'COMPOUNDCURVE ((0 0,1 1),(1 1,0 0))')

    # Cannot add compoundcurve in compoundcurve
    g = ogr.Geometry(ogr.wkbCompoundCurve)
    with _disable_exceptions(), gdal.quiet_errors():
      g.AddGeometryDirectly(
          ogr.CreateGeometryFromWkt('COMPOUNDCURVE((1 1,2 2))')
      )
    self.assertEqual(g.ExportToWkt(), 'COMPOUNDCURVE EMPTY')

    # Check that discretization is not sensitive to winding order (strict
    # binary equality intended).
    g1 = ogr.CreateGeometryFromWkt(
        'COMPOUNDCURVE((-1 0,0 1),CIRCULARSTRING (0 1,0.25 0,0.1 -0.5),'
        '(0.1 -0.5,-1 0))'
    )
    g2 = g1.GetLinearGeometry(1.5)
    g1 = ogr.CreateGeometryFromWkt(
        'COMPOUNDCURVE((-1 0,0.1 -0.5),CIRCULARSTRING (0.1 -0.5,0.25 0,0 1),'
        '(0 1,-1 0))'
    )
    g3 = g1.GetLinearGeometry(1.5)
    p_count = g2.GetPointCount()
    for i in range(p_count):
      self.assertEqual(g2.GetX(i), g3.GetX(p_count - 1 - i))
      self.assertEqual(g2.GetY(i), g3.GetY(p_count - 1 - i))

    # Test Transform
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    g.AssignSpatialReference(sr)
    g.TransformTo(sr)

    # Invalid wkb
    wkb_list = (
        b'\x01\x09\x00\x00\x00\x01\x00\x00\x00',
        b'\x01\x09\x00\x00\x00\xff\xff\xff\x7f',
        b'\x01\x09\x00\x00\x00\x01\x00\x00\x00\x01\xff\x00\x00\x00\x00\x00\x00\x00',
        b'\x01\x09\x00\x00\x00\x01\x00\x00\x00\x01\x02\x00\x00\x00\x01\x00\x00\x00',
        (
            b'\x01\x09\x00\x00\x00\x01\x00\x00\x00\x01\x02\x00\x00\x00\x01\x00'
            b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
            b'\x00\x00'
        ),
        (
            b'\x01\x09\x00\x00\x00\x01\x00\x00\x00\x01\x01\x00\x00\x00\x00\x00'
            b'\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
        ),
        b'\x01\x09\x00\x00\x00\x01\x00\x00\x00\x01\x09\x00\x00\x00\x00\x00\x00\x00',
    )
    for wkb in wkb_list:
      with _disable_exceptions(), gdal.quiet_errors():
        self.assertIsNone(ogr.CreateGeometryFromWkb(wkb), repr(wkb))

    # EMPTY
    for in_wkt in ('COMPOUNDCURVE EMPTY', 'COMPOUNDCURVE Z EMPTY'):
      g1 = ogr.CreateGeometryFromWkt(in_wkt)
      self.assertEqual(g1.ExportToWkt(), in_wkt)
      self.assertEqual(g1.Length(), 0.0)
      g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
      self.assertEqual(g2.ExportToWkt(), in_wkt)

  def test_ogr_geom_curvepolygon(self):
    in_wkt = 'CURVEPOLYGON (CIRCULARSTRING (0 0,1 0,0 0))'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbCurvePolygon)
    self.assertEqual(g1.ExportToWkt(), in_wkt)

    if _have_geos():
      p1 = g1.PointOnSurface()
      self.assertLessEqual(
          (p1.GetX() - 0.5) * (p1.GetX() - 0.5) + p1.GetY() * p1.GetY(),
          0.5 * 0.5,
      )

    env = g1.GetEnvelope()
    expected_env = (0.0, 1.0, -0.5, 0.5)
    for i in range(4):
      self.assertAlmostEqual(env[i], expected_env[i], delta=1e-8)

    self.assertAlmostEqual(g1.Area(), math.pi * 0.5 * 0.5, delta=1e-8)

    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # CURVEPOLYGON of LINESTRING
    g1 = ogr.CreateGeometryFromWkt('CURVEPOLYGON ((0 0,0 10,10 10,10 0,0 0))')
    self.assertTrue(g1.HasCurveGeometry())
    self.assertFalse(g1.HasCurveGeometry(True))

    # With Z
    in_wkt = 'CURVEPOLYGON Z (CIRCULARSTRING Z (0 0 10,1 0 10,0 0 10))'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbCurvePolygonZ)
    self.assertEqual(g1.ExportToWkt(), in_wkt)

    wkb = g1.ExportToWkb()
    isowkb = g1.ExportToIsoWkb()
    self.assertEqual(wkb, isowkb)
    g2 = ogr.CreateGeometryFromWkb(wkb)
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Test stroking
    with gdal.config_options(
        {'OGR_STROKE_CURVE': 'TRUE', 'OGR_ARC_STEPSIZE': '45'}
    ):
      g1 = ogr.CreateGeometryFromWkt(
          'CURVEPOLYGON (CIRCULARSTRING (0 0,1 0,0 0))'
      )

    expected_g = ogr.CreateGeometryFromWkt(
        'POLYGON ((0 0,0.116977778440514 -0.321393804843282,'
        '0.413175911166547 -0.49240387650611,0.75 -0.433012701892224,'
        '0.969846310392967 -0.171010071662835,'
        '0.969846310392967 0.171010071662835,0.75 0.433012701892224,'
        '0.413175911166547 0.49240387650611,'
        '0.116977778440514 0.321393804843282,0 0))'
    )
    self._check_geometry(g1, expected_g)

    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 0,0 0))'
    )
    in_wkb = g1.ExportToWkb()

    with gdal.config_options(
        {'OGR_STROKE_CURVE': 'TRUE', 'OGR_ARC_STEPSIZE': '45'}
    ):
      g2 = ogr.CreateGeometryFromWkb(in_wkb)
    self._check_geometry(g2, expected_g)

    # Test HasCurveGeometry
    self.assertTrue(g1.HasCurveGeometry())
    self.assertTrue(g1.HasCurveGeometry(True))

    # Test GetLinearGeometry
    self._check_geometry(g1.GetLinearGeometry(45), expected_g)

    # Test ForceToPolygon
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToPolygon(g1)
    self._check_geometry(g2, expected_g)

    # Test ForceToMultiPolygon
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      expected_g2 = ogr.CreateGeometryFromWkt(
          'MULTIPOLYGON (((0 0,0.116977778440514 -0.321393804843282,'
          '0.413175911166547 -0.49240387650611,0.75 -0.433012701892224,'
          '0.969846310392967 -0.171010071662835,'
          '0.969846310392967 0.171010071662835,0.75 0.433012701892224,'
          '0.413175911166547 0.49240387650611,'
          '0.116977778440514 0.321393804843282,0 0)))'
      )
      g2 = ogr.ForceToMultiPolygon(g1)
    self._check_geometry(g2, expected_g2)

    # Test ForceToMultiLineString
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToMultiLineString(g1)
    expected_g3 = ogr.CreateGeometryFromWkt(
        'MULTILINESTRING ((0 0,0.116977778440514 -0.321393804843282,'
        '0.413175911166547 -0.49240387650611,0.75 -0.433012701892224,'
        '0.969846310392967 -0.171010071662835,'
        '0.969846310392967 0.171010071662835,0.75 0.433012701892224,'
        '0.413175911166547 0.49240387650611,'
        '0.116977778440514 0.321393804843282,0 0))'
    )
    self._check_geometry(g2, expected_g3)

    # Test GEOS operations
    if _have_geos():
      self.assertIsNotNone(g1.Intersection(g1))

    # Test CURVEPOLYGON and COMPOUNDCURVE, CIRCULARSTRING, LINESTRING
    in_wkt = (
        'CURVEPOLYGON (COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0),(2 0,0 0)),'
        '(0.1 0.1,0.1 0.2,0.2 0.2,0.2 0.1,0.1 0.1),'
        'CIRCULARSTRING (0.25 0.25,0.75 0.25,0.25 0.25))'
    )
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Test CURVEPOLYGON wrapped in a GEOMETRYCOLLECTION
    g1 = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(CURVEPOLYGON(CIRCULARSTRING (0 0,1 0,0 0)))'
    )
    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertTrue(g1.Equals(g2))

    if _have_geos():
      self.assertIsNotNone(g1.Intersection(g1))

    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToPolygon(g1)
    self._check_geometry(g2, expected_g)

    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToMultiPolygon(g1)
    self._check_geometry(g2, expected_g2)

    # Error cases
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertIsNone(
          ogr.CreateGeometryFromWkt('CURVEPOLYGON ((0 0,0 1,0 0))')
      )
      self.assertIsNone(ogr.CreateGeometryFromWkt('CURVEPOLYGON (POINT EMPTY)'))
      with gdal.config_option('OGR_GEOMETRY_ACCEPT_UNCLOSED_RING', 'NO'):
        self.assertIsNone(
            ogr.CreateGeometryFromWkt('CURVEPOLYGON ((0 0,0 1,1 1,1 0))')
        )

    # Area
    for area_wkt in (
        'CURVEPOLYGON ((0 0,1 1,1 0,0 0))',
        'CURVEPOLYGON (COMPOUNDCURVE((0 0,1 1,1 0,0 0)))',
        'CURVEPOLYGON (COMPOUNDCURVE((0 0,1 1),(1 1,1 0,0 0)))',
        (
            'CURVEPOLYGON ((100000000 100000000,100000001 100000001,'
            '100000001 100000000,100000000 100000000))'
        ),
    ):
      g = ogr.CreateGeometryFromWkt(area_wkt)
      self.assertEqual(g.Area(), 0.5)

    # Equals
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON ((0 0,1 1,1 0,0 0),'
        'CIRCULARSTRING(0.75 0.5,0.85 0.5,0.75 0.5))'
    )
    self.assertTrue(g1.Equals(g1))
    self.assertTrue(g1.Equals(g1.Clone()))
    self.assertFalse(g1.Equals(ogr.CreateGeometryFromWkt('POINT(0 0)')))
    self.assertFalse(
        g1.Equals(ogr.CreateGeometryFromWkt('CURVEPOLYGON ((0 0,1 1,1 0,0 0))'))
    )

    # Intersects optimizations on a circle
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (CIRCULARSTRING (0 0,2 0,0 0))'
    )
    p1 = ogr.CreateGeometryFromWkt(
        f'POINT ({1 + math.cos(math.pi / 6) - 1e-4:.16g}'
        f' {math.sin(math.pi / 6):.16g})'
    )
    # To prove that we don't use discretization
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      self.assertTrue(g1.Intersects(p1))
      self.assertTrue(p1.Intersects(g1))
      self.assertTrue(g1.Contains(p1))
      self.assertTrue(p1.Within(g1))

    # Point slightly outside circle
    p2 = ogr.CreateGeometryFromWkt(
        f'POINT ({1 + math.cos(math.pi / 6) + 1e-4:.16g}'
        f' {math.sin(math.pi / 6):.16g})'
    )
    self.assertFalse(p2.Within(g1))

    # Full circle defined by 2 arcs
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 1,2 0,1 -1,0 0))'
    )
    self.assertTrue(p1.Within(g1))

    # Same but in reverse order
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 -1,2 0,1 1,0 0))'
    )
    self.assertTrue(p1.Within(g1))

    # This is not a circle
    p2 = ogr.CreateGeometryFromWkt(
        f'POINT ({1 + math.cos(math.pi / 6) - 1e-2:.16g}'
        f' {math.sin(math.pi / 6):.16g})'
    )
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 1,2 0,1 1,0 0))'
    )
    self.assertFalse(p2.Within(g1))

    # Test area on circle in 2 pieces
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 1,2 0,1 -1,0 0))'
    )
    self.assertAlmostEqual(g1.Area(), math.pi, delta=1e-10)

    # Test area on hippodrome
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 1,2 0,2 -1,2 -2,1 -3,0 -2,'
        '0 -1,0 0))'
    )
    self.assertAlmostEqual(g1.Area(), math.pi + 2 * 2, delta=1e-10)

    # Same hippodrome but with different WKT
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (COMPOUNDCURVE(CIRCULARSTRING (0 0,1 1,2 0),(2 0,2 -2),'
        'CIRCULARSTRING(2 -2,1 -3,0 -2),(0 -2,0 0)))'
    )
    self.assertAlmostEqual(g1.Area(), math.pi + 2 * 2, delta=1e-10)

    # Similar, but with concave part (does not trigger optimization)
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON (COMPOUNDCURVE(CIRCULARSTRING (0 0,1 1,2 0),(2 0,2 -2),'
        'CIRCULARSTRING(2 -2,1 -1,0 -2),(0 -2,0 0)))'
    )
    self.assertAlmostEqual(g1.Area(), 2 * 2, delta=1e-10)

    # EMPTY
    for in_wkt in ('CURVEPOLYGON EMPTY', 'CURVEPOLYGON Z EMPTY'):
      g1 = ogr.CreateGeometryFromWkt(in_wkt)
      self.assertEqual(g1.ExportToWkt(), in_wkt)
      self.assertEqual(g1.Area(), 0.0)
      g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
      self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Empty CircularString
    g = ogr.Geometry(ogr.wkbCurvePolygon)
    g.AddGeometry(ogr.Geometry(ogr.wkbCircularString))
    self.assertEqual(g.Area(), 0)

    # Non-convex CircularString
    g = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON( COMPOUNDCURVE('
        ' CIRCULARSTRING(0 0,0.5 -0.5,1 0,1.5 0.5,2 0), (2 0,2 1,0 1,0 0) ) )'
    )
    self.assertAlmostEqual(g.Area(), 2.0, delta=1e-10)

  def test_ogr_geom_multicurve(self):
    # Simple test
    in_wkt = 'MULTICURVE (CIRCULARSTRING (0 0,1 0,0 0))'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbMultiCurve)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    self.assertEqual(g1.GetDimension(), 1)

    env = g1.GetEnvelope()
    expected_env = (0.0, 1.0, -0.5, 0.5)
    for i in range(4):
      self.assertAlmostEqual(env[i], expected_env[i], delta=1e-8)

    self.assertAlmostEqual(g1.Length(), 2 * math.pi * 0.5, delta=1e-8)

    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    self.assertTrue(g1.HasCurveGeometry())
    self.assertTrue(g1.HasCurveGeometry(True))

    # MULTICURVE of LINESTRING
    g1 = ogr.CreateGeometryFromWkt('MULTICURVE ((0 0,1 0))')
    self.assertTrue(g1.HasCurveGeometry())
    self.assertFalse(g1.HasCurveGeometry(True))

    # Z
    in_wkt = 'MULTICURVE Z (CIRCULARSTRING Z (0 0 10,1 0 10,0 0 10))'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbMultiCurveZ)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # WKT with all possible sub geometries
    in_wkt = (
        'MULTICURVE (CIRCULARSTRING (0 0,1 0,0 0),(0 0,1 1),'
        'COMPOUNDCURVE ((0 0,1 1),CIRCULARSTRING (1 1,2 2,3 3)))'
    )
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Test ForceToMultiLineString
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToMultiLineString(g1)
    expected_g = (
        'MULTILINESTRING ((0 0,0.116977778440514 -0.321393804843282,'
        '0.413175911166547 -0.49240387650611,0.75 -0.433012701892224,'
        '0.969846310392967 -0.171010071662835,'
        '0.969846310392967 0.171010071662835,0.75 0.433012701892224,'
        '0.413175911166547 0.49240387650611,'
        '0.116977778440514 0.321393804843282,0 0),(0 0,1 1),(0 0,1 1,2 2,3 3))'
    )
    self._check_geometry(g2, expected_g)

    # Test GetLinearGeometry
    self._check_geometry(g1.GetLinearGeometry(45), expected_g)

    # Error case: wrong sub-geometry type
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertIsNone(
          ogr.CreateGeometryFromWkt('MULTILINESTRING (POINT EMPTY)')
      )

  def test_ogr_geom_multisurface(self):
    # Simple test
    in_wkt = 'MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0,1 0,0 0)))'
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbMultiSurface)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    self.assertEqual(g1.GetDimension(), 2)

    env = g1.GetEnvelope()
    expected_env = (0.0, 1.0, -0.5, 0.5)
    for i in range(4):
      self.assertAlmostEqual(env[i], expected_env[i], delta=1e-8)

    self.assertAlmostEqual(g1.Area(), math.pi * 0.5 * 0.5, delta=1e-8)

    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    self.assertTrue(g1.HasCurveGeometry())
    self.assertTrue(g1.HasCurveGeometry(True))

    # Z
    in_wkt = (
        'MULTISURFACE Z (CURVEPOLYGON Z (CIRCULARSTRING Z'
        ' (0 0 10,1 0 10,0 0 10)))'
    )
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.GetGeometryType(), ogr.wkbMultiSurfaceZ)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # MULTISURFACE of POLYGON
    g1 = ogr.CreateGeometryFromWkt(
        'MULTISURFACE (((0 0,0 10,10 10,10 0,0 0)))'
    )
    self.assertTrue(g1.HasCurveGeometry())
    self.assertFalse(g1.HasCurveGeometry(True))

    # WKT with all possible sub geometries
    in_wkt = (
        'MULTISURFACE (((0 0,0 10,10 10,10 0,0 0)),'
        'CURVEPOLYGON (CIRCULARSTRING (0 0,1 0,0 0)))'
    )
    g1 = ogr.CreateGeometryFromWkt(in_wkt)
    self.assertEqual(g1.ExportToWkt(), in_wkt)
    g2 = ogr.CreateGeometryFromWkb(g1.ExportToWkb())
    self.assertEqual(g2.ExportToWkt(), in_wkt)

    # Test ForceToMultiPolygon
    with gdal.config_option('OGR_ARC_STEPSIZE', '45'):
      g2 = ogr.ForceToMultiPolygon(g1)
    expected_g = (
        'MULTIPOLYGON (((0 0,0 10,10 10,10 0,0 0)),'
        '((0 0,0.116977778440514 -0.321393804843282,'
        '0.413175911166547 -0.49240387650611,0.75 -0.433012701892224,'
        '0.969846310392967 -0.171010071662835,'
        '0.969846310392967 0.171010071662835,0.75 0.433012701892224,'
        '0.413175911166547 0.49240387650611,'
        '0.116977778440514 0.321393804843282,0 0)))'
    )
    self._check_geometry(g2, expected_g)

    # Test GetLinearGeometry
    g2 = g1.GetLinearGeometry(45)
    self._check_geometry(g2, expected_g)
    # Check that GetLinearGeometry() is idempotent on MULTIPOLYGON
    g3 = g2.GetLinearGeometry(45)
    self.assertTrue(g3.Equals(g2))

    # PointOnSurface
    if _have_geos():
      in_wkt = (
          'MULTISURFACE (((0 0,0 10,10 10,10 0,0 0),'
          '(1 1,1 9,9 9,9 1,1 1)),((10 0,10 10,20 10,20 0,10 0),'
          '(11 1,11 9,19 9,19 1,11 1)))'
      )
      g1 = ogr.CreateGeometryFromWkt(in_wkt)
      p1 = g1.PointOnSurface()
      self.assertEqual(p1.ExportToWkt(), 'POINT (0.5 5.0)')

    # Error case: wrong sub-geometry type
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertIsNone(ogr.CreateGeometryFromWkt('MULTIPOLYGON (POINT EMPTY)'))

  def test_ogr_geom_getcurvegeometry(self):
    for geom_name in (
        'POINT',
        'LINESTRING',
        'POLYGON',
        'MULTIPOINT',
        'MULTILINESTRING',
        'MULTIPOLYGON',
        'GEOMETRYCOLLECTION',
        'CIRCULARSTRING',
        'COMPOUNDCURVE',
        'CURVEPOLYGON',
        'MULTICURVE',
        'MULTISURFACE',
    ):
      in_wkt = geom_name + ' EMPTY'
      g = ogr.CreateGeometryFromWkt(in_wkt)
      g2 = g.GetCurveGeometry()
      self.assertEqual(g2.ExportToWkt(), in_wkt)

    for wkt in (
        'GEOMETRYCOLLECTION(POINT (0 1))',
        'GEOMETRYCOLLECTION(LINESTRING (0 0,0 1,0 0))',
        'POLYGON((0 0,0 1,1 1,1 0,0 0))',
        'POLYGON Z ((0 0 10,0 1 10,1 1 10,1 0 10,0 0 10))',
    ):
      g = ogr.CreateGeometryFromWkt(wkt)
      g2 = g.GetCurveGeometry()
      self.assertTrue(g.Equals(g2))

    # CircularString with large step
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1 1,2 0)')
    g2 = g1.GetLinearGeometry(15)
    g3 = g2.GetCurveGeometry()
    self.assertEqual(g3.GetGeometryType(), ogr.wkbCircularString)
    self.assertEqual(g1.GetPoint(0), g3.GetPoint(0))
    self.assertEqual(g1.GetPoint(2), g3.GetPoint(2))
    self.assertLessEqual(
        abs((g3.GetX(1) - 1) * (g3.GetX(1) - 1) + g3.GetY(1) * g3.GetY(1) - 1),
        1e-8,
    )

    # CurvePolygon with large step
    g1 = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON( CIRCULARSTRING (0 0,1 1,0 0))'
    )
    g2 = g1.GetLinearGeometry(15)
    g3 = g2.GetCurveGeometry()
    self.assertTrue(g3.Equals(g1))

    # This is a straight line
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (1 2,3 4,5 6)')
    g2 = g1.GetLinearGeometry()
    g3 = g2.GetCurveGeometry()
    self.assertEqual(g3.ExportToWkt(), 'LINESTRING (1 2,3 4,5 6)')

    # "Random" arcs
    for wkt in (
        'CIRCULARSTRING (1 2,3 1,5 6)',
        'CIRCULARSTRING (1 -2,3 -1,5 -6)',
        'CIRCULARSTRING (-1 2,-3 1,-5 6)',
        'CIRCULARSTRING (5 6,3 1,1 2)',
        'CIRCULARSTRING (-5 6,-3 1,-1 2)',
        'CIRCULARSTRING (5 -6,3 -1,1 -2)',
        'CIRCULARSTRING (215725 -977513,872751 872597,560240 -7500)',
        'CIRCULARSTRING (-492367 816163,537838 -421954,745494 -65479)',
        'CIRCULARSTRING (543208 -865295,582257 635396,563925 -68156)',
        'CIRCULARSTRING (-481 -193,1 329,-692 -421)',
        'CIRCULARSTRING (525407 781005,710737 463833,-674365 340022)',
        'CIRCULARSTRING (743949 709309,743952 709307,743964 709298)',
        'CIRCULARSTRING (283167 -48388,536492 -197399,-449301 382451)',
    ):
      g1 = ogr.CreateGeometryFromWkt(wkt)
      g2 = g1.GetLinearGeometry()
      g3 = g2.GetCurveGeometry()
      self.assertTrue(g3.Equals(g1), (wkt, g3.ExportToIsoWkt()))

    # Random arcs stress checks (historically non-fatal on rare roundtrip drift)
    rng = random.Random(0)
    for coord_limit, radius_limit in (
        (1000, None),
        (1000000, 10),
        (1000000, None),
    ):
      for _ in range(1000):
        if radius_limit is None:
          v = [rng.randint(-coord_limit, coord_limit) for _ in range(6)]
        else:
          cx = rng.randint(-coord_limit, coord_limit)
          cy = rng.randint(-coord_limit, coord_limit)
          d = [rng.randint(-radius_limit, radius_limit) for _ in range(6)]
          v = [cx + d[0], cy + d[1], cx + d[2], cy + d[3], cx + d[4], cy + d[5]]
        if v[0] != v[4] or v[1] != v[5]:
          wkt = f'CIRCULARSTRING ({v[0]} {v[1]},{v[2]} {v[3]},{v[4]} {v[5]})'
          g1 = ogr.CreateGeometryFromWkt(wkt)
          g2 = g1.GetLinearGeometry()
          if g2.GetPointCount() != 3:
            _ = g2.GetCurveGeometry()

    # 5 points full circle
    g1 = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (0 0,0.5 -0.5,1 0,0.5 0.5,0 0)'
    )
    g2 = g1.GetLinearGeometry()
    g3 = g2.GetCurveGeometry()
    self.assertTrue(g3.Equals(g1))

    # 3 points full circle
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1 0,0 0)')
    g2 = g1.GetLinearGeometry()
    g3 = g2.GetCurveGeometry()
    self.assertEqual(g3.GetGeometryType(), ogr.wkbCircularString)
    self.assertEqual(g1.GetPoint(0), g3.GetPoint(0))
    self.assertEqual(g1.GetPoint(1), g3.GetPoint(2))
    self.assertEqual(g1.GetPoint(2), g3.GetPoint(4))
    self.assertLessEqual(
        abs(
            (g3.GetX(1) - 0.5) * (g3.GetX(1) - 0.5)
            + g3.GetY(1) * g3.GetY(1)
            - 0.5 * 0.5
        ),
        1e-12,
    )
    self.assertLessEqual(
        abs(
            (g3.GetX(3) - 0.5) * (g3.GetX(3) - 0.5)
            + g3.GetY(3) * g3.GetY(3)
            - 0.5 * 0.5
        ),
        1e-12,
    )

    # 3 points full circle in a CurvePolygon
    for wkt in (
        'CURVEPOLYGON( CIRCULARSTRING (0 0,1 0,0 0))',
        'CURVEPOLYGON( CIRCULARSTRING (0 0,0 1,0 0))',
        'CURVEPOLYGON( CIRCULARSTRING (0 0,-1 0,0 0))',
        'CURVEPOLYGON( CIRCULARSTRING (0 0,0 -1,0 0))',
    ):
      g1 = ogr.CreateGeometryFromWkt(wkt)
      g2 = g1.GetLinearGeometry()
      g3 = g2.GetCurveGeometry()
      self.assertTrue(g3.Equals(g1))

    # Multi-segment curves and compound curves
    for wkt in (
        'CIRCULARSTRING (0 0,1 1,2 0,3 -1,4 0)',
        'CIRCULARSTRING (0 0,1 1,2 0,3 -1,4 0,5 1,6 0)',
        (
            'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,2 0),(2 0,3 0,4 0),'
            'CIRCULARSTRING (4 0,5 1,6 0))'
        ),
        (
            'COMPOUNDCURVE ((-1 0,-0.5 0.5,0 0),'
            'CIRCULARSTRING (0 0,1 1,2 0),(2 0,3 0,4 0),'
            'CIRCULARSTRING (4 0,5 1,6 0),(6 0,7 0))'
        ),
    ):
      g1 = ogr.CreateGeometryFromWkt(wkt)
      g2 = g1.GetLinearGeometry()
      g3 = g2.GetCurveGeometry()
      self.assertTrue(g3.Equals(g1))

    # Check with default discretization method
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1.2 1.0,2 0)')
    g2 = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,0.000997093138961 0.068920262501281,'
        '0.006738548124866 0.137608197923873,'
        '0.017197098230267 0.205737600590167,'
        '0.032323074784779 0.272984917348872,'
        '0.052044643056421 0.339030784160355,'
        '0.076268143401735 0.403561542787492,'
        '0.104878536064531 0.466270730389102,'
        '0.137739947510866 0.526860534941749,'
        '0.174696315705653 0.585043209577972,'
        '0.215572131266425 0.640542439124111,'
        '0.260173270974444 0.693094652347904,'
        '0.30828791968472 0.742450273683876,'
        '0.359687576256715 0.788374908491942,'
        '0.414128138728451 0.830650456220319,'
        '0.471351063580441 0.869076146186235,'
        '0.531084593583979 0.903469491055406,'
        '0.593045048402615 0.933667153492107,'
        '0.656938171817632 0.959525721864054,'
        '0.722460529179411 0.980922391318168,'
        '0.789300948448056 0.99775554699276,'
        '0.857141997979641 1.009945246596371,'
        '0.925661494039921 1.017433600061489,'
        '0.994534030886182 1.020185044470095,'
        '1.063432526150724 1.01818651294542,'
        '1.132029774186796 1.01144749670781,1.2 1.0,'
        '1.267547127648721 0.983752320094182,'
        '1.333803428245673 0.962854851656094,'
        '1.398449236893265 0.937408418110045,'
        '1.461172658788815 0.907535790143504,'
        '1.521671074014578 0.873381093378847,'
        '1.579652597579492 0.835109113014538,'
        '1.634837487668428 0.792904498790658,'
        '1.686959495304612 0.746970874114522,'
        '1.73576714891352 0.697529853644598,'
        '1.781024967590663 0.644819974072535,'
        '1.822514597219645 0.589095543261942,'
        '1.860035863959079 0.53062541329644,'
        '1.89340774001566 0.469691683356621,'
        '1.922469217043826 0.406588338684124,'
        '1.94708008295817 0.341619832199361,'
        '1.967121598410718 0.27509961561613,'
        '1.982497069669296 0.207348627140011,'
        '1.993132315133044 0.138693743046887,'
        '1.998976023234287 0.069466200612231,2 0)'
    )
    self.assertTrue(g2.GetCurveGeometry().Equals(g1))

    # Check with alternate discretization method: ROUND_ANGLE_METHOD
    g2 = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,-0.000199980003999 0.02,'
        '0.002236456877416 0.089770423644023,'
        '0.009533897446083 0.159200932797352,'
        '0.021656789305088 0.227953268998521,'
        '0.038546070943884 0.295692477776518,'
        '0.060119459480029 0.362088540515299,'
        '0.086271851533127 0.426817982271309,'
        '0.116875835277923 0.489565447710894,'
        '0.151782311181914 0.550025237489786,'
        '0.190821218403283 0.607902797589577,'
        '0.233802363310224 0.662916154355295,'
        '0.280516346085201 0.7147972882427,'
        '0.330735580899807 0.763293439582565,'
        '0.384215404690045 0.80816834000038,'
        '0.440695269130182 0.849203363492117,'
        '0.499900009998002 0.886198591548151,'
        '0.561541187747236 0.918973787136141,'
        '0.62531849275604 0.947369272797732,'
        '0.690921208405283 0.971246708581086,'
        '0.758029724858684 0.990489766019218,'
        '0.826317096169807 1.005004694870594,'
        '0.895450633129846 1.014720779860886,'
        '0.965093524096011 1.019590685200679,'
        '1.034906475903993 1.019590685200679,'
        '1.104549366870158 1.014720779860886,'
        '1.173682903830197 1.005004694870593,1.2 1.0,'
        '1.241970275141317 0.990489766019217,'
        '1.309078791594718 0.971246708581085,'
        '1.374681507243961 0.947369272797731,'
        '1.438458812252765 0.91897378713614,'
        '1.500099990002 0.886198591548151,'
        '1.559304730869819 0.849203363492116,'
        '1.615784595309956 0.808168340000379,'
        '1.669264419100194 0.763293439582565,'
        '1.7194836539148 0.714797288242699,'
        '1.766197636689777 0.662916154355294,'
        '1.809178781596718 0.607902797589576,'
        '1.848217688818087 0.550025237489785,'
        '1.883124164722078 0.489565447710893,'
        '1.913728148466874 0.426817982271308,'
        '1.939880540519971 0.362088540515298,'
        '1.961453929056117 0.295692477776516,'
        '1.978343210694912 0.227953268998519,'
        '1.990466102553917 0.15920093279735,'
        '1.997763543122584 0.089770423644022,'
        '2.000199980003999 0.02,2 0)'
    )
    self.assertTrue(g2.GetCurveGeometry().Equals(g1))

    # Check with PostgreSQL output of SELECT ST_CurveToLine(...)
    g2 = ogr.CreateGeometryFromWkt(
        'LINESTRING (0 0,0.000223190308279 0.049091765203314,'
        '0.002854930521212 0.098113445796116,'
        '0.007888880546112 0.146946944256066,'
        '0.015312913156447 0.195474616408063,'
        '0.025109143207391 0.243579554839372,'
        '0.037253970722702 0.291145870539817,'
        '0.051718137749134 0.338058972088558,'
        '0.068466798841411 0.384205841714863,'
        '0.08745960500795 0.429475307567812,'
        '0.108650800915124 0.473758311539029,'
        '0.131989335115863 0.516948171993215,'
        '0.157418983037062 0.558940840773549,'
        '0.184878482429505 0.599635153862819,'
        '0.214301680953989 0.638933075096389,'
        '0.245617695548099 0.676739932339917,'
        '0.27875108318972 0.712964645562815,'
        '0.31362202264588 0.747519946258017,'
        '0.350146506769097 0.780322587679462,'
        '0.388236544877951 0.811293545390794,'
        '0.427800374734344 0.840358207642151,'
        '0.46874268360677 0.867446555116406,'
        '0.510964837887032 0.892493329611833,'
        '0.554365120707246 0.915438191254836,'
        '0.598838976984681 0.936225863863983,'
        '0.644279265304116 0.954806268115175,'
        '0.690576516030889 0.971134642187118,'
        '0.737619195032841 0.985171649596477,'
        '0.785293972375802 0.996883473962907,'
        '0.833485995345338 1.006241900475673,'
        '0.88207916513699 1.013224383865605,'
        '0.930956416548473 1.017814102718623,0.98 1.02,'
        '1.029091765203309 1.019776809691721,'
        '1.078113445796111 1.017145069478788,'
        '1.12694694425606 1.012111119453889,'
        '1.175474616408058 1.004687086843554,'
        '1.223579554839367 0.994890856792611,'
        '1.271145870539812 0.9827460292773,'
        '1.318058972088553 0.968281862250867,'
        '1.364205841714858 0.951533201158591,'
        '1.409475307567807 0.932540394992052,'
        '1.453758311539024 0.911349199084878,'
        '1.49694817199321 0.88801066488414,'
        '1.538940840773545 0.862581016962941,'
        '1.579635153862814 0.835121517570498,'
        '1.618933075096384 0.805698319046015,'
        '1.656739932339913 0.774382304451905,'
        '1.692964645562811 0.741248916810284,'
        '1.727519946258013 0.706377977354124,'
        '1.760322587679458 0.669853493230907,'
        '1.791293545390791 0.631763455122053,'
        '1.820358207642148 0.59219962526566,'
        '1.847446555116403 0.551257316393235,'
        '1.872493329611831 0.509035162112973,'
        '1.895438191254833 0.465634879292759,'
        '1.916225863863981 0.421161023015324,'
        '1.934806268115173 0.37572073469589,'
        '1.951134642187117 0.329423483969116,'
        '1.965171649596476 0.282380804967164,'
        '1.976883473962906 0.234706027624203,'
        '1.986241900475672 0.186514004654668,'
        '1.993224383865604 0.137920834863015,'
        '1.997814102718623 0.089043583451532,2.0 0.04,2 0)'
    )
    g1_expected = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (0 0,0.98 1.02,2 0)'
    )
    self.assertTrue(g2.GetCurveGeometry().Equals(g1_expected))

    # Test default (implicit option ADD_INTERMEDIATE_POINT=STEALTH)
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1.2 1.0,2 0)')
    self.assertTrue(g1.GetLinearGeometry().GetCurveGeometry().Equals(g1))

    # Test with Z
    for wkt in (
        'CIRCULARSTRING (0 0 1,1 1 2,2 0 3)',
        'CIRCULARSTRING (2 0 3,1 1 2,0 0 1)',
    ):
      g1 = ogr.CreateGeometryFromWkt(wkt)
      self.assertTrue(g1.GetLinearGeometry().GetCurveGeometry().Equals(g1))

    # Test options ADD_INTERMEDIATE_POINT=STEALTH / YES
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1.2 1.0,2 0)')
    for opt in ('ADD_INTERMEDIATE_POINT=STEALTH', 'ADD_INTERMEDIATE_POINT=YES'):
      g2 = g1.GetLinearGeometry(options=[opt])
      self.assertTrue(g2.GetCurveGeometry().Equals(g1))

    # Big coordinates: (2,49),(3,50),(4,49) from EPSG:4326 to EPSG:32631
    g1 = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (426857.987717275 5427937.52346616,'
        '500000.000000001 5538630.70286887,'
        '573142.012282726 5427937.52346616)'
    )
    self._check_geometry(g1.GetLinearGeometry().GetCurveGeometry(), g1)

    g1 = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (426858 5427938,500000 5538632,573142 5427938)'
    )
    self.assertTrue(g1.GetLinearGeometry().GetCurveGeometry().Equals(g1))

    # Test option ADD_INTERMEDIATE_POINT=FALSE
    g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1.2 1.0,2 0)')
    g2 = g1.GetLinearGeometry(options=['ADD_INTERMEDIATE_POINT=FALSE'])
    g1_expected = ogr.CreateGeometryFromWkt(
        'CIRCULARSTRING (0 0,1.0 1.020199980003999,2 0)'
    )
    self._check_geometry(g2.GetCurveGeometry(), g1_expected)

    # Test with unrecognized options
    with gdal.quiet_errors():
      g2_new = g1.GetLinearGeometry(
          options=['bla', 'ADD_INTERMEDIATE_POINT=FALSE', 'foo=bar']
      )
    self.assertTrue(g2_new.Equals(g2))

    # Add repeated point at end / start of line
    g2 = g1.GetLinearGeometry()
    g2.AddPoint_2D(2 - 1e-14, 0)
    self.assertTrue(g2.GetCurveGeometry().Equals(g1))

    g2 = g1.GetLinearGeometry()
    g2_new = ogr.Geometry(ogr.wkbLineString)
    g2_new.AddPoint_2D(0, 1e-14)
    for i in range(g2.GetPointCount()):
      g2_new.AddPoint_2D(g2.GetX(i), g2.GetY(i))
    self.assertTrue(g2_new.GetCurveGeometry().Equals(g1))

    # Test various configurations
    configs = (
        ('CIRCULARSTRING (0 0,0.5 0.5,0 1,-0.5 0.5,0 0)', 0),
        ('CIRCULARSTRING (0 0,-0.5 0.5,0 1,0.5 0.5,0 0)', 0),
        ('CIRCULARSTRING (0 0,0.5 -0.5,0 -1,-0.5 -0.5,0 0)', 0),
        ('CIRCULARSTRING (0 0,-0.5 -0.5,0 -1,0.5 -0.5,0 0)', 0),
        ('CIRCULARSTRING (-1 -1,-1 1,1 1,1 -1,-1 -1)', 0),
        ('CIRCULARSTRING (-1 -1,1 -1,1 1,-1 1,-1 -1)', 0),
        ('CIRCULARSTRING (0 1,1 0,0 -1,-1 0,0 1)', 0),
        ('CIRCULARSTRING (0 0.01,0.01 0,0 -0.01,-0.01 0,0 0.01)', 0),
        ('CIRCULARSTRING (0 0.0001,0.0001 0,0 -0.0001,-0.0001 0,0 0.0001)', 0),
        (
            (
                'CIRCULARSTRING (0 1000000,1000000 0,0 -1000000,-1000000 0,'
                '0 1000000)'
            ),
            0,
        ),
        (
            (
                'CIRCULARSTRING (1234567 8901234,5678901 23456789,'
                '0123456 78901234)'
            ),
            0,
        ),
        (
            (
                'CIRCULARSTRING (1234567.12 8901234.34,5678901.56 23456789.01,'
                '0123456.78 78901234.56)'
            ),
            1e-1,
        ),
        (
            (
                'CIRCULARSTRING (1234567 -8901234,-5678901 23456789,'
                '0123456 -78901234)'
            ),
            0,
        ),
        (
            (
                'CIRCULARSTRING (0 0.000001,0.000001 0,0 -0.000001,'
                '-0.000001 0,0 0.000001)'
            ),
            1e-12,
        ),
        (
            (
                'CIRCULARSTRING (0 0.00000001,0.00000001 0,0 -0.00000001,'
                '-0.00000001 0,0 0.00000001)'
            ),
            1e-12,
        ),
        (
            (
                'CIRCULARSTRING (1 0.00000001,1.00000001 0,1 -0.00000001,'
                '0.99999999 0,1 0.00000001)'
            ),
            1e-12,
        ),
        (
            (
                'CIRCULARSTRING (100000000 1,100000001 0,100000000 -1,'
                '99999999 0,100000000 1)'
            ),
            0,
        ),
        (
            (
                'CIRCULARSTRING (-100000000 1,-100000001 0,-100000000 -1,'
                '-99999999 0,-100000000 1)'
            ),
            0,
        ),
        (
            (
                'CIRCULARSTRING (100000000 100000001,100000001 100000000,'
                '100000000 99999999,99999999 100000000,100000000 100000001)'
            ),
            0,
        ),
        (
            (
                'CIRCULARSTRING (760112.098000001162291 207740.096999999135733,'
                '760116.642489952617325 207741.101843414857285,'
                '760120.967999998480082 207742.820000000298023,'
                '760123.571822694852017 207744.275888498465065,'
                '760126.011999998241663 207745.991999998688698,'
                '760127.330062366439961 207747.037052432337077,'
                '760128.585999999195337 207748.155999999493361)'
            ),
            1e-5,
        ),
    )
    for wkt, eps in configs:
      g1 = ogr.CreateGeometryFromWkt(wkt)
      g3 = g1.GetLinearGeometry().GetCurveGeometry()
      if eps == 0:
        self.assertTrue(g3.Equals(g1), (wkt, g3.ExportToIsoWkt()))
      else:
        self._check_geometry(g3, g1, eps)

    # Test with container types
    for wkt in (
        'GEOMETRYCOLLECTION(CIRCULARSTRING (0 0,0.5 0.5,0 1,-0.5 0.5,0 0))',
        'MULTICURVE(CIRCULARSTRING (0 0,0.5 0.5,0 1,-0.5 0.5,0 0))',
        (
            'MULTISURFACE(CURVEPOLYGON(COMPOUNDCURVE('
            'CIRCULARSTRING (0 0,0.5 0.5,1 0),(1 0,0 0))))'
        ),
    ):
      g1 = ogr.CreateGeometryFromWkt(wkt)
      self.assertTrue(g1.GetLinearGeometry().GetCurveGeometry().Equals(g1))

    if _have_geos():
      g1 = ogr.CreateGeometryFromWkt(
          'CURVEPOLYGON(CIRCULARSTRING (0 0,2 0,0 0))'
      )
      g2 = ogr.CreateGeometryFromWkt(
          'CURVEPOLYGON(CIRCULARSTRING (1 0,3 1,1 0))'
      )
      self.assertEqual(
          g1.Intersection(g2).GetGeometryType(), ogr.wkbCurvePolygon
      )

      g2 = ogr.CreateGeometryFromWkt('CURVEPOLYGON((1 -1,1 1,3 1,3 -1,1 -1))')
      self.assertEqual(
          g1.Intersection(g2).GetGeometryType(), ogr.wkbCurvePolygon
      )

      g2 = ogr.CreateGeometryFromWkt(
          'CURVEPOLYGON(CIRCULARSTRING (3 0,5 0,3 0))'
      )
      self.assertEqual(
          g1.Union(g2).ExportToWkt(),
          'MULTISURFACE (CURVEPOLYGON (CIRCULARSTRING (0 0,2 0,0 0)),'
          'CURVEPOLYGON (CIRCULARSTRING (3 0,5 0,3 0)))',
      )

      g1 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (0 0,1 1,2 0))')
      g2 = ogr.CreateGeometryFromWkt('CIRCULARSTRING (2 0,1 -1,0 0))')
      union_wkt = g1.Union(g2).ExportToWkt()
      self.assertIn(
          union_wkt,
          (
              (
                  'MULTICURVE (CIRCULARSTRING (0 0,1 1,2 0),'
                  'CIRCULARSTRING (2 0,1 -1,0 0))'
              ),
              (
                  'MULTICURVE (CIRCULARSTRING (2 0,1 -1,0 0),'
                  'CIRCULARSTRING (0 0,1 1,2 0))'
              ),
          ),
      )

      g1 = (
          ogr.CreateGeometryFromWkt('POINT(1 2)')
          .Buffer(0.5)
          .GetCurveGeometry()
      )
      self.assertEqual(
          g1.ExportToWkt(),
          'CURVEPOLYGON (CIRCULARSTRING (1.5 2.0,0.5 2.0,1.5 2.0))',
      )

  def test_ogr_geom_getcurvegeometry_issue_8332(self):
    wkt = (
        'CURVEPOLYGON (COMPOUNDCURVE ('
        '(6.32498053375485 51.247181030842,6.32502606590151 51.2471797425536,'
        '6.32508564297808 51.2471779479449,6.3250808695535 51.2471782165485),'
        'CIRCULARSTRING (6.3250808695535 51.2471782165485,'
        '6.32505008690255 51.2471913007955,6.32504708890539 51.2472145542884),'
        '(6.32504708890539 51.2472145542884,6.32515225862976 51.247389900326,'
        '6.32518998812724 51.2474610619898,6.32522098151148 51.2475319886651,'
        '6.32522171612714 51.2475341460903,6.3252459540758 51.2476051701272,'
        '6.32526068943956 51.2476643653221),'
        'CIRCULARSTRING (6.32526068943956 51.2476643653221,'
        '6.32527641474695 51.2476781173008,6.32529935905561 51.2476869975705),'
        '(6.32529935905561 51.2476869975705,6.32526584392693 51.2476849360681,'
        '6.32525050727703 51.2476839917687,6.32522156833565 51.2476820978503,'
        '6.32518488221357 51.2476798558865),'
        'CIRCULARSTRING (6.32518488221357 51.2476798558865,'
        '6.32520508094068 51.2476723398662,6.32521597122632 51.247659277417),'
        '(6.32521597122632 51.247659277417,6.32520359420846 51.2476100657756,'
        '6.32517934693727 51.2475392034125,6.32514850449832 51.2474686399467,'
        '6.32511179828043 51.2473993544804,6.32499141360737 51.2471992501215),'
        'CIRCULARSTRING (6.32499141360737 51.2471992501215,'
        '6.32497317065307 51.2471881387982,6.32494975639483 51.2471819279964),'
        '(6.32494975639483 51.2471819279964,6.32498053375485 51.247181030842)))'
    )
    g = ogr.CreateGeometryFromWkt(wkt)
    g3 = g.GetLinearGeometry().GetCurveGeometry()
    wkt2 = (
        'CURVEPOLYGON (COMPOUNDCURVE ('
        '(6.32498053375485 51.247181030842,6.32502606590151 51.2471797425536,'
        '6.32508564297808 51.2471779479449,6.3250808695535 51.2471782165485),'
        'CIRCULARSTRING (6.3250808695535 51.2471782165485,'
        '6.32505009 51.2471913,6.32504708890539 51.2472145542884),'
        '(6.32504708890539 51.2472145542884,6.32515225862976 51.247389900326,'
        '6.32518998812724 51.2474610619898,6.32522098151148 51.2475319886651,'
        '6.32522171612714 51.2475341460903,6.3252459540758 51.2476051701272,'
        '6.32526068943956 51.2476643653221),'
        'CIRCULARSTRING (6.32526068943956 51.2476643653221,'
        '6.32527641 51.24767812,6.32529935905561 51.2476869975705),'
        '(6.32529935905561 51.2476869975705,6.32526584392693 51.2476849360681,'
        '6.32525050727703 51.2476839917687,6.32522156833565 51.2476820978503,'
        '6.32518488221357 51.2476798558865),'
        'CIRCULARSTRING (6.32518488221357 51.2476798558865,'
        '6.32520508 51.24767234,6.32521597122632 51.247659277417),'
        '(6.32521597122632 51.247659277417,6.32520359420846 51.2476100657756,'
        '6.32517934693727 51.2475392034125,6.32514850449832 51.2474686399467,'
        '6.32511179828043 51.2473993544804,6.32499141360737 51.2471992501215),'
        'CIRCULARSTRING (6.32499141360737 51.2471992501215,'
        '6.32497317 51.24718814,6.32494975639483 51.2471819279964),'
        '(6.32494975639483 51.2471819279964,6.32498053375485 51.247181030842)))'
    )
    self._check_geometry(g3, wkt2, 1e-10)

  def test_ogr_geom_getcurvegeometry_issue9382(self):
    wkt = (
        'POLYGON ((299517.68 6695650.45,299519.85 6695648.29,'
        '299522.85 6695645.24,299523.253879511 6695644.9271687,'
        '299523.639375687 6695644.59194529,299524.00524874 6695644.23540783,'
        '299524.350321919 6695643.8587029,299524.673485444 6695643.46304214,'
        '299524.973699995 6695643.04969797,299525.25 6695642.62,'
        '299525.27 6695642.6,299525.28 6695642.57,'
        '299525.581819649 6695642.02405493,299525.844959794 6695641.4584502,'
        '299526.068147436 6695640.87592268,299526.250302851 6695640.27928972,'
        '299526.390544832 6695639.67143822,299526.488194942 6695639.05530858,'
        '299526.542780772 6695638.43388152,299526.554038241 6695637.81016326,'
        '299526.521912903 6695637.18717098,299526.446560174 6695636.56791901,'
        '299526.328344583 6695635.95540261,299526.167838007 6695635.35258508,'
        '299525.965816945 6695634.76238274,299525.723258704 6695634.18765068,'
        '299525.441336721 6695633.63116956,299525.121414825 6695633.09563136,'
        '299524.765040724 6695632.58362663,299524.373938437 6695632.09763257,'
        '299523.95 6695631.64,299522.14 6695633.39,299522.32 6695633.59,'
        '299522.48 6695633.77,299522.63 6695633.96,299522.77 6695634.15,'
        '299522.9 6695634.35,299523.03 6695634.55,299523.15 6695634.76,'
        '299523.26 6695634.98,299523.37 6695635.19,299523.46 6695635.41,'
        '299523.55 6695635.64,299523.63 6695635.86,299523.7 6695636.09,'
        '299523.76 6695636.32,299523.81 6695636.56,299523.86 6695636.79,'
        '299523.9 6695637.03,299523.92 6695637.27,299523.94 6695637.51,'
        '299523.95 6695637.71,299523.95 6695637.97,299523.94 6695638.23,'
        '299523.93 6695638.47,299523.9 6695638.71,299523.87 6695638.95,'
        '299523.82 6695639.18,299523.77 6695639.42,299523.71 6695639.65,'
        '299523.64 6695639.88,299523.56 6695640.11,299523.47 6695640.33,'
        '299523.38 6695640.55,299523.28 6695640.77,299523.17 6695640.98,'
        '299523.05 6695641.19,299522.92 6695641.4,299522.79 6695641.6,'
        '299522.65 6695641.79,299522.5 6695641.98,299522.34 6695642.16,'
        '299522.21 6695642.31,299522.08 6695642.45,299515.85 6695648.75,'
        '299515.41 6695649.19,299514.69 6695649.95,299513.99 6695650.71,'
        '299513.29 6695651.49,299512.902658228 6695651.94,'
        '299516.254162679 6695651.94,299517.68 6695650.45))'
    )
    g = ogr.CreateGeometryFromWkt(wkt).GetCurveGeometry()
    expected_wkt = (
        'CURVEPOLYGON (COMPOUNDCURVE ('
        '(299517.68 6695650.45,299519.85 6695648.29,299522.85 6695645.24),'
        'CIRCULARSTRING (299522.85 6695645.24,'
        '299524.00524874 6695644.23540783,299525.25 6695642.62),'
        '(299525.25 6695642.62,299525.27 6695642.6,299525.28 6695642.57),'
        'CIRCULARSTRING (299525.28 6695642.57,'
        '299526.521912903 6695637.18717098,299523.95 6695631.64),'
        '(299523.95 6695631.64,299522.14 6695633.39,299522.32 6695633.59,'
        '299522.48 6695633.77,299522.63 6695633.96,299522.77 6695634.15,'
        '299522.9 6695634.35,299523.03 6695634.55,299523.15 6695634.76,'
        '299523.26 6695634.98,299523.37 6695635.19,299523.46 6695635.41,'
        '299523.55 6695635.64,299523.63 6695635.86,299523.7 6695636.09,'
        '299523.76 6695636.32,299523.81 6695636.56,299523.86 6695636.79,'
        '299523.9 6695637.03,299523.92 6695637.27,299523.94 6695637.51,'
        '299523.95 6695637.71,299523.95 6695637.97,299523.94 6695638.23,'
        '299523.93 6695638.47,299523.9 6695638.71,299523.87 6695638.95,'
        '299523.82 6695639.18,299523.77 6695639.42,299523.71 6695639.65,'
        '299523.64 6695639.88,299523.56 6695640.11,299523.47 6695640.33,'
        '299523.38 6695640.55,299523.28 6695640.77,299523.17 6695640.98,'
        '299523.05 6695641.19,299522.92 6695641.4,299522.79 6695641.6,'
        '299522.65 6695641.79,299522.5 6695641.98,299522.34 6695642.16,'
        '299522.21 6695642.31,299522.08 6695642.45,299515.85 6695648.75,'
        '299515.41 6695649.19,299514.69 6695649.95,299513.99 6695650.71,'
        '299513.29 6695651.49,299512.902658228 6695651.94,'
        '299516.254162679 6695651.94,299517.68 6695650.45)))'
    )
    self._check_geometry(g, expected_wkt, 1e-10)

  @parameterized.parameters(
      (ogr.wkbPoint, 0),
      (ogr.wkbPoint25D, 1),
      (ogr.wkbPointM, 0),
      (ogr.wkbPointZM, 1),
      (ogr.wkbCircularString, 0),
      (ogr.wkbCircularStringZ, 1),
      (ogr.wkbCircularStringM, 0),
      (ogr.wkbCircularStringZM, 1),
  )
  def test_ogr_geom_GT_HasZ(self, gt, res):
    self.assertEqual(ogr.GT_HasZ(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, ogr.wkbPoint25D),
      (ogr.wkbPoint25D, ogr.wkbPoint25D),
      (ogr.wkbPointM, ogr.wkbPointZM),
      (ogr.wkbPointZM, ogr.wkbPointZM),
      (ogr.wkbCircularString, ogr.wkbCircularStringZ),
      (ogr.wkbCircularStringZ, ogr.wkbCircularStringZ),
      (ogr.wkbCircularStringM, ogr.wkbCircularStringZM),
      (ogr.wkbCircularStringZM, ogr.wkbCircularStringZM),
  )
  def test_ogr_geom_GT_SetZ(self, gt, res):
    self.assertEqual(ogr.GT_SetZ(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, 0),
      (ogr.wkbPoint25D, 0),
      (ogr.wkbPointM, 1),
      (ogr.wkbPointZM, 1),
      (ogr.wkbCircularString, 0),
      (ogr.wkbCircularStringZ, 0),
      (ogr.wkbCircularStringM, 1),
      (ogr.wkbCircularStringZM, 1),
  )
  def test_ogr_geom_GT_HasM(self, gt, res):
    self.assertEqual(ogr.GT_HasM(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, ogr.wkbPointM),
      (ogr.wkbPoint25D, ogr.wkbPointZM),
      (ogr.wkbPointM, ogr.wkbPointM),
      (ogr.wkbPointZM, ogr.wkbPointZM),
      (ogr.wkbCircularString, ogr.wkbCircularStringM),
      (ogr.wkbCircularStringZ, ogr.wkbCircularStringZM),
      (ogr.wkbCircularStringM, ogr.wkbCircularStringM),
      (ogr.wkbCircularStringZM, ogr.wkbCircularStringZM),
  )
  def test_ogr_geom_GT_SetM(self, gt, res):
    self.assertEqual(ogr.GT_SetM(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, 0, 0, ogr.wkbPoint),
      (ogr.wkbPoint, 1, 0, ogr.wkbPoint25D),
      (ogr.wkbPoint, 0, 1, ogr.wkbPointM),
      (ogr.wkbPoint, 1, 1, ogr.wkbPointZM),
      (ogr.wkbPoint25D, 0, 0, ogr.wkbPoint),
      (ogr.wkbPoint25D, 1, 0, ogr.wkbPoint25D),
      (ogr.wkbPoint25D, 0, 1, ogr.wkbPointM),
      (ogr.wkbPoint25D, 1, 1, ogr.wkbPointZM),
      (ogr.wkbPointM, 0, 0, ogr.wkbPoint),
      (ogr.wkbPointM, 1, 0, ogr.wkbPoint25D),
      (ogr.wkbPointM, 0, 1, ogr.wkbPointM),
      (ogr.wkbPointM, 1, 1, ogr.wkbPointZM),
      (ogr.wkbCircularString, 1, 0, ogr.wkbCircularStringZ),
      (ogr.wkbCircularStringZ, 1, 0, ogr.wkbCircularStringZ),
      (ogr.wkbPoint, 0, 0, ogr.wkbPoint),
      (ogr.wkbPoint25D, 0, 0, ogr.wkbPoint),
      (ogr.wkbCircularString, 0, 0, ogr.wkbCircularString),
      (ogr.wkbCircularStringZ, 0, 0, ogr.wkbCircularString),
  )
  def test_ogr_geom_GT_SetModifier(self, gt, mod_z, mod_m, res):
    self.assertEqual(ogr.GT_SetModifier(gt, mod_z, mod_m), res)

  @parameterized.parameters(
      (ogr.wkbPoint, ogr.wkbPoint),
      (ogr.wkbPoint25D, ogr.wkbPoint),
      (ogr.wkbPointM, ogr.wkbPoint),
      (ogr.wkbPointZM, ogr.wkbPoint),
      (ogr.wkbCircularString, ogr.wkbCircularString),
      (ogr.wkbTriangleZ, ogr.wkbTriangle),
      (ogr.wkbCircularStringZ, ogr.wkbCircularString),
      (ogr.wkbCircularStringM, ogr.wkbCircularString),
      (ogr.wkbCircularStringZM, ogr.wkbCircularString),
  )
  def test_ogr_geom_GT_Flatten(self, gt, res):
    self.assertEqual(ogr.GT_Flatten(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, ogr.wkbPoint, 1),
      (ogr.wkbPoint25D, ogr.wkbPoint, 1),
      (ogr.wkbPoint, ogr.wkbUnknown, 1),
      (ogr.wkbPoint, ogr.wkbLineString, 0),
      (ogr.wkbPolygon, ogr.wkbCurvePolygon, 1),
      (ogr.wkbTriangle, ogr.wkbCurvePolygon, 1),
      (ogr.wkbTriangle, ogr.wkbPolygon, 1),
      (ogr.wkbMultiSurface, ogr.wkbGeometryCollection, 1),
      (ogr.wkbMultiPolygon, ogr.wkbMultiSurface, 1),
      (ogr.wkbMultiLineString, ogr.wkbMultiCurve, 1),
      (ogr.wkbUnknown, ogr.wkbUnknown, 1),
      (ogr.wkbUnknown, ogr.wkbPoint, 0),
      (ogr.wkbTIN, ogr.wkbPolyhedralSurface, 1),
      (ogr.wkbPolyhedralSurface, ogr.wkbTIN, 0),
  )
  def test_ogr_geom_GT_IsSubClassOf(self, gt, gt2, res):
    self.assertEqual(ogr.GT_IsSubClassOf(gt, gt2), res)

  @parameterized.parameters(
      (ogr.wkbPoint, 0),
      (ogr.wkbPoint25D, 0),
      (ogr.wkbPointM, 0),
      (ogr.wkbPointZM, 0),
      (ogr.wkbCircularString, 1),
      (ogr.wkbCircularStringZ, 1),
      (ogr.wkbLineString, 1),
      (ogr.wkbCompoundCurve, 1),
      (ogr.wkbCompoundCurveZ, 1),
      (ogr.wkbCompoundCurveM, 1),
      (ogr.wkbCompoundCurveZM, 1),
      (ogr.wkbCurvePolygon, 0),
      (ogr.wkbTriangle, 0),
      (ogr.wkbPolyhedralSurface, 0),
      (ogr.wkbTIN, 0),
  )
  def test_ogr_geom_GT_IsCurve(self, gt, res):
    self.assertEqual(ogr.GT_IsCurve(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, 0),
      (ogr.wkbPoint25D, 0),
      (ogr.wkbPointM, 0),
      (ogr.wkbPointZM, 0),
      (ogr.wkbCircularString, 0),
      (ogr.wkbCurvePolygon, 1),
      (ogr.wkbCurvePolygonZ, 1),
      (ogr.wkbCurvePolygonM, 1),
      (ogr.wkbCurvePolygonZM, 1),
      (ogr.wkbPolygon, 1),
      (ogr.wkbTriangle, 1),
      (ogr.wkbPolyhedralSurface, 1),
      (ogr.wkbTIN, 1),
  )
  def test_ogr_geom_GT_IsSurface(self, gt, res):
    self.assertEqual(ogr.GT_IsSurface(gt), res)

  @parameterized.parameters(
      (ogr.wkbNone, ogr.wkbNone),
      (ogr.wkbPoint, ogr.wkbMultiPoint),
      (ogr.wkbPoint25D, ogr.wkbMultiPoint25D),
      (ogr.wkbPointM, ogr.wkbMultiPointM),
      (ogr.wkbPointZM, ogr.wkbMultiPointZM),
      (ogr.wkbCircularString, ogr.wkbMultiCurve),
      (ogr.wkbCompoundCurve, ogr.wkbMultiCurve),
      (ogr.wkbCurvePolygon, ogr.wkbMultiSurface),
      (ogr.wkbLineString, ogr.wkbMultiLineString),
      (ogr.wkbPolygon, ogr.wkbMultiPolygon),
  )
  def test_ogr_geom_GT_GetCollection(self, gt, res):
    self.assertEqual(ogr.GT_GetCollection(gt), res)

  @parameterized.parameters(
      (ogr.wkbNone, ogr.wkbNone),
      (ogr.wkbMultiPoint, ogr.wkbPoint),
      (ogr.wkbMultiPoint25D, ogr.wkbPoint25D),
      (ogr.wkbMultiPointM, ogr.wkbPointM),
      (ogr.wkbMultiPointZM, ogr.wkbPointZM),
      (ogr.wkbMultiLineString, ogr.wkbLineString),
      (ogr.wkbMultiPolygon, ogr.wkbPolygon),
      (ogr.wkbMultiCurve, ogr.wkbCompoundCurve),
      (ogr.wkbMultiSurface, ogr.wkbCurvePolygon),
      (ogr.wkbPoint, ogr.wkbPoint),
      (ogr.wkbGeometryCollection, ogr.wkbUnknown),
      (ogr.wkbUnknown, ogr.wkbUnknown),
  )
  def test_ogr_geom_GT_GetSingle(self, gt, res):
    self.assertEqual(ogr.GT_GetSingle(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, 0),
      (ogr.wkbPoint25D, 0),
      (ogr.wkbPointM, 0),
      (ogr.wkbPointZM, 0),
      (ogr.wkbCircularString, 1),
      (ogr.wkbCircularStringM, 1),
      (ogr.wkbCircularStringZ, 1),
      (ogr.wkbCircularStringZM, 1),
      (ogr.wkbCompoundCurve, 1),
      (ogr.wkbCurvePolygon, 1),
      (ogr.wkbMultiCurve, 1),
      (ogr.wkbMultiSurface, 1),
      (ogr.wkbLineString, 0),
      (ogr.wkbPolygon, 0),
      (ogr.wkbTriangle, 0),
  )
  def test_ogr_geom_GT_IsNonLinear(self, gt, res):
    self.assertEqual(ogr.GT_IsNonLinear(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, ogr.wkbPoint),
      (ogr.wkbPoint25D, ogr.wkbPoint25D),
      (ogr.wkbPointM, ogr.wkbPointM),
      (ogr.wkbPointZM, ogr.wkbPointZM),
      (ogr.wkbCircularString, ogr.wkbCircularString),
      (ogr.wkbCircularStringZ, ogr.wkbCircularStringZ),
      (ogr.wkbCircularStringM, ogr.wkbCircularStringM),
      (ogr.wkbCircularStringZM, ogr.wkbCircularStringZM),
      (ogr.wkbCompoundCurve, ogr.wkbCompoundCurve),
      (ogr.wkbCurvePolygon, ogr.wkbCurvePolygon),
      (ogr.wkbLineString, ogr.wkbCompoundCurve),
      (ogr.wkbPolygon, ogr.wkbCurvePolygon),
      (ogr.wkbMultiLineString, ogr.wkbMultiCurve),
      (ogr.wkbMultiPolygon, ogr.wkbMultiSurface),
      (ogr.wkbMultiCurve, ogr.wkbMultiCurve),
      (ogr.wkbMultiSurface, ogr.wkbMultiSurface),
  )
  def test_ogr_geom_GT_GetCurve(self, gt, res):
    self.assertEqual(ogr.GT_GetCurve(gt), res)

  @parameterized.parameters(
      (ogr.wkbPoint, ogr.wkbPoint),
      (ogr.wkbPoint25D, ogr.wkbPoint25D),
      (ogr.wkbPointM, ogr.wkbPointM),
      (ogr.wkbPointZM, ogr.wkbPointZM),
      (ogr.wkbCircularString, ogr.wkbLineString),
      (ogr.wkbCircularStringM, ogr.wkbLineStringM),
      (ogr.wkbCircularStringZ, ogr.wkbLineString25D),
      (ogr.wkbCircularStringZM, ogr.wkbLineStringZM),
      (ogr.wkbCompoundCurve, ogr.wkbLineString),
      (ogr.wkbCurvePolygon, ogr.wkbPolygon),
      (ogr.wkbLineString, ogr.wkbLineString),
      (ogr.wkbPolygon, ogr.wkbPolygon),
      (ogr.wkbMultiLineString, ogr.wkbMultiLineString),
      (ogr.wkbMultiPolygon, ogr.wkbMultiPolygon),
      (ogr.wkbMultiCurve, ogr.wkbMultiLineString),
      (ogr.wkbMultiSurface, ogr.wkbMultiPolygon),
  )
  def test_ogr_geom_GT_GetLinear(self, gt, res):
    self.assertEqual(ogr.GT_GetLinear(gt), res)

  def test_ogr_geom_api_limit_tests(self):
    p = ogr.Geometry(ogr.wkbPoint)
    lyr = ogr.Geometry(ogr.wkbLineString)
    poly = ogr.Geometry(ogr.wkbPolygon)

    with _disable_exceptions(), gdal.quiet_errors():
      p.GetX(1)
      p.GetY(1)
      p.GetZ(1)

      lyr.GetX(1)
      lyr.GetY(1)
      lyr.GetZ(1)

      poly.GetX()
      poly.GetY()
      poly.GetZ()

      poly.GetPoints()

      p.GetPoint(1)
      lyr.GetPoint(1)
      poly.GetPoint(1)

      p.SetPoint(1, 0, 0)
      lyr.SetPoint(-1, 0, 0)
      poly.SetPoint(0, 0, 0)

      p.SetPoint_2D(1, 0, 0)
      lyr.SetPoint_2D(-1, 0, 0)
      poly.SetPoint_2D(0, 0, 0)

      poly.AddPoint(0, 0)
      poly.AddPoint_2D(0, 0)

      p.GetGeometryRef(1)
      p.AddGeometry(p)
      p.AddGeometryDirectly(p)

  def test_ogr_geom_equals(self):
    p_empty = ogr.Geometry(ogr.wkbPoint)
    p_0 = ogr.CreateGeometryFromWkt('POINT (0 0)')
    p_1 = ogr.CreateGeometryFromWkt('POINT (1 1)')
    self.assertTrue(p_empty.Equals(p_empty))
    self.assertTrue(p_0.Equals(p_0))
    self.assertTrue(p_0.Equals(p_0.Clone()))
    self.assertFalse(p_empty.Equals(p_0))
    self.assertFalse(p_0.Equals(p_empty))
    self.assertFalse(p_0.Equals(p_1))

    l_empty = ogr.Geometry(ogr.wkbLineString)
    l_0_1 = ogr.CreateGeometryFromWkt('LINESTRING (0 0,1 1)')
    l_0_1_2 = ogr.CreateGeometryFromWkt('LINESTRING (0 0,1 1,2 2)')
    self.assertTrue(l_0_1.Equals(l_0_1))
    self.assertTrue(l_0_1.Equals(l_0_1.Clone()))
    self.assertFalse(l_empty.Equals(l_0_1))
    self.assertFalse(l_0_1.Equals(l_empty))
    self.assertFalse(l_0_1.Equals(l_0_1_2))

    gc_empty = ogr.Geometry(ogr.wkbGeometryCollection)
    gc_p_0 = ogr.CreateGeometryFromWkt('GEOMETRYCOLLECTION (POINT (0 0))')
    gc_p_1 = ogr.CreateGeometryFromWkt('GEOMETRYCOLLECTION (POINT (1 1))')
    self.assertTrue(gc_empty.Equals(gc_empty))
    self.assertTrue(gc_p_0.Equals(gc_p_0))
    self.assertTrue(gc_p_0.Equals(gc_p_0.Clone()))
    self.assertFalse(gc_empty.Equals(gc_p_0))
    self.assertFalse(gc_p_0.Equals(gc_empty))
    self.assertFalse(gc_p_0.Equals(gc_p_1))

  def test_ogr_geom_measured_geometries_to_2D_or_3D(self):
    list_wkt = (
        ('POINT M (1 2 3)', 'POINT (1 2)', 'POINT Z (1 2 0)'),
        ('POINT ZM (1 2 3 4)', 'POINT (1 2)', 'POINT Z (1 2 3)'),
        ('LINESTRING M (1 2 3)', 'LINESTRING (1 2)', 'LINESTRING Z (1 2 0)'),
        (
            'LINESTRING ZM (1 2 3 4)',
            'LINESTRING (1 2)',
            'LINESTRING Z (1 2 3)',
        ),
        ('POLYGON M ((1 2 3))', 'POLYGON ((1 2))', 'POLYGON Z ((1 2 0))'),
        ('POLYGON ZM ((1 2 3 4))', 'POLYGON ((1 2))', 'POLYGON Z ((1 2 3))'),
        (
            'CIRCULARSTRING M (1 2 3,4 5 6,7 8 9)',
            'CIRCULARSTRING (1 2,4 5,7 8)',
            'CIRCULARSTRING Z (1 2 0,4 5 0,7 8 0)',
        ),
        (
            'CIRCULARSTRING ZM (1 2 3 0,4 5 6 0,7 8 9 0)',
            'CIRCULARSTRING (1 2,4 5,7 8)',
            'CIRCULARSTRING Z (1 2 3,4 5 6,7 8 9)',
        ),
        (
            'COMPOUNDCURVE M ((1 2 3,4 5 6))',
            'COMPOUNDCURVE ((1 2,4 5))',
            'COMPOUNDCURVE Z ((1 2 0,4 5 0))',
        ),
        (
            'COMPOUNDCURVE ZM ((1 2 3 4,5 6 7 8))',
            'COMPOUNDCURVE ((1 2,5 6))',
            'COMPOUNDCURVE Z ((1 2 3,5 6 7))',
        ),
        (
            'MULTIPOINT M ((1 2 3))',
            'MULTIPOINT ((1 2))',
            'MULTIPOINT Z ((1 2 0))',
        ),
        (
            'MULTIPOINT ZM ((1 2 3 4))',
            'MULTIPOINT ((1 2))',
            'MULTIPOINT Z ((1 2 3))',
        ),
        (
            'MULTILINESTRING M ((1 2 3))',
            'MULTILINESTRING ((1 2))',
            'MULTILINESTRING Z ((1 2 0))',
        ),
        (
            'MULTILINESTRING ZM ((1 2 3 4))',
            'MULTILINESTRING ((1 2))',
            'MULTILINESTRING Z ((1 2 3))',
        ),
        (
            'MULTICURVE M ((1 2 3))',
            'MULTICURVE ((1 2))',
            'MULTICURVE Z ((1 2 0))',
        ),
        (
            'MULTICURVE ZM ((1 2 3 4))',
            'MULTICURVE ((1 2))',
            'MULTICURVE Z ((1 2 3))',
        ),
        (
            'MULTIPOLYGON M (((1 2 3)))',
            'MULTIPOLYGON (((1 2)))',
            'MULTIPOLYGON Z (((1 2 0)))',
        ),
        (
            'MULTIPOLYGON ZM (((1 2 3 4)))',
            'MULTIPOLYGON (((1 2)))',
            'MULTIPOLYGON Z (((1 2 3)))',
        ),
        (
            'MULTISURFACE M (((1 2 3)))',
            'MULTISURFACE (((1 2)))',
            'MULTISURFACE Z (((1 2 0)))',
        ),
        (
            'MULTISURFACE ZM (((1 2 3 4)))',
            'MULTISURFACE (((1 2)))',
            'MULTISURFACE Z (((1 2 3)))',
        ),
        (
            'GEOMETRYCOLLECTION M (POINT M (1 2 3))',
            'GEOMETRYCOLLECTION (POINT (1 2))',
            'GEOMETRYCOLLECTION Z (POINT Z (1 2 0))',
        ),
        (
            'GEOMETRYCOLLECTION ZM (POINT ZM (1 2 3 4))',
            'GEOMETRYCOLLECTION (POINT (1 2))',
            'GEOMETRYCOLLECTION Z (POINT Z (1 2 3))',
        ),
        (
            'TRIANGLE M ((0 0 3,0 1 3,1 1 3,0 0 3))',
            'TRIANGLE ((0 0,0 1,1 1,0 0))',
            'TRIANGLE Z ((0 0 0,0 1 0,1 1 0,0 0 0))',
        ),
        (
            'TRIANGLE ZM ((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4))',
            'TRIANGLE ((0 0,0 1,1 1,0 0))',
            'TRIANGLE Z ((0 0 3,0 1 3,1 1 3,0 0 3))',
        ),
        (
            'POLYHEDRALSURFACE M (((0 0 3,0 1 3,1 1 3,0 0 3)))',
            'POLYHEDRALSURFACE (((0 0,0 1,1 1,0 0)))',
            'POLYHEDRALSURFACE Z (((0 0 0,0 1 0,1 1 0,0 0 0)))',
        ),
        (
            'POLYHEDRALSURFACE ZM (((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4)))',
            'POLYHEDRALSURFACE (((0 0,0 1,1 1,0 0)))',
            'POLYHEDRALSURFACE Z (((0 0 3,0 1 3,1 1 3,0 0 3)))',
        ),
        (
            'TIN M (((0 0 3,0 1 3,1 1 3,0 0 3)))',
            'TIN (((0 0,0 1,1 1,0 0)))',
            'TIN Z (((0 0 0,0 1 0,1 1 0,0 0 0)))',
        ),
        (
            'TIN ZM (((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4)))',
            'TIN (((0 0,0 1,1 1,0 0)))',
            'TIN Z (((0 0 3,0 1 3,1 1 3,0 0 3)))',
        ),
    )
    for before, after_2d, after_3d in list_wkt:
      geom = ogr.CreateGeometryFromWkt(before)
      wkb = geom.ExportToIsoWkb()
      geom2 = ogr.CreateGeometryFromWkb(wkb)
      self.assertTrue(
          geom.Equals(geom2), (before, geom2.ExportToIsoWkt())
      )

      geom = ogr.CreateGeometryFromWkt(before)
      geom.FlattenTo2D()
      self.assertEqual(geom.ExportToIsoWkt(), after_2d, before)

      geom = ogr.CreateGeometryFromWkt(before)
      geom.SetCoordinateDimension(2)
      self.assertEqual(geom.ExportToIsoWkt(), after_2d, before)

      geom = ogr.CreateGeometryFromWkt(before)
      geom.SetCoordinateDimension(3)
      self.assertEqual(geom.ExportToIsoWkt(), after_3d, before)

      # Test no-op
      geom = ogr.CreateGeometryFromWkt(before)
      geom.Set3D(geom.Is3D())
      geom.SetMeasured(geom.IsMeasured())
      self.assertEqual(geom.ExportToIsoWkt(), before)

    self.assertEqual(
        ogr.CreateGeometryFromWkt('POINT (1 2)').CoordinateDimension(), 2
    )
    self.assertEqual(
        ogr.CreateGeometryFromWkt('POINT M (1 2 3)').CoordinateDimension(), 3
    )
    self.assertEqual(
        ogr.CreateGeometryFromWkt('POINT Z (1 2 3)').CoordinateDimension(), 3
    )
    self.assertEqual(
        ogr.CreateGeometryFromWkt('POINT ZM (1 2 3 4)').CoordinateDimension(), 4
    )

    # Test old-style unqualified non-bracketed ZM import
    g = ogr.CreateGeometryFromWkt('MULTIPOINT (1 2 3 4)')
    self.assertEqual(g.ExportToIsoWkt(), 'MULTIPOINT ZM ((1 2 3 4))')

  def test_ogr_geom_postgis_ewkt_xym(self):
    list_wkt = (
        ('POINTM(1 2 3)', 'POINT M (1 2 3)'),
        (
            'GEOMETRYCOLLECTIONM(POINTM(1 2 3))',
            'GEOMETRYCOLLECTION M (POINT M (1 2 3))',
        ),
    )
    for before, after in list_wkt:
      geom = ogr.CreateGeometryFromWkt(before)
      self.assertEqual(geom.ExportToIsoWkt(), after, before)

  def test_ogr_geom_curve_surface(self):
    tests = (
        (ogr.wkbCurve, 'Curve'),
        (ogr.wkbCurveZ, '3D Curve'),
        (ogr.wkbCurveM, 'Measured Curve'),
        (ogr.wkbCurveZM, '3D Measured Curve'),
        (ogr.wkbSurface, 'Surface'),
        (ogr.wkbSurfaceZ, '3D Surface'),
        (ogr.wkbSurfaceM, 'Measured Surface'),
        (ogr.wkbSurfaceZM, '3D Measured Surface'),
    )
    for wkb_type, name in tests:
      self.assertEqual(ogr.GeometryTypeToName(wkb_type), name)

  def test_ogr_geom_import_corrupted_wkb(self):
    list_wkt = (
        'POINT ZM (1 2 3 4)',
        'LINESTRING ZM (1 2 3 4)',
        'POLYGON ZM ((1 2 3 4))',
        'CIRCULARSTRING ZM (1 2 3 0,4 5 6 0,7 8 9 0)',
        'COMPOUNDCURVE ZM ((1 2 3 4,5 6 7 8))',
        'MULTIPOINT ZM ((1 2 3 4))',
        'MULTILINESTRING ZM ((1 2 3 4))',
        'MULTICURVE ZM ((1 2 3 4))',
        'MULTIPOLYGON ZM (((1 2 3 4)))',
        'MULTISURFACE ZM (((1 2 3 4)))',
        'GEOMETRYCOLLECTION ZM (POINT ZM (1 2 3 4))',
        'TRIANGLE ZM ((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4))',
        (
            'POLYHEDRALSURFACE ZM (((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4)),'
            '((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4)))'
        ),
        (
            'TIN ZM (((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4)),'
            '((0 0 3 4,0 1 3 4,1 1 3 4,0 0 3 4)))'
        ),
    )

    for wkt in list_wkt:
      g = ogr.CreateGeometryFromWkt(wkt)
      wkb = bytearray(g.ExportToWkb())

      # Test altering the WKB
      for i, _ in enumerate(wkb):
        for method in range(4 + 4 + 4 + 1):
          init_val = wkb[i]
          if method < 4:
            wkb[i] = method
          elif method < 8:
            wkb[i] = 255 - (method - 4)
          elif method < 12:
            wkb[i] = 127 + 2 - (method - 8)
          else:
            wkb[i] = 255 - wkb[i]
          with _disable_exceptions(), gdal.quiet_errors():
            g = ogr.CreateGeometryFromWkb(bytes(wkb))
          if g:
            g2 = ogr.CreateGeometryFromWkb(g.ExportToIsoWkb())
            self.assertTrue(g.Equals(g2), (wkt, i, method))
          wkb[i] = init_val

      # Test truncation of the WKB
      for i in range(len(wkb)):
        with _disable_exceptions(), gdal.quiet_errors():
          g = ogr.CreateGeometryFromWkb(bytes(wkb[:i]))
        self.assertIsNone(g, (wkt, i))

  def test_ogr_geom_triangle_ps_tin_conversion(self):
    wkts = (
        'TRIANGLE ((0 0,0 1,1 1,0 0))',
        'POLYGON ((0 0,0 1,1 1,0 0))',
        'CURVEPOLYGON ((0 0,0 1,1 1,0 0))',
        'MULTIPOLYGON (((0 0,0 1,1 1,0 0)))',
        'MULTISURFACE (((0 0,0 1,1 1,0 0)))',
        'TIN (((0 0,0 1,1 1,0 0)))',
        'POLYHEDRALSURFACE (((0 0,0 1,1 1,0 0)))',
    )
    surface_types = (ogr.wkbTriangle, ogr.wkbTIN, ogr.wkbPolyhedralSurface)

    for wkt in wkts:
      for wkt_target in wkts:
        g = ogr.CreateGeometryFromWkt(wkt)
        g2 = ogr.CreateGeometryFromWkt(wkt_target)
        got_wkt = ogr.ForceTo(g, g2.GetGeometryType()).ExportToWkt()
        if (
            g.GetGeometryType() not in surface_types
            and g2.GetGeometryType() not in surface_types
        ):
          continue
        wkt_expected = wkt_target
        if (
            g.GetGeometryType() in (ogr.wkbTIN, ogr.wkbPolyhedralSurface)
            and g2.GetGeometryType() == ogr.wkbCurvePolygon
        ):
          wkt_expected = wkt
        self.assertEqual(got_wkt, wkt_expected, (wkt, wkt_target))

    g = ogr.ForceTo(
        ogr.CreateGeometryFromWkt('POLYGON ((0 0,0 1,1 1,1 0,0 0))'),
        ogr.wkbTriangle,
    )
    self.assertEqual(g.GetGeometryType(), ogr.wkbPolygon, g.ExportToWkt())

    g = ogr.ForceTo(
        ogr.CreateGeometryFromWkt(
            'POLYHEDRALSURFACE (((0 0,0 1,1 1,1 0,0 0)))'
        ),
        ogr.wkbTIN,
    )
    self.assertEqual(
        g.GetGeometryType(), ogr.wkbPolyhedralSurface, g.ExportToWkt()
    )

    conversions = (
        (
            'GEOMETRYCOLLECTION (POLYHEDRALSURFACE (((0 0,0 1,1 1,1 0,0 0))))',
            ogr.wkbMultiPolygon,
            'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))',
        ),
        (
            'GEOMETRYCOLLECTION (TRIANGLE ((0 0,0 1,1 1,0 0)))',
            ogr.wkbMultiPolygon,
            'MULTIPOLYGON (((0 0,0 1,1 1,0 0)))',
        ),
        (
            'MULTIPOLYGON (((0 0,0 1,1 1,0 0)))',
            ogr.wkbGeometryCollection,
            'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,0 0)))',
        ),
        (
            'TRIANGLE ((0 0,0 1,1 1,0 0))',
            ogr.wkbGeometryCollection,
            'GEOMETRYCOLLECTION (TRIANGLE ((0 0,0 1,1 1,0 0)))',
        ),
        (
            'TIN (((0 0,0 1,1 1,0 0)))',
            ogr.wkbGeometryCollection,
            'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,0 0)))',
        ),
        (
            'POLYHEDRALSURFACE (((0 0,0 1,1 1,0 0)))',
            ogr.wkbGeometryCollection,
            'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,0 0)))',
        ),
    )
    for src_wkt, dst_type, expected_wkt in conversions:
      g = ogr.ForceTo(ogr.CreateGeometryFromWkt(src_wkt), dst_type)
      self.assertEqual(g.ExportToWkt(), expected_wkt)

  def test_ogr_geom_multipoint_envelope_bug(self):
    g = ogr.CreateGeometryFromWkt('MULTIPOINT(0 0,1 1)')
    self.assertEqual(g.GetEnvelope(), (0, 1, 0, 1))

    g = ogr.CreateGeometryFromWkt('MULTIPOINT(0 0 0,1 1 1)')
    self.assertEqual(g.GetEnvelope3D(), (0, 1, 0, 1, 0, 1))

  def test_ogr_geom_polygon_empty_ring(self):
    g = ogr.Geometry(ogr.wkbPolygon)
    g2 = ogr.Geometry(ogr.wkbLinearRing)
    g.AddGeometryDirectly(g2)
    self.assertTrue(g.IsEmpty())

  def test_ogr_geom_polygon_intersects_point(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    poly = ogr.CreateGeometryFromWkt('POLYGON((0 0,5 5,10 0,0 0))')
    point = ogr.Geometry(ogr.wkbPoint)
    point.AddPoint(10, 0)
    self.assertEqual(poly.Intersects(point), 1)
    self.assertEqual(poly.Contains(point), 0)

  def test_ogr_geom_geometrycollection(self):
    wkt_list = (
        'GEOMETRYCOLLECTION (POINT EMPTY)',
        'GEOMETRYCOLLECTION (LINESTRING EMPTY)',
        'GEOMETRYCOLLECTION (POLYGON EMPTY)',
        'GEOMETRYCOLLECTION (MULTIPOINT EMPTY)',
        'GEOMETRYCOLLECTION (MULTILINESTRING EMPTY)',
        'GEOMETRYCOLLECTION (MULTIPOLYGON EMPTY)',
        'GEOMETRYCOLLECTION (GEOMETRYCOLLECTION EMPTY)',
        'GEOMETRYCOLLECTION (CIRCULARSTRING EMPTY)',
        'GEOMETRYCOLLECTION (COMPOUNDCURVE EMPTY)',
        'GEOMETRYCOLLECTION (CURVEPOLYGON EMPTY)',
        'GEOMETRYCOLLECTION (MULTICURVE EMPTY)',
        'GEOMETRYCOLLECTION (MULTISURFACE EMPTY)',
        'GEOMETRYCOLLECTION (TRIANGLE EMPTY)',
        'GEOMETRYCOLLECTION (POLYHEDRALSURFACE EMPTY)',
        'GEOMETRYCOLLECTION (TIN EMPTY)',
    )
    for wkt in wkt_list:
      g = ogr.CreateGeometryFromWkt(wkt)
      self.assertEqual(g.ExportToWkt(), wkt)

  def test_ogr_geom_assignspatialref(self):
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(POLYGON((0 0,0 1,1 1,0 0)),'
        'COMPOUNDCURVE(CIRCULARSTRING(0 0,1 1,2 0)),'
        'POLYHEDRALSURFACE(((0 0,0 1,1 1,0 0))))'
    )
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    g.AssignSpatialReference(sr)
    sr_wkt = sr.ExportToWkt()
    for i in range(3):
      self.assertEqual(
          g.GetGeometryRef(i).GetSpatialReference().ExportToWkt(), sr_wkt
      )
      self.assertEqual(
          g.GetGeometryRef(i)
          .GetGeometryRef(0)
          .GetSpatialReference()
          .ExportToWkt(),
          sr_wkt,
      )

  def test_ogr_geom_swapxy(self):
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(POINT(1 2),LINESTRING(1 2,2 3),'
        'POLYGON((0 0,0 1,1 1,0 0)),'
        'COMPOUNDCURVE(CIRCULARSTRING(0 0,1 1,2 0)),'
        'POLYHEDRALSURFACE(((0 0,0 1,1 1,0 0))))'
    )
    g.SwapXY()
    self.assertEqual(
        g.ExportToWkt(),
        'GEOMETRYCOLLECTION (POINT (2 1),LINESTRING (2 1,3 2),'
        'POLYGON ((0 0,1 0,1 1,0 0)),'
        'COMPOUNDCURVE (CIRCULARSTRING (0 0,1 1,0 2)),'
        'POLYHEDRALSURFACE (((0 0,1 0,1 1,0 0))))',
    )

  def test_ogr_geom_remove_geometry(self):
    # With GEOMETRYCOLLECTION
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION (POINT (1 2),LINESTRING (1 2,2 3),POINT (3 4))'
    )
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertNotEqual(g.RemoveGeometry(3), 0)
      self.assertNotEqual(g.RemoveGeometry(-2), 0)

    self.assertEqual(g.RemoveGeometry(1), 0)
    self.assertEqual(
        g.ExportToWkt(), 'GEOMETRYCOLLECTION (POINT (1 2),POINT (3 4))'
    )
    self.assertEqual(g.RemoveGeometry(1), 0)
    self.assertEqual(g.ExportToWkt(), 'GEOMETRYCOLLECTION (POINT (1 2))')
    self.assertEqual(g.RemoveGeometry(0), 0)
    self.assertEqual(g.ExportToWkt(), 'GEOMETRYCOLLECTION EMPTY')

    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION (POINT (1 2),LINESTRING (1 2,2 3),POINT (3 4))'
    )
    self.assertEqual(g.RemoveGeometry(-1), 0)
    self.assertEqual(g.ExportToWkt(), 'GEOMETRYCOLLECTION EMPTY')

    # With POLYHEDRALSURFACE/TIN
    g = ogr.CreateGeometryFromWkt(
        'TIN (((0 0,0 1,1 1,0 0)),((0 0,1 0,1 1,0 0)))'
    )
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertNotEqual(g.RemoveGeometry(2), 0)
      self.assertNotEqual(g.RemoveGeometry(-2), 0)

    self.assertEqual(g.RemoveGeometry(1), 0)
    self.assertEqual(g.ExportToWkt(), 'TIN (((0 0,0 1,1 1,0 0)))')
    self.assertEqual(g.RemoveGeometry(0), 0)
    self.assertEqual(g.ExportToWkt(), 'TIN EMPTY')

    g = ogr.CreateGeometryFromWkt(
        'TIN (((0 0,0 1,1 1,0 0)),((0 0,1 0,1 1,0 0)))'
    )
    self.assertEqual(g.RemoveGeometry(-1), 0)
    self.assertEqual(g.ExportToWkt(), 'TIN EMPTY')

    # With POLYGON
    g = ogr.CreateGeometryFromWkt(
        'POLYGON ((0 0,0 10,10 10,10 0,0 0),(1 1,1 9,9 9,1 1))'
    )
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertNotEqual(g.RemoveGeometry(2), 0)
      self.assertNotEqual(g.RemoveGeometry(-2), 0)

    self.assertEqual(g.RemoveGeometry(1), 0)
    self.assertEqual(g.ExportToWkt(), 'POLYGON ((0 0,0 10,10 10,10 0,0 0))')
    self.assertEqual(g.RemoveGeometry(0), 0)
    self.assertEqual(g.ExportToWkt(), 'POLYGON EMPTY')

    g = ogr.CreateGeometryFromWkt(
        'POLYGON ((0 0,0 10,10 10,10 0,0 0),(1 1,1 9,9 9,1 1))'
    )
    self.assertEqual(g.RemoveGeometry(-1), 0)
    self.assertEqual(g.ExportToWkt(), 'POLYGON EMPTY')

    # Unsupported type
    g = ogr.CreateGeometryFromWkt('POINT (0 0)')
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertNotEqual(g.RemoveGeometry(0), 0)

  def test_ogr_geom_sfcgal(self):
    if not _have_sfcgal():
      self.skipTest('SFCGAL is not available')

    g1 = ogr.CreateGeometryFromWkt('TIN EMPTY')
    g2_poly = ogr.CreateGeometryFromWkt('POLYGON((0 0,0 1,1 1,0 0))')
    g1.Distance(g2_poly.GetGeometryRef(0))
    g1.Distance(ogr.CreateGeometryFromWkt('CIRCULARSTRING EMPTY'))
    g1.Distance(ogr.CreateGeometryFromWkt('CURVEPOLYGON EMPTY'))

  def test_ogr_geom_sfcgal_distance3D(self):
    if not _have_sfcgal():
      self.skipTest('SFCGAL is not available')

    point1 = ogr.CreateGeometryFromWkt('POINT (1.0 1.0 1.0)')
    point2 = ogr.CreateGeometryFromWkt('POINT (4.0 1.0 5.0)')
    self.assertEqual(point1.Distance3D(point2), 5.0)

  def test_ogr_geom_sfcgal_intersection3D(self):
    if not _have_sfcgal():
      self.skipTest('SFCGAL is not available')

    phsurface = ogr.CreateGeometryFromWkt(
        'POLYHEDRALSURFACE Z (((0 0 0,0 0 2,0 2 2,0 2 0,0 0 0)),'
        '((0 0 0,0 2 0,2 2 0,2 0 0,0 0 0)),'
        '((0 0 0,2 0 0,2 0 2,0 0 2,0 0 0)),'
        '((2 2 0,2 2 2,2 0 2,2 0 0,2 2 0)),'
        '((0 2 0,0 2 2,2 2 2,2 2 0,0 2 0)),'
        '((0 0 2,2 0 2,2 2 2,0 2 2,0 0 2)))'
    )
    line = ogr.CreateGeometryFromWkt('LINESTRING Z (-1 1 1, 3 1 1)')
    result = phsurface.Intersection(line)
    self.assertEqual(result.ExportToWkt(), 'MULTIPOINT (0 1 1,2 1 1)')

  def test_ogr_geom_cannot_add_triangle_to_multisurface(self):
    g = ogr.Geometry(ogr.wkbMultiSurface)
    with _disable_exceptions(), gdal.quiet_errors():
      self.assertNotEqual(g.AddGeometry(ogr.Geometry(ogr.wkbTriangle)), 0)

  def test_ogr_geom_force_polygonzm_to_linestring(self):
    g = ogr.CreateGeometryFromWkt(
        'POLYGON ZM ((0 0 10 20,0 1 30 40,1 1 50 60,0 0 10 70))'
    )
    wkt = ogr.ForceToLineString(g).ExportToIsoWkt()
    self.assertEqual(
        wkt, 'LINESTRING ZM (0 0 10 20,0 1 30 40,1 1 50 60,0 0 10 70)'
    )

  def test_ogr_geom_force_polygonzm_to_multilinestring(self):
    g = ogr.CreateGeometryFromWkt(
        'POLYGON ZM ((0 0 10 20,0 1 30 40,1 1 50 60,0 0 10 70))'
    )
    wkt = ogr.ForceToMultiLineString(g).ExportToIsoWkt()
    self.assertEqual(
        wkt, 'MULTILINESTRING ZM ((0 0 10 20,0 1 30 40,1 1 50 60,0 0 10 70))'
    )

  def test_ogr_geom_create_from_wkt_polyhedrasurface(self):
    g = ogr.CreateGeometryFromWkt(
        'POLYHEDRALSURFACE (((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)))'
    )
    self.assertEqual(
        g.ExportToWkt(),
        'POLYHEDRALSURFACE Z (((0 0 0,0 0 1,0 1 1,0 1 0,0 0 0)))',
    )

  def test_ogr_geom_makevalid(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    g = ogr.CreateGeometryFromWkt('POINT (0 0)').MakeValid()
    self.assertTrue(g is None or g.ExportToWkt() == 'POINT (0 0)')

    g = ogr.CreateGeometryFromWkt('POINT EMPTY').MakeValid()
    self.assertTrue(g is None or g.ExportToWkt() == 'POINT EMPTY')

    g = ogr.CreateGeometryFromWkt('LINESTRING (0 0)')
    with _disable_exceptions(), gdal.quiet_errors():
      g = g.MakeValid()
    self.assertFalse(g)

    g = ogr.CreateGeometryFromWkt(
        'CURVEPOLYGON ((0 0,0 1,1 0,0 0))'
    ).MakeValid()
    self.assertTrue(
        g is None or g.ExportToWkt() == 'CURVEPOLYGON ((0 0,0 1,1 0,0 0))'
    )

    # Invalid
    expected_bowtie = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON (((0 0,5 5,10 0,0 0)),((5 5,0 10,10 10,5 5)))'
    ).Normalize()
    for wkt in (
        'POLYGON ((0 0,10 10,0 10,10 0,0 0))',
        'CURVEPOLYGON ((0 0,10 10,0 10,10 0,0 0))',
    ):
      g = ogr.CreateGeometryFromWkt(wkt).MakeValid()
      if g is not None:
        self.assertTrue(g.IsValid())
        normalized = g.Normalize()
        self.assertTrue(normalized.Equals(expected_bowtie))
        self._check_geometry(normalized, expected_bowtie)

  def test_ogr_geom_makevalid_structure(self):
    if not _have_geos() or _geos_version() < (3, 10, 0):
      self.skipTest('GEOS >= 3.10.0 is not available')

    g = ogr.CreateGeometryFromWkt(
        'POLYGON ((0 0,0 10,10 10,10 0,0 0),(5 5,15 10,15 0,5 5))'
    )
    g = g.MakeValid(['METHOD=STRUCTURE'])
    self._check_geometry(
        g, 'POLYGON ((0 10,10 10,10.0 7.5,5 5,10.0 2.5,10 0,0 0,0 10))'
    )

    # Already valid multi-polygon made of a single-part
    g = ogr.CreateGeometryFromWkt('MULTIPOLYGON (((0 0,1 0,1 1,0 1,0 0)))')
    g = g.MakeValid(['METHOD=STRUCTURE'])
    self.assertIn(
        g.ExportToIsoWkt(),
        (
            'MULTIPOLYGON (((0 0,1 0,1 1,0 1,0 0)))',
            'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))',
        ),
    )

    # Already valid multi-polygon made of a single-part, with duplicated point
    g = ogr.CreateGeometryFromWkt('MULTIPOLYGON (((0 0,1 0,1 0,1 1,0 1,0 0)))')
    g = g.MakeValid(['METHOD=STRUCTURE'])
    self.assertIn(
        g.ExportToIsoWkt(),
        (
            'MULTIPOLYGON (((0 0,1 0,1 1,0 1,0 0)))',
            'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))',
        ),
    )

    # Already valid multi-polygon made of a single-part (Z)
    g = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON Z (((0 0 10,1 0 10,1 1 10,0 1 10,0 0 10)))'
    )
    g = g.MakeValid(['METHOD=STRUCTURE'])
    self.assertIn(
        g.ExportToIsoWkt(),
        (
            'MULTIPOLYGON Z (((0 0 10,1 0 10,1 1 10,0 1 10,0 0 10)))',
            'MULTIPOLYGON Z (((0 0 10,0 1 10,1 1 10,1 0 10,0 0 10)))',
        ),
    )

    # Already valid geometry collection
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION (POLYGON ((0 0,1 0,1 1,0 1,0 0)))'
    )
    g = g.MakeValid(['METHOD=STRUCTURE'])
    self.assertIn(
        g.ExportToIsoWkt(),
        (
            'GEOMETRYCOLLECTION (POLYGON ((0 0,1 0,1 1,0 1,0 0)))',
            'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,1 0,0 0)))',
        ),
    )

  def test_ogr_geom_normalize(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    g = ogr.CreateGeometryFromWkt('POLYGON ((0 1,1 1,1 0,0 0,0 1))').Normalize()
    self.assertTrue(
        g is None or g.ExportToWkt() == 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'
    )

  def test_ogr_geom_force_multipolygon_z_to_compound_curve(self):
    g = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON Z (((0 0 0,0 1 0,1 1 0,0 0 0)))'
    )
    g = ogr.ForceTo(g, ogr.wkbCompoundCurveZ)
    self.assertEqual(
        g.ExportToIsoWkt(), 'COMPOUNDCURVE Z ((0 0 0,0 1 0,1 1 0,0 0 0))'
    )

  @parameterized.parameters(
      ('POINT EMPTY', 'POINT EMPTY'),
      ('POINT (1 2)', 'POINT (1 2)'),
      ('POINT Z (1 2 3)', 'POINT Z (1 2 3)'),
      ('LINESTRING (1 2,3 4)', 'LINESTRING (1 2,3 4)'),
      ('POLYGON ((0 0,0 1,1 1,0 0))', 'POLYGON ((0 0,0 1,1 1,0 0))'),
      ('GEOMETRYCOLLECTION EMPTY', 'GEOMETRYCOLLECTION EMPTY'),
      ('GEOMETRYCOLLECTION Z (POINT Z (1 2 3))', 'POINT Z (1 2 3)'),
      ('GEOMETRYCOLLECTION (LINESTRING (1 2,3 4))', 'LINESTRING (1 2,3 4)'),
      (
          'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,0 0)))',
          'POLYGON ((0 0,0 1,1 1,0 0))',
      ),
      (
          'GEOMETRYCOLLECTION (LINESTRING (1 2,3 4),POINT (1 2))',
          'LINESTRING (1 2,3 4)',
      ),
      (
          'GEOMETRYCOLLECTION (POINT (1 2),LINESTRING (1 2,3 4))',
          'LINESTRING (1 2,3 4)',
      ),
      (
          'GEOMETRYCOLLECTION (LINESTRING (1 2,3 4),LINESTRING (5 6,7 8))',
          'MULTILINESTRING ((1 2,3 4),(5 6,7 8))',
      ),
      (
          'GEOMETRYCOLLECTION (POINT (1 2),POLYGON ((0 0,0 1,1 1,0 0)))',
          'POLYGON ((0 0,0 1,1 1,0 0))',
      ),
      (
          (
              'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,0 0)),'
              'POLYGON ((10 0,10 1,11 1,10 0)))'
          ),
          'MULTIPOLYGON (((0 0,0 1,1 1,0 0)),((10 0,10 1,11 1,10 0)))',
      ),
      (
          (
              'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,0 0)),'
              'MULTIPOLYGON (((10 0,10 1,11 1,10 0))))'
          ),
          'MULTIPOLYGON (((0 0,0 1,1 1,0 0)),((10 0,10 1,11 1,10 0)))',
      ),
      (
          (
              'GEOMETRYCOLLECTION (CIRCULARSTRING (0 0,1 1,2 0),'
              'LINESTRING(3 4,5 6))'
          ),
          'MULTICURVE (CIRCULARSTRING (0 0,1 1,2 0),(3 4,5 6))',
      ),
      (
          (
              'GEOMETRYCOLLECTION (POLYGON ((0 0,0 1,1 1,0 0)),'
              'CURVEPOLYGON ((10 0,10 1,11 1,10 0)))'
          ),
          (
              'MULTISURFACE (((0 0,0 1,1 1,0 0)),'
              'CURVEPOLYGON ((10 0,10 1,11 1,10 0)))'
          ),
      ),
      (
          (
              'GEOMETRYCOLLECTION (MULTIPOLYGON (((0 0,0 1,1 1,0 0)),'
              '((10 0,10 1,11 1,10 0))),POINT (1 2))'
          ),
          'MULTIPOLYGON (((0 0,0 1,1 1,0 0)),((10 0,10 1,11 1,10 0)))',
      ),
  )
  def test_ogr_geom_removeLowerDimensionSubGeoms(self, input_wkt, expected_wkt):
    g = ogr.CreateGeometryFromWkt(input_wkt).RemoveLowerDimensionSubGeoms()
    self.assertEqual(g.ExportToIsoWkt(), expected_wkt)

  def test_ogr_geom_copy(self):
    g = ogr.CreateGeometryFromWkt('POINT (0 1)')
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    g.AssignSpatialReference(sr)

    g2 = copy.copy(g)
    self.assertEqual(g2.ExportToWkt(), g.ExportToWkt())
    self.assertTrue(g2.GetSpatialReference().IsSame(sr))
    sr.ImportFromEPSG(32631)
    self.assertTrue(g2.GetSpatialReference().IsSame(sr))

  def test_ogr_geom_deepcopy(self):
    g = ogr.CreateGeometryFromWkt('POINT (0 1)')
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(4326)
    g.AssignSpatialReference(sr)

    g2 = copy.deepcopy(g)
    self.assertEqual(g2.ExportToWkt(), g.ExportToWkt())
    self.assertTrue(g2.GetSpatialReference().IsSame(sr))
    sr.ImportFromEPSG(32631)
    self.assertFalse(g2.GetSpatialReference().IsSame(sr))

  def test_ogr_geom_point_nan(self):
    geom = ogr.Geometry(type=ogr.wkbPoint)
    geom.AddPoint_2D(float('nan'), float('nan'))
    self.assertTrue(geom.IsEmpty())

  def test_ogr_isclockwise(self):
    geom = ogr.Geometry(type=ogr.wkbPoint)
    with self.assertRaisesRegex(
        RuntimeError, 'Incompatible geometry for operation'
    ):
      geom.IsClockwise()

    for wkt, expected in (
        ('POLYGON((10 100,10 101,11 101,10 100))', True),
        ('POLYGON((10 100,11 100,11 101,10 100))', False),
        ('POLYGON((1334 547,1341 551,1338 557,1334 547))', False),
        ('POLYGON((1341 551,1338 557,1334 547,1341 551))', False),
        ('POLYGON((1338 557,1334 547,1341 551,1338 557))', False),
        ('POLYGON((1334 547,1338 557,1341 551,1334 547))', True),
        ('POLYGON((1341 551,1334 547,1338 557,1341 551))', True),
        ('POLYGON((1338 557,1341 551,1334 547,1338 557))', True),
        # Two vertices with same coordinates are the lowest rightmost vertex.
        ('POLYGON((1334 547,1334 547,1341 551,1338 557,1334 547))', False),
        ('POLYGON((1334 547,1341 551,1338 557,1334 547,1334 547))', False),
        ('POLYGON((1341 551,1338 557,1334 547,1334 547,1341 551))', False),
        ('POLYGON((1334 547,1338 557,1341 551,1334 547,1334 547))', True),
    ):
      ring = ogr.CreateGeometryFromWkt(wkt).GetGeometryRef(0)
      self.assertEqual(bool(ring.IsClockwise()), expected, wkt)

  def test_ogr_isclockwise_on_compoundcurve(self):
    for wkt, expected in (
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1334 547,1341 551,1338 557,1334 547)))'
            ),
            False,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1341 551,1338 557,1334 547,1341 551)))'
            ),
            False,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1338 557,1334 547,1341 551,1338 557)))'
            ),
            False,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1334 547,1338 557,1341 551,1334 547)))'
            ),
            True,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1341 551,1334 547,1338 557,1341 551)))'
            ),
            True,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1338 557,1341 551,1334 547,1338 557)))'
            ),
            True,
        ),
        # Two vertices with same coordinates are the lowest rightmost vertex.
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1334 547,1334 547,1341 551,1338 557,1334 547)))'
            ),
            False,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1334 547,1341 551,1338 557,1334 547,1334 547)))'
            ),
            False,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1341 551,1338 557,1334 547,1334 547,1341 551)))'
            ),
            False,
        ),
        (
            (
                'CURVEPOLYGON(COMPOUNDCURVE('
                '(1334 547,1338 557,1341 551,1334 547,1334 547)))'
            ),
            True,
        ),
    ):
      ring = ogr.CreateGeometryFromWkt(wkt).GetGeometryRef(0)
      self.assertEqual(bool(ring.IsClockwise()), expected, wkt)

  @parameterized.parameters(
      ogr.wkbPoint,
      ogr.wkbPoint25D,
      ogr.wkbPointM,
      ogr.wkbPointZM,
      ogr.wkbLineString,
      ogr.wkbPolygon,
      ogr.wkbMultiPoint,
      ogr.wkbMultiLineString,
      ogr.wkbMultiPolygon,
      ogr.wkbGeometryCollection,
      ogr.wkbCircularString,
      ogr.wkbCompoundCurve,
      ogr.wkbCurvePolygon,
      ogr.wkbMultiCurve,
      ogr.wkbMultiSurface,
      ogr.wkbPolyhedralSurface,
      ogr.wkbTIN,
  )
  def test_ogr_geom_CreateGeometry(self, geom_type):
    geom = ogr.Geometry(geom_type)
    self.assertEqual(geom.GetGeometryType(), geom_type)
    self.assertTrue(geom.IsEmpty())

  def test_ogr_geom_GeodesicArea(self):
    # Lat, lon order
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicArea(), 4068384291.8911743)

    # Lon, lat order
    g = ogr.CreateGeometryFromWkt('POLYGON((2 49,3 49,3 48,2 49))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicArea(), 4068384291.8911743)

    # Lon, lat order
    g = ogr.CreateGeometryFromWkt('POLYGON((12 49,13 49,13 48,12 49))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicArea(), 4068384291.8911743)

    # Lon, lat order
    g = ogr.CreateGeometryFromWkt('POLYGON((2 89,3 89,3 88,2 89))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicArea(), 108860488.12023926)

    # Easting, northing
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    other_srs = osr.SpatialReference()
    other_srs.ImportFromEPSG(32631)
    g.TransformTo(other_srs)
    self.assertAlmostEqual(
        g.GeodesicArea(), 4068384291.8911743, delta=4068384291.8911743 * 1e-6
    )
    # For comparison: cartesian area in UTM.
    self.assertAlmostEqual(
        g.Area(), 4065070548.465351, delta=4065070548.465351 * 1e-6
    )

    # POLYGON with hole
    g = ogr.CreateGeometryFromWkt(
        'POLYGON((49 2,49 3,48 3,49 2),(49 2,49 3,48 3,49 2))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicArea(), 0)

    # MULTIPOLYGON
    g = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON(((49 2,49 3,48 3,49 2)),((89 2,89 3,88 3,89 2)))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(
        g.GeodesicArea(), 4068384291.8911743 + 108860488.12023926
    )

    # POLYGON EMPTY
    g = ogr.CreateGeometryFromWkt('POLYGON EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicArea(), 0)

    # GEOMETRYCOLLECTION
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(GEOMETRYCOLLECTION(POLYGON((49 2,49 3,48 3,49 2))),'
        'LINESTRING(89 2,89 3,88 3,89 2))))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(
        g.GeodesicArea(), 4068384291.8911743 + 108860488.12023926
    )

    # CIRCULARSTRING
    g = ogr.CreateGeometryFromWkt('CIRCULARSTRING(0 0,1 1,2 0,1 -1,0 0)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicArea(), ogr.ForceToLineString(g).GeodesicArea())

    # CIRCULARSTRING EMPTY
    g = ogr.CreateGeometryFromWkt('CIRCULARSTRING EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicArea(), 0)

    # CIRCULARSTRING not closed
    g = ogr.CreateGeometryFromWkt('CIRCULARSTRING(0 0,1 1,2 0)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(RuntimeError, 'Non-closed geometry'):
      g.GeodesicArea()

    # GEOMETRYCOLLECTION of CIRCULARSTRING not closed
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(CIRCULARSTRING(0 0,1 1,2 0))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(RuntimeError, 'Non-closed geometry'):
      g.GeodesicArea()

    # COMPOUNDCURVE
    g = ogr.CreateGeometryFromWkt(
        'COMPOUNDCURVE(CIRCULARSTRING(0 0,1 1,2 0,1 -1,0 0))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicArea(), ogr.ForceToLineString(g).GeodesicArea())

    # COMPOUNDCURVE EMPTY
    g = ogr.CreateGeometryFromWkt('COMPOUNDCURVE EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicArea(), 0)

    # COMPOUNDCURVE not closed
    g = ogr.CreateGeometryFromWkt('COMPOUNDCURVE(CIRCULARSTRING(0 0,1 1,2 0))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(RuntimeError, 'Non-closed geometry'):
      g.GeodesicArea()

    # POLYHEDRALSURFACE EMPTY
    g = ogr.CreateGeometryFromWkt('POLYHEDRALSURFACE EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicArea(), 0)

    # POLYHEDRALSURFACE
    g = ogr.CreateGeometryFromWkt(
        'POLYHEDRALSURFACE(((0 0 0,0 1 0,1 1 0,0 0 0)))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(
        RuntimeError, 'not implemented for PolyhedralSurface'
    ):
      g.GeodesicArea()

    # GEOMETRYCOLLECTION of POLYHEDRALSURFACE
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(GEOMETRYCOLLECTION('
        'POLYHEDRALSURFACE(((0 0 0,0 1 0,1 1 0,0 0 0)))))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(
        RuntimeError, 'not implemented for PolyhedralSurface'
    ):
      g.GeodesicArea()

    # Incompatible geometry type
    g = ogr.CreateGeometryFromWkt('POINT(0 1)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(RuntimeError, 'non-surface geometry type'):
      g.GeodesicArea()

    # No SRS
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    with self.assertRaisesRegex(
        RuntimeError, 'Cannot compute area on ellipsoid due to missing SRS'
    ):
      g.GeodesicArea()

    # Engineering SRS
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    srs = osr.SpatialReference()
    srs.SetFromUserInput('LOCAL_CS["dummy"]')
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(RuntimeError, 'CRS has no geodetic CRS'):
      g.GeodesicArea()

  def test_ogr_geom_GeodesicLength(self):
    # Lat, lon order, not forming a polygon
    g = ogr.CreateGeometryFromWkt('LINESTRING(49 2,49 3)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    l1 = g.GeodesicLength()
    self.assertAlmostEqual(l1, 73171.26435678436)

    g = ogr.CreateGeometryFromWkt('LINESTRING(49 3,48 3)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    l2 = g.GeodesicLength()
    self.assertAlmostEqual(l2, 111200.0367623785)

    g = ogr.CreateGeometryFromWkt('LINESTRING(48 3,49 2)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    l3 = g.GeodesicLength()
    self.assertAlmostEqual(l3, 133514.4852804854)

    # Lat, lon order
    g = ogr.CreateGeometryFromWkt('LINESTRING(49 2,49 3,48 3,49 2)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicLength(), l1 + l2 + l3)

    # Lat, lon order
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicLength(), 317885.78639964823)

    # Lon, lat order
    g = ogr.CreateGeometryFromWkt('POLYGON((2 49,3 49,3 48,2 49))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicLength(), 317885.78639964823)

    # Lon, lat order
    g = ogr.CreateGeometryFromWkt('POLYGON((12 49,13 49,13 48,12 49))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicLength(), 317885.78639964823)

    # Lon, lat order
    g = ogr.CreateGeometryFromWkt('POLYGON((2 89,3 89,3 88,2 89))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicLength(), 225369.66747743438)

    # Easting, northing
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    other_srs = osr.SpatialReference()
    other_srs.ImportFromEPSG(32631)
    g.TransformTo(other_srs)
    self.assertAlmostEqual(g.GeodesicLength(), 317885.78639964823)
    # For comparison: cartesian length in UTM.
    self.assertAlmostEqual(g.Length(), 317763.15996565996)

    # POLYGON with hole
    g = ogr.CreateGeometryFromWkt(
        'POLYGON((49 2,49 3,48 3,49 2),(49 2,49 3,48 3,49 2))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(g.GeodesicLength(), 635771.5727992965)

    # MULTIPOLYGON
    g = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON(((49 2,49 3,48 3,49 2)),((89 2,89 3,88 3,89 2)))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(
        g.GeodesicLength(), 317885.78639964823 + 225369.66747743438
    )

    # POLYGON EMPTY
    g = ogr.CreateGeometryFromWkt('POLYGON EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicLength(), 0)

    # GEOMETRYCOLLECTION
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(GEOMETRYCOLLECTION(POLYGON((49 2,49 3,48 3,49 2))),'
        'LINESTRING(89 2,89 3,88 3,89 2))))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertAlmostEqual(
        g.GeodesicLength(), 317885.78639964823 + 225369.66747743438
    )

    # CIRCULARSTRING
    g = ogr.CreateGeometryFromWkt('CIRCULARSTRING(0 0,1 1,2 0,1 -1,0 0)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(
        g.GeodesicLength(), ogr.ForceToLineString(g).GeodesicLength()
    )

    # CIRCULARSTRING EMPTY
    g = ogr.CreateGeometryFromWkt('CIRCULARSTRING EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicLength(), 0)

    # COMPOUNDCURVE
    g = ogr.CreateGeometryFromWkt(
        'COMPOUNDCURVE(CIRCULARSTRING(0 0,1 1,2 0,1 -1,0 0))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(
        g.GeodesicLength(), ogr.ForceToLineString(g).GeodesicLength()
    )

    # COMPOUNDCURVE EMPTY
    g = ogr.CreateGeometryFromWkt('COMPOUNDCURVE EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicLength(), 0)

    # POLYHEDRALSURFACE EMPTY
    g = ogr.CreateGeometryFromWkt('POLYHEDRALSURFACE EMPTY')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    self.assertEqual(g.GeodesicLength(), 0)

    # POLYHEDRALSURFACE
    g = ogr.CreateGeometryFromWkt(
        'POLYHEDRALSURFACE(((0 0 0,0 1 0,1 1 0,0 0 0)))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(
        RuntimeError, 'not implemented for PolyhedralSurface'
    ):
      g.GeodesicLength()

    # GEOMETRYCOLLECTION of POLYHEDRALSURFACE
    g = ogr.CreateGeometryFromWkt(
        'GEOMETRYCOLLECTION(GEOMETRYCOLLECTION('
        'POLYHEDRALSURFACE(((0 0 0,0 1 0,1 1 0,0 0 0)))))'
    )
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(
        RuntimeError, 'not implemented for PolyhedralSurface'
    ):
      g.GeodesicLength()

    # Incompatible geometry type
    g = ogr.CreateGeometryFromWkt('POINT(0 1)')
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(RuntimeError, 'non-curve geometry type'):
      g.GeodesicLength()

    # No SRS
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    with self.assertRaisesRegex(
        RuntimeError, 'Cannot compute length on ellipsoid due to missing SRS'
    ):
      g.GeodesicLength()

    # Engineering SRS
    g = ogr.CreateGeometryFromWkt('POLYGON((49 2,49 3,48 3,49 2))')
    srs = osr.SpatialReference()
    srs.SetFromUserInput('LOCAL_CS["dummy"]')
    g.AssignSpatialReference(srs)
    with self.assertRaisesRegex(RuntimeError, 'CRS has no geodetic CRS'):
      g.GeodesicLength()

  def test_ogr_geom_buffer_with_args(self):
    if not _have_geos():
      self.skipTest('GEOS is not available')

    geom = ogr.CreateGeometryFromWkt('LINESTRING (0 0, 10 10, 20 0)')
    baseline = geom.Buffer(1, 32)
    smooth = geom.Buffer(1, {'QUADRANT_SEGMENTS': 64})
    self.assertGreater(
        smooth.GetGeometryRef(0).GetPointCount(),
        baseline.GetGeometryRef(0).GetPointCount(),
    )

    one_sided = geom.Buffer(1, {'QUADRANT_SEGMENTS': 32, 'SINGLE_SIDED': 'YES'})
    self.assertLess(one_sided.GetArea(), baseline.GetArea())

    modified = geom.Buffer(
        1,
        {
            'QUADRANT_SEGMENTS': 32,
            'JOIN_STYLE': 'MITRE',
            'ENDCAP_STYLE': 'FLAT',
            'MITRE_LIMIT': '0.25',
        },
    )
    self.assertNotEqual(modified.ExportToWkt(), baseline.ExportToWkt())

    with self.assertRaisesRegex(RuntimeError, 'Invalid value for JOIN_STYLE'):
      geom.Buffer(1, {'JOIN_STYLE': 'SMOOTH'})

    with self.assertRaisesRegex(
        RuntimeError, 'Invalid value for ENDCAP_STYLE'
    ):
      geom.Buffer(1, {'ENDCAP_STYLE': 'AVANT_GARDE'})

    for bad_segments in ('LOTS', ' ', '', '10000000000000'):
      with self.assertRaisesRegex(
          RuntimeError, 'Invalid value for QUADRANT_SEGMENTS'
      ):
        geom.Buffer(1, {'QUADRANT_SEGMENTS': bad_segments})

    for bad_mitre in ('', ' '):
      with self.assertRaisesRegex(
          RuntimeError, 'Invalid value for MITRE_LIMIT'
      ):
        geom.Buffer(1, {'MITRE_LIMIT': bad_mitre})

    with self.assertRaisesRegex(RuntimeError, 'Unsupported buffer option'):
      geom.Buffer(1, {'QUALITY': 'HIGH'})

  def test_ogr_subgeom_use_after_parent_free(self):
    g = ogr.CreateGeometryFromWkt('POLYGON ((0 0, 1 0, 1 1, 0 0))')
    exterior_ring = g.GetGeometryRef(0)
    del g
    self.assertGreater(exterior_ring.GetPointCount(), 0)

  def test_ogr_geom_create_from_envelope(self):
    srs = osr.SpatialReference(epsg=32145)
    geom = ogr.CreateGeometryFromEnvelope(424788, 25211, 581555, 279799, srs)
    self.assertTrue(geom.GetSpatialReference().IsSame(srs))
    self.assertEqual(
        geom.ExportToWkt(),
        'POLYGON ((424788 25211,424788 279799,581555 279799,581555 25211,'
        '424788 25211))',
    )


if __name__ == '__main__':
  googletest.main()
