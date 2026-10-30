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

This is a port of the ogr_shape.py test file from:
  https://github.com/OSGeo/gdal/blob/166eaf63c98eebb727cb66f5afd304228c680c1d/autotest/ogr/ogr_shape.py

Tests were ported from the upstream file with the help of LLMs.
"""

from collections.abc import Iterator, Sequence
import contextlib
import decimal
import importlib.util
import math
import os
import shutil
import stat
import struct
import sys
import time
import unittest

from osgeo import gdal
from osgeo import gdal_array
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

DRIVER = ogr_util.SHAPEFILE_DRIVER
EXT = '.shp'


def setUpModule() -> None:
  ogr_util.SetupTestEnv()
  gdal.DontUseExceptions()


def _have_geos() -> bool:
  point1 = ogr.CreateGeometryFromWkt('POINT(10 20)')
  point2 = ogr.CreateGeometryFromWkt('POINT(30 20)')
  return point1.Union(point2) is not None


@contextlib.contextmanager
def _enable_exceptions() -> Iterator[None]:
  with (
      gdal.ExceptionMgr(useExceptions=True),
      ogr.ExceptionMgr(useExceptions=True),
      osr.ExceptionMgr(useExceptions=True),
  ):
    yield


@contextlib.contextmanager
def _attribute_filter(lyr: ogr.Layer, filter_txt: str | None) -> Iterator[None]:
  lyr.SetAttributeFilter(filter_txt)
  try:
    yield
  finally:
    lyr.SetAttributeFilter(None)


@contextlib.contextmanager
def _spatial_filter_geom(
    lyr: ogr.Layer, geom_or_wkt: ogr.Geometry | str
) -> Iterator[None]:
  if isinstance(geom_or_wkt, str):
    lyr.SetSpatialFilter(ogr.CreateGeometryFromWkt(geom_or_wkt))
  else:
    lyr.SetSpatialFilter(geom_or_wkt)
  try:
    yield
  finally:
    lyr.SetSpatialFilter(None)


@contextlib.contextmanager
def _spatial_filter_rect(
    lyr: ogr.Layer, minx: float, miny: float, maxx: float, maxy: float
) -> Iterator[None]:
  lyr.SetSpatialFilterRect(minx, miny, maxx, maxy)
  try:
    yield
  finally:
    lyr.SetSpatialFilter(None)


def _quick_create_layer_def(
    lyr: ogr.Layer,
    field_list: Sequence[
        tuple[str, int] | tuple[str, int, int] | tuple[str, int, int, int]
    ],
) -> None:
  for field in field_list:
    name = field[0]
    field_type = field[1] if len(field) > 1 else ogr.OFTString
    field_defn = ogr.FieldDefn(name, field_type)
    if len(field) > 2:
      field_defn.SetWidth(int(field[2]))
    if len(field) > 3:
      field_defn.SetPrecision(int(field[3]))
    lyr.CreateField(field_defn)


def _load_poly_features() -> list[ogr.Feature]:
  filepath = ogr_util.GetTestFilePath('shape/poly/poly.shp')
  with ogr.Open(filepath) as shp_ds:
    shp_lyr = shp_ds.GetLayer(0)
    return list(shp_lyr)


@contextlib.contextmanager
def _shape_ds_context(tmpdir: str) -> Iterator[gdal.Dataset]:
  with ogr.GetDriverByName(DRIVER).CreateDataSource(tmpdir) as ds:
    shape_lyr = ds.CreateLayer('tpoly')
    _quick_create_layer_def(
        shape_lyr,
        [
            ('AREA', ogr.OFTReal),
            ('EAS_ID', ogr.OFTInteger),
            ('PRFEDEA', ogr.OFTString),
        ],
    )
    dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
    for feat in _load_poly_features():
      dst_feat.SetFrom(feat)
      shape_lyr.CreateFeature(dst_feat)
    dst_feat = None
    shape_lyr = None
    yield ds


def _supports_sparse_files(dirpath: str) -> bool:
  probe_path = os.path.join(dirpath, 'sparse_probe.bin')
  try:
    with open(probe_path, 'wb') as f:
      f.seek(1024 * 1024)
      f.write(b'0')
    st = os.stat(probe_path)
    return st.st_blocks * 512 < 1024 * 1024
  except OSError:
    return False
  finally:
    if os.path.exists(probe_path):
      os.unlink(probe_path)


def _check_eof(filename: str, expected: bool = True) -> bool:
  f = gdal.VSIFOpenL(filename, 'rb')
  if f is None:
    return False
  try:
    size = gdal.VSIStatL(filename).size
    content = gdal.VSIFReadL(1, size, f)
  finally:
    gdal.VSIFCloseL(f)
  pos = content.find(b'\x1a')
  if expected:
    return pos >= 0 and pos == size - 1
  return pos < 0


def _is_same(filename1: str, filename2: str) -> bool:
  f1 = gdal.VSIFOpenL(filename1, 'rb')
  if f1 is None:
    return False
  f2 = gdal.VSIFOpenL(filename2, 'rb')
  if f2 is None:
    gdal.VSIFCloseL(f1)
    return False
  try:
    size1 = gdal.VSIStatL(filename1).size
    size2 = gdal.VSIStatL(filename2).size
    if size1 != size2:
      return False
    data1 = gdal.VSIFReadL(1, size1, f1)
    data2 = gdal.VSIFReadL(1, size2, f2)
    return data1 == data2
  finally:
    gdal.VSIFCloseL(f1)
    gdal.VSIFCloseL(f2)


class OgrShapefileTest(ogr_util.DriverTestCase, parameterized.TestCase):

  def setUp(self) -> None:  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)

  def _check_feature_geometry(
      self,
      actual: ogr.Feature | ogr.Geometry | None,
      expected: ogr.Geometry | str | None,
      max_error: float = 0.0001,
      *,
      pointwise: bool = False,
      context: str | None = None,
      _root_actual: ogr.Geometry | None = None,
      _root_expected: ogr.Geometry | None = None,
  ) -> None:
    if isinstance(actual, ogr.Feature):
      actual_geom: ogr.Geometry | None = actual.GetGeometryRef()
    else:
      actual_geom = actual

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
          actual_geom, f'expected NULL geometry but got one{context_msg}'
      )
      return
    self.assertIsNotNone(
        actual_geom, f'expected geometry but got NULL{context_msg}'
    )
    assert actual_geom is not None
    assert expected_geom is not None

    self.assertEqual(
        actual_geom.GetGeometryName(),
        expected_geom.GetGeometryName(),
        f'geometry types do not match{context_msg}',
    )
    self.assertEqual(
        actual_geom.GetGeometryCount(),
        expected_geom.GetGeometryCount(),
        f'sub-geometry counts do not match{context_msg}',
    )
    self.assertEqual(
        actual_geom.GetPointCount(),
        expected_geom.GetPointCount(),
        f'point counts do not match{context_msg}',
    )

    if expected_geom.Is3D():
      self.assertTrue(actual_geom.Is3D(), 'expected Z dimension not found')
    if actual_geom.Is3D():
      self.assertTrue(expected_geom.Is3D(), 'unexpected Z dimension')

    if expected_geom.IsMeasured():
      self.assertTrue(
          actual_geom.IsMeasured(), 'expected M dimension not found'
      )
    if actual_geom.IsMeasured():
      self.assertTrue(expected_geom.IsMeasured(), 'unexpected M dimension')

    if (
        not pointwise
        and _have_geos()
        and actual_geom.Within(expected_geom)
        and expected_geom.Within(actual_geom)
        and not actual_geom.Is3D()
        and not actual_geom.IsMeasured()
    ):
      return

    if _root_actual is None:
      _root_actual = actual_geom
    if _root_expected is None:
      _root_expected = expected_geom

    if actual_geom.GetGeometryCount() > 0:
      for i in range(actual_geom.GetGeometryCount()):
        self._check_feature_geometry(
            actual_geom.GetGeometryRef(i),
            expected_geom.GetGeometryRef(i),
            max_error,
            pointwise=pointwise,
            context=context,
            _root_actual=_root_actual,
            _root_expected=_root_expected,
        )
    else:
      count = actual_geom.GetPointCount()
      if ogr.GT_Flatten(actual_geom.GetGeometryType()) == ogr.wkbPoint:
        count = 1
        if expected_geom.IsEmpty():
          self.assertTrue(actual_geom.IsEmpty())
          return
        self.assertFalse(actual_geom.IsEmpty())

      for i in range(count):
        actual_pt = [actual_geom.GetX(i), actual_geom.GetY(i)]
        expected_pt = [expected_geom.GetX(i), expected_geom.GetY(i)]

        if actual_geom.Is3D() or expected_geom.Is3D():
          actual_pt.append(actual_geom.GetZ(i))
          expected_pt.append(expected_geom.GetZ(i))

        if actual_geom.IsMeasured() or expected_geom.IsMeasured():
          # Shapefile not-a-number M values equal -1.79769313486232e+308.
          if (
              actual_geom.GetM(i) >= -1.7e308
              and expected_geom.GetM(i) >= -1.7e308
          ):
            actual_pt.append(actual_geom.GetM(i))
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

  def _write_valid_and_invalid(
      self,
      dir_name: str,
      layer_name: str,
      wkt: str,
      invalid_wkt: str,
      wkb_type: int,
      is_empty: bool,
  ) -> None:
    with ogr.GetDriverByName(DRIVER).CreateDataSource(dir_name) as ds:
      if wkb_type == ogr.wkbUnknown:
        shape_lyr = ds.CreateLayer(layer_name)
      else:
        shape_lyr = ds.CreateLayer(layer_name, geom_type=wkb_type)

      dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
      dst_feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(wkt))
      shape_lyr.CreateFeature(dst_feat)

      dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
      dst_feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(invalid_wkt))
      with gdal.quiet_errors():
        shape_lyr.CreateFeature(dst_feat)

    with ogr.GetDriverByName(DRIVER).Open(dir_name, update=1) as ds:
      read_lyr = ds.GetLayerByName(layer_name)
      self.assertEqual(read_lyr.GetFeatureCount(), 1, layer_name)
      feat_read = read_lyr.GetNextFeature()

      if is_empty and feat_read.GetGeometryRef() is None:
        return

      self._check_feature_geometry(
          feat_read, ogr.CreateGeometryFromWkt(wkt), max_error=1e-9
      )

  def _write_geom(
      self,
      dir_name: str,
      layer_name: str,
      geom: ogr.Geometry,
      expected_geom: ogr.Geometry | None,
      wkb_type: int,
  ) -> None:
    with ogr.GetDriverByName(DRIVER).CreateDataSource(dir_name) as ds:
      if wkb_type == ogr.wkbUnknown:
        shape_lyr = ds.CreateLayer(layer_name)
      else:
        shape_lyr = ds.CreateLayer(layer_name, geom_type=wkb_type)

      dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
      dst_feat.SetGeometry(geom)
      shape_lyr.CreateFeature(dst_feat)

    with ogr.GetDriverByName(DRIVER).Open(dir_name, update=1) as ds:
      read_lyr = ds.GetLayerByName(layer_name)
      self.assertEqual(read_lyr.GetFeatureCount(), 1)
      feat_read = read_lyr.GetNextFeature()

      if expected_geom is None:
        self.assertIsNone(feat_read.GetGeometryRef())
        return

      self._check_feature_geometry(feat_read, expected_geom, max_error=1e-9)

  def _create_numbered_layer(self, ds: gdal.Dataset, layer_index: int) -> None:
    lyr = ds.CreateLayer(f'layer{layer_index:03d}')
    lyr.CreateField(ogr.FieldDefn('strfield', ogr.OFTString))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField(0, f'val{layer_index}')
    if (layer_index % 2) == 0:
      feat.SetGeometry(
          ogr.CreateGeometryFromWkt(f'POINT ({layer_index} {layer_index + 1})')
      )
    lyr.CreateFeature(feat)

  def _verify_numbered_layer(self, ds: gdal.Dataset, layer_index: int) -> None:
    lyr = ds.GetLayerByName(f'layer{layer_index:03d}')
    self.assertIsNotNone(lyr, f'failed for layer {layer_index}')
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.assertIsNotNone(feat, f'failed for layer {layer_index}')
    self.assertEqual(
        feat.GetField(0), f'val{layer_index}', f'failed for layer {layer_index}'
    )
    if (layer_index % 2) == 0:
      geom = feat.GetGeometryRef()
      self.assertIsNotNone(geom, f'failed for layer {layer_index}')
      self.assertEqual(
          geom.ExportToWkt(),
          f'POINT ({layer_index} {layer_index + 1})',
          f'failed for layer {layer_index}',
      )

  def testReadPoint(self) -> None:
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

  def test_ogr_shape_1(self) -> None:
    poly_features = _load_poly_features()
    self.assertLen(poly_features, 10)
    self.assertEqual(
        [feat.GetField('EAS_ID') for feat in poly_features],
        [168, 179, 171, 173, 172, 169, 166, 158, 165, 170],
    )
    for feat in poly_features:
      self.assertIsNotNone(feat.GetGeometryRef())

  def test_ogr_shape_2(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_2') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        self.assertEqual(shape_lyr.GetName(), 'tpoly')
        self.assertEqual(shape_lyr.GetFeatureCount(), 10)

  def test_ogr_shape_3(self) -> None:
    self.assertEqual(
        gdal.GetDriverByName(DRIVER).GetMetadataItem(
            gdal.DMD_MAX_STRING_LENGTH
        ),
        '254',
    )
    poly_features = _load_poly_features()
    with gcore_util.TestTemporaryDirectory(prefix='shape_3') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        expect = [168, 169, 166, 158, 165]
        with _attribute_filter(shape_lyr, 'eas_id < 170'):
          self.CheckFeaturesAgainstList(shape_lyr, 'eas_id', expect)

        for orig_feat in poly_features:
          read_feat = shape_lyr.GetNextFeature()
          self._check_feature_geometry(
              read_feat, orig_feat.GetGeometryRef(), max_error=1e-9
          )
          for fld in range(3):
            self.assertEqual(
                orig_feat.GetField(fld),
                read_feat.GetField(fld),
                f'Attribute {fld} does not match',
            )

  def test_ogr_shape_4(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_4') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
        dst_feat.SetField('PRFEDEA', 'nulled')
        shape_lyr.CreateFeature(dst_feat)

        with _attribute_filter(shape_lyr, "PRFEDEA = 'nulled'"):
          feat_read = shape_lyr.GetNextFeature()
        self.assertIsNotNone(
            feat_read, "Didn't get feature with null geometry back."
        )
        self.assertIsNone(
            feat_read.GetGeometryRef(), "Didn't get null geometry as expected."
        )

  def test_ogr_shape_5(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_5') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        for _ in range(2):
          dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
          shape_lyr.CreateFeature(dst_feat)

        expect = [179, 173, 172, 171, 170, 169, 168, 166, 165, 158, None]
        with shape_ds.ExecuteSQL(
            'select distinct eas_id from tpoly order by eas_id desc'
        ) as sql_lyr:
          self.CheckFeaturesAgainstList(sql_lyr, 'eas_id', expect)

  def test_ogr_shape_6(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_6') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        with shape_ds.ExecuteSQL(
            "select * from tpoly where prfedea = '35043413'"
        ) as sql_lyr:
          self.CheckFeaturesAgainstList(sql_lyr, 'prfedea', ['35043413'])
          sql_lyr.ResetReading()
          feat_read = sql_lyr.GetNextFeature()
          self._check_feature_geometry(
              feat_read,
              'POLYGON ((479750.688 4764702.000,479658.594 4764670.000,'
              '479640.094 4764721.000,479735.906 4764752.000,'
              '479750.688 4764702.000))',
              max_error=0.001,
          )

  def test_ogr_shape_7(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_7') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        with _spatial_filter_geom(
            shape_lyr, 'LINESTRING(479505 4763195,480526 4762819)'
        ):
          self.CheckFeaturesAgainstList(shape_lyr, 'eas_id', [158])

  def test_ogr_shape_8(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_8') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_dir = shape_ds.GetDescription()
        shape_lyr = shape_ds.GetLayer(0)
        index_fname = os.path.join(shape_dir, 'tpoly.qix')

        shape_ds.ExecuteSQL('CREATE SPATIAL INDEX ON tpoly')
        self.assertTrue(
            os.access(index_fname, os.F_OK), 'tpoly.qix not created'
        )

        with _spatial_filter_geom(
            shape_lyr, 'LINESTRING(479505 4763195,480526 4762819)'
        ):
          self.CheckFeaturesAgainstList(shape_lyr, 'eas_id', [158])

        # Test recreating while already existing.
        shape_ds.ExecuteSQL('CREATE SPATIAL INDEX ON tpoly')
        shape_ds.ExecuteSQL('DROP SPATIAL INDEX ON tpoly')
        self.assertFalse(
            os.access(index_fname, os.F_OK), 'tpoly.qix not deleted'
        )

  def test09SearchInsidePolyReturnNone(self) -> None:
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()

    layer.SetSpatialFilterRect(-10, -130, 10, -110)
    if _have_geos():
      self.assertEqual(layer.GetFeatureCount(), 0)
    else:
      self.assertEqual(layer.GetFeatureCount(), 1)

  def test10SelectSomePolygonsByRegion(self) -> None:
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()
    layer.SetSpatialFilterRect(-400, 22, -120, 400)

    index = layer.GetLayerDefn().GetFieldIndex('FID')
    fids = [feature.GetField(index) for feature in ogr_util.Features(layer)]

    self.assertEqual(fids, [0, 4, 8])

  def test11SelectAreaAndFidReturnNone(self) -> None:
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()

    layer.SetAttributeFilter('FID = 5')
    layer.SetSpatialFilterRect(-400, 22, -120, 400)
    index = layer.GetLayerDefn().GetFieldIndex('FID')
    fids = [feature.GetField(index) for feature in ogr_util.Features(layer)]

    self.assertFalse(fids)

  def test11SelectAreaAndFidReturnsOne(self) -> None:
    filepath = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer()

    layer.SetAttributeFilter('FID = 4')
    layer.SetSpatialFilterRect(-400, 22, -120, 400)
    index = layer.GetLayerDefn().GetFieldIndex('FID')
    fids = [feature.GetField(index) for feature in ogr_util.Features(layer)]

    self.assertEqual(fids, [4])

  def test12Multipolygon(self) -> None:
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
      self.assertEqual(
          poly.GetGeometryRef(0).GetPointCount(), point_counts[geom_index]
      )

  def test13SetFeature(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_setfeature') as tmpdir:
      field_settings = (
          ('real_field', ogr.OFTReal, '1.23', '7.8', 7.8),
          ('int_field', ogr.OFTInteger, '2', '3', 3),
          ('str_field', ogr.OFTString, 'original', 'new', 'new'),
      )

      for field_name, field_type, original, new, result in field_settings:
        filepath = os.path.join(tmpdir, field_name + 'tmp.shp')
        with self.driver.CreateDataSource(filepath) as dst:
          layer = dst.CreateLayer('test_layer')
          layer.CreateField(ogr.FieldDefn(field_name, field_type))
          feature = ogr.Feature(layer.GetLayerDefn())
          feature.SetField(field_name, original)
          feature.SetGeometry(ogr.CreateGeometryFromWkt('POINT(4 5)'))
          layer.CreateFeature(feature)

        with ogr.Open(filepath, update=True) as dst:
          layer = dst.GetLayer()
          feature = layer.GetFeature(0)
          feature.SetField(field_name, new)
          new_geom_str = 'POINT (9 0)'
          feature.SetGeometry(ogr.CreateGeometryFromWkt(new_geom_str))
          self.assertEqual(layer.SetFeature(feature), 0)

        self.CheckOpen(filepath)
        layer = self.src.GetLayer()
        feature = layer.GetFeature(0)
        self.assertEqual(feature.GetField(0), result)
        self.assertEqual(feature.GetGeometryRef().ExportToWkt(), new_geom_str)

  def test_ogr_shape_13_resize_polygon(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_13') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shp_path = os.path.join(shape_ds.GetDescription(), 'tpoly.shp')

      with ogr.Open(shp_path, update=1) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)

        # Update FID 9 (EAS_ID=170), making the polygon larger.
        feat = shape_lyr.GetFeature(9)
        feat.SetField('AREA', '6000.00')
        geom = ogr.CreateGeometryFromWkt(
            'POLYGON ((0 0, 0 60, 100 60, 100 0, 200 -30, 0 0))'
        )
        feat.SetGeometry(geom)
        self.assertEqual(shape_lyr.SetFeature(feat), 0, 'SetFeature() failed.')

        # Update FID 8 (EAS_ID=165), making the polygon smaller.
        feat = shape_lyr.GetFeature(8)
        feat.SetField('AREA', '7000.00')
        geom = ogr.CreateGeometryFromWkt(
            'POLYGON ((0 0, 0 60, 100 60, 100 0, 0 0))'
        )
        feat.SetGeometry(geom)
        self.assertEqual(shape_lyr.SetFeature(feat), 0, 'SetFeature() failed.')

      with ogr.Open(shp_path, update=1) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)

        feat = shape_lyr.GetFeature(9)
        self.assertEqual(
            feat.GetField('AREA'), 6000.0, 'AREA update failed, FID 9.'
        )
        self._check_feature_geometry(
            feat, 'POLYGON ((0 0, 0 60, 100 60, 100 0, 200 -30, 0 0))'
        )

        feat = shape_lyr.GetFeature(8)
        self.assertEqual(
            feat.GetField('AREA'), 7000.0, 'AREA update failed, FID 8.'
        )
        self._check_feature_geometry(
            feat, 'POLYGON ((0 0, 0 60, 100 60, 100 0, 0 0))'
        )

  def test_ogr_shape_15(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_15') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        original_count = shape_lyr.GetFeatureCount()

        self.assertEqual(shape_lyr.DeleteFeature(9), 0, 'DeleteFeature failed.')

        count = 0
        for feat in shape_lyr:
          self.assertNotEqual(feat.GetFID(), 9, 'Still an FID 9 in dataset.')
          count += 1

        self.assertEqual(
            count, original_count - 1, 'Did not get expected FID count.'
        )

  def test_ogr_shape_16(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_16') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_ds.ExecuteSQL('REPACK tpoly')
        shape_lyr = shape_ds.GetLayer(0)

        got_9 = False
        count = 0
        shape_lyr.ResetReading()
        for feat in shape_lyr:
          if feat.GetFID() == 9:
            got_9 = True
          count += 1

        self.assertEqual(count, 10, 'Did not get expected FID count.')
        self.assertTrue(got_9, 'Did not get FID 9 as expected.')
        self.assertIsNotNone(shape_lyr.GetFeature(9))

  def test_ogr_shape_16_1(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_16_1') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        field_defn = ogr.FieldDefn('NEWFLD', ogr.OFTString)
        field_defn.SetWidth(12)
        result = shape_lyr.CreateField(field_defn)
        self.assertEqual(result, 0, 'failed to create new field.')

        feat = shape_lyr.GetFeature(8)
        self.assertEqual(feat.GetField('EAS_ID'), 165, 'Got wrong EAS_ID')
        self.assertTrue(
            feat.IsFieldNull('NEWFLD'), 'Expected NULL NEWFLD value!'
        )

  def test_ogr_shape_17(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_17') as tmpdir:
      for ext in ('.shp', '.shx'):
        shutil.copyfile(
            ogr_util.GetTestFilePath(f'shape/can_caps{ext}'),
            os.path.join(tmpdir, f'can_caps{ext}'),
        )

      with ogr.Open(os.path.join(tmpdir, 'can_caps.shp'), update=1) as shp_ds:
        shp_lyr = shp_ds.GetLayer(0)
        self.assertEqual(
            shp_lyr.GetLayerDefn().GetFieldCount(),
            0,
            'Unexpectedly got attribute fields.',
        )

        count = 0
        while True:
          feat = shp_lyr.GetNextFeature()
          if feat is None:
            break
          # Re-write feature to test SetFeature() without a DBF.
          shp_lyr.SetFeature(feat)
          count += 1

        self.assertEqual(count, 13, 'Got wrong number of features.')

        # Create new feature without a DBF.
        feat = ogr.Feature(shp_lyr.GetLayerDefn())
        shp_lyr.CreateFeature(feat)
        self.assertEqual(feat.GetFID(), 13, 'Got wrong FID.')

  def test_ogr_shape_18(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/poly/poly.shp')) as shp_ds:
      shp_lyr = shp_ds.GetLayer(0)
      srs_lyr = shp_lyr.GetSpatialRef()
      self.assertIsNotNone(srs_lyr, 'Missing projection definition.')
      self.assertEqual(srs_lyr.GetAuthorityCode(None), '27700')

  def test_ogr_shape_19(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/Stacks.shp')) as ds:
      lyr = ds.GetLayer(0)
      lyr.ResetReading()
      feat = lyr.GetNextFeature()
      wkt = (
          'MULTIPOLYGON (((3115478.809630727861077 13939288.008583962917328,'
          '3134266.47213465673849 13971973.394036004319787,'
          '3176989.101938112173229 13957303.575368551537395,'
          '3198607.7820796193555 13921787.172278933227062,'
          '3169010.779504936654121 13891675.439224690198898,'
          '3120368.749186545144767 13897852.204979406669736,'
          '3115478.809630727861077 13939288.008583962917328),'
          '(3130405.993537959177047 13935427.529987264424562,'
          '3135038.567853996530175 13902742.144535223022103,'
          '3167209.22282647760585 13902227.414055664092302,'
          '3184452.693891727831215 13922559.267998272553086,'
          '3172871.258101634215564 13947781.061496697366238,'
          '3144561.081725850701332 13957818.305848112329841,'
          '3130405.993537959177047 13935427.529987264424562)),'
          '((3143016.890287171583623 13932596.512349685654044,'
          '3152282.038919246289879 13947266.331017138436437,'
          '3166179.761867358349264 13940060.104303302243352,'
          '3172099.162382294889539 13928221.303273428231478,'
          '3169268.144744716584682 13916897.23272311501205,'
          '3158201.439434182830155 13911235.197447959333658,'
          '3144818.446965630631894 13911749.927927518263459,'
          '3139928.507409813348204 13916382.502243556082249,'
          '3143016.890287171583623 13932596.512349685654044),'
          '(3149193.65604188805446 13926677.11183474957943,'
          '3150737.84748056717217 13918698.789401574060321,'
          '3158458.804673962760717 13919728.250360693782568,'
          '3164892.935668459162116 13923331.36371761187911,'
          '3163863.474709339439869 13928736.033752989023924,'
          '3157171.978475063573569 13935427.529987264424562,'
          '3149193.65604188805446 13926677.11183474957943)))'
      )
      self._check_feature_geometry(feat, wkt, max_error=1e-8)

  @parameterized.named_parameters(
      ('emptymultipoint', 'shape/emptymultipoint.shp'),
      ('emptymultiline', 'shape/emptymultiline.shp'),
      ('emptymultipoly', 'shape/emptymultipoly.shp'),
  )
  def test_ogr_shape_20(self, relpath: str) -> None:
    with ogr.Open(ogr_util.GetTestFilePath(relpath)) as ds:
      lyr = ds.GetLayer(0)
      lyr.ResetReading()
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertIsNone(feat.GetGeometryRef())

  @parameterized.named_parameters(
      ('buggypoint', 'shape/buggypoint.shp'),
      ('buggymultipoint', 'shape/buggymultipoint.shp'),
      ('buggymultiline', 'shape/buggymultiline.shp'),
      ('buggymultipoly', 'shape/buggymultipoly.shp'),
      ('buggymultipoly2', 'shape/buggymultipoly2.shp'),
  )
  def test_ogr_shape_21(self, relpath: str) -> None:
    with ogr.Open(ogr_util.GetTestFilePath(relpath)) as ds:
      lyr = ds.GetLayer(0)
      lyr.ResetReading()
      with gdal.quiet_errors():
        feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertIsNone(feat.GetGeometryRef())

      lyr.ResetReading()
      minx, maxx, miny, maxy = lyr.GetExtent()
      with (
          _spatial_filter_rect(
              lyr, minx + 1e-9, miny + 1e-9, maxx - 1e-9, maxy - 1e-9
          ),
          gdal.quiet_errors(),
      ):
        feat = lyr.GetNextFeature()
      self.assertTrue(feat is None or feat.GetGeometryRef() is None)

  def test_ogr_shape_22(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_22') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_path = shape_ds.GetDescription()

      with ogr.GetDriverByName(DRIVER).Open(shape_path, update=1) as shape_ds:
        shape_lyr = shape_ds.CreateLayer('datatypes')
        _quick_create_layer_def(
            shape_lyr,
            [
                ('REAL', ogr.OFTReal),
                ('INTEGER', ogr.OFTInteger),
                ('STRING', ogr.OFTString),
                ('DATE', ogr.OFTDate),
            ],
        )
        dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
        dst_feat.SetField('REAL', 1.2)
        dst_feat.SetField('INTEGER', 3)
        dst_feat.SetField('STRING', 'aString')
        dst_feat.SetField('DATE', '2005/10/12')
        shape_lyr.CreateFeature(dst_feat)

      with ogr.GetDriverByName(DRIVER).Open(shape_path, update=1) as shape_ds:
        shape_lyr = shape_ds.GetLayerByName('datatypes')
        feat_read = shape_lyr.GetNextFeature()
        self.assertEqual(feat_read.GetField('REAL'), 1.2)
        self.assertEqual(feat_read.GetField('INTEGER'), 3)
        self.assertEqual(feat_read.GetField('STRING'), 'aString')
        self.assertEqual(feat_read.GetFieldAsString('DATE'), '2005/10/12')

  @parameterized.named_parameters(
      ('with_wkb_type', True),
      ('without_wkb_type', False),
  )
  def test_ogr_shape_23(self, use_wkb_type: bool) -> None:
    test_geom_array = [
        ('points', 'POINT(0 1)', 'LINESTRING(0 1)', ogr.wkbPoint),
        ('points25D', 'POINT(0 1 2)', 'LINESTRING(0 1)', ogr.wkbPoint25D),
        (
            'multipoints',
            'MULTIPOINT(0 1,2 3)',
            'POINT (0 1)',
            ogr.wkbMultiPoint,
        ),
        (
            'multipoints25D',
            'MULTIPOINT(0 1 2,3 4 5)',
            'POINT (0 1)',
            ogr.wkbMultiPoint25D,
        ),
        (
            'linestrings',
            'LINESTRING(0 1,2 3,4 5,0 1)',
            'POINT (0 1)',
            ogr.wkbLineString,
        ),
        (
            'linestrings25D',
            'LINESTRING(0 1 2,3 4 5,6 7 8,0 1 2)',
            'POINT (0 1)',
            ogr.wkbLineString25D,
        ),
        (
            'multilinestrings',
            'MULTILINESTRING((0 1,2 3,4 5,0 1), (0 1,2 3,4 5,0 1))',
            'POINT (0 1)',
            ogr.wkbMultiLineString,
        ),
        (
            'multilinestrings25D',
            (
                'MULTILINESTRING((0 1 2,3 4 5,6 7 8,0 1 2),'
                '(0 1 2,3 4 5,6 7 8,0 1 2))'
            ),
            'POINT (0 1)',
            ogr.wkbMultiLineString25D,
        ),
        (
            'polygons',
            'POLYGON((0 0,0 10,10 10,0 0),(0.25 0.5,1 1,0.5 1,0.25 0.5))',
            'POINT (0 1)',
            ogr.wkbPolygon,
        ),
        (
            'polygons25D',
            'POLYGON((0 0 2,0 10 5,10 10 8,0 1 2))',
            'POINT (0 1)',
            ogr.wkbPolygon25D,
        ),
        (
            'multipolygons',
            (
                'MULTIPOLYGON(((0 0,0 10,10 10,0 0),(0.25 0.5,1 1,0.5 1,0.25'
                ' 0.5)),((100 0,100 10,110 10,100 0),(100.25 0.5,100.5 1,100'
                ' 1,100.25 0.5)))'
            ),
            'POINT (0 1)',
            ogr.wkbMultiPolygon,
        ),
        (
            'multipolygons25D',
            (
                'MULTIPOLYGON(((0 0 0,0 10,10 10,0 0),'
                '(0.25 0.5,1 1,0.5 1,0.25 0.5)),'
                '((100 0,100 10,110 10,100 0),'
                '(100.25 0.5,100.5 1,100 1,100.25 0.5)))'
            ),
            'POINT (0 1)',
            ogr.wkbMultiPolygon25D,
        ),
    ]

    test_empty_geom_array = [
        ('emptypoints', 'POINT EMPTY', 'LINESTRING(0 1)', ogr.wkbPoint),
        (
            'emptymultipoints',
            'MULTIPOINT EMPTY',
            'POINT(0 1)',
            ogr.wkbMultiPoint,
        ),
        (
            'emptylinestrings',
            'LINESTRING EMPTY',
            'POINT(0 1)',
            ogr.wkbLineString,
        ),
        (
            'emptymultilinestrings',
            'MULTILINESTRING EMPTY',
            'POINT(0 1)',
            ogr.wkbMultiLineString,
        ),
        ('emptypolygons', 'POLYGON EMPTY', 'POINT(0 1)', ogr.wkbPolygon),
        (
            'emptymultipolygons',
            'MULTIPOLYGON EMPTY',
            'POINT(0 1)',
            ogr.wkbMultiPolygon,
        ),
    ]

    with gcore_util.TestTemporaryDirectory(prefix='shape_23') as tmpdir:
      for layer_name, wkt, invalid_wkt, wkb_type in test_geom_array:
        self._write_valid_and_invalid(
            tmpdir,
            layer_name,
            wkt,
            invalid_wkt,
            wkb_type if use_wkb_type else ogr.wkbUnknown,
            is_empty=False,
        )
      for layer_name, wkt, invalid_wkt, wkb_type in test_empty_geom_array:
        self._write_valid_and_invalid(
            tmpdir,
            layer_name,
            wkt,
            invalid_wkt,
            wkb_type if use_wkb_type else ogr.wkbUnknown,
            is_empty=True,
        )

  def test_ogr_shape_23a(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_23a') as tmpdir:
      layer_name = 'geometrycollections'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(tmpdir) as ds:
        shape_lyr = ds.CreateLayer(layer_name, geom_type=ogr.wkbMultiPolygon)

        # Incompatible with a multipolygon layer.
        geom = ogr.CreateGeometryFromWkt('GEOMETRYCOLLECTION(POINT (0 0))')
        dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
        dst_feat.SetGeometry(geom)
        with gdal.quiet_errors():
          shape_lyr.CreateFeature(dst_feat)

        # Compatible geometrycollection written as multipolygon.
        wkt = (
            'GEOMETRYCOLLECTION('
            'POLYGON((0 0,0 10,10 10,0 0),(0.25 0.5,1 1,0.5 1,0.25 0.5)),'
            'POLYGON((100 0,100 10,110 10,100 0),'
            '(100.25 0.5,100.5 1,100 1,100.25 0.5)))'
        )
        geom = ogr.CreateGeometryFromWkt(wkt)
        dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
        dst_feat.SetGeometry(geom)
        shape_lyr.CreateFeature(dst_feat)

      with ogr.GetDriverByName(DRIVER).Open(tmpdir, update=1) as ds:
        read_lyr = ds.GetLayerByName(layer_name)
        feat_read = read_lyr.GetNextFeature()
        self._check_feature_geometry(
            feat_read,
            ogr.CreateGeometryFromWkt(
                'MULTIPOLYGON (((0 0,0 10,10 10,0 0),'
                '(0.25 0.5,1 1,0.5 1.0,0.25 0.5)),'
                '((100 0,100 10,110 10,100 0),'
                '(100.25 0.5,100.5 1.0,100 1,100.25 0.5)))'
            ),
            max_error=1e-9,
        )

  def test_ogr_shape_23b(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_23b') as tmpdir:
      geom = ogr.CreateGeometryFromWkt('MULTIPOINT(0 1)')
      geom.AddGeometry(ogr.Geometry(type=ogr.wkbPoint))
      self._write_geom(
          tmpdir,
          'strangemultipoints',
          geom,
          ogr.CreateGeometryFromWkt(geom.ExportToWkt()),
          ogr.wkbUnknown,
      )

  def test_ogr_shape_23c(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_23c') as tmpdir:
      geom = ogr.CreateGeometryFromWkt(
          'MULTILINESTRING((0 1,2 3,4 5,0 1), (0 1,2 3,4 5,0 1))'
      )
      geom.AddGeometry(ogr.Geometry(type=ogr.wkbLineString))
      self._write_geom(
          tmpdir,
          'strangemultilinestrings',
          geom,
          ogr.CreateGeometryFromWkt(geom.ExportToWkt()),
          ogr.wkbUnknown,
      )

  def test_ogr_shape_23d(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_23d') as tmpdir:
      geom = ogr.CreateGeometryFromWkt('POLYGON EMPTY')
      geom.AddGeometry(ogr.Geometry(type=ogr.wkbLinearRing))
      ring = ogr.Geometry(type=ogr.wkbLinearRing)
      ring.AddPoint_2D(0, 0)
      ring.AddPoint_2D(10, 0)
      ring.AddPoint_2D(10, 10)
      ring.AddPoint_2D(0, 10)
      ring.AddPoint_2D(0, 0)
      geom.AddGeometry(ring)
      self._write_geom(
          tmpdir, 'polygonwithemptyexternalring', geom, None, ogr.wkbUnknown
      )

  def test_ogr_shape_23e(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_23e') as tmpdir:
      geom = ogr.CreateGeometryFromWkt('POLYGON((100 0,100 10,110 10,100 0))')
      geom.AddGeometry(ogr.Geometry(type=ogr.wkbLinearRing))
      self._write_geom(
          tmpdir,
          'polygonwithemptyinternalring',
          geom,
          ogr.CreateGeometryFromWkt(geom.ExportToWkt()),
          ogr.wkbUnknown,
      )

  def test_ogr_shape_23f(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_23f') as tmpdir:
      geom = ogr.CreateGeometryFromWkt(
          'MULTIPOLYGON(((0 0,0 10,10 10,0 0)), ((100 0,100 10,110 10,100 0)))'
      )
      geom.AddGeometry(ogr.Geometry(type=ogr.wkbPolygon))
      poly = ogr.CreateGeometryFromWkt(
          'POLYGON((-100 0,-110 10,-100 10,-100 0))'
      )
      poly.AddGeometry(ogr.Geometry(type=ogr.wkbLinearRing))
      geom.AddGeometry(poly)
      self._write_geom(
          tmpdir,
          'strangemultipolygons',
          geom,
          ogr.CreateGeometryFromWkt(geom.ExportToWkt()),
          ogr.wkbUnknown,
      )

  def test_ogr_shape_23g(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_23g') as tmpdir:
      wkt = 'MULTIPOLYGON (((1 1,1 2,2 2,2 1,1 1)),((2 1,2 2,3 2,3 1,2 1)))'
      geom = ogr.CreateGeometryFromWkt(wkt)
      self._write_geom(
          tmpdir,
          'multipolygon_two_parts_touching_one_edge',
          geom,
          geom,
          ogr.wkbUnknown,
      )

  def test_ogr_shape_24(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_24') as tmpdir:
      wkt = (
          'MULTIPOLYGON(((0 0,0 10,10 10,0 0), (0 0,1 1,0 1,0 0)), '
          '((100 100,100 200,200 200,200 100,100 100)))'
      )
      geom = ogr.CreateGeometryFromWkt(wkt)
      self._write_geom(
          tmpdir,
          'touchingrings',
          geom,
          ogr.CreateGeometryFromWkt(geom.ExportToWkt()),
          ogr.wkbUnknown,
      )

  @parameterized.named_parameters(
      ('default_env', None),
      ('organize_default', 'DEFAULT'),
  )
  def test_ogr_shape_25(self, organize_polygons: str | None) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_25') as tmpdir:
      wkt = (
          'MULTIPOLYGON(((10 5, 5 5,5 0,0 0,0 10,10 10,10 5)),'
          '((10 5,10 0,5 0,5 4.9,10 5)), '
          '((100 100,100 200,200 200,200 100,100 100)))'
      )
      geom = ogr.CreateGeometryFromWkt(wkt)
      with gdal.config_option('OGR_ORGANIZE_POLYGONS', organize_polygons):
        self._write_geom(
            tmpdir,
            'touchingrings2',
            geom,
            ogr.CreateGeometryFromWkt(geom.ExportToWkt()),
            ogr.wkbUnknown,
        )

  def test_ogr_shape_26(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_26') as tmpdir:
      wkt = (
          'POLYGON ((100 100,100 200,200 200,200 100,100 100),'
          '(110 110,120 110,120 120,110 120,110 110),'
          '(130 110,140 110,140 120,130 120,130 110))'
      )
      geom = ogr.CreateGeometryFromWkt(wkt)
      self._write_geom(
          tmpdir,
          'oneouterring',
          geom,
          ogr.CreateGeometryFromWkt(geom.ExportToWkt()),
          ogr.wkbUnknown,
      )

  def test_ogr_shape_read_multipolygon_as_invalid_polygon(self) -> None:
    filepath = ogr_util.GetTestFilePath(
        'shape/multipolygon_as_invalid_polygon.shp'
    )
    with ogr.Open(filepath) as ds:
      lyr = ds.GetLayer(0)
      gdal.ErrorReset()
      with gdal.quiet_errors():
        f = lyr.GetNextFeature()
        self.assertIn(
            'contains polygon(s) with rings with invalid winding order',
            gdal.GetLastErrorMsg(),
        )
      self.assertEqual(
          f.GetGeometryRef().ExportToWkt(),
          'MULTIPOLYGON (((0 0,0 1,1 1,0 0)),((10 0,11 1,10 1,10 0)))',
      )
      gdal.ErrorReset()
      f = lyr.GetNextFeature()
      self.assertEqual(gdal.GetLastErrorMsg(), '')
      self.assertEqual(
          f.GetGeometryRef().ExportToWkt(),
          'MULTIPOLYGON (((0 0,0 1,1 1,0 0)),'
          '((0.5 -0.5,1.5 0.5,0.5 0.5,0.5 -0.5)))',
      )

  def test_ogr_shape_read_multipolygon_as_invalid_polygon_no_warning(
      self,
  ) -> None:
    filepath = ogr_util.GetTestFilePath(
        'shape/cb_2022_us_county_20m_extract.shp'
    )
    with ogr.Open(filepath) as ds:
      lyr = ds.GetLayer(0)
      gdal.ErrorReset()
      with gdal.quiet_errors():
        lyr.GetNextFeature()
        self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_shape_27(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/water_main_dist.dbf')) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertEqual(
          feat.GetField('installe_1'), '1989/04/25', 'got wrong date result!'
      )

  def test_ogr_shape_28(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_28') as tmpdir:
      if not _supports_sparse_files(tmpdir):
        self.skipTest('Filesystem does not support sparse files')

      shp_path = os.path.join(tmpdir, 'hugedbf.shp')
      dbf_path = os.path.join(tmpdir, 'hugedbf.dbf')
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('test')
        field_defn = ogr.FieldDefn()
        field_defn.SetName('test')
        field_defn.SetWidth(99)
        lyr.CreateField(field_defn)

      os.remove(shp_path)
      os.remove(os.path.join(tmpdir, 'hugedbf.shx'))

      with open(dbf_path, 'rb+') as f:
        # Set record count to 24,000,000 (0x016e3600 little-endian).
        f.seek(4, 0)
        f.write(b'\x00\x36\x6e\x01')
        # Set value for record 23,900,000 at
        # offset 2,390,000,066 = (23,900,000 * (99 + 1) + 65) + 1.
        f.seek(2390000066, 0)
        f.write(b'value_over_2GB')
        # Extend to 3 GB file.
        f.seek(3000000000, 0)
        f.write(b'0')

      with ogr.Open(dbf_path, update=1) as ds:
        self.assertIsNotNone(ds, f'Cannot open {dbf_path}')
        lyr = ds.GetLayer(0)
        feat = lyr.GetFeature(23900000)
        self.assertEqual(feat.GetFieldAsString(0), 'value_over_2GB')

        feat.SetField(0, 'updated_value')
        lyr.SetFeature(feat)
        feat = None

        # Creating a feature over 2 GB limit without SHAPE_2GB_LIMIT succeeds
        # with a warning.
        gdal.ErrorReset()
        feat = ogr.Feature(lyr.GetLayerDefn())
        with gdal.quiet_errors():
          ret = lyr.CreateFeature(feat)
        self.assertEqual(ret, 0)
        feat = None
        self.assertIn(
            '2GB file size limit reached',
            gdal.GetLastErrorMsg(),
            'did not find expected warning',
        )

      with (
          gdal.config_option('SHAPE_2GB_LIMIT', 'TRUE'),
          ogr.Open(dbf_path, 1) as ds,
      ):
        lyr = ds.GetLayer(0)
        feat = lyr.GetFeature(23900000)
        self.assertEqual(feat.GetFieldAsString(0), 'updated_value')
        feat = None

        # Creating a feature over 2 GB limit with SHAPE_2GB_LIMIT=TRUE fails.
        gdal.ErrorReset()
        feat = ogr.Feature(lyr.GetLayerDefn())
        with gdal.quiet_errors():
          ret = lyr.CreateFeature(feat)
        self.assertNotEqual(ret, 0)
        feat = None
        self.assertIn(
            '2GB file size limit reached',
            gdal.GetLastErrorMsg(),
            'did not find expected warning',
        )

  def test_ogr_shape_29(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_29') as tmpdir:
      upper_dir = os.path.join(tmpdir, 'UPPERCASE')
      os.mkdir(upper_dir)
      for ext in ('shp', 'shx', 'dbf'):
        shutil.copyfile(
            ogr_util.GetTestFilePath(f'shape/poly/poly.{ext}'),
            os.path.join(upper_dir, f'UPPERCASE.{ext.upper()}'),
        )
      with open(os.path.join(upper_dir, 'UPPERCASE.CPG'), 'wb') as f:
        f.write(b'UTF-8')

      with ogr.Open(upper_dir, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetMetadata_Dict('SHAPEFILE'),
            {
                'CPG_VALUE': 'UTF-8',
                'ENCODING_FROM_CPG': 'UTF-8',
                'SOURCE_ENCODING': 'UTF-8',
            },
        )
        lyr.DeleteFeature(0)
        ds.ExecuteSQL('REPACK UPPERCASE')

      lst = gdal.ReadDir(upper_dir)
      self.assertLen(lst, 6)
      for filename in lst:
        self.assertIn(
            filename,
            [
                '.',
                '..',
                'UPPERCASE.SHP',
                'UPPERCASE.SHX',
                'UPPERCASE.DBF',
                'UPPERCASE.CPG',
            ],
        )
        self.assertNotIn('packed', filename)

  def test_ogr_shape_30(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_30') as tmpdir:
      lower_dir = os.path.join(tmpdir, 'lowercase')
      os.mkdir(lower_dir)
      for ext in ('shp', 'shx', 'dbf'):
        shutil.copyfile(
            ogr_util.GetTestFilePath(f'shape/poly/poly.{ext}'),
            os.path.join(lower_dir, f'lowercase.{ext}'),
        )

      with ogr.Open(lower_dir, update=1) as ds:
        lyr = ds.GetLayer(0)
        lyr.DeleteFeature(0)
        ds.ExecuteSQL('REPACK lowercase')

      lst = gdal.ReadDir(lower_dir)
      self.assertLen(lst, 5)
      for filename in lst:
        self.assertIn(
            filename,
            ['.', '..', 'lowercase.shp', 'lowercase.shx', 'lowercase.dbf'],
        )

  def test_ogr_shape_31(self) -> None:
    fields = [
        ('a', ogr.OFTReal),
        ('A', ogr.OFTInteger),
        ('A_1', ogr.OFTInteger),
        ('A_1', ogr.OFTInteger),
        ('a_1_2', ogr.OFTInteger),
        ('aaaaaAAAAAb', ogr.OFTInteger),
        ('aAaaaAAAAAc', ogr.OFTInteger),
        ('aaaaaAAAABa', ogr.OFTInteger),
        ('aaaaaAAAABb', ogr.OFTInteger),
        ('aaaaaAAA_1', ogr.OFTInteger),
        ('aaaaaAAAABc', ogr.OFTInteger),
        ('aaaaaAAAABd', ogr.OFTInteger),
        ('aaaaaAAAABe', ogr.OFTInteger),
        ('aaaaaAAAABf', ogr.OFTInteger),
        ('aaaaaAAAABg', ogr.OFTInteger),
        ('aaaaaAAAABh', ogr.OFTInteger),
        ('aaaaaAAAABi', ogr.OFTInteger),
        ('aaaaaAAA10', ogr.OFTString),
        ('', ogr.OFTInteger),
        ('', ogr.OFTInteger),
    ]

    expected_fields = [
        'a',
        'A_1',
        'A_1_1',
        'A_1_2',
        'a_1_2_1',
        'aaaaaAAAAA',
        'aAaaaAAA_1',
        'aaaaaAAAAB',
        'aaaaaAAA_2',
        'aaaaaAAA_3',
        'aaaaaAAA_4',
        'aaaaaAAA_5',
        'aaaaaAAA_6',
        'aaaaaAAA_7',
        'aaaaaAAA_8',
        'aaaaaAAA_9',
        'aaaaaAAA10',
        'aaaaaAAA11',
        '',
        '_1',
    ]

    with gcore_util.TestTemporaryDirectory(prefix='shape_31') as tmpdir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(tmpdir) as shape_ds:
        shape_lyr = shape_ds.CreateLayer('Fields')
        with gdal.quiet_errors():
          _quick_create_layer_def(shape_lyr, fields)

        layer_defn = shape_lyr.GetLayerDefn()
        for i in range(layer_defn.GetFieldCount()):
          self.assertEqual(
              layer_defn.GetFieldDefn(i).GetNameRef(), expected_fields[i]
          )

  @unittest.skip('Requires 4.5 GB disk space and several minutes')
  def test_ogr_shape_32(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_32') as tmpdir:
      shape_drv = ogr.GetDriverByName(DRIVER)
      with shape_drv.CreateDataSource(tmpdir) as shape_ds_big:
        shape_lyr = shape_ds_big.CreateLayer(
            'bigLayer', geom_type=ogr.wkbPolygon
        )
        wkt = 'POLYGON((0 0,0 10,10 10,0 0),(0.25 0.5,1 1.1,0.5 1,0.25 0.5))'
        geom = ogr.CreateGeometryFromWkt(wkt)
        geom.AddGeometry(ogr.Geometry(type=ogr.wkbPolygon))

        for n in range(22845571):
          dst_feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
          dst_feat.SetGeometry(geom)
          ret = shape_lyr.CreateFeature(dst_feat)
          self.assertTrue(
              ret == 0 or n >= 22845570, 'File limit reached before 4GB!'
          )
          if (n % 22846) == 0:
            _ = n / decimal.Decimal('228460.0')

      with shape_drv.Open(tmpdir, update=0) as shape_ds_big:
        read_lyr = shape_ds_big.GetLayerByName('bigLayer')
        for i in (0, 1, read_lyr.GetFeatureCount() - 1):
          feat_read = read_lyr.GetFeature(i)
          self.assertIsNotNone(
              feat_read, f'Could not retrieve geometry at FID {i}'
          )
          self._check_feature_geometry(
              feat_read,
              'POLYGON((0 0,0 10,10 10,0 0),(0.25 0.5,1 1.1,0.5 1,0.25 0.5))',
              max_error=1e-9,
              context=f'FID {i}',
          )

  def test_ogr_shape_33(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/bigoffset.shp')) as ds:
      lyr = ds.GetLayer(0)
      feat_read = lyr.GetNextFeature()
      self._check_feature_geometry(
          feat_read,
          'MULTIPOLYGON( ((0 0,0 1,1 1,1 0,0 0)),'
          '((100000000000 100000000000,100000000000 100000000001,'
          '100000000001 100000000001,100000000001 100000000000,'
          '100000000000 100000000000)) )',
          max_error=1e-9,
      )

  def test_ogr_shape_34(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_34') as tmpdir:
      shp_path = os.path.join(tmpdir, 'bigoffset.shp')
      geom_wkt = (
          'MULTIPOLYGON( ((0 0,0 1,1 1,1 0,0 0)),'
          '((100000000000 100000000000,100000000000 100000000001,'
          '100000000001 100000000001,100000000001 100000000000,'
          '100000000000 100000000000)) )'
      )
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('bigoffset')
        feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt(geom_wkt))
        lyr.CreateFeature(feat)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        feat_read = lyr.GetNextFeature()
        self._check_feature_geometry(feat_read, geom_wkt, max_error=1e-9)

  def test_ogr_shape_35(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_35') as vsimem_dir:
      shp_path = f'{vsimem_dir}/test35.shp'
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('test35', srs=srs)
        feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(0 1)'))
        lyr.CreateFeature(feat)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        srs_read = lyr.GetSpatialRef()
        self.assertEqual(
            srs_read.ExportToWkt(),
            srs.ExportToWkt(),
            'did not get expected SRS',
        )
        feat_read = lyr.GetNextFeature()
        self._check_feature_geometry(
            feat_read, ogr.CreateGeometryFromWkt('POINT(0 1)'), max_error=1e-9
        )

  def test_ogr_shape_36(self) -> None:
    zip_path = '/vsizip/' + ogr_util.GetTestFilePath('shape/poly.zip')
    with ogr.Open(zip_path) as ds:
      self.assertIsNotNone(ds)
      lyr = ds.GetLayer(0)
      srs = lyr.GetSpatialRef()
      wkt = srs.ExportToWkt()
      self.assertIn('OSGB', wkt, 'did not get expected SRS')

      feat_read = lyr.GetFeature(9)
      self._check_feature_geometry(
          feat_read,
          ogr.CreateGeometryFromWkt(
              'POLYGON ((479750.6875 4764702.0,479658.59375 4764670.0,'
              '479640.09375 4764721.0,479735.90625 4764752.0,'
              '479750.6875 4764702.0))'
          ),
          max_error=1e-9,
      )

  def test_ogr_shape_37(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_37') as vsimem_dir:
      tar_gz_path = f'{vsimem_dir}/poly.tar.gz'
      with open(ogr_util.GetTestFilePath('shape/poly.tar.gz'), 'rb') as f:
        gdal.FileFromMemBuffer(tar_gz_path, f.read())

      with ogr.Open(f'/vsitar/{tar_gz_path}') as ds:
        self.assertIsNotNone(ds)
        lyr = ds.GetLayer(0)
        srs = lyr.GetSpatialRef()
        wkt = srs.ExportToWkt()
        self.assertIn('OSGB', wkt, 'did not get expected SRS')

        for i in range(10):
          feat_read = lyr.GetNextFeature()
          if i == 9:
            self._check_feature_geometry(
                feat_read,
                ogr.CreateGeometryFromWkt(
                    'POLYGON ((479750.6875 4764702.0,479658.59375 4764670.0,'
                    '479640.09375 4764721.0,479735.90625 4764752.0,'
                    '479750.6875 4764702.0))'
                ),
                max_error=1e-9,
            )

        lyr.ResetReading()
        feat_read = lyr.GetFeature(9)
        self._check_feature_geometry(
            feat_read,
            ogr.CreateGeometryFromWkt(
                'POLYGON ((479750.6875 4764702.0,479658.59375 4764670.0,'
                '479640.09375 4764721.0,479735.90625 4764752.0,'
                '479750.6875 4764702.0))'
            ),
            max_error=1e-9,
        )

  def test_ogr_shape_37_bis(self) -> None:
    tar_path = '/vsitar/' + ogr_util.GetTestFilePath('shape/poly.tar')
    with ogr.Open(tar_path) as ds:
      self.assertIsNotNone(ds)
      lyr = ds.GetLayer(0)
      srs = lyr.GetSpatialRef()
      wkt = srs.ExportToWkt()
      self.assertIn('OSGB', wkt, 'did not get expected SRS')

      for i in range(10):
        feat_read = lyr.GetNextFeature()
        if i == 9:
          self._check_feature_geometry(
              feat_read,
              ogr.CreateGeometryFromWkt(
                  'POLYGON ((479750.6875 4764702.0,479658.59375 4764670.0,'
                  '479640.09375 4764721.0,479735.90625 4764752.0,'
                  '479750.6875 4764702.0))'
              ),
              max_error=1e-9,
          )

      lyr.ResetReading()
      feat_read = lyr.GetFeature(9)
      self._check_feature_geometry(
          feat_read,
          ogr.CreateGeometryFromWkt(
              'POLYGON ((479750.6875 4764702.0,479658.59375 4764670.0,'
              '479640.09375 4764721.0,479735.90625 4764752.0,'
              '479750.6875 4764702.0))'
          ),
          max_error=1e-9,
      )

  def test_ogr_shape_38(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_38') as vsimem_dir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(vsimem_dir) as ds:
        ds.CreateLayer('test35')

      self.assertIsNotNone(gdal.VSIStatL(f'{vsimem_dir}/test35.shp'))

      with ogr.Open(vsimem_dir, update=1) as ds:
        with gdal.quiet_errors():
          lyr = ds.CreateLayer('test35')
        self.assertIsNone(lyr, 'should not have created a new layer')

  def test_ogr_shape_39(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/multipatch.shp')) as ds:
      lyr = ds.GetLayer(0)
      feat_read = lyr.GetNextFeature()
      self._check_feature_geometry(
          feat_read,
          ogr.CreateGeometryFromWkt(
              'GEOMETRYCOLLECTION (TIN (((5 4 10,0 0 5,10 0 5,5 4 10)),((5 4'
              ' 10,10 0 5,10 8 5,5 4 10)),((5 4 10,10 8 5,0 8 5,5 4 10)),((5 4'
              ' 10,0 8 5,0 0 5,5 4 10))),TIN (((10 0 5,10 0 0,10 8 5,10 0'
              ' 5)),((10 0 0,10 8 5,10 8 0,10 0 0)),((10 8 5,10 8 0,0 8 5,10 8'
              ' 5)),((10 8 0,0 8 5,0 8 0,10 8 0)),((0 8 5,0 8 0,0 0 5,0 8'
              ' 5)),((0 8 0,0 0 5,0 0 0,0 8 0))),MULTIPOLYGON (((0 0 0,0 0 5,10'
              ' 0 5,10 0 0,6 0 0,6 0 3,4 0 3,4 0 0,0 0 0),(1 0 2,3 0 2,3 0 4,1'
              ' 0 4,1 0 2),(7 0 2,9 0 2,9 0 4,7 0 4,7 0 2))))'
          ),
          max_error=1e-9,
      )

  @parameterized.named_parameters(
      ('update', 'update'),
      ('add', 'add'),
      ('delete', 'delete'),
  )
  def test_ogr_shape_40(self, operation: str) -> None:
    datafiles = ('gjpoint.dbf', 'gjpoint.shp', 'gjpoint.shx')
    indexfiles = ('gjpoint.sbn', 'gjpoint.sbx', 'gjpoint.qix')
    with gcore_util.TestTemporaryDirectory(prefix='shape_40') as tmpdir:
      for filename in datafiles:
        shutil.copyfile(
            ogr_util.GetTestFilePath(filename),
            os.path.join(tmpdir, filename),
        )
      for i in range(2):
        shutil.copyfile(
            ogr_util.GetTestFilePath(indexfiles[i]),
            os.path.join(tmpdir, indexfiles[i]),
        )

      with ogr.Open(os.path.join(tmpdir, 'gjpoint.shp'), update=1) as shape_ds:
        shape_lyr = shape_ds.GetLayer(0)
        shape_ds.ExecuteSQL('CREATE SPATIAL INDEX ON gjpoint')

        if operation == 'update':
          feat = shape_lyr.GetFeature(0)
          geom = ogr.CreateGeometryFromWkt('POINT (99 1)')
          feat.SetGeometry(geom)

          for filename in indexfiles:
            self.assertTrue(os.path.exists(os.path.join(tmpdir, filename)))

          shape_lyr.SetFeature(feat)
        elif operation == 'add':
          feat = ogr.Feature(shape_lyr.GetLayerDefn())
          geom = ogr.CreateGeometryFromWkt('POINT (98 2)')
          feat.SetGeometry(geom)
          feat.SetField('NAME', 'Point 2')
          feat.SetField('FID', '2')
          feat.SetFID(1)

          for filename in indexfiles:
            self.assertTrue(os.path.exists(os.path.join(tmpdir, filename)))

          shape_lyr.CreateFeature(feat)
        elif operation == 'delete':
          for filename in indexfiles:
            self.assertTrue(os.path.exists(os.path.join(tmpdir, filename)))
          self.assertEqual(
              shape_lyr.DeleteFeature(0), 0, 'DeleteFeature failed.'
          )

  # No test_ogrsf binary in google3 - test_ogr_shape_41
  # No test_ogrsf binary in google3 - test_ogr_shape_42
  # No vsicurl in google3 - test_ogr_shape_43
  # No vsicurl in google3 - test_ogr_shape_44
  # No vsicurl in google3 - test_ogr_shape_vsicurl_url

  def test_ogr_shape_45(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/poly/poly.shp')) as shp_ds:
      shp_layer = shp_ds.GetLayer(0)
      shp_layer.SetIgnoredFields(['AREA'])

      feat = shp_layer.GetNextFeature()
      self.assertFalse(
          feat.IsFieldSet('AREA'), 'got area despite request to ignore it.'
      )
      self.assertEqual(
          feat.GetFieldAsInteger('EAS_ID'), 168, 'missing or wrong eas_id'
      )

      wkt = (
          'POLYGON ((479819.84375 4765180.5,479690.1875 4765259.5,479647.0'
          ' 4765369.5,479730.375 4765400.5,480039.03125 4765539.5,480035.34375'
          ' 4765558.5,480159.78125 4765610.5,480202.28125 4765482.0,480365.0'
          ' 4765015.5,480389.6875 4764950.0,480133.96875 4764856.5,480080.28125'
          ' 4764979.5,480082.96875 4765049.5,480088.8125 4765139.5,480059.90625'
          ' 4765239.5,480019.71875 4765319.5,479980.21875 4765409.5,479909.875'
          ' 4765370.0,479859.875 4765270.0,479819.84375 4765180.5))'
      )
      self._check_feature_geometry(feat, wkt, max_error=1e-8)

      fd = shp_layer.GetLayerDefn()
      fld = fd.GetFieldDefn(0)  # area
      self.assertTrue(
          fld.IsIgnored(), 'AREA unexpectedly not marked as ignored.'
      )

      fld = fd.GetFieldDefn(1)  # eas_id
      self.assertFalse(
          fld.IsIgnored(), 'EAS_ID unexpectedly marked as ignored.'
      )

      self.assertFalse(fd.IsGeometryIgnored(), 'geometry unexpectedly ignored.')
      self.assertFalse(fd.IsStyleIgnored(), 'style unexpectedly ignored.')

      fd.SetGeometryIgnored(1)
      self.assertTrue(
          fd.IsGeometryIgnored(), 'geometry unexpectedly not ignored.'
      )

      feat = shp_layer.GetNextFeature()
      self.assertIsNone(
          feat.GetGeometryRef(), 'Unexpectedly got a geometry on feature 2.'
      )
      self.assertFalse(
          feat.IsFieldSet('AREA'), 'got area despite request to ignore it.'
      )
      self.assertEqual(
          feat.GetFieldAsInteger('EAS_ID'), 179, 'missing or wrong eas_id'
      )

  def test_ogr_shape_46(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_46') as vsimem_dir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(
          f'{vsimem_dir}/ogr_shape_46.shp'
      ) as ds:
        ds.CreateLayer('you_can_put_here_what_you_want_i_dont_care')
        ds.CreateLayer('this_one_i_care_46')

      with ogr.Open(f'{vsimem_dir}/ogr_shape_46.shp') as ds:
        self.assertEqual(ds.GetLayerCount(), 1)

      with ogr.Open(f'{vsimem_dir}/this_one_i_care_46.shp') as ds:
        self.assertEqual(ds.GetLayerCount(), 1)

  @unittest.skip(
      'readlink() is disabled in google3 gdalopeninfo.cpp for CAS compatibility'
  )
  def test_ogr_shape_47(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_47') as tmpdir:
      link_path = os.path.join(tmpdir, 'poly.zip')
      target = '/vsizip/' + ogr_util.GetTestFilePath('shape/poly.zip')
      try:
        os.symlink(target, link_path)
      except (OSError, NotImplementedError):
        self.skipTest('Symlinks not supported')

      with ogr.Open(link_path) as ds:
        self.assertIsNotNone(ds, f'{link_path} symlink does not open.')

  def test_ogr_shape_48(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_48') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_48.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_48')
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(1 2)'))
        lyr.CreateFeature(feat)

        feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(3 4)'))
        lyr.SetFeature(feat)
        extent = lyr.GetExtent()
        self.assertEqual(
            extent, (1, 3, 2, 4), 'did not get expected extent (1)'
        )
        extent_3d = lyr.GetExtent3D()
        self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent3D))
        self.assertEqual(
            extent_3d,
            (1, 3, 2, 4, float('inf'), float('-inf')),
            'did not get expected extent 3D',
        )

        ds.ExecuteSQL('RECOMPUTE EXTENT ON ogr_shape_48')
        extent = lyr.GetExtent()
        self.assertEqual(
            extent, (3, 3, 4, 4), 'did not get expected extent (2)'
        )

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        extent = lyr.GetExtent()
        self.assertEqual(
            extent, (3, 3, 4, 4), 'did not get expected extent (3)'
        )

      ogr.GetDriverByName(DRIVER).DeleteDataSource(shp_path)

      # Test with Polygon
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_48')
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(
            ogr.CreateGeometryFromWkt('POLYGON((0 0,0 -1,-1 -1,-1 0,0 0))')
        )
        lyr.CreateFeature(feat)
        feat.SetGeometry(
            ogr.CreateGeometryFromWkt('POLYGON((0 0,0 1,1 1,1 0,0 0))')
        )
        lyr.SetFeature(feat)
        ds.ExecuteSQL('RECOMPUTE EXTENT ON ogr_shape_48')
        extent = lyr.GetExtent()
        self.assertEqual(
            extent, (0, 1, 0, 1), 'did not get expected extent (4)'
        )

      ogr.GetDriverByName(DRIVER).DeleteDataSource(shp_path)

      # Test with PolygonZ
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_48')
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'POLYGON((0 0 -2,0 -1 -2,-1 -1 -2,-1 0 -2,0 0 -2))'
            )
        )
        lyr.CreateFeature(feat)
        feat.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'POLYGON((0 0 2,0 1 1,1 1 2,1 0 2,0 0 3))'
            )
        )
        lyr.SetFeature(feat)
        ds.ExecuteSQL('RECOMPUTE EXTENT ON ogr_shape_48')
        extent = lyr.GetExtent()
        self.assertEqual(
            extent, (0, 1, 0, 1), 'did not get expected extent (4)'
        )
        extent_3d = lyr.GetExtent3D()
        self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent3D))
        self.assertEqual(
            extent_3d,
            (0, 1, 0, 1, 1, 3),
            'did not get expected extent 3D',
        )

  def test_ogr_shape_49(self) -> None:
    with ogr.Open(
        ogr_util.GetTestFilePath('shape/facility_surface_dd.dbf')
    ) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(
          lyr.GetMetadata_Dict('SHAPEFILE'),
          {
              'ENCODING_FROM_LDID': 'ISO-8859-1',
              'LDID_VALUE': '87',
              'SOURCE_ENCODING': 'ISO-8859-1',
          },
      )
      feat = lyr.GetFeature(91)
      name = feat.GetField('NAME')
      self.assertEqual(
          name,
          'OSEBERG S\u00d8R',
          'Did not get expected name, encoding problems?',
      )

  def test_ogr_shape_50(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/chinese.dbf')) as ds:
      if ds is None:
        self.skipTest('Cannot open chinese.dbf')
      lyr = ds.GetLayer(0)
      reconv_possible = lyr.TestCapability(ogr.OLCStringsAsUTF8) == 1

      if 'Recode from CP936 to UTF-8' in gdal.GetLastErrorMsg():
        self.assertFalse(
            reconv_possible,
            'Recode failed, but TestCapability(OLCStringsAsUTF8) returns TRUE',
        )
        self.skipTest('skipping test: iconv support needed')

      self.assertEqual(
          lyr.GetMetadata_Dict('SHAPEFILE'),
          {
              'ENCODING_FROM_LDID': 'CP936',
              'LDID_VALUE': '77',
              'SOURCE_ENCODING': 'CP936',
          },
      )

      fieldname = '\u4e2d\u56fd'
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldIndex(fieldname),
          0,
          lyr.GetLayerDefn().GetFieldDefn(0).GetNameRef(),
      )
      self.assertTrue(
          reconv_possible, 'TestCapability(OLCStringsAsUTF8) should return TRUE'
      )

      with ds.ExecuteSQL(f'SELECT * FROM {lyr.GetName()}') as sql_lyr:
        self.assertTrue(sql_lyr.TestCapability(ogr.OLCStringsAsUTF8))

      if ogr.GetDriverByName('SQLITE'):
        with ds.ExecuteSQL(
            f'SELECT * FROM {lyr.GetName()}', dialect='SQLITE'
        ) as sql_lyr:
          self.assertTrue(sql_lyr.TestCapability(ogr.OLCStringsAsUTF8))

  def test_ogr_shape_51(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_51') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_51.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_51')
        feat = ogr.Feature(lyr.GetLayerDefn())
        lyr.CreateFeature(feat)

      gdal.Unlink(f'{vsimem_dir}/ogr_shape_51.dbf')

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        feat = lyr.GetNextFeature()
        feat.SetField(0, 'bar')
        lyr.SetFeature(feat)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        value = feat.GetFieldAsString(0)
        field_count = lyr.GetLayerDefn().GetFieldCount()

      ogr.GetDriverByName(DRIVER).DeleteDataSource(shp_path)
      self.assertEqual(field_count, 1, 'did not get expected field count')
      self.assertEqual(value, 'bar', 'did not get expected value')

  def test_ogr_shape_52(self) -> None:
    expected_geom = ogr.CreateGeometryFromWkt(
        'MULTIPOLYGON (((175.524709766699999 -40.17203475,'
        '175.524757883299998 -40.172050566700001,'
        '175.52480505 -40.1720663,'
        '175.524858766699992 -40.172091433299997,'
        '175.524913916700001 -40.172112966699999,'
        '175.524966049999989 -40.172136933300003,'
        '175.525030633299991 -40.17216185,'
        '175.5250873 -40.17218215,'
        '175.52515168330001 -40.1722011,'
        '175.525217666700001 -40.172221216700002,'
        '175.525269416700013 -40.172234466699997,'
        '175.5253165 -40.1722478,'
        '175.52535415 -40.1722577667,'
        '175.52538385 -40.17226365,'
        '175.525436816699994 -40.1722814333,'
        '175.525507016700004 -40.17229905,'
        '175.525594783299994 -40.172322033299999,'
        '175.525669933300009 -40.172339533299997,'
        '175.52574 -40.17235335,'
        '175.525807566699996 -40.1723672,'
        '175.52585005 -40.17237395,'
        '175.52588115 -40.172378683300003,'
        '175.525969816700012 -40.172388633300002,'
        '175.526057266700008 -40.1724020833,'
        '175.52723455 -40.17253515,'
        '175.527275583299996 -40.1725388,'
        '175.527324533300003 -40.17254675,'
        '175.527394866700007 -40.172552766700001,'
        '175.527473066699997 -40.172561616700001,'
        '175.527576666700014 -40.172572916699998,'
        '175.527678333300003 -40.172584266699999,'
        '175.527787883299993 -40.17259845,'
        '175.52789345 -40.172609716700002,'
        '175.527953933300012 -40.17261295,'
        '175.528028083300001 -40.1726174,'
        '175.52809835 -40.1726219333,'
        '175.528151650000012 -40.172625833300003,'
        '175.528190349999988 -40.17262725,'
        '175.528230900000011 -40.172631183299998,'
        '175.5282776 -40.1726338,'
        '175.528322800000012 -40.172637633299999,'
        '175.5283648 -40.17263915,'
        '175.5284115 -40.172641766700004,'
        '175.528452133299993 -40.17264435,'
        '175.528492133300006 -40.172646033299998,'
        '175.52856465 -40.17264805,'
        '175.528621733300014 -40.1726492,'
        '175.52868035 -40.172650333299998,'
        '175.528751333299994 -40.172652383299997,'
        '175.528814566699992 -40.1726534,'
        '175.528883933299994 -40.172653116699998,'
        '175.528939383300013 -40.17265195,'
        '175.529002566700001 -40.1726518,'
        '175.529070350000012 -40.172650366699997,'
        '175.529136633299998 -40.17265015,'
        '175.529193616700013 -40.17264895,'
        '175.529250616700011 -40.172647733300003,'
        '175.529313800000011 -40.172647583299998,'
        '175.529376783299995 -40.172647016699997,'
        '175.52895773329999 -40.172694633299997,'
        '175.528450866700013 -40.172752216699998,'
        '175.52835635 -40.172753466700001,'
        '175.52741181670001 -40.1727757333,'
        '175.52685245 -40.172532333299998,'
        '175.52627245 -40.172501266700003,'
        '175.5262405167 -40.172502816700003,'
        '175.5258356 -40.172522816700003,'
        '175.5256125 -40.172533833300001,'
        '175.525424433300003 -40.172543116699998,'
        '175.524834133300004 -40.1725533,'
        '175.524739033299994 -40.172414983300001,'
        '175.5247128 -40.17207405,'
        '175.524709766699999 -40.17203475)),'
        '((175.531267916699989 -40.17286525,'
        '175.5312654 -40.172863283300003,'
        '175.531252849999987 -40.172853516700002,'
        '175.531054566699993 -40.172822366699997,'
        '175.530193283300008 -40.172687333299997,'
        '175.529890266699994 -40.1726398,'
        '175.529916116700008 -40.172639383300002,'
        '175.529972483300014 -40.172639216699999,'
        '175.53002885 -40.1726398,'
        '175.530085183300002 -40.17264115,'
        '175.530141500000013 -40.17264325,'
        '175.530197733300014 -40.172646133299999,'
        '175.530253916699991 -40.172649766699998,'
        '175.530309983299986 -40.172654166699999,'
        '175.53036595 -40.172659333299997,'
        '175.5304218 -40.17266525,'
        '175.53047748329999 -40.172671916699997,'
        '175.530533016699991 -40.17267935,'
        '175.5305883833 -40.1726875333,'
        '175.530643533300008 -40.172696466700003,'
        '175.530722333299991 -40.172710633299999,'
        '175.530800633300004 -40.1727263167,'
        '175.5308541 -40.17273795,'
        '175.5309073 -40.1727503,'
        '175.530960216700009 -40.172763366700003,'
        '175.531012816700013 -40.172777133300002,'
        '175.5310651 -40.1727916,'
        '175.53111705 -40.172806766699999,'
        '175.531168650000012 -40.172822633300001,'
        '175.531219883299997 -40.172839183299999,'
        '175.531270733300005 -40.1728564,'
        '175.531267916699989 -40.17286525)))'
    )

    with ogr.Open(ogr_util.GetTestFilePath('shape/test3356.shp')) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self._check_feature_geometry(feat, expected_geom, max_error=1e-9)

    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_52') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_52.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_52')
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(expected_geom)
        lyr.CreateFeature(feat)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self._check_feature_geometry(feat, expected_geom, max_error=1e-9)

  def test_ogr_shape_53(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_53') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_53.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_53')

        # Test ReorderFields() when there are no fields
        ret = lyr.ReorderFields([])
        self.assertEqual(ret, 0)

        # Test REPACK when there are no features
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ds.ExecuteSQL('REPACK ogr_shape_53')
        self.assertEqual(gdal.GetLastErrorMsg(), '')

        # Create a field
        fd = ogr.FieldDefn('foo', ogr.OFTString)
        lyr.CreateField(fd)

        # GetFeature() on an invalid FID
        gdal.ErrorReset()
        with gdal.quiet_errors():
          feat = lyr.GetFeature(-1)
        self.assertIsNone(feat)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # SetFeature() on an invalid FID
        gdal.ErrorReset()
        with gdal.quiet_errors():
          feat = ogr.Feature(lyr.GetLayerDefn())
          ret = lyr.SetFeature(feat)
        self.assertNotEqual(ret, 0)

        # SetFeature() on FID 1000
        gdal.ErrorReset()
        with gdal.quiet_errors():
          feat = ogr.Feature(lyr.GetLayerDefn())
          feat.SetFID(1000)
          ret = lyr.SetFeature(feat)
        self.assertNotEqual(ret, 0)

        # DeleteFeature() on an invalid FID
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.DeleteFeature(-1)
        self.assertNotEqual(ret, 0)

        feat = ogr.Feature(lyr.GetLayerDefn())
        lyr.CreateFeature(feat)

        ret = lyr.DeleteFeature(0)
        self.assertEqual(ret, 0)

        # Try deleting an already deleted feature
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.DeleteFeature(0)
        self.assertNotEqual(ret, 0)

        # Test DeleteField() on an invalid index
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.DeleteField(-1)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test ReorderFields() with invalid permutation
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.ReorderFields([1])
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test AlterFieldDefn() on an invalid index
        gdal.ErrorReset()
        with gdal.quiet_errors():
          fd = ogr.FieldDefn('foo2', ogr.OFTString)
          ret = lyr.AlterFieldDefn(-1, fd, 0)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test AlterFieldDefn() attempting conversion from OFTString to Integer
        gdal.ErrorReset()
        with gdal.quiet_errors():
          fd = ogr.FieldDefn('foo', ogr.OFTInteger)
          ret = lyr.AlterFieldDefn(0, fd, ogr.ALTER_TYPE_FLAG)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test DROP SPATIAL INDEX ON layer without index
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ds.ExecuteSQL('DROP SPATIAL INDEX ON ogr_shape_53')
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Re-create a feature
        feat = ogr.Feature(lyr.GetLayerDefn())
        lyr.CreateFeature(feat)

      # Test that some operations are not possible in read-only mode
      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)

        self.assertEqual(lyr.TestCapability(ogr.OLCSequentialWrite), 0)
        self.assertEqual(lyr.TestCapability(ogr.OLCDeleteFeature), 0)
        self.assertEqual(lyr.TestCapability(ogr.OLCCreateField), 0)
        self.assertEqual(lyr.TestCapability(ogr.OLCDeleteField), 0)
        self.assertEqual(lyr.TestCapability(ogr.OLCReorderFields), 0)
        self.assertEqual(lyr.TestCapability(ogr.OLCAlterFieldDefn), 0)

        # Test CreateField()
        fd = ogr.FieldDefn('bar', ogr.OFTString)
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.CreateField(fd)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test ReorderFields()
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.ReorderFields([0])
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test DeleteField()
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.DeleteField(0)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test AlterFieldDefn()
        gdal.ErrorReset()
        with gdal.quiet_errors():
          fd = ogr.FieldDefn('foo2', ogr.OFTString)
          ret = lyr.AlterFieldDefn(0, fd, 0)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test CreateFeature()
        feat = ogr.Feature(lyr.GetLayerDefn())
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.CreateFeature(feat)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test DeleteFeature()
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.DeleteFeature(0)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test SetFeature()
        feat = lyr.GetNextFeature()
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.SetFeature(feat)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test REPACK
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ds.ExecuteSQL('REPACK ogr_shape_53')
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test RECOMPUTE EXTENT ON
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ds.ExecuteSQL('RECOMPUTE EXTENT ON ogr_shape_53')
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

      # Attempt to delete shape in shapefile with no .dbf file
      gdal.Unlink(f'{vsimem_dir}/ogr_shape_53.dbf')
      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)

        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.DeleteFeature(0)
        self.assertNotEqual(ret, 0)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

        # Test REPACK
        ds.ExecuteSQL('REPACK ogr_shape_53')

      # Tests on a DBF only
      with ogr.Open(ogr_util.GetTestFilePath('shape/idlink.dbf')) as ds:
        lyr = ds.GetLayer(0)
        lyr.GetExtent()

        # Test RECOMPUTE EXTENT ON
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ds.ExecuteSQL('RECOMPUTE EXTENT ON ogr_shape_53')
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_shape_54(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    num_layers = 500
    lru_list_size = 100
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_54') as vsimem_dir:
      ds_name = f'{vsimem_dir}/ds'

      # Test creating N layers
      with shape_drv.CreateDataSource(ds_name) as ds:
        for i in range(num_layers):
          self._create_numbered_layer(ds, i)

      # Test access to the N layers in sequence
      with ogr.Open(ds_name) as ds:
        for i in range(num_layers):
          self._verify_numbered_layer(ds, i)

        # Now some 'random' access
        self._verify_numbered_layer(ds, num_layers - 1 - lru_list_size)
        self._verify_numbered_layer(ds, num_layers - lru_list_size // 2)
        self._verify_numbered_layer(ds, num_layers - lru_list_size // 4)
        self._verify_numbered_layer(ds, 0)
        self._verify_numbered_layer(ds, 0)
        self._verify_numbered_layer(ds, 2)
        self._verify_numbered_layer(ds, 1)

      # Test adding a new layer
      with ogr.Open(ds_name, update=1) as ds:
        self._create_numbered_layer(ds, num_layers)

      # Test accessing the new layer
      with ogr.Open(ds_name) as ds:
        self._verify_numbered_layer(ds, num_layers)

      # Test deleting layers
      with ogr.Open(ds_name, update=1) as ds:
        for i in range(num_layers):
          self._verify_numbered_layer(ds, i)
        for i in range(num_layers - lru_list_size + 1, num_layers):
          ds.ExecuteSQL(f'DROP TABLE layer{i:03d}')
        self._verify_numbered_layer(ds, num_layers - lru_list_size)
        self._create_numbered_layer(ds, num_layers + 2)
        for i in range(0, num_layers - lru_list_size + 1):
          ds.ExecuteSQL(f'DROP TABLE layer{i:03d}')
        self._verify_numbered_layer(ds, num_layers)
        self._verify_numbered_layer(ds, num_layers + 2)

      # Destroy and recreate datasource
      shape_drv.DeleteDataSource(ds_name)
      with shape_drv.CreateDataSource(ds_name) as ds:
        for i in range(num_layers):
          self._create_numbered_layer(ds, i)

      # Reopen in read-only so as to be able to delete files
      with ogr.Open(ds_name) as ds:
        # Test corner case where we cannot reopen a closed layer
        deleted_layer = 0
        gdal.Unlink(f'{ds_name}/layer{deleted_layer:03d}.shp')
        with gdal.quiet_errors():
          lyr = ds.GetLayerByName(f'layer{deleted_layer:03d}')
        if lyr is not None:
          gdal.ErrorReset()
          with gdal.quiet_errors():
            lyr.ResetReading()
            lyr.GetNextFeature()
          self.assertNotEqual(gdal.GetLastErrorMsg(), '')
        gdal.ErrorReset()

        deleted_layer = 1
        gdal.Unlink(f'{ds_name}/layer{deleted_layer:03d}.dbf')
        lyr = ds.GetLayerByName(f'layer{deleted_layer:03d}')
        gdal.ErrorReset()
        with gdal.quiet_errors():
          lyr.ResetReading()
          lyr.GetNextFeature()
        gdal.ErrorReset()

  def test_ogr_shape_55(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_55') as vsimem_dir:
      with shape_drv.CreateDataSource(vsimem_dir) as ds:
        lyr = ds.CreateLayer('ogr_shape_55')
        max_field_count = (65535 - 33) // 32  # 2046

        for i in range(max_field_count):
          if i == 255:
            gdal.ErrorReset()
            with gdal.quiet_errors():
              ret = lyr.CreateField(ogr.FieldDefn(f'foo{i}', ogr.OFTInteger))
            self.assertNotEqual(
                gdal.GetLastErrorMsg(),
                '',
                'expecting a warning for 256th field added',
            )
          else:
            ret = lyr.CreateField(ogr.FieldDefn(f'foo{i}', ogr.OFTInteger))
          self.assertEqual(ret, 0, f'failed creating field foo{i}')

        with gdal.quiet_errors():
          ret = lyr.CreateField(
              ogr.FieldDefn(f'foo{max_field_count}', ogr.OFTInteger)
          )
        self.assertNotEqual(
            ret, 0, f'should have failed creating field foo{max_field_count}'
        )

        for _ in range(2):
          feat = ogr.Feature(lyr.GetLayerDefn())
          for i in range(max_field_count):
            feat.SetField(i, i)
          lyr.CreateFeature(feat)

  def test_ogr_shape_56(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_56') as vsimem_dir:
      with shape_drv.CreateDataSource(vsimem_dir) as ds:
        lyr = ds.CreateLayer('ogr_shape_56')
        max_field_count = 65535 // 80  # 819

        for i in range(max_field_count):
          if i == 255:
            gdal.ErrorReset()
            with gdal.quiet_errors():
              ret = lyr.CreateField(ogr.FieldDefn(f'foo{i}', ogr.OFTString))
            self.assertNotEqual(
                gdal.GetLastErrorMsg(),
                '',
                'expecting a warning for 256th field added',
            )
          else:
            ret = lyr.CreateField(ogr.FieldDefn(f'foo{i}', ogr.OFTString))
          self.assertEqual(ret, 0, f'failed creating field foo{i}')

        with gdal.quiet_errors():
          ret = lyr.CreateField(
              ogr.FieldDefn(f'foo{max_field_count}', ogr.OFTString)
          )
        self.assertNotEqual(
            ret, 0, f'should have failed creating field foo{max_field_count}'
        )

        for _ in range(2):
          feat = ogr.Feature(lyr.GetLayerDefn())
          for i in range(max_field_count):
            feat.SetField(i, f'foo{i}')
          lyr.CreateFeature(feat)

  def test_ogr_shape_57(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_57') as vsimem_dir:
      with shape_drv.CreateDataSource(vsimem_dir) as ds:
        lyr = ds.CreateLayer('ogr_shape_57')
        field_defn = ogr.FieldDefn('foo', ogr.OFTString)
        field_defn.SetWidth(1024)

        gdal.ErrorReset()
        with gdal.quiet_errors():
          lyr.CreateField(field_defn)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '', 'expecting a warning')

        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField(0, '0123456789' * 27)

        gdal.ErrorReset()
        with gdal.quiet_errors():
          lyr.CreateFeature(feat)
        self.assertNotEqual(gdal.GetLastErrorMsg(), '', 'expecting a warning')

  def test_ogr_shape_58(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    wkt_list = [
        'POINT (0 1)',
        'POINT (0 1 2)',
        'MULTIPOINT (0 1,2 3)',
        'MULTIPOINT (0 1 2,3 4 5)',
        'LINESTRING (0 1,2 3)',
        'LINESTRING (0 1 2,3 4 5)',
        'MULTILINESTRING ((0 1,2 3),(0 1,2 3))',
        'MULTILINESTRING ((0 1 2,3 4 5),(0 1 2,3 4 5))',
        'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
        'POLYGON ((0 0 2,0 1 2,1 1 2,1 0 2,0 0 2))',
        (
            'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)),'
            '((100 0,100 1,101 1,101 0,100 0)))'
        ),
        (
            'MULTIPOLYGON (((0 0 2,0 1 2,1 1 2,1 0 2,0 0 2)),'
            '((100 0 2,100 1 2,101 1 2,101 0 2,100 0 2)))'
        ),
    ]

    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_58') as vsimem_dir:
      with shape_drv.CreateDataSource(vsimem_dir) as ds:
        for wkt in wkt_list:
          geom = ogr.CreateGeometryFromWkt(wkt)
          layer_name = geom.GetGeometryName()
          if geom.GetGeometryType() & ogr.wkb25Bit:
            layer_name = layer_name + '3D'
          lyr = ds.CreateLayer(layer_name)
          feat = ogr.Feature(lyr.GetLayerDefn())
          feat.SetGeometry(geom)
          lyr.CreateFeature(feat)

      with ogr.Open(vsimem_dir) as ds:
        for wkt in wkt_list:
          geom = ogr.CreateGeometryFromWkt(wkt)
          layer_name = geom.GetGeometryName()
          if geom.GetGeometryType() & ogr.wkb25Bit:
            layer_name = layer_name + '3D'
          lyr = ds.GetLayerByName(layer_name)
          lyr.ResetReading()
          feat = lyr.GetNextFeature()
          geom_read = feat.GetGeometryRef()
          self.assertEqual(
              geom_read.ExportToWkt(),
              wkt,
              f'did not get expected geom for field {layer_name}',
          )

  def test_ogr_shape_59(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/testpointm.shp')) as shp_ds:
      if shp_ds is None:
        self.skipTest('Cannot open testpointm.shp')
      shp_lyr = shp_ds.GetLayer(0)
      feat = shp_lyr.GetNextFeature()
      geom = feat.GetGeometryRef()
      self.assertEqual(
          geom.GetGeometryName(), 'POINT', 'Geometry of wrong type.'
      )
      self.assertEqual(geom.GetCoordinateDimension(), 2, 'dimension wrong.')
      self.assertEqual(
          geom.GetPointZM(0),
          (1.0, 2.0, 0.0, 3.0),
          'Did not get right point result.',
      )

    with ogr.Open(ogr_util.GetTestFilePath('shape/arcm_with_m.shp')) as shp_ds:
      shp_lyr = shp_ds.GetLayer(0)
      feat = shp_lyr.GetNextFeature()
      geom = feat.GetGeometryRef()
      self.assertEqual(geom.ExportToIsoWkt(), 'LINESTRING M (0 0 10,1 1 20)')
      feat = shp_lyr.GetNextFeature()
      geom = feat.GetGeometryRef()
      self.assertEqual(
          geom.ExportToIsoWkt(),
          'MULTILINESTRING M ((0 0 10,1 1 20),(2 2 30,3 3 40))',
      )

    with ogr.Open(
        ogr_util.GetTestFilePath('shape/polygonm_with_m.shp')
    ) as shp_ds:
      shp_lyr = shp_ds.GetLayer(0)
      feat = shp_lyr.GetNextFeature()
      geom = feat.GetGeometryRef()
      self.assertEqual(
          geom.ExportToIsoWkt(), 'POLYGON M ((0 0 10,0 1 20,1 1 30,0 0 40))'
      )
      feat = shp_lyr.GetNextFeature()
      geom = feat.GetGeometryRef()
      self.assertEqual(
          geom.ExportToIsoWkt(),
          'POLYGON M ((0 0 10,0 1 20,1 1 30,0 0 40),'
          '(0.25 0.25 50,0.75 0.75 60,0.25 0.75 70,0.25 0.25 80))',
      )

  def test_ogr_shape_60(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/testpointzm.shp')) as shp_ds:
      if shp_ds is None:
        self.skipTest('Cannot open testpointzm.shp')
      shp_lyr = shp_ds.GetLayer(0)
      feat = shp_lyr.GetNextFeature()
      geom = feat.GetGeometryRef()
      self.assertEqual(
          geom.GetGeometryName(), 'POINT', 'Geometry of wrong type.'
      )
      self.assertEqual(geom.GetCoordinateDimension(), 3, 'dimension wrong.')
      self.assertEqual(
          geom.GetPoint(0), (1.0, 2.0, 3.0), 'Did not get right point result.'
      )

  def test_ogr_shape_61(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_61') as vsimem_dir:
      with shape_drv.CreateDataSource(vsimem_dir) as ds:
        lyr = ds.CreateLayer('ogr_shape_61')
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))

        field_defn = ogr.FieldDefn('intfield', ogr.OFTInteger)
        field_defn.SetWidth(1)
        lyr.CreateField(field_defn)

        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField(0, '0123456789' * 8)
        feat.SetField(1, 2)
        lyr.CreateFeature(feat)

        field_defn = lyr.GetLayerDefn().GetFieldDefn(0)
        self.assertEqual(
            field_defn.GetWidth(), 80, 'did not get initial field size'
        )

        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField(0, '0123456789' * 9)
        feat.SetField(1, 34)
        lyr.CreateFeature(feat)

        field_defn = lyr.GetLayerDefn().GetFieldDefn(0)
        self.assertEqual(field_defn.GetWidth(), 90, 'did not extend field')

        field_defn = lyr.GetLayerDefn().GetFieldDefn(1)
        self.assertEqual(field_defn.GetWidth(), 2, 'did not extend field')

      with ogr.Open(vsimem_dir) as ds:
        lyr = ds.GetLayer(0)
        field_defn = lyr.GetLayerDefn().GetFieldDefn(0)
        self.assertEqual(
            field_defn.GetWidth(), 90, 'did not get expected field size'
        )

        feat = lyr.GetFeature(1)
        self.assertEqual(
            feat.GetFieldAsString(0),
            '0123456789' * 9,
            'did not get expected field value',
        )
        self.assertEqual(
            feat.GetFieldAsInteger(1), 34, 'did not get expected field value'
        )

  def test_ogr_shape_62(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    values = ['ab', 'deef', 'ghi']
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_62') as vsimem_dir:
      with shape_drv.CreateDataSource(vsimem_dir) as ds:
        lyr = ds.CreateLayer('ogr_shape_62', options=['RESIZE=YES'])
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        lyr.CreateField(ogr.FieldDefn('bar', ogr.OFTInteger))
        lyr.CreateField(ogr.FieldDefn('baz', ogr.OFTInteger))

        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField(0, 'hugehugehugehuge')
        lyr.CreateFeature(feat)

        lyr.DeleteFeature(0)

        for value in values:
          feat = ogr.Feature(lyr.GetLayerDefn())
          feat.SetField(0, value)
          feat.SetField(2, 12)
          lyr.CreateFeature(feat)

      # Reopen file
      with ogr.Open(vsimem_dir) as ds:
        lyr = ds.GetLayer(0)
        field_defn = lyr.GetLayerDefn().GetFieldDefn(0)
        self.assertEqual(
            field_defn.GetWidth(), 4, 'did not get expected field size'
        )

      # Reopen file in update mode
      with ogr.Open(vsimem_dir, update=1) as ds:
        lyr = ds.GetLayer(0)
        # Should do nothing
        ds.ExecuteSQL('RESIZE ogr_shape_62')

        lyr.ResetReading()
        for expected_value in values:
          feat = lyr.GetNextFeature()
          self.assertEqual(
              feat.GetFieldAsString(0),
              expected_value,
              'did not get expected value',
          )
          self.assertEqual(
              feat.GetFieldAsInteger(2), 12, 'did not get expected value'
          )

  def test_ogr_shape_63(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_63') as vsimem_dir:
      dbf_path = f'{vsimem_dir}/ogr_shape_63.dbf'
      cpg_path = f'{vsimem_dir}/ogr_shape_63.cpg'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(dbf_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_63', geom_type=ogr.wkbNone)
        fieldname = '\xc3\xa9'
        self.assertEqual(
            lyr.CreateField(ogr.FieldDefn(fieldname, ogr.OFTString)), 0
        )

        fieldname = '\xc3\xa9\xc3\xa9'
        self.assertEqual(
            lyr.AlterFieldDefn(
                0,
                ogr.FieldDefn(fieldname, ogr.OFTString),
                ogr.ALTER_NAME_FLAG,
            ),
            0,
        )

        chinese_str = struct.pack('B' * 6, 229, 144, 141, 231, 167, 176).decode(
            'UTF-8'
        )

        with gdal.quiet_errors():
          ret = lyr.AlterFieldDefn(
              0,
              ogr.FieldDefn(chinese_str, ogr.OFTString),
              ogr.ALTER_NAME_FLAG,
          )
        self.assertNotEqual(ret, 0)

        with gdal.quiet_errors():
          ret = lyr.CreateField(ogr.FieldDefn(chinese_str, ogr.OFTString))
        self.assertNotEqual(ret, 0)

      with ogr.Open(dbf_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.TestCapability(ogr.OLCStringsAsUTF8), 1)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetName(), fieldname
        )

      # Set an invalid encoding
      gdal.FileFromMemBuffer(cpg_path, 'FOO')

      with ogr.Open(dbf_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.TestCapability(ogr.OLCStringsAsUTF8), 0)

  def test_ogr_shape_64(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_64') as vsimem_dir:
      ds_path = f'{vsimem_dir}/ogr_shape_64'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(ds_path) as ds:
        lyr = ds.CreateLayer('a.b')
        self.assertEqual(lyr.GetName(), 'a.b')
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('foo', 'bar')
        lyr.CreateFeature(feat)

        lyr = ds.CreateLayer('a.c')
        self.assertEqual(lyr.GetName(), 'a.c')

        # Test that we cannot create a duplicate layer
        with gdal.quiet_errors():
          lyr = ds.CreateLayer('a.b')
        self.assertIsNone(lyr)

      with ogr.Open(f'{ds_path}/a.b.shp') as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat.GetFieldAsString('foo'), 'bar')

  def test_ogr_shape_65(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/nan.dbf')) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      val = feat.GetFieldAsDouble(0)
    self.assertTrue(math.isnan(val))

  def test_ogr_shape_66(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with shape_drv.CreateDataSource('/i_dont_exist/bar.dbf') as ds:
      with gdal.quiet_errors():
        lyr = ds.CreateLayer('bar', geom_type=ogr.wkbNone)
      self.assertIsNone(lyr)

    with shape_drv.CreateDataSource('/i_dont_exist/bar.shp') as ds:
      with gdal.quiet_errors():
        lyr = ds.CreateLayer('bar', geom_type=ogr.wkbPoint)
      self.assertIsNone(lyr)

    with gdal.quiet_errors():
      ds = shape_drv.CreateDataSource('/i_dont_exist/bar')
    self.assertIsNone(ds)

    with gcore_util.TestTemporaryDirectory(prefix='shape_66') as tmpdir:
      foo_path = os.path.join(tmpdir, 'foo')
      with open(foo_path, 'wb'):
        pass
      with gdal.quiet_errors():
        ds = shape_drv.CreateDataSource(foo_path)
      self.assertIsNone(ds)

  def test_ogr_shape_67(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_67') as tmpdir:
      for ext in ('shp', 'shx', 'sbn', 'sbx'):
        filename = f'emptyshapefilewithsbn.{ext}'
        shutil.copyfile(
            ogr_util.GetTestFilePath(f'shape/{filename}'),
            os.path.join(tmpdir, filename),
        )

      with ogr.Open(
          os.path.join(tmpdir, 'emptyshapefilewithsbn.shp'), update=1
      ) as ds:
        ds.ExecuteSQL('DROP SPATIAL INDEX ON emptyshapefilewithsbn')

      with self.assertRaises(OSError):
        os.stat(os.path.join(tmpdir, 'emptyshapefilewithsbn.sbn'))

  @unittest.skipIf(sys.platform == 'darwin', 'Fails on MacOSX. Not sure why.')
  def test_ogr_shape_68(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_68') as tmpdir:
      mixedcase_dir = os.path.join(tmpdir, 'mixedcase')
      for i in range(2):
        if i == 1 and sys.platform != 'win32':
          break

        if os.path.exists(mixedcase_dir):
          shutil.rmtree(mixedcase_dir)
        os.mkdir(mixedcase_dir)
        shutil.copyfile(
            ogr_util.GetTestFilePath('shape/poly/poly.shp'),
            os.path.join(mixedcase_dir, 'mixedcase.shp'),
        )
        shutil.copyfile(
            ogr_util.GetTestFilePath('shape/poly/poly.shx'),
            os.path.join(mixedcase_dir, 'mixedcase.shx'),
        )
        shutil.copyfile(
            ogr_util.GetTestFilePath('shape/poly/poly.dbf'),
            os.path.join(mixedcase_dir, 'MIXEDCASE.DBF'),
        )

        with ogr.Open(mixedcase_dir, update=1) as ds:
          expected_layer_count = 1 if sys.platform == 'win32' else 2
          self.assertEqual(
              ds.GetLayerCount(),
              expected_layer_count,
              f'expected {expected_layer_count} layers, got '
              f'{ds.GetLayerCount()}',
          )
          lyr = ds.GetLayerByName('mixedcase' if i == 1 else 'MIXEDCASE')
          lyr.DeleteFeature(0)
          ds.ExecuteSQL('REPACK mixedcase' if i == 1 else 'REPACK MIXEDCASE')

          if sys.platform == 'win32':
            self.assertEqual(lyr.GetGeomType(), ogr.wkbPolygon)
          else:
            self.assertEqual(lyr.GetGeomType(), ogr.wkbNone)
            lyr = ds.GetLayerByName('mixedcase')
            self.assertEqual(lyr.GetGeomType(), ogr.wkbPolygon)
            with gdal.quiet_errors():
              ret = lyr.DeleteFeature(0)
            self.assertNotEqual(ret, 0, 'expected failure on DeleteFeature()')
            ds.ExecuteSQL('REPACK mixedcase')

        ori_shp_size = os.stat(
            ogr_util.GetTestFilePath('shape/poly/poly.shp')
        ).st_size
        ori_shx_size = os.stat(
            ogr_util.GetTestFilePath('shape/poly/poly.shx')
        ).st_size
        ori_dbf_size = os.stat(
            ogr_util.GetTestFilePath('shape/poly/poly.dbf')
        ).st_size

        new_shp_size = os.stat(
            os.path.join(mixedcase_dir, 'mixedcase.shp')
        ).st_size
        new_shx_size = os.stat(
            os.path.join(mixedcase_dir, 'mixedcase.shx')
        ).st_size
        new_dbf_size = os.stat(
            os.path.join(mixedcase_dir, 'MIXEDCASE.DBF')
        ).st_size

        self.assertNotEqual(new_dbf_size, ori_dbf_size)
        if sys.platform == 'win32':
          self.assertNotEqual(new_shp_size, ori_shp_size)
          self.assertNotEqual(new_shx_size, ori_shx_size)
        else:
          self.assertEqual(new_shp_size, ori_shp_size)
          self.assertEqual(new_shx_size, ori_shx_size)

  def test_ogr_shape_69(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_69') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_69.shp'
      with shape_drv.CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_69')
        field_defn = ogr.FieldDefn('intfield', ogr.OFTInteger)
        field_defn.SetWidth(64)
        lyr.CreateField(field_defn)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField(0, 123456)
        lyr.CreateFeature(feat)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTReal
        )
        feat = lyr.GetNextFeature()
        self.assertEqual(feat.GetField(0), 123456)

  @unittest.skipUnless(sys.platform == 'win32', 'Incorrect platform')
  def test_ogr_shape_70(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_70') as tmpdir:
      shp_path = os.path.join(tmpdir, 'ogr_shape_70.shp')
      dbf_path = os.path.join(tmpdir, 'ogr_shape_70.dbf')
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_70')
        lyr.CreateField(ogr.FieldDefn('intfield', ogr.OFTInteger))
        feat = ogr.Feature(lyr.GetLayerDefn())
        lyr.CreateFeature(feat)
        fid = feat.GetFID()
        lyr.DeleteFeature(fid)

        with open(dbf_path, 'r+'):
          gdal.ErrorReset()
          with (
              gdal.quiet_errors(),
              gdal.config_option('OGR_SHAPE_PACK_IN_PLACE', 'NO'),
          ):
            ds.ExecuteSQL('REPACK ogr_shape_70')
          errmsg = gdal.GetLastErrorMsg()

      self.assertNotEqual(errmsg, '')

  @unittest.skipUnless(sys.platform == 'linux', 'Incorrect platform')
  def test_ogr_shape_71(self) -> None:
    if os.getuid() == 0:
      self.skipTest('running as root... skipping')

    with gcore_util.TestTemporaryDirectory(prefix='shape_71') as tmpdir:
      shp_path = os.path.join(tmpdir, 'ogr_shape_71.shp')
      shx_path = os.path.join(tmpdir, 'ogr_shape_71.shx')
      dbf_path = os.path.join(tmpdir, 'ogr_shape_71.dbf')
      shutil.copyfile(ogr_util.GetTestFilePath('shape/poly/poly.shp'), shp_path)
      shutil.copyfile(ogr_util.GetTestFilePath('shape/poly/poly.shx'), shx_path)
      shutil.copyfile(ogr_util.GetTestFilePath('shape/poly/poly.dbf'), dbf_path)

      old_mode = os.stat(dbf_path).st_mode
      os.chmod(dbf_path, stat.S_IREAD)
      try:
        with gdal.quiet_errors():
          self.assertIsNone(ogr.Open(shp_path, update=1))
      finally:
        os.chmod(dbf_path, old_mode)

  def test_ogr_shape_72(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_72') as tmpdir:
      if not _supports_sparse_files(tmpdir):
        self.skipTest('Filesystem does not support sparse files')

      shp_path = os.path.join(tmpdir, 'ogr_shape_72.shp')
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('2gb', geom_type=ogr.wkbPoint)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
        lyr.CreateFeature(feat)

      with open(shp_path, 'rb+') as f:
        f.seek(24)
        f.write(struct.pack('B' * 4, 0x7F, 0xFF, 0xFF, 0xFE))

      # Test creating a feature over 4 GB file limit -> should fail
      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (3 4)'))
        with gdal.quiet_errors():
          ret = lyr.CreateFeature(feat)
        self.assertNotEqual(ret, 0)

      with open(shp_path, 'rb+') as f:
        f.seek(24)
        f.write(struct.pack('B' * 4, 0x3F, 0xFF, 0xFF, 0xFE))

      # Test creating a feature over 2 GB file limit -> should fail
      with (
          gdal.config_option('SHAPE_2GB_LIMIT', 'TRUE'),
          ogr.Open(shp_path, update=1) as ds,
      ):
        lyr = ds.GetLayer(0)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (5 6)'))
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.CreateFeature(feat)
        self.assertNotEqual(ret, 0)

      # Test creating a feature over 2 GB file limit -> should succeed with
      # warning.
      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (7 8)'))
        gdal.ErrorReset()
        with gdal.quiet_errors():
          ret = lyr.CreateFeature(feat)
        self.assertEqual(ret, 0)
        self.assertIn(
            '2GB file size limit reached',
            gdal.GetLastErrorMsg(),
            'did not find expected warning',
        )

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetFeature(1)
        self.assertEqual(feat.GetGeometryRef().ExportToWkt(), 'POINT (7 8)')

  def test_ogr_shape_73(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_73') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_73.shp'
      geom = ogr.CreateGeometryFromWkt(
          'POLYGON ((0 0,0 10,10 10,10 0,0 0),(5 1,4 3,4 2,5 1,6 2,6 3,5 1))'
      )
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_73', geom_type=ogr.wkbPolygon)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(geom)
        lyr.CreateFeature(feat)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        got_geom = feat.GetGeometryRef()
        self.assertEqual(geom.ExportToWkt(), got_geom.ExportToWkt())

  def test_ogr_shape_74(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_74') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_74.shp'
      geom = ogr.CreateGeometryFromWkt(
          'MULTIPOLYGON (((0 10,10 10,10 0,0 0,0 1,9 1,9 9,0 9,0 10)),'
          '((9 5,5 4,0 5,5 6, 9 5)))'
      )
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_74', geom_type=ogr.wkbPolygon)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(geom)
        lyr.CreateFeature(feat)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        feat = lyr.GetNextFeature()
        got_geom = feat.GetGeometryRef()
        self.assertEqual(geom.ExportToWkt(), got_geom.ExportToWkt())

        lyr.ResetReading()
        with gdal.config_option('OGR_ORGANIZE_POLYGONS', 'DEFAULT'):
          feat = lyr.GetNextFeature()
        got_geom = feat.GetGeometryRef()
        self.assertEqual(geom.ExportToWkt(), got_geom.ExportToWkt())

  def test_ogr_shape_75(self) -> None:
    poly_shp = ogr_util.GetTestFilePath('shape/poly/poly.shp')
    poly_shx = ogr_util.GetTestFilePath('shape/poly/poly.shx')
    poly_dbf = ogr_util.GetTestFilePath('shape/poly/poly.dbf')
    poly_prj_upper = ogr_util.GetTestFilePath('shape/poly/poly.PRJ')
    poly_prj_lower = ogr_util.GetTestFilePath('shape/poly/poly.prj')

    with gdal.OpenEx(poly_shp) as ds:
      filelist_prj_upper = [poly_shp, poly_shx, poly_dbf, poly_prj_upper]
      filelist_prj_lower = [poly_shp, poly_shx, poly_dbf, poly_prj_lower]
      poly_dir = os.path.dirname(poly_shp)
      if (
          sys.platform in ('win32', 'darwin')
          and 'poly.PRJ' in os.listdir(poly_dir)
          and os.path.exists(poly_prj_lower)
      ):
        self.assertEqual(ds.GetFileList(), filelist_prj_upper)
      else:
        self.assertIn(
            ds.GetFileList(), (filelist_prj_upper, filelist_prj_lower)
        )

    idlink_dbf = ogr_util.GetTestFilePath('shape/idlink.dbf')
    with gdal.OpenEx(idlink_dbf) as ds:
      self.assertEqual(ds.GetFileList(), [idlink_dbf])

    simplepoly_shp = ogr_util.GetTestFilePath('shape/simplepoly/poly.shp')
    simplepoly_shx = ogr_util.GetTestFilePath('shape/simplepoly/poly.shx')
    simplepoly_dbf = ogr_util.GetTestFilePath('shape/simplepoly/poly.dbf')
    simplepoly_qix = ogr_util.GetTestFilePath('shape/simplepoly/poly.qix')
    with gdal.OpenEx(simplepoly_shp) as ds:
      self.assertEqual(
          ds.GetFileList(),
          [simplepoly_shp, simplepoly_shx, simplepoly_dbf, simplepoly_qix],
      )

    sbn_shp = ogr_util.GetTestFilePath('shape/emptyshapefilewithsbn.shp')
    sbn_shx = ogr_util.GetTestFilePath('shape/emptyshapefilewithsbn.shx')
    sbn_sbn = ogr_util.GetTestFilePath('shape/emptyshapefilewithsbn.sbn')
    sbn_sbx = ogr_util.GetTestFilePath('shape/emptyshapefilewithsbn.sbx')
    with gdal.OpenEx(sbn_shx) as ds:
      self.assertEqual(ds.GetFileList(), [sbn_shp, sbn_shx, sbn_sbn, sbn_sbx])

    with gdal.OpenEx(simplepoly_shp) as ds:
      self.assertEqual(
          ds.GetFileList(),
          [simplepoly_shp, simplepoly_shx, simplepoly_dbf, simplepoly_qix],
      )

    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_75') as vsimem_dir:
      copy_shp = f'{vsimem_dir}/test_copy.shp'
      driver = gdal.GetDriverByName(DRIVER)

      # Test that CreateCopy() + GetFileList() list the .prj file when it exists
      with (
          gdal.OpenEx(ogr_util.GetTestFilePath('shape/Stacks.shp')) as src_ds,
          driver.CreateCopy(copy_shp, src_ds) as copy_ds,
      ):
        self.assertEqual(
            copy_ds.GetFileList(),
            [
                f'{vsimem_dir}/test_copy.shp',
                f'{vsimem_dir}/test_copy.shx',
                f'{vsimem_dir}/test_copy.dbf',
                f'{vsimem_dir}/test_copy.prj',
            ],
        )
      driver.Delete(copy_shp)

      # Test that CreateCopy() + GetFileList() don't list .prj when absent
      with (
          gdal.OpenEx(simplepoly_shp) as src_ds,
          driver.CreateCopy(copy_shp, src_ds) as copy_ds,
      ):
        self.assertEqual(
            copy_ds.GetFileList(),
            [
                f'{vsimem_dir}/test_copy.shp',
                f'{vsimem_dir}/test_copy.shx',
                f'{vsimem_dir}/test_copy.dbf',
            ],
        )

  def test_ogr_shape_76(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/prjwithutf8bom.shp')) as ds:
      lyr = ds.GetLayer(0)
      sr = lyr.GetSpatialRef()
      self.assertTrue(sr.ExportToWkt().startswith('GEOGCS["NAD83"'))

  def test_ogr_shape_77(self) -> None:
    with ogr.Open(
        ogr_util.GetTestFilePath('shape/nonconformant_shx_ticket5608.shp')
    ) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      geom = feat.GetGeometryRef()
      self.assertEqual(geom.ExportToWkt(), 'LINESTRING (0 1,2 3)')

  def test_ogr_shape_78(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_78') as vsimem_dir:
      dbf_path = f'{vsimem_dir}/ogr_shape_78.dbf'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(dbf_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_78')

        fd = ogr.FieldDefn('dblfield', ogr.OFTReal)
        fd.SetWidth(20)
        lyr.CreateField(fd)

        fd = ogr.FieldDefn('dblfield2', ogr.OFTReal)
        fd.SetWidth(20)
        fd.SetPrecision(1)
        lyr.CreateField(fd)

        fd = ogr.FieldDefn('dblfield3', ogr.OFTReal)
        lyr.CreateField(fd)

        # Integer values up to 2^53 can be exactly converted into a double.
        gdal.ErrorReset()
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetField('dblfield', (2**53) * 1.0)
        lyr.CreateFeature(f)
        self.assertEqual(
            gdal.GetLastErrorMsg(), '', 'got unexpected error/warning'
        )

        # Field width too small
        gdal.ErrorReset()
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetField('dblfield2', 1e21)
        with gdal.quiet_errors():
          lyr.CreateFeature(f)
        self.assertNotEqual(
            gdal.GetLastErrorMsg(), '', 'did not get expected error/warning'
        )

        # Likely precision loss
        gdal.ErrorReset()
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetField('dblfield', (2**53) * 1.0 + 2)
        with gdal.quiet_errors():
          lyr.CreateFeature(f)
        self.assertNotEqual(
            gdal.GetLastErrorMsg(), '', 'did not get expected error/warning'
        )

        # Exact precision
        gdal.ErrorReset()
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetField('dblfield3', 1623819823.809)
        lyr.CreateFeature(f)
        self.assertEqual(gdal.GetLastErrorMsg(), '')

        gdal.ErrorReset()

      with ogr.Open(dbf_path) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetField('dblfield'), 9007199254740992.0)
        f = lyr.GetNextFeature()
        self.assertTrue(f.IsFieldNull('dblfield'))
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetField('dblfield'), 9007199254740994.0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetField('dblfield3'), 1623819823.809)

  def test_ogr_shape_79(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_79') as vsimem_dir:
      dbf_path = f'{vsimem_dir}/ogr_shape_79.dbf'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(dbf_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_79')
        # This will create a (for now) invisible 'FID' field
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

        # This will delete the implicit field
        fd = ogr.FieldDefn('field1', ogr.OFTReal)
        lyr.CreateField(fd)
        fd = ogr.FieldDefn('field2', ogr.OFTReal)
        lyr.CreateField(fd)

        # If the implicit field isn't deleted, this will cause crash
        lyr.ReorderField(0, 1)
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

      with ogr.Open(dbf_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)

  def test_ogr_shape_80(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/extentnan.shp')) as ds:
      lyr = ds.GetLayer(0)
      extent = lyr.GetExtent()
      self.assertTrue(extent is None or extent[0] == extent[0])

  def test_ogr_shape_81(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_81') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_81.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_81')
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
        lyr.CreateFeature(f)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,-1 -1)'))
        lyr.CreateFeature(f)

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)

        # Add junk behind our back
        fh = gdal.VSIFOpenL(shp_path, 'ab')
        gdal.VSIFWriteL(b'foo', 1, 3, fh)
        gdal.VSIFCloseL(fh)

        size_before = gdal.VSIStatL(shp_path).size

        # Should be a no-op
        ds.ExecuteSQL('REPACK ogr_shape_81')
        size_after = gdal.VSIStatL(shp_path).size
        self.assertEqual(size_after, size_before)

        f = lyr.GetNextFeature()
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(2 2,3 3)'))
        lyr.SetFeature(f)

        # Should be a no-op
        ds.ExecuteSQL('REPACK ogr_shape_81')
        size_after = gdal.VSIStatL(shp_path).size
        self.assertEqual(size_after, size_before)

        # Writes a longer geometry. So .shp will be extended
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(2 2,3 3,4 4)'))
        lyr.SetFeature(f)
        size_after = gdal.VSIStatL(shp_path).size
        self.assertNotEqual(size_after, size_before)

        # Should do something
        size_before = size_after
        ds.ExecuteSQL('REPACK ogr_shape_81')
        size_after = gdal.VSIStatL(shp_path).size
        self.assertNotEqual(size_after, size_before)

        # Writes a shorter geometry, so .shp should not change size.
        size_before = size_after
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(3 3,4 4)'))
        lyr.SetFeature(f)
        size_after = gdal.VSIStatL(shp_path).size
        self.assertEqual(size_after, size_before)
        size_before = size_after

        # Should do something
        ds.ExecuteSQL('REPACK ogr_shape_81')
        size_after = gdal.VSIStatL(shp_path).size
        self.assertNotEqual(size_after, size_before)

  def test_ogr_shape_82(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_82') as tmpdir:
      with _shape_ds_context(tmpdir) as shape_ds:
        shape_lyr = shape_ds.CreateLayer(
            'test_utf_cut', geom_type=ogr.wkbPoint, options=['ENCODING=UTF-8']
        )

        field_defn = ogr.FieldDefn('cut_field', ogr.OFTString)
        field_defn.SetWidth(254)
        result = shape_lyr.CreateField(field_defn)
        self.assertEqual(result, 0, 'failed to create new field.')

        # Insert feature with long string in Russian.
        feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
        init_rus = (
            'работает два мастера, установка набоек, замена подошвы, замена '
            'каблуков, растяжка обуви, растяжка голенищ сапог, швейные работы, '
            'ушив голенища сапога, чистка обуви, чистка замшевой обуви, замена '
            'стелек'
        )
        result_rus = (
            'работает два мастера, установка набоек, замена подошвы, замена '
            'каблуков, растяжка обуви, растяжка голенищ сапог, швейные работы, '
            'ушив голен'
        )
        feat.SetField('cut_field', init_rus)
        with gdal.quiet_errors():
          shape_lyr.CreateFeature(feat)

        init_en = (
            'Remont kablukov i ih zamena; zamena naboek; profilaktika i remont '
            'podoshvy; remont i zamena supinatorov; zamena stelek; zamena '
            'obuvnoj furnitury; remont golenishha; rastjazhka obuvi; chistka i '
            'pokraska obuvi. Smolenskaja oblast, p. Monastyrshhina, ulica '
            'Sovetskaja, d. 38.\tRabotaet ponedelnik \u2013 chetverg s 9.00 do '
            '18.00, pjatnica s 10.00 do 17.00, vyhodnoj: subbota'
        )
        result_en = (
            'Remont kablukov i ih zamena; zamena naboek; profilaktika i remont '
            'podoshvy; remont i zamena supinatorov; zamena stelek; zamena '
            'obuvnoj furnitury; remont golenishha; rastjazhka obuvi; chistka '
            'i pokraska obuvi. Smolenskaja oblast, p. Monastyrshhina, ulica'
        )
        feat = ogr.Feature(feature_def=shape_lyr.GetLayerDefn())
        feat.SetField('cut_field', init_en)
        shape_lyr.CreateFeature(feat)

        feat = shape_lyr.GetFeature(0)
        self.assertEqual(
            feat.GetField('cut_field'), result_rus, 'Wrong rus string cut'
        )

        feat = shape_lyr.GetFeature(1)
        self.assertEqual(
            feat.GetField('cut_field'), result_en, 'Wrong en string cut'
        )

  def test_ogr_shape_83(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_83') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_83.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_83', geom_type=ogr.wkbCurvePolygon)
        self.assertEqual(lyr.GetGeomType(), ogr.wkbPolygon)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt('CURVEPOLYGON((0 0,0 1,1 1,1 0,0 0))')
        )
        lyr.CreateFeature(f)

        f = lyr.GetFeature(0)
        self.assertEqual(f.GetGeometryRef().GetGeometryType(), ogr.wkbPolygon)

  def test_ogr_shape_84(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_84') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_84.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_84', options=['SPATIAL_INDEX=YES'])
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt('POLYGON((0 0,0 1,1 1,1 0,0 0))')
        )
        lyr.CreateFeature(f)

      self.assertIsNotNone(gdal.VSIStatL(f'{vsimem_dir}/ogr_shape_84.qix'))

  def test_ogr_shape_85(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_85') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_85.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_85')
        lyr.CreateField(ogr.FieldDefn('int', ogr.OFTInteger))
        lyr.CreateField(ogr.FieldDefn('int64', ogr.OFTInteger64))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetField(0, 123456789)
        f.SetField(1, 123456789012345678)
        lyr.CreateFeature(f)

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTInteger
        )
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(1).GetType(), ogr.OFTInteger64
        )
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetField(0), 123456789)
        self.assertEqual(f.GetField(1), 123456789012345678)
        # Passing from 9 to 10 figures causes "promotion" to Integer64
        f.SetField(0, 2000000000)
        # Passing from 18 to 19 figures causes "promotion" to Real
        f.SetField(1, 9000000000000000000)
        lyr.SetFeature(f)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTInteger64
        )
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(1).GetType(), ogr.OFTReal
        )
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetField(0), 2000000000)
        self.assertEqual(f.GetField(1), 9000000000000000000)

      # Test open option ADJUST_TYPE
      with gdal.OpenEx(
          shp_path, gdal.OF_VECTOR, open_options={'ADJUST_TYPE': 'YES'}
      ) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTInteger
        )
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(1).GetType(), ogr.OFTInteger64
        )
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetField(0), 2000000000)
        self.assertEqual(f.GetField(1), 9000000000000000000)

  def test_ogr_shape_85bis(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_85bis') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_85.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_85')
        lyr.CreateField(ogr.FieldDefn('int', ogr.OFTInteger))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetField(0, 123456789)
        lyr.CreateFeature(f)
        fd = ogr.FieldDefn('foo', ogr.OFTInteger64)
        ret = lyr.AlterFieldDefn(0, fd, ogr.ALTER_TYPE_FLAG)
        self.assertEqual(ret, 0)
        f.SetField(0, 123456789012345678)
        lyr.SetFeature(f)

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTInteger64
        )
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetField(0), 123456789012345678)

  def test_ogr_shape_86(self) -> None:
    with ogr.Open(
        ogr_util.GetTestFilePath('shape/mixed_shape_type_non_conformant.shp')
    ) as ds:
      with ds.ExecuteSQL(
          'select count(distinct ogr_geometry) from '
          'mixed_shape_type_non_conformant'
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        val = f.GetField(0)
    self.assertEqual(val, 6)

  def test_ogr_shape_87(self) -> None:
    with ogr.Open(
        ogr_util.GetTestFilePath('shape/weird_header_length.dbf')
    ) as ds:
      lyr = ds.GetLayer(0)
      f = lyr.GetNextFeature()
      self.assertEqual(f.GetField(0), 1)

  def test_ogr_shape_88(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_88') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_88.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_88')
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
        lyr.CreateFeature(f)

      gdal.Unlink(f'{vsimem_dir}/ogr_shape_88.dbf')

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1,2 2)'))
        lyr.SetFeature(f)
        ds.ExecuteSQL('REPACK ogr_shape_88')

  def test_ogr_shape_89(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_89') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_89.shp'
      shx_path = f'{vsimem_dir}/ogr_shape_89.shx'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_89')
        f = ogr.Feature(lyr.GetLayerDefn())
        g = ogr.Geometry(ogr.wkbLineString)
        g.AddPoint_2D(0, 0)
        g.AddPoint_2D(1, 1)
        f.SetGeometryDirectly(g)
        lyr.CreateFeature(f)

      gdal.Unlink(f'{vsimem_dir}/ogr_shape_89.dbf')

      # The declared file size doesn't match the real one
      fh = gdal.VSIFOpenL(shx_path, 'rb+')
      gdal.VSIFSeekL(fh, 100 + 4, 0)
      gdal.VSIFWriteL(struct.pack('>i', (10 * 1024 * 1024) // 2), 1, 4, fh)
      gdal.VSIFCloseL(fh)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        with gdal.quiet_errors():
          f = lyr.GetNextFeature()
        self.assertTrue(f is None or f.GetGeometryRef() is None)

      fh = gdal.VSIFOpenL(shp_path, 'rb+')
      gdal.VSIFSeekL(fh, 100 + 8 + 10 * 1024 * 1024 - 1, 0)
      gdal.VSIFWriteL(struct.pack('B', 0), 1, 1, fh)
      gdal.VSIFCloseL(fh)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        g = f.GetGeometryRef()
        self.assertEqual(g.GetPointCount(), 2)

  def test_ogr_shape_90(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_90') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_90.shp'
      shx_path = f'{vsimem_dir}/ogr_shape_90.shx'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_90')
        g = ogr.CreateGeometryFromWkt('POINT(0 0)')
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(g)
        lyr.CreateFeature(f)

      gdal.Unlink(f'{vsimem_dir}/ogr_shape_90.dbf')

      # The declared file size doesn't match the real one
      fh = gdal.VSIFOpenL(shx_path, 'rb+')
      filesize = (100 + 8 * 1024 * 1024) // 2
      gdal.VSIFSeekL(fh, 24, 0)
      gdal.VSIFWriteL(struct.pack('>i', filesize), 1, 4, fh)
      gdal.VSIFCloseL(fh)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 1)

      # Now it is consistent
      fh = gdal.VSIFOpenL(shx_path, 'rb+')
      gdal.VSIFSeekL(fh, 100 + 8 * 1024 * 1024 - 1, 0)
      gdal.VSIFWriteL(struct.pack('B', 0), 1, 1, fh)
      gdal.VSIFCloseL(fh)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 1024 * 1024)

  def test_ogr_shape_91(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/arcm_without_m.shp')) as ds:
      lyr = ds.GetLayer(0)
      for _ in lyr:
        pass

    with ogr.Open(
        ogr_util.GetTestFilePath('shape/polygonm_without_m.shp')
    ) as ds:
      lyr = ds.GetLayer(0)
      for _ in lyr:
        pass

  def test_ogr_shape_92(self) -> None:
    with ogr.Open(
        ogr_util.GetTestFilePath('shape/multipointz_without_m.shp')
    ) as ds:
      lyr = ds.GetLayer(0)
      f = lyr.GetNextFeature()
      wkt = f.GetGeometryRef().ExportToIsoWkt()
      self.assertEqual(wkt, 'MULTIPOINT Z ((0 1 2),(3 4 5))')

  def test_ogr_shape_93(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/pointz_without_m.shp')) as ds:
      lyr = ds.GetLayer(0)
      f = lyr.GetNextFeature()
      wkt = f.GetGeometryRef().ExportToIsoWkt()
      self.assertEqual(wkt, 'POINT Z (1 2 3)')

  @parameterized.named_parameters(
      ('point', 'POINT', ogr.wkbPoint, 'POINT (1 2)'),
      ('pointm', 'POINTM', ogr.wkbPointM, 'POINT M (1 2 3)'),
      ('pointz', 'POINTZ', ogr.wkbPoint25D, 'POINT Z (1 2 3)'),
      ('pointzm', 'POINTZM', ogr.wkbPointZM, 'POINT ZM (1 2 3 4)'),
      ('multipoint', 'MULTIPOINT', ogr.wkbMultiPoint, 'MULTIPOINT ((1 2))'),
      (
          'multipointm',
          'MULTIPOINTM',
          ogr.wkbMultiPointM,
          'MULTIPOINT M ((1 2 3))',
      ),
      (
          'multipointz',
          'MULTIPOINTZ',
          ogr.wkbMultiPoint25D,
          'MULTIPOINT Z ((1 2 3))',
      ),
      (
          'multipointzm',
          'MULTIPOINTZM',
          ogr.wkbMultiPointZM,
          'MULTIPOINT ZM ((1 2 3 4))',
      ),
      ('arc_linestring', 'ARC', ogr.wkbLineString, 'LINESTRING (1 2,3 4)'),
      (
          'arc_linestringm',
          'ARCM',
          ogr.wkbLineStringM,
          'LINESTRING M (1 2 3,5 6 7)',
      ),
      (
          'arc_linestringz',
          'ARCZ',
          ogr.wkbLineString25D,
          'LINESTRING Z (1 2 3,5 6 7)',
      ),
      (
          'arc_linestringzm',
          'ARCZM',
          ogr.wkbLineStringZM,
          'LINESTRING ZM (1 2 3 4,5 6 7 8)',
      ),
      (
          'arc_multilinestring',
          'ARC',
          ogr.wkbMultiLineString,
          'MULTILINESTRING ((1 2,3 4),(1 2,3 4))',
      ),
      (
          'arc_multilinestringm',
          'ARCM',
          ogr.wkbMultiLineStringM,
          'MULTILINESTRING M ((1 2 3,5 6 7),(1 2 3,5 6 7))',
      ),
      (
          'arc_multilinestringz',
          'ARCZ',
          ogr.wkbMultiLineString25D,
          'MULTILINESTRING Z ((1 2 3,5 6 7),(1 2 3,5 6 7))',
      ),
      (
          'arc_multilinestringzm',
          'ARCZM',
          ogr.wkbMultiLineStringZM,
          'MULTILINESTRING ZM ((1 2 3 4,5 6 7 8),(1 2 3 4,5 6 7 8))',
      ),
      ('polygon', 'POLYGON', ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,1 0))'),
      (
          'polygonm',
          'POLYGONM',
          ogr.wkbPolygonM,
          'POLYGON M ((0 0 2,0 1 2,1 1 2,1 0 2))',
      ),
      (
          'polygonz',
          'POLYGONZ',
          ogr.wkbPolygon25D,
          'POLYGON Z ((0 0 2,0 1 2,1 1 2,1 0 2))',
      ),
      (
          'polygonzm',
          'POLYGONZM',
          ogr.wkbPolygonZM,
          'POLYGON ZM ((0 0 2 3,0 1 2 3,1 1 2 3,1 0 2 3))',
      ),
      (
          'polygon_multipolygon',
          'POLYGON',
          ogr.wkbMultiPolygon,
          'MULTIPOLYGON (((0 0,0 1,1 1,1 0)),((100 0,100 1,101 1,101 0)))',
      ),
      (
          'polygon_multipolygonm',
          'POLYGONM',
          ogr.wkbMultiPolygonM,
          (
              'MULTIPOLYGON M (((0 0 2,0 1 2,1 1 2,1 0 2)),'
              '((100 0 2,100 1 2,101 1 2,101 0 2)))'
          ),
      ),
      (
          'polygon_multipolygonz',
          'POLYGONZ',
          ogr.wkbMultiPolygon25D,
          (
              'MULTIPOLYGON Z (((0 0 2,0 1 2,1 1 2,1 0 2)),'
              '((100 0 2,100 1 2,101 1 2,101 0 2)))'
          ),
      ),
      (
          'polygon_multipolygonzm',
          'POLYGONZM',
          ogr.wkbMultiPolygonZM,
          (
              'MULTIPOLYGON ZM (((0 0 2 3,0 1 2 3,1 1 2 3,1 0 2 3)),'
              '((100 0 2 3,100 1 2 3,101 1 2 3,101 0 2 3)))'
          ),
      ),
  )
  def test_ogr_shape_94(self, shpt: str, geom_type: int, wkt: str) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_94') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_94.shp'
      for i in range(2):
        with shape_drv.CreateDataSource(shp_path) as ds:
          if i == 0:
            lyr = ds.CreateLayer('ogr_shape_94', options=[f'SHPT={shpt}'])
          else:
            lyr = ds.CreateLayer('ogr_shape_94', geom_type=geom_type)
          test_lyr_geom_type = (
              ogr.GT_Flatten(geom_type) != ogr.wkbMultiLineString
              and ogr.GT_Flatten(geom_type) != ogr.wkbMultiPolygon
          )
          self.assertTrue(
              not test_lyr_geom_type or lyr.GetGeomType() == geom_type,
              f'i={i} shpt={shpt} geom_type={geom_type} wkt={wkt} '
              f'got={lyr.GetGeomType()}',
          )
          f = ogr.Feature(lyr.GetLayerDefn())
          f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
          lyr.CreateFeature(f)

        with ogr.Open(shp_path) as ds:
          lyr = ds.GetLayer(0)
          self.assertTrue(
              not test_lyr_geom_type or lyr.GetGeomType() == geom_type,
              f'shpt={shpt} geom_type={geom_type} wkt={wkt} '
              f'got={lyr.GetGeomType()}',
          )
          f = lyr.GetNextFeature()
          self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), wkt)

        shape_drv.DeleteDataSource(shp_path)

  def test_ogr_shape_95(self) -> None:
    nodata_m_path = ogr_util.GetTestFilePath(
        'shape/pointzm_with_all_nodata_m.shp'
    )
    with gdal.OpenEx(nodata_m_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint25D)
      f = lyr.GetNextFeature()
      self.assertEqual(
          f.GetGeometryRef().ExportToIsoWkt(),
          'POINT Z (1 2 3)',
          str(lyr.GetGeomType()),
      )

    with gdal.OpenEx(nodata_m_path, open_options=['ADJUST_GEOM_TYPE=NO']) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPointZM)
      f = lyr.GetNextFeature()
      self._check_feature_geometry(
          f,
          ogr.CreateGeometryFromWkt('POINT ZM (1 2 3 -1.79769313486232e+308)'),
      )

    one_valid_m_path = ogr_util.GetTestFilePath(
        'shape/pointzm_with_one_valid_m.shp'
    )
    with gdal.OpenEx(
        one_valid_m_path, open_options=['ADJUST_GEOM_TYPE=FIRST_SHAPE']
    ) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint25D)

    with gdal.OpenEx(
        one_valid_m_path, open_options=['ADJUST_GEOM_TYPE=ALL_SHAPES']
    ) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPointZM)

  def test_ogr_shape_96(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_96') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_96.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_96')
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT M (1 2 3)'))
        lyr.CreateFeature(f)

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), 'POINT M (1 2 3)')
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT M (1 2 4)'))
        lyr.SetFeature(f)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), 'POINT M (1 2 4)')

  def test_ogr_shape_97(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_97') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_97.shp'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_97')
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT ZM (1 2 3 4)'))
        lyr.CreateFeature(f)

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(
            f.GetGeometryRef().ExportToIsoWkt(), 'POINT ZM (1 2 3 4)'
        )
        f.SetGeometry(ogr.CreateGeometryFromWkt('POINT ZM (1 2 5 6)'))
        lyr.SetFeature(f)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(
            f.GetGeometryRef().ExportToIsoWkt(), 'POINT ZM (1 2 5 6)'
        )

  def test_ogr_shape_98(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_98') as tmpdir:
      shp_path = os.path.join(tmpdir, 'can_caps.shp')
      shx_path = os.path.join(tmpdir, 'can_caps.shx')
      with gdal.config_option('SHAPE_RESTORE_SHX', 'TRUE'):
        shutil.copyfile(
            ogr_util.GetTestFilePath('shape/can_caps.shp'), shp_path
        )
        with ogr.Open(shp_path, update=1) as shp_ds:
          shp_lyr = shp_ds.GetLayer(0)
          self.assertEqual(
              shp_lyr.GetFeatureCount(), 13, 'Got wrong number of features.'
          )

      with open(ogr_util.GetTestFilePath('shape/can_caps.shx'), 'rb') as f:
        ref_shx = f.read()
      with open(shx_path, 'rb') as f:
        got_shx = f.read()

      self.assertEqual(
          ref_shx, got_shx, 'Rebuilt shx is different from original shx.'
      )

  @parameterized.named_parameters(
      ('point', ogr.wkbPoint, 'POINT (0 1)'),
      ('linestring', ogr.wkbLineString, 'LINESTRING (0 1,2 3)'),
      ('polygon', ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,0 0))'),
      ('multipoint', ogr.wkbMultiPoint, 'MULTIPOINT (0 1,2 3)'),
      (
          'multilinestring',
          ogr.wkbMultiLineString,
          'MULTILINESTRING ((0 1,2 3))',
      ),
      (
          'multipolygon',
          ogr.wkbMultiPolygon,
          'MULTIPOLYGON (((0 0,0 1,1 1,0 0)))',
      ),
      ('point_m', ogr.wkbPointM, 'POINT M (0 1 2)'),
      ('point_z', ogr.wkbPoint25D, 'POINT Z (0 1 2)'),
      ('point_zm', ogr.wkbPointZM, 'POINT ZM (0 1 2 3)'),
      ('linestring_m', ogr.wkbLineStringM, 'LINESTRING M (0 1 10,2 3 10)'),
      ('linestring_z', ogr.wkbLineString25D, 'LINESTRING Z (0 1 10,2 3 10)'),
      (
          'linestring_zm',
          ogr.wkbLineStringZM,
          'LINESTRING ZM (0 1 10 20,2 3 10 20)',
      ),
      (
          'polygon_m',
          ogr.wkbPolygonM,
          'POLYGON M ((0 0 10,0 1 10,1 1 10,0 0 10))',
      ),
      (
          'polygon_z',
          ogr.wkbPolygon25D,
          'POLYGON Z ((0 0 10,0 1 10,1 1 10,0 0 10))',
      ),
      (
          'polygon_zm',
          ogr.wkbPolygonZM,
          'POLYGON ZM ((0 0 10 20,0 1 10 20,1 1 10 20,0 0 10 20))',
      ),
      ('multipoint_m', ogr.wkbMultiPointM, 'MULTIPOINT M (0 1 10,2 3 10)'),
      ('multipoint_z', ogr.wkbMultiPoint25D, 'MULTIPOINT Z (0 1 10,2 3 10)'),
      (
          'multipoint_zm',
          ogr.wkbMultiPointZM,
          'MULTIPOINT ZM (0 1 10 20,2 3 10 20)',
      ),
      ('tin_z', ogr.wkbTINZ, 'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))'),
  )
  def test_ogr_shape_restore_shx(self, geom_type: int, wkt: str) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_restore_shx'
    ) as vsimem_dir:
      filename = f'{vsimem_dir}/test_ogr_shape_restore_shx.shp'
      shx_filename = f'{vsimem_dir}/test_ogr_shape_restore_shx.shx'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('test_ogr_shape_restore_shx', geom_type=geom_type)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometryDirectly(ogr.CreateGeometryFromWkt(wkt))
        lyr.CreateFeature(f)

      fh = gdal.VSIFOpenL(shx_filename, 'rb')
      expected_data = gdal.VSIFReadL(1, 1000, fh)
      gdal.VSIFCloseL(fh)

      gdal.Unlink(shx_filename)

      with gdal.config_option('SHAPE_RESTORE_SHX', 'TRUE'):
        with ogr.Open(filename, update=1):
          pass

      fh = gdal.VSIFOpenL(shx_filename, 'rb')
      got_data = gdal.VSIFReadL(1, 1000, fh)
      gdal.VSIFCloseL(fh)

      self.assertEqual(got_data, expected_data)

  def test_ogr_shape_wgs84_with_zero_towgs84(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_wgs84_towgs84'
    ) as vsimem_dir:
      shp_path = f'{vsimem_dir}/test_ogr_shape_wgs84_with_zero_TOWGS84.shp'
      prj_path = f'{vsimem_dir}/test_ogr_shape_wgs84_with_zero_TOWGS84.prj'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        ds.CreateLayer('test_ogr_shape_wgs84_with_zero_TOWGS84')

      gdal.FileFromMemBuffer(
          prj_path,
          'GEOGCS["WGS84 Coordinate System",DATUM["WGS 1984",'
          'SPHEROID["WGS 1984",6378137,298.257223563],'
          'TOWGS84[0,0,0,0,0,0,0],AUTHORITY["EPSG","6326"]],'
          'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],'
          'AUTHORITY["EPSG","4326"],'
          'AUTHORITY["SBMG","LAT-LONG,LAT-LONG,WGS84,METERS"]]',
      )
      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        got_wkt = lyr.GetSpatialRef().ExportToPrettyWkt()

      self.assertTrue(got_wkt.startswith('GEOGCS['))
      self.assertIn('4326', got_wkt)

  def test_ogr_shape_etrs89_with_zero_towgs84(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_etrs89_towgs84'
    ) as vsimem_dir:
      shp_path = f'{vsimem_dir}/test_ogr_shape_etrs89_with_zero_TOWGS84.shp'
      prj_path = f'{vsimem_dir}/test_ogr_shape_etrs89_with_zero_TOWGS84.prj'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        ds.CreateLayer('test_ogr_shape_etrs89_with_zero_TOWGS84')

      gdal.FileFromMemBuffer(
          prj_path,
          'PROJCS["ETRS89 / Portugal TM06", GEOGCS["ETRS89", '
          'DATUM["European Terrestrial Reference System 1989", '
          'SPHEROID["GRS 1980", 6378137.0, 298.257222101, '
          'AUTHORITY["EPSG","7019"]], '
          'TOWGS84[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0], '
          'AUTHORITY["EPSG","6258"]], '
          'PRIMEM["Greenwich", 0.0, AUTHORITY["EPSG","8901"]], '
          'UNIT["degree", 0.017453292519943295], '
          'AXIS["Geodetic longitude", EAST], '
          'AXIS["Geodetic latitude", NORTH], AUTHORITY["EPSG","4258"]], '
          'PROJECTION["Transverse_Mercator", AUTHORITY["EPSG","9807"]], '
          'PARAMETER["central_meridian", -8.133108333333334], '
          'PARAMETER["latitude_of_origin", 39.66825833333334], '
          'PARAMETER["scale_factor", 1.0], PARAMETER["false_easting", 0.0], '
          'PARAMETER["false_northing", 0.0], UNIT["m", 1.0], '
          'AXIS["Easting", EAST], AXIS["Northing", NORTH], '
          'AUTHORITY["EPSG","3763"]]',
      )
      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        srs = lyr.GetSpatialRef()
        self.assertEqual(srs.GetAuthorityCode(None), '3763')
        # In older EPSG registries, EPSG:3763 used base geographic CRS
        # EPSG:4258 (ETRS89), so OGRSpatialReference::AutoIdentifyEPSG()
        # stripped the zero TOWGS84 and produced a plain PROJCRS without
        # BOUNDCRS. In newer EPSG registries (including google3's PROJ),
        # EPSG:3763 uses base geographic CRS EPSG:11108 (ETRS89-PRT [1995]),
        # which is not matched by the EPSG:4258 check in GDAL's
        # ogrspatialreference.cpp, so WKT2 retains the BOUNDCRS.
        if srs.GetAuthorityCode('GEOGCS') == '4258':
          self.assertNotIn('BOUNDCRS', srs.ExportToWkt(['FORMAT=WKT2']))
        else:
          self.assertEqual(srs.GetAuthorityCode('GEOGCS'), '11108')
          self.assertIn('BOUNDCRS', srs.ExportToWkt(['FORMAT=WKT2']))

  @parameterized.named_parameters(
      ('in_place_yes', 'YES'),
      ('in_place_no', 'NO'),
  )
  def test_ogr_shape_100(self, variant: str) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_100') as tmpdir:
      shp_path = os.path.join(tmpdir, 'ogr_shape_100.shp')
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_100')
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
        f.SetField('foo', '1')
        lyr.CreateFeature(f)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(1 1,2 2,3 3)'))
        f.SetField('foo', '2')
        lyr.CreateFeature(f)
        lyr.DeleteFeature(0)

        with gdal.config_option('OGR_SHAPE_PACK_IN_PLACE', variant):
          f_dbf = None
          f_shp = None
          f_shx = None
          if sys.platform == 'win32' and variant == 'YES':
            f_dbf = open(os.path.join(tmpdir, 'ogr_shape_100.dbf'), 'rb')
            f_shp = open(os.path.join(tmpdir, 'ogr_shape_100.shp'), 'rb')
            f_shx = open(os.path.join(tmpdir, 'ogr_shape_100.shx'), 'rb')

          try:
            ds.ExecuteSQL('REPACK ogr_shape_100')
          finally:
            if f_dbf is not None:
              f_dbf.close()
            if f_shp is not None:
              f_shp.close()
            if f_shx is not None:
              f_shx.close()

        self.assertEqual(gdal.GetLastErrorMsg(), '', variant)

        for ext in ('dbf', 'shp', 'shx', 'cpg'):
          self.assertIsNone(
              gdal.VSIStatL(
                  os.path.join(tmpdir, f'ogr_shape_100_packed.{ext}')
              ),
              variant,
          )

        f = lyr.GetFeature(0)
        self.assertEqual(f['foo'], '2')
        self.assertEqual(
            f.GetGeometryRef().ExportToWkt(), 'LINESTRING (1 1,2 2,3 3)'
        )

        with gdal.quiet_errors():
          f = lyr.GetFeature(1)
        self.assertIsNone(f, variant)
        lyr.ResetReading()
        self.assertEqual(lyr.GetFeatureCount(), 1, variant)
        f = lyr.GetNextFeature()
        self.assertEqual(f['foo'], '2')
        self.assertEqual(
            f.GetGeometryRef().ExportToWkt(), 'LINESTRING (1 1,2 2,3 3)'
        )
        f = lyr.GetNextFeature()
        self.assertIsNone(f, variant)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (3 3,4 4,5 5,6 6)'))
        f.SetField('foo', '3')
        lyr.CreateFeature(f)

      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 2, variant)
        f = lyr.GetNextFeature()
        self.assertEqual(f['foo'], '2')
        self.assertEqual(
            f.GetGeometryRef().ExportToWkt(), 'LINESTRING (1 1,2 2,3 3)'
        )

        f = lyr.GetNextFeature()
        self.assertEqual(f['foo'], '3')
        self.assertEqual(
            f.GetGeometryRef().ExportToWkt(), 'LINESTRING (3 3,4 4,5 5,6 6)'
        )

        f = lyr.GetNextFeature()
        self.assertIsNone(f, variant)

  def test_ogr_shape_101(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_101') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_101.shp'
      for i in range(2):
        # Auto-repack on create
        ds = shape_drv.CreateDataSource(shp_path)
        lyr = ds.CreateLayer('ogr_shape_101')
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
        f.SetField('foo', '1')
        lyr.CreateFeature(f)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(1 1,2 2,3 3)'))
        f.SetField('foo', '2')
        lyr.CreateFeature(f)
        f = None
        lyr.DeleteFeature(0)

        if i == 0:
          ds = None
        else:
          ds.SyncToDisk()
          self.assertEqual(lyr.GetFeatureCount(), 1, str(i))
          # No-op
          ds.ExecuteSQL('REPACK ogr_shape_101')

        with ogr.Open(shp_path) as ds_read:
          lyr = ds_read.GetLayer(0)
          self.assertEqual(lyr.GetFeatureCount(), 1, str(i))
          f = lyr.GetNextFeature()
          self.assertEqual(f.GetFID(), 0)
          self.assertEqual(f['foo'], '2')
          self.assertEqual(
              f.GetGeometryRef().ExportToWkt(), 'LINESTRING (1 1,2 2,3 3)'
          )
          f = lyr.GetNextFeature()
          self.assertIsNone(f, str(i))

        ds = None

        if i == 0:
          # Auto-repack on update
          with ogr.Open(shp_path, update=1) as ds:
            lyr = ds.GetLayer(0)
            f = ogr.Feature(lyr.GetLayerDefn())
            f.SetGeometry(
                ogr.CreateGeometryFromWkt('LINESTRING (3 3,4 4,5 5,6 6)')
            )
            f.SetField('foo', '3')
            lyr.CreateFeature(f)
            lyr.DeleteFeature(0)

          with ogr.Open(shp_path) as ds:
            lyr = ds.GetLayer(0)
            self.assertEqual(lyr.GetFeatureCount(), 1, str(i))
            f = lyr.GetNextFeature()
            self.assertEqual(f.GetFID(), 0)
            self.assertEqual(f['foo'], '3')
            self.assertEqual(
                f.GetGeometryRef().ExportToWkt(),
                'LINESTRING (3 3,4 4,5 5,6 6)',
            )
            f = lyr.GetNextFeature()
            self.assertIsNone(f, str(i))

          # Test disabling auto-repack on update
          with gdal.OpenEx(
              shp_path, gdal.OF_UPDATE, open_options=['AUTO_REPACK=NO']
          ) as ds:
            lyr = ds.GetLayer(0)
            f = ogr.Feature(lyr.GetLayerDefn())
            f.SetField('foo', '4')
            lyr.CreateFeature(f)
            lyr.DeleteFeature(0)

          with ogr.Open(shp_path) as ds:
            lyr = ds.GetLayer(0)
            self.assertEqual(lyr.GetFeatureCount(), 2, str(i))
            f = lyr.GetNextFeature()
            self.assertEqual(f.GetFID(), 1)
            self.assertEqual(f['foo'], '4')

        shape_drv.DeleteDataSource(shp_path)

  def test_ogr_shape_102(self) -> None:
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_102') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_102.shp'
      prj_path = f'{vsimem_dir}/ogr_shape_102.prj'
      with ogr.GetDriverByName(DRIVER).CreateDataSource(shp_path) as ds:
        ds.CreateLayer('ogr_shape_102', geom_type=ogr.wkbPoint)
      gdal.FileFromMemBuffer(prj_path, 'invalid')
      with ogr.Open(shp_path) as ds:
        lyr = ds.GetLayer(0)
        lyr.GetSpatialRef()

  @parameterized.named_parameters(
      ('eof_yes', ['DBF_EOF_CHAR=YES'], True),
      ('eof_default', [], True),
      ('eof_no', ['DBF_EOF_CHAR=NO'], False),
  )
  def test_ogr_shape_103(self, options: Sequence[str], expected: bool) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    opts = [*options, 'DBF_DATE_LAST_UPDATE=1970-01-01']
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_103') as vsimem_dir:
      filename = f'{vsimem_dir}/ogr_shape_103.dbf'

      # Create empty file
      with shape_drv.CreateDataSource(filename) as ds:
        ds.CreateLayer('ogr_shape_103', geom_type=ogr.wkbNone, options=opts)
      self.assertTrue(_check_eof(filename, expected=expected), str(opts))

      # Add field
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
      self.assertTrue(_check_eof(filename, expected=expected))

      # Add record
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
      self.assertTrue(_check_eof(filename, expected=expected))

      # Add another field
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        lyr.CreateField(ogr.FieldDefn('foo2', ogr.OFTString))
      self.assertTrue(_check_eof(filename, expected=expected))

      # Grow a field
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        fd = lyr.GetLayerDefn().GetFieldDefn(0)
        new_fd = ogr.FieldDefn(fd.GetName(), fd.GetType())
        new_fd.SetWidth(fd.GetWidth() + 1)
        lyr.AlterFieldDefn(0, fd, ogr.ALTER_ALL_FLAG)
      self.assertTrue(_check_eof(filename, expected=expected))

      # Reorder fields
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        lyr.ReorderFields([1, 0])
      self.assertTrue(_check_eof(filename, expected=expected))

      # Shrink a field
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        fd = lyr.GetLayerDefn().GetFieldDefn(0)
        new_fd = ogr.FieldDefn(fd.GetName(), fd.GetType())
        new_fd.SetWidth(fd.GetWidth() + 1)
        lyr.AlterFieldDefn(0, fd, ogr.ALTER_ALL_FLAG)
      self.assertTrue(_check_eof(filename, expected=expected))

      # Remove a field
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        lyr.DeleteField(0)
      self.assertTrue(_check_eof(filename, expected=expected))

      shape_drv.DeleteDataSource(filename)

      # Create file with one field but no record
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(
            'ogr_shape_103', geom_type=ogr.wkbNone, options=opts
        )
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
      self.assertTrue(_check_eof(filename, expected=expected))
      shape_drv.DeleteDataSource(filename)

      # Create file with two records
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(
            'ogr_shape_103', geom_type=ogr.wkbNone, options=opts
        )
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
      self.assertTrue(_check_eof(filename, expected=expected))

      # Test editing a record that is not the last one
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        lyr.SetFeature(lyr.GetNextFeature())
      self.assertTrue(_check_eof(filename, expected=expected))

      # Test editing the last record
      with gdal.OpenEx(filename, gdal.OF_UPDATE, open_options=opts) as ds:
        lyr = ds.GetLayer(0)
        lyr.GetNextFeature()
        lyr.SetFeature(lyr.GetNextFeature())
      self.assertTrue(_check_eof(filename, expected=expected))

      shape_drv.DeleteDataSource(filename)

      # Test appending to a file without an EOF marker
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(
            'ogr_shape_103',
            geom_type=ogr.wkbNone,
            options=['DBF_EOF_CHAR=NO', 'DBF_DATE_LAST_UPDATE=1970-01-01'],
        )
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

      with gdal.OpenEx(
          filename,
          gdal.OF_UPDATE,
          open_options=['DBF_DATE_LAST_UPDATE=1970-01-01'],
      ) as ds:
        lyr = ds.GetLayer(0)
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
        ds.FlushCache()
        self.assertTrue(_check_eof(filename))

      shape_drv.DeleteDataSource(filename)

      # Test editing a record (not the last one) in a file without EOF marker
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(
            'ogr_shape_103',
            geom_type=ogr.wkbNone,
            options=['DBF_EOF_CHAR=NO', 'DBF_DATE_LAST_UPDATE=1970-01-01'],
        )
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

      with gdal.OpenEx(
          filename,
          gdal.OF_UPDATE,
          open_options=['DBF_DATE_LAST_UPDATE=1970-01-01'],
      ) as ds:
        lyr = ds.GetLayer(0)
        lyr.SetFeature(lyr.GetNextFeature())

      self.assertTrue(_check_eof(filename, expected=False))

  @parameterized.named_parameters(
      (
          'tin_single',
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
          ogr.wkbUnknown,
          [],
          None,
      ),
      (
          'tin_fan',
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)),((0 0 0,1 1 3,2 2 4,0 0 0)))',
          ogr.wkbUnknown,
          [],
          None,
      ),
      (
          'tin_strip',
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)),((0 1 2,1 1 3,4 4 5,0 1 2)))',
          ogr.wkbUnknown,
          [],
          None,
      ),
      (
          'tin_no_fan_no_strip',
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)),((1 1 3,0 1 2,4 4 5,1 1 3)))',
          ogr.wkbUnknown,
          [],
          None,
      ),
      (
          'tin_dup_triangle',
          (
              'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)),((0 0 0,0 1 2,1 1 3,0 0 0)),'
              '((1 1 3,0 1 2,4 4 5,1 1 3)))'
          ),
          ogr.wkbUnknown,
          [],
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)),((1 1 3,0 1 2,4 4 5,1 1 3)))',
      ),
      (
          'polyhedralsurface',
          'POLYHEDRALSURFACE Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
          ogr.wkbUnknown,
          [],
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
      ),
      (
          'geometrycollection',
          'GEOMETRYCOLLECTION Z (TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0))))',
          ogr.wkbUnknown,
          [],
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
      ),
      (
          'triangle_shpt_multipatch',
          'TRIANGLE Z ((0 0 0,0 1 2,1 1 3,0 0 0))',
          ogr.wkbUnknown,
          ['SHPT=MULTIPATCH'],
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
      ),
      (
          'triangle_wkbtinz',
          'TRIANGLE Z ((0 0 0,0 1 2,1 1 3,0 0 0))',
          ogr.wkbTINZ,
          [],
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
      ),
      (
          'polygon_wkbtinz',
          'POLYGON Z ((0 0 0,0 1 2,1 1 3,0 0 0))',
          ogr.wkbTINZ,
          [],
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
      ),
      (
          'multipolygon_wkbtinz',
          'MULTIPOLYGON Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
          ogr.wkbTINZ,
          [],
          'TIN Z (((0 0 0,0 1 2,1 1 3,0 0 0)))',
      ),
  )
  def test_ogr_shape_104(
      self,
      wkt: str,
      lyr_type: int,
      options: Sequence[str],
      expected_wkt: str | None,
  ) -> None:
    if expected_wkt is None:
      expected_wkt = wkt

    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_104') as vsimem_dir:
      filename = f'{vsimem_dir}/ogr_shape_104.shp'
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(
            'ogr_shape_104', geom_type=lyr_type, options=list(options)
        )
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        lyr.CreateFeature(f)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), expected_wkt)

  def test_ogr_shape_105(self) -> None:
    with ogr.Open(
        ogr_util.GetTestFilePath('shape/padding_after_field_defns.dbf')
    ) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
      f = lyr.GetNextFeature()
      self.assertEqual(f['id'], '1')
      self.assertEqual(f['foo'], '2')

  def test_ogr_shape_106(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_106') as vsimem_dir:
      shp_path = f'{vsimem_dir}/ogr_shape_106.shp'
      with shape_drv.CreateDataSource(shp_path) as ds:
        lyr = ds.CreateLayer('ogr_shape_81')
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(0 0,1 1)'))
        lyr.CreateFeature(f)

      self.assertEqual(gdal.VSIStatL(shp_path).size, 188)

      with ogr.Open(shp_path, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(2 2,3 3,4 4)'))
        lyr.SetFeature(f)

      self.assertEqual(gdal.VSIStatL(shp_path).size, 188 + 2 * 8)

  def test_ogr_shape_107(self) -> None:
    layer_name = 'ogr_shape_107'
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_107') as vsimem_dir:
      filename = f'{vsimem_dir}/{layer_name}.shp'
      copy_filename = f'{vsimem_dir}/{layer_name}_copy.shp'

      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(layer_name)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(2.5 3.5)'))
        lyr.CreateFeature(f)

        f = lyr.GetFeature(0)
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (1 2,3 4)'))
        lyr.SetFeature(f)

        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (5 6)'))
        lyr.CreateFeature(f)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(
            f.GetGeometryRef().ExportToWkt(), 'LINESTRING (1 2,3 4)'
        )
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'LINESTRING (5 6)')

      gdal.VectorTranslate(copy_filename, filename)
      self.assertTrue(_is_same(copy_filename, filename))

      shape_drv.DeleteDataSource(copy_filename)
      shape_drv.DeleteDataSource(filename)

      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(layer_name)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (1 2,1.5 2.5,3 4)'))
        lyr.CreateFeature(f)

        f = lyr.GetFeature(0)
        f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING(1 2,3 4)'))
        lyr.SetFeature(f)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(
            f.GetGeometryRef().ExportToWkt(), 'LINESTRING (1 2,3 4)'
        )

      gdal.VectorTranslate(copy_filename, filename)
      self.assertTrue(_is_same(copy_filename, filename))

  def test_ogr_shape_108(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/poly/poly.shp')) as ds:
      lyr = ds.GetLayer(0)
      with _spatial_filter_rect(
          lyr, 479750.6875, 4764702.0, 479750.6875, 4764702.0
      ):
        expected_fc = lyr.GetFeatureCount()
        with _attribute_filter(lyr, '1=1'):
          self.assertEqual(lyr.GetFeatureCount(), expected_fc)

  def test_ogr_shape_109(self) -> None:
    layer_name = 'ogr_shape_109'
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_109') as vsimem_dir:
      filename = f'{vsimem_dir}/{layer_name}.shp'
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(layer_name)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(ogr.CreateGeometryFromWkt('POLYGON ((1 2))'))
        lyr.CreateFeature(f)

  def test_ogr_shape_110_write_invalid_multipatch(self) -> None:
    layer_name = 'ogr_shape_110'
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_110') as vsimem_dir:
      filename = f'{vsimem_dir}/{layer_name}.shp'
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(layer_name, options=['SHPT=MULTIPATCH'])
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt('GEOMETRYCOLLECTION(POINT(0 0))')
        )
        lyr.CreateFeature(f)

  def test_ogr_shape_111_delete_field_no_record(self) -> None:
    layer_name = 'ogr_shape_111_delete_field_no_record'
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_111') as vsimem_dir:
      filename = f'{vsimem_dir}/{layer_name}.shp'
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(layer_name)
        lyr.CreateField(ogr.FieldDefn('field_1'))
        lyr.CreateField(ogr.FieldDefn('field_2'))

      with ogr.Open(filename, update=1) as ds:
        lyr = ds.GetLayer(0)
        lyr.DeleteField(1)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'field_1'
        )

  def test_ogr_shape_delete_all_fields_with_records(self) -> None:
    layer_name = 'ogr_shape_delete_all_fields_with_records'
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_del_all_fields'
    ) as vsimem_dir:
      filename = f'{vsimem_dir}/{layer_name}.shp'
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(layer_name, geom_type=ogr.wkbPoint)
        lyr.CreateField(ogr.FieldDefn('field_1'))

      with ogr.Open(filename, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (1 2)'))
        self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

      with ogr.Open(filename, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)

      with ogr.Open(filename, update=1) as ds:
        lyr = ds.GetLayer(0)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (3 4)'))
        self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetFID(), 0)
        self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'POINT (1 2)')
        f = lyr.GetNextFeature()
        self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'POINT (3 4)')

  def test_ogr_shape_112_delete_layer(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_112') as dirname:
      with shape_drv.CreateDataSource(dirname) as ds:
        ds.CreateLayer('test')

      with ogr.Open(dirname) as ds:
        with gdal.quiet_errors():
          self.assertNotEqual(ds.DeleteLayer(0), 0)

      with ogr.Open(dirname, update=1) as ds:
        with gdal.quiet_errors():
          self.assertNotEqual(ds.DeleteLayer(-1), 0)
          self.assertNotEqual(ds.DeleteLayer(1), 0)
        gdal.FileFromMemBuffer(f'{dirname}/test.cpg', 'foo')
        self.assertEqual(ds.DeleteLayer(0), 0)
        self.assertIsNone(gdal.VSIStatL(f'{dirname}/test.shp'))
        self.assertIsNone(gdal.VSIStatL(f'{dirname}/test.cpg'))

  def test_ogr_shape_113_restore_shx_empty_shp_shx(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir('/vsimem/ogr_shape_113') as vsimem_dir:
      dirname = f'{vsimem_dir}/test_ogr_shape_113_restore_shx_empty_shp_shx'
      dbfname = f'{dirname}/foo.dbf'
      with shape_drv.CreateDataSource(dbfname) as ds:
        lyr = ds.CreateLayer('test')
        lyr.CreateField(ogr.FieldDefn('foo'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['foo'] = 'bar'
        lyr.CreateFeature(f)

      gdal.FileFromMemBuffer(f'{dirname}/foo.shp', '')
      gdal.FileFromMemBuffer(f'{dirname}/foo.shx', '')

      with (
          gdal.config_option('SHAPE_RESTORE_SHX', 'YES'),
          gdal.quiet_errors(),
          ogr.Open(dbfname) as ds,
      ):
        self.assertIsNotNone(ds)
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertEqual(f['foo'], 'bar')

  def test_ogr_shape_layer_no_geom_but_srs(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_no_geom_srs'
    ) as vsimem_dir:
      filename = f'{vsimem_dir}/test_ogr_shape_layer_no_geom_but_srs'
      with shape_drv.CreateDataSource(filename) as ds:
        sr = osr.SpatialReference()
        ds.CreateLayer('test', sr, ogr.wkbNone)
        sr = osr.SpatialReference()
        sr.ImportFromEPSG(4326)
        ds.CreateLayer('test2', sr, ogr.wkbNone)

  @unittest.skip(
      'b/335317901: OGRShapeDataSource::Recompress comparator in '
      'ogrshapedatasource.cpp:1629 violates strict weak ordering'
  )
  def test_ogr_shape_114_shz(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.TestTemporaryDirectory(prefix='shape_114') as tmpdir:
      shz_name = os.path.join(tmpdir, 'test_ogr_shape_114.shz')
      self.assertIsNotNone(
          gdal.VectorTranslate(
              shz_name, ogr_util.GetTestFilePath('shape/poly/poly.shp')
          )
      )

      # Add an extra unrelated file
      fh = gdal.VSIFOpenL(f'/vsizip/{{{shz_name}}}/README.TXT', 'wb')
      self.assertIsNotNone(fh)
      gdal.VSIFWriteL(b'hello', 1, 5, fh)
      gdal.VSIFCloseL(fh)

      with ogr.Open(shz_name) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetName(), 'test_ogr_shape_114')
        self.assertEqual(lyr.GetFeatureCount(), 10)

      with ogr.Open(shz_name, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 10)
        self.assertEqual(ds.TestCapability(ogr.ODsCCreateLayer), 0)
        with gdal.quiet_errors():
          self.assertIsNone(ds.CreateLayer('foo'))
        self.assertEqual(ds.TestCapability(ogr.ODsCDeleteLayer), 0)
        with gdal.quiet_errors():
          self.assertEqual(ds.DeleteLayer(0), ogr.OGRERR_FAILURE)
        self.assertEqual(lyr.DeleteFeature(1), 0)

      with ogr.Open(shz_name) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 9)

      # No test_ogrsf binary in google3

      # Check that our extra unrelated file is still there
      self.assertIsNotNone(gdal.VSIStatL(f'/vsizip/{{{shz_name}}}/README.TXT'))

      shape_drv.DeleteDataSource(shz_name)
      self.assertIsNone(gdal.VSIStatL(shz_name))

      with gdal.quiet_errors():
        self.assertIsNone(shape_drv.CreateDataSource('/i_do/not_exist/my.shz'))

  @unittest.skip(
      'b/335317901: OGRShapeDataSource::Recompress comparator in '
      'ogrshapedatasource.cpp:1629 violates strict weak ordering'
  )
  def test_ogr_shape_115_shp_zip(self) -> None:
    poly_shp = ogr_util.GetTestFilePath('shape/poly/poly.shp')
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.TestTemporaryDirectory(prefix='shape_115') as tmpdir:
      dirname = os.path.join(tmpdir, 'work')
      os.mkdir(dirname)
      filename = os.path.join(dirname, 'test_ogr_shape_115.shp.zip')
      tmp_uncompressed = 'test_ogr_shape_115.shp.zip_tmp_uncompressed'
      lockfile = 'test_ogr_shape_115.shp.zip.gdal.lock'

      with gdal.config_option('OGR_SHAPE_PACK_IN_PLACE', 'YES'):
        ds = gdal.VectorTranslate(filename, poly_shp)
        self.assertIn(tmp_uncompressed, gdal.ReadDir(dirname))
      self.assertIsNotNone(ds)
      with gdal.config_option('OGR_SHAPE_PACK_IN_PLACE', 'NO'):
        self.assertTrue(
            gdal.VectorTranslate(ds, poly_shp, options='-nln polyY')
        )
      ds = None

      with (
          gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds,
          gdal.config_option('OGR_SHAPE_USE_VSIMEM_FOR_TEMP', 'NO'),
      ):
        self.assertTrue(
            gdal.VectorTranslate(ds, poly_shp, options='-nln polyX')
        )
        self.assertIn(tmp_uncompressed, gdal.ReadDir(dirname))

      with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
        self.assertTrue(
            gdal.VectorTranslate(ds, poly_shp, options='-nln polyZ')
        )
        self.assertNotIn(tmp_uncompressed, gdal.ReadDir(dirname))

      # Add an extra unrelated file
      fh = gdal.VSIFOpenL(f'/vsizip/{{{filename}}}/README.TXT', 'wb')
      self.assertIsNotNone(fh)
      gdal.VSIFWriteL(b'hello', 1, 5, fh)
      gdal.VSIFCloseL(fh)

      with gdal.config_option('OGR_SHAPE_USE_VSIMEM_FOR_TEMP', 'NO'):
        with ogr.Open(filename) as ds:
          self.assertEqual(ds.GetLayerCount(), 4)
          self.assertEqual(
              [ds.GetLayer(i).GetName() for i in range(4)],
              ['poly', 'polyY', 'polyX', 'polyZ'],
          )
          self.assertEqual(
              [ds.GetLayer(i).GetFeatureCount() for i in range(4)],
              [10, 10, 10, 10],
          )
          self.assertNotIn(tmp_uncompressed, gdal.ReadDir(dirname))
          gdal.ErrorReset()
          ds.ExecuteSQL('UNCOMPRESS')
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          self.assertNotIn(tmp_uncompressed, gdal.ReadDir(dirname))

        with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
          lyr = ds.GetLayer(0)
          self.assertNotIn(tmp_uncompressed, gdal.ReadDir(dirname))
          with gdal.config_option('OGR_SHAPE_LOCK_DELAY', '0.01'):
            ds.ExecuteSQL('UNCOMPRESS')
          self.assertIn(tmp_uncompressed, gdal.ReadDir(dirname))
          self.assertIn(lockfile, gdal.ReadDir(dirname))

          lockfile_path = os.path.join(dirname, lockfile)
          with open(lockfile_path, 'rb') as f:
            old_data = f.read()
          deadline = time.monotonic() + 2.0
          new_data = old_data
          while time.monotonic() < deadline:
            time.sleep(0.05)
            with open(lockfile_path, 'rb') as f:
              new_data = f.read()
            if new_data != old_data:
              break
          self.assertNotEqual(new_data, old_data)

          gdal.ErrorReset()
          ds.ExecuteSQL('UNCOMPRESS')
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          self.assertLen(ds.GetFileList(), 1)
          ds.ExecuteSQL('RECOMPRESS')
          self.assertNotIn(tmp_uncompressed, gdal.ReadDir(dirname))
          self.assertNotIn(lockfile, gdal.ReadDir(dirname))
          gdal.ErrorReset()
          ds.ExecuteSQL('RECOMPRESS')
          self.assertEqual(gdal.GetLastErrorMsg(), '')
          self.assertEqual(lyr.DeleteFeature(1), 0)
          self.assertIn(tmp_uncompressed, gdal.ReadDir(dirname))
          self.assertIn(lockfile, gdal.ReadDir(dirname))

          # Check lock file
          with ogr.Open(filename, update=1) as ds2:
            lyr2 = ds2.GetLayer(0)
            with gdal.quiet_errors():
              self.assertNotEqual(lyr2.DeleteFeature(2), 0)

      with ogr.Open(filename) as ds:
        self.assertEqual(
            [ds.GetLayer(i).GetFeatureCount() for i in range(4)],
            [9, 10, 10, 10],
        )

      with ogr.Open(filename, update=1) as ds:
        self.assertEqual(ds.DeleteLayer(1), 0)

      with ogr.Open(filename) as ds:
        self.assertEqual(ds.GetLayerCount(), 3)
        self.assertEqual(
            [ds.GetLayer(i).GetName() for i in range(3)],
            ['poly', 'polyX', 'polyZ'],
        )

      # No test_ogrsf binary in google3

      # Check that our extra unrelated file is still there
      self.assertIsNotNone(gdal.VSIStatL(f'/vsizip/{{{filename}}}/README.TXT'))

      shape_drv.DeleteDataSource(filename)
      self.assertIsNone(gdal.VSIStatL(filename))
      self.assertIn(set(gdal.ReadDir(dirname)), ({'.', '..'}, set()))
      gdal.RmdirRecursive(dirname)

      with gdal.quiet_errors():
        self.assertIsNone(
            shape_drv.CreateDataSource('/i_do/not_exist/my.shp.zip')
        )

  def test_ogr_shape_116_invalid_layer_name(self) -> None:
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.TestTemporaryDirectory(prefix='shape_116') as tmpdir:
      with shape_drv.CreateDataSource(tmpdir) as ds:
        with gdal.quiet_errors():
          self.assertIsNotNone(
              ds.CreateLayer('test<>:"/\\?*', None, ogr.wkbNone)
          )
      with ogr.Open(tmpdir) as ds:
        self.assertEqual(ds.GetLayerCount(), 1)

  def test_ogr_shape_ldid_and_cpg(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_ldid_cpg'
    ) as vsimem_dir:
      dbf_path = f'{vsimem_dir}/tmp.dbf'
      cpg_path = f'{vsimem_dir}/tmp.cpg'
      with open(
          ogr_util.GetTestFilePath('shape/facility_surface_dd.dbf'), 'rb'
      ) as f:
        gdal.FileFromMemBuffer(dbf_path, f.read())
      gdal.FileFromMemBuffer(cpg_path, 'UTF-8')

      with gdal.OpenEx(dbf_path) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetMetadata_Dict('SHAPEFILE'),
            {
                'CPG_VALUE': 'UTF-8',
                'ENCODING_FROM_CPG': 'UTF-8',
                'ENCODING_FROM_LDID': 'ISO-8859-1',
                'LDID_VALUE': '87',
                'SOURCE_ENCODING': 'UTF-8',
            },
        )

      # Disable recoding
      with gdal.OpenEx(dbf_path, open_options=['ENCODING=']) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(
            lyr.GetMetadata_Dict('SHAPEFILE'),
            {
                'CPG_VALUE': 'UTF-8',
                'ENCODING_FROM_CPG': 'UTF-8',
                'ENCODING_FROM_LDID': 'ISO-8859-1',
                'LDID_VALUE': '87',
                'SOURCE_ENCODING': '',
            },
        )

  def test_ogr_shape_point_nan(self) -> None:
    with ogr.Open(ogr_util.GetTestFilePath('shape/pointnan.shp')) as ds:
      lyr = ds.GetLayer(0)
      with _spatial_filter_rect(lyr, 0, 0, 100, 100):
        count = sum(1 for _ in lyr)
      self.assertEqual(count, 1)

  def test_ogr_shape_write_point_z_non_finite(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_point_z_non_finite'
    ) as vsimem_dir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(
          f'{vsimem_dir}/test.shp'
      ) as ds:
        lyr = ds.CreateLayer('test')
        g = ogr.Geometry(ogr.wkbPoint25D)
        g.AddPoint(0, 0, float('inf'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(g)
        with gdal.quiet_errors():
          self.assertNotEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

  def test_ogr_shape_write_linestring_z_non_finite(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_linestring_z_non_finite'
    ) as vsimem_dir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(
          f'{vsimem_dir}/test.shp'
      ) as ds:
        lyr = ds.CreateLayer('test')
        g = ogr.Geometry(ogr.wkbLineString25D)
        g.AddPoint(0, 0, 0)
        g.AddPoint(0, 1, float('inf'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(g)
        with gdal.quiet_errors():
          self.assertNotEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

  def test_ogr_shape_write_multilinestring_z_non_finite(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_multilinestring_z_non_finite'
    ) as vsimem_dir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(
          f'{vsimem_dir}/test.shp'
      ) as ds:
        lyr = ds.CreateLayer('test')
        ls = ogr.Geometry(ogr.wkbLineString25D)
        ls.AddPoint(0, 0, 0)
        ls.AddPoint(0, 1, float('inf'))
        g = ogr.Geometry(ogr.wkbMultiLineString25D)
        g.AddGeometry(ls)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(g)
        with gdal.quiet_errors():
          self.assertNotEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

  def test_ogr_shape_write_polygon_z_non_finite(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_polygon_z_non_finite'
    ) as vsimem_dir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(
          f'{vsimem_dir}/test.shp'
      ) as ds:
        lyr = ds.CreateLayer('test')
        ls = ogr.Geometry(ogr.wkbLinearRing)
        ls.AddPoint(0, 0, 0)
        ls.AddPoint(0, 1, float('inf'))
        ls.AddPoint(1, 1, 0)
        ls.AddPoint(0, 0, 0)
        g = ogr.Geometry(ogr.wkbPolygon25D)
        g.AddGeometry(ls)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(g)
        with gdal.quiet_errors():
          self.assertNotEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

  def test_ogr_shape_write_multipolygon_z_non_finite(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_multipolygon_z_non_finite'
    ) as vsimem_dir:
      with ogr.GetDriverByName(DRIVER).CreateDataSource(
          f'{vsimem_dir}/test.shp'
      ) as ds:
        lyr = ds.CreateLayer('test')
        ls = ogr.Geometry(ogr.wkbLinearRing)
        ls.AddPoint(0, 0, 0)
        ls.AddPoint(0, 1, float('inf'))
        ls.AddPoint(1, 1, 0)
        ls.AddPoint(0, 0, 0)
        p = ogr.Geometry(ogr.wkbPolygon25D)
        p.AddGeometry(ls)
        g = ogr.Geometry(ogr.wkbMultiPolygon25D)
        g.AddGeometry(p)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(g)
        with gdal.quiet_errors():
          self.assertNotEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

  def test_ogr_shape_write_multipolygon_parts_slightly_overlapping(
      self,
  ) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_slightly_overlapping'
    ) as vsimem_dir:
      outfilename = f'{vsimem_dir}/out.shp'
      gdal.VectorTranslate(
          outfilename,
          ogr_util.GetTestFilePath('shape/slightly_overlapping_polygons.shp'),
      )
      with ogr.Open(outfilename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        geom = f.GetGeometryRef()
        self.assertEqual(geom.GetGeometryType(), ogr.wkbMultiPolygon)
        self.assertEqual(geom.GetGeometryCount(), 3)

        # In full analyzer mode, one ring is treated as inner ring of another.
        with gdal.config_option('OGR_ORGANIZE_POLYGONS', 'DEFAULT'):
          lyr.ResetReading()
          f = lyr.GetNextFeature()
          geom = f.GetGeometryRef()
          self.assertEqual(geom.GetGeometryType(), ogr.wkbMultiPolygon)
          self.assertEqual(geom.GetGeometryCount(), 2)

  def test_ogr_shape_write_multipolygon_parts_non_constant_z(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_non_constant_z'
    ) as vsimem_dir:
      outfilename = f'{vsimem_dir}/out.shp'
      gdal.VectorTranslate(
          outfilename,
          ogr_util.GetTestFilePath('shape/multipointz_non_constant_z.shp'),
      )
      with ogr.Open(outfilename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        geom = f.GetGeometryRef()
        self.assertEqual(geom.GetGeometryType(), ogr.wkbMultiPolygon25D)
        self.assertEqual(geom.GetGeometryCount(), 7)

  def test_ogr_shape_rename_layer(self) -> None:
    with (
        _enable_exceptions(),
        gcore_util.TestTemporaryDirectory(prefix='shape_rename') as tmpdir,
    ):
      outfilename = os.path.join(tmpdir, 'test_rename.shp')
      gdal.VectorTranslate(
          outfilename, ogr_util.GetTestFilePath('shape/poly/poly.shp')
      )

      with ogr.Open(outfilename, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.TestCapability(ogr.OLCRename), 1)

        with self.assertRaisesRegex(RuntimeError, 'Illegal character'):
          lyr.Rename('illegal/slash')

        with self.assertRaises(RuntimeError):
          lyr.Rename('test_rename')

        dummy_dbf = os.path.join(tmpdir, 'test_rename_foo.dbf')
        fh = gdal.VSIFOpenL(dummy_dbf, 'wb')
        self.assertIsNotNone(fh)
        gdal.VSIFCloseL(fh)

        with self.assertRaises(RuntimeError):
          lyr.Rename('test_rename_foo')

        gdal.Unlink(dummy_dbf)

        self.assertEqual(sum(1 for _ in lyr), 10)

        self.assertEqual(lyr.Rename('test_rename_foo'), ogr.OGRERR_NONE)
        for ext in ('shp', 'shx', 'dbf', 'prj'):
          self.assertIsNotNone(
              gdal.VSIStatL(os.path.join(tmpdir, f'test_rename_foo.{ext}'))
          )
        self.assertEqual(lyr.GetDescription(), 'test_rename_foo')
        self.assertEqual(lyr.GetLayerDefn().GetName(), 'test_rename_foo')

        self.assertEqual(sum(1 for _ in lyr), 10)

        ds.ExecuteSQL('ALTER TABLE test_rename_foo RENAME TO test_rename_bar')
        for ext in ('shp', 'shx', 'dbf', 'prj'):
          self.assertIsNotNone(
              gdal.VSIStatL(os.path.join(tmpdir, f'test_rename_bar.{ext}'))
          )
        self.assertEqual(lyr.GetDescription(), 'test_rename_bar')
        self.assertEqual(lyr.GetLayerDefn().GetName(), 'test_rename_bar')

        self.assertEqual(sum(1 for _ in lyr), 10)

  @unittest.skip(
      'b/335317901: OGRShapeDataSource::Recompress comparator in '
      'ogrshapedatasource.cpp:1629 violates strict weak ordering'
  )
  def test_ogr_shape_rename_layer_zip(self) -> None:
    with gcore_util.TestTemporaryDirectory(prefix='shape_rename_zip') as tmpdir:
      outfilename = os.path.join(tmpdir, 'test_rename.shp.zip')
      gdal.VectorTranslate(
          outfilename, ogr_util.GetTestFilePath('shape/poly/poly.shp')
      )

      with ogr.Open(outfilename, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.TestCapability(ogr.OLCRename), 1)

        self.assertEqual(lyr.Rename('test_rename_foo'), ogr.OGRERR_NONE)
        self.assertEqual(lyr.GetDescription(), 'test_rename_foo')
        self.assertEqual(lyr.GetLayerDefn().GetName(), 'test_rename_foo')
        self.assertEqual(sum(1 for _ in lyr), 10)

        self.assertEqual(lyr.Rename('test_rename_bar'), ogr.OGRERR_NONE)
        self.assertEqual(lyr.GetDescription(), 'test_rename_bar')
        self.assertEqual(lyr.GetLayerDefn().GetName(), 'test_rename_bar')
        self.assertEqual(sum(1 for _ in lyr), 10)

  def test_ogr_shape_alter_geom_field_defn(self) -> None:
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_alter_geom_field_defn'
    ) as vsimem_dir:
      outfilename = f'{vsimem_dir}/test_ogr_shape_alter_geom_field_defn.shp'
      srs_4326 = osr.SpatialReference()
      srs_4326.ImportFromEPSG(4326)
      with ogr.GetDriverByName(DRIVER).CreateDataSource(outfilename) as ds:
        ds.CreateLayer(
            'test_ogr_shape_alter_geom_field_defn',
            geom_type=ogr.wkbPoint,
            srs=srs_4326,
        )

      with ogr.Open(outfilename, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertTrue(lyr.TestCapability(ogr.OLCAlterGeomFieldDefn))

        new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbPoint)
        other_srs = osr.SpatialReference()
        other_srs.ImportFromEPSG(4269)
        new_geom_field_defn.SetSpatialRef(other_srs)
        self.assertEqual(
            lyr.AlterGeomFieldDefn(
                0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_ALL_FLAG
            ),
            ogr.OGRERR_NONE,
        )
        self.assertTrue(lyr.GetSpatialRef().IsSame(other_srs))

      with ogr.Open(outfilename, update=1) as ds:
        lyr = ds.GetLayer(0)
        srs = lyr.GetSpatialRef()
        self.assertIsNotNone(srs)
        self.assertEqual(srs.GetAuthorityCode(None), '4269')

        new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbPoint)
        self.assertEqual(
            lyr.AlterGeomFieldDefn(
                0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_ALL_FLAG
            ),
            ogr.OGRERR_NONE,
        )
        self.assertIsNone(lyr.GetSpatialRef())

      with ogr.Open(outfilename, update=1) as ds:
        lyr = ds.GetLayer(0)
        self.assertIsNone(lyr.GetSpatialRef())

        new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbPoint)
        new_geom_field_defn.SetSpatialRef(srs_4326)
        self.assertEqual(
            lyr.AlterGeomFieldDefn(
                0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_ALL_FLAG
            ),
            ogr.OGRERR_NONE,
        )

      with ogr.Open(outfilename, update=1) as ds:
        lyr = ds.GetLayer(0)
        srs = lyr.GetSpatialRef()
        self.assertIsNotNone(srs)
        self.assertEqual(srs.GetAuthorityCode(None), '4326')

        # Wrong index
        new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbPoint)
        with gdal.quiet_errors():
          self.assertNotEqual(
              lyr.AlterGeomFieldDefn(
                  -1, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_ALL_FLAG
              ),
              ogr.OGRERR_NONE,
          )

        # Changing geometry type ==> unsupported
        new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbLineString)
        with gdal.quiet_errors():
          self.assertNotEqual(
              lyr.AlterGeomFieldDefn(
                  0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_ALL_FLAG
              ),
              ogr.OGRERR_NONE,
          )

        # Setting coordinate epoch ==> unsupported
        new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbPoint)
        srs_with_epoch = osr.SpatialReference()
        srs_with_epoch.ImportFromEPSG(4326)
        srs_with_epoch.SetCoordinateEpoch(2022)
        new_geom_field_defn.SetSpatialRef(srs_with_epoch)
        with gdal.quiet_errors():
          self.assertNotEqual(
              lyr.AlterGeomFieldDefn(
                  0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_ALL_FLAG
              ),
              ogr.OGRERR_NONE,
          )

  def test_ogr_shape_write_non_planar_polygon(self) -> None:
    layer_name = 'test_ogr_shape_write_non_planar_polygon'
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_non_planar_polygon'
    ) as vsimem_dir:
      filename = f'{vsimem_dir}/{layer_name}.shp'
      with shape_drv.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer(layer_name, geom_type=ogr.wkbPolygon25D)
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometry(
            ogr.CreateGeometryFromWkt(
                'POLYGON Z ((516113.631069 5041435.137874 137.334, '
                '516141.2239 5041542.465874 137.614, '
                '515998.390418 5041476.527121 137.288, '
                '516113.631069 5041435.137874 137.334), '
                '(516041.808551 5041476.527121 137.418, '
                '516111.602184 5041505.337284 137.322, '
                '516098.617322 5041456.644051 137.451, '
                '516041.808551 5041476.527121 137.418))'
            )
        )
        lyr.CreateFeature(f)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self._check_feature_geometry(
            f,
            'POLYGON Z ((516113.631069 5041435.137874 137.334,'
            '515998.390418 5041476.527121 137.288,'
            '516141.2239 5041542.465874 137.614,'
            '516113.631069 5041435.137874 137.334),'
            '(516041.808551 5041476.527121 137.418,'
            '516098.617322 5041456.644051 137.451,'
            '516111.602184 5041505.337284 137.322,'
            '516041.808551 5041476.527121 137.418))',
        )

  def test_ogr_shape_prj_with_wrong_axis_order(self) -> None:
    layer_name = 'test_ogr_shape_prj_with_wrong_axis_order'
    shape_drv = ogr.GetDriverByName(DRIVER)
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_wrong_axis_order'
    ) as vsimem_dir:
      filename = f'{vsimem_dir}/{layer_name}.shp'
      with shape_drv.CreateDataSource(filename) as ds:
        ds.CreateLayer(layer_name, geom_type=ogr.wkbPolygon25D)
      prj = """GEOGCS["WGS 84",
    DATUM["World Geodetic System 1984",
        SPHEROID["WGS 84", 6378137.0, 298.257223563,
            AUTHORITY["EPSG","7030"]],
        AUTHORITY["EPSG","6326"]],
    PRIMEM["Greenwich", 0.0,
        AUTHORITY["EPSG","8901"]],
    UNIT["degree", 0.017453292519943295],
    AXIS["Geodetic longitude", EAST],
    AXIS["Geodetic latitude", NORTH],
    AUTHORITY["EPSG","4326"]]
    """
      gdal.FileFromMemBuffer(f'{vsimem_dir}/{layer_name}.prj', prj)
      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetSpatialRef().GetAxisName(None, 0), 'Latitude')
        self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), '4326')
        self.assertEqual(
            lyr.GetSpatialRef().GetDataAxisToSRSAxisMapping(), [2, 1]
        )

  def test_ogr_shape_write_arrow_fallback_types(self) -> None:
    with (
        _enable_exceptions(),
        gcore_util.VsimemCleanupDir(
            '/vsimem/ogr_shape_write_arrow_fallback'
        ) as vsimem_dir,
    ):
      src_ds = ogr.GetDriverByName('MEM').CreateDataSource('')
      src_lyr = src_ds.CreateLayer('test')
      src_lyr.CreateField(ogr.FieldDefn('string', ogr.OFTString))
      src_lyr.CreateField(ogr.FieldDefn('int', ogr.OFTInteger))
      src_lyr.CreateField(ogr.FieldDefn('int64', ogr.OFTInteger64))
      src_lyr.CreateField(ogr.FieldDefn('real', ogr.OFTReal))
      src_lyr.CreateField(ogr.FieldDefn('date', ogr.OFTDate))
      src_lyr.CreateField(ogr.FieldDefn('time', ogr.OFTTime))
      src_lyr.CreateField(ogr.FieldDefn('datetime', ogr.OFTDateTime))
      src_lyr.CreateField(ogr.FieldDefn('binary', ogr.OFTBinary))
      src_lyr.CreateField(ogr.FieldDefn('stringlist', ogr.OFTStringList))
      src_lyr.CreateField(ogr.FieldDefn('intlist', ogr.OFTIntegerList))
      src_lyr.CreateField(ogr.FieldDefn('int64list', ogr.OFTInteger64List))
      src_lyr.CreateField(ogr.FieldDefn('reallist', ogr.OFTRealList))
      f = ogr.Feature(src_lyr.GetLayerDefn())
      f['string'] = 'foo'
      f['int'] = 123
      f['int64'] = 12345678901234
      f['real'] = 1.5
      f['date'] = '2023/10/06'
      f['time'] = '12:34:56'
      f['datetime'] = '2023/10/06 19:43:00'
      f.SetField('binary', b'\x01\x23\x46\x57\x89\xAB\xCD\xEF')
      f['stringlist'] = ['foo', 'bar']
      f['intlist'] = [1, 2]
      f['int64list'] = [12345678901234, 2]
      f['reallist'] = [1.5, 2.5]
      src_lyr.CreateFeature(f)

      filename = f'{vsimem_dir}/test_ogr_gpkg_write_arrow_fallback_types.shp'
      with gdal.GetDriverByName(DRIVER).Create(
          filename, 0, 0, 0, gdal.GDT_Unknown
      ) as ds:
        lyr = ds.CreateLayer('test')

        stream = src_lyr.GetArrowStream()
        schema = stream.GetSchema()

        success, _ = lyr.IsArrowSchemaSupported(schema)
        self.assertTrue(success)

        for i in range(schema.GetChildrenCount()):
          if schema.GetChild(i).GetName() not in ('wkb_geometry', 'OGC_FID'):
            lyr.CreateFieldFromArrowSchema(schema.GetChild(i))

        while True:
          array = stream.GetNextRecordBatch()
          if array is None:
            break
          lyr.WriteArrowBatch(schema, array, ['FID=OGC_FID'])

        del stream

        f = lyr.GetNextFeature()
        self.assertEqual(f['string'], 'foo')
        self.assertEqual(f['int'], 123)
        self.assertEqual(f['int64'], 12345678901234)
        self.assertEqual(f['real'], 1.5)
        self.assertEqual(f['date'], '2023/10/06')
        self.assertEqual(f['time'], '12:34:56')
        self.assertEqual(f['binary'], '0123465789ABCDEF')
        self.assertEqual(f['datetime'], '2023-10-06T19:43:00')
        self.assertEqual(f['stringlist'], '[ "foo", "bar" ]')
        self.assertEqual(f['intlist'], '[ 1, 2 ]')
        self.assertEqual(f['int64list'], '[ 12345678901234, 2 ]')
        self.assertEqual(f['reallist'], '[ 1.5, 2.5 ]')

  def test_ogr_shape_write_arrow_if_fid_not_preserved_error(self) -> None:
    with (
        _enable_exceptions(),
        gcore_util.VsimemCleanupDir(
            '/vsimem/ogr_shape_write_arrow_fid_err'
        ) as vsimem_dir,
    ):
      src_ds = ogr.GetDriverByName('MEM').CreateDataSource('')
      src_lyr = src_ds.CreateLayer('test')
      f = ogr.Feature(src_lyr.GetLayerDefn())
      f.SetFID(1)
      src_lyr.CreateFeature(f)

      filename = f'{vsimem_dir}/test_ogr_shape_write_arrow_IF_FID_NOT_PRESERVED_ERROR.shp'
      with gdal.GetDriverByName(DRIVER).Create(
          filename, 0, 0, 0, gdal.GDT_Unknown
      ) as ds:
        lyr = ds.CreateLayer('test')

        stream = src_lyr.GetArrowStream()
        schema = stream.GetSchema()

        while True:
          array = stream.GetNextRecordBatch()
          if array is None:
            break
          with self.assertRaisesRegex(
              RuntimeError, 'Feature id 1 not preserved'
          ):
            lyr.WriteArrowBatch(
                schema, array, ['FID=OGC_FID', 'IF_FID_NOT_PRESERVED=ERROR']
            )

        del stream

  def test_ogr_shape_write_date_0000_00_00(self) -> None:
    with (
        _enable_exceptions(),
        gcore_util.VsimemCleanupDir(
            '/vsimem/ogr_shape_write_date_0000'
        ) as vsimem_dir,
    ):
      filename = f'{vsimem_dir}/test_ogr_shape_write_date_0000_00_00.shp'
      with gdal.GetDriverByName(DRIVER).Create(
          filename, 0, 0, 0, gdal.GDT_Unknown
      ) as ds:
        lyr = ds.CreateLayer('test')
        lyr.CreateField(ogr.FieldDefn('date', ogr.OFTDate))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['date'] = '0000/00/00'
        lyr.CreateFeature(f)
        f = None

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        f = lyr.GetNextFeature()
        self.assertTrue(f.IsFieldNull('date'))

  def test_ogr_shape_arrow_stream(self) -> None:
    if not hasattr(gdal_array, '_RecordBatchAsNumpy'):
      self.skipTest('osgeo.gdal_array._RecordBatchAsNumpy not available')
    if importlib.util.find_spec('numpy') is None:
      self.skipTest('numpy not available')

    with ogr.Open(ogr_util.GetTestFilePath('shape/poly/poly.shp')) as ds:
      lyr = ds.GetLayer(0)
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = list(stream)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'NO',
      )
      self.assertLen(batches, 1)
      self.assertLen(batches[0], 5)
      self.assertLen(batches[0]['OGC_FID'], 10)
      self.assertEqual(
          list(batches[0]['OGC_FID']), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
      )
      self.assertEqual(
          list(batches[0]['EAS_ID']),
          [168, 179, 171, 173, 172, 169, 166, 158, 165, 170],
      )

  def test_ogr_shape_arrow_stream_fid_optim(self) -> None:
    if not hasattr(gdal_array, '_RecordBatchAsNumpy'):
      self.skipTest('osgeo.gdal_array._RecordBatchAsNumpy not available')
    if importlib.util.find_spec('numpy') is None:
      self.skipTest('numpy not available')

    with ogr.Open(ogr_util.GetTestFilePath('shape/poly/poly.shp')) as ds:
      lyr = ds.GetLayer(0)
      ignored_fields = ['OGR_GEOMETRY']
      for i in range(lyr.GetLayerDefn().GetFieldCount()):
        ignored_fields.append(lyr.GetLayerDefn().GetFieldDefn(i).GetName())
      lyr.SetIgnoredFields(ignored_fields)

      # Optimized code path
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = list(stream)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'YES',
      )
      self.assertLen(batches, 1)
      self.assertLen(batches[0], 1)
      self.assertLen(batches[0]['OGC_FID'], 10)
      self.assertEqual(
          list(batches[0]['OGC_FID']), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
      )

      # Optimized code path
      stream = lyr.GetArrowStreamAsNumPy(
          options=['USE_MASKED_ARRAYS=NO', 'MAX_FEATURES_IN_BATCH=7']
      )
      batches = list(stream)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'YES',
      )
      self.assertLen(batches, 2)
      self.assertLen(batches[0], 1)
      self.assertLen(batches[0]['OGC_FID'], 7)
      self.assertEqual(list(batches[0]['OGC_FID']), [0, 1, 2, 3, 4, 5, 6])
      self.assertLen(batches[1]['OGC_FID'], 3)
      self.assertEqual(list(batches[1]['OGC_FID']), [7, 8, 9])

      # Regular code path
      lyr.SetAttributeFilter('1 = 1')
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = list(stream)
      lyr.SetAttributeFilter(None)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'NO',
      )
      self.assertLen(batches, 1)
      self.assertLen(batches[0], 1)
      self.assertLen(batches[0]['OGC_FID'], 10)
      self.assertEqual(
          list(batches[0]['OGC_FID']), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
      )

      # Regular code path
      minx, maxx, miny, maxy = lyr.GetExtent()
      lyr.SetSpatialFilterRect(minx, miny, maxx, maxy)
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = list(stream)
      lyr.SetSpatialFilter(None)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'NO',
      )
      self.assertEmpty(batches)

      # Regular code path
      lyr.SetIgnoredFields(ignored_fields[0:-1])
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = list(stream)
      lyr.SetIgnoredFields(ignored_fields)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'NO',
      )
      self.assertLen(batches, 1)
      self.assertLen(batches[0], 2)
      self.assertLen(batches[0]['OGC_FID'], 10)
      self.assertEqual(
          list(batches[0]['OGC_FID']), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
      )

      # Regular code path
      lyr.SetIgnoredFields(ignored_fields[1:])
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = list(stream)
      lyr.SetIgnoredFields(ignored_fields)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'NO',
      )
      self.assertLen(batches, 1)
      self.assertLen(batches[0], 2)
      self.assertLen(batches[0]['OGC_FID'], 10)
      self.assertEqual(
          list(batches[0]['OGC_FID']), [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
      )

      # Regular code path
      stream = lyr.GetArrowStreamAsNumPy(
          options=['USE_MASKED_ARRAYS=NO', 'INCLUDE_FID=NO']
      )
      batches = list(stream)
      self.assertEqual(
          lyr.GetMetadataItem(
              'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH', '__DEBUG__'
          ),
          'NO',
      )
      self.assertEmpty(batches)

    # Test hole in FID numbering
    with gcore_util.VsimemCleanupDir(
        '/vsimem/ogr_shape_arrow_fid_optim'
    ) as vsimem_dir:
      filename = f'{vsimem_dir}/test_ogr_shape_arrow_stream_fid_optim.shp'
      dbf_filename = f'{vsimem_dir}/test_ogr_shape_arrow_stream_fid_optim.dbf'
      with gdal.GetDriverByName(DRIVER).Create(
          filename, 0, 0, 0, gdal.GDT_Unknown
      ) as ds:
        lyr = ds.CreateLayer(
            'test', geom_type=ogr.wkbPoint, options=['AUTO_REPACK=NO']
        )
        lyr.CreateField(ogr.FieldDefn('str', ogr.OFTString))
        for _ in range(5):
          f = ogr.Feature(lyr.GetLayerDefn())
          f['str'] = 'foo'
          lyr.CreateFeature(f)
        lyr.DeleteFeature(3)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        lyr.SetIgnoredFields(['OGR_GEOMETRY', 'str'])

        # Optimized code path
        stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
        batches = list(stream)
        self.assertEqual(
            lyr.GetMetadataItem(
                'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH',
                '__DEBUG__',
            ),
            'YES',
        )
        self.assertLen(batches, 1)
        self.assertLen(batches[0], 1)
        self.assertLen(batches[0]['OGC_FID'], 4)
        self.assertEqual(list(batches[0]['OGC_FID']), [0, 1, 2, 4])

        fh = gdal.VSIFOpenL(dbf_filename, 'rb+')
        self.assertIsNotNone(fh)
        gdal.VSIFTruncateL(fh, 300)
        gdal.VSIFCloseL(fh)

        stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
        with gdal.quiet_errors():
          batches = list(stream)
        self.assertEqual(
            lyr.GetMetadataItem(
                'LAST_GET_NEXT_ARROW_ARRAY_USED_OPTIMIZED_CODE_PATH',
                '__DEBUG__',
            ),
            'YES',
        )
        self.assertEmpty(batches)

  def test_ogr_shape_logical_field(self) -> None:
    with (
        _enable_exceptions(),
        gcore_util.VsimemCleanupDir(
            '/vsimem/ogr_shape_logical_field'
        ) as vsimem_dir,
    ):
      filename = f'{vsimem_dir}/test_ogr_shape_logical_field.shp'
      with gdal.GetDriverByName(DRIVER).CreateVector(filename) as ds:
        lyr = ds.CreateLayer('test')
        fld_defn = ogr.FieldDefn('bool_field', ogr.OFTInteger)
        fld_defn.SetSubType(ogr.OFSTBoolean)
        lyr.CreateField(fld_defn)
        lyr.CreateField(ogr.FieldDefn('int_field', ogr.OFTInteger))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['bool_field'] = True
        f['int_field'] = 1234
        lyr.CreateFeature(f)
        f = ogr.Feature(lyr.GetLayerDefn())
        f['bool_field'] = False
        f['int_field'] = -1234
        lyr.CreateFeature(f)
        f = ogr.Feature(lyr.GetLayerDefn())
        f['bool_field'] = None
        lyr.CreateFeature(f)
        f = None

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        fld_defn = lyr.GetLayerDefn().GetFieldDefn(0)
        self.assertEqual(fld_defn.GetType(), ogr.OFTInteger)
        self.assertEqual(fld_defn.GetSubType(), ogr.OFSTBoolean)
        self.assertEqual(fld_defn.GetWidth(), 1)
        f = lyr.GetNextFeature()
        self.assertEqual(f['bool_field'], True)
        self.assertEqual(f['int_field'], 1234)
        f = lyr.GetNextFeature()
        self.assertEqual(f['bool_field'], False)
        self.assertEqual(f['int_field'], -1234)
        f = lyr.GetNextFeature()
        self.assertIsNone(f['bool_field'])

  def test_ogr_shape_read_date_empty_string(self) -> None:
    with (
        _enable_exceptions(),
        ogr.Open(ogr_util.GetTestFilePath('shape/date_empty_string.dbf')) as ds,
    ):
      lyr = ds.GetLayer(0)
      f = lyr.GetNextFeature()
      self.assertIsNone(f['date'])

  @parameterized.named_parameters(
      ('poly_golden', 'shape/poly_golden'),
  )
  def test_ogr_shape_write_check_golden_file(self, src_rel_dir: str) -> None:
    src_directory = ogr_util.GetTestFilePath(src_rel_dir)
    with gcore_util.TestTemporaryDirectory(prefix='shape_golden') as tmpdir:
      out_directory = os.path.join(tmpdir, 'test')
      gdal.VectorTranslate(
          out_directory,
          src_directory,
          format=DRIVER,
          layerCreationOptions=['DBF_DATE_LAST_UPDATE=2000-01-01'],
      )
      for filename in os.listdir(src_directory):
        src_filename = os.path.join(src_directory, filename)
        out_filename = os.path.join(out_directory, filename)

        self.assertEqual(
            os.stat(src_filename).st_size,
            os.stat(out_filename).st_size,
            filename,
        )
        with (
            open(src_filename, 'rb') as f_src,
            open(out_filename, 'rb') as f_out,
        ):
          self.assertEqual(f_src.read(), f_out.read(), filename)


if __name__ == '__main__':
  googletest.main()
