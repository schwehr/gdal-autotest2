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
# Copyright (c) 2004, Paul Ramsey <pramsey@boundlessgeom.com>
# Copyright (c) 2014, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
"""Test the GeoPackage (gpkg) vector driver.

Format is described here:

https://gdal.org/drivers/vector/gpkg.html

Rewrite of:

https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gpkg.py
"""

from collections.abc import Iterator, Sequence
import contextlib
import importlib
import importlib.util
import math
import os
import pathlib
import re
import sqlite3
import struct
import subprocess
import sys
import threading
import time
from typing import Any

from osgeo import gdal
from osgeo import gdal_array
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

DRIVER = ogr_util.GPKG_DRIVER
EXT = '.gpkg'

_VALIDATE_GPKG: Any = importlib.import_module(
    'google3.third_party.gdal.swig.python.'
    'gdal-utils.osgeo_utils.samples.validate_gpkg'
)


def setUpModule() -> None:
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


@contextlib.contextmanager
def _quiet_error_handler() -> Iterator[None]:
  gdal.PushErrorHandler('CPLQuietErrorHandler')
  try:
    yield
  finally:
    gdal.PopErrorHandler()


@contextlib.contextmanager
def _error_raised(err_type: int, match: str = '') -> Iterator[None]:
  err_levels = {
      gdal.CE_Debug: 'CE_Debug',
      gdal.CE_Failure: 'CE_Failure',
      gdal.CE_Fatal: 'CE_Fatal',
      gdal.CE_Warning: 'CE_Warning',
  }
  errors: list[dict[str, Any]] = []

  def handler(lvl: int, no: int, msg: str) -> None:
    errors.append({'level': lvl, 'number': no, 'message': msg})

  gdal.PushErrorHandler(handler)
  try:
    yield
  finally:
    gdal.PopErrorHandler()

  received = f'{[(err_levels[err["level"]], err["message"]) for err in errors]}'
  if err_type == gdal.CE_None:
    assert not any(
        err['level'] != gdal.CE_Debug for err in errors
    ), f'Received: {received}'
  else:
    assert any(
        err['level'] == err_type and match in err['message'] for err in errors
    ), (
        f'Did not receive an error of type {err_levels[err_type]} matching '
        f'"{match}". Received: {received}'
    )


@contextlib.contextmanager
def _spatial_filter(lyr: ogr.Layer, *args: Any) -> Iterator[None]:
  if len(args) == 1:
    if isinstance(args[0], str):
      lyr.SetSpatialFilter(ogr.CreateGeometryFromWkt(args[0]))
    else:
      lyr.SetSpatialFilter(args[0])
  elif len(args) == 4:
    lyr.SetSpatialFilterRect(*args)
  else:
    raise ValueError(f'Unknown spatial filter args: {args!r}')
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


def _reopen(
    ds: gdal.Dataset,
    update: bool = False,
    open_options: Sequence[str] | dict[str, str] | None = None,
) -> gdal.Dataset:
  ds_loc = ds.GetDescription()
  ds_drv = ds.GetDriver()
  ds.Close()
  flags = gdal.OF_UPDATE if update else 0
  return gdal.OpenEx(
      ds_loc,
      flags,
      allowed_drivers=[ds_drv.GetDescription()],
      open_options=open_options or [],
  )


def _have_geos() -> bool:
  point1 = ogr.CreateGeometryFromWkt('POINT(10 20)')
  point2 = ogr.CreateGeometryFromWkt('POINT(30 20)')
  return point1.Union(point2) is not None


def _get_sqlite_version() -> tuple[int, ...]:
  with gdal.ExceptionMgr(useExceptions=False):
    ds = ogr.Open(':memory:')
  if ds is None:
    return (0, 0, 0)
  with ds.ExecuteSQL('SELECT sqlite_version()') as sql_lyr:
    f = sql_lyr.GetNextFeature()
    version = f.GetField(0)
  return tuple(int(x) for x in version.split('.')[0:3])


def _has_spatialite_4_3_or_later(ds: gdal.Dataset) -> bool:
  has_spatialite = False
  with _quiet_error_handler():
    sql_lyr = ds.ExecuteSQL('SELECT spatialite_version()')
    if sql_lyr:
      f = sql_lyr.GetNextFeature()
      version = f.GetField(0)
      version_num = float('.'.join(version.split('.')[0:2]))
      if version_num >= 4.3:
        has_spatialite = True
      ds.ReleaseResultSet(sql_lyr)
  return has_spatialite


def _load_poly_features() -> list[ogr.Feature]:
  with ogr.Open(ogr_util.GetTestFilePath('shape/poly/poly.shp')) as shp_ds:
    shp_lyr = shp_ds.GetLayer(0)
    return list(shp_lyr)


class GpkgTest(ogr_util.DriverTestCase, parameterized.TestCase):

  def setUp(self) -> None:  # pyrefly: ignore[bad-override]
    super().setUp(DRIVER, EXT)
    self.enterContext(gdal.ExceptionMgr(useExceptions=False))
    self.enterContext(ogr.ExceptionMgr(useExceptions=False))
    self.enterContext(osr.ExceptionMgr(useExceptions=False))
    self.enterContext(gdal.config_options({'OGR_SQLITE_SYNCHRONOUS': 'OFF'}))

    safe_id = re.sub(r'[^a-zA-Z0-9_]', '_', self.id().split('.')[-1])
    self.vsimem_dir = pathlib.PurePosixPath(f'/vsimem/{safe_id}')
    gdal.Mkdir(str(self.vsimem_dir), 0o755)
    self.addCleanup(gdal.RmdirRecursive, str(self.vsimem_dir))
    self.tmp_dir = pathlib.Path(self.create_tempdir().full_path)

  def getTestFilePath(self, filename: str) -> str:
    return ogr_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def validate(self, gpkg: Any, quiet: bool = False) -> bool:
    if hasattr(gpkg, 'GetDescription'):
      path_str = gpkg.GetDescription()
    else:
      path_str = str(gpkg)

    if path_str.startswith('/vsimem/'):
      stat = gdal.VSIStatL(path_str)
      if stat is None:
        if quiet:
          return False
        self.fail(f'Missing /vsimem/ file: {path_str}')
      content = gcore_util.vsi_read_file_as_bytes(path_str, stat.size)
      real_path = self.create_tempfile('validate.gpkg').full_path
      pathlib.Path(real_path).write_bytes(content)
    else:
      real_path = path_str

    try:
      _VALIDATE_GPKG.check(real_path, extra_checks=True, warning_as_error=True)
      return True
    except Exception:
      if not quiet:
        raise
      return False

  def _create_gpkg_ds(self, name: str = 'test.gpkg') -> gdal.Dataset:
    return self.driver.CreateDataSource(str(self.tmp_dir / name))

  def _create_test_layer(
      self,
      ds: gdal.Dataset,
      src_features: Sequence[ogr.Feature],
      layer_name: str,
  ) -> None:
    lyr = ds.CreateLayer(layer_name)
    _quick_create_layer_def(
        lyr,
        [
            ('AREA', ogr.OFTReal),
            ('EAS_ID', ogr.OFTInteger),
            ('PRFEDEA', ogr.OFTString),
            ('SHORTNAME', ogr.OFTString, 8),
            ('REALLIST', ogr.OFTRealList),
        ],
    )
    dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
    for feat in src_features:
      dst_feat.SetFrom(feat)
      lyr.CreateFeature(dst_feat)

  def _create_tpoly(self, ds: gdal.Dataset) -> None:
    self._create_test_layer(ds, _load_poly_features(), 'tpoly')

  def _create_tpoly_dbl_quote(self, ds: gdal.Dataset) -> None:
    self._create_test_layer(ds, _load_poly_features(), 'tpoly"dbl_quote')

  def _create_a_layer(self, ds: gdal.Dataset) -> None:
    ds.CreateLayer('a_layer', options=['SPATIAL_INDEX=NO'])

  def _create_tbl_linestring(self, ds: gdal.Dataset) -> None:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    lyr = ds.CreateLayer('tbl_linestring', geom_type=ogr.wkbLineString, srs=srs)
    self.assertIsNotNone(lyr)

    lyr.StartTransaction()
    lyr.CreateField(ogr.FieldDefn('fld_integer', ogr.OFTInteger))
    lyr.CreateField(ogr.FieldDefn('fld_string', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('fld_real', ogr.OFTReal))
    lyr.CreateField(ogr.FieldDefn('fld_date', ogr.OFTDate))
    lyr.CreateField(ogr.FieldDefn('fld_datetime', ogr.OFTDateTime))
    lyr.CreateField(ogr.FieldDefn('fld_binary', ogr.OFTBinary))
    fld_defn = ogr.FieldDefn('fld_boolean', ogr.OFTInteger)
    fld_defn.SetSubType(ogr.OFSTBoolean)
    lyr.CreateField(fld_defn)
    fld_defn = ogr.FieldDefn('fld_smallint', ogr.OFTInteger)
    fld_defn.SetSubType(ogr.OFSTInt16)
    lyr.CreateField(fld_defn)
    fld_defn = ogr.FieldDefn('fld_float', ogr.OFTReal)
    fld_defn.SetSubType(ogr.OFSTFloat32)
    lyr.CreateField(fld_defn)
    lyr.CreateField(ogr.FieldDefn('fld_integer64', ogr.OFTInteger64))

    geom = ogr.CreateGeometryFromWkt('LINESTRING(5 5,10 5,10 10,5 10)')
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(geom)

    for i in range(10):
      feat.SetFID(-1)
      feat.SetField('fld_integer', 10 + i)
      feat.SetField('fld_real', 3.14159 / (i + 1))
      feat.SetField('fld_string', f'test string {i} test')
      feat.SetField('fld_date', '2014/05/17 ')
      feat.SetField('fld_datetime', '2014/12/31  23:59:59.999Z')
      feat.SetField('fld_binary', b'\xff\xfe')
      feat.SetField('fld_boolean', 1)
      feat.SetField('fld_smallint', -32768)
      feat.SetField('fld_float', 1.23)
      feat.SetField('fld_integer64', 1000000000000 + i)
      self.assertEqual(lyr.CreateFeature(feat), 0)
    lyr.CommitTransaction()

  def _create_point_no_spi_but_with_dashes(self, ds: gdal.Dataset) -> None:
    sr = osr.SpatialReference()
    sr.ImportFromEPSG(32631)
    lyr = ds.CreateLayer(
        'point_no_spi-but-with-dashes',
        geom_type=ogr.wkbPoint,
        options=['SPATIAL_INDEX=NO'],
        srs=sr,
    )
    for wkt in (
        'POINT(1000 30000000)',
        'POINT(-1000 30000000)',
        'POINT(1000 -30000000)',
        'POINT(-1000 -30000000)',
        None,
        'POINT EMPTY',
    ):
      feat = ogr.Feature(lyr.GetLayerDefn())
      if wkt is not None:
        feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(feat)

  def _create_point_with_spi_and_dashes(self, ds: gdal.Dataset) -> None:
    lyr = ds.CreateLayer(
        'point-with-spi-and-dashes',
        geom_type=ogr.wkbPoint,
        options=['SRID=0'],
    )
    self.assertEqual(lyr.TestCapability(ogr.OLCFastSpatialFilter), 1)
    for wkt in (
        'POINT(1000 30000000)',
        'POINT(-1000 30000000)',
        'POINT(1000 -30000000)',
        'POINT(-1000 -30000000)',
        None,
        'POINT EMPTY',
    ):
      feat = ogr.Feature(lyr.GetLayerDefn())
      if wkt is not None:
        feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(feat)

  def test_ogr_gpkg_1(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self.assertTrue(self.validate(gpkg_ds))

  def test_ogr_gpkg_2(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    # Should default to GPKG 1.4
    with gpkg_ds.ExecuteSQL('PRAGMA application_id') as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['application_id'], 1196444487)

    with gpkg_ds.ExecuteSQL('PRAGMA user_version') as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['user_version'], 10400)

  def test_ogr_gpkg_2bis(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    # Test creating a layer with an existing name
    lyr = gpkg_ds.CreateLayer('a_layer', options=['SPATIAL_INDEX=NO'])
    self.assertIsNotNone(lyr)
    with _quiet_error_handler():
      lyr = gpkg_ds.CreateLayer('a_layer', options=['SPATIAL_INDEX=NO'])
    self.assertIsNone(lyr, 'layer creation should have failed')

  def test_ogr_gpkg_3(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    srs4326 = osr.SpatialReference()
    srs4326.ImportFromEPSG(4326)
    lyr = gpkg_ds.CreateLayer(
        'first_layer',
        geom_type=ogr.wkbPoint,
        srs=srs4326,
        options=['GEOMETRY_NAME=gpkg_geometry', 'SPATIAL_INDEX=NO'],
    )
    self.assertIsNotNone(lyr)
    self.assertEqual(gpkg_ds.GetSpatialRef().GetAuthorityCode(None), '4326')

    gpkg_ds.CreateLayer('a_layer', options=['SPATIAL_INDEX=NO'])
    self.assertTrue(self.validate(gpkg_ds))

    gpkg_ds = _reopen(gpkg_ds)
    self.assertIsNotNone(gpkg_ds)
    self.assertEqual(gpkg_ds.GetLayerCount(), 2)

    lyr0 = gpkg_ds.GetLayer(0)
    self.assertEqual(lyr0.GetFIDColumn(), 'fid')

    gpkg_ds = _reopen(gpkg_ds, update=True)
    lyr0 = gpkg_ds.GetLayer(0)
    self.assertEqual(lyr0.GetName(), 'first_layer')

    gpkg_ds = _reopen(gpkg_ds, update=True)
    lyr0 = gpkg_ds.GetLayer(0)
    lyr1 = gpkg_ds.GetLayer(1)
    self.assertEqual(
        lyr0.GetLayerDefn().GetGeomFieldDefn(0).GetName(), 'gpkg_geometry'
    )
    self.assertEqual(lyr1.GetName(), 'a_layer')

    with gpkg_ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'gpkg_extensions'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)

  def test_ogr_gpkg_5(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_a_layer(gpkg_ds)
    self._create_tpoly(gpkg_ds)
    self.assertEqual(gpkg_ds.GetLayerCount(), 2)

    with _quiet_error_handler():
      ret = gpkg_ds.DeleteLayer(-1)
    self.assertNotEqual(ret, 0)

    with _quiet_error_handler():
      ret = gpkg_ds.DeleteLayer(gpkg_ds.GetLayerCount())
    self.assertNotEqual(ret, 0)

    self.assertEqual(gpkg_ds.DeleteLayer(1), 0)
    self.assertEqual(gpkg_ds.DeleteLayer(0), 0)
    self.assertEqual(gpkg_ds.GetLayerCount(), 0)

  def test_ogr_gpkg_truncate_spatial_layer(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_tpoly_dbl_quote(gpkg_ds)
    poly_feat = _load_poly_features()
    with _enable_exceptions():
      gpkg_ds.ExecuteSQL('DELETE FROM "tpoly""dbl_quote"')

      lyr = gpkg_ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 0)

      dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
      for feat in poly_feat:
        dst_feat.SetFrom(feat)
        lyr.CreateFeature(dst_feat)

      for feat in poly_feat:
        minx, maxx, miny, maxy = feat.GetGeometryRef().GetEnvelope()
        lyr.SetSpatialFilterRect(minx, miny, maxx, maxy)
        self.assertGreaterEqual(lyr.GetFeatureCount(), 1)

  def test_ogr_gpkg_truncate_non_spatial_layer(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')
    with _enable_exceptions():
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)
        lyr.CreateField(ogr.FieldDefn('foo'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['foo'] = 'bar'
        lyr.CreateFeature(f)

      with ogr.Open(filename, 1) as ds:
        ds.ExecuteSQL('DELETE FROM test WHERE 0')
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 1)

      with ogr.Open(filename, 1) as ds:
        ds.ExecuteSQL('DELETE FROM test')
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 0)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 0)

  def test_ogr_gpkg_truncate_with_custom_trigger(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')
    with _enable_exceptions():
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)
        lyr.CreateField(ogr.FieldDefn('foo'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['foo'] = 'bar'
        lyr.CreateFeature(f)
        ds.ExecuteSQL(
            'CREATE TABLE counter(table_name TEXT UNIQUE NOT NULL, counter INT)'
        )
        ds.ExecuteSQL("INSERT INTO counter VALUES ('test', 1)")
        ds.ExecuteSQL(
            'CREATE TRIGGER my_trigger AFTER DELETE ON test BEGIN UPDATE'
            ' counter SET counter = counter - 1 WHERE table_name ='
            " 'test'; END"
        )

      with ogr.Open(filename, 1) as ds:
        with ds.ExecuteSQL(
            "SELECT counter FROM counter WHERE table_name = 'test'"
        ) as sql_lyr:
          f = sql_lyr.GetNextFeature()
          self.assertEqual(f['counter'], 1)
        ds.ExecuteSQL('DELETE FROM "test"')
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 0)
        with ds.ExecuteSQL(
            "SELECT counter FROM counter WHERE table_name = 'test'"
        ) as sql_lyr:
          f = sql_lyr.GetNextFeature()
          self.assertEqual(f['counter'], 0)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 0)

  def test_ogr_gpkg_truncate_with_custom_trigger_error(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')
    with _enable_exceptions():
      with self.driver.CreateDataSource(filename) as ds:
        lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)
        lyr.CreateField(ogr.FieldDefn('foo'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['foo'] = 'bar'
        lyr.CreateFeature(f)
        ds.ExecuteSQL(
            'CREATE TABLE counter(table_name TEXT UNIQUE NOT NULL, counter INT)'
        )
        ds.ExecuteSQL("INSERT INTO counter VALUES ('test', 1)")
        ds.ExecuteSQL(
            'CREATE TRIGGER my_trigger AFTER DELETE ON test BEGIN'
            " SELECT RAISE(ABORT, 'error from trigger'); END"
        )

      with ogr.Open(filename, 1) as ds:
        with ds.ExecuteSQL(
            "SELECT counter FROM counter WHERE table_name = 'test'"
        ) as sql_lyr:
          f = sql_lyr.GetNextFeature()
          self.assertEqual(f['counter'], 1)
        with self.assertRaisesRegex(Exception, 'error from trigger'):
          ds.ExecuteSQL('DELETE FROM "test"')
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 1)
        with ds.ExecuteSQL(
            "SELECT counter FROM counter WHERE table_name = 'test'"
        ) as sql_lyr:
          f = sql_lyr.GetNextFeature()
          self.assertEqual(f['counter'], 1)

      with ogr.Open(filename) as ds:
        lyr = ds.GetLayer(0)
        self.assertEqual(lyr.GetFeatureCount(), 1)

  def test_ogr_gpkg_truncate_read_only_mode(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')
    with _enable_exceptions():
      with self.driver.CreateDataSource(filename) as ds:
        ds.CreateLayer('test')

      with ogr.Open(filename) as ds:
        with self.assertRaisesRegex(Exception, 'read-only'):
          ds.ExecuteSQL('DELETE FROM "test"')

  def test_ogr_gpkg_truncate_non_existing_table(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')
    with _enable_exceptions():
      with self.driver.CreateDataSource(filename) as ds:
        with self.assertRaisesRegex(Exception, 'no such table'):
          ds.ExecuteSQL('DELETE FROM "i_do_not_exist"')

  def test_ogr_gpkg_6(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    srs4326 = osr.SpatialReference()
    srs4326.ImportFromEPSG(4326)
    lyr = gpkg_ds.CreateLayer(
        'field_test_layer', geom_type=ogr.wkbPoint, srs=srs4326
    )
    self.assertIsNotNone(lyr)

    field_defn = ogr.FieldDefn('dummy', ogr.OFTString)
    lyr.CreateField(field_defn)
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
    )

    gpkg_ds = _reopen(gpkg_ds)
    self.assertTrue(self.validate(gpkg_ds))

    with _quiet_error_handler():
      gpkg_ds = _reopen(gpkg_ds)

    self.assertIsNotNone(gpkg_ds)
    self.assertEqual(gpkg_ds.GetLayerCount(), 1)
    lyr = gpkg_ds.GetLayer(0)
    self.assertEqual(lyr.GetName(), 'field_test_layer')

    field_defn_out = lyr.GetLayerDefn().GetFieldDefn(0)
    self.assertEqual(field_defn_out.GetType(), ogr.OFTString)
    self.assertEqual(field_defn_out.GetName(), 'dummy')

  def test_ogr_gpkg_7(self) -> None:
    gpkg_ds = self._create_gpkg_ds()

    def get_feature_count_from_gpkg_contents():
      with gpkg_ds.ExecuteSQL(
          'SELECT feature_count FROM gpkg_ogr_contents '
          "WHERE table_name = 'field_test_layer'",
          dialect='DEBUG',
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        return f.GetField(0)

    lyr = gpkg_ds.CreateLayer('field_test_layer', geom_type=ogr.wkbPoint)
    lyr.CreateField(ogr.FieldDefn('dummy', ogr.OFTString))
    gpkg_ds.SyncToDisk()
    self.assertEqual(get_feature_count_from_gpkg_contents(), 0)

    gpkg_ds = _reopen(gpkg_ds, update=True)
    self.assertEqual(get_feature_count_from_gpkg_contents(), 0)

    lyr = gpkg_ds.GetLayerByName('field_test_layer')
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(10 10)'))
    feat.SetField('dummy', 'a dummy value')

    self.assertEqual(lyr.TestCapability(ogr.OLCSequentialWrite), 1)
    self.assertEqual(lyr.CreateFeature(feat), 0)
    self.assertIsNone(get_feature_count_from_gpkg_contents())

    lyr.ResetReading()
    feat_read = lyr.GetNextFeature()
    self.assertEqual(feat_read.GetField('dummy'), 'a dummy value')
    self.assertIsNone(lyr.GetNextFeature())
    self.assertIsNone(lyr.GetNextFeature())

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(100 100)'))
    feat.SetField('dummy', 'who you calling a dummy?')
    self.assertEqual(lyr.CreateFeature(feat), 0)

    self.assertEqual(lyr.TestCapability(ogr.OLCRandomRead), 1)
    feat_read_random = lyr.GetFeature(feat.GetFID())
    self.assertEqual(
        feat_read_random.GetField('dummy'), 'who you calling a dummy?'
    )

    self.assertEqual(lyr.TestCapability(ogr.OLCRandomWrite), 1)
    feat.SetField('dummy', 'i am no dummy')
    lyr.SetFeature(feat)
    feat_read_random = lyr.GetFeature(feat.GetFID())
    self.assertEqual(feat_read_random.GetField('dummy'), 'i am no dummy')

    self.assertEqual(lyr.TestCapability(ogr.OLCDeleteFeature), 1)
    self.assertEqual(lyr.GetFeatureCount(), 2)
    self.assertEqual(
        lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn())), ogr.OGRERR_NONE
    )
    self.assertEqual(lyr.GetFeatureCount(), 3)

    feat.SetField('dummy', 'updated')
    fid = feat.GetFID()
    self.assertEqual(lyr.UpsertFeature(feat), ogr.OGRERR_NONE)
    self.assertEqual(feat.GetFID(), fid)
    upserted_feat = lyr.GetFeature(feat.GetFID())
    self.assertEqual(upserted_feat.GetField('dummy'), 'updated')

    lyr.DeleteFeature(feat.GetFID())
    self.assertIsNone(get_feature_count_from_gpkg_contents())
    lyr.SyncToDisk()
    self.assertIsNone(get_feature_count_from_gpkg_contents())
    self.assertEqual(lyr.GetFeatureCount(), 2)
    self.assertEqual(get_feature_count_from_gpkg_contents(), 2)

    self.assertEqual(lyr.UpsertFeature(feat), ogr.OGRERR_NONE)
    self.assertEqual(feat.GetFID(), fid)
    self.assertEqual(get_feature_count_from_gpkg_contents(), 3)
    self.assertEqual(lyr.GetFeatureCount(), 3)
    lyr.SyncToDisk()
    self.assertEqual(get_feature_count_from_gpkg_contents(), 3)

    feat.SetFID(-10)
    self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NON_EXISTING_FEATURE)
    self.assertEqual(lyr.DeleteFeature(-10), ogr.OGRERR_NON_EXISTING_FEATURE)
    self.assertEqual(gpkg_ds.DeleteLayer('field_test_layer'), ogr.OGRERR_NONE)

  def test_ogr_gpkg_8(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_tbl_linestring(gpkg_ds)
    lyr = gpkg_ds.GetLayer('tbl_linestring')

    feat = ogr.Feature(lyr.GetLayerDefn())
    self.assertEqual(lyr.CreateFeature(feat), 0)
    feat.SetFID(6)
    self.assertEqual(lyr.SetFeature(feat), 0)

    gpkg_ds = _reopen(gpkg_ds, update=True)
    self.assertTrue(self.validate(gpkg_ds))

    lyr = gpkg_ds.GetLayerByName('tbl_linestring')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(6).GetSubType(), ogr.OFSTBoolean
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(7).GetSubType(), ogr.OFSTInt16
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(8).GetSubType(), ogr.OFSTFloat32
    )
    feat = lyr.GetNextFeature()
    self.assertEqual(feat.GetField(0), 10)
    self.assertEqual(feat.GetField(1), 'test string 0 test')
    self.assertEqual(feat.GetField(2), 3.14159)
    self.assertEqual(feat.GetField(3), '2014/05/17')
    self.assertEqual(feat.GetField(4), '2014/12/31 23:59:59.999+00')
    self.assertEqual(feat.GetField(5), 'FFFE')
    self.assertEqual(feat.GetField(6), 1)
    self.assertEqual(feat.GetField(7), -32768)
    self.assertEqual(feat.GetField(8), 1.23)
    self.assertEqual(feat.GetField(9), 1000000000000)

  def test_ogr_gpkg_8a(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    lyr = gpkg_ds.CreateLayer('tbl_polygon', geom_type=ogr.wkbPolygon, srs=srs)
    self.assertIsNotNone(lyr)

    lyr.StartTransaction()
    lyr.CreateField(ogr.FieldDefn('fld_datetime', ogr.OFTDateTime))
    lyr.CreateField(ogr.FieldDefn('fld_string', ogr.OFTString))

    geom = ogr.CreateGeometryFromWkt(
        'POLYGON((5 5, 10 5, 10 10, 5 10, 5 5),(6 6, 6 7, 7 7, 7 6, 6 6))'
    )
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(geom)

    for i in range(10):
      feat.SetFID(-1)
      feat.SetField('fld_string', f'my super string {i}')
      feat.SetField('fld_datetime', '2010-01-01')
      self.assertEqual(lyr.CreateFeature(feat), 0)
    lyr.CommitTransaction()

    feat = lyr.GetFeature(3)
    geom_read = feat.GetGeometryRef()
    self.assertEqual(geom.ExportToWkt(), geom_read.ExportToWkt())

  def test_ogr_gpkg_8b(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    lyr = gpkg_ds.CreateLayer(
        'tbl_polygon25d', geom_type=ogr.wkbPolygon25D, srs=srs
    )
    self.assertIsNotNone(lyr)

    lyr.CreateField(ogr.FieldDefn('fld_string', ogr.OFTString))
    geom = ogr.CreateGeometryFromWkt(
        'POLYGON((5 5 1, 10 5 2, 10 10 3, 5 104 , 5 5 1),'
        '(6 6 4, 6 7 5, 7 7 6, 7 6 7, 6 6 4))'
    )
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    geom_read = feat.GetGeometryRef()
    self.assertEqual(geom.ExportToWkt(), geom_read.ExportToWkt())

  def test_ogr_gpkg_9(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_tbl_linestring(gpkg_ds)
    lyr = gpkg_ds.GetLayerByName('tbl_linestring')
    self.assertEqual(lyr.GetExtent(), (5.0, 10.0, 5.0, 10.0))
    self.assertEqual(
        lyr.GetExtent3D(),
        (5.0, 10.0, 5.0, 10.0, float('inf'), float('-inf')),
    )
    self.assertEqual(lyr.GetFeatureCount(), 10)

  def test_ogr_gpkg_11(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_tbl_linestring(gpkg_ds)
    gpkg_ds.ExecuteSQL(
        'CREATE INDEX tbl_linestring_fld_integer_idx ON'
        ' tbl_linestring(fld_integer)'
    )
    gpkg_ds.ExecuteSQL(
        'ALTER TABLE tbl_linestring RENAME TO tbl_linestring_renamed;'
    )
    gpkg_ds.ExecuteSQL('VACUUM')

    gpkg_ds = _reopen(gpkg_ds, update=True)
    lyr = gpkg_ds.GetLayerByName('tbl_linestring_renamed')
    self.assertIsNotNone(lyr)
    lyr.SetAttributeFilter('fld_integer = 10')
    self.assertEqual(lyr.GetFeatureCount(), 1)

  def test_ogr_gpkg_12(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_tbl_linestring(gpkg_ds)
    gpkg_ds.ExecuteSQL(
        'ALTER TABLE tbl_linestring RENAME TO tbl_linestring_renamed;'
    )

    with gpkg_ds.ExecuteSQL('SELECT * FROM tbl_linestring_renamed') as sql_lyr:
      self.assertEqual(sql_lyr.GetFIDColumn(), 'fid')
      self.assertEqual(sql_lyr.GetGeomType(), ogr.wkbLineString)
      self.assertEqual(sql_lyr.GetGeometryColumn(), 'geom')
      self.assertIn('32631', sql_lyr.GetSpatialRef().ExportToWkt())
      feat = sql_lyr.GetNextFeature()
      self.assertEqual(feat.GetFID(), 1)
      self.assertEqual(sql_lyr.GetFeatureCount(), 10)
      self.assertEqual(sql_lyr.GetLayerDefn().GetFieldCount(), 10)
      self.assertEqual(
          sql_lyr.GetLayerDefn().GetFieldDefn(6).GetSubType(), ogr.OFSTBoolean
      )
      self.assertEqual(
          sql_lyr.GetLayerDefn().GetFieldDefn(7).GetSubType(), ogr.OFSTInt16
      )
      self.assertEqual(
          sql_lyr.GetLayerDefn().GetFieldDefn(8).GetSubType(), ogr.OFSTFloat32
      )

    with gpkg_ds.ExecuteSQL(
        'SELECT '
        'CAST(fid AS INTEGER) AS FID, '
        'CAST(fid AS INTEGER) AS FID, '
        '_rowid_ ,'
        'CAST(geom AS BLOB) AS GEOM, '
        'CAST(geom AS BLOB) AS GEOM, '
        'CAST(fld_integer AS INTEGER) AS FLD_INTEGER, '
        'CAST(fld_integer AS INTEGER) AS FLD_INTEGER, '
        'CAST(fld_string AS TEXT) AS FLD_STRING, '
        'CAST(fld_real AS REAL) AS FLD_REAL, '
        'CAST(fld_binary as BLOB) as FLD_BINARY, '
        'CAST(fld_integer64 AS INTEGER) AS FLD_INTEGER64 '
        'FROM tbl_linestring_renamed'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFIDColumn(), 'FID')
      self.assertEqual(sql_lyr.GetGeometryColumn(), 'GEOM')
      defn = sql_lyr.GetLayerDefn()
      self.assertEqual(defn.GetFieldCount(), 5)
      self.assertEqual(defn.GetFieldDefn(0).GetName(), 'FLD_INTEGER')
      self.assertEqual(defn.GetFieldDefn(0).GetType(), ogr.OFTInteger)
      self.assertEqual(defn.GetFieldDefn(1).GetName(), 'FLD_STRING')
      self.assertEqual(defn.GetFieldDefn(1).GetType(), ogr.OFTString)
      self.assertEqual(defn.GetFieldDefn(2).GetName(), 'FLD_REAL')
      self.assertEqual(defn.GetFieldDefn(2).GetType(), ogr.OFTReal)
      self.assertEqual(defn.GetFieldDefn(3).GetName(), 'FLD_BINARY')
      self.assertEqual(defn.GetFieldDefn(3).GetType(), ogr.OFTBinary)
      self.assertEqual(defn.GetFieldDefn(4).GetName(), 'FLD_INTEGER64')
      self.assertEqual(defn.GetFieldDefn(4).GetType(), ogr.OFTInteger64)

    with gpkg_ds.ExecuteSQL(
        'SELECT * FROM tbl_linestring_renamed WHERE 0=1'
    ) as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature())

    for sql in (
        'SELECT * FROM tbl_linestring_renamed LIMIT 1',
        'SELECT * FROM tbl_linestring_renamed ORDER BY fld_integer LIMIT 1',
        (
            'SELECT * FROM tbl_linestring_renamed UNION ALL SELECT * FROM'
            ' tbl_linestring_renamed ORDER BY fld_integer LIMIT 1'
        ),
    ):
      with gpkg_ds.ExecuteSQL(sql) as sql_lyr:
        self.assertIsNotNone(sql_lyr.GetNextFeature())
        self.assertIsNone(sql_lyr.GetNextFeature())
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    with gpkg_ds.ExecuteSQL('SELECT sqlite_version()') as sql_lyr:
      self.assertIsNotNone(sql_lyr.GetNextFeature())
      self.assertEqual(sql_lyr.GetLayerDefn().GetFieldCount(), 1)
      self.assertEqual(sql_lyr.GetLayerDefn().GetGeomFieldCount(), 0)

  def test_ogr_gpkg_13(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    lyr = gpkg_ds.CreateLayer('non_spatial', geom_type=ogr.wkbNone)
    lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
    lyr.CreateField(ogr.FieldDefn('fld_integer', ogr.OFTInteger))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField('fld_integer', 1)
    lyr.CreateFeature(feat)
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.assertTrue(feat.IsFieldNull('fld_integer'))
    feat = lyr.GetNextFeature()
    self.assertEqual(feat.GetField('fld_integer'), 1)

    gpkg_ds.CreateLayer('non_spatial2', geom_type=ogr.wkbNone)

    gdal.ErrorReset()
    gpkg_ds = _reopen(gpkg_ds, update=True)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    self.assertEqual(gpkg_ds.GetLayerCount(), 2)
    lyr = gpkg_ds.GetLayer('non_spatial')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbNone)
    feat = lyr.GetNextFeature()
    self.assertTrue(feat.IsFieldNull('fld_integer'))
    feat = lyr.GetNextFeature()
    self.assertEqual(feat.GetField('fld_integer'), 1)

  def test_ogr_gpkg_14(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_point_no_spi_but_with_dashes(gpkg_ds)
    self._create_point_with_spi_and_dashes(gpkg_ds)

    lyr = gpkg_ds.GetLayer('point_no_spi-but-with-dashes')
    self.assertEqual(lyr.TestCapability(ogr.OLCFastSpatialFilter), 0)

    f = lyr.GetFeature(5)
    self.assertIsNone(f.GetGeometryRef())

    f = lyr.GetFeature(6)
    self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'POINT EMPTY')

    with gpkg_ds.ExecuteSQL(
        'SELECT * FROM "point_no_spi-but-with-dashes"'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.TestCapability(ogr.OLCFastSpatialFilter), 0)

    with gpkg_ds.ExecuteSQL(
        'SELECT * FROM "point-with-spi-and-dashes"'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.TestCapability(ogr.OLCFastSpatialFilter), 1)

    lyr.SetSpatialFilterRect(1000, 30000000, 1000, 30000000)
    lyr.ResetReading()
    self.assertIsNotNone(lyr.GetNextFeature())
    self.assertIsNone(lyr.GetNextFeature())

  def test_ogr_gpkg_15(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_tbl_linestring(gpkg_ds)
    self._create_point_no_spi_but_with_dashes(gpkg_ds)
    self._create_point_with_spi_and_dashes(gpkg_ds)
    gpkg_ds = _reopen(gpkg_ds, update=True)

    with gpkg_ds.ExecuteSQL(
        'SELECT ST_IsEmpty(geom), ST_SRID(geom), ST_GeometryType(geom), '
        'ST_MinX(geom), ST_MinY(geom), ST_MaxX(geom), ST_MaxY(geom) '
        'FROM "point_no_spi-but-with-dashes" WHERE fid = 1'
    ) as sql_lyr:
      feat = sql_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField(0), 0)
      self.assertEqual(feat.GetField(1), 32631)
      self.assertEqual(feat.GetField(2), 'POINT')
      self.assertEqual(feat.GetField(3), 1000)
      self.assertEqual(feat.GetField(4), 30000000)
      self.assertEqual(feat.GetField(5), 1000)
      self.assertEqual(feat.GetField(6), 30000000)

    lyr = gpkg_ds.GetLayer('tbl_linestring')
    self.assertEqual(lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn())), 0)

    with gpkg_ds.ExecuteSQL(
        'SELECT ST_IsEmpty(geom), ST_SRID(geom), ST_GeometryType(geom), '
        'ST_MinX(geom), ST_MinY(geom), ST_MaxX(geom), ST_MaxY(geom) '
        'FROM tbl_linestring WHERE geom IS NULL'
    ) as sql_lyr:
      feat = sql_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      for i in range(7):
        self.assertTrue(feat.IsFieldNull(i))

    for expected_type, actual_type, expected_result in (
        ('POINT', 'POINT', 1),
        ('LINESTRING', 'POINT', 0),
        ('GEOMETRY', 'POINT', 1),
        ('POINT', 'GEOMETRY', 0),
        ('GEOMETRYCOLLECTION', 'MULTIPOINT', 1),
        ('GEOMETRYCOLLECTION', 'POINT', 0),
    ):
      with gpkg_ds.ExecuteSQL(
          f"SELECT GPKG_IsAssignable('{expected_type}', '{actual_type}')"
      ) as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertEqual(
            feat.GetField(0),
            expected_result,
            msg=f'{expected_type}/{actual_type}',
        )

    lyr = gpkg_ds.CreateLayer('non_spatial', geom_type=ogr.wkbNone)
    lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

    for sql, expected_result in (
        ("SELECT HasSpatialIndex('point-with-spi-and-dashes', 'geom')", 1),
        ("SELECT DisableSpatialIndex('point-with-spi-and-dashes', 'geom')", 1),
        ("SELECT HasSpatialIndex('point-with-spi-and-dashes', 'geom')", 0),
        ("SELECT DisableSpatialIndex('point-with-spi-and-dashes', 'geom')", 0),
        ("SELECT CreateSpatialIndex('point-with-spi-and-dashes', 'geom')", 1),
        ("SELECT HasSpatialIndex('point-with-spi-and-dashes', 'geom')", 1),
        ("SELECT CreateSpatialIndex('point-with-spi-and-dashes', 'geom')", 0),
        ("SELECT CreateSpatialIndex('point-with-spi-and-dashes', NULL)", 0),
        ("SELECT HasSpatialIndex('point-with-spi-and-dashes', NULL)", 0),
        ("SELECT CreateSpatialIndex(NULL, 'geom')", 0),
        ("SELECT CreateSpatialIndex('bla', 'geom')", 0),
        ("SELECT CreateSpatialIndex('point-with-spi-and-dashes', 'bla')", 0),
        ("SELECT DisableSpatialIndex('point-with-spi-and-dashes', NULL)", 0),
        ("SELECT DisableSpatialIndex(NULL, 'geom')", 0),
        ("SELECT DisableSpatialIndex('bla', 'geom')", 0),
        ("SELECT DisableSpatialIndex('point-with-spi-and-dashes', 'bla')", 0),
        ("SELECT HasSpatialIndex(NULL, 'geom')", 0),
        ("SELECT HasSpatialIndex('bla', 'geom')", 0),
        ("SELECT HasSpatialIndex('point-with-spi-and-dashes', 'bla')", 0),
        ("SELECT CreateSpatialIndex('non_spatial', '')", 0),
        (
            "SELECT CreateSpatialIndex('point_no_spi-but-with-dashes', 'geom')",
            1,
        ),
        (
            (
                "SELECT DisableSpatialIndex('point_no_spi-but-with-dashes',"
                " 'geom')"
            ),
            1,
        ),
    ):
      if expected_result == 0:
        with _quiet_error_handler(), gpkg_ds.ExecuteSQL(sql) as sql_lyr:
          got_result = sql_lyr.GetNextFeature().GetField(0)
      else:
        with gpkg_ds.ExecuteSQL(sql) as sql_lyr:
          got_result = sql_lyr.GetNextFeature().GetField(0)
      self.assertEqual(got_result, expected_result, msg=sql)

    with gpkg_ds.ExecuteSQL('SELECT SridFromAuthCRS(NULL, 4326)') as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), -1)

    with gpkg_ds.ExecuteSQL("SELECT SridFromAuthCRS('epsg', NULL)") as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), -1)

    with gpkg_ds.ExecuteSQL("SELECT SridFromAuthCRS('epsg', 4326)") as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 4326)

    with gpkg_ds.ExecuteSQL("SELECT SridFromAuthCRS('epsg', 1234)") as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), -1)

    with gpkg_ds.ExecuteSQL('SELECT ImportFromEPSG(NULL)') as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), -1)

    with gpkg_ds.ExecuteSQL('SELECT ImportFromEPSG(4326)') as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 4326)

    with gpkg_ds.ExecuteSQL('SELECT ImportFromEPSG(32633)') as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 32633)

    with _quiet_error_handler():
      with gpkg_ds.ExecuteSQL('SELECT ImportFromEPSG(0)') as sql_lyr:
        self.assertEqual(sql_lyr.GetNextFeature().GetField(0), -1)

    with gpkg_ds.ExecuteSQL('SELECT ST_Transform(NULL, 4326)') as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature().GetGeometryRef())

    with _quiet_error_handler():
      with gpkg_ds.ExecuteSQL("SELECT ST_Transform(x'00', 4326)") as sql_lyr:
        self.assertIsNone(sql_lyr.GetNextFeature().GetGeometryRef())

    with gpkg_ds.ExecuteSQL(
        'SELECT ST_Transform(geom, NULL) FROM tbl_linestring'
    ) as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature().GetGeometryRef())

    with _quiet_error_handler():
      with gpkg_ds.ExecuteSQL(
          'SELECT ST_Transform(geom, 0), ST_SRID(ST_Transform(geom, 0)) '
          'FROM tbl_linestring'
      ) as sql_lyr:
        self.assertIn(
            'Undefined geographic SRS', sql_lyr.GetSpatialRef().ExportToWkt()
        )
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat.GetGeometryRef())
        self.assertEqual(feat.GetField(0), 0)

    src_lyr = gpkg_ds.GetLayerByName('point-with-spi-and-dashes')
    self.assertIn(
        'Undefined geographic SRS', src_lyr.GetSpatialRef().ExportToWkt()
    )
    with _quiet_error_handler():
      with gpkg_ds.ExecuteSQL(
          'SELECT ST_Transform(geom, 4326), ST_SRID(ST_Transform(geom, 4326)) '
          'FROM "point-with-spi-and-dashes"'
      ) as sql_lyr:
        self.assertIn('WGS_1984', sql_lyr.GetSpatialRef().ExportToWkt())
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat.GetGeometryRef())
        self.assertEqual(feat.GetField(0), 4326)

    with _quiet_error_handler():
      with gpkg_ds.ExecuteSQL(
          "SELECT ST_Transform(x'0001E6100000000000000000000000000000000000000"
          "00000000000000000000000000000007C04000000000000FE', 4326) "
          'FROM tbl_linestring'
      ) as sql_lyr:
        self.assertIsNone(sql_lyr.GetNextFeature().GetGeometryRef())

    with gpkg_ds.ExecuteSQL(
        'SELECT ST_Transform(geom, ST_SRID(geom)) FROM tbl_linestring'
    ) as sql_lyr:
      feat = sql_lyr.GetNextFeature()
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(),
          'LINESTRING (5 5,10 5,10 10,5 10)',
      )

    with gpkg_ds.ExecuteSQL(
        'SELECT ST_SRID(ST_Transform(geom, 4326)) FROM tbl_linestring'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 4326)

    with gpkg_ds.ExecuteSQL(
        "SELECT ST_SRID(ST_Transform(x'0001E6100000000000000000000000000000000"
        "00000000000000000000000000000000000007C0400000000000000FE', 4326)) "
        'FROM tbl_linestring'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 4326)

    with gpkg_ds.ExecuteSQL("SELECT ST_MinX(x'00')") as sql_lyr:
      self.assertFalse(sql_lyr.GetNextFeature().IsFieldSetAndNotNull(0))

    with gpkg_ds.ExecuteSQL("SELECT ST_MinX(x'0001020304050607')") as sql_lyr:
      self.assertFalse(sql_lyr.GetNextFeature().IsFieldSetAndNotNull(0))

    with gpkg_ds.ExecuteSQL(
        "SELECT ST_GeometryType(x'4750001100000000')"
    ) as sql_lyr:
      self.assertFalse(sql_lyr.GetNextFeature().IsFieldSetAndNotNull(0))

    with gpkg_ds.ExecuteSQL(
        "SELECT ST_GeometryType(x'475000110000000001040000')"
    ) as sql_lyr:
      self.assertFalse(sql_lyr.GetNextFeature().IsFieldSetAndNotNull(0))

    with gpkg_ds.ExecuteSQL(
        "SELECT ST_GeometryType(x'47500011000000000104000000')"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 'MULTIPOINT')

    with gpkg_ds.ExecuteSQL(
        "SELECT ST_GeometryType(x'000100000000000000000000000000000000000000000"
        "00000000000000000000000000000007C0400000000000000FE')"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 'MULTIPOINT')

    with gpkg_ds.ExecuteSQL(
        "SELECT ST_IsEmpty(x'0001000000000000000000000000000000000000000000000"
        "0000000000000000000000000007C0400000000000000FE')"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 1)

    with _quiet_error_handler():
      with gpkg_ds.ExecuteSQL(
          "SELECT ST_GeometryType(x'475000030000000000000000000000000000000000"
          '000000000000000000000000000000000000000000000000000000000000000000000'
          "000000000000000000000000000000000000000000000000000')"
      ) as sql_lyr:
        self.assertFalse(sql_lyr.GetNextFeature().IsFieldSetAndNotNull(0))

    with gpkg_ds.ExecuteSQL(
        "SELECT GPKG_IsAssignable('POINT', NULL)"
    ) as sql_lyr:
      sql_lyr.GetNextFeature()

    with gpkg_ds.ExecuteSQL(
        "SELECT GPKG_IsAssignable(NULL, 'POINT')"
    ) as sql_lyr:
      sql_lyr.GetNextFeature()

    with gpkg_ds.ExecuteSQL("SELECT hstore_get_value('a=>b', 'a')") as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 'b')

    with gpkg_ds.ExecuteSQL("SELECT hstore_get_value('a=>b', 'x')") as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature().GetField(0))

    with gpkg_ds.ExecuteSQL("SELECT hstore_get_value('a=>b', NULL)") as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature().GetField(0))

    with gpkg_ds.ExecuteSQL("SELECT hstore_get_value(NULL, 'a')") as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature().GetField(0))

    if (
        ogr.GetGEOSVersionMajor() * 10000
        + ogr.GetGEOSVersionMinor() * 100
        + ogr.GetGEOSVersionMicro()
        >= 30800
    ):
      with gpkg_ds.ExecuteSQL('SELECT ST_MakeValid(NULL)') as sql_lyr:
        self.assertIsNone(sql_lyr.GetNextFeature().GetGeometryRef())

      with gpkg_ds.ExecuteSQL("SELECT ST_MakeValid('invalid')") as sql_lyr:
        self.assertIsNone(sql_lyr.GetNextFeature().GetGeometryRef())

      with gpkg_ds.ExecuteSQL(
          'SELECT ST_MakeValid(geom) FROM tbl_linestring'
      ) as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertEqual(
            feat.GetGeometryRef().ExportToWkt(),
            'LINESTRING (5 5,10 5,10 10,5 10)',
        )

    if _has_spatialite_4_3_or_later(gpkg_ds):
      with gpkg_ds.ExecuteSQL(
          'SELECT ST_Buffer(geom, 1e-10) FROM tbl_linestring'
      ) as sql_lyr:
        self.assertEqual(sql_lyr.GetGeomType(), ogr.wkbPolygon)
        self.assertIn('32631', sql_lyr.GetSpatialRef().ExportToWkt())

  def test_ogr_gpkg_SetSRID(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_SetSRID.gpkg')
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('foo')
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    lyr.CreateFeature(f)

    for srid in (32631, 0, -1, 12345678):
      with ds.ExecuteSQL(
          f'SELECT ST_SRID(SetSRID(geom, {srid})) FROM foo'
      ) as sql_lyr:
        self.assertEqual(sql_lyr.GetNextFeature().GetField(0), srid)

    with ds.ExecuteSQL(
        'SELECT ST_SRID(SetSRID(NULL, 32631)) FROM foo'
    ) as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature().GetField(0))

  def test_ogr_gpkg_ST_EnvIntersects(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_ST_EnvIntersects.gpkg')
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('foo')

    for wkt in ('LINESTRING(1 2,3 4)', 'LINESTRING(5 6,7 8)'):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(f)

    with ds.ExecuteSQL(
        'SELECT ST_EnvIntersects(geom, 0, 0, 0.99, 100),'
        '       ST_EnvIntersects(geom, 0, 4.01, 100, 100),'
        '       ST_EnvIntersects(geom, 3.01, 0, 100, 100),'
        '       ST_EnvIntersects(geom, 0, 0, 100, 1.99),'
        '       ST_EnvIntersects(geom, 0.99, 1.99, 1.01, 2.01),'
        '       ST_EnvIntersects(geom, 0.99, 3.99, 1.01, 4.01),'
        '       ST_EnvIntersects(geom, 2.99, 3.99, 3.01, 4.01),'
        '       ST_EnvIntersects(geom, 2.99, 1.99, 3.01, 2.01)'
        ' FROM foo WHERE fid = 1'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      for idx in range(4):
        self.assertEqual(f.GetField(idx), 0)
      for idx in range(4, 8):
        self.assertEqual(f.GetField(idx), 1)

    with ds.ExecuteSQL(
        'SELECT ST_EnvIntersects(a.geom, b.geom) FROM foo a, foo b '
        'WHERE a.fid = 1 AND b.fid = 1'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 1)

    with ds.ExecuteSQL(
        'SELECT ST_EnvIntersects(a.geom, b.geom) FROM foo a, foo b '
        'WHERE a.fid = 1 AND b.fid = 2'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 0)

    with ds.ExecuteSQL(
        'SELECT ST_EnvIntersects(a.geom, b.geom) FROM foo a, foo b '
        'WHERE a.fid = 2 AND b.fid = 1'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 0)

  def test_ogr_gpkg_16(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpk_16.gpkg')
    ds = self.driver.CreateDataSource(fname)
    ds.CreateLayer('foo')
    ds.ExecuteSQL(
        'INSERT INTO gpkg_extensions ( table_name, column_name, '
        'extension_name, definition, scope ) VALUES '
        "( 'foo', 'geom', 'myext', 'some ext', 'write-only' ) "
    )
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayer(0)
    lyr.GetLayerDefn()
    gdal.ErrorReset()
    ds = None
    self.assertEqual(gdal.GetLastErrorMsg(), '')

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer(0)
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.GetLayerDefn()
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    ds.ExecuteSQL(
        "UPDATE gpkg_extensions SET scope = 'read-write' "
        "WHERE extension_name = 'myext'"
    )
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayer(0)
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.GetLayerDefn()
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer(0)
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.GetLayerDefn()
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_16a(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpk_16a.gpkg')
    ds = self.driver.CreateDataSource(fname)
    ds.CreateLayer('foo')
    ds.ExecuteSQL(
        'INSERT INTO gpkg_extensions ( table_name, column_name, '
        'extension_name, definition, scope ) VALUES '
        "( 'foo', 'geom', 'gpkg_geom_XXXX', 'some ext', 'read-write' ) "
    )
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayer(0)
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.GetLayerDefn()
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_16b(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpk_16b.gpkg')
    ds = self.driver.CreateDataSource(fname)
    ds.CreateLayer('foo')
    ds.ExecuteSQL(
        'INSERT INTO gpkg_extensions ( '
        'extension_name, definition, scope ) VALUES '
        "( 'myext', 'some ext', 'write-only' ) "
    )
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayer(0)
    gdal.ErrorReset()
    lyr.GetLayerDefn()
    self.assertEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(fname, 1)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    ds.ExecuteSQL(
        "UPDATE gpkg_extensions SET scope = 'read-write' "
        "WHERE extension_name = 'myext'"
    )
    ds = None

    gdal.ErrorReset()
    with _quiet_error_handler():
      ogr.Open(fname)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with _quiet_error_handler():
      ogr.Open(fname, 1)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_17(self) -> None:
    ds = self.driver.CreateDataSource(str(self.vsimem_dir / 'ogr_gpkg_17.gpkg'))
    with ds.ExecuteSQL(
        'SELECT ogr_version()', dialect='INDIRECT_SQLITE'
    ) as sql_lyr:
      self.assertIsNotNone(sql_lyr.GetNextFeature())

  def test_ogr_gpkg_18(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_18.gpkg')
    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('wkbCircularString', geom_type=ogr.wkbCircularString)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('CIRCULARSTRING(0 0,1 0,0 0)'))
    lyr.CreateFeature(f)
    f = None
    ds = None

    self.assertTrue(self.validate(fname))

    gdal.ErrorReset()
    ds = ogr.Open(fname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetGeomType(), ogr.wkbCircularString)
    f = lyr.GetNextFeature()
    g = f.GetGeometryRef()
    self.assertEqual(g.GetGeometryType(), ogr.wkbCircularString)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_extensions WHERE table_name = 'wkbCircularString' "
        "AND extension_name = 'gpkg_geom_CIRCULARSTRING'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

  def test_ogr_gpkg_18a(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_18a.gpkg')
    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test')
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('CIRCULARSTRING(0 0,1 0,0 0)'))
    lyr.CreateFeature(f)
    f = None

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_extensions WHERE table_name = 'test' "
        "AND extension_name = 'gpkg_geom_CIRCULARSTRING'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    ds = None

    gdal.ErrorReset()
    ds = ogr.Open(fname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    self.assertEqual(
        f.GetGeometryRef().GetGeometryType(), ogr.wkbCircularString
    )
    ds = None

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer(0)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('CIRCULARSTRING(0 0,1 0,0 0)'))
    gdal.ErrorReset()
    self.assertEqual(lyr.CreateFeature(f), 0)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_18b(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_18b.gpkg')
    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbTriangle)
    self.assertIsNotNone(lyr)
    with _quiet_error_handler():
      ds.FlushCache()
    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_extensions WHERE table_name = 'test' "
        "AND extension_name = 'gpkg_geom_TRIANGLE'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    ds = None

    self.assertFalse(self.validate(fname, quiet=True))

  def test_ogr_gpkg_18c(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_18c.gpkg')
    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test')
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(
        ogr.CreateGeometryFromWkt(
            'GEOMETRYCOLLECTION(CIRCULARSTRING(0 0,1 0,0 0))'
        )
    )
    lyr.CreateFeature(f)
    f = None
    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_extensions WHERE table_name = 'test' "
        "AND extension_name LIKE 'gpkg_geom_%'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

  def test_ogr_gpkg_19(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_19.gpkg')
    ds = self.driver.CreateDataSource(fname)
    self.assertFalse(ds.GetMetadata())
    lyr = ds.CreateLayer('test_without_md')
    self.assertFalse(lyr.GetMetadata())

    ds.SetMetadataItem('foo', 'bar')
    with _quiet_error_handler():
      ds.SetMetadata(ds.GetMetadata('GEOPACKAGE'), 'GEOPACKAGE')
      ds.SetMetadataItem(
          'foo', ds.GetMetadataItem('foo', 'GEOPACKAGE'), 'GEOPACKAGE'
      )
    ds = None

    ds = ogr.Open(fname)
    self.assertEqual(ds.GetMetadataDomainList(), [''])
    self.assertLen(ds.GetMetadata(), 1)
    self.assertEqual(ds.GetMetadataItem('foo'), 'bar')
    ds = None

    ds = ogr.Open(fname, 1)
    lyr = ds.CreateLayer(
        'test_with_md', options=['IDENTIFIER=ident', 'DESCRIPTION=desc']
    )
    lyr.SetMetadataItem('IDENTIFIER', 'ignored_because_of_lco')
    lyr.SetMetadataItem('DESCRIPTION', 'ignored_because_of_lco')
    lyr.SetMetadata({
        'IDENTIFIER': 'ignored_because_of_lco',
        'DESCRIPTION': 'ignored_because_of_lco',
    })
    ds = None

    ds = ogr.Open(fname)
    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE type = 'trigger' "
        "AND tbl_name IN ('gpkg_metadata', 'gpkg_metadata_reference')"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
    lyr = ds.GetLayer('test_with_md')
    self.assertEqual(lyr.GetMetadataItem('IDENTIFIER'), 'ident')
    self.assertEqual(lyr.GetMetadataItem('DESCRIPTION'), 'desc')

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer('test_with_md')
    self.assertEqual(
        lyr.GetMetadata(), {'IDENTIFIER': 'ident', 'DESCRIPTION': 'desc'}
    )
    lyr.SetMetadataItem('IDENTIFIER', 'another_ident')
    lyr.SetMetadataItem('DESCRIPTION', 'another_desc')
    ds = None

    gdal.Unlink(str(self.vsimem_dir / 'ogr_gpkg_19.gpkg.aux.xml'))

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer('test_with_md')
    self.assertEqual(
        lyr.GetMetadata(),
        {'IDENTIFIER': 'another_ident', 'DESCRIPTION': 'another_desc'},
    )
    lyr.SetMetadataItem('foo', 'bar')
    lyr.SetMetadataItem('bar', 'baz', 'another_domain')
    ds = None

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer('test_with_md')
    self.assertEqual(lyr.GetMetadataDomainList(), ['', 'another_domain'])
    ds = None

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer('test_with_md')
    self.assertEqual(
        lyr.GetMetadata(),
        {
            'IDENTIFIER': 'another_ident',
            'foo': 'bar',
            'DESCRIPTION': 'another_desc',
        },
    )
    self.assertEqual(lyr.GetMetadata('another_domain'), {'bar': 'baz'})
    lyr.SetMetadata(None)
    lyr.SetMetadata(None, 'another_domain')
    ds = None

    ds = ogr.Open(fname, 1)
    lyr = ds.GetLayer('test_with_md')
    self.assertEqual(
        lyr.GetMetadata(),
        {'IDENTIFIER': 'another_ident', 'DESCRIPTION': 'another_desc'},
    )
    self.assertEqual(lyr.GetMetadataDomainList(), [''])
    ds = None

    self.assertTrue(self.validate(fname))

  def test_ogr_gpkg_20(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_20.gpkg')
    ds = self.driver.CreateDataSource(fname)

    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'GEOGCS["my geogcs",DATUM["my datum",SPHEROID["my spheroid",1000,0]],'
        'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],'
        'AUTHORITY["my_org","4326"]]'
    )
    ds.CreateLayer('my_org_4326', srs=srs)

    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'GEOGCS["another geogcs",DATUM["another datum",'
        'SPHEROID["another spheroid",1000,0]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433]]'
    )
    ds.CreateLayer('without_org', srs=srs)
    ds = None

    ds = ogr.Open(fname)
    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_spatial_ref_sys WHERE srs_name='my geogcs' "
        "AND srs_id = 100000 AND organization='MY_ORG' "
        'AND organization_coordsys_id=4326 AND description is NULL'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_spatial_ref_sys WHERE srs_name='another geogcs' "
        "AND srs_id = 100001 AND organization='NONE' "
        'AND organization_coordsys_id=100001 AND description is NULL'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    self.assertIn(
        'my geogcs', ds.GetLayer('my_org_4326').GetSpatialRef().ExportToWkt()
    )
    self.assertIn(
        'another geogcs',
        ds.GetLayer('without_org').GetSpatialRef().ExportToWkt(),
    )
    ds = None

    self.assertTrue(self.validate(fname))

  def test_ogr_gpkg_20a(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_20a.gpkg')
    ds = self.driver.CreateDataSource(fname)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    lyr = ds.CreateLayer('foo4326', srs=srs)
    self.assertIsNotNone(lyr)
    ds.ExecuteSQL(
        "UPDATE gpkg_spatial_ref_sys SET definition='invalid', "
        "organization='', organization_coordsys_id = 0 WHERE srs_id = 4326"
    )
    ds = None

    with (
        gdal.config_option('OGR_SQLITE_PRAGMA', 'FOREIGN_KEYS=0'),
        _quiet_error_handler(),
    ):
      ds = ogr.Open(fname, 1)
      ds.ExecuteSQL('DELETE FROM gpkg_spatial_ref_sys WHERE srs_id = 4326')
      ds = None

    with (
        gdal.config_option('OGR_GPKG_FOREIGN_KEY_CHECK', 'NO'),
        _quiet_error_handler(),
    ):
      ds = ogr.Open(fname, 1)
      self.assertIsNotNone(ds)

  def test_ogr_gpkg_20b(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_20b.gpkg')
    with gdal.config_option('OGR_SQLITE_PRAGMA', 'FOREIGN_KEYS=0'):
      ds = self.driver.CreateDataSource(fname)
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(4326)
      self.assertIsNotNone(ds.CreateLayer('foo4326', srs=srs))

      ds.ExecuteSQL('DROP TABLE gpkg_spatial_ref_sys')
      ds.ExecuteSQL(
          'CREATE TABLE gpkg_spatial_ref_sys (srs_name TEXT, '
          'srs_id INTEGER, organization TEXT, '
          'organization_coordsys_id INTEGER, definition TEXT)'
      )
      ds.ExecuteSQL(
          'INSERT INTO gpkg_spatial_ref_sys '
          '(srs_name,srs_id,organization,organization_coordsys_id,'
          'definition) VALUES (NULL,4326,NULL,NULL,NULL)'
      )
      ds = None

    with (
        gdal.config_option('OGR_GPKG_FOREIGN_KEY_CHECK', 'NO'),
        _quiet_error_handler(),
    ):
      ds = ogr.Open(fname, 1)
      self.assertIsNotNone(ds)

  def test_ogr_gpkg_srs_non_duplication_custom_crs(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_20_srs_non_duplication.gpkg')

    ds = self.driver.CreateDataSource(fname)
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'GEOGCS["my custom geogcs",'
        'DATUM["my datum",'
        'SPHEROID["my spheroid",1000,0]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433]]'
    )
    lyr = ds.CreateLayer('test', srs=srs)
    self.assertIsNotNone(lyr)
    lyr = ds.CreateLayer('test2', srs=srs)
    self.assertIsNotNone(lyr)

    with ds.ExecuteSQL('SELECT * FROM gpkg_spatial_ref_sys') as sql_lyr:
      fc = sql_lyr.GetFeatureCount()
    self.assertEqual(fc, 4)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_spatial_ref_sys WHERE srs_name='my custom geogcs'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['srs_id'], 100000)
      self.assertEqual(f['organization'], 'NONE')
      self.assertEqual(f['organization_coordsys_id'], 100000)

    srs_3d = osr.SpatialReference()
    srs_3d.SetFromUserInput("""GEOGCRS["srs 3d",
    DATUM["some datum",
        ELLIPSOID["some ellipsoid",6378137,298.257223563,
            LENGTHUNIT["metre",1]]],
    PRIMEM["Greenwich",0,
        ANGLEUNIT["degree",0.0174532925199433]],
    CS[ellipsoidal,3],
        AXIS["geodetic latitude (Lat)",north,
            ORDER[1],
            ANGLEUNIT["degree",0.0174532925199433]],
        AXIS["geodetic longitude (Lon)",east,
            ORDER[2],
            ANGLEUNIT["degree",0.0174532925199433]],
        AXIS["ellipsoidal height (h)",up,
            ORDER[3],
            LENGTHUNIT["metre",1]]]""")
    lyr = ds.CreateLayer('test_3d', srs=srs_3d)
    self.assertIsNotNone(lyr)
    lyr = ds.CreateLayer('test_3d_bis', srs=srs_3d)
    self.assertIsNotNone(lyr)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_spatial_ref_sys WHERE srs_name='srs 3d'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    lyr = ds.CreateLayer('test3', srs=srs)
    self.assertIsNotNone(lyr)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_spatial_ref_sys WHERE srs_name='my custom geogcs'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    ds = None

  def test_ogr_gpkg_srs_non_consistent_with_official_definition(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_20_non_consistent.gpkg')

    ds = self.driver.CreateDataSource(fname)
    test_fake_4267 = osr.SpatialReference()
    test_fake_4267.SetFromUserInput("""GEOGCS["my geogcs 4267",
    DATUM["WGS_1984",
        SPHEROID["my spheroid",1000,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],
    AUTHORITY["EPSG","4267"]]""")
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr = ds.CreateLayer('test_fake_4267', srs=test_fake_4267)
    self.assertEqual(
        gdal.GetLastErrorMsg(),
        'Passed SRS uses EPSG:4267 identification, but its definition is not '
        'compatible with the official definition of the object. Registering it '
        'as a non-EPSG entry into the database.',
    )
    self.assertIsNotNone(lyr)

    test_fake_4326 = osr.SpatialReference()
    test_fake_4326.SetFromUserInput("""GEOGCS["my geogcs 4326",
    DATUM["WGS_1984",
        SPHEROID["my spheroid",1000,0]],PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],
    AUTHORITY["EPSG","4326"]]""")
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr = ds.CreateLayer('test_fake_4326', srs=test_fake_4326)
    self.assertEqual(
        gdal.GetLastErrorMsg(),
        'Passed SRS uses EPSG:4326 identification, but its definition is not '
        'compatible with the definition of that object already in the '
        'database. Registering it as a new entry into the database.',
    )
    self.assertIsNotNone(lyr)

    ds = None

    ds = ogr.Open(fname, update=1)
    lyr = ds.GetLayer('test_fake_4267')
    self.assertEqual(
        lyr.GetSpatialRef()
        .ExportToWkt()
        .replace(',AUTHORITY["EPSG","9122"]', ''),
        'GEOGCS["my geogcs 4267",DATUM["WGS_1984",SPHEROID["my'
        ' spheroid",1000,0]],'
        'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],'
        'AXIS["Latitude",NORTH],AXIS["Longitude",EAST],AUTHORITY["EPSG","4267"]]',
    )

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_spatial_ref_sys WHERE srs_name='my geogcs 4267'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['srs_id'], 100000)
      self.assertEqual(f['organization'], 'NONE')
      self.assertEqual(f['organization_coordsys_id'], 100000)

    lyr = ds.GetLayer('test_fake_4326')
    self.assertEqual(
        lyr.GetSpatialRef()
        .ExportToWkt()
        .replace(',AUTHORITY["EPSG","9122"]', ''),
        'GEOGCS["my geogcs 4326",DATUM["WGS_1984",SPHEROID["my'
        ' spheroid",1000,0]],'
        'PRIMEM["Greenwich",0],UNIT["degree",0.0174532925199433],'
        'AXIS["Latitude",NORTH],AXIS["Longitude",EAST],AUTHORITY["EPSG","4326"]]',
    )

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_spatial_ref_sys WHERE srs_name='my geogcs 4326'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['srs_id'], 100001)
      self.assertEqual(f['organization'], 'NONE')
      self.assertEqual(f['organization_coordsys_id'], 100001)

    with ds.ExecuteSQL('SELECT * FROM gpkg_spatial_ref_sys') as sql_lyr:
      fc_before = sql_lyr.GetFeatureCount()

    gdal.ErrorReset()
    lyr = ds.CreateLayer('test_fake_4267_bis', srs=test_fake_4267)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

    with ds.ExecuteSQL('SELECT * FROM gpkg_spatial_ref_sys') as sql_lyr:
      fc_after = sql_lyr.GetFeatureCount()

    self.assertEqual(fc_before, fc_after)
    ds = None

  def test_ogr_gpkg_write_srs_undefined_geographic(self) -> None:
    fname = str(self.tmp_dir / 'ogr_gpkg_srs_undefined_geographic.gpkg')

    gpkg_ds = self.driver.CreateDataSource(fname)
    self.assertIsNotNone(gpkg_ds)

    with gpkg_ds.ExecuteSQL(
        'SELECT COUNT(*) FROM gpkg_spatial_ref_sys'
    ) as sql_lyr:
      gpkg_spatial_ref_sys_total = sql_lyr.GetNextFeature().GetField(0)
      self.assertEqual(gpkg_spatial_ref_sys_total, 3)

    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'GEOGCS["Undefined geographic SRS",DATUM["unknown",'
        'SPHEROID["unknown",6378137,298.257223563]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AXIS["Latitude",NORTH],AXIS["Longitude",EAST]]'
    )
    lyr = gpkg_ds.CreateLayer(
        'srs_test_geographic_layer', geom_type=ogr.wkbPoint, srs=srs
    )
    srs_wkt = lyr.GetSpatialRef().ExportToWkt()
    self.assertIn('Undefined geographic SRS', srs_wkt)
    self.assertTrue(lyr.GetSpatialRef().IsGeographic())

    gpkg_ds = None
    gpkg_ds = ogr.Open(fname)

    with gpkg_ds.ExecuteSQL(
        'SELECT COUNT(*) FROM gpkg_spatial_ref_sys'
    ) as sql_lyr:
      self.assertEqual(
          gpkg_spatial_ref_sys_total, sql_lyr.GetNextFeature().GetField(0)
      )

    lyr = gpkg_ds.GetLayer(0)
    srs_wkt = lyr.GetSpatialRef().ExportToWkt()
    self.assertIn('Undefined geographic SRS', srs_wkt)
    self.assertTrue(lyr.GetSpatialRef().IsGeographic())

    gpkg_ds = None

  def test_ogr_gpkg_write_srs_undefined_Cartesian(self) -> None:
    fname = str(self.tmp_dir / 'ogr_gpkg_srs_Cartesian.gpkg')

    gpkg_ds = self.driver.CreateDataSource(fname)
    self.assertIsNotNone(gpkg_ds)

    with gpkg_ds.ExecuteSQL(
        'SELECT COUNT(*) FROM gpkg_spatial_ref_sys'
    ) as sql_lyr:
      gpkg_spatial_ref_sys_total = sql_lyr.GetNextFeature().GetField(0)
      self.assertEqual(gpkg_spatial_ref_sys_total, 3)

    srs = osr.SpatialReference()
    srs.SetFromUserInput('LOCAL_CS["Undefined Cartesian SRS"]')
    lyr = gpkg_ds.CreateLayer(
        'srs_test_Cartesian_layer', geom_type=ogr.wkbPoint, srs=srs
    )
    srs_wkt = lyr.GetSpatialRef().ExportToWkt()
    self.assertIn('Undefined Cartesian SRS', srs_wkt)
    self.assertTrue(lyr.GetSpatialRef().IsLocal())

    gpkg_ds = None
    gpkg_ds = ogr.Open(fname)

    with gpkg_ds.ExecuteSQL(
        'SELECT COUNT(*) FROM gpkg_spatial_ref_sys'
    ) as sql_lyr:
      self.assertEqual(
          gpkg_spatial_ref_sys_total, sql_lyr.GetNextFeature().GetField(0)
      )

    lyr = gpkg_ds.GetLayer(0)
    srs_wkt = lyr.GetSpatialRef().ExportToWkt()
    self.assertIn('Undefined Cartesian SRS', srs_wkt)
    self.assertTrue(lyr.GetSpatialRef().IsLocal())

    gpkg_ds = None

  @parameterized.parameters(True, False)
  def test_ogr_gpkg_write_no_srs(self, crs_wkt_extension: bool) -> None:
    with _enable_exceptions():
      fname = str(self.tmp_dir / 'test_ogr_gpkg_write_no_srs.gpkg')

      options = []
      if crs_wkt_extension:
        options += ['CRS_WKT_EXTENSION=YES']
      gpkg_ds = self.driver.CreateDataSource(fname, options=options)
      self.assertIsNotNone(gpkg_ds)

      lyr = gpkg_ds.CreateLayer('layer1', geom_type=ogr.wkbPoint, srs=None)
      self.assertIsNone(lyr.GetSpatialRef())

      lyr = gpkg_ds.CreateLayer('layer2', geom_type=ogr.wkbPoint, srs=None)
      self.assertIsNone(lyr.GetSpatialRef())

      srs = osr.SpatialReference()
      srs.SetFromUserInput(
          'LOCAL_CS["Undefined SRS",LOCAL_DATUM["unknown",32767],'
          'UNIT["unknown",0],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
      )
      gpkg_ds.CreateLayer('layer3', geom_type=ogr.wkbPoint, srs=srs)

      gpkg_ds = None
      gpkg_ds = ogr.Open(fname)

      with gpkg_ds.ExecuteSQL(
          'SELECT COUNT(*) FROM gpkg_spatial_ref_sys'
      ) as sql_lyr:
        self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 4)

      with gpkg_ds.ExecuteSQL(
          'SELECT * FROM gpkg_spatial_ref_sys WHERE srs_id = 99999'
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        self.assertEqual(f['srs_name'], 'Undefined SRS')
        self.assertEqual(f['organization'], 'GDAL')
        self.assertEqual(f['organization_coordsys_id'], 99999)
        self.assertEqual(
            f['definition'],
            'LOCAL_CS["Undefined SRS",LOCAL_DATUM["unknown",32767],'
            'UNIT["unknown",0],AXIS["Easting",EAST],AXIS["Northing",NORTH]]',
        )
        if crs_wkt_extension:
          self.assertEqual(
              f['definition_12_063'],
              'ENGCRS["Undefined SRS",EDATUM["unknown"],CS[Cartesian,2],'
              'AXIS["easting",east,ORDER[1],LENGTHUNIT["unknown",0]],'
              'AXIS["northing",north,ORDER[2],LENGTHUNIT["unknown",0]]]',
          )
        self.assertEqual(
            f['description'], 'Custom undefined coordinate reference system'
        )

      for lyr in gpkg_ds:
        self.assertIsNone(lyr.GetSpatialRef())

      gpkg_ds = None

  @parameterized.parameters(
      (-1, 'ENGCRS["Undefined Cartesian SRS",'),
      (0, 'GEOGCRS["Undefined geographic SRS",'),
      (4326, 'GEOGCRS["WGS 84",'),
      (4258, 'GEOGCRS["ETRS89",'),
      (1, None),
      (99999, None),
      (123456, None),
  )
  def test_ogr_gpkg_SRID_creation_option(
      self, srid: int, expected_wkt: str | None
  ) -> None:
    with _enable_exceptions():
      fname = str(self.tmp_dir / 'test_ogr_gpkg_SRID_creation_option.gpkg')

      gpkg_ds = self.driver.CreateDataSource(fname)
      self.assertIsNotNone(gpkg_ds)

      if srid in (1, 123456):
        with _quiet_error_handler():
          lyr = gpkg_ds.CreateLayer(
              'layer1', geom_type=ogr.wkbPoint, options=[f'SRID={srid}']
          )
        self.assertEqual(
            gdal.GetLastErrorMsg(),
            f'No entry in gpkg_spatial_ref_sys matching SRID={srid}',
        )
      else:
        lyr = gpkg_ds.CreateLayer(
            'layer1', geom_type=ogr.wkbPoint, options=[f'SRID={srid}']
        )
      got_srs = lyr.GetSpatialRef()
      if expected_wkt is None:
        self.assertIsNone(got_srs)
      else:
        self.assertTrue(
            got_srs.ExportToWkt(['FORMAT=WKT2_2019']).startswith(expected_wkt)
        )

      gpkg_ds = None
      gpkg_ds = ogr.Open(fname)

      if srid in (1, 123456):
        with _quiet_error_handler():
          lyr = gpkg_ds.GetLayer(0)
          got_srs = lyr.GetSpatialRef()
        self.assertEqual(
            gdal.GetLastErrorMsg(),
            f"unable to read srs_id '{srid}' from gpkg_spatial_ref_sys",
        )
      else:
        lyr = gpkg_ds.GetLayer(0)
        got_srs = lyr.GetSpatialRef()
      if expected_wkt is None:
        self.assertIsNone(got_srs)
      else:
        self.assertTrue(
            got_srs.ExportToWkt(['FORMAT=WKT2_2019']).startswith(expected_wkt)
        )

      gpkg_ds = None

  def test_ogr_gpkg_21(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_21.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test')
    field_defn = ogr.FieldDefn('str', ogr.OFTString)
    field_defn.SetWidth(2)
    lyr.CreateField(field_defn)
    ds = None

    ds = ogr.Open(fname, update=1)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetWidth(), 2)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField(0, 'ab')
    gdal.ErrorReset()
    lyr.CreateFeature(f)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFieldBinaryFromHexString(0, '41E9')
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.CreateFeature(f)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField(0, 'abc')
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.CreateFeature(f)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    f = lyr.GetFeature(f.GetFID())
    self.assertEqual(f.GetField(0), 'abc')

  def test_ogr_gpkg_21a(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_21a.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test', options=['TRUNCATE_FIELDS=YES'])
    field_defn = ogr.FieldDefn('str', ogr.OFTString)
    field_defn.SetWidth(2)
    lyr.CreateField(field_defn)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFieldBinaryFromHexString(0, '41E9')
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.CreateFeature(f)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    f = lyr.GetFeature(f.GetFID())
    self.assertEqual(f.GetField(0), 'A_')

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField(0, 'abc')
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.CreateFeature(f)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    f = lyr.GetFeature(f.GetFID())
    self.assertEqual(f.GetField(0), 'ab')

  def test_ogr_gpkg_table_in_gpkg_content_but_missing(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_table_in_gpkg_content_but_missing.gpkg'
    )

    ds = self.driver.CreateDataSource(filename)
    ds.CreateLayer('valid')
    ds.ExecuteSQL(
        "INSERT INTO gpkg_contents VALUES('non_existent','attributes',"
        "'non_existent','','2022-05-18T17:24:49.837Z',NULL,NULL,NULL,NULL,-1);"
    )
    ds = None
    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(filename)
    self.assertIn('non_existent', gdal.GetLastErrorMsg())
    self.assertEqual(ds.GetLayerCount(), 1)
    ds = None

  def test_ogr_gpkg_22(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_22.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test')
    field_defn = ogr.FieldDefn('foo', ogr.OFTString)
    lyr.CreateField(field_defn)

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField('foo', 'bar')
    feat.SetFID(1234567890123)
    lyr.CreateFeature(feat)
    feat = None
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayerByName('test')
    self.assertIsNotNone(lyr.GetMetadataItem(ogr.OLMD_FID64))
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 1234567890123)

  def test_ogr_gpkg_create_with_fid_0(self) -> None:
    fname = str(self.vsimem_dir / 'test_ogr_gpkg_create_with_fid_0.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test')

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetFID(0)
    lyr.CreateFeature(feat)
    feat = None
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayerByName('test')
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 0)

  def test_ogr_gpkg_23(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_23.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)
    field_defn = ogr.FieldDefn('field_not_nullable', ogr.OFTString)
    field_defn.SetNullable(0)
    lyr.CreateField(field_defn)
    field_defn = ogr.FieldDefn('field_nullable', ogr.OFTString)
    lyr.CreateField(field_defn)
    field_defn = ogr.GeomFieldDefn('geomfield_not_nullable', ogr.wkbPoint)
    field_defn.SetNullable(0)
    lyr.CreateGeomField(field_defn)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('field_not_nullable', 'not_null')
    f.SetGeomFieldDirectly(
        'geomfield_not_nullable', ogr.CreateGeometryFromWkt('POINT(0 0)')
    )
    lyr.CreateFeature(f)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('field_not_nullable', 'not_null')
    with _quiet_error_handler():
      ret = lyr.CreateFeature(f)
    self.assertNotEqual(ret, 0)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    with _quiet_error_handler():
      ret = lyr.CreateFeature(f)
    self.assertNotEqual(ret, 0)
    f = None

    lyr = ds.CreateLayer(
        'test2', geom_type=ogr.wkbPoint, options=['SPATIAL_INDEX=NO']
    )

    with _quiet_error_handler():
      ret = lyr.CreateGeomField(ogr.GeomFieldDefn('foo', ogr.wkbPoint))
    self.assertNotEqual(ret, 0)

    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    f = None

    lyr = ds.CreateLayer('test3', geom_type=ogr.wkbNone)

    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    f = None

    field_defn = ogr.FieldDefn('field_not_nullable', ogr.OFTString)
    field_defn.SetNullable(0)
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_nullable', ogr.OFTString)
    lyr.CreateField(field_defn)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_contents WHERE data_type = 'features'"
    ) as sql_lyr:
      fc = sql_lyr.GetFeatureCount()
    self.assertEqual(fc, 2)

    with ds.ExecuteSQL(
        "SELECT 1 FROM sqlite_master WHERE name='gpkg_extensions'"
    ) as sql_lyr:
      has_gpkg_extensions = sql_lyr.GetFeatureCount() == 1
    self.assertFalse(has_gpkg_extensions)

    with ds.ExecuteSQL('SELECT * FROM gpkg_geometry_columns') as sql_lyr:
      fc = sql_lyr.GetFeatureCount()
    self.assertEqual(fc, 2)

    field_defn = ogr.GeomFieldDefn('geomfield_not_nullable', ogr.wkbPoint)
    field_defn.SetNullable(0)
    lyr.CreateGeomField(field_defn)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_contents WHERE data_type = 'features'"
    ) as sql_lyr:
      fc = sql_lyr.GetFeatureCount()
    self.assertEqual(fc, 3)

    with ds.ExecuteSQL(
        "SELECT 1 FROM sqlite_master WHERE name='gpkg_extensions'"
    ) as sql_lyr:
      has_gpkg_extensions = sql_lyr.GetFeatureCount() == 1
    self.assertFalse(has_gpkg_extensions)

    with ds.ExecuteSQL('SELECT * FROM gpkg_geometry_columns') as sql_lyr:
      fc = sql_lyr.GetFeatureCount()
    self.assertEqual(fc, 3)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('field_not_nullable', 'not_null')
    f.SetGeomFieldDirectly(
        'geomfield_not_nullable', ogr.CreateGeometryFromWkt('POINT(0 0)')
    )
    lyr.CreateFeature(f)
    f = None

    lyr = ds.CreateLayer(
        'test4', geom_type=ogr.wkbPoint, options=['GEOMETRY_NULLABLE=NO']
    )
    self.assertEqual(lyr.GetLayerDefn().GetGeomFieldDefn(0).IsNullable(), 0)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    lyr.CreateFeature(f)
    f = None

    ds.CreateLayer('test5', geom_type=ogr.wkbNone)

    ds = None

    ds = ogr.Open(fname)

    lyr = ds.GetLayerByName('test5')
    field_defn = ogr.GeomFieldDefn('', ogr.wkbPoint)
    with _quiet_error_handler():
      self.assertNotEqual(lyr.CreateGeomField(field_defn), 0)

    lyr = ds.GetLayerByName('test')
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_not_nullable'))
        .IsNullable(),
        0,
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_nullable'))
        .IsNullable(),
        1,
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetGeomFieldDefn(
            lyr.GetLayerDefn().GetGeomFieldIndex('geomfield_not_nullable')
        )
        .IsNullable(),
        0,
    )

    lyr = ds.GetLayerByName('test2')
    self.assertEqual(lyr.GetLayerDefn().GetGeomFieldDefn(0).IsNullable(), 1)

    lyr = ds.GetLayerByName('test3')
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_not_nullable'))
        .IsNullable(),
        0,
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_nullable'))
        .IsNullable(),
        1,
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetGeomFieldDefn(
            lyr.GetLayerDefn().GetGeomFieldIndex('geomfield_not_nullable')
        )
        .IsNullable(),
        0,
    )

    lyr = ds.GetLayerByName('test4')
    self.assertEqual(lyr.GetLayerDefn().GetGeomFieldDefn(0).IsNullable(), 0)

    ds = None

  def test_ogr_gpkg_unique(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_unique.gpkg')

    ds = ogr.GetDriverByName('GPKG').CreateDataSource(fname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)

    field_defn = ogr.FieldDefn('field_default', ogr.OFTString)
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_no_unique', ogr.OFTString)
    field_defn.SetUnique(0)
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_unique', ogr.OFTString)
    field_defn.SetUnique(1)
    lyr.CreateField(field_defn)

    layer_definition = lyr.GetLayerDefn()
    fld_def = layer_definition.GetFieldDefn(0)
    self.assertFalse(fld_def.IsUnique())
    fld_def = layer_definition.GetFieldDefn(1)
    self.assertFalse(fld_def.IsUnique())
    fld_def = layer_definition.GetFieldDefn(2)
    self.assertTrue(fld_def.IsUnique())

    f = ogr.Feature(layer_definition)
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    f = None

    field_defn = ogr.FieldDefn('field_unique_failure', ogr.OFTString)
    field_defn.SetUnique(1)
    with _quiet_error_handler():
      self.assertEqual(lyr.CreateField(field_defn), ogr.OGRERR_FAILURE)

    sql = (
        (
            'CREATE TABLE IF NOT EXISTS "test2" ( "fid" INTEGER PRIMARY KEY'
            ' AUTOINCREMENT NOT NULL, "field_default" TEXT, "field_no_unique"'
            ' TEXT, "field_unique" TEXT UNIQUE,`field unique2` TEXT'
            ' UNIQUE,field_unique3 TEXT UNIQUE, FIELD_UNIQUE_INDEX TEXT, `field'
            ' unique index2` TEXT, "field_unique_index3" TEXT, NOT_UNIQUE'
            ' TEXT);'
        ),
        'CREATE UNIQUE INDEX test2_unique_idx ON test2(field_unique_index);',
        (
            'CREATE UNIQUE INDEX test2_unique_idx2 ON test2(`field unique'
            ' index2`);'
        ),
        (
            'CREATE UNIQUE INDEX test2_unique_idx3 ON '
            'test2("field_unique_index3");'
        ),
        (
            "INSERT INTO gpkg_contents VALUES('test2','attributes','test2','',"
            "'2020-05-27T12:27:30.136Z',NULL,NULL,NULL,NULL,0);"
        ),
        "INSERT INTO gpkg_ogr_contents VALUES('test2',NULL);",
    )

    for s in sql:
      ds.ExecuteSQL(s)

    ds = None

    ds = ogr.Open(fname)

    lyr = ds.GetLayerByName('test')
    layer_definition = lyr.GetLayerDefn()
    self.assertFalse(layer_definition.GetFieldDefn(0).IsUnique())
    self.assertFalse(layer_definition.GetFieldDefn(1).IsUnique())
    self.assertTrue(layer_definition.GetFieldDefn(2).IsUnique())

    lyr = ds.GetLayerByName('test2')
    layer_definition = lyr.GetLayerDefn()
    self.assertFalse(layer_definition.GetFieldDefn(0).IsUnique())
    self.assertFalse(layer_definition.GetFieldDefn(1).IsUnique())
    self.assertTrue(layer_definition.GetFieldDefn(2).IsUnique())
    self.assertTrue(layer_definition.GetFieldDefn(3).IsUnique())
    self.assertTrue(layer_definition.GetFieldDefn(4).IsUnique())
    self.assertTrue(layer_definition.GetFieldDefn(5).IsUnique())
    self.assertTrue(layer_definition.GetFieldDefn(6).IsUnique())
    self.assertTrue(layer_definition.GetFieldDefn(7).IsUnique())
    self.assertFalse(layer_definition.GetFieldDefn(8).IsUnique())

    ds = None

  def test_ogr_gpkg_unique_many_layers(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_unique_many_layers.gpkg')
    ds = ogr.GetDriverByName('GPKG').CreateDataSource(filename)
    threshold = 10
    for i in range(threshold + 1):
      lyr_name = 'test' + str(i)
      ds.ExecuteSQL(
          f'CREATE TABLE IF NOT EXISTS "{lyr_name}" ( "fid" INTEGER PRIMARY '
          'KEY AUTOINCREMENT NOT NULL, other_field TEXT, "field_unique" TEXT '
          'UNIQUE);'
      )
      ds.ExecuteSQL(
          f'CREATE UNIQUE INDEX {lyr_name}_unique_idx ON '
          f'{lyr_name}(other_field);'
      )
    ds = None

    ds = ogr.Open(filename)
    for i in range(threshold + 1):
      lyr = ds.GetLayerByName('test' + str(i))
      lyr_defn = lyr.GetLayerDefn()
      self.assertTrue(lyr_defn.GetFieldDefn(0).IsUnique())
      self.assertTrue(lyr_defn.GetFieldDefn(1).IsUnique())
    ds = None

  def test_ogr_gpkg_24(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_24.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)

    field_defn = ogr.FieldDefn('field_string', ogr.OFTString)
    field_defn.SetDefault("'a''b'")
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_int', ogr.OFTInteger)
    field_defn.SetDefault('123')
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_real', ogr.OFTReal)
    field_defn.SetDefault('1.23')
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_nodefault', ogr.OFTInteger)
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_datetime', ogr.OFTDateTime)
    field_defn.SetDefault('CURRENT_TIMESTAMP')
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_datetime2', ogr.OFTDateTime)
    field_defn.SetDefault("'2015/06/30 12:34:56'")
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_datetime3', ogr.OFTDateTime)
    field_defn.SetDefault("(strftime('%Y-%m-%dT%H:%M:%fZ','now'))")
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_datetime4', ogr.OFTDateTime)
    field_defn.SetDefault("'2015/06/30 12:34:56.123'")
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_date', ogr.OFTDate)
    field_defn.SetDefault('CURRENT_DATE')
    lyr.CreateField(field_defn)

    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    f = None

    field_defn = ogr.FieldDefn('field_datetime5', ogr.OFTDateTime)
    field_defn.SetDefault("'2016/06/30 12:34:56.123'")
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_datetime6', ogr.OFTDateTime)
    field_defn.SetDefault("'2016/06/30 12:34:56'")
    lyr.CreateField(field_defn)

    field_defn = ogr.FieldDefn('field_string2', ogr.OFTString)
    field_defn.SetDefault("'X'")
    lyr.CreateField(field_defn)

    ds = None

    ds = ogr.Open(fname, update=1)
    lyr = ds.GetLayerByName('test')
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_string'))
        .GetDefault(),
        "'a''b'",
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_int'))
        .GetDefault(),
        '123',
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_real'))
        .GetDefault(),
        '1.23',
    )
    self.assertIsNone(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_nodefault'))
        .GetDefault()
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_datetime'))
        .GetDefault(),
        'CURRENT_TIMESTAMP',
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_datetime2'))
        .GetDefault(),
        "'2015/06/30 12:34:56'",
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_datetime3'))
        .GetDefault(),
        'CURRENT_TIMESTAMP',
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_datetime4'))
        .GetDefault(),
        "'2015/06/30 12:34:56.123'",
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(lyr.GetLayerDefn().GetFieldIndex('field_date'))
        .GetDefault(),
        'CURRENT_DATE',
    )
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('field_string'), "a'b")
    self.assertEqual(f.GetField('field_int'), 123)
    self.assertEqual(f.GetField('field_real'), 1.23)
    self.assertTrue(f.IsFieldNull('field_nodefault'))
    self.assertTrue(f.IsFieldSet('field_datetime'))
    self.assertEqual(f.GetField('field_datetime2'), '2015/06/30 12:34:56+00')
    self.assertEqual(
        f.GetField('field_datetime4'), '2015/06/30 12:34:56.123+00'
    )
    self.assertTrue(f.IsFieldSet('field_datetime3'))
    self.assertTrue(f.IsFieldSet('field_date'))
    self.assertEqual(
        f.GetField('field_datetime5'), '2016/06/30 12:34:56.123+00'
    )
    self.assertEqual(f.GetField('field_datetime6'), '2016/06/30 12:34:56+00')
    self.assertEqual(f.GetField('field_string2'), 'X')

    ds = None

  def test_ogr_gpkg_25(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_25.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone, options=['FID=myfid'])

    lyr.CreateField(ogr.FieldDefn('str', ogr.OFTString))
    with _quiet_error_handler():
      ret = lyr.CreateField(ogr.FieldDefn('myfid', ogr.OFTString))
    self.assertNotEqual(ret, 0)

    ret = lyr.CreateField(ogr.FieldDefn('myfid', ogr.OFTInteger))
    self.assertEqual(ret, 0)
    lyr.CreateField(ogr.FieldDefn('str2', ogr.OFTString))

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField('str', 'first string')
    feat.SetField('myfid', 10)
    feat.SetField('str2', 'second string')
    ret = lyr.CreateFeature(feat)
    self.assertEqual(ret, 0)
    self.assertEqual(feat.GetFID(), 10)

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField('str2', 'second string')
    ret = lyr.CreateFeature(feat)
    self.assertEqual(ret, 0)
    self.assertGreaterEqual(feat.GetFID(), 0)
    self.assertEqual(feat.GetField('myfid'), feat.GetFID())

    feat.SetField('str', 'foo')
    ret = lyr.SetFeature(feat)
    self.assertEqual(ret, 0)

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetFID(1)
    feat.SetField('myfid', 10)
    with _quiet_error_handler():
      ret = lyr.CreateFeature(feat)
    self.assertNotEqual(ret, 0)

    with _quiet_error_handler():
      ret = lyr.SetFeature(feat)
    self.assertNotEqual(ret, 0)

    feat.UnsetField('myfid')
    with _quiet_error_handler():
      ret = lyr.SetFeature(feat)
    self.assertNotEqual(ret, 0)

    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f.GetField('str'), 'first string')
    self.assertEqual(f.GetField('str2'), 'second string')
    self.assertEqual(f.GetField('myfid'), 10)

    f = lyr.GetFeature(f.GetFID())
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f.GetField('str'), 'first string')
    self.assertEqual(f.GetField('str2'), 'second string')
    self.assertEqual(f.GetField('myfid'), 10)
    f = None
    ds = None

  def test_ogr_gpkg_26(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_26.gpkg')

    ds = self.driver.CreateDataSource(fname)
    self.assertEqual(ds.TestCapability(ogr.ODsCTransactions), 1)

    ret = ds.StartTransaction()
    self.assertEqual(ret, 0)
    with _quiet_error_handler():
      ret = ds.StartTransaction()
    self.assertNotEqual(ret, 0)

    lyr = ds.CreateLayer('test')
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    ret = ds.RollbackTransaction()
    self.assertEqual(ret, 0)
    with _quiet_error_handler():
      ret = ds.RollbackTransaction()
    self.assertNotEqual(ret, 0)
    ds = None

    ds = ogr.Open(fname, update=1)
    self.assertEqual(ds.GetLayerCount(), 0)
    ret = ds.StartTransaction()
    self.assertEqual(ret, 0)
    with _quiet_error_handler():
      ret = ds.StartTransaction()
    self.assertNotEqual(ret, 0)

    lyr = ds.CreateLayer('test')
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    ret = ds.CommitTransaction()
    self.assertEqual(ret, 0)
    with _quiet_error_handler():
      ret = ds.CommitTransaction()
    self.assertNotEqual(ret, 0)
    ds = None

    ds = ogr.Open(fname, update=1)
    self.assertEqual(ds.GetLayerCount(), 1)
    lyr = ds.GetLayerByName('test')

    ds.StartTransaction()
    lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertIsNotNone(f)
    self.assertEqual(lyr.GetFeatureCount(), 1)
    ds.RollbackTransaction()
    self.assertEqual(lyr.GetFeatureCount(), 0)

    ds.StartTransaction()
    lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
    lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertIsNotNone(f)
    self.assertEqual(f.GetFID(), 1)
    ds.CommitTransaction()
    f = lyr.GetNextFeature()
    self.assertIsNotNone(f)
    self.assertEqual(f.GetFID(), 2)
    self.assertEqual(lyr.GetFeatureCount(), 2)

    ds.StartTransaction()
    lyr = ds.CreateLayer('test2', geom_type=ogr.wkbPoint)
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    ret = lyr.CreateFeature(f)
    ds.CommitTransaction()
    self.assertEqual(ret, 0)

    ds.StartTransaction()
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    ret = lyr.CreateFeature(f)
    ds.CommitTransaction()
    self.assertEqual(ret, 0)

    ds = None

  def test_ogr_gpkg_27(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_27.gpkg')

    ds = self.driver.CreateDataSource(fname)
    with _quiet_error_handler():
      sql_lyr = ds.ExecuteSQL('SELECT GeomFromGPB(null)')
    if sql_lyr is None:
      ds = None
      self.skipTest('SpatiaLite not available')
    ds.ReleaseResultSet(sql_lyr)

    lyr = ds.CreateLayer('test')
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (2 49)'))
    lyr.CreateFeature(f)
    with ds.ExecuteSQL('SELECT GeomFromGPB(geom) FROM test') as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'POINT (2 49)')

    ds = None

  def test_ogr_gpkg_28(self) -> None:
    src_ds = gdal.OpenEx(ogr_util.GetTestFilePath('shape/poly/poly.shp'))
    ds = gdal.VectorTranslate(
        str(self.vsimem_dir / 'ogr_gpkg_28.gpkg'),
        src_ds,
        format='GPKG',
        dstSRS='EPSG:4326',
    )
    self.assertIn('1984', str(ds.GetLayer(0).GetSpatialRef()))
    ds = None

  def test_ogr_gpkg_29(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_29.gpkg')

    ds = self.driver.CreateDataSource(fname)
    self.assertEqual(ds.TestCapability(ogr.ODsCMeasuredGeometries), 1)
    lyr = ds.CreateLayer('pointm', geom_type=ogr.wkbPointM)
    self.assertEqual(lyr.TestCapability(ogr.OLCMeasuredGeometries), 1)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT M (1 2 3)'))
    lyr.CreateFeature(f)
    lyr = ds.CreateLayer('pointzm', geom_type=ogr.wkbPointZM)
    self.assertEqual(lyr.TestCapability(ogr.OLCMeasuredGeometries), 1)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT ZM (1 2 3 4)'))
    lyr.CreateFeature(f)
    ds = None

    self.assertTrue(self.validate(fname))

    ds = ogr.Open(fname, update=1)
    lyr = ds.GetLayerByName('pointm')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPointM)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), 'POINT M (1 2 3)')

    ds.ExecuteSQL(
        'UPDATE pointm SET geom = '
        "x'4750000700000000000000000000F03F000000000000F03F0000000000000040"
        '00000000000000400000000000000840000000000000084001D1070000000000000000'
        "F03F00000000000000400000000000000840'"
    )

    lyr = ds.GetLayerByName('pointzm')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPointZM)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), 'POINT ZM (1 2 3 4)')

    ds.ExecuteSQL(
        'UPDATE pointzm SET geom = '
        "x'4750000900000000000000000000F03F000000000000F03F0000000000000040"
        '0000000000000040000000000000084000000000000008400000000000001040000000'
        '000000104001B90B0000000000000000F03F0000000000000040000000000000084000'
        "00000000001040'"
    )
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayerByName('pointm')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPointM)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), 'POINT M (1 2 3)')
    lyr = ds.GetLayerByName('pointzm')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPointZM)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetGeometryRef().ExportToIsoWkt(), 'POINT ZM (1 2 3 4)')
    ds = None

  def test_ogr_gpkg_30(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_30.geopkg')

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = self.driver.CreateDataSource(fname)
    self.assertIsNotNone(ds)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    ds = None

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(fname, update=1)
    self.assertIsNotNone(ds)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    ds = None

    with _quiet_error_handler():
      self.driver.DeleteDataSource(fname)

  def test_ogr_gpkg_31(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_31.gpkg')

    ds = self.driver.CreateDataSource(fname)
    lyr = ds.CreateLayer('curve', geom_type=ogr.wkbCurve)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('LINESTRING (1 2,3 4)'))
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('COMPOUNDCURVE ((1 2,3 4))')
    )
    lyr.CreateFeature(f)
    lyr = ds.CreateLayer('surface', geom_type=ogr.wkbSurface)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON ((0 0,0 1,1 1,0 0))')
    )
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('CURVEPOLYGON ((0 0,0 1,1 1,0 0))')
    )
    lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayerByName('curve')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbCurve)
    lyr = ds.GetLayerByName('surface')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbSurface)
    ds = None

    self.assertTrue(self.validate(fname))

  def test_ogr_gpkg_32(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_32.gpkg')

    ds = self.driver.CreateDataSource(fname)
    ds.CreateLayer(
        'aspatial',
        geom_type=ogr.wkbNone,
        options=['ASPATIAL_VARIANT=NOT_REGISTERED'],
    )
    ds = None

    ds = ogr.Open(fname)
    self.assertEqual(ds.GetLayerCount(), 1)
    with ds.ExecuteSQL('SELECT * FROM gpkg_contents') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
    with ds.ExecuteSQL('SELECT * FROM gpkg_geometry_columns') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'gpkg_extensions'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
    ds = None

    self.assertTrue(self.validate(fname))

  def test_ogr_gpkg_33(self) -> None:
    fname = str(self.vsimem_dir / 'ogr_gpkg_33.gpkg')

    with gdal.config_option('OGR_CURRENT_DATE', '2000-01-01T:00:00:00.000Z'):
      ds = self.driver.CreateDataSource(fname)
      ds.CreateLayer('test', geom_type=ogr.wkbNone)
      ds = None

    ds = ogr.Open(fname)
    with ds.ExecuteSQL(
        'SELECT * FROM gpkg_contents WHERE '
        "last_change = '2000-01-01T:00:00:00.000Z'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    ds = None

  def test_ogr_gpkg_34(self) -> None:
    layer_name = """weird'layer"name"""

    dbname = str(self.vsimem_dir / 'ogr_gpkg_34.gpkg')
    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer(layer_name, geom_type=ogr.wkbCurvePolygon)
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('CURVEPOLYGON ((0 0,0 1,1 1,0 0))')
    )
    lyr.CreateFeature(f)
    lyr.SetMetadataItem('FOO', 'BAR')
    ds.ExecuteSQL("""CREATE TABLE gpkg_data_columns (
  table_name TEXT NOT NULL,
  column_name TEXT NOT NULL,
  name TEXT,
  title TEXT,
  description TEXT,
  mime_type TEXT,
  constraint_name TEXT,
  CONSTRAINT pk_gdc PRIMARY KEY (table_name, column_name),
  CONSTRAINT gdc_tn UNIQUE (table_name, name)
)""")
    ds.ExecuteSQL(
        "INSERT INTO gpkg_data_columns VALUES('weird''layer\"name', 'foo', "
        "'foo_constraints', NULL, NULL, NULL, NULL)"
    )

    ds.ExecuteSQL(
        'CREATE TABLE "layer_styles" ( "id" INTEGER PRIMARY KEY AUTOINCREMENT '
        'NOT NULL, "f_table_catalog" TEXT(256), "f_table_schema" TEXT(256), '
        '"f_table_name" TEXT(256), "f_geometry_column" TEXT(256), '
        '"styleName" TEXT(30), "styleQML" TEXT, "styleSLD" TEXT, '
        '"useAsDefault" BOOLEAN, "description" TEXT, "owner" TEXT(30), '
        '"ui" TEXT(30), "update_time" DATETIME DEFAULT '
        "(strftime('%Y-%m-%dT%H:%M:%fZ','now')))"
    )
    ds.ExecuteSQL(
        "INSERT INTO layer_styles VALUES(1, NULL, NULL, 'weird''layer\"name', "
        "'geom', 'styleName', 'styleQML', 'styleSLD', 0, 'description', "
        "'owner', 'ui', NULL)"
    )
    ds = None

    f = gdal.VSIFOpenL(dbname, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)
    self.assertIn(layer_name, content)

    ds = ogr.Open(dbname, update=1)
    new_layer_name = """weird2'layer"name"""
    gdal.ErrorReset()
    with _quiet_error_handler():
      ds.ExecuteSQL('ALTER TABLE "weird\'layer""name" RENAME TO gpkg_contents')
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    gdal.ErrorReset()
    ds.ExecuteSQL(
        'ALTER TABLE "weird\'layer""name" RENAME TO "weird2\'layer""name"'
    )
    ds = None

    ds = ogr.Open(dbname, update=1)
    gdal.ErrorReset()
    ds.ExecuteSQL(
        'ALTER TABLE "weird2\'layer""name" RENAME COLUMN "foo" TO "bar"'
    )
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    lyr = ds.GetLayerByName(new_layer_name)
    self.assertGreaterEqual(lyr.GetLayerDefn().GetFieldIndex('bar'), 0)

    gdal.ErrorReset()
    ds.ExecuteSQL('ALTER TABLE "weird2\'layer""name" DROP COLUMN "bar"')
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    self.assertLess(lyr.GetLayerDefn().GetFieldIndex('bar'), 0)

    ds.ExecuteSQL('VACUUM')
    ds = None

    f = gdal.VSIFOpenL(dbname, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn(layer_name, content)
    layer_name = new_layer_name

    ds = ogr.Open(dbname, update=1)
    ds.ExecuteSQL('DELETE FROM layer_styles')
    gdal.ErrorReset()
    with _quiet_error_handler():
      ds.ExecuteSQL('DELLAYER:does_not_exist')
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    gdal.ErrorReset()
    ds.ExecuteSQL('DELLAYER:' + layer_name)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    ds.ExecuteSQL('VACUUM')
    ds = None

    f = gdal.VSIFOpenL(dbname, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn(layer_name, content)

    self.driver.DeleteDataSource(dbname)

    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer(layer_name, geom_type=ogr.wkbCurvePolygon)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('CURVEPOLYGON ((0 0,0 1,1 1,0 0))')
    )
    lyr.CreateFeature(f)
    lyr.SetMetadataItem('FOO', 'BAR')
    ds.CreateLayer('another_layer_name')
    ds = None

    ds = ogr.Open(dbname, update=1)
    gdal.ErrorReset()
    ds.ExecuteSQL('DROP TABLE "weird2\'layer""name"')
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    ds.ExecuteSQL('DROP TABLE another_layer_name')
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    with _quiet_error_handler():
      ds.ExecuteSQL('DROP TABLE "foobar"')
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    gdal.ErrorReset()
    ds.ExecuteSQL('VACUUM')
    ds = None

    f = gdal.VSIFOpenL(dbname, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn(layer_name, content)
    self.assertNotIn('another_layer_name', content)

  def test_ogr_gpkg_35(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_35.gpkg')
    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPolygon)
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('bar_i_will_disappear', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('baz', ogr.OFTString))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFID(10)
    f.SetField('foo', 'fooval')
    f.SetField('bar_i_will_disappear', 'barval')
    f.SetField('baz', 'bazval')
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON ((0 0,0 1,1 1,0 0))')
    )
    lyr.CreateFeature(f)

    lyr_nonspatial = ds.CreateLayer('test_nonspatial', geom_type=ogr.wkbNone)
    lyr_nonspatial.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    lyr_nonspatial.CreateField(
        ogr.FieldDefn('bar_i_will_disappear', ogr.OFTString)
    )
    lyr_nonspatial.CreateField(ogr.FieldDefn('baz', ogr.OFTString))

    lyr_nonspatial.SetMetadataItem('FOO', 'BAR')
    ds.ExecuteSQL(
        "INSERT INTO gpkg_metadata_reference VALUES ('column', "
        "'test_nonspatial', 'bar_i_will_disappear', NULL, "
        "'2021-01-01T00:00:00.000Z', 1, NULL)"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_metadata VALUES (2, 'dataset','http://gdal.org',"
        "'text/plain','bla')"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_metadata_reference VALUES ('column', "
        "'test_nonspatial', 'bar_i_will_disappear', NULL, "
        "'2021-01-01T00:00:00.000Z', 2, NULL)"
    )

    f = ogr.Feature(lyr_nonspatial.GetLayerDefn())
    f.SetFID(10)
    f.SetField('foo', 'fooval')
    f.SetField('bar_i_will_disappear', 'barval')
    f.SetField('baz', 'bazval')
    lyr_nonspatial.CreateFeature(f)

    ds.ExecuteSQL("""CREATE TABLE gpkg_data_columns (
  table_name TEXT NOT NULL,
  column_name TEXT NOT NULL,
  name TEXT,
  title TEXT,
  description TEXT,
  mime_type TEXT,
  constraint_name TEXT,
  CONSTRAINT pk_gdc PRIMARY KEY (table_name, column_name),
  CONSTRAINT gdc_tn UNIQUE (table_name, name)
)""")
    ds.ExecuteSQL("""CREATE TABLE gpkg_data_column_constraints (
            constraint_name TEXT NOT NULL,
            constraint_type TEXT NOT NULL,
            value TEXT,
            min NUMERIC,
            min_is_inclusive BOOLEAN,
            max NUMERIC,
            max_is_inclusive BOOLEAN,
            description TEXT,
            CONSTRAINT gdcc_ntv UNIQUE (constraint_name,
            constraint_type, value))""")
    ds.ExecuteSQL(
        "INSERT INTO gpkg_data_columns VALUES('test', 'bar_i_will_disappear', "
        "'bar_constraints', NULL, NULL, NULL, NULL)"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('test', 'bar_i_will_disappear', "
        "'extension_name', 'definition', 'scope')"
    )

    self.assertEqual(lyr.TestCapability(ogr.OLCDeleteField), 1)

    with _quiet_error_handler():
      ret = lyr.DeleteField(-1)
    self.assertNotEqual(ret, 0)

    with _quiet_error_handler():
      ret = lyr.DeleteField(lyr.GetLayerDefn().GetFieldCount())
    self.assertNotEqual(ret, 0)

    self.assertEqual(lyr.DeleteField(1), 0)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)

    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f['foo'], 'fooval')
    self.assertEqual(f['baz'], 'bazval')
    self.assertEqual(
        f.GetGeometryRef().ExportToWkt(), 'POLYGON ((0 0,0 1,1 1,0 0))'
    )
    lyr = None
    lyr_nonspatial = None
    ds = None

    with gdal.config_option('OGR_SQLITE_PRAGMA', 'FOREIGN_KEYS=0'):
      ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayerByName('test')
    lyr_nonspatial = ds.GetLayerByName('test_nonspatial')
    lyr.StartTransaction()
    ret = lyr_nonspatial.DeleteField(1)
    lyr.CommitTransaction()
    self.assertEqual(ret, 0)
    lyr_nonspatial.ResetReading()
    f = lyr_nonspatial.GetNextFeature()
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f['foo'], 'fooval')
    self.assertEqual(f['baz'], 'bazval')

    ds.ExecuteSQL('VACUUM')
    ds = None

    self.assertTrue(self.validate(dbname))

    ds = ogr.Open(dbname)

    with ds.ExecuteSQL('SELECT * FROM gpkg_metadata WHERE id = 1') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    with ds.ExecuteSQL('SELECT * FROM gpkg_metadata WHERE id = 2') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)

    lyr = ds.GetLayerByName('test_nonspatial')
    self.assertEqual(lyr.GetMetadataItem('FOO'), 'BAR')

    lyr = ds.GetLayer(0)
    with _quiet_error_handler():
      ret = lyr.DeleteField(0)
    self.assertNotEqual(ret, 0)
    ds = None

    f = gdal.VSIFOpenL(dbname, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn('bar_i_will_disappear', content)

  def test_ogr_gpkg_36(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_36.gpkg')
    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPolygon)
    field = ogr.FieldDefn('foo', ogr.OFTString)
    field.SetAlternativeName('constraint')
    lyr.CreateField(field)
    lyr.CreateField(ogr.FieldDefn('baz', ogr.OFTString))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFID(10)
    f.SetField('foo', '10.5')
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON ((0 0,0 1,1 1,0 0))')
    )
    lyr.CreateFeature(f)
    f = None

    ds.ExecuteSQL("""CREATE TABLE gpkg_data_column_constraints (
            constraint_name TEXT NOT NULL,
            constraint_type TEXT NOT NULL,
            value TEXT,
            min NUMERIC,
            min_is_inclusive BOOLEAN,
            max NUMERIC,
            max_is_inclusive BOOLEAN,
            description TEXT,
            CONSTRAINT gdcc_ntv UNIQUE (constraint_name,
            constraint_type, value))""")
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('test', 'foo', 'extension_name', "
        "'definition', 'read-write')"
    )
    ds.ExecuteSQL('CREATE INDEX my_idx ON test(foo)')

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_data_columns WHERE table_name = 'test' AND "
        "column_name = 'foo'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    lyr.SetMetadataItem('FOO', 'BAR')
    ds.ExecuteSQL(
        "INSERT INTO gpkg_metadata_reference VALUES ('column', 'test', 'foo', "
        "NULL, '2021-01-01T00:00:00.000Z', 1, NULL)"
    )

    self.assertEqual(lyr.TestCapability(ogr.OLCAlterFieldDefn), 1)

    with _quiet_error_handler():
      ret = lyr.AlterFieldDefn(-1, ogr.FieldDefn('foo'), ogr.ALTER_ALL_FLAG)
    self.assertNotEqual(ret, 0)

    with _quiet_error_handler():
      ret = lyr.AlterFieldDefn(1, ogr.FieldDefn('foo'), ogr.ALTER_ALL_FLAG)
    self.assertNotEqual(ret, 0)

    with _quiet_error_handler():
      ret = lyr.AlterFieldDefn(
          0, ogr.FieldDefn(lyr.GetGeometryColumn()), ogr.ALTER_ALL_FLAG
      )
    self.assertNotEqual(ret, 0)

    with _quiet_error_handler():
      ret = lyr.AlterFieldDefn(
          0, ogr.FieldDefn(lyr.GetFIDColumn()), ogr.ALTER_ALL_FLAG
      )
    self.assertNotEqual(ret, 0)

    with _quiet_error_handler():
      ret = lyr.AlterFieldDefn(0, ogr.FieldDefn('baz'), ogr.ALTER_ALL_FLAG)
    self.assertNotEqual(ret, 0)

    new_field_defn = ogr.FieldDefn('bar', ogr.OFTReal)
    new_field_defn.SetSubType(ogr.OFSTFloat32)
    new_field_defn.SetWidth(10)
    new_field_defn.SetDefault('5')

    self.assertEqual(
        lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_ALL_FLAG), 0
    )

    new_field_defn.SetNullable(False)
    self.assertEqual(
        lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_ALL_FLAG), 0
    )

    new_field_defn.SetUnique(True)
    new_field_defn.SetAlternativeName('alt name')
    self.assertEqual(
        lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_ALL_FLAG), 0
    )

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_data_columns WHERE table_name = 'test' AND "
        "column_name = 'bar'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    new_field_defn = ogr.FieldDefn('baz', ogr.OFTString)
    new_field_defn.SetNullable(False)
    with _quiet_error_handler():
      self.assertNotEqual(
          lyr.AlterFieldDefn(1, new_field_defn, ogr.ALTER_ALL_FLAG), 0
      )

    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f['bar'], 10.5)
    self.assertEqual(
        f.GetGeometryRef().ExportToWkt(), 'POLYGON ((0 0,0 1,1 1,0 0))'
    )
    f = None

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_data_columns WHERE table_name = 'test' AND "
        "column_name = 'bar'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    lyr.StartTransaction()
    new_field_defn = ogr.FieldDefn('baw2', ogr.OFTString)
    self.assertEqual(
        lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_NAME_FLAG), 0
    )
    lyr.CommitTransaction()

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_data_columns WHERE table_name = 'test' AND "
        "column_name = 'baw2'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f['baw2'], 10.5)
    self.assertEqual(
        f.GetGeometryRef().ExportToWkt(), 'POLYGON ((0 0,0 1,1 1,0 0))'
    )
    f = None

    lyr.StartTransaction()
    new_field_defn = ogr.FieldDefn('baw', ogr.OFTString)
    new_field_defn.SetAlternativeName('baw alt')
    new_field_defn.SetComment('baw comment')
    self.assertEqual(
        lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_ALL_FLAG), 0
    )
    lyr.CommitTransaction()

    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), 'baw alt'
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), 'baw comment'
    )

    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f['baw'], '10.5')
    self.assertEqual(
        f.GetGeometryRef().ExportToWkt(), 'POLYGON ((0 0,0 1,1 1,0 0))'
    )
    f = None

    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'my_idx'"
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertIsNotNone(f)
      f = None

    ds.ExecuteSQL('VACUUM')
    ds = None

    self.assertTrue(self.validate(dbname))

    ds = ogr.Open(dbname)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_data_columns WHERE table_name = 'test' AND "
        "column_name = 'baw'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_metadata_reference WHERE table_name = 'test' AND "
        "column_name = 'baw'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    lyr = ds.GetLayer(0)
    with _quiet_error_handler():
      ret = lyr.AlterFieldDefn(0, ogr.FieldDefn('foo'), ogr.ALTER_ALL_FLAG)
    self.assertNotEqual(ret, 0)
    ds = None

    f = gdal.VSIFOpenL(dbname, 'rb')
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn('foo', content)

    self.driver.DeleteDataSource(dbname)

    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPolygon)
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
    gdal.Unlink(dbname)
    with _quiet_error_handler():
      new_field_defn = ogr.FieldDefn('bar')
      new_field_defn.SetNullable(False)
      ret = lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_ALL_FLAG)
    self.assertNotEqual(ret, 0)
    with _quiet_error_handler():
      ds = None

  def test_ogr_gpkg_36_alter_comment_after_alternative_name(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_36a.gpkg')
    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPolygon)
    field = ogr.FieldDefn('foo', ogr.OFTString)
    lyr.CreateField(field)

    new_field_defn = ogr.FieldDefn(field.GetName(), field.GetType())
    new_field_defn.SetAlternativeName('alt name')
    self.assertEqual(
        lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_ALTERNATIVE_NAME_FLAG),
        0,
    )

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_data_columns WHERE table_name = 'test' AND "
        "column_name = 'foo'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    new_field_defn = ogr.FieldDefn(field.GetName(), field.GetType())
    new_field_defn.SetComment('alt comment')
    self.assertEqual(
        lyr.AlterFieldDefn(0, new_field_defn, ogr.ALTER_COMMENT_FLAG), 0
    )

    with ds.ExecuteSQL(
        "SELECT * FROM gpkg_data_columns WHERE table_name = 'test' AND "
        "column_name = 'foo'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), 'alt name'
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), 'alt comment'
    )

    ds.ExecuteSQL('VACUUM')
    ds = None

    self.assertTrue(self.validate(dbname))

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)

    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), 'alt name'
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), 'alt comment'
    )

  def test_ogr_gpkg_37(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_37.gpkg')
    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPolygon)
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('bar', ogr.OFTString))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFID(10)
    f.SetField('foo', 'fooval')
    f.SetField('bar', 'barval')
    f.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON ((0 0,0 1,1 1,0 0))')
    )
    lyr.CreateFeature(f)

    ds.ExecuteSQL("""CREATE TABLE gpkg_data_columns (
  table_name TEXT NOT NULL,
  column_name TEXT NOT NULL,
  name TEXT,
  title TEXT,
  description TEXT,
  mime_type TEXT,
  constraint_name TEXT,
  CONSTRAINT pk_gdc PRIMARY KEY (table_name, column_name),
  CONSTRAINT gdc_tn UNIQUE (table_name, name)
)""")
    ds.ExecuteSQL(
        "INSERT INTO gpkg_data_columns VALUES('test', 'foo', 'constraint', "
        'NULL, NULL, NULL, NULL)'
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('test', 'foo', 'extension_name', "
        "'definition', 'scope')"
    )
    ds.ExecuteSQL('CREATE INDEX my_idx_foo ON test(foo)')
    ds.ExecuteSQL('CREATE INDEX my_idx_bar ON test(bar)')

    self.assertEqual(lyr.TestCapability(ogr.OLCReorderFields), 1)

    with _quiet_error_handler():
      ret = lyr.ReorderFields([-1, -1])
    self.assertNotEqual(ret, 0)

    self.assertEqual(lyr.ReorderFields([1, 0]), 0)

    lyr.ResetReading()
    self.assertEqual(lyr.GetLayerDefn().GetFieldIndex('foo'), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldIndex('bar'), 0)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10)
    self.assertEqual(f['foo'], 'fooval')
    self.assertEqual(f['bar'], 'barval')
    self.assertEqual(
        f.GetGeometryRef().ExportToWkt(), 'POLYGON ((0 0,0 1,1 1,0 0))'
    )

    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'my_idx_foo' OR "
        "name = 'my_idx_bar'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 2)

    ds = None

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)
    with _quiet_error_handler():
      ret = lyr.ReorderFields([1, 0])
    self.assertNotEqual(ret, 0)
    ds = None

  @parameterized.parameters('YES', 'NO')
  def test_ogr_gpkg_38(self, spatial_index: str) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_38.gpkg')
    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer(
        'test',
        geom_type=ogr.wkbLineString,
        options={'SPATIAL_INDEX': spatial_index},
    )
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('LINESTRING (1 2,3 4)'))
    lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(dbname, update=1)
    ds.ExecuteSQL(
        'UPDATE gpkg_contents SET min_x = NULL, min_y = NULL, max_x = NULL, '
        'max_y = NULL'
    )
    ds = None

    ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayer(0)
    extent = lyr.GetExtent(force=1)
    self.assertEqual(extent, (1, 3, 2, 4))
    with ds.ExecuteSQL(
        'SELECT min_x, min_y, max_x, max_y FROM gpkg_contents'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f['min_x'], 1)
      self.assertEqual(f['min_y'], 2)
      self.assertEqual(f['max_x'], 3)
      self.assertEqual(f['max_y'], 4)

    extent = lyr.GetExtent(force=0)
    self.assertEqual(extent, (1, 3, 2, 4))

    f = lyr.GetFeature(1)
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('LINESTRING (-1 -2,-3 -4)'))
    lyr.SetFeature(f)

    extent = lyr.GetExtent(force=0)
    self.assertEqual(extent, (-3.0, 3.0, -4.0, 4.0))

    ds.ExecuteSQL('RECOMPUTE EXTENT ON test')
    extent = lyr.GetExtent(force=0)
    self.assertEqual(extent, (-3.0, -1.0, -4.0, -2.0))
    ds = None

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)
    extent = lyr.GetExtent(force=0)
    self.assertEqual(extent, (-3.0, -1.0, -4.0, -2.0))
    ds = None

    ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayer(0)
    lyr.DeleteFeature(1)

    ds.ExecuteSQL('RECOMPUTE EXTENT ON test')
    extent = lyr.GetExtent(force=0, can_return_null=True)
    self.assertIsNone(extent)
    ds = None

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)
    extent = lyr.GetExtent(force=0, can_return_null=True)
    self.assertIsNone(extent)
    ds = None

  def test_ogr_gpkg_39(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_39.gpkg')
    ds = self.driver.CreateDataSource(dbname)

    ds.CreateLayer('test')

    lyr = ds.CreateLayer(
        'test_with_explicit_identifier',
        options=['IDENTIFIER=explicit_identifier'],
    )
    self.assertIsNotNone(lyr)

    lyr = ds.CreateLayer(
        'test_with_explicit_identifier',
        options=['IDENTIFIER=explicit_identifier', 'OVERWRITE=YES'],
    )
    self.assertIsNotNone(lyr)

    with _quiet_error_handler():
      lyr = ds.CreateLayer('test2', options=['IDENTIFIER=test'])
    self.assertIsNone(lyr)

    with _quiet_error_handler():
      lyr = ds.CreateLayer('test2', options=['IDENTIFIER=explicit_identifier'])
    self.assertIsNone(lyr)

    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents ( table_name, identifier, data_type ) '
        "VALUES ( 'some_table', 'another_identifier', 'some_data_type' )"
    )
    with _quiet_error_handler():
      lyr = ds.CreateLayer('test2', options=['IDENTIFIER=another_identifier'])
    self.assertIsNone(lyr)
    ds = None

  def test_ogr_gpkg_40(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_40.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    ds.CreateLayer('aspatial', geom_type=ogr.wkbNone)
    ds = None

    ds = ogr.Open(dbname)
    self.assertEqual(ds.GetLayerCount(), 1)
    with ds.ExecuteSQL('SELECT * FROM gpkg_contents') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)

    with ds.ExecuteSQL('SELECT * FROM gpkg_geometry_columns') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)

    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'gpkg_extensions'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)

    ds = None

    self.assertTrue(self.validate(dbname))

  def test_ogr_gpkg_41(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_41.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    ds.ExecuteSQL('CREATE TABLE foo (mycol VARCHAR_ILLEGAL)')
    ds.ExecuteSQL("INSERT INTO foo VALUES ('myval')")
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents '
        '(table_name,data_type,identifier,description,last_change,srs_id) '
        "VALUES ('foo','attributes','foo','','',0)"
    )
    ds = None

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertEqual(f['mycol'], 'myval')
    self.assertEqual(f.GetFID(), 1)
    ds = None

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)
    with _quiet_error_handler():
      f = lyr.GetFeature(1)
    self.assertEqual(f['mycol'], 'myval')
    self.assertEqual(f.GetFID(), 1)
    ds = None

  def _foo_has_trigger(self, ds: Any) -> bool:
    with ds.ExecuteSQL(
        'SELECT COUNT(*) FROM sqlite_master WHERE '
        "name = 'trigger_insert_feature_count_foo'",
        dialect='DEBUG',
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      return f.GetField(0) == 1

  def _get_feature_count_from_gpkg_contents(self, ds: Any) -> int | None:
    with ds.ExecuteSQL(
        'SELECT feature_count FROM gpkg_ogr_contents', dialect='DEBUG'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      return f.GetField(0)

  def test_ogr_gpkg_42(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_42.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('foo', geom_type=ogr.wkbNone)
    lyr.CreateField(ogr.FieldDefn('i', ogr.OFTInteger))
    for i in range(5):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetField(0, i)
      lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)
    self.assertEqual(self._get_feature_count_from_gpkg_contents(ds), 5)
    self.assertTrue(self._foo_has_trigger(ds))
    self.assertNotEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
    ds = None

    ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayer(0)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField(0, 10)
    lyr.CreateFeature(f)

    self.assertIsNone(self._get_feature_count_from_gpkg_contents(ds))
    self.assertFalse(self._foo_has_trigger(ds))

    self.assertEqual(lyr.GetFeatureCount(), 6)

    ds.ExecuteSQL('DELETE FROM foo WHERE i = 1')

    self.assertTrue(self._foo_has_trigger(ds))
    self.assertEqual(self._get_feature_count_from_gpkg_contents(ds), 5)
    self.assertEqual(lyr.GetFeatureCount(), 5)
    self.assertEqual(self._get_feature_count_from_gpkg_contents(ds), 5)

    f = ogr.Feature(lyr.GetLayerDefn())
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertIsNone(self._get_feature_count_from_gpkg_contents(ds))
    self.assertEqual(lyr.SyncToDisk(), ogr.OGRERR_NONE)
    self.assertEqual(self._get_feature_count_from_gpkg_contents(ds), 6)

    self.assertEqual(lyr.DeleteFeature(f.GetFID()), ogr.OGRERR_NONE)
    self.assertIsNone(self._get_feature_count_from_gpkg_contents(ds))

    ds.ExecuteSQL('INSERT OR REPLACE INTO foo (fid) VALUES (1)')
    self.assertEqual(self._get_feature_count_from_gpkg_contents(ds), 5)
    ds = None

    ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetFeatureCount(), 5)
    ds.ExecuteSQL('UPDATE gpkg_ogr_contents SET feature_count = NULL')
    ds = None

    ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayer(0)
    self.assertIsNone(self._get_feature_count_from_gpkg_contents(ds))
    self.assertEqual(lyr.GetFeatureCount(), 5)
    ds = None

    ds = ogr.Open(dbname, update=1)
    self.assertEqual(self._get_feature_count_from_gpkg_contents(ds), 5)

    ds.ExecuteSQL('UPDATE gpkg_ogr_contents SET feature_count = 5000')

    ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetFeatureCount(), 5000)

    self.assertEqual(lyr.TestCapability(ogr.OLCRename), 1)
    self.assertEqual(lyr.Rename('bar'), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetDescription(), 'bar')
    self.assertEqual(lyr.GetLayerDefn().GetName(), 'bar')
    with _quiet_error_handler():
      self.assertNotEqual(lyr.Rename('bar'), ogr.OGRERR_NONE)
    with _quiet_error_handler():
      self.assertNotEqual(lyr.Rename('gpkg_ogr_contents'), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetDescription(), 'bar')
    self.assertEqual(lyr.GetLayerDefn().GetName(), 'bar')

    ds = None
    ds = ogr.Open(dbname, update=1)
    with ds.ExecuteSQL(
        "SELECT feature_count FROM gpkg_ogr_contents WHERE table_name = 'bar'",
        dialect='DEBUG',
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      val = f.GetField(0)
      f = None
    self.assertEqual(val, 5000)

    ds.DeleteLayer(0)
    with ds.ExecuteSQL(
        'SELECT feature_count FROM gpkg_ogr_contents', dialect='DEBUG'
    ) as sql_lyr:
      self.assertIsNone(sql_lyr.GetNextFeature())
    ds = None

    ds = self.driver.CreateDataSource(
        dbname, options=['ADD_GPKG_OGR_CONTENTS=FALSE']
    )
    lyr = ds.CreateLayer('foo', geom_type=ogr.wkbNone)
    lyr.CreateField(ogr.FieldDefn('i', ogr.OFTInteger))
    for i in range(5):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetField(0, i)
      lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(dbname, update=1)

    with ds.ExecuteSQL('PRAGMA table_info(gpkg_contents)') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 10)

    self.assertFalse(self._foo_has_trigger(ds))

    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
    self.assertEqual(lyr.GetFeatureCount(), 5)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField(0, 10)
    lyr.CreateFeature(f)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetFeatureCount(), 6)
    ds.ExecuteSQL('DELETE FROM foo WHERE i = 1')
    self.assertEqual(lyr.GetFeatureCount(), 5)
    ds = None

  def test_ogr_gpkg_43(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_43.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    ds.StartTransaction()
    for i in range(1001):
      ds.ExecuteSQL(
          'INSERT INTO gpkg_contents (table_name, data_type, identifier) '
          f"VALUES ('tiles{i + 1}', 'tiles', 'tiles{i + 1}')"
      )
      ds.ExecuteSQL(
          'INSERT INTO gpkg_tile_matrix_set VALUES '
          f"('tiles{i + 1}', 0, 440720, 3750120, 441920, 3751320)"
      )
    for i in range(1001):
      ds.ExecuteSQL(
          'INSERT INTO gpkg_contents (table_name, data_type, identifier) '
          f"VALUES ('attr{i + 1}', 'attributes', 'attr{i + 1}')"
      )
      ds.ExecuteSQL(
          f'CREATE TABLE attr{i + 1} (id INTEGER PRIMARY KEY AUTOINCREMENT)'
      )
    ds.CommitTransaction()
    ds = None

    ds = gdal.OpenEx(dbname)
    self.assertLen(ds.GetMetadata_List('SUBDATASETS'), 2 * 1001)
    self.assertEqual(ds.GetLayerCount(), 1001)

    with gdal.config_option('OGR_TABLE_LIMIT', '1000'):
      with _quiet_error_handler():
        ds = gdal.OpenEx(dbname)
        self.assertLen(ds.GetMetadata_List('SUBDATASETS'), 2 * 1000)
        self.assertEqual(ds.GetLayerCount(), 1000)
    ds = None

  def test_ogr_gpkg_METADATA_TABLES_NO(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_METADATA_TABLES_NO.gpkg')

    ds = self.driver.CreateDataSource(filename, options=['METADATA_TABLES=NO'])
    ds.CreateLayer('foo')
    ds.SetMetadataItem('FOO', 'BAR')
    ds = None

    self.assertTrue(self.validate(filename))

    ds = ogr.Open(filename)
    self.assertEqual(ds.GetMetadata(), {})
    self.assertEqual(ds.GetLayer(0).GetMetadata(), {})
    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'gpkg_metadata'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
    ds = None

    ds = ogr.Open(filename, update=1)
    ds.SetMetadataItem('FOO', 'BAR')
    ds = None

    ds = ogr.Open(filename)
    self.assertEqual(ds.GetMetadata(), {'FOO': 'BAR'})
    ds = None

  def test_ogr_gpkg_METADATA_TABLES_YES(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_METADATA_TABLES_YES.gpkg')

    ds = self.driver.CreateDataSource(filename, options=['METADATA_TABLES=YES'])
    ds.CreateLayer('foo')
    ds = None

    self.assertTrue(self.validate(filename))

    ds = ogr.Open(filename)
    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'gpkg_metadata'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    ds = None

  def test_ogr_gpkg_METADATA_TABLES_AUTO(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_METADATA_TABLES_AUTO.gpkg')

    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('foo')
    lyr.SetMetadataItem('foo', 'bar')
    ds = None

    self.assertTrue(self.validate(filename))

    ds = ogr.Open(filename)
    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'gpkg_metadata'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    self.assertEqual(ds.GetLayer(0).GetMetadata(), {'foo': 'bar'})
    ds = None

  def test_ogr_gpkg_METADATA_TABLES_AUTO_not_needed(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_METADATA_TABLES_AUTO_not_needed.gpkg'
    )

    ds = self.driver.CreateDataSource(filename)
    ds.CreateLayer('foo')
    ds = None

    self.assertTrue(self.validate(filename))

    ds = ogr.Open(filename)
    with ds.ExecuteSQL(
        "SELECT * FROM sqlite_master WHERE name = 'gpkg_metadata'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
    ds = None

  def test_ogr_gpkg_45(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_45.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    ds.ExecuteSQL(
        'CREATE TABLE test (a INTEGER, b INTEGER, '
        'CONSTRAINT pkid_constraint PRIMARY KEY (a, b))'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents ( table_name, identifier, data_type ) '
        "VALUES ( 'test', 'test', 'attributes' )"
    )
    ds = None
    ds = ogr.Open(dbname)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetFIDColumn(), '')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    ds = None

  def test_ogr_gpkg_46(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_46.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('foo')
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(1 1)'))
    lyr.CreateFeature(f)
    ds.ExecuteSQL(
        'CREATE VIEW my_view AS SELECT geom AS my_geom, fid AS my_fid FROM foo'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, identifier, data_type, srs_id) '
        "VALUES ( 'my_view', 'my_view', 'features', 0 )"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns (table_name, column_name, '
        'geometry_type_name, srs_id, z, m) values '
        "('my_view', 'my_geom', 'GEOMETRY', 0, 0, 0)"
    )

    ds.ExecuteSQL(
        'CREATE VIEW my_view2 AS SELECT geom, fid AS OGC_FID, '
        "'bla' as another_column FROM foo"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, identifier, data_type, srs_id) '
        "VALUES ( 'my_view2', 'my_view2', 'features', 0 )"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns (table_name, column_name, '
        'geometry_type_name, srs_id, z, m) values '
        "('my_view2', 'geom', 'GEOMETRY', 0, 0, 0)"
    )

    ds.ExecuteSQL(
        'CREATE VIEW my_view3 AS SELECT a.fid * 10000 + b.fid as my_fid, '
        'a.fid as fid1, a.geom, b.fid as fid2 FROM foo a, foo b'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, identifier, data_type, srs_id) '
        "VALUES ( 'my_view3', 'my_view3', 'features', 0 )"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns (table_name, column_name, '
        'geometry_type_name, srs_id, z, m) values '
        "('my_view3', 'geom', 'GEOMETRY', 0, 0, 0)"
    )
    ds = None

    ds = ogr.Open(dbname, update=1)
    lyr = ds.GetLayerByName('my_view')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetGeometryColumn(), 'my_geom')

    with _quiet_error_handler():
      with ds.ExecuteSQL(
          "SELECT CreateSpatialIndex('my_view', 'my_geom')"
      ) as _:
        pass
      with ds.ExecuteSQL(
          "SELECT DisableSpatialIndex('my_view', 'my_geom')"
      ) as _:
        pass
      lyr.AlterFieldDefn(
          0, lyr.GetLayerDefn().GetFieldDefn(0), ogr.ALTER_ALL_FLAG
      )
      lyr.DeleteField(0)
      lyr.ReorderFields([0])
      lyr.CreateField(ogr.FieldDefn('bar'))

    with ds.ExecuteSQL(
        "SELECT HasSpatialIndex('my_view', 'my_geom')"
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      has_spatial_index = f.GetField(0) == 1
    if not has_spatial_index:
      ds = None
      self.skipTest('SQLite likely built without SQLITE_HAS_COLUMN_METADATA')

    lyr.SetSpatialFilterRect(-0.5, -0.5, 0.5, 0.5)
    self.assertEqual(lyr.GetFeatureCount(), 1)
    self.assertIsNotNone(lyr.GetNextFeature())
    self.assertIsNone(lyr.GetNextFeature())
    lyr.SetSpatialFilter(None)

    lyr.SetIgnoredFields(['my_fid', 'my_geom'])
    lyr.ResetReading()
    self.assertIsNotNone(lyr.GetNextFeature())
    lyr.SetIgnoredFields([])

    lyr = ds.GetLayerByName('my_view2')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetFIDColumn(), 'OGC_FID')
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 1)
    self.assertEqual(f.GetField(0), 'bla')

    lyr = ds.GetLayerByName('my_view3')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10001)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 10002)
    f2 = lyr.GetFeature(10002)
    self.assertTrue(f.Equal(f2))
    ds = None

  def test_ogr_gpkg_47(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_47.gpkg')

    self.driver.CreateDataSource(dbname)
    fp = gdal.VSIFOpenL(dbname, 'rb+')
    gdal.VSIFSeekL(fp, 68, 0)
    gdal.VSIFWriteL(struct.pack('B' * 4, 0, 0, 0, 0), 4, 1, fp)
    gdal.VSIFCloseL(fp)

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(dbname, update=1)
    self.assertIsNotNone(ds)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with gdal.config_option('GPKG_WARN_UNRECOGNIZED_APPLICATION_ID', 'NO'):
      ogr.Open(dbname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_47a(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_47a.gpkg')

    self.driver.CreateDataSource(dbname, options={'VERSION': '1.2'})
    fp = gdal.VSIFOpenL(dbname, 'rb+')
    gdal.VSIFSeekL(fp, 60, 0)
    gdal.VSIFWriteL(struct.pack('B' * 4, 0, 0, 0, 0), 4, 1, fp)
    gdal.VSIFCloseL(fp)

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(dbname, update=1)
    self.assertIsNotNone(ds)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    ds = None

    gdal.ErrorReset()
    with gdal.config_option('GPKG_WARN_UNRECOGNIZED_APPLICATION_ID', 'NO'):
      ogr.Open(dbname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_47b(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_47b.gpkg')

    self.driver.CreateDataSource(dbname, options=['VERSION=1.2'])
    fp = gdal.VSIFOpenL(dbname, 'rb+')
    gdal.VSIFSeekL(fp, 60, 0)
    self.assertEqual(struct.unpack('>I', gdal.VSIFReadL(4, 1, fp))[0], 10200)
    gdal.VSIFSeekL(fp, 60, 0)
    gdal.VSIFWriteL(struct.pack('>I', 10201), 4, 1, fp)
    gdal.VSIFCloseL(fp)

    gdal.ErrorReset()
    ds = ogr.Open(dbname, update=1)
    self.assertIsNotNone(ds)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    ds = None

    gdal.ErrorReset()
    with gdal.config_option('GPKG_WARN_UNRECOGNIZED_APPLICATION_ID', 'NO'):
      ogr.Open(dbname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_47c(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_47c.gpkg')

    self.driver.CreateDataSource(dbname, options={'VERSION': '1.3'})
    fp = gdal.VSIFOpenL(dbname, 'rb')
    gdal.VSIFSeekL(fp, 60, 0)
    self.assertEqual(struct.unpack('>I', gdal.VSIFReadL(4, 1, fp))[0], 10300)
    gdal.VSIFCloseL(fp)

    gdal.ErrorReset()
    ds = ogr.Open(dbname, update=1)
    self.assertIsNotNone(ds)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    ds = None

    gdal.ErrorReset()
    with gdal.config_option('GPKG_WARN_UNRECOGNIZED_APPLICATION_ID', 'NO'):
      ogr.Open(dbname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_47c2(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_47c2.gpkg')

    self.driver.CreateDataSource(dbname, options={'VERSION': '1.4'})
    fp = gdal.VSIFOpenL(dbname, 'rb')
    gdal.VSIFSeekL(fp, 60, 0)
    self.assertEqual(struct.unpack('>I', gdal.VSIFReadL(4, 1, fp))[0], 10400)
    gdal.VSIFCloseL(fp)

    gdal.ErrorReset()
    ds = ogr.Open(dbname, update=1)
    self.assertIsNotNone(ds)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    ds = None

    gdal.ErrorReset()
    with gdal.config_option('GPKG_WARN_UNRECOGNIZED_APPLICATION_ID', 'NO'):
      ogr.Open(dbname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_47d(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_47d.gpkg')

    self.driver.CreateDataSource(dbname, options=['VERSION=1.2'])
    fp = gdal.VSIFOpenL(dbname, 'rb+')
    gdal.VSIFSeekL(fp, 60, 0)
    gdal.VSIFWriteL(struct.pack('>I', 19900), 4, 1, fp)
    gdal.VSIFCloseL(fp)

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(dbname, update=1)
    self.assertIsNotNone(ds)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with gdal.config_option('GPKG_WARN_UNRECOGNIZED_APPLICATION_ID', 'NO'):
      ogr.Open(dbname)
    self.assertEqual(gdal.GetLastErrorMsg(), '')

  def test_ogr_gpkg_47e(self) -> None:
    dbname = str(self.vsimem_dir / '.cur_input')

    with _quiet_error_handler():
      self.driver.CreateDataSource(dbname)
    fp = gdal.VSIFOpenL(dbname, 'rb+')
    gdal.VSIFSeekL(fp, 68, 0)
    gdal.VSIFWriteL(struct.pack('B' * 4, 0, 0, 0, 0), 4, 1, fp)
    gdal.VSIFCloseL(fp)
    ds = ogr.Open(dbname)
    self.assertIsNotNone(ds)

  def test_ogr_gpkg_47f(self) -> None:
    dbname = str(self.vsimem_dir / '.cur_input')

    with _quiet_error_handler():
      self.driver.CreateDataSource(dbname, options=['VERSION=1.2'])

    fp = gdal.VSIFOpenL(dbname, 'rb+')
    gdal.VSIFSeekL(fp, 60, 0)
    gdal.VSIFWriteL(struct.pack('B' * 4, 0, 0, 0, 0), 4, 1, fp)
    gdal.VSIFCloseL(fp)
    ds = ogr.Open(dbname)
    self.assertIsNone(ds)

  def test_ogr_gpkg_47g(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_47g.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    ds.CreateLayer('foo')
    ds = None
    fp = gdal.VSIFOpenL(dbname, 'rb')
    content = gdal.VSIFReadL(1, 1000000, fp)
    gdal.VSIFCloseL(fp)
    zip_path = f'/vsizip/{self.vsimem_dir}/ogr_gpkg_47.zip'
    fzip = gdal.VSIFOpenL(zip_path, 'wb')
    fp = gdal.VSIFOpenL(f'{zip_path}/my.gpkg', 'wb')
    gdal.VSIFWriteL(content, 1, len(content), fp)
    gdal.VSIFCloseL(fp)
    gdal.VSIFCloseL(fzip)
    ds = ogr.Open(zip_path)
    self.assertEqual(ds.GetDriver().GetName(), 'GPKG')
    ds = None

  def test_ogr_gpkg_48(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_48.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer('foo')
    lyr.CreateField(ogr.FieldDefn('a'))
    lyr.CreateField(ogr.FieldDefn('b'))
    lyr.CreateField(ogr.FieldDefn('c'))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('a', 'a')
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('b', 'b')
    f.SetField('c', 'c')
    lyr.CreateFeature(f)
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('a'), 'a')
    self.assertIsNone(f.GetField('b'))
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('b'), 'b')
    self.assertEqual(f.GetField('c'), 'c')
    self.assertIsNone(f.GetField('a'))

    lyr = ds.CreateLayer('default_field_no_geom', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('foo')
    fld_defn.SetDefault("'x'")
    lyr.CreateField(fld_defn)
    f = ogr.Feature(lyr.GetLayerDefn())
    self.assertEqual(lyr.CreateFeature(f), 0)
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('foo'), 'x')
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFID(1)
    self.assertEqual(lyr.SetFeature(f), 0)
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('foo'), 'x')
    ds = None

  def test_ogr_gpkg_49(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_49.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    lyr = ds.CreateLayer(
        'test',
        geom_type=ogr.wkbNone,
        options=['ASPATIAL_VARIANT=GPKG_ATTRIBUTES'],
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    f = None

    field_defn = ogr.GeomFieldDefn('', ogr.wkbPoint)
    self.assertEqual(lyr.CreateGeomField(field_defn), 0)
    ds = None

  @parameterized.parameters('1.2', '1.4')
  def test_ogr_gpkg_CRS_WKT_EXTENSION(self, gpkg_version: str) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_50.gpkg')

    self.driver.CreateDataSource(
        dbname,
        options=['CRS_WKT_EXTENSION=YES', 'VERSION=' + gpkg_version],
    )

    ds = ogr.Open(dbname, update=1)
    srs32631 = osr.SpatialReference()
    srs32631.ImportFromEPSG(32631)
    ds.CreateLayer('test', srs=srs32631)

    srs_without_org = osr.SpatialReference()
    srs_without_org.SetFromUserInput(
        'GEOGCS["another geogcs",DATUM["another datum",'
        'SPHEROID["another spheroid",1000,0]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433]]'
    )
    lyr = ds.CreateLayer('without_org', srs=srs_without_org)
    ds = None

    self.assertTrue(self.validate(dbname))

    ds = ogr.Open(dbname)
    lyr = ds.GetLayer('test')
    self.assertTrue(lyr.GetSpatialRef().IsSame(srs32631))
    lyr = ds.GetLayer('without_org')
    self.assertTrue(lyr.GetSpatialRef().IsSame(srs_without_org))
    with ds.ExecuteSQL(
        'SELECT definition_12_063 FROM gpkg_spatial_ref_sys WHERE '
        'srs_id = 32631'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
    self.assertTrue(f.GetField(0).startswith('PROJCRS["WGS 84 / UTM zone 31N"'))

    with ds.ExecuteSQL('PRAGMA table_info(gpkg_spatial_ref_sys)') as sql_lyr:
      has_epoch = any(feat['name'] == 'epoch' for feat in sql_lyr)
      if gpkg_version == '1.2':
        self.assertFalse(has_epoch)
      else:
        self.assertTrue(has_epoch)
    ds = None

  def test_ogr_gpkg_51(self) -> None:
    if self.driver.GetMetadataItem('ENABLE_SQL_GPKG_FORMAT') != 'YES':
      self.skipTest('ENABLE_SQL_GPKG_FORMAT not enabled')

    ds = ogr.Open(self.getTestFilePath('poly.gpkg.sql'))
    lyr = ds.GetLayer(0)
    self.assertIsNotNone(lyr.GetNextFeature())

  def test_ogr_gpkg_52(self) -> None:
    ds = ogr.Open(self.getTestFilePath('poly_non_conformant.gpkg'))
    lyr = ds.GetLayer(0)
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertIsNotNone(f)

  def test_ogr_gpkg_53(self) -> None:
    if self.driver.GetMetadataItem('ENABLE_SQL_GPKG_FORMAT') != 'YES':
      self.skipTest('ENABLE_SQL_GPKG_FORMAT not enabled')

    ds = ogr.Open(self.getTestFilePath('poly_inconsistent_case.gpkg.sql'))
    self.assertEqual(ds.GetLayerCount(), 1)
    lyr = ds.GetLayer(0)
    self.assertIsNotNone(lyr.GetNextFeature())
    # No test_ogrsf binary in google3.

  def test_ogr_gpkg_54(self) -> None:
    tmpfile = str(self.tmp_dir / 'ogr_gpkg_54.gpkg')
    ds = self.driver.CreateDataSource(tmpfile)
    lyr = ds.CreateLayer('layer1', geom_type=ogr.wkbPoint)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    lyr.CreateFeature(f)
    f = None
    lyr = ds.CreateLayer('layer2', geom_type=ogr.wkbPoint)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(1 1)'))
    lyr.CreateFeature(f)
    f = None
    ds = None

    ds1 = ogr.Open(tmpfile, update=1)
    ds2 = ogr.Open(tmpfile, update=1)

    lyr1 = ds1.GetLayer(0)
    lyr2 = ds2.GetLayer(1)

    f1 = lyr1.GetFeature(1)
    f1.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
    lyr1.SetFeature(f1)

    f2 = lyr2.GetFeature(1)
    f2.SetGeometry(ogr.CreateGeometryFromWkt('POINT (3 4)'))
    lyr2.SetFeature(f2)

    f1 = lyr1.GetFeature(1)
    f1.SetGeometry(ogr.CreateGeometryFromWkt('POINT (5 6)'))
    lyr1.SetFeature(f1)

    f2 = lyr2.GetFeature(1)
    f2.SetGeometry(ogr.CreateGeometryFromWkt('POINT (7 8)'))
    lyr2.SetFeature(f2)

    ds1 = None
    ds2 = None

    ds = ogr.Open(tmpfile)
    lyr1 = ds.GetLayer(0)
    f = lyr1.GetNextFeature()
    self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'POINT (5 6)')
    lyr2 = ds.GetLayer(1)
    f = lyr2.GetNextFeature()
    self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'POINT (7 8)')
    ds = None

  def test_ogr_gpkg_55(self) -> None:
    tmpfile = str(self.vsimem_dir / 'ogr_gpkg_55.gpkg')
    ds = self.driver.CreateDataSource(tmpfile)
    lyr = ds.CreateLayer('layer1', geom_type=ogr.wkbLineString)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    gdal.ErrorReset()
    with _quiet_error_handler():
      lyr.CreateFeature(f)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    f = None
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(1 1)'))
    gdal.ErrorReset()
    lyr.CreateFeature(f)
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    f = None
    ds = None

  def test_ogr_gpkg_56(self) -> None:
    ds = gdal.VectorTranslate(
        str(self.vsimem_dir / 'ogr_gpkg_56.gpkg'),
        ogr_util.GetTestFilePath('shape/poly/poly.shp'),
        format='GPKG',
    )
    with ds.ExecuteSQL(
        'select a.fid as fid1, b.fid as fid2 from poly a, poly b '
        'order by fid1, fid2'
    ) as lyr:
      lyr.GetNextFeature()
      f = lyr.GetNextFeature()
      self.assertEqual(f.GetField('fid1'), 1)
      self.assertEqual(f.GetField('fid2'), 2)
    ds = None

  def test_ogr_gpkg_creation_fid(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_creation_fid.gpkg')
    ds = self.driver.CreateDataSource(filename)

    lyr = ds.CreateLayer('fid_integer')
    self.assertEqual(
        lyr.CreateField(ogr.FieldDefn('fid', ogr.OFTInteger)), ogr.OGRERR_NONE
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 12
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(f.GetFID(), 12)
    f = lyr.GetFeature(f.GetFID())
    self.assertIsNotNone(f)
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 13
    f.SetFID(13)
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(f.GetFID(), 13)
    f = lyr.GetFeature(f.GetFID())
    self.assertIsNotNone(f)
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFID(14)
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(f.GetFID(), 14)

    lyr = ds.CreateLayer('fid_integer64')
    self.assertEqual(
        lyr.CreateField(ogr.FieldDefn('fid', ogr.OFTInteger64)), ogr.OGRERR_NONE
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 1234567890123
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(f.GetFID(), 1234567890123)
    f = lyr.GetFeature(f.GetFID())
    self.assertIsNotNone(f)
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 1234567890124
    f.SetFID(1234567890124)
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(f.GetFID(), 1234567890124)
    f = lyr.GetFeature(f.GetFID())
    self.assertIsNotNone(f)
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 1234567890125
    f.SetFID(1)
    with _quiet_error_handler():
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_FAILURE)

    lyr = ds.CreateLayer('fid_real')
    fld_defn = ogr.FieldDefn('fid', ogr.OFTReal)
    fld_defn.SetWidth(20)
    fld_defn.SetPrecision(0)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 1234567890123
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(f.GetFID(), 1234567890123)
    f = lyr.GetFeature(f.GetFID())
    self.assertIsNotNone(f)
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 1234567890124
    f.SetFID(1234567890124)
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(f.GetFID(), 1234567890124)
    f = lyr.GetFeature(f.GetFID())
    self.assertIsNotNone(f)
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 1234567890123.5
    with _quiet_error_handler():
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_FAILURE)

    f = ogr.Feature(lyr.GetLayerDefn())
    f['fid'] = 1234567890125
    f.SetFID(1)
    with _quiet_error_handler():
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_FAILURE)

    ds = None

  def test_ogr_gpkg_57(self) -> None:
    out_filename = str(self.vsimem_dir / 'test_ogr_gpkg_57.gpkg')
    self.driver.CreateDataSource(out_filename)

    with gdal.config_option('OGR_SQLITE_PRAGMA', 'FOREIGN_KEYS=0'):
      ds = ogr.Open(out_filename, update=1)
    ds.ExecuteSQL('DROP TABLE gpkg_contents')
    ds.ExecuteSQL(
        'CREATE TABLE gpkg_contents (table_name,data_type,identifier,'
        'description,last_change,min_x, min_y,max_x, max_y,srs_id)'
    )
    ds.ExecuteSQL(
        'INSERT INTO "gpkg_contents" '
        "VALUES('poly','features','poly','','',NULL,NULL,NULL,NULL,0)"
    )
    ds.ExecuteSQL(
        'INSERT INTO "gpkg_contents" '
        "VALUES('poly','features','poly','','',NULL,NULL,NULL,NULL,0)"
    )
    ds.ExecuteSQL(
        'INSERT INTO "gpkg_geometry_columns" '
        "VALUES('poly','geom','POLYGON',0,0,0)"
    )
    ds.ExecuteSQL(
        'CREATE TABLE "poly"("fid" INTEGER PRIMARY KEY, "geom" POLYGON)'
    )
    ds = None

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(out_filename)
    self.assertEqual(ds.GetLayerCount(), 1)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    ds = None

  def test_ogr_gpkg_multiple_geom_columns(self) -> None:
    out_filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_multiple_geom_columns.gpkg'
    )
    self.driver.CreateDataSource(out_filename)

    ds = ogr.Open(out_filename, update=1)
    ds.ExecuteSQL('DROP TABLE gpkg_geometry_columns')
    ds.ExecuteSQL(
        'CREATE TABLE gpkg_geometry_columns (table_name TEXT NOT NULL,'
        'column_name TEXT NOT NULL,geometry_type_name TEXT NOT NULL,'
        'srs_id INTEGER NOT NULL,z TINYINT NOT NULL,m TINYINT NOT NULL,'
        'CONSTRAINT pk_geom_cols PRIMARY KEY (table_name, column_name),'
        'CONSTRAINT uk_gc_table_name_column_name UNIQUE '
        '(table_name, column_name),CONSTRAINT fk_gc_tn FOREIGN KEY '
        '(table_name) REFERENCES gpkg_contents(table_name),CONSTRAINT '
        'fk_gc_srs FOREIGN KEY (srs_id) REFERENCES '
        'gpkg_spatial_ref_sys (srs_id));'
    )
    ds.ExecuteSQL(
        'CREATE TABLE "test" ( "ogc_fid" INTEGER PRIMARY KEY AUTOINCREMENT '
        'NOT NULL, "poly" POLYGON, "pt" POINT, "area" REAL, '
        '"eas_id" INTEGER, "prfedea" TEXT(16))'
    )
    ds.ExecuteSQL(
        "INSERT INTO test VALUES(1,X'4750000300000000000000401f401d41000000e0"
        '5e511d4100000080322c5241000000001d2d52410103000000010000001b00000000'
        '0000c01a481d4100000080072d5241000000e0814b1d41000000001d2d5241000000'
        '40c44b1d41000000000f2d5241000000002c4c1d41000000a0002d52410000000077'
        '4d1d41000000c0072d5241000000a0c44e1d4100000080112d52410000002008501d'
        '41000000c0172d5241000000e05e511d4100000020dd2c5241000000405e511d4100'
        '000040cf2c524100000000f0501d41000000c0ba2c52410000008084501d41000000'
        '20af2c524100000040a94f1d4100000000a42c524100000080744e1d41000000a09a'
        '2c524100000040014f1d41000000c0852c5241000000e0e04d1d4100000020872c52'
        '4100000040f8441d41000000e0432c5241000000c012441d4100000080322c524100'
        '000000ff431d4100000020362c5241000000004b431d4100000080552c5241000000'
        '0030431d41000000c05d2c5241000000c09b421d4100000000712c524100000080d6'
        '411d4100000080912c52410000008027411d4100000040b22c5241000000401f401d'
        '4100000040d62c5241000000a043441d4100000060f02c524100000060aa461d4100'
        "000080ff2c5241000000c01a481d4100000080072d5241',"
        "X'4750000100000000010100000000000000804b1d41000000001d2d5241',"
        'NULL,170,NULL);'
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_contents VALUES('test','features','test',NULL,"
        "'2023-04-21T13:53:59.009Z',478315.53124999999998,4762880.5,"
        '481645.3125,4765610.4999999999998,0);'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns '
        "VALUES('test','poly','POLYGON',-1,0,0);"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns '
        "VALUES('test','pt','POINT',4326,0,0);"
    )
    ds = None

    gdal.ErrorReset()
    with _quiet_error_handler():
      ds = ogr.Open(out_filename)
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')
    self.assertEqual(ds.GetLayerCount(), 2)

    lyr = ds.GetLayerByName('test (poly)')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPolygon)
    self.assertEqual(lyr.GetGeometryColumn(), 'poly')
    self.assertEqual(lyr.GetSpatialRef().GetName(), 'Undefined Cartesian SRS')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 1)
    self.assertEqual(f['eas_id'], 170)
    self.assertTrue(
        f.GetGeometryRef().ExportToWkt().startswith('POLYGON ((479750')
    )

    lyr = ds.GetLayerByName('test (pt)')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint)
    self.assertEqual(lyr.GetGeometryColumn(), 'pt')
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), '4326')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetFID(), 1)
    self.assertEqual(f['eas_id'], 170)
    self.assertEqual(f.GetGeometryRef().ExportToWkt(), 'POINT (479968 4764788)')
    ds = None
    gdal.Unlink(out_filename)

  def test_ogr_gpkg_58(self) -> None:
    out_filename = str(self.vsimem_dir / 'ogr_gpkg_58.gpkg')
    poly_shp = ogr_util.GetTestFilePath('shape/poly/poly.shp')
    gdal.VectorTranslate(out_filename, poly_shp, format='GPKG')
    gdal.VectorTranslate(
        out_filename, poly_shp, format='GPKG', accessMode='overwrite'
    )

    ds = ogr.Open(out_filename)
    with ds.ExecuteSQL("SELECT HasSpatialIndex('poly', 'geom')") as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f.GetField(0), 1)
    ds = None
    gdal.Unlink(out_filename)

  def test_ogr_gpkg_59(self) -> None:
    out_filename = str(self.vsimem_dir / 'ogr_gpkg_59.gpkg')
    gdal.VectorTranslate(
        out_filename,
        ogr_util.GetTestFilePath('shape/poly/poly.shp'),
        format='GPKG',
        layerCreationOptions=['SPATIAL_INDEX=NO'],
    )

    ds = ogr.Open(out_filename, update=1)
    with ds.ExecuteSQL("SELECT CreateSpatialIndex('poly', 'geom')") as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertEqual(f.GetField(0), 1)
    ds = None

  def test_ogr_gpkg_savepoint(self) -> None:
    filename = str(self.vsimem_dir / 'ogr_gpkg_savepoint.gpkg')
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('foo')
    lyr.CreateField(ogr.FieldDefn('str', ogr.OFTString))
    f = ogr.Feature(lyr.GetLayerDefn())
    f['str'] = 'foo'
    lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    ds.StartTransaction()
    ds.ExecuteSQL('SAVEPOINT pt')
    lyr.DeleteFeature(1)
    ds.ExecuteSQL('ROLLBACK TO SAVEPOINT pt')
    f = ogr.Feature(lyr.GetLayerDefn())
    f['str'] = 'bar'
    lyr.CreateFeature(f)
    ds.CommitTransaction()
    ds = None

    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetFeatureCount(), 2)
    ds = None

  def test_ogr_gpkg_wal(self) -> None:
    ogrinfo_path = os.path.join(
        googletest.get_default_test_srcdir(),
        'google3/third_party/gdal/ogrinfo',
    )
    if not os.path.exists(ogrinfo_path):
      self.skipTest('ogrinfo binary not available')

    filename = self.tmp_dir / 'ogr_gpkg_wal.gpkg'
    with gdal.config_option('OGR_SQLITE_JOURNAL', 'WAL'):
      ds = self.driver.CreateDataSource(str(filename))
    ds.CreateLayer('foo')
    ds = None

    ds = ogr.Open(str(filename), update=1)
    self.assertTrue(filename.with_suffix('.gpkg-wal').exists())

    ds_ro = ogr.Open(str(filename))
    ds_ro.GetName()
    self.assertTrue(filename.with_suffix('.gpkg-wal').exists())

    subprocess.run([ogrinfo_path, str(filename)], check=True)
    self.assertTrue(filename.with_suffix('.gpkg-wal').exists())

    ds = None
    ds_ro = None

  def test_ogr_gpkg_nolock(self) -> None:
    def get_nolock(ds_obj: Any) -> bool:
      with ds_obj.ExecuteSQL('SELECT nolock', dialect='DEBUG') as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        return feat[0] == 1

    filename = self.tmp_dir / 'test_ogr_gpkg_#_nolock.gpkg'
    ds = self.driver.CreateDataSource(str(filename))
    lyr = ds.CreateLayer('foo')
    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    f = None
    ds = None

    full_filename = str(filename.absolute())
    ds = gdal.OpenEx(full_filename, gdal.OF_VECTOR, open_options=['NOLOCK=YES'])
    self.assertIsNotNone(ds)
    ds = None

    ds = gdal.OpenEx(str(filename), gdal.OF_VECTOR, open_options=['NOLOCK=YES'])
    self.assertIsNotNone(ds)
    self.assertTrue(get_nolock(ds))

    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    ds2 = ogr.Open(str(filename), update=1)
    lyr2 = ds2.GetLayer(0)
    f = ogr.Feature(lyr2.GetLayerDefn())
    self.assertEqual(lyr2.CreateFeature(f), ogr.OGRERR_NONE)
    f = None
    ds2 = None
    ds = None

    ds = gdal.OpenEx(
        str(filename),
        gdal.OF_VECTOR | gdal.OF_UPDATE,
        open_options=['NOLOCK=YES'],
    )
    self.assertIsNotNone(ds)
    self.assertFalse(get_nolock(ds))
    ds = None

    ds = ogr.Open(str(filename), update=1)
    with ds.ExecuteSQL('PRAGMA journal_mode = WAL') as sql_lyr:
      self.assertIsNotNone(sql_lyr)
    ds = None

    # Avoid OGR2SQLITE_static_register running inside sqlite3_open_v2 on the
    # WAL+nolock retry after earlier INDIRECT_SQLITE tests registered it.
    with gdal.config_option('OGR_SQLITE_STATIC_VIRTUAL_OGR', 'NO'):
      ds = gdal.OpenEx(
          str(filename), gdal.OF_VECTOR, open_options=['NOLOCK=YES']
      )
      self.assertIsNotNone(ds)
      self.assertFalse(get_nolock(ds))
      ds = None

    zip_gpkg = f"/vsizip/{self.getTestFilePath('poly.gpkg.zip')}/poly.gpkg"
    ds = gdal.OpenEx(zip_gpkg, gdal.OF_VECTOR, open_options=['NOLOCK=YES'])
    self.assertIsNotNone(ds)

  def test_ogr_gpkg_test_ogrsf(self) -> None:
    gpkg_ds = self._create_gpkg_ds()
    self._create_tpoly(gpkg_ds)
    self._create_tbl_linestring(gpkg_ds)

    gpkg_ds = _reopen(gpkg_ds)
    with gpkg_ds.ExecuteSQL('PRAGMA integrity_check') as sql_lyr:
      feat = sql_lyr.GetNextFeature()
      self.assertEqual(feat.GetField(0), 'ok', 'integrity check failed')
    gpkg_ds.Close()
    self.skipTest('test_ogrsf binary not available in google3')

  def test_ogr_gpkg_json(self) -> None:
    filename = str(self.vsimem_dir / 'ogr_gpkg_json.gpkg')
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('test')

    fld_defn = ogr.FieldDefn('test_json', ogr.OFTString)
    fld_defn.SetSubType(ogr.OFSTJSON)
    lyr.CreateField(fld_defn)
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetSubType(), ogr.OFSTJSON
    )

    with ds.ExecuteSQL('SELECT 1 FROM test') as _:
      pass

    fld_defn = ogr.FieldDefn('test2_json', ogr.OFTString)
    fld_defn.SetSubType(ogr.OFSTJSON)
    lyr.CreateField(fld_defn)
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetSubType(), ogr.OFSTJSON
    )

    fld_defn = ogr.FieldDefn('test_string', ogr.OFTString)
    lyr.CreateField(fld_defn)
    ds = None

    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetSubType(), ogr.OFSTJSON
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetSubType(), ogr.OFSTJSON
    )

    new_defn = ogr.FieldDefn('test_was_json_now_string', ogr.OFTString)
    self.assertEqual(
        lyr.AlterFieldDefn(
            lyr.GetLayerDefn().GetFieldIndex('test2_json'),
            new_defn,
            ogr.ALTER_ALL_FLAG,
        ),
        0,
    )

    new_defn = ogr.FieldDefn('test_was_string_now_json', ogr.OFTString)
    new_defn.SetSubType(ogr.OFSTJSON)
    self.assertEqual(
        lyr.AlterFieldDefn(
            lyr.GetLayerDefn().GetFieldIndex('test_string'),
            new_defn,
            ogr.ALTER_ALL_FLAG,
        ),
        0,
    )

    self.assertEqual(
        lyr.DeleteField(lyr.GetLayerDefn().GetFieldIndex('test_json')), 0
    )
    ds = None

    self.assertTrue(self.validate(filename))

    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(
            lyr.GetLayerDefn().GetFieldIndex('test_was_json_now_string')
        )
        .GetSubType(),
        ogr.OFSTNone,
    )
    self.assertEqual(
        lyr.GetLayerDefn()
        .GetFieldDefn(
            lyr.GetLayerDefn().GetFieldIndex('test_was_string_now_json')
        )
        .GetSubType(),
        ogr.OFSTJSON,
    )

    with ds.ExecuteSQL(
        "SELECT 1 FROM gpkg_data_columns WHERE table_name = 'test'"
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    ds = None

  def test_ogr_gpkg_invalid_values_in_records(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_invalid_date_content.gpkg')
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('test')

    lyr.CreateField(ogr.FieldDefn('dt', ogr.OFTDateTime))
    lyr.CreateField(ogr.FieldDefn('d', ogr.OFTDate))

    for _ in range(6):
      lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
    ds.ExecuteSQL("UPDATE test SET dt = 'foo' WHERE fid = 1")
    ds.ExecuteSQL("UPDATE test SET d = 'bar' WHERE fid = 2")
    ds.ExecuteSQL('UPDATE test SET dt = 3 WHERE fid = 3')
    ds.ExecuteSQL('UPDATE test SET d = 4 WHERE fid = 4')
    ds.ExecuteSQL("UPDATE test SET dt = '2020/01/21 12:34:56+01' WHERE fid = 5")
    ds.ExecuteSQL("UPDATE test SET d = '2020/01/21' WHERE fid = 6")

    lyr.ResetReading()

    gdal.ErrorReset()
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertEqual(
        gdal.GetLastErrorMsg(), 'Invalid content for record 1 in column dt: foo'
    )
    self.assertFalse(f.IsFieldSet('dt'))

    gdal.ErrorReset()
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertEqual(
        gdal.GetLastErrorMsg(), 'Invalid content for record 2 in column d: bar'
    )
    self.assertFalse(f.IsFieldSet('d'))

    gdal.ErrorReset()
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertEqual(
        gdal.GetLastErrorMsg(), 'Unexpected data type for record 3 in column dt'
    )
    self.assertFalse(f.IsFieldSet('dt'))

    gdal.ErrorReset()
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertEqual(
        gdal.GetLastErrorMsg(), 'Unexpected data type for record 4 in column d'
    )
    self.assertFalse(f.IsFieldSet('d'))

    gdal.ErrorReset()
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertEqual(
        gdal.GetLastErrorMsg(),
        'Non-conformant content for record 5 in column dt, '
        '2020/01/21 12:34:56+01, successfully parsed',
    )
    self.assertTrue(f.IsFieldSet('dt'))
    self.assertEqual(f['dt'], '2020/01/21 12:34:56+01')

    gdal.ErrorReset()
    with _quiet_error_handler():
      f = lyr.GetNextFeature()
    self.assertEqual(
        gdal.GetLastErrorMsg(),
        'Non-conformant content for record 6 in column d, 2020/01/21, '
        'successfully parsed',
    )
    self.assertTrue(f.IsFieldSet('d'))
    self.assertEqual(f['d'], '2020/01/21')

    ds = None

  def test_ogr_gpkg_mixed_dimensionality_unknown_layer_geometry_type(
      self,
  ) -> None:
    filename = str(
        self.vsimem_dir
        / 'test_ogr_gpkg_mixed_dimensionality_unknown_layer_geometry_type.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('test')

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
    lyr.CreateFeature(f)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2 3)'))
    lyr.CreateFeature(f)
    ds = None

    self.assertTrue(self.validate(filename))

    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetGeomType(), ogr.wkbUnknown)

    with ds.ExecuteSQL('SELECT z FROM gpkg_geometry_columns') as sql_lyr:
      self.assertEqual(sql_lyr.GetNextFeature().GetField(0), 2)
    ds = None

  def test_ogr_gpkg_z_or_m_geometry_in_non_zm_layer(self) -> None:
    fname = str(self.vsimem_dir / 'tmp_z_or_m.gpkg')
    ds = self.driver.Create(fname, 0, 0, 0, gdal.GDT_Unknown)
    lyr = ds.CreateLayer('foo', geom_type=ogr.wkbPoint)

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT Z (1 2 3)'))
    with _quiet_error_handler():
      lyr.CreateFeature(feat)
      self.assertEqual(
          gdal.GetLastErrorMsg(),
          "Layer 'foo' has been declared with non-Z geometry type Point, "
          'but it does contain geometries with Z. Setting the Z=2 hint into '
          'gpkg_geometry_columns',
      )

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT M (1 2 3)'))
    with _quiet_error_handler():
      lyr.CreateFeature(feat)
      self.assertEqual(
          gdal.GetLastErrorMsg(),
          "Layer 'foo' has been declared with non-M geometry type Point, "
          'but it does contain geometries with M. Setting the M=2 hint into '
          'gpkg_geometry_columns',
      )

    ds = None

    ds = ogr.Open(fname)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPointZM)

  def test_ogr_gpkg_fixup_wrong_rtree_trigger(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_fixup_wrong_rtree_trigger.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    ds.CreateLayer('test-with-dash')
    ds.CreateLayer('test2')
    ds = None
    with _quiet_error_handler():
      ds = ogr.Open(filename, update=1)
      ds.ExecuteSQL('DROP TRIGGER "rtree_test-with-dash_geometry_update3"')
      wrong_trigger = (
          'CREATE TRIGGER "rtree_test-with-dash_geometry_update3" AFTER '
          'UPDATE OF "geometry" ON "test-with-dash" WHEN '
          'OLD."fid" != NEW."fid" AND (NEW."geometry" NOTNULL AND NOT '
          'ST_IsEmpty(NEW."geometry")) BEGIN DELETE FROM '
          '"rtree_test_geometry" WHERE id = OLD."fid"; INSERT OR REPLACE '
          'INTO "rtree_test_geometry" VALUES (NEW."fid",ST_MinX(NEW."geometry"'
          '), ST_MaxX(NEW."geometry"),ST_MinY(NEW."geometry"), '
          'ST_MaxY(NEW."geometry")); END'
      )
      ds.ExecuteSQL(wrong_trigger)

      ds.ExecuteSQL('DROP TRIGGER rtree_test2_geometry_update3')
      wrong_trigger2 = (
          'CREATE TRIGGER "rtree_test2_geometry_update3" AFTER UPDATE OF   '
          'geometry    ON test2 WHEN OLD."fid" != NEW."fid" AND '
          '(NEW."geometry" NOTNULL AND NOT ST_IsEmpty(NEW."geometry")) BEGIN '
          'DELETE FROM "rtree_test_geometry" WHERE id = OLD."fid"; INSERT OR '
          'REPLACE INTO "rtree_test_geometry" VALUES (NEW."fid",'
          'ST_MinX(NEW."geometry"), ST_MaxX(NEW."geometry"),'
          'ST_MinY(NEW."geometry"), ST_MaxY(NEW."geometry")); END'
      )
      ds.ExecuteSQL(wrong_trigger2)
      ds = None

    ds = ogr.Open(filename)
    with ds.ExecuteSQL(
        "SELECT sql FROM sqlite_master WHERE type = 'trigger' AND "
        "name = 'rtree_test-with-dash_geometry_update3'"
    ) as sql_lyr:
      sql = sql_lyr.GetNextFeature()['sql']
    ds = None
    self.assertEqual(sql, wrong_trigger)

    ds = ogr.Open(filename, update=1)
    with ds.ExecuteSQL(
        "SELECT sql FROM sqlite_master WHERE type = 'trigger' AND "
        "name = 'rtree_test-with-dash_geometry_update3'"
    ) as sql_lyr:
      sql = sql_lyr.GetNextFeature()['sql']
    with ds.ExecuteSQL(
        "SELECT sql FROM sqlite_master WHERE type = 'trigger' AND "
        "name = 'rtree_test2_geometry_update3'"
    ) as sql_lyr:
      sql2 = sql_lyr.GetNextFeature()['sql']
    ds = None

    gdal.Unlink(filename)
    self.assertEqual(
        sql,
        'CREATE TRIGGER "rtree_test-with-dash_geometry_update3" AFTER UPDATE '
        'ON "test-with-dash" WHEN OLD."fid" != NEW."fid" AND '
        '(NEW."geometry" NOTNULL AND NOT ST_IsEmpty(NEW."geometry")) BEGIN '
        'DELETE FROM "rtree_test_geometry" WHERE id = OLD."fid"; INSERT OR '
        'REPLACE INTO "rtree_test_geometry" VALUES (NEW."fid",'
        'ST_MinX(NEW."geometry"), ST_MaxX(NEW."geometry"),'
        'ST_MinY(NEW."geometry"), ST_MaxY(NEW."geometry")); END',
    )
    self.assertEqual(
        sql2,
        'CREATE TRIGGER "rtree_test2_geometry_update3" AFTER UPDATE    ON '
        'test2 WHEN OLD."fid" != NEW."fid" AND (NEW."geometry" NOTNULL AND NOT '
        'ST_IsEmpty(NEW."geometry")) BEGIN DELETE FROM "rtree_test_geometry" '
        'WHERE id = OLD."fid"; INSERT OR REPLACE INTO "rtree_test_geometry" '
        'VALUES (NEW."fid",ST_MinX(NEW."geometry"), ST_MaxX(NEW."geometry"),'
        'ST_MinY(NEW."geometry"), ST_MaxY(NEW."geometry")); END',
    )

  def test_ogr_gpkg_prelude_statements(self) -> None:
    fname = str(self.vsimem_dir / 'test_prelude.gpkg')
    gdal.VectorTranslate(
        fname, ogr_util.GetTestFilePath('shape/poly/poly.shp'), format='GPKG'
    )
    ds = gdal.OpenEx(
        fname,
        open_options=[f"PRELUDE_STATEMENTS=ATTACH DATABASE '{fname}' AS other"],
    )
    with ds.ExecuteSQL(
        'SELECT * FROM poly JOIN other.poly USING (eas_id)'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 10)

  def test_ogr_gpkg_prelude_statements_after_spatialite_loading(self) -> None:
    fname = str(self.vsimem_dir / 'test_prelude_spatialite.gpkg')
    gdal.VectorTranslate(
        fname, ogr_util.GetTestFilePath('shape/poly/poly.shp'), format='GPKG'
    )

    with ogr.Open(fname) as ds:
      if not _has_spatialite_4_3_or_later(ds):
        self.skipTest('spatialite missing')

    with gdal.OpenEx(
        fname,
        open_options=['PRELUDE_STATEMENTS=SELECT setdecimalprecision(1)'],
    ) as ds:
      with ds.ExecuteSQL('SELECT ST_AsText(geom) FROM poly LIMIT 1') as sql_lyr:
        f = sql_lyr.GetNextFeature()
        self.assertTrue(f[0].startswith('POLYGON((479819.8 4765180.5,'))

  def test_ogr_gpkg_datetime_timezones(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_datetime_timezones.gpkg')
    ds = self.driver.CreateDataSource(
        filename, options=['DATETIME_FORMAT=UTC', 'VERSION=1.2']
    )
    lyr = ds.CreateLayer('test')
    lyr.CreateField(ogr.FieldDefn('dt', ogr.OFTDateTime))
    for val in [
        '2020/01/01 01:34:56',
        '2020/01/01 01:34:56+00',
        '2020/01/01 01:34:56.789+02',
    ]:
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetField('dt', val)
      lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('dt'), '2020/01/01 01:34:56+00')
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('dt'), '2020/01/01 01:34:56+00')
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('dt'), '2019/12/31 23:34:56.789+00')

    with ds.ExecuteSQL("SELECT dt || '' FROM test") as sql_lyr:
      f = sql_lyr.GetNextFeature()
      # Milliseconds are written to be strictly compliant with GPKG 1.2.
      self.assertEqual(f.GetField(0), '2020-01-01T01:34:56.000Z')
    ds = None

  def test_ogr_gpkg_datetime_timezones_gpkg_1_4(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_datetime_timezones_1_4.gpkg'
    )
    ds = self.driver.CreateDataSource(
        filename, options=['DATETIME_FORMAT=UTC', 'VERSION=1.4']
    )
    lyr = ds.CreateLayer('test')
    lyr.CreateField(ogr.FieldDefn('dt', ogr.OFTDateTime))
    for val in [
        '2020/01/01 01:34:56',
        '2020/01/01 01:34:56+00',
        '2020/01/01 01:34:56.789+02',
    ]:
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetField('dt', val)
      lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('dt'), '2020/01/01 01:34:56+00')
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('dt'), '2020/01/01 01:34:56+00')
    f = lyr.GetNextFeature()
    self.assertEqual(f.GetField('dt'), '2019/12/31 23:34:56.789+00')

    with ds.ExecuteSQL("SELECT dt || '' FROM test") as sql_lyr:
      f = sql_lyr.GetNextFeature()
      # Milliseconds are not required since GPKG 1.4.
      self.assertEqual(f.GetField(0), '2020-01-01T01:34:56Z')
    ds = None

  def test_abort_sql(self) -> None:
    filename = self.getTestFilePath('poly_non_conformant.gpkg')
    ds = ogr.Open(filename)

    def abort_after_delay() -> None:
      self.assertEqual(ds.AbortSQL(), ogr.OGRERR_NONE)

    t = threading.Timer(0.5, abort_after_delay)
    t.start()

    start = time.time()
    sql = """
        WITH RECURSIVE r(i) AS (
            VALUES(0)
            UNION ALL
            SELECT i FROM r
            LIMIT 100000000
            )
        SELECT i FROM r WHERE i = 1;"""

    with _quiet_error_handler():
      self.assertIsNone(ds.ExecuteSQL(sql))
    t.join()

    end = time.time()
    self.assertLess(int(end - start), 2)

    ds2 = gdal.OpenEx(filename, gdal.OF_VECTOR)

    def abort_after_delay2() -> None:
      self.assertEqual(ds2.AbortSQL(), ogr.OGRERR_NONE)

    t = threading.Timer(0.5, abort_after_delay2)
    t.start()

    start = time.time()
    with _quiet_error_handler():
      self.assertIsNone(ds2.ExecuteSQL(sql))
    t.join()

    end = time.time()
    self.assertLess(int(end - start), 2)

  def test_ogr_gpkg_st_transform_no_record_spatial_ref_sys(self) -> None:
    if sys.platform == 'win32':
      self.skipTest('f.GetGeometryRef() returns None on Windows CI')

    ds = self.driver.CreateDataSource(str(self.vsimem_dir / 'test.gpkg'))
    lyr = ds.CreateLayer('test')
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (500000 0)'))
    lyr.CreateFeature(f)
    f = None

    if not _has_spatialite_4_3_or_later(ds):
      ds = None
      self.skipTest('Spatialite missing or too old')

    with ds.ExecuteSQL(
        'SELECT ST_Transform(SetSRID(geom, 32631), 32731) FROM test'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertEqual(
          f.GetGeometryRef().ExportToWkt(), 'POINT (500000 10000000)'
      )
      f = None

    ds = None

  def test_ogr_gpkg_deferred_spi_creation(self) -> None:
    def has_spi(ds_obj: Any) -> bool:
      with ds_obj.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'rtree_test_geom'",
          dialect='DEBUG',
      ) as sql_lyr:
        return sql_lyr.GetNextFeature() is not None

    ds = self.driver.CreateDataSource(str(self.vsimem_dir / 'test.gpkg'))

    lyr = ds.CreateLayer('test')
    self.assertFalse(has_spi(ds))

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
    lyr.CreateFeature(f)
    fid = f.GetFID()
    f = None
    self.assertFalse(has_spi(ds))

    lyr.ResetReading()
    self.assertIsNotNone(lyr.GetNextFeature())
    self.assertFalse(has_spi(ds))

    self.assertIsNotNone(lyr.GetFeature(fid))
    self.assertFalse(has_spi(ds))

    self.assertEqual(
        lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString)), ogr.OGRERR_NONE
    )
    self.assertFalse(has_spi(ds))

    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertFalse(has_spi(ds))

    with ds.ExecuteSQL('SELECT 1') as sql_lyr:
      self.assertIsNotNone(sql_lyr)
    self.assertTrue(has_spi(ds))

    # GetNextFeature() with spatial filter should cause SPI creation.
    ds.ExecuteSQL('DELLAYER:test')
    lyr = ds.CreateLayer('test')
    self.assertFalse(has_spi(ds))

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
    lyr.CreateFeature(f)
    f = None
    self.assertFalse(has_spi(ds))

    lyr.SetSpatialFilterRect(-1, -1, 1, 1)

    lyr.ResetReading()
    self.assertIsNotNone(lyr.GetNextFeature())
    self.assertTrue(has_spi(ds))

    ds = None

  @parameterized.parameters('1.2', '1.4')
  def test_ogr_gpkg_deferred_spi_update(self, gpkg_version: str) -> None:
    def has_spi_triggers(ds_obj: Any) -> bool:
      with ds_obj.ExecuteSQL(
          "SELECT * FROM sqlite_master WHERE type = 'trigger' AND name LIKE"
          " 'rtree_test_geom%'",
          dialect='DEBUG',
      ) as sql_lyr:
        res = sql_lyr.GetFeatureCount()
      return res == 6 or res == 7

    filename = str(self.vsimem_dir / 'test.gpkg')

    # Basic test
    ds = self.driver.CreateDataSource(
        filename, options=[f'VERSION={gpkg_version}']
    )
    ds.CreateLayer('test')
    ds = None
    with gdal.config_option('OGR_GPKG_DEFERRED_SPI_UPDATE_THRESHOLD', '2'):
      ds = ogr.Open(filename, update=1)
      lyr = ds.GetLayer(0)

      ds.StartTransaction()

      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
      lyr.CreateFeature(f)
      self.assertTrue(has_spi_triggers(ds))

      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (1 1)'))
      lyr.CreateFeature(f)
      self.assertFalse(has_spi_triggers(ds))

      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (2 2)'))
      lyr.CreateFeature(f)
      self.assertFalse(has_spi_triggers(ds))

      with ds.ExecuteSQL(
          'SELECT * FROM rtree_test_geom', dialect='DEBUG'
      ) as sql_lyr:
        res = sql_lyr.GetFeatureCount()
      self.assertEqual(res, 2)

      ds.CommitTransaction()
      self.assertTrue(has_spi_triggers(ds))

      with ds.ExecuteSQL(
          'SELECT * FROM rtree_test_geom', dialect='DEBUG'
      ) as sql_lyr:
        res = sql_lyr.GetFeatureCount()
      self.assertEqual(res, 3)

      ds = None

    # Check effect of RollbackTransaction()
    ds = self.driver.CreateDataSource(
        filename, options=[f'VERSION={gpkg_version}']
    )
    ds.CreateLayer('test')
    ds = None
    with gdal.config_option('OGR_GPKG_DEFERRED_SPI_UPDATE_THRESHOLD', '1'):
      ds = ogr.Open(filename, update=1)
      lyr = ds.GetLayer(0)

      ds.StartTransaction()

      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
      lyr.CreateFeature(f)
      self.assertFalse(has_spi_triggers(ds))

      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (1 1)'))
      lyr.CreateFeature(f)
      self.assertFalse(has_spi_triggers(ds))

      with ds.ExecuteSQL(
          'SELECT * FROM rtree_test_geom', dialect='DEBUG'
      ) as sql_lyr:
        res = sql_lyr.GetFeatureCount()
      self.assertEqual(res, 1)

      ds.RollbackTransaction()
      self.assertTrue(has_spi_triggers(ds))

      with ds.ExecuteSQL(
          'SELECT * FROM rtree_test_geom', dialect='DEBUG'
      ) as sql_lyr:
        res = sql_lyr.GetFeatureCount()
      self.assertEqual(res, 0)

      ds = None

    # Check that GetNextFeature() with a spatial filter causes flushing of
    # deferred SPI values.
    ds = self.driver.CreateDataSource(
        filename, options=[f'VERSION={gpkg_version}']
    )
    ds.CreateLayer('test')
    ds = None
    with gdal.config_option('OGR_GPKG_DEFERRED_SPI_UPDATE_THRESHOLD', '1'):
      ds = ogr.Open(filename, update=1)
      lyr = ds.GetLayer(0)

      ds.StartTransaction()

      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
      lyr.CreateFeature(f)
      self.assertFalse(has_spi_triggers(ds))

      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (1 1)'))
      lyr.CreateFeature(f)

      lyr.SetSpatialFilterRect(0, 0, 1, 1)
      lyr.ResetReading()
      self.assertIsNotNone(lyr.GetNextFeature())
      self.assertTrue(has_spi_triggers(ds))
      self.assertIsNotNone(lyr.GetNextFeature())
      self.assertIsNone(lyr.GetNextFeature())
      ds = None

  def test_ogr_gpkg_field_domains(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')

    # Test write support
    ds = gdal.GetDriverByName('GPKG').Create(
        filename, 0, 0, 0, gdal.GDT_Unknown
    )

    self.assertTrue(ds.TestCapability(ogr.ODsCAddFieldDomain))
    self.assertIsNone(ds.GetFieldDomainNames())
    self.assertIsNone(ds.GetFieldDomain('does_not_exist'))

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateRangeFieldDomain(
                'range_domain_int',
                'my desc',
                ogr.OFTInteger,
                ogr.OFSTNone,
                1,
                True,
                2,
                False,
            )
        )
    )
    self.assertIsNotNone(ds.GetFieldDomain('range_domain_int'))
    self.assertEqual(set(ds.GetFieldDomainNames()), {'range_domain_int'})

    with _error_raised(
        gdal.CE_Failure, match='A domain of identical name already exists'
    ):
      self.assertFalse(
          ds.AddFieldDomain(
              ogr.CreateRangeFieldDomain(
                  'range_domain_int',
                  'my desc',
                  ogr.OFTInteger,
                  ogr.OFSTNone,
                  1,
                  True,
                  2,
                  True,
              )
          )
      )

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateRangeFieldDomain(
                'range_domain_int64',
                '',
                ogr.OFTInteger64,
                ogr.OFSTNone,
                -1234567890123,
                False,
                1234567890123,
                True,
            )
        )
    )
    self.assertIsNotNone(ds.GetFieldDomain('range_domain_int64'))

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateRangeFieldDomain(
                'range_domain_real',
                '',
                ogr.OFTReal,
                ogr.OFSTNone,
                1.5,
                True,
                2.5,
                True,
            )
        )
    )
    self.assertIsNotNone(ds.GetFieldDomain('range_domain_real'))

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateRangeFieldDomain(
                'range_domain_real_inf',
                '',
                ogr.OFTReal,
                ogr.OFSTNone,
                -math.inf,
                True,
                math.inf,
                True,
            )
        )
    )
    self.assertIsNotNone(ds.GetFieldDomain('range_domain_real_inf'))

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateGlobFieldDomain(
                'glob_domain', 'my desc', ogr.OFTString, ogr.OFSTNone, '*'
            )
        )
    )
    self.assertIsNotNone(ds.GetFieldDomain('glob_domain'))

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain',
                '',
                ogr.OFTInteger64,
                ogr.OFSTNone,
                {1: 'one', '2': None},
            )
        )
    )
    self.assertIsNotNone(ds.GetFieldDomain('enum_domain'))

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_int_single',
                'my desc',
                ogr.OFTInteger,
                ogr.OFSTNone,
                {1: 'one'},
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_int',
                '',
                ogr.OFTInteger,
                ogr.OFSTNone,
                {1: 'one', 2: 'two'},
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_int64_single_1',
                '',
                ogr.OFTInteger64,
                ogr.OFSTNone,
                {1234567890123: '1234567890123'},
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_int64_single_2',
                '',
                ogr.OFTInteger64,
                ogr.OFSTNone,
                {-1234567890123: '-1234567890123'},
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_int64',
                '',
                ogr.OFTInteger64,
                ogr.OFSTNone,
                {1: 'one', 1234567890123: '1234567890123', 3: 'three'},
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_real_single',
                '',
                ogr.OFTReal,
                ogr.OFSTNone,
                {1.5: 'one dot five'},
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_real',
                '',
                ogr.OFTReal,
                ogr.OFSTNone,
                {
                    1: 'one',
                    1.5: 'one dot five',
                    1234567890123: '1234567890123',
                    3: 'three',
                },
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_string_single',
                '',
                ogr.OFTString,
                ogr.OFSTNone,
                {'three': 'three'},
            )
        )
    )
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain_guess_string',
                '',
                ogr.OFTString,
                ogr.OFSTNone,
                {1: 'one', 1.5: 'one dot five', 'three': 'three', 4: 'four'},
            )
        )
    )

    self.assertEqual(
        len(ds.GetFieldDomainNames()), len(set(ds.GetFieldDomainNames()))
    )
    expected_domains = {
        'enum_domain',
        'enum_domain_guess_int',
        'enum_domain_guess_int64',
        'enum_domain_guess_int64_single_1',
        'enum_domain_guess_int64_single_2',
        'enum_domain_guess_int_single',
        'enum_domain_guess_real',
        'enum_domain_guess_real_single',
        'enum_domain_guess_string',
        'enum_domain_guess_string_single',
        'glob_domain',
        'range_domain_int',
        'range_domain_int64',
        'range_domain_real',
        'range_domain_real_inf',
    }
    self.assertEqual(set(ds.GetFieldDomainNames()), expected_domains)

    lyr = ds.CreateLayer('test')

    fld_defn = ogr.FieldDefn('with_range_domain_int', ogr.OFTInteger)
    fld_defn.SetDomainName('range_domain_int')
    lyr.CreateField(fld_defn)

    fld_defn = ogr.FieldDefn('with_range_domain_int64', ogr.OFTInteger64)
    fld_defn.SetDomainName('range_domain_int64')
    lyr.CreateField(fld_defn)

    fld_defn = ogr.FieldDefn('with_range_domain_real', ogr.OFTReal)
    fld_defn.SetDomainName('range_domain_real')
    lyr.CreateField(fld_defn)

    fld_defn = ogr.FieldDefn('with_glob_domain', ogr.OFTString)
    fld_defn.SetDomainName('glob_domain')
    lyr.CreateField(fld_defn)

    fld_defn = ogr.FieldDefn('with_enum_domain', ogr.OFTInteger64)
    fld_defn.SetDomainName('enum_domain')
    lyr.CreateField(fld_defn)

    fld_defn = ogr.FieldDefn('without_domain_initially', ogr.OFTInteger)
    lyr.CreateField(fld_defn)

    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    # Test read support
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR)

    with ds.ExecuteSQL('SELECT * FROM gpkg_data_column_constraints') as sql_lyr:
      self.assertIsNotNone(sql_lyr)

    self.assertEqual(set(ds.GetFieldDomainNames()), expected_domains)

    domain = ds.GetFieldDomain('range_domain_int')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetName(), 'range_domain_int')
    self.assertEqual(domain.GetDescription(), 'my desc')
    self.assertEqual(domain.GetDomainType(), ogr.OFDT_RANGE)
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger)
    self.assertEqual(domain.GetMinAsDouble(), 1.0)
    self.assertTrue(domain.IsMinInclusive())
    self.assertEqual(domain.GetMaxAsDouble(), 2.0)
    self.assertFalse(domain.IsMaxInclusive())

    domain = ds.GetFieldDomain('range_domain_int64')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetName(), 'range_domain_int64')
    self.assertEqual(domain.GetDescription(), '')
    self.assertEqual(domain.GetDomainType(), ogr.OFDT_RANGE)
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger64)
    self.assertEqual(domain.GetMinAsDouble(), -1234567890123)
    self.assertFalse(domain.IsMinInclusive())
    self.assertEqual(domain.GetMaxAsDouble(), 1234567890123)
    self.assertTrue(domain.IsMaxInclusive())

    domain = ds.GetFieldDomain('range_domain_real')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetName(), 'range_domain_real')
    self.assertEqual(domain.GetDescription(), '')
    self.assertEqual(domain.GetDomainType(), ogr.OFDT_RANGE)
    self.assertEqual(domain.GetFieldType(), ogr.OFTReal)
    self.assertEqual(domain.GetMinAsDouble(), 1.5)
    self.assertTrue(domain.IsMinInclusive())
    self.assertEqual(domain.GetMaxAsDouble(), 2.5)
    self.assertTrue(domain.IsMaxInclusive())

    domain = ds.GetFieldDomain('range_domain_real_inf')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetName(), 'range_domain_real_inf')
    self.assertEqual(domain.GetDescription(), '')
    self.assertEqual(domain.GetDomainType(), ogr.OFDT_RANGE)
    self.assertEqual(domain.GetFieldType(), ogr.OFTReal)
    self.assertEqual(domain.GetMinAsDouble(), -math.inf)
    self.assertTrue(domain.IsMinInclusive())
    self.assertEqual(domain.GetMaxAsDouble(), math.inf)
    self.assertTrue(domain.IsMaxInclusive())

    domain = ds.GetFieldDomain('glob_domain')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetName(), 'glob_domain')
    self.assertEqual(domain.GetDescription(), 'my desc')
    self.assertEqual(domain.GetDomainType(), ogr.OFDT_GLOB)
    self.assertEqual(domain.GetFieldType(), ogr.OFTString)
    self.assertEqual(domain.GetGlob(), '*')

    domain = ds.GetFieldDomain('enum_domain')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetName(), 'enum_domain')
    self.assertEqual(domain.GetDescription(), '')
    self.assertEqual(domain.GetDomainType(), ogr.OFDT_CODED)
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger64)
    self.assertEqual(domain.GetEnumeration(), {'1': 'one', '2': None})

    domain = ds.GetFieldDomain('enum_domain_guess_int_single')
    self.assertEqual(domain.GetDescription(), 'my desc')
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger)

    domain = ds.GetFieldDomain('enum_domain_guess_int')
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger)

    domain = ds.GetFieldDomain('enum_domain_guess_int64_single_1')
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger64)

    domain = ds.GetFieldDomain('enum_domain_guess_int64_single_2')
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger64)

    domain = ds.GetFieldDomain('enum_domain_guess_int64')
    self.assertEqual(domain.GetFieldType(), ogr.OFTInteger64)

    domain = ds.GetFieldDomain('enum_domain_guess_real_single')
    self.assertEqual(domain.GetFieldType(), ogr.OFTReal)

    domain = ds.GetFieldDomain('enum_domain_guess_real')
    self.assertEqual(domain.GetFieldType(), ogr.OFTReal)

    domain = ds.GetFieldDomain('enum_domain_guess_string_single')
    self.assertEqual(domain.GetFieldType(), ogr.OFTString)

    domain = ds.GetFieldDomain('enum_domain_guess_string')
    self.assertEqual(domain.GetFieldType(), ogr.OFTString)

    lyr = ds.GetLayerByName('test')
    lyr_defn = lyr.GetLayerDefn()
    fld_defn = lyr_defn.GetFieldDefn(
        lyr_defn.GetFieldIndex('with_range_domain_int')
    )
    self.assertEqual(fld_defn.GetDomainName(), 'range_domain_int')

    if self.driver.GetMetadataItem('SQLITE_HAS_COLUMN_METADATA'):
      with ds.ExecuteSQL('SELECT with_range_domain_int FROM test') as sql_lyr:
        self.assertEqual(
            sql_lyr.GetLayerDefn().GetFieldDefn(0).GetDomainName(),
            'range_domain_int',
        )

    ds = None

    # Test AlterFieldDefn() support
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    lyr = ds.GetLayer(0)
    lyr_defn = lyr.GetLayerDefn()

    # Unset domain name
    idx = lyr_defn.GetFieldIndex('with_range_domain_int')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    fld_defn = ogr.FieldDefn(fld_defn.GetName(), fld_defn.GetType())
    fld_defn.SetDomainName('')
    self.assertEqual(lyr.AlterFieldDefn(idx, fld_defn, ogr.ALTER_ALL_FLAG), 0)

    # Change domain name
    idx = lyr_defn.GetFieldIndex('with_range_domain_int64')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    fld_defn = ogr.FieldDefn(fld_defn.GetName(), fld_defn.GetType())
    fld_defn.SetDomainName('with_enum_domain')
    self.assertEqual(lyr.AlterFieldDefn(idx, fld_defn, ogr.ALTER_ALL_FLAG), 0)

    # Set domain name
    idx = lyr_defn.GetFieldIndex('without_domain_initially')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    fld_defn = ogr.FieldDefn(fld_defn.GetName(), fld_defn.GetType())
    fld_defn.SetDomainName('range_domain_int')
    self.assertEqual(lyr.AlterFieldDefn(idx, fld_defn, ogr.ALTER_ALL_FLAG), 0)

    # Don't change anything
    idx = lyr_defn.GetFieldIndex('with_glob_domain')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    self.assertEqual(lyr.AlterFieldDefn(idx, fld_defn, ogr.ALTER_ALL_FLAG), 0)

    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    # Test read support
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR)
    lyr = ds.GetLayer(0)
    lyr_defn = lyr.GetLayerDefn()

    idx = lyr_defn.GetFieldIndex('with_range_domain_int')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    self.assertEqual(fld_defn.GetDomainName(), '')

    idx = lyr_defn.GetFieldIndex('with_range_domain_int64')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    self.assertEqual(fld_defn.GetDomainName(), 'with_enum_domain')

    idx = lyr_defn.GetFieldIndex('without_domain_initially')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    self.assertEqual(fld_defn.GetDomainName(), 'range_domain_int')

    idx = lyr_defn.GetFieldIndex('with_glob_domain')
    fld_defn = lyr_defn.GetFieldDefn(idx)
    self.assertEqual(fld_defn.GetDomainName(), 'glob_domain')

    self.assertEqual(set(ds.GetFieldDomainNames()), expected_domains)

    self.assertEqual(ds.TestCapability(ogr.ODsCAddFieldDomain), 0)
    self.assertEqual(ds.TestCapability(ogr.ODsCUpdateFieldDomain), 0)
    self.assertEqual(ds.TestCapability(ogr.ODsCDeleteFieldDomain), 0)

    ds = None

    # Test UpdateFieldDomain()
    with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
      with _error_raised(
          gdal.CE_Failure, match='not supported on read-only dataset'
      ):
        self.assertFalse(
            ds.UpdateFieldDomain(
                ogr.CreateRangeFieldDomain(
                    'range_domain_int',
                    'new desc',
                    ogr.OFTInteger,
                    ogr.OFSTNone,
                    10,
                    False,
                    20,
                    True,
                )
            )
        )
    with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      self.assertEqual(ds.TestCapability(ogr.ODsCUpdateFieldDomain), 1)

      self.assertIs(
          ds.UpdateFieldDomain(
              ogr.CreateCodedFieldDomain(
                  'non_existing',
                  '',
                  ogr.OFTInteger64,
                  ogr.OFSTNone,
                  {1: 'one', '2': None},
              )
          ),
          False,
      )

      self.assertTrue(
          ds.UpdateFieldDomain(
              ogr.CreateRangeFieldDomain(
                  'range_domain_int',
                  'new desc',
                  ogr.OFTInteger,
                  ogr.OFSTNone,
                  10,
                  False,
                  20,
                  True,
              )
          )
      )

    with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
      domain = ds.GetFieldDomain('range_domain_int')
      self.assertIsNotNone(domain)
      self.assertEqual(domain.GetName(), 'range_domain_int')
      self.assertEqual(domain.GetDescription(), 'new desc')
      self.assertEqual(domain.GetDomainType(), ogr.OFDT_RANGE)
      self.assertEqual(domain.GetFieldType(), ogr.OFTInteger)
      self.assertEqual(domain.GetMinAsDouble(), 10.0)
      self.assertFalse(domain.IsMinInclusive())
      self.assertEqual(domain.GetMaxAsDouble(), 20.0)
      self.assertTrue(domain.IsMaxInclusive())

    # Test DeleteFieldDomain()
    with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
      with _error_raised(
          gdal.CE_Failure, match='not supported on read-only dataset'
      ):
        self.assertIs(ds.DeleteFieldDomain('range_domain_int'), False)

    with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      self.assertEqual(ds.TestCapability(ogr.ODsCDeleteFieldDomain), 1)

      self.assertIs(ds.DeleteFieldDomain('non_existing'), False)
      domain_names = ds.GetFieldDomainNames()
      for name in domain_names:
        self.assertTrue(ds.DeleteFieldDomain(name))
        self.assertIsNone(ds.GetFieldDomain(name))

    with gdal.OpenEx(filename, gdal.OF_VECTOR) as ds:
      self.assertIsNone(ds.GetFieldDomainNames())

  def test_ogr_gpkg_field_domains_errors(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')

    ds = gdal.GetDriverByName('GPKG').Create(
        filename, 0, 0, 0, gdal.GDT_Unknown
    )
    ds.CreateLayer('test')
    # DDL lacks NOT NULL constraints on constraint_name and constraint_type.
    ds.ExecuteSQL(
        'CREATE TABLE gpkg_data_column_constraints ('
        'constraint_name TEXT,constraint_type TEXT,value TEXT,'
        'min NUMERIC,min_is_inclusive BOOLEAN,'
        'max NUMERIC,max_is_inclusive BOOLEAN,description TEXT)'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_data_column_constraints VALUES '
        "('null_constraint_type', NULL, NULL, NULL, NULL, NULL, NULL, NULL)"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_data_column_constraints VALUES '
        "('invalid_constraint_type', 'invalid', NULL, NULL, NULL, NULL, NULL,"
        ' NULL)'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_data_column_constraints VALUES '
        "('mix_glob_enum', 'glob', '*', NULL, NULL, NULL, NULL, NULL)"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_data_column_constraints VALUES '
        "('mix_glob_enum', 'enum', 'foo', NULL, NULL, NULL, NULL, 'bar')"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_data_column_constraints VALUES '
        "('null_in_enum_code', 'enum', NULL, NULL, NULL, NULL, NULL, 'bar')"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_data_column_constraints VALUES '
        "('null_in_glob_value', 'glob', NULL, NULL, NULL, NULL, NULL, NULL)"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_data_column_constraints VALUES '
        "('null_in_range', 'range', NULL, NULL, NULL, NULL, NULL, NULL)"
    )
    ds = None

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR)

    self.assertIsNone(ds.GetFieldDomain('null_constraint_type'))

    for bad_name in (
        'invalid_constraint_type',
        'mix_glob_enum',
        'null_in_enum_code',
        'null_in_glob_value',
    ):
      with _quiet_error_handler():
        gdal.ErrorReset()
        self.assertIsNone(ds.GetFieldDomain(bad_name))
        self.assertNotEqual(gdal.GetLastErrorMsg(), '')

    # Non-conformant, but accepted.
    domain = ds.GetFieldDomain('null_in_range')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetMinAsDouble(), -math.inf)
    self.assertEqual(domain.GetMaxAsDouble(), math.inf)

    ds = None

  def test_ogr_gpkg_field_domain_gpkg_1_0(self) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')

    ds = gdal.GetDriverByName('GPKG').Create(
        filename, 0, 0, 0, gdal.GDT_Unknown, options=['VERSION=1.0']
    )
    ds.CreateLayer('test')
    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateRangeFieldDomain(
                'range_domain_int',
                'my desc',
                ogr.OFTReal,
                ogr.OFSTNone,
                1.5,
                True,
                2.5,
                False,
            )
        )
    )
    ds = None

    self.assertTrue(self.validate(filename))

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR)

    gdal.ErrorReset()
    domain = ds.GetFieldDomain('range_domain_int')
    self.assertEqual(gdal.GetLastErrorMsg(), '')
    self.assertIsNotNone(domain)
    self.assertEqual(domain.GetName(), 'range_domain_int')
    self.assertEqual(domain.GetDescription(), 'my desc')
    self.assertEqual(domain.GetDomainType(), ogr.OFDT_RANGE)
    self.assertEqual(domain.GetFieldType(), ogr.OFTReal)
    self.assertEqual(domain.GetMinAsDouble(), 1.5)
    self.assertTrue(domain.IsMinInclusive())
    self.assertEqual(domain.GetMaxAsDouble(), 2.5)
    self.assertFalse(domain.IsMaxInclusive())

    ds = None

  def test_ogr_gpkg_views(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_views.gpkg')
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('foo', geom_type=ogr.wkbPoint, options=['SRID=0'])
    lyr.CreateField(ogr.FieldDefn('str_field'))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(0 0)'))
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT(1 1)'))
    lyr.CreateFeature(f)

    ds.ExecuteSQL(
        'CREATE VIEW geom_view AS SELECT fid AS my_fid, geom AS my_geom'
        ' FROM foo'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, identifier, data_type, srs_id)'
        " VALUES ( 'geom_view', 'geom_view', 'features', 0 )"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns (table_name, column_name,'
        " geometry_type_name, srs_id, z, m) values ('geom_view', 'my_geom',"
        " 'POINT', 0, 0, 0)"
    )

    ds.ExecuteSQL(
        'CREATE VIEW attr_view AS SELECT fid AS my_fid, str_field FROM foo'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, identifier, data_type) VALUES ('
        " 'attr_view', 'attr_view', 'attributes' )"
    )

    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    ds = ogr.Open(filename, update=1)
    self.assertEqual(ds.GetLayerCount(), 3)

    lyr = ds.GetLayerByName('geom_view')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint)

    lyr = ds.GetLayerByName('attr_view')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbNone)

    if not self.driver.GetMetadataItem('SQLITE_HAS_COLUMN_METADATA'):
      ds = None
      self.skipTest('SQLite built without SQLITE_HAS_COLUMN_METADATA')

    f = lyr.GetFeature(1)
    f['str_field'] = 'bar'
    with _quiet_error_handler():
      self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_FAILURE)

    ds.ExecuteSQL(
        'CREATE TRIGGER attr_view_str_field_chng INSTEAD OF UPDATE OF str_field'
        ' ON attr_view BEGIN UPDATE foo SET str_field=NEW.str_field WHERE'
        ' fid=NEW.my_fid; END;'
    )

    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(lyr.UpdateFeature(f, [0], [], False), ogr.OGRERR_NONE)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFID(100)
    f['str_field'] = 'bar'
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NON_EXISTING_FEATURE)
    self.assertEqual(
        lyr.UpdateFeature(f, [0], [], False), ogr.OGRERR_NON_EXISTING_FEATURE
    )

    with _quiet_error_handler():
      self.assertEqual(lyr.DeleteFeature(1), ogr.OGRERR_FAILURE)

    ds.ExecuteSQL(
        'CREATE TRIGGER attr_view_delete INSTEAD OF DELETE ON attr_view BEGIN'
        ' DELETE FROM foo WHERE fid=OLD.my_fid; END;'
    )

    self.assertEqual(lyr.DeleteFeature(1), ogr.OGRERR_NONE)
    self.assertEqual(lyr.DeleteFeature(100), ogr.OGRERR_NON_EXISTING_FEATURE)

    ds = None

  def test_ogr_gpkg_spatial_view_computed_geom_column(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_spatial_view_computed_geom_column.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)

    if not _has_spatialite_4_3_or_later(ds):
      ds = None
      self.skipTest('spatialite missing')

    lyr = ds.CreateLayer('foo', geom_type=ogr.wkbPoint25D)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT Z (1 2 3)'))
    lyr.CreateFeature(f)

    ds.ExecuteSQL(
        'CREATE VIEW geom_view AS SELECT fid AS my_fid, AsGPB(ST_Multi(geom))'
        ' AS my_geom FROM foo'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, identifier, data_type, srs_id)'
        " VALUES ( 'geom_view', 'geom_view', 'features', 4326 )"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns (table_name, column_name,'
        " geometry_type_name, srs_id, z, m) values ('geom_view', 'my_geom',"
        " 'MULTIPOINT', 4326, 1, 0)"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('geom_view', 'my_geom',"
        " 'gdal_spatialite_computed_geom_column',"
        " 'https://gdal.org/drivers/vector/gpkg_spatialite_computed_column.html',"
        " 'read-write')"
    )

    ds = None

    conn = sqlite3.connect(':memory:')
    can_use_validate = False
    try:
      conn.enable_load_extension(True)
      conn.execute('SELECT load_extension("mod_spatialite")')
      can_use_validate = True
    except Exception:
      pass
    conn.close()
    if can_use_validate:
      self.assertTrue(self.validate(filename), 'validation failed')

    ds = ogr.Open(filename)

    lyr = ds.GetLayerByName('geom_view')
    self.assertEqual(lyr.GetGeomType(), ogr.wkbMultiPoint25D)
    self.assertEqual(lyr.GetSpatialRef().GetAuthorityCode(None), '4326')
    f = lyr.GetNextFeature()
    self.assertEqual(
        f.GetGeometryRef().ExportToIsoWkt(), 'MULTIPOINT Z ((1 2 3))'
    )
    self.assertEqual(lyr.GetExtent(), (1, 1, 2, 2))
    self.assertEqual(lyr.GetExtent3D(), (1, 1, 2, 2, 3, 3))

    ds = None

  def test_ogr_gpkg_read_deprecated_gdal_aspatial(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_aspatial.gpkg')
    ds = self.driver.CreateDataSource(filename)
    ds.ExecuteSQL(
        'CREATE TABLE gpkg_extensions ('
        'table_name TEXT,'
        'column_name TEXT,'
        'extension_name TEXT NOT NULL,'
        'definition TEXT NOT NULL,'
        'scope TEXT NOT NULL,'
        'CONSTRAINT ge_tce UNIQUE (table_name, column_name, extension_name)'
        ')'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_extensions '
        '(table_name, column_name, extension_name, definition, scope) '
        'VALUES '
        "(NULL, NULL, 'gdal_aspatial',"
        " 'http://gdal.org/geopackage_aspatial.html', 'read-write')"
    )
    ds.ExecuteSQL(
        'CREATE TABLE aspatial_layer(fid INTEGER PRIMARY KEY,bar TEXT)'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, data_type) VALUES'
        " ('aspatial_layer', 'aspatial')"
    )
    ds.CreateLayer('spatial_layer', options=['SPATIAL_INDEX=NO'])
    ds = None

    ds = ogr.Open(filename)
    self.assertEqual(ds.GetLayerCount(), 2)
    ds = None

  def test_ogr_gpkg_fixup_wrong_mr_column_name_update_trigger(self) -> None:
    filename = str(
        self.vsimem_dir
        / 'test_ogr_gpkg_fixup_wrong_mr_column_name_update_trigger.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    ds.SetMetadata('FOO', 'BAR')
    ds = None

    ds = ogr.Open(filename, update=1)
    # Inject wrong trigger on purpose.
    wrong_trigger = (
        "CREATE TRIGGER 'gpkg_metadata_reference_column_name_update' "
        + "BEFORE UPDATE OF column_name ON 'gpkg_metadata_reference' "
        + 'FOR EACH ROW BEGIN '
        + "SELECT RAISE(ABORT, 'update on table gpkg_metadata_reference "
        + 'violates constraint: column name must be NULL when reference_scope '
        + 'is "geopackage", "table" or "row"\') '
        + "WHERE (NEW.reference_scope IN ('geopackage','table','row') "
        + 'AND NEW.column_nameIS NOT NULL); END;'
    )
    ds.ExecuteSQL(wrong_trigger)
    ds = None

    # Open in update mode.
    ds = ogr.Open(filename, update=1)
    with ds.ExecuteSQL(
        "SELECT sql FROM sqlite_master WHERE type = 'trigger' "
        + "AND name = 'gpkg_metadata_reference_column_name_update'"
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      sql = f['sql']
    ds = None

    self.assertNotIn('column_nameIS', sql)

  def test_ogr_gpkg_crs_coordinate_epoch(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_crs_coordinate_epoch.gpkg')
    ds = self.driver.CreateDataSource(filename)

    srs = osr.SpatialReference()
    srs.SetFromUserInput('+proj=longlat +ellps=GRS80 +towgs84=0,0,0')
    srs.SetCoordinateEpoch(2021.3)
    ds.CreateLayer('lyr_with_coordinate_epoch_unknown_srs', srs=srs)

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(7665)  # WGS 84 (G1762) (3D)
    srs.SetCoordinateEpoch(2021.3)
    ds.CreateLayer('lyr_with_coordinate_epoch', srs=srs)

    srs.SetCoordinateEpoch(2021.3)
    ds.CreateLayer('lyr_with_same_coordinate_epoch', srs=srs)

    srs.SetCoordinateEpoch(2021.2)
    ds.CreateLayer('lyr_with_different_coordinate_epoch', srs=srs)

    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4258)  # ETRS89
    ds.CreateLayer('lyr_without_coordinate_epoch', srs=srs)

    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    ds = ogr.Open(filename)

    with ds.ExecuteSQL(
        'SELECT * FROM gpkg_spatial_ref_sys ORDER BY srs_id'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 7)

      sql_lyr.GetNextFeature()
      sql_lyr.GetNextFeature()

      f = sql_lyr.GetNextFeature()
      self.assertIsNotNone(f)
      self.assertEqual(f['srs_id'], 4258)
      self.assertEqual(f['organization'], 'EPSG')
      self.assertEqual(f['organization_coordsys_id'], 4258)
      self.assertIsNone(f['epoch'])

      f = sql_lyr.GetNextFeature()
      self.assertIsNotNone(f)
      self.assertEqual(f['srs_id'], 4326)
      self.assertEqual(f['organization'], 'EPSG')
      self.assertEqual(f['organization_coordsys_id'], 4326)
      self.assertIsNone(f['epoch'])

      f = sql_lyr.GetNextFeature()
      self.assertIsNotNone(f)
      self.assertEqual(f['srs_id'], 100000)
      self.assertEqual(f['organization'], 'NONE')
      self.assertEqual(f['organization_coordsys_id'], 100000)
      self.assertEqual(f['epoch'], 2021.3)

      f = sql_lyr.GetNextFeature()
      self.assertIsNotNone(f)
      self.assertEqual(f['srs_id'], 100001)
      self.assertEqual(f['organization'], 'EPSG')
      self.assertEqual(f['organization_coordsys_id'], 7665)
      self.assertEqual(f['epoch'], 2021.3)

      f = sql_lyr.GetNextFeature()
      self.assertIsNotNone(f)
      self.assertEqual(f['srs_id'], 100002)
      self.assertEqual(f['organization'], 'EPSG')
      self.assertEqual(f['organization_coordsys_id'], 7665)
      self.assertEqual(f['epoch'], 2021.2)

    lyr = ds.GetLayerByName('lyr_with_coordinate_epoch_unknown_srs')
    srs = lyr.GetSpatialRef()
    self.assertEqual(srs.GetCoordinateEpoch(), 2021.3)

    lyr = ds.GetLayerByName('lyr_with_coordinate_epoch')
    srs = lyr.GetSpatialRef()
    self.assertEqual(srs.GetCoordinateEpoch(), 2021.3)

    lyr = ds.GetLayerByName('lyr_with_same_coordinate_epoch')
    srs = lyr.GetSpatialRef()
    self.assertEqual(srs.GetCoordinateEpoch(), 2021.3)

    lyr = ds.GetLayerByName('lyr_with_different_coordinate_epoch')
    srs = lyr.GetSpatialRef()
    self.assertEqual(srs.GetCoordinateEpoch(), 2021.2)

    lyr = ds.GetLayerByName('lyr_without_coordinate_epoch')
    srs = lyr.GetSpatialRef()
    self.assertEqual(srs.GetCoordinateEpoch(), 0)

    ds = None

  def test_ogr_gpkg_CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE_a(self) -> None:
    # First check that CPL_TMPDIR is ignored for regular files.
    filename = str(
        self.vsimem_dir
        / 'test_ogr_gpkg_CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE.gpkg'
    )
    with gdal.config_option('CPL_TMPDIR', '/i_do/not/exist'):
      ds = self.driver.CreateDataSource(filename)
    self.assertIsNotNone(ds)
    ds = None

  def test_ogr_gpkg_CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE_b(self) -> None:
    filename = str(
        self.vsimem_dir
        / 'test_ogr_gpkg_CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE.gpkg'
    )

    # Check that CPL_TMPDIR is honored for
    # CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE=FORCED.
    temp_dir = str(self.vsimem_dir / 'temporary_location')
    gdal.Mkdir(temp_dir, 0o755)
    with gdal.config_options({
        'CPL_TMPDIR': temp_dir,
        'CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE': 'FORCED',
    }):
      ds = self.driver.CreateDataSource(filename)
    self.assertIsNotNone(ds)
    self.assertIsNone(gdal.VSIStatL(filename))
    self.assertNotEqual(len(gdal.ReadDir(temp_dir)), 0)
    ds = None
    self.assertIsNotNone(gdal.VSIStatL(filename))
    self.assertIsNone(gdal.ReadDir(temp_dir))

  def test_ogr_gpkg_relations(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_relations.gpkg')
    ds = self.driver.CreateDataSource(filename)
    lyr = ds.CreateLayer('a')
    lyr.CreateField(ogr.FieldDefn('some_id', ogr.OFTInteger))
    lyr = ds.CreateLayer('b')
    lyr.CreateField(ogr.FieldDefn('other_id', ogr.OFTInteger))
    ds = None

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    self.assertIsNone(ds.GetRelationshipNames())

    ds.ExecuteSQL("""CREATE TABLE 'gpkgext_relations' (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          base_table_name TEXT NOT NULL,
          base_primary_column TEXT NOT NULL DEFAULT 'id',
          related_table_name TEXT NOT NULL,
          related_primary_column TEXT NOT NULL DEFAULT 'id',
          relation_name TEXT NOT NULL,
          mapping_table_name TEXT NOT NULL UNIQUE
         );""")

    # Not yet valid...
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    self.assertIsNone(ds.GetRelationshipNames())

    ds.ExecuteSQL(
        "INSERT INTO gpkgext_relations VALUES(1, 'a', 'some_id', 'b',"
        " 'other_id', 'attributes', 'my_mapping_table')"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('gpkgext_relations',NULL,"
        "'gpkg_related_tables','http://www.geopackage.org/18-000.html',"
        "'read-write');"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('my_mapping_table',NULL,"
        "'gpkg_related_tables','http://www.geopackage.org/18-000.html',"
        "'read-write');"
    )
    ds = None

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    self.assertIsNone(ds.GetRelationshipNames())
    ds.ExecuteSQL(
        'CREATE TABLE my_mapping_table(base_id INTEGER NOT NULL, related_id'
        ' INTEGER NOT NULL);'
    )
    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    self.assertEqual(ds.GetRelationshipNames(), ['a_b_attributes'])
    self.assertIsNone(ds.GetRelationship('xxx'))
    rel = ds.GetRelationship('a_b_attributes')
    self.assertIsNotNone(rel)
    self.assertEqual(rel.GetName(), 'a_b_attributes')
    self.assertEqual(rel.GetLeftTableName(), 'a')
    self.assertEqual(rel.GetRightTableName(), 'b')
    self.assertEqual(rel.GetMappingTableName(), 'my_mapping_table')
    self.assertEqual(rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(rel.GetLeftTableFields(), ['some_id'])
    self.assertEqual(rel.GetRightTableFields(), ['other_id'])
    self.assertEqual(rel.GetLeftMappingTableFields(), ['base_id'])
    self.assertEqual(rel.GetRightMappingTableFields(), ['related_id'])
    self.assertEqual(rel.GetRelatedTableType(), 'attributes')

    # Ensure that mapping table in gpkgext_relations but not gpkg_contents
    # can be opened as a layer.
    lyr = ds.GetLayer('my_mapping_table')
    self.assertIsNotNone(lyr)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'base_id')
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'related_id')

    lyr = ds.GetLayer('a')
    lyr.Rename('a_renamed')
    lyr.AlterFieldDefn(
        lyr.GetLayerDefn().GetFieldIndex('some_id'),
        ogr.FieldDefn('some_id_renamed', ogr.OFTInteger),
        ogr.ALTER_ALL_FLAG,
    )
    lyr = ds.GetLayer('b')
    lyr.Rename('b_renamed')
    lyr.AlterFieldDefn(
        lyr.GetLayerDefn().GetFieldIndex('other_id'),
        ogr.FieldDefn('other_id_renamed', ogr.OFTInteger),
        ogr.ALTER_ALL_FLAG,
    )

    self.assertEqual(
        ds.GetRelationshipNames(), ['a_renamed_b_renamed_attributes']
    )
    self.assertIsNone(ds.GetRelationship('xxx'))
    rel = ds.GetRelationship('a_renamed_b_renamed_attributes')
    self.assertIsNotNone(rel)
    self.assertEqual(rel.GetName(), 'a_renamed_b_renamed_attributes')
    self.assertEqual(rel.GetLeftTableName(), 'a_renamed')
    self.assertEqual(rel.GetRightTableName(), 'b_renamed')
    self.assertEqual(rel.GetMappingTableName(), 'my_mapping_table')
    self.assertEqual(rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(rel.GetLeftTableFields(), ['some_id_renamed'])
    self.assertEqual(rel.GetRightTableFields(), ['other_id_renamed'])
    self.assertEqual(rel.GetLeftMappingTableFields(), ['base_id'])
    self.assertEqual(rel.GetRightMappingTableFields(), ['related_id'])
    self.assertEqual(rel.GetRelatedTableType(), 'attributes')

    ds = None
    self.assertTrue(self.validate(filename), 'validation failed')

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    ds.ExecuteSQL('DELLAYER:a_renamed')
    with ds.ExecuteSQL(
        'SELECT * FROM gpkg_extensions WHERE extension_name IN'
        " ('related_tables', 'gpkg_related_tables')"
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      self.assertIsNone(f)
    self.assertIsNone(ds.GetRelationshipNames())
    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    # User-defined relation
    ds = ogr.Open(filename, update=1)
    lyr = ds.CreateLayer('a')
    lyr.CreateField(ogr.FieldDefn('some_id', ogr.OFTInteger))
    ds.ExecuteSQL(
        "INSERT INTO gpkgext_relations VALUES(1, 'a', 'some_id', 'b',"
        " 'other_id', 'custom_type', 'my_mapping_table')"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('gpkgext_relations',NULL,"
        "'gpkg_related_tables','http://www.geopackage.org/18-000.html',"
        "'read-write');"
    )
    ds.ExecuteSQL(
        "INSERT INTO gpkg_extensions VALUES('my_mapping_table',NULL,"
        "'gpkg_related_tables','http://www.geopackage.org/18-000.html',"
        "'read-write');"
    )

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR)
    self.assertEqual(ds.GetRelationshipNames(), ['custom_type'])
    self.assertIsNone(ds.GetRelationship('xxx'))
    rel = ds.GetRelationship('custom_type')
    self.assertIsNotNone(rel)
    self.assertEqual(rel.GetName(), 'custom_type')
    self.assertEqual(rel.GetLeftTableName(), 'a')
    self.assertEqual(rel.GetRightTableName(), 'b')
    self.assertEqual(rel.GetMappingTableName(), 'my_mapping_table')
    self.assertEqual(rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(rel.GetLeftTableFields(), ['some_id'])
    self.assertEqual(rel.GetRightTableFields(), ['other_id'])
    self.assertEqual(rel.GetLeftMappingTableFields(), ['base_id'])
    self.assertEqual(rel.GetRightMappingTableFields(), ['related_id'])
    self.assertEqual(rel.GetRelatedTableType(), 'features')

    # One-to-many relationship defined using foreign key constraints.
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    ds.ExecuteSQL(
        'CREATE TABLE test_relation_a(artistid INTEGER PRIMARY KEY, artistname'
        ' TEXT)'
    )
    ds.ExecuteSQL(
        'CREATE TABLE test_relation_b(trackid INTEGER, trackname TEXT,'
        ' trackartist INTEGER, FOREIGN KEY(trackartist) REFERENCES'
        ' test_relation_a(artistid))'
    )
    ds = None

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    self.assertEqual(
        ds.GetRelationshipNames(),
        ['custom_type', 'test_relation_a_test_relation_b'],
    )
    self.assertIsNotNone(ds.GetRelationship('custom_type'))
    rel = ds.GetRelationship('test_relation_a_test_relation_b')
    self.assertIsNotNone(rel)
    self.assertEqual(rel.GetName(), 'test_relation_a_test_relation_b')
    self.assertEqual(rel.GetLeftTableName(), 'test_relation_a')
    self.assertEqual(rel.GetRightTableName(), 'test_relation_b')
    self.assertEqual(rel.GetCardinality(), gdal.GRC_ONE_TO_MANY)
    self.assertEqual(rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(rel.GetLeftTableFields(), ['artistid'])
    self.assertEqual(rel.GetRightTableFields(), ['trackartist'])
    self.assertEqual(rel.GetRelatedTableType(), 'features')

    ds = None

  def test_ogr_gpkg_relations_sqlite_foreign_keys(self) -> None:
    tmpfilename = str(self.vsimem_dir / 'test_ogr_gpkg_relations_sqlite.gpkg')

    ds = self.driver.CreateDataSource(tmpfilename)
    lyr = ds.CreateLayer('a')
    lyr.CreateField(ogr.FieldDefn('some_id', ogr.OFTInteger))
    lyr = ds.CreateLayer('b')
    lyr.CreateField(ogr.FieldDefn('other_id', ogr.OFTInteger))
    ds = None

    ds = gdal.OpenEx(tmpfilename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    self.assertIsNone(ds.GetRelationshipNames())

    ds.ExecuteSQL(
        'CREATE TABLE test_relation_a(artistid INTEGER PRIMARY KEY, artistname'
        ' TEXT)'
    )
    ds.ExecuteSQL(
        'CREATE TABLE test_relation_b(trackid INTEGER, trackname TEXT,'
        ' trackartist INTEGER, FOREIGN KEY(trackartist) REFERENCES'
        ' test_relation_a(artistid))'
    )
    ds = None

    ds = gdal.OpenEx(tmpfilename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    self.assertEqual(
        ds.GetRelationshipNames(), ['test_relation_a_test_relation_b']
    )
    self.assertIsNone(ds.GetRelationship('xxx'))
    rel = ds.GetRelationship('test_relation_a_test_relation_b')
    self.assertIsNotNone(rel)
    self.assertEqual(rel.GetName(), 'test_relation_a_test_relation_b')
    self.assertEqual(rel.GetLeftTableName(), 'test_relation_a')
    self.assertEqual(rel.GetRightTableName(), 'test_relation_b')
    self.assertEqual(rel.GetCardinality(), gdal.GRC_ONE_TO_MANY)
    self.assertEqual(rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(rel.GetLeftTableFields(), ['artistid'])
    self.assertEqual(rel.GetRightTableFields(), ['trackartist'])
    self.assertEqual(rel.GetRelatedTableType(), 'features')

  def test_ogr_gpkg_alter_relations(self) -> None:
    def clone_relationship(relationship: Any) -> Any:
      res = gdal.Relationship(
          relationship.GetName(),
          relationship.GetLeftTableName(),
          relationship.GetRightTableName(),
          relationship.GetCardinality(),
      )
      res.SetLeftTableFields(relationship.GetLeftTableFields())
      res.SetRightTableFields(relationship.GetRightTableFields())
      res.SetMappingTableName(relationship.GetMappingTableName())
      res.SetLeftMappingTableFields(relationship.GetLeftMappingTableFields())
      res.SetRightMappingTableFields(relationship.GetRightMappingTableFields())
      res.SetType(relationship.GetType())
      res.SetForwardPathLabel(relationship.GetForwardPathLabel())
      res.SetBackwardPathLabel(relationship.GetBackwardPathLabel())
      res.SetRelatedTableType(relationship.GetRelatedTableType())
      return res

    filename = str(self.vsimem_dir / 'test_ogr_gpkg_relation_create.gpkg')
    ds = self.driver.CreateDataSource(filename)
    ds = None

    relationship = gdal.Relationship(
        'my_relationship', 'origin_table', 'dest_table', gdal.GRC_MANY_TO_MANY
    )
    relationship.SetLeftTableFields(['o_pkey'])
    relationship.SetRightTableFields(['dest_pkey'])
    relationship.SetRelatedTableType('media')

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)

    def get_query_row_count(query: str) -> int:
      assert ds is not None
      with ds.ExecuteSQL(query) as sql_lyr:
        return sql_lyr.GetFeatureCount()

    # No tables yet
    self.assertFalse(ds.AddRelationship(relationship))

    lyr = ds.CreateLayer('origin_table', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('o_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    fld_defn = ogr.FieldDefn('o_pkey2', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    ds.ExecuteSQL(
        'CREATE UNIQUE INDEX origin_table_o_pkey_idx ON origin_table(o_pkey)'
    )

    self.assertFalse(ds.AddRelationship(relationship))

    lyr = ds.CreateLayer('dest_table', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('dest_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    fld_defn = ogr.FieldDefn('dest_pkey2', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    ds.ExecuteSQL(
        'CREATE UNIQUE INDEX dest_table_dest_pkey_idx ON dest_table(dest_pkey)'
    )

    # Left table fields must be set, only one field.
    relationship.SetLeftTableFields([])
    self.assertFalse(ds.AddRelationship(relationship))
    relationship.SetLeftTableFields(['o_pkey', 'another'])
    self.assertFalse(ds.AddRelationship(relationship))
    relationship.SetLeftTableFields(['o_pkey_nope'])
    self.assertFalse(ds.AddRelationship(relationship))

    relationship.SetLeftTableFields(['o_pkey'])

    # Right table fields must be set, only one field.
    relationship.SetRightTableFields([])
    self.assertFalse(ds.AddRelationship(relationship))
    relationship.SetRightTableFields(['dest_pkey', 'another'])
    self.assertFalse(ds.AddRelationship(relationship))
    relationship.SetRightTableFields(['dest_pkey_nope'])
    self.assertFalse(ds.AddRelationship(relationship))

    relationship.SetRightTableFields(['dest_pkey'])

    self.assertTrue(ds.AddRelationship(relationship))

    self.assertEqual(
        set(ds.GetRelationshipNames()), {'origin_table_dest_table_media'}
    )
    retrieved_rel = ds.GetRelationship('origin_table_dest_table_media')
    self.assertEqual(retrieved_rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(retrieved_rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(retrieved_rel.GetLeftTableName(), 'origin_table')
    self.assertEqual(retrieved_rel.GetRightTableName(), 'dest_table')
    self.assertEqual(retrieved_rel.GetLeftTableFields(), ['o_pkey'])
    self.assertEqual(retrieved_rel.GetRightTableFields(), ['dest_pkey'])
    self.assertEqual(retrieved_rel.GetRelatedTableType(), 'media')
    self.assertEqual(
        retrieved_rel.GetMappingTableName(), 'origin_table_dest_table'
    )
    self.assertEqual(retrieved_rel.GetLeftMappingTableFields(), ['base_id'])
    self.assertEqual(retrieved_rel.GetRightMappingTableFields(), ['related_id'])

    # Try again, should fail because relationship already exists.
    self.assertFalse(ds.AddRelationship(relationship))

    # Validate extensions table.
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE table_name ='
            " 'gpkgext_relations' AND extension_name = 'gpkg_related_tables'"
        ),
        1,
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE table_name ='
            " 'origin_table_dest_table' AND extension_name ="
            " 'gpkg_related_tables'"
        ),
        1,
    )

    # Validate gpkgext_relations.
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkgext_relations WHERE base_table_name ='
            " 'origin_table' AND base_primary_column = 'o_pkey' AND"
            " related_table_name = 'dest_table' AND related_primary_column ="
            " 'dest_pkey' AND relation_name = 'media' AND"
            " mapping_table_name = 'origin_table_dest_table'"
        ),
        1,
    )

    self.assertEqual(
        get_query_row_count("SELECT * FROM 'origin_table_dest_table'"), 0
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_contents WHERE'
            " table_name='origin_table_dest_table' AND data_type='attributes'"
        ),
        1,
    )
    # Delete from gpkg_contents and check layer still loads via gpkgext_relations.
    ds.ExecuteSQL(
        "DELETE FROM gpkg_contents WHERE table_name='origin_table_dest_table'"
    )

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)
    lyr = ds.GetLayer('origin_table_dest_table')
    self.assertIsNotNone(lyr)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'base_id')
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'related_id')

    lyr = ds.CreateLayer('origin_table2', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('o_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)

    lyr = ds.CreateLayer('dest_table2', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('dest_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)

    # Only many-to-many relationships are supported.
    for cardinality in (
        gdal.GRC_ONE_TO_ONE,
        gdal.GRC_ONE_TO_MANY,
        gdal.GRC_MANY_TO_ONE,
    ):
      relationship = gdal.Relationship(
          'my_relationship', 'origin_table2', 'dest_table2', cardinality
      )
      relationship.SetLeftTableFields(['o_pkey'])
      relationship.SetRightTableFields(['dest_pkey'])
      relationship.SetRelatedTableType('features')
      self.assertFalse(ds.AddRelationship(relationship))

    # Only features/media/simple_attributes/attributes/tiles are supported.
    relationship = gdal.Relationship(
        'my_relationship',
        'origin_table2',
        'dest_table2',
        gdal.GRC_MANY_TO_MANY,
    )
    relationship.SetLeftTableFields(['o_pkey'])
    relationship.SetRightTableFields(['dest_pkey'])
    relationship.SetRelatedTableType('something else')
    self.assertFalse(ds.AddRelationship(relationship))

    # Defaults to 'features' if nothing explicitly specified.
    relationship.SetRelatedTableType('')
    self.assertTrue(ds.AddRelationship(relationship))

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {
            'origin_table_dest_table_media',
            'origin_table2_dest_table2_features',
        },
    )
    retrieved_rel = ds.GetRelationship('origin_table2_dest_table2_features')
    self.assertEqual(retrieved_rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(retrieved_rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(retrieved_rel.GetLeftTableName(), 'origin_table2')
    self.assertEqual(retrieved_rel.GetRightTableName(), 'dest_table2')
    self.assertEqual(retrieved_rel.GetLeftTableFields(), ['o_pkey'])
    self.assertEqual(retrieved_rel.GetRightTableFields(), ['dest_pkey'])
    self.assertEqual(retrieved_rel.GetRelatedTableType(), 'features')

    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE table_name ='
            " 'origin_table2_dest_table2' AND extension_name ="
            " 'gpkg_related_tables'"
        ),
        1,
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkgext_relations WHERE base_table_name ='
            " 'origin_table2' AND base_primary_column = 'o_pkey' AND"
            " related_table_name = 'dest_table2' AND related_primary_column ="
            " 'dest_pkey' AND relation_name = 'features' AND"
            " mapping_table_name = 'origin_table2_dest_table2'"
        ),
        1,
    )

    # Try with an existing mapping table.
    lyr = ds.CreateLayer('origin_table3', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('o_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)

    lyr = ds.CreateLayer('dest_table3', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('dest_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)

    lyr = ds.CreateLayer(
        'origin_table3_to_dest_table_3_mapping', geom_type=ogr.wkbNone
    )
    fld_defn = ogr.FieldDefn('base_id', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    fld_defn = ogr.FieldDefn('related_id', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    self.assertEqual(
        get_query_row_count(
            'SELECT 1 FROM sqlite_master WHERE name ='
            " 'origin_table3_to_dest_table_3_mapping' AND type in ('table',"
            " 'view')"
        ),
        1,
    )

    relationship = gdal.Relationship(
        'my_relationship',
        'origin_table3',
        'dest_table3',
        gdal.GRC_MANY_TO_MANY,
    )
    # fid fields should be permitted for relationship use.
    relationship.SetLeftTableFields(['fid'])
    relationship.SetRightTableFields(['fid'])
    relationship.SetMappingTableName('nope')
    self.assertFalse(ds.AddRelationship(relationship))

    relationship.SetMappingTableName('origin_table3_to_dest_table_3_mapping')
    self.assertTrue(ds.AddRelationship(relationship))

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {
            'origin_table_dest_table_media',
            'origin_table2_dest_table2_features',
            'origin_table3_dest_table3_features',
        },
    )
    retrieved_rel = ds.GetRelationship('origin_table3_dest_table3_features')
    self.assertEqual(retrieved_rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(retrieved_rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(retrieved_rel.GetLeftTableName(), 'origin_table3')
    self.assertEqual(retrieved_rel.GetRightTableName(), 'dest_table3')
    self.assertEqual(retrieved_rel.GetLeftTableFields(), ['fid'])
    self.assertEqual(retrieved_rel.GetRightTableFields(), ['fid'])
    self.assertEqual(retrieved_rel.GetRelatedTableType(), 'features')
    self.assertEqual(
        retrieved_rel.GetMappingTableName(),
        'origin_table3_to_dest_table_3_mapping',
    )

    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE table_name ='
            " 'origin_table3_to_dest_table_3_mapping' AND extension_name ="
            " 'gpkg_related_tables'"
        ),
        1,
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkgext_relations WHERE base_table_name ='
            " 'origin_table3' AND base_primary_column = 'fid' AND"
            " related_table_name = 'dest_table3' AND related_primary_column ="
            " 'fid' AND relation_name = 'features' AND mapping_table_name ="
            " 'origin_table3_to_dest_table_3_mapping'"
        ),
        1,
    )

    # Try with a mapping table that doesn't match requirements.
    lyr = ds.CreateLayer('origin_table4', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('o_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    lyr = ds.CreateLayer('dest_table4', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('dest_pkey', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    lyr = ds.CreateLayer(
        'origin_table4_to_dest_table_4_mapping', geom_type=ogr.wkbNone
    )
    fld_defn = ogr.FieldDefn('not_base_id', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    fld_defn = ogr.FieldDefn('related_id', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    self.assertEqual(
        get_query_row_count(
            'SELECT 1 FROM sqlite_master WHERE name ='
            " 'origin_table4_to_dest_table_4_mapping' AND type in ('table',"
            " 'view')"
        ),
        1,
    )
    relationship = gdal.Relationship(
        'my_relationship',
        'origin_table4',
        'dest_table4',
        gdal.GRC_MANY_TO_MANY,
    )
    relationship.SetLeftTableFields(['o_pkey'])
    relationship.SetRightTableFields(['dest_pkey'])
    relationship.SetMappingTableName('origin_table4_to_dest_table_4_mapping')
    self.assertFalse(ds.AddRelationship(relationship))

    lyr = ds.CreateLayer(
        'origin_table4_to_dest_table_4_mappingv2', geom_type=ogr.wkbNone
    )
    fld_defn = ogr.FieldDefn('base_id', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    fld_defn = ogr.FieldDefn('not_related_id', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    self.assertEqual(
        get_query_row_count(
            'SELECT 1 FROM sqlite_master WHERE name ='
            " 'origin_table4_to_dest_table_4_mappingv2' AND type in ('table',"
            " 'view')"
        ),
        1,
    )
    relationship.SetMappingTableName('origin_table4_to_dest_table_4_mappingv2')
    self.assertFalse(ds.AddRelationship(relationship))

    ds = None
    self.assertTrue(self.validate(filename), 'validation failed')
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)

    # Delete relationship
    self.assertFalse(ds.DeleteRelationship('nope'))

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {
            'origin_table_dest_table_media',
            'origin_table2_dest_table2_features',
            'origin_table3_dest_table3_features',
        },
    )

    self.assertTrue(ds.DeleteRelationship('origin_table2_dest_table2_features'))

    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE table_name ='
            " 'origin_table2_dest_table2' AND extension_name ="
            " 'gpkg_related_tables'"
        ),
        0,
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkgext_relations WHERE base_table_name ='
            " 'origin_table2' AND base_primary_column = 'o_pkey' AND"
            " related_table_name = 'dest_table2' AND related_primary_column ="
            " 'dest_pkey' AND relation_name = 'features' AND"
            " mapping_table_name = 'origin_table2_dest_table2'"
        ),
        0,
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT 1 FROM sqlite_master WHERE name ='
            " 'origin_table2_dest_table2' AND type in ('table', 'view')"
        ),
        0,
    )

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {
            'origin_table_dest_table_media',
            'origin_table3_dest_table3_features',
        },
    )

    ds = None
    self.assertTrue(self.validate(filename), 'validation failed')
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)

    # Update relationship
    retrieved_rel = ds.GetRelationship('origin_table_dest_table_media')

    # Can't update a relationship that doesn't exist.
    relationship = gdal.Relationship(
        'nope',
        retrieved_rel.GetLeftTableName(),
        retrieved_rel.GetRightTableName(),
        gdal.GRC_MANY_TO_MANY,
    )
    relationship.SetLeftTableFields(retrieved_rel.GetLeftTableFields())
    relationship.SetRightTableFields(retrieved_rel.GetRightTableFields())
    relationship.SetMappingTableName(retrieved_rel.GetMappingTableName())
    relationship.SetLeftMappingTableFields(
        retrieved_rel.GetLeftMappingTableFields()
    )
    relationship.SetRightMappingTableFields(
        retrieved_rel.GetRightMappingTableFields()
    )

    self.assertFalse(ds.UpdateRelationship(clone_relationship(relationship)))

    retrieved_rel = ds.GetRelationship('origin_table_dest_table_media')
    retrieved_rel.SetRelatedTableType('nope')
    self.assertFalse(ds.UpdateRelationship(clone_relationship(retrieved_rel)))

    # Change related table type
    retrieved_rel = ds.GetRelationship('origin_table_dest_table_media')
    retrieved_rel.SetRelatedTableType('attributes')
    self.assertTrue(ds.UpdateRelationship(clone_relationship(retrieved_rel)))

    ds = None
    self.assertTrue(self.validate(filename), 'validation failed')
    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {
            'origin_table_dest_table_attributes',
            'origin_table3_dest_table3_features',
        },
    )

    retrieved_rel = ds.GetRelationship('origin_table_dest_table_attributes')
    self.assertEqual(retrieved_rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(retrieved_rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(retrieved_rel.GetLeftTableName(), 'origin_table')
    self.assertEqual(retrieved_rel.GetRightTableName(), 'dest_table')
    self.assertEqual(retrieved_rel.GetLeftTableFields(), ['o_pkey'])
    self.assertEqual(retrieved_rel.GetRightTableFields(), ['dest_pkey'])
    self.assertEqual(retrieved_rel.GetRelatedTableType(), 'attributes')

    # Change base table field
    retrieved_rel.SetLeftTableFields(['o_pkey2'])
    self.assertTrue(ds.UpdateRelationship(clone_relationship(retrieved_rel)))

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {
            'origin_table_dest_table_attributes',
            'origin_table3_dest_table3_features',
        },
    )

    retrieved_rel = ds.GetRelationship('origin_table_dest_table_attributes')
    self.assertEqual(retrieved_rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(retrieved_rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(retrieved_rel.GetLeftTableName(), 'origin_table')
    self.assertEqual(retrieved_rel.GetRightTableName(), 'dest_table')
    self.assertEqual(retrieved_rel.GetLeftTableFields(), ['o_pkey2'])
    self.assertEqual(retrieved_rel.GetRightTableFields(), ['dest_pkey'])
    self.assertEqual(retrieved_rel.GetRelatedTableType(), 'attributes')

    retrieved_rel.SetRightTableFields(['dest_pkey2'])
    self.assertTrue(ds.UpdateRelationship(clone_relationship(retrieved_rel)))

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {
            'origin_table_dest_table_attributes',
            'origin_table3_dest_table3_features',
        },
    )

    retrieved_rel = ds.GetRelationship('origin_table_dest_table_attributes')
    self.assertEqual(retrieved_rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(retrieved_rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(retrieved_rel.GetLeftTableName(), 'origin_table')
    self.assertEqual(retrieved_rel.GetRightTableName(), 'dest_table')
    self.assertEqual(retrieved_rel.GetLeftTableFields(), ['o_pkey2'])
    self.assertEqual(retrieved_rel.GetRightTableFields(), ['dest_pkey2'])
    self.assertEqual(retrieved_rel.GetRelatedTableType(), 'attributes')

    # Try updating to field which doesn't exist.
    retrieved_rel.SetRightTableFields(['dest_pkey2xxx'])
    self.assertFalse(ds.UpdateRelationship(clone_relationship(retrieved_rel)))

    # Delete all relationships.
    self.assertTrue(ds.DeleteRelationship('origin_table_dest_table_attributes'))

    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE table_name ='
            " 'origin_table_dest_table' AND extension_name ="
            " 'gpkg_related_tables'"
        ),
        0,
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkgext_relations WHERE base_table_name ='
            " 'origin_table' AND base_primary_column = 'o_pkey2' AND"
            " related_table_name = 'dest_table' AND related_primary_column ="
            " 'dest_pkey2' AND relation_name = 'attributes' AND"
            " mapping_table_name = 'origin_table_dest_table'"
        ),
        0,
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT 1 FROM sqlite_master WHERE name ='
            " 'origin_table_dest_table' AND type in ('table', 'view')"
        ),
        0,
    )

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {'origin_table3_dest_table3_features'},
    )
    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE extension_name ='
            " 'gpkg_related_tables'"
        ),
        2,
    )

    self.assertTrue(ds.DeleteRelationship('origin_table3_dest_table3_features'))

    self.assertEqual(
        get_query_row_count(
            'SELECT * FROM gpkg_extensions WHERE extension_name ='
            " 'gpkg_related_tables'"
        ),
        0,
    )
    self.assertEqual(get_query_row_count('SELECT * FROM gpkgext_relations'), 0)

    ds = None
    self.assertTrue(self.validate(filename), 'validation failed')

  def test_ogr_gpkg_add_relationship_complex_names(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_relation_create_complex.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    ds = None

    relationship = gdal.Relationship(
        'my_relationship',
        "Origin' [tàble!",
        'dést ]table$',
        gdal.GRC_MANY_TO_MANY,
    )
    relationship.SetLeftTableFields(['o pkéy'])
    relationship.SetRightTableFields(['Dest pkéy'])
    relationship.SetRelatedTableType('media')

    ds = gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE)

    lyr = ds.CreateLayer("Origin' [tàble!", geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('o pkéy', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    ds.ExecuteSQL(
        'CREATE UNIQUE INDEX origin_table_o_pkey_idx ON'
        ' "Origin\' [tàble!"("o pkéy")'
    )

    lyr = ds.CreateLayer('dést ]table$', geom_type=ogr.wkbNone)
    fld_defn = ogr.FieldDefn('Dest pkéy', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld_defn), ogr.OGRERR_NONE)
    ds.ExecuteSQL(
        'CREATE UNIQUE INDEX dest_table_dest_pkey_idx ON'
        ' "dést ]table$"("Dest pkéy")'
    )

    relationship.SetLeftTableFields(['o pkéy'])
    relationship.SetRightTableFields(['Dest pkéy'])

    self.assertTrue(ds.AddRelationship(relationship))

    self.assertEqual(
        set(ds.GetRelationshipNames()),
        {"Origin' [tàble!_dést ]table$_media"},
    )
    retrieved_rel = ds.GetRelationship("Origin' [tàble!_dést ]table$_media")
    self.assertEqual(retrieved_rel.GetCardinality(), gdal.GRC_MANY_TO_MANY)
    self.assertEqual(retrieved_rel.GetType(), gdal.GRT_ASSOCIATION)
    self.assertEqual(retrieved_rel.GetLeftTableName(), "Origin' [tàble!")
    self.assertEqual(retrieved_rel.GetRightTableName(), 'dést ]table$')
    self.assertEqual(retrieved_rel.GetLeftTableFields(), ['o pkéy'])
    self.assertEqual(retrieved_rel.GetRightTableFields(), ['Dest pkéy'])
    self.assertEqual(retrieved_rel.GetRelatedTableType(), 'media')
    self.assertEqual(
        retrieved_rel.GetMappingTableName(), "Origin' [tàble!_dést ]table$"
    )
    self.assertEqual(retrieved_rel.GetLeftMappingTableFields(), ['base_id'])
    self.assertEqual(retrieved_rel.GetRightMappingTableFields(), ['related_id'])

    ds = None
    self.assertTrue(self.validate(filename), 'validation failed')

  def test_ogr_gpkg_alter_geom_field_defn(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_alter_geom_field_defn.gpkg')
    ds = self.driver.CreateDataSource(filename)
    srs_4326 = osr.SpatialReference()
    srs_4326.ImportFromEPSG(4326)
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint, srs=srs_4326)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (1 2)'))
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    ds = None

    # Test renaming column
    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    self.assertTrue(lyr.TestCapability(ogr.OLCAlterGeomFieldDefn))

    new_geom_field_defn = ogr.GeomFieldDefn('new_geom_name', ogr.wkbNone)
    self.assertEqual(
        lyr.AlterGeomFieldDefn(
            0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_NAME_FLAG
        ),
        ogr.OGRERR_NONE,
    )
    self.assertEqual(lyr.GetGeometryColumn(), 'new_geom_name')
    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    ds = ogr.Open(filename)
    lyr = ds.GetLayer(0)
    self.assertEqual(lyr.GetGeometryColumn(), 'new_geom_name')
    srs = lyr.GetSpatialRef()
    self.assertIsNotNone(srs)
    self.assertEqual(srs.GetAuthorityCode(None), '4326')
    ds = None

    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbNone)
    self.assertEqual(
        lyr.AlterGeomFieldDefn(
            0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_SRS_FLAG
        ),
        ogr.OGRERR_NONE,
    )
    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    srs = lyr.GetSpatialRef()
    self.assertIsNone(srs)

    new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbNone)
    new_geom_field_defn.SetSpatialRef(srs_4326)
    self.assertEqual(
        lyr.AlterGeomFieldDefn(
            0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_SRS_FLAG
        ),
        ogr.OGRERR_NONE,
    )
    ds = None

    self.assertTrue(self.validate(filename), 'validation failed')

    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    srs = lyr.GetSpatialRef()
    self.assertIsNotNone(srs)
    self.assertEqual(srs.GetAuthorityCode(None), '4326')

    new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbNone)
    other_srs = osr.SpatialReference()
    other_srs.ImportFromEPSG(4269)
    new_geom_field_defn.SetSpatialRef(other_srs)
    self.assertEqual(
        lyr.AlterGeomFieldDefn(
            0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_SRS_FLAG
        ),
        ogr.OGRERR_NONE,
    )
    ds = None

    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    srs = lyr.GetSpatialRef()
    self.assertIsNotNone(srs)
    self.assertEqual(srs.GetAuthorityCode(None), '4269')

    new_geom_field_defn = ogr.GeomFieldDefn('', ogr.wkbNone)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4269)
    srs.SetCoordinateEpoch(2022)
    new_geom_field_defn.SetSpatialRef(srs)
    self.assertEqual(
        lyr.AlterGeomFieldDefn(
            0,
            new_geom_field_defn,
            ogr.ALTER_GEOM_FIELD_DEFN_SRS_COORD_EPOCH_FLAG,
        ),
        ogr.OGRERR_NONE,
    )
    ds = None

    ds = ogr.Open(filename, update=1)
    lyr = ds.GetLayer(0)
    srs = lyr.GetSpatialRef()
    self.assertIsNotNone(srs)
    self.assertEqual(srs.GetAuthorityCode(None), '4269')
    self.assertEqual(srs.GetCoordinateEpoch(), 2022)
    ds = None

  def test_ogr_gpkg_arrow_stream_pyarrow_timezone(self) -> None:
    if importlib.util.find_spec('pyarrow') is None:
      self.skipTest('pyarrow missing')

    filename = str(self.vsimem_dir / 'test_ogr_gpkg_arrow_stream_pyarrow.gpkg')
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('datetime', ogr.OFTDateTime))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('datetime', '2022-05-31T12:34:56.789Z')
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('datetime', '2022-05-31T12:34:56.789+01:00')
    lyr.CreateFeature(f)

    stream = lyr.GetArrowStreamAsPyArrow()
    self.assertEqual(stream.schema.field('datetime').type.tz, 'UTC')
    values = []
    for batch in stream:
      for x in batch.field('datetime'):
        values.append(x.value)
    self.assertEqual(values, [1654000496789, 1653996896789])

    stream = lyr.GetArrowStreamAsPyArrow(['TIMEZONE=+01:00'])
    self.assertEqual(stream.schema.field('datetime').type.tz, '+01:00')
    values = []
    for batch in stream:
      for x in batch.field('datetime'):
        values.append(x.value)
    self.assertEqual(values, [1654000496789, 1653996896789])
    ds = None

  def test_ogr_gpkg_arrow_stream_numpy(self) -> None:
    if (
        not hasattr(gdal_array, '_RecordBatchAsNumpy')
        or importlib.util.find_spec('numpy') is None
    ):
      self.skipTest('gdal_array or numpy missing')
    numpy = importlib.import_module('numpy')

    filename = str(self.vsimem_dir / 'test.gpkg')

    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
    assert lyr is not None
    self.assertEqual(lyr.TestCapability(ogr.OLCFastGetArrowStream), 1)

    field = ogr.FieldDefn('str', ogr.OFTString)
    lyr.CreateField(field)

    field = ogr.FieldDefn('bool', ogr.OFTInteger)
    field.SetSubType(ogr.OFSTBoolean)
    lyr.CreateField(field)

    field = ogr.FieldDefn('int16', ogr.OFTInteger)
    field.SetSubType(ogr.OFSTInt16)
    lyr.CreateField(field)

    self.assertTrue(
        ds.AddFieldDomain(
            ogr.CreateCodedFieldDomain(
                'enum_domain',
                '',
                ogr.OFTInteger,
                ogr.OFSTNone,
                {1: 'one', '2': None},
            )
        )
    )
    field = ogr.FieldDefn('int32', ogr.OFTInteger)
    field.SetDomainName('enum_domain')
    lyr.CreateField(field)

    field = ogr.FieldDefn('int64', ogr.OFTInteger64)
    lyr.CreateField(field)

    field = ogr.FieldDefn('float32', ogr.OFTReal)
    field.SetSubType(ogr.OFSTFloat32)
    lyr.CreateField(field)

    field = ogr.FieldDefn('float64', ogr.OFTReal)
    lyr.CreateField(field)

    field = ogr.FieldDefn('date', ogr.OFTDate)
    lyr.CreateField(field)

    field = ogr.FieldDefn('datetime', ogr.OFTDateTime)
    lyr.CreateField(field)

    field = ogr.FieldDefn('binary', ogr.OFTBinary)
    lyr.CreateField(field)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('bool', 1)
    f.SetField('int16', -12345)
    f.SetField('int32', 12345678)
    f.SetField('int64', 12345678901234)
    f.SetField('float32', 1.25)
    f.SetField('float64', 1.250123)
    f.SetField('str', 'abc')
    f.SetField('date', '2022-05-31')
    f.SetField('datetime', '2022-05-31T12:34:56.789Z')
    f.SetField('binary', b'\xDE\xAD')
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 2)'))
    lyr.CreateFeature(f)

    f2 = ogr.Feature(lyr.GetLayerDefn())
    f2.SetField('bool', 0)
    lyr.CreateFeature(f2)

    f3 = ogr.Feature(lyr.GetLayerDefn())
    f3.SetField('int16', 123)
    f3.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(-1 2)'))
    lyr.CreateFeature(f3)

    ds = None

    ds = ogr.Open(filename)
    assert ds is not None

    with ds.ExecuteSQL('SELECT * FROM test WHERE 0=1') as sql_lyr:
      stream = sql_lyr.GetArrowStream()
      array = stream.GetNextRecordBatch()
      self.assertIsNone(array)
      del stream

    lyr = ds.GetLayer(0)
    assert lyr is not None
    stream = lyr.GetArrowStream()
    array = stream.GetNextRecordBatch()
    self.assertEqual(array.GetChildrenCount(), 12)
    del array
    del stream
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None

    if importlib.util.find_spec('pyarrow') is not None:
      stream = lyr.GetArrowStreamAsPyArrow()
      _ = [batch for batch in stream]

    for i in range(2):
      with gdal.config_options(
          {'OGR_GPKG_STREAM_BASE_IMPL': 'YES'} if i == 1 else {}
      ):
        stream = lyr.GetArrowStreamAsNumPy(
            options=['USE_MASKED_ARRAYS=NO', 'MAX_FEATURES_IN_BATCH=2']
        )
        batches = [batch for batch in stream]
        self.assertEqual(len(batches), 2)
        batch = batches[0]

        self.assertEqual(
            batch.keys(),
            {
                'fid',
                'str',
                'bool',
                'int16',
                'int32',
                'int64',
                'float32',
                'float64',
                'date',
                'datetime',
                'binary',
                'geom',
            },
        )

        self.assertEqual(batch['fid'][0], 1)
        self.assertEqual(len(batch['fid']), 2)
        for fieldname in (
            'bool',
            'int16',
            'int32',
            'int64',
            'float32',
            'float64',
        ):
          self.assertEqual(batch[fieldname][0], f.GetField(fieldname))
        self.assertEqual(batch['str'][0], f.GetField('str').encode('utf-8'))
        self.assertEqual(batch['date'][0], numpy.datetime64('2022-05-31'))
        self.assertEqual(
            batch['datetime'][0], numpy.datetime64('2022-05-31T12:34:56.789')
        )
        self.assertEqual(bytes(batch['binary'][0]), b'\xDE\xAD')
        self.assertEqual(len(bytes(batch['geom'][0])), 21)

        self.assertEqual(batch['fid'][1], 2)
        self.assertFalse(batch['bool'][1])
        self.assertIsNone(batch['geom'][1])

        batch = batches[1]
        self.assertEqual(
            batch.keys(),
            {
                'fid',
                'str',
                'bool',
                'int16',
                'int32',
                'int64',
                'float32',
                'float64',
                'date',
                'datetime',
                'binary',
                'geom',
            },
        )

        self.assertEqual(batch['fid'][0], 3)
        self.assertEqual(batch['int16'][0], 123)
        self.assertEqual(len(batch['fid']), 1)

        self.assertEqual(lyr.SetNextByIndex(1), ogr.OGRERR_NONE)
        stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
        batches = [batch for batch in stream]
        self.assertEqual(len(batches), 1)
        self.assertEqual(list(batches[0]['fid']), [2, 3])

    with ds.ExecuteSQL('SELECT * FROM test') as sql_lyr:
      self.assertEqual(sql_lyr.SetNextByIndex(1), ogr.OGRERR_NONE)
      stream = sql_lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 1)
      self.assertEqual(list(batches[0]['fid']), [2, 3])

    with lyr.GetArrowStreamAsNumPy(
        options=['MAX_FEATURES_IN_BATCH=1']
    ) as stream:
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 3)
      self.assertEqual(len(batches[0]['fid']), 1)
      self.assertEqual(batches[0]['fid'][0], 1)
      self.assertEqual(len(batches[1]['fid']), 1)
      self.assertEqual(batches[1]['fid'][0], 2)
      self.assertEqual(len(batches[2]['fid']), 1)
      self.assertEqual(batches[2]['fid'][0], 3)

    for i in range(2):
      with lyr.GetArrowStreamAsNumPy(
          options=['MAX_FEATURES_IN_BATCH=1']
      ) as stream:
        batch = stream.GetNextRecordBatch()
        self.assertEqual(len(batch['fid']), 1, str(i))
        self.assertEqual(batch['fid'][0], 1, str(i))

    lyr.SetIgnoredFields([
        lyr.GetLayerDefn().GetFieldDefn(i).GetNameRef()
        for i in range(lyr.GetLayerDefn().GetFieldCount())
    ])
    with lyr.GetArrowStreamAsNumPy(options=['INCLUDE_FID=NO']) as stream:
      batch = stream.GetNextRecordBatch()
      self.assertEqual(len(batch['geom']), 3)
      self.assertGreater(len(batch['geom'][0]), 0)
    lyr.SetIgnoredFields([])

    # Test attribute filter
    lyr.SetAttributeFilter('int16 = 123')
    stream = lyr.GetArrowStreamAsNumPy()
    batches = [batch for batch in stream]
    lyr.SetAttributeFilter(None)
    self.assertEqual(len(batches), 1)
    self.assertEqual(len(batches[0]['fid']), 1)
    self.assertEqual(batches[0]['fid'][0], 3)

    for i in range(2):
      lyr.SetAttributeFilter('1 = 1')
      with lyr.GetArrowStreamAsNumPy(
          options=['MAX_FEATURES_IN_BATCH=1']
      ) as stream:
        batch = stream.GetNextRecordBatch()
        self.assertEqual(len(batch['fid']), 1, str(i))
        self.assertEqual(batch['fid'][0], 1, str(i))
        batch = stream.GetNextRecordBatch()
        self.assertEqual(len(batch['fid']), 1, str(i))
        self.assertEqual(batch['fid'][0], 2, str(i))
      lyr.SetAttributeFilter(None)

    # Test spatial filter
    lyr.SetSpatialFilterRect(0, 0, 10, 10)
    stream = lyr.GetArrowStreamAsNumPy()
    batches = [batch for batch in stream]
    lyr.SetSpatialFilter(None)
    self.assertEqual(len(batches), 1)
    self.assertEqual(len(batches[0]['fid']), 1)
    self.assertEqual(batches[0]['fid'][0], 1)

    # Test ignored fields
    self.assertEqual(lyr.SetIgnoredFields(['geom', 'int16']), ogr.OGRERR_NONE)
    stream = lyr.GetArrowStreamAsNumPy(options=['INCLUDE_FID=NO'])
    batches = [batch for batch in stream]
    lyr.SetIgnoredFields([])
    batch = batches[0]
    self.assertEqual(
        batch.keys(),
        {
            'str',
            'bool',
            'int32',
            'int64',
            'float32',
            'float64',
            'date',
            'datetime',
            'binary',
        },
    )

    # Check that OGR_GPKG_FillArrowArray_INTERNAL() function is no longer
    # registered
    with _quiet_error_handler():
      sql_lyr = ds.ExecuteSQL(
          'SELECT 1 FROM pragma_function_list WHERE'
          " name=lower('OGR_GPKG_FillArrowArray_INTERNAL')"
      )
    if sql_lyr:
      fc = sql_lyr.GetFeatureCount()
      ds.ReleaseResultSet(sql_lyr)
      self.assertEqual(fc, 0)

    # Test invalid filter and exceptions
    with _enable_exceptions():
      lyr.SetAttributeFilter('invalid')
      with self.assertRaisesRegex(Exception, 'no such column: invalid'):
        stream = lyr.GetArrowStreamAsNumPy()
        _ = [batch for batch in stream]

      lyr.SetAttributeFilter('1 = 1')
      stream = lyr.GetArrowStreamAsNumPy()
      self.assertEqual(len([batch for batch in stream]), 1)

    ds = None

    self.driver.DeleteDataSource(filename)

  @parameterized.parameters(
      (201, 100, 1),
      (200, 100, 2),
      (201, 100, 2),
      (201, 100, 3),
      (299, 100, 3),
      (300, 100, 3),
      (301, 100, 3),
      (400, 100, 3),
      (901, 100, 3),
      (1001, 100, 3),
  )
  def test_ogr_gpkg_arrow_stream_numpy_multi_threading(
      self, num_features: int, batch_size: int, num_threads: int
  ) -> None:
    if (
        not hasattr(gdal_array, '_RecordBatchAsNumpy')
        or importlib.util.find_spec('numpy') is None
    ):
      self.skipTest('gdal_array or numpy missing')

    filename = str(self.vsimem_dir / 'test.gpkg')

    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
    assert lyr is not None

    for i in range(num_features):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt(f'POINT({i} {i})'))
      lyr.CreateFeature(f)

    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None
    with gdal.config_option('OGR_GPKG_NUM_THREADS', str(num_threads)):
      stream = lyr.GetArrowStreamAsNumPy(
          options=[
              'USE_MASKED_ARRAYS=NO',
              f'MAX_FEATURES_IN_BATCH={batch_size}',
          ]
      )

    got_msg: list[str] = []

    def my_handler(error_class: int, errno: int, msg: str) -> None:
      del errno
      if error_class != gdal.CE_Debug:
        got_msg.append(msg)

    gdal.PushErrorHandler(my_handler)
    try:
      batches = [batch for batch in stream]
    finally:
      gdal.PopErrorHandler()

    self.assertEmpty(got_msg)

    self.assertEqual(
        len(batches), (num_features + batch_size - 1) // batch_size
    )
    i = 0
    for batch in batches:
      for wkb in batch['geom']:
        geom = ogr.CreateGeometryFromWkb(wkb)
        assert geom is not None
        self.assertEqual(geom.ExportToIsoWkt(), f'POINT ({i} {i})')
        i += 1
    self.assertEqual(i, num_features)
    ds = None

  def test_ogr_gpkg_arrow_stream_numpy_bool_field(self) -> None:
    if (
        not hasattr(gdal_array, '_RecordBatchAsNumpy')
        or importlib.util.find_spec('numpy') is None
    ):
      self.skipTest('gdal_array or numpy missing')

    filename = str(self.vsimem_dir / 'test.gpkg')

    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
    assert lyr is not None
    self.assertEqual(lyr.TestCapability(ogr.OLCFastGetArrowStream), 1)

    field = ogr.FieldDefn('bool', ogr.OFTInteger)
    field.SetSubType(ogr.OFSTBoolean)
    lyr.CreateField(field)

    for i in range(60):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetField('bool', False if (i % 3) == 0 else True)
      lyr.CreateFeature(f)

    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None

    stream = lyr.GetArrowStreamAsNumPy()
    batches = [batch for batch in stream]
    lyr.SetAttributeFilter(None)
    self.assertEqual(len(batches), 1)
    self.assertEqual(
        list(batches[0]['bool']),
        [False if (i % 3) == 0 else True for i in range(60)],
    )

    ds = None
    self.driver.DeleteDataSource(filename)

  @parameterized.parameters(False, True)
  def test_ogr_gpkg_arrow_stream_numpy_more_than_125_columns(
      self, with_filter: bool
  ) -> None:
    if (
        not hasattr(gdal_array, '_RecordBatchAsNumpy')
        or importlib.util.find_spec('numpy') is None
    ):
      self.skipTest('gdal_array or numpy missing')

    filename = str(self.vsimem_dir / 'test.gpkg')

    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
    assert lyr is not None
    nfields = 125 + 2 * 126
    for i in range(nfields):
      lyr.CreateField(ogr.FieldDefn(f'field{i}', ogr.OFTInteger))
    for j in range(2):
      f = ogr.Feature(lyr.GetLayerDefn())
      for i in range(nfields):
        f.SetField(i, j * 1000 + i)
      f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
      lyr.CreateFeature(f)

    if with_filter:
      lyr.SetAttributeFilter('1 = 1')
    stream = lyr.GetArrowStreamAsNumPy()
    batches = [batch for batch in stream]
    lyr.SetSpatialFilter(None)
    self.assertEqual(len(batches), 1)
    self.assertEqual(len(batches[0]['fid']), 2)
    self.assertEqual(list(batches[0]['fid']), [1, 2])
    self.assertEqual(len(bytes(batches[0]['geom'][0])), 21)
    self.assertEqual(len(bytes(batches[0]['geom'][1])), 21)
    for i in range(nfields):
      self.assertEqual(list(batches[0][f'field{i}']), [i, i + 1000])
    ds = None

  @parameterized.parameters('direct', 'sql')
  def test_ogr_gpkg_arrow_stream_numpy_detailed_spatial_filter(
      self, layer_type: str
  ) -> None:
    if (
        not hasattr(gdal_array, '_RecordBatchAsNumpy')
        or importlib.util.find_spec('numpy') is None
    ):
      self.skipTest('gdal_array or numpy missing')

    filename = str(
        self.vsimem_dir
        / 'test_ogr_gpkg_arrow_stream_numpy_detailed_spatial_filter.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test', options=['FID=fid'])
    assert lyr is not None
    for idx, wkt in enumerate([
        'POINT(1 2)',
        'MULTIPOINT(0 0,1 2)',
        'LINESTRING(3 4,5 6)',
        'MULTILINESTRING((7 8,7.5 8.5),(3 4,5 6))',
        'POLYGON((10 20,10 30,20 30,10 20),(11 21,11 29,19 29,11 21))',
        (
            'MULTIPOLYGON(((100 100,100 200,200 200,100 100)),((10 20,10 30,20'
            ' 30,10 20),(11 21,11 29,19 29,11 21)))'
        ),
        'LINESTRING EMPTY',
        'MULTILINESTRING EMPTY',
        'POLYGON EMPTY',
        'MULTIPOLYGON EMPTY',
        'GEOMETRYCOLLECTION EMPTY',
    ]):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetFID(idx)
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt(wkt))
      lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    if layer_type == 'direct':
      lyr = ds.GetLayer(0)
    else:
      lyr = ds.ExecuteSQL('SELECT * FROM test')
    assert lyr is not None

    eps = 1e-1

    # Select nothing
    with _spatial_filter(lyr, 6, 0, 8, 1):
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      self.assertEmpty(batches)

    # Select POINT and MULTIPOINT
    with _spatial_filter(lyr, 1 - eps, 2 - eps, 1 + eps, 2 + eps):
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 1)
      self.assertEqual(list(batches[0]['fid']), [0, 1])
      self.assertEqual([f.GetFID() for f in lyr], [0, 1])

    # Select LINESTRING and MULTILINESTRING due to point falling in bbox
    with _spatial_filter(lyr, 3 - eps, 4 - eps, 3 + eps, 4 + eps):
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 1)
      self.assertEqual(list(batches[0]['fid']), [2, 3])
      self.assertEqual([f.GetFID() for f in lyr], [2, 3])

    # Select LINESTRING and MULTILINESTRING due to point falling in bbox
    with _spatial_filter(lyr, 5 - eps, 6 - eps, 5 + eps, 6 + eps):
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 1)
      self.assertEqual(list(batches[0]['fid']), [2, 3])
      self.assertEqual([f.GetFID() for f in lyr], [2, 3])

    # Select LINESTRING and MULTILINESTRING due to more generic intersection
    with _spatial_filter(lyr, 4 - eps, 5 - eps, 4 + eps, 5 + eps):
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 1)
      self.assertEqual(list(batches[0]['fid']), [2, 3])
      self.assertEqual([f.GetFID() for f in lyr], [2, 3])

    # Select POLYGON and MULTIPOLYGON due to point falling in bbox
    with _spatial_filter(lyr, 10 - eps, 20 - eps, 10 + eps, 20 + eps):
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 1)
      self.assertEqual(list(batches[0]['fid']), [4, 5])
      self.assertEqual([f.GetFID() for f in lyr], [4, 5])

    # bbox with polygon hole
    with _spatial_filter(lyr, 12 - eps, 20.5 - eps, 12 + eps, 20.5 + eps):
      stream = lyr.GetArrowStreamAsNumPy(options=['USE_MASKED_ARRAYS=NO'])
      batches = [batch for batch in stream]
      if _have_geos():
        self.assertEmpty(batches)
      else:
        self.assertEqual(len(batches), 1)
        self.assertEqual(list(batches[0]['fid']), [4, 5])
        self.assertEqual([f.GetFID() for f in lyr], [4, 5])

    if layer_type != 'direct':
      ds.ReleaseResultSet(lyr)

    ds = None

  def test_ogr_gpkg_arrow_stream_empty_file(self) -> None:
    ds = self.driver.CreateDataSource(str(self.vsimem_dir / 'test.gpkg'))
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPoint)
    assert lyr is not None
    self.assertEqual(lyr.TestCapability(ogr.OLCFastGetArrowStream), 1)
    stream = lyr.GetArrowStream()
    self.assertIsNone(stream.GetNextRecordBatch())
    del stream

    with gdal.config_option('OGR_GPKG_STREAM_BASE_IMPL', 'YES'):
      stream = lyr.GetArrowStream()
      self.assertIsNone(stream.GetNextRecordBatch())
      del stream

    with ds.ExecuteSQL('SELECT * FROM test') as sql_lyr:
      stream = sql_lyr.GetArrowStream()
      self.assertIsNone(stream.GetNextRecordBatch())
      del stream
    ds = None

  def test_ogr_gpkg_immutable(self) -> None:
    if sys.platform != 'linux':
      self.skipTest('Incorrect platform')
    if os.getuid() == 0:
      self.skipTest('running as root... skipping')

    ro_dir = self.tmp_dir / 'read_only_test_ogr_gpkg_immutable'
    ro_dir.mkdir(mode=0o755)
    self.addCleanup(os.chmod, ro_dir, 0o755)

    test_gpkg = str(ro_dir / 'test.gpkg')
    ds = self.driver.CreateDataSource(test_gpkg)
    assert ds is not None
    ds.CreateLayer('foo')
    with ds.ExecuteSQL('PRAGMA journal_mode = WAL') as sql_lyr:
      self.assertIsNotNone(sql_lyr)
    ds = None

    # Turn directory in read-only mode
    os.chmod(ro_dir, 0o555)

    with _quiet_error_handler():
      self.assertIsNone(gdal.OpenEx(test_gpkg, gdal.OF_VECTOR | gdal.OF_UPDATE))
      self.assertIsNone(
          gdal.OpenEx(
              test_gpkg,
              gdal.OF_VECTOR,
              open_options=['IMMUTABLE=NO'],
          )
      )

    gdal.ErrorReset()
    self.assertIsNotNone(
        gdal.OpenEx(
            test_gpkg,
            gdal.OF_VECTOR,
            open_options=['IMMUTABLE=YES'],
        )
    )
    self.assertEqual(gdal.GetLastErrorMsg(), '')

    gdal.ErrorReset()
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_STATIC_VIRTUAL_OGR', 'NO'):
        self.assertIsNotNone(ogr.Open(test_gpkg))
    self.assertNotEqual(gdal.GetLastErrorMsg(), '')

  @parameterized.parameters(
      (True, '1.2'),
      (True, '1.4'),
      (False, '1.2'),
      (False, '1.4'),
  )
  def test_ogr_gpkg_upsert_without_fid(
      self, with_geom: bool, gpkg_version: str
  ) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_upsert_without_fid.gpkg')
    ds = self.driver.CreateDataSource(
        filename, options=['VERSION=' + gpkg_version]
    )
    assert ds is not None
    lyr = ds.CreateLayer(
        'foo', geom_type=(ogr.wkbUnknown if with_geom else ogr.wkbNone)
    )
    assert lyr is not None
    self.assertEqual(
        lyr.CreateField(ogr.FieldDefn('other', ogr.OFTString)), ogr.OGRERR_NONE
    )
    unique_field = ogr.FieldDefn('unique_field', ogr.OFTString)
    unique_field.SetUnique(True)
    self.assertEqual(lyr.CreateField(unique_field), ogr.OGRERR_NONE)
    for i in range(5):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetField('unique_field', i + 1)
      if i < 4 and with_geom:
        f.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt('POINT (%d %d)' % (i, i))
        )
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    ds = None

    ds = ogr.Open(filename, update=1)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('unique_field', '2')
    f.SetField('other', 'foo')
    if with_geom:
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (10 10)'))
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)

    if _get_sqlite_version() >= (3, 35, 0):
      self.assertEqual(f.GetFID(), 2)

    if with_geom and gpkg_version == '1.2':
      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'rtree_foo_geom_update1'",
          dialect='DEBUG',
      )
      assert sql_lyr is not None
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'rtree_foo_geom_update6'",
          dialect='DEBUG',
      )
      assert sql_lyr is not None
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'rtree_foo_geom_update7'",
          dialect='DEBUG',
      )
      assert sql_lyr is not None
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      ds.ReleaseResultSet(sql_lyr)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('unique_field', '3')
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('unique_field', '4')
    if with_geom:
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (20 20)'))
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)

    ds = None

    self.validate(filename)

    ds = ogr.Open(filename)
    assert ds is not None

    if with_geom and gpkg_version == '1.2':
      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'rtree_foo_geom_update1'",
          dialect='DEBUG',
      )
      assert sql_lyr is not None
      self.assertEqual(sql_lyr.GetFeatureCount(), 1)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'rtree_foo_geom_update6'",
          dialect='DEBUG',
      )
      assert sql_lyr is not None
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      ds.ReleaseResultSet(sql_lyr)

      sql_lyr = ds.ExecuteSQL(
          "SELECT 1 FROM sqlite_master WHERE name = 'rtree_foo_geom_update7'",
          dialect='DEBUG',
      )
      assert sql_lyr is not None
      self.assertEqual(sql_lyr.GetFeatureCount(), 0)
      ds.ReleaseResultSet(sql_lyr)

    lyr = ds.GetLayer(0)
    assert lyr is not None

    f = lyr.GetFeature(2)
    assert f is not None
    self.assertEqual(f['unique_field'], '2')
    self.assertEqual(f['other'], 'foo')
    if with_geom:
      geom = f.GetGeometryRef()
      assert geom is not None
      self.assertEqual(geom.ExportToWkt(), 'POINT (10 10)')

    f = lyr.GetFeature(3)
    assert f is not None
    self.assertIsNone(f.GetGeometryRef())

    f = lyr.GetFeature(4)
    assert f is not None
    if with_geom:
      geom = f.GetGeometryRef()
      assert geom is not None
      self.assertEqual(geom.ExportToWkt(), 'POINT (20 20)')

    ds = None

  def test_ogr_gpkg_get_geometry_types(self) -> None:
    """Test Layer.GetGeometryTypes()."""
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_get_geometry_types.gpkg')
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('layer')
    assert lyr is not None

    self.assertEqual(lyr.GetGeometryTypes(), {})

    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    self.assertEqual(lyr.GetGeometryTypes(), {ogr.wkbNone: 1})

    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    self.assertEqual(
        lyr.GetGeometryTypes(callback=lambda x, y, z: 1), {ogr.wkbNone: 2}
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT EMPTY'))
    lyr.CreateFeature(f)
    self.assertEqual(lyr.GetGeometryTypes(), {ogr.wkbNone: 2, ogr.wkbPoint: 1})
    lyr.SetAttributeFilter('1')
    self.assertEqual(lyr.GetGeometryTypes(), {ogr.wkbNone: 2, ogr.wkbPoint: 1})
    lyr.SetAttributeFilter('0')
    self.assertEqual(lyr.GetGeometryTypes(), {})
    lyr.SetAttributeFilter(None)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POLYGON EMPTY'))
    lyr.CreateFeature(f)
    self.assertEqual(
        lyr.GetGeometryTypes(),
        {
            ogr.wkbNone: 2,
            ogr.wkbPoint: 1,
            ogr.wkbPolygon: 1,
        },
    )
    self.assertEqual(
        lyr.GetGeometryTypes(flags=ogr.GGT_STOP_IF_MIXED),
        {
            ogr.wkbNone: 2,
            ogr.wkbPoint: 1,
            ogr.wkbPolygon: 1,
        },
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (0 0,1 1)'))
    lyr.CreateFeature(f)
    self.assertEqual(
        lyr.GetGeometryTypes(),
        {
            ogr.wkbNone: 2,
            ogr.wkbPoint: 1,
            ogr.wkbPolygon: 1,
            ogr.wkbLineString: 1,
        },
    )
    self.assertEqual(
        lyr.GetGeometryTypes(geom_field=0, flags=ogr.GGT_STOP_IF_MIXED),
        {
            ogr.wkbNone: 2,
            ogr.wkbPoint: 1,
            ogr.wkbPolygon: 1,
        },
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(
        ogr.CreateGeometryFromWkt(
            'GEOMETRYCOLLECTION Z(TIN Z(((0 0 0,0 1 0,1 1 0,0 0 0))))'
        )
    )
    lyr.CreateFeature(f)
    self.assertEqual(
        lyr.GetGeometryTypes(),
        {
            ogr.wkbNone: 2,
            ogr.wkbPoint: 1,
            ogr.wkbPolygon: 1,
            ogr.wkbLineString: 1,
            ogr.wkbGeometryCollection25D: 1,
        },
    )
    self.assertEqual(
        lyr.GetGeometryTypes(flags=ogr.GGT_GEOMCOLLECTIONZ_TINZ),
        {
            ogr.wkbNone: 2,
            ogr.wkbPoint: 1,
            ogr.wkbPolygon: 1,
            ogr.wkbLineString: 1,
            ogr.wkbTINZ: 1,
        },
    )

    with _enable_exceptions(), _quiet_error_handler():
      with self.assertRaises(Exception):
        lyr.GetGeometryTypes(geom_field=1)

    lyr.StartTransaction()
    for _ in range(1000):
      # 1000 because COUNT_VM_INSTRUCTIONS = 1000 in
      # OGRGeoPackageTableLayer::GetGeometryTypes()
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometry(ogr.CreateGeometryFromWkt('POINT EMPTY'))
      lyr.CreateFeature(f)
    lyr.CommitTransaction()
    with _enable_exceptions(), _quiet_error_handler():
      # Cancellation requires GDAL built with SQLITE_HAS_PROGRESS_HANDLER,
      # which is disabled in Google3's third_party/gdal/BUILD.
      try:
        lyr.GetGeometryTypes(callback=lambda x, y, z: 0)
      except Exception:
        pass

    self.assertEqual(
        lyr.GetGeometryTypes(),
        {
            ogr.wkbNone: 2,
            ogr.wkbPoint: 1001,
            ogr.wkbPolygon: 1,
            ogr.wkbLineString: 1,
            ogr.wkbGeometryCollection25D: 1,
        },
    )

    ds = None

  @parameterized.product(
      write_to_disk=(True, False),
      max_ram_usage_rtree=(1, 1000, None),
      simulate_insert_error=(True, False),
  )
  def test_ogr_gpkg_background_rtree_build(
      self,
      write_to_disk: bool,
      max_ram_usage_rtree: int | None,
      simulate_insert_error: bool,
  ) -> None:
    if write_to_disk:
      filepath = self.tmp_dir / 'test_ogr_gpkg_background_rtree_build.gpkg'
    else:
      filepath = self.vsimem_dir / 'test_ogr_gpkg_background_rtree_build.gpkg'
    filename = str(filepath)

    # Batch insertion only
    gdal.ErrorReset()

    options: dict[str, str | None] = {
        'OGR_GPKG_MAX_RAM_USAGE_RTREE': (
            str(max_ram_usage_rtree)
            if max_ram_usage_rtree is not None
            else None
        ),
        'OGR_GPKG_SIMULATE_INSERT_INTO_MY_RTREE_PREPARATION_ERROR': (
            'TRUE' if simulate_insert_error else None
        ),
    }
    with gdal.config_options(options, thread_local=False):
      ds = self.driver.CreateDataSource(filename)
      assert ds is not None
      with gdal.config_option(
          'OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE', 'YES'
      ):
        lyr = ds.CreateLayer('foo')
      assert lyr is not None
      self.assertEqual(lyr.StartTransaction(), ogr.OGRERR_NONE)
      for i in range(1000):
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt('POINT(%d %d)' % (10000 + i, i))
        )
        self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
        if i == 500:
          self.assertEqual(lyr.CommitTransaction(), ogr.OGRERR_NONE)
          self.assertEqual(lyr.StartTransaction(), ogr.OGRERR_NONE)
      self.assertEqual(lyr.CommitTransaction(), ogr.OGRERR_NONE)
      self.assertEqual(gdal.GetLastErrorMsg(), '')

      with gdal.config_option(
          'OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE', 'YES'
      ):
        lyr = ds.CreateLayer('bar')
      assert lyr is not None
      self.assertEqual(lyr.StartTransaction(), ogr.OGRERR_NONE)
      for i in range(900):
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt('POINT(%d %d)' % (-i, -i))
        )
        self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
        if i == 500:
          self.assertEqual(lyr.CommitTransaction(), ogr.OGRERR_NONE)
          self.assertEqual(lyr.StartTransaction(), ogr.OGRERR_NONE)
      self.assertEqual(lyr.CommitTransaction(), ogr.OGRERR_NONE)
      self.assertEqual(gdal.GetLastErrorMsg(), '')

      ds.Close()

    self.assertIsNone(
        gdal.VSIStatL(str(filepath.with_suffix('.gpkg.tmp_rtree_foo.db')))
    )
    self.assertIsNone(
        gdal.VSIStatL(str(filepath.with_suffix('.gpkg.tmp_rtree_bar.db')))
    )

    ds = ogr.Open(filename)
    assert ds is not None
    with ds.ExecuteSQL("SELECT rtreecheck('rtree_foo_geom')") as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertEqual(f.GetField(0), 'ok')
    with ds.ExecuteSQL('SELECT * FROM rtree_foo_geom') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 1000)
    foo_lyr = ds.GetLayerByName('foo')
    assert foo_lyr is not None
    for i in range(1000):
      foo_lyr.SetSpatialFilterRect(
          10000 + i - 0.5, i - 0.5, 10000 + i + 0.5, i + 0.5
      )
      self.assertEqual(foo_lyr.GetFeatureCount(), 1, str(i))
    with ds.ExecuteSQL("SELECT rtreecheck('rtree_bar_geom')") as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertEqual(f.GetField(0), 'ok')
    with ds.ExecuteSQL('SELECT * FROM rtree_bar_geom') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 900)
    bar_lyr = ds.GetLayerByName('bar')
    assert bar_lyr is not None
    for i in range(900):
      bar_lyr.SetSpatialFilterRect(-i - 0.5, -i - 0.5, -i + 0.5, -i + 0.5)
      self.assertEqual(bar_lyr.GetFeatureCount(), 1, str(i))
    ds = None

    gdal.Unlink(filename)

    # Test SetFeature() after batch insertion
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    with gdal.config_option('OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE', 'YES'):
      lyr = ds.CreateLayer('footoooooooooooooooooooooooooooooooooooooooooolong')
    assert lyr is not None
    f = None
    for _ in range(10):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    assert f is not None
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 1)'))
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    with ds.ExecuteSQL(
        'SELECT'
        " rtreecheck('rtree_footoooooooooooooooooooooooooooooooooooooooooolong_geom')"
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertEqual(f.GetField(0), 'ok')
    with ds.ExecuteSQL(
        'SELECT * FROM'
        ' rtree_footoooooooooooooooooooooooooooooooooooooooooolong_geom'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 10)
    lyr = ds.GetLayer(0)
    assert lyr is not None
    lyr.SetSpatialFilterRect(0.5, 0.5, 1.5, 1.5)
    self.assertEqual(lyr.GetFeatureCount(), 1)
    lyr.SetSpatialFilterRect(-0.5, -0.5, 0.5, 0.5)
    self.assertEqual(lyr.GetFeatureCount(), 9)
    ds = None

    gdal.Unlink(filename)

    # Test DeleteFeature() after batch insertion
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    with gdal.config_option('OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE', 'YES'):
      lyr = ds.CreateLayer('foo with space')
    assert lyr is not None
    f = None
    for _ in range(10):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    assert f is not None
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 1)'))
    self.assertEqual(lyr.DeleteFeature(f.GetFID()), ogr.OGRERR_NONE)
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    with ds.ExecuteSQL(
        "SELECT rtreecheck('rtree_foo with space_geom')"
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertEqual(f.GetField(0), 'ok')
    with ds.ExecuteSQL('SELECT * FROM "rtree_foo with space_geom"') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 9)
    ds = None

    # Test RollbackTransaction() after batch insertion
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    with gdal.config_option('OGR_GPKG_THREADED_RTREE_AT_FIRST_FEATURE', 'YES'):
      lyr = ds.CreateLayer('foo')
    assert lyr is not None
    for _ in range(10):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    lyr.StartTransaction()
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 1)'))
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    lyr.RollbackTransaction()
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    with ds.ExecuteSQL("SELECT rtreecheck('rtree_foo_geom')") as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertEqual(f.GetField(0), 'ok')
    with ds.ExecuteSQL('SELECT * FROM rtree_foo_geom') as sql_lyr:
      self.assertEqual(sql_lyr.GetFeatureCount(), 10)
    lyr = ds.GetLayer(0)
    assert lyr is not None
    lyr.SetSpatialFilterRect(-0.5, -0.5, 1.5, 1.5)
    self.assertEqual(lyr.GetFeatureCount(), 10)
    ds = None

  def test_ogr_gpkg_detect_broken_rtree_gdal_3_6_0(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_detect_broken_rtree_gdal_3_6_0.gpkg'
    )

    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('foo')
    assert lyr is not None
    for i in range(100):
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometryDirectly(
          ogr.CreateGeometryFromWkt('POINT(%d %d)' % (i % 10, i // 10))
      )
      lyr.CreateFeature(f)
    ds = None

    # Voluntary corrupt the RTree by removing the entry for the last feature
    ds = ogr.Open(filename, update=1)
    assert ds is not None
    sql_lyr = ds.ExecuteSQL('DELETE FROM rtree_foo_geom WHERE id = 100')
    ds.ReleaseResultSet(sql_lyr)
    ds = None

    with gdal.config_option('OGR_GPKG_THRESHOLD_DETECT_BROKEN_RTREE', '100'):
      ds = ogr.Open(filename)
      assert ds is not None
      lyr = ds.GetLayer(0)
      assert lyr is not None
      with _quiet_error_handler():
        gdal.ErrorReset()
        lyr.SetSpatialFilterRect(8.5, 8.5, 9.5, 9.5)
        self.assertIn(
            'Spatial index (perhaps created with GDAL 3.6.0) of table foo is'
            ' corrupted',
            gdal.GetLastErrorMsg(),
        )
      self.assertEqual(lyr.GetFeatureCount(), 1)
      ds = None

  @parameterized.parameters(
      (None, None),
      ("X'0001'", None),
      ('POINT EMPTY', 0),
      ('LINESTRING(1 2,3 4)', 0),
      ('POLYGON EMPTY', 0),
      ('POLYGON ((0 0,0 1,1 1,0 0))', 0.5),
      ('POLYGON Z ((0 0 100,0 1 100,1 1 100,0 0 100))', 0.5),
      ('POLYGON M ((0 0 100,0 1 100,1 1 100,0 0 100))', 0.5),
      ('POLYGON ZM ((0 0 100 200,0 1 100 200,1 1 100 200,0 0 100 200))', 0.5),
      (
          (
              'POLYGON ((0 0,0 1,1 1,1 0,0 0),(0.25 0.25,0.25 0.75,0.75'
              ' 0.75,0.75 0.25,0.25 0.25))'
          ),
          0.75,
      ),
      ('MULTIPOLYGON EMPTY', 0),
      ('MULTIPOLYGON (((0 0,0 1,1 1,0 0)))', 0.5),
      ('MULTIPOLYGON (((0 0,0 1,1 1,0 0)),((10 0,10 1,11 1,10 0)))', 1),
      ('MULTIPOLYGON Z (((0 0 100,0 1 100,1 1 100,0 0 100)))', 0.5),
      ('MULTIPOLYGON M (((0 0 100,0 1 100,1 1 100,0 0 100)))', 0.5),
      (
          (
              'MULTIPOLYGON ZM (((0 0 100 200,0 1 100 200,1 1 100 200,0 0 100'
              ' 200)))'
          ),
          0.5,
      ),
      ('CURVEPOLYGON ((0 0,0 1,1 1,0 0))', 0.5),
      ('MULTISURFACE (((0 0,0 1,1 1,0 0)))', 0.5),
      # Below is Spatialite encoding of POLYGON((0 0,0 1,1 1,0 0))
      (
          "X'00010000000000000000000000000000000000000000000000000000f03f000000000000f03f7c030000000100000004000000000000000000000000000000000000000000000000000000000000000000f03f000000000000f03f000000000000f03f00000000000000000000000000000000fe'",
          0.5,
      ),
  )
  def test_ogr_gpkg_st_area(
      self, wkt_or_binary: str | None, area: float | None
  ) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_st_area.gpkg')
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test')
    assert lyr is not None
    if wkt_or_binary and wkt_or_binary.startswith("X'"):
      sql = 'INSERT INTO test(geom) VALUES (' + wkt_or_binary + ')'
      ds.ExecuteSQL(sql)
    else:
      f = ogr.Feature(lyr.GetLayerDefn())
      if wkt_or_binary:
        f.SetGeometryDirectly(ogr.CreateGeometryFromWkt(wkt_or_binary))
      lyr.CreateFeature(f)
    sql_lyr = ds.ExecuteSQL('SELECT ST_Area(geom) FROM test')
    assert sql_lyr is not None
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)
    ds = None
    gdal.Unlink(filename)
    self.assertEqual(f.GetField(0), area)

  def test_ogr_gpkg_read_generated_column(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_read_generated_column.gpkg')
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    ds.ExecuteSQL(
        'CREATE TABLE test (fid INTEGER PRIMARY KEY NOT NULL,unused'
        ' TEXT,strfield TEXT,strfield_generated TEXT GENERATED ALWAYS AS'
        " (strfield || '_generated'),intfield_generated_stored INTEGER"
        ' GENERATED ALWAYS AS (5) STORED)'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents'
        ' (table_name,data_type,identifier,description,last_change,srs_id)'
        " VALUES ('test','attributes','test','','',0)"
    )
    ds = None

    ds = ogr.Open(filename, update=1)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 4)
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(2).GetName(), 'strfield_generated'
    )
    self.assertTrue(lyr.GetLayerDefn().GetFieldDefn(2).IsGenerated())
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(2).GetType(), ogr.OFTString
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(3).GetName(),
        'intfield_generated_stored',
    )
    self.assertTrue(lyr.GetLayerDefn().GetFieldDefn(3).IsGenerated())
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(3).GetType(), ogr.OFTInteger64
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('strfield', 'foo')
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

    lyr.ResetReading()
    f = lyr.GetNextFeature()
    assert f is not None
    self.assertEqual(f['strfield'], 'foo')
    self.assertEqual(f['strfield_generated'], 'foo_generated')
    self.assertEqual(f['intfield_generated_stored'], 5)
    self.assertTrue(f.GetFieldDefnRef(2).IsGenerated())
    self.assertTrue(f.GetFieldDefnRef(3).IsGenerated())

    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    assert f is not None
    self.assertEqual(f['strfield'], 'foo')
    self.assertEqual(f['strfield_generated'], 'foo_generated')

    f.SetField('strfield', 'bar')
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    assert f is not None
    self.assertEqual(f['strfield'], 'bar')
    self.assertEqual(f['strfield_generated'], 'bar_generated')

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('strfield', 'foo2')
    f.SetField('strfield_generated', 'ignored')
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

    f = lyr.GetFeature(2)
    assert f is not None
    self.assertEqual(f['strfield'], 'foo2')
    self.assertEqual(f['strfield_generated'], 'foo2_generated')
    f = None

    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('strfield', 'foo3')
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

    f = lyr.GetFeature(3)
    assert f is not None
    self.assertEqual(f['strfield'], 'foo3')
    # None for sqlite < 3.35.5 that uses table recreation for DeleteField()
    # implementation and thus for now the generated column expression is lost
    self.assertIn(f['strfield_generated'], ('foo3_generated', None))

    ds = None

  def test_ogr_gpkg_sql_gdal_get_pixel_value(self) -> None:
    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_sql_gdal_get_pixel_value.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None

    byte_tif = gcore_util.GetTestFilePath('byte.tif')
    float64_tif = gcore_util.GetTestFilePath('float64.tif')

    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      sql_lyr = ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{byte_tif}', 1, 'georef', 440780,"
          ' 3751080)'
      )
    assert sql_lyr is not None
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)
    self.assertEqual(f[0], 156)

    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      with ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{byte_tif}', 1, 'georef', 440780 + 30,"
          ' 3751080 - 30)'
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertEqual(f[0], 156)

    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      with ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{byte_tif}', 1, 'georef', 440780 + 30,"
          " 3751080 - 30, 'cubicspline')"
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertAlmostEqual(f[0], 150.1388888888889)

    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      sql_lyr = ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{byte_tif}', 1, 'pixel', 1, 4)"
      )
    assert sql_lyr is not None
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)
    self.assertEqual(f[0], 156)

    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      sql_lyr = ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{float64_tif}', 1, 'pixel', 0, 1)"
      )
    assert sql_lyr is not None
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)
    self.assertEqual(f[0], 115.0)

    # Invalid column
    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      sql_lyr = ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{byte_tif}', 1, 'pixel', -1, 0)"
      )
    assert sql_lyr is not None
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)
    self.assertIsNone(f[0])

    # Missing OGR_SQLITE_ALLOW_EXTERNAL_ACCESS
    with _quiet_error_handler():
      sql_lyr = ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{byte_tif}', 1, 'georef', 440720,"
          ' 3751320)'
      )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # NULL as 1st arg
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
        sql_lyr = ds.ExecuteSQL(
            "select gdal_get_pixel_value(NULL, 1, 'pixel', 0, 0)"
        )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # NULL as 2nd arg
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
        sql_lyr = ds.ExecuteSQL(
            f"select gdal_get_pixel_value('{byte_tif}', NULL, 'pixel', 0, 0)"
        )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # NULL as 3rd arg
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
        sql_lyr = ds.ExecuteSQL(
            f"select gdal_get_pixel_value('{byte_tif}', 1, NULL, 0, 0)"
        )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # NULL as 4th arg
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
        sql_lyr = ds.ExecuteSQL(
            f"select gdal_get_pixel_value('{byte_tif}', 1, 'pixel', NULL, 0)"
        )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # NULL as 5th arg
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
        sql_lyr = ds.ExecuteSQL(
            f"select gdal_get_pixel_value('{byte_tif}', 1, 'pixel', 0, NULL)"
        )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # Invalid band number
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
        sql_lyr = ds.ExecuteSQL(
            f"select gdal_get_pixel_value('{byte_tif}', 0, 'pixel', 0, 0)"
        )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # Invalid value for 3rd argument
    with _quiet_error_handler():
      with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
        sql_lyr = ds.ExecuteSQL(
            f"select gdal_get_pixel_value('{byte_tif}', 1, 'invalid', 0, 0)"
        )
      assert sql_lyr is not None
      f = sql_lyr.GetNextFeature()
      assert f is not None
      ds.ReleaseResultSet(sql_lyr)
      self.assertIsNone(f[0])

    # Test Int64
    tmp_filename = str(self.vsimem_dir / 'tmp_int64.tif')
    tmp_ds = gdal.GetDriverByName('GTiff').Create(
        tmp_filename, 1, 1, 1, gdal.GDT_Int64
    )
    assert tmp_ds is not None
    tmp_ds.WriteRaster(0, 0, 1, 1, struct.pack('q', (1 << 63) - 1))
    tmp_ds.Close()

    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      with ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{tmp_filename}', 1, 'pixel', 0, 0)"
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertEqual(f[0], (1 << 63) - 1)

    # Test UInt64
    tmp_filename = str(self.vsimem_dir / 'tmp_uint64.tif')
    tmp_ds = gdal.GetDriverByName('GTiff').Create(
        tmp_filename, 2, 1, 1, gdal.GDT_UInt64
    )
    assert tmp_ds is not None
    tmp_ds.WriteRaster(0, 0, 1, 1, struct.pack('Q', (1 << 63) - 1))
    tmp_ds.WriteRaster(1, 0, 1, 1, struct.pack('Q', (1 << 64) - 1))
    tmp_ds.Close()

    with gdal.config_option('OGR_SQLITE_ALLOW_EXTERNAL_ACCESS', 'YES'):
      with ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{tmp_filename}', 1, 'pixel', 0, 0)"
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertEqual(f[0], (1 << 63) - 1)

      with ds.ExecuteSQL(
          f"select gdal_get_pixel_value('{tmp_filename}', 1, 'pixel', 1, 0)"
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertEqual(f[0], float((1 << 64) - 1))

    ds = None

  def test_ogr_gpkg_sozip(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_sozip.gpkg.zip')

    with gdal.config_options({
        'CPL_SOZIP_MIN_FILE_SIZE': '256',
        'CPL_VSIL_DEFLATE_CHUNK_SIZE': '128',
    }):
      ds = self.driver.CreateDataSource(filename)
      assert ds is not None
      ds.CreateLayer('foo')
      ds = None

    md = gdal.GetFileMetadata(
        f'/vsizip/{filename}/test_ogr_gpkg_sozip.gpkg', 'ZIP'
    )
    self.assertEqual(md['SOZIP_VALID'], 'YES')

    ds = ogr.Open(filename)
    self.assertIsNotNone(ds)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None
    self.assertEqual(lyr.GetName(), 'foo')
    ds = None

  @parameterized.parameters(True, False)
  def test_ogr_gpkg_add_non_spatial_layer_in_existing_database_with_unregistered(
      self, with_gpkg_ogr_contents: bool
  ) -> None:
    filename = str(
        self.vsimem_dir
        / 'ogr_gpkg_add_non_spatial_layer_in_existing_database_with_unregistered.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    ds.CreateLayer('point', geom_type=ogr.wkbPoint)
    ds.ExecuteSQL(
        'CREATE TABLE non_spatial(fid INTEGER PRIMARY KEY AUTOINCREMENT,'
        ' str TEXT)'
    )
    ds = None

    if not with_gpkg_ogr_contents:
      ds = ogr.Open(filename, update=1)
      assert ds is not None
      ds.ExecuteSQL('DROP TABLE gpkg_ogr_contents')
      ds = None

    ds = ogr.Open(filename, update=1)
    assert ds is not None
    self.assertEqual(ds.GetLayerCount(), 2)
    self.assertEqual(
        {ds.GetLayer(i).GetName() for i in range(ds.GetLayerCount())},
        {'point', 'non_spatial'},
    )
    sql_lyr = ds.ExecuteSQL('SELECT * FROM gpkg_contents')
    assert sql_lyr is not None
    self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    ds.ReleaseResultSet(sql_lyr)

    self.assertIsNotNone(ds.CreateLayer('non_spatial2', geom_type=ogr.wkbNone))
    ds = None

    self.validate(filename)

    ds = ogr.Open(filename)
    assert ds is not None
    self.assertEqual(ds.GetLayerCount(), 3)
    self.assertEqual(
        {ds.GetLayer(i).GetName() for i in range(ds.GetLayerCount())},
        {'point', 'non_spatial', 'non_spatial2'},
    )
    sql_lyr = ds.ExecuteSQL('SELECT * FROM gpkg_contents')
    assert sql_lyr is not None
    self.assertEqual(sql_lyr.GetFeatureCount(), 1)
    ds.ReleaseResultSet(sql_lyr)
    if with_gpkg_ogr_contents:
      sql_lyr = ds.ExecuteSQL('SELECT * FROM gpkg_ogr_contents')
      assert sql_lyr is not None
      self.assertEqual(sql_lyr.GetFeatureCount(), 2)
      ds.ReleaseResultSet(sql_lyr)
    ds = None

  def test_ogr_gpkg_update_feature(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_update_feature.gpkg.zip')

    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test')
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('int_field', ogr.OFTInteger))
    lyr.CreateField(ogr.FieldDefn('str_field', ogr.OFTString))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
    f['int_field'] = 1
    f['str_field'] = 'foo'
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(lyr.TestCapability(ogr.OLCUpdateFeature), 1)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFID(1)
    f['int_field'] = 123  # will be ignored
    f['str_field'] = 'bar'
    self.assertEqual(lyr.UpdateFeature(f, [1], [], False), ogr.OGRERR_NONE)
    # Check recycling of existing statement
    f['str_field'] = 'baz'
    self.assertEqual(lyr.UpdateFeature(f, [1], [], False), ogr.OGRERR_NONE)
    f = lyr.GetFeature(1)
    assert f is not None
    geom = f.GetGeometryRef()
    assert geom is not None
    self.assertEqual(geom.ExportToWkt(), 'POINT (1 2)')
    self.assertEqual(f['int_field'], 1)
    self.assertEqual(f['str_field'], 'baz')

    # Do not modify unset fields
    f.UnsetField(1)
    self.assertEqual(lyr.UpdateFeature(f, [1], [], False), ogr.OGRERR_NONE)
    f = lyr.GetFeature(1)
    assert f is not None
    self.assertEqual(f['str_field'], 'baz')

    # Nullify geometry
    f.SetGeometry(None)
    self.assertEqual(lyr.UpdateFeature(f, [], [0], False), ogr.OGRERR_NONE)
    f = lyr.GetFeature(1)
    assert f is not None
    self.assertIsNone(f.GetGeometryRef())

    # Set non null geometry
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
    self.assertEqual(lyr.UpdateFeature(f, [], [0], False), ogr.OGRERR_NONE)
    geom = f.GetGeometryRef()
    assert geom is not None
    self.assertEqual(geom.ExportToWkt(), 'POINT (1 2)')

    ds = None

  def test_ogr_gpkg_ogr_layer_Extent(self) -> None:
    tmpfilename = str(self.vsimem_dir / 'test_ogr_gpkg_ogr_layer_Extent.gpkg')

    ds = self.driver.CreateDataSource(tmpfilename)
    assert ds is not None
    lyr = ds.CreateLayer('my_layer', geom_type=ogr.wkbLineString)
    assert lyr is not None
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('LINESTRING (0 1,2 3)'))
    lyr.CreateFeature(feat)
    feat = None

    # Test with invalid parameter
    with _quiet_error_handler():
      sql_lyr = ds.ExecuteSQL('SELECT ogr_layer_Extent(12)')
    assert sql_lyr is not None
    feat = sql_lyr.GetNextFeature()
    assert feat is not None
    geom = feat.GetGeometryRef()
    ds.ReleaseResultSet(sql_lyr)

    self.assertIsNone(geom)

    # Test on non existing layer
    with _quiet_error_handler():
      sql_lyr = ds.ExecuteSQL("SELECT ogr_layer_Extent('foo')")
    assert sql_lyr is not None
    feat = sql_lyr.GetNextFeature()
    assert feat is not None
    geom = feat.GetGeometryRef()
    ds.ReleaseResultSet(sql_lyr)

    self.assertIsNone(geom)

    # Test ogr_layer_Extent()
    sql_lyr = ds.ExecuteSQL("SELECT ogr_layer_Extent('my_layer')")
    assert sql_lyr is not None
    feat = sql_lyr.GetNextFeature()
    assert feat is not None
    geom = feat.GetGeometryRef()
    assert geom is not None
    geom_wkt = geom.ExportToWkt()
    feat = None
    ds.ReleaseResultSet(sql_lyr)

    self.assertEqual(geom_wkt, 'POLYGON ((0 1,2 1,2 3,0 3,0 1))')
    ds = None

  def test_ogr_gpkg_field_alternative_names_comment(self) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_alternative_names.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPolygon)
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('baz', ogr.OFTString))

    # with no gpkg_data_columns table
    lyr = ds.GetLayer('test')
    assert lyr is not None
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'foo')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), ''
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), '')
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'baz')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetAlternativeName(), ''
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetComment(), '')

    ds.ExecuteSQL("""CREATE TABLE gpkg_data_columns (
  table_name TEXT NOT NULL,
  column_name TEXT NOT NULL,
  name TEXT,
  title TEXT,
  description TEXT,
  mime_type TEXT,
  constraint_name TEXT,
  CONSTRAINT pk_gdc PRIMARY KEY (table_name, column_name),
  CONSTRAINT gdc_tn UNIQUE (table_name, name)
)""")
    # name same as column name, won't be used as alternative name
    ds.ExecuteSQL(
        "INSERT INTO gpkg_data_columns('table_name', 'column_name', 'name',"
        " 'description') VALUES ('test', 'foo', 'foo', 'my description')"
    )
    ds = None

    ds = gdal.OpenEx(dbname, gdal.OF_VECTOR | gdal.OF_UPDATE)
    assert ds is not None
    lyr = ds.GetLayer('test')
    assert lyr is not None
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'foo')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), ''
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), 'my description'
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'baz')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetAlternativeName(), ''
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetComment(), '')

    # name different from column name, should be used as alternative names
    ds.ExecuteSQL('DELETE FROM gpkg_data_columns')
    ds.ExecuteSQL(
        "INSERT INTO gpkg_data_columns('table_name', 'column_name', 'name')"
        " VALUES ('test', 'foo', 'Foo field')"
    )
    ds = None

    ds = self.driver.Open(dbname)
    assert ds is not None
    lyr = ds.GetLayer('test')
    assert lyr is not None
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'foo')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), 'Foo field'
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'baz')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetAlternativeName(), ''
    )
    ds = None

  def test_ogr_gpkg_field_alter_field_defn_alternative_names_comment(
      self,
  ) -> None:
    dbname = str(self.vsimem_dir / 'ogr_gpkg_alternative_names_alter_defn.gpkg')

    ds = self.driver.CreateDataSource(dbname)
    assert ds is not None
    lyr = ds.CreateLayer('test', geom_type=ogr.wkbPolygon)
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('baz', ogr.OFTString))

    # with no gpkg_data_columns table
    lyr = ds.GetLayer('test')
    assert lyr is not None
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'foo')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), ''
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), '')
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'baz')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetAlternativeName(), ''
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetComment(), '')

    foo_with_alternative_name = ogr.FieldDefn('foo')
    foo_with_alternative_name.SetAlternativeName('alt foo name')

    ret = lyr.AlterFieldDefn(0, foo_with_alternative_name, ogr.ALTER_ALL_FLAG)
    self.assertEqual(ret, 0)

    baz_with_comment = ogr.FieldDefn('baz')
    baz_with_comment.SetComment('baz comment')

    ret = lyr.AlterFieldDefn(1, baz_with_comment, ogr.ALTER_ALL_FLAG)
    self.assertEqual(ret, 0)

    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'foo')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), 'alt foo name'
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), '')
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'baz')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetAlternativeName(), ''
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetComment(), 'baz comment'
    )

    del lyr
    ds = None

    ds = gdal.OpenEx(dbname, gdal.OF_VECTOR | gdal.OF_UPDATE)
    assert ds is not None
    lyr = ds.GetLayer('test')
    assert lyr is not None
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'foo')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), 'alt foo name'
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), '')
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'baz')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetAlternativeName(), ''
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetComment(), 'baz comment'
    )

    # create field
    field_defn = ogr.FieldDefn('third', ogr.OFTString)
    field_defn.SetAlternativeName('third alias')
    field_defn.SetComment('third comment')
    self.assertEqual(lyr.CreateField(field_defn), 0)

    del lyr
    ds = None

    ds = gdal.OpenEx(dbname, gdal.OF_VECTOR)
    assert ds is not None
    lyr = ds.GetLayer('test')
    assert lyr is not None

    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'foo')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetAlternativeName(), 'alt foo name'
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetComment(), '')
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'baz')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetAlternativeName(), ''
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetComment(), 'baz comment'
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(2).GetName(), 'third')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(2).GetAlternativeName(), 'third alias'
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(2).GetComment(), 'third comment'
    )
    ds = None

  @parameterized.parameters('1.2', '1.4')
  def test_ogr_gpkg_rtree_triggers(self, gpkg_version: str) -> None:
    def get_rtree_entry_count(ds: gdal.Dataset) -> int:
      with ds.ExecuteSQL('SELECT * FROM rtree_test_geom') as sql_lyr:
        return sql_lyr.GetFeatureCount()

    dbname = str(self.vsimem_dir / 'test_ogr_gpkg_rtree_triggers.gpkg')

    ds = self.driver.CreateDataSource(
        dbname, options=['VERSION=' + gpkg_version]
    )
    assert ds is not None
    ds.CreateLayer('test', geom_type=ogr.wkbPoint)
    ds = None

    ds = ogr.Open(dbname, update=1)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None

    # Create a feature without geometry
    f = ogr.Feature(lyr.GetLayerDefn())
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    # Update the feature with a geometry
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 1)
    lyr.SetSpatialFilterRect(1, 2, 1, 2)
    self.assertEqual(lyr.GetFeatureCount(), 1)

    # Update the feature with another geometry
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (3 4)'))
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 1)
    lyr.SetSpatialFilterRect(3, 4, 3, 4)
    self.assertEqual(lyr.GetFeatureCount(), 1)

    # Upsert the feature with another geometry
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (5 6)'))
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 1)
    lyr.SetSpatialFilterRect(5, 6, 5, 6)
    self.assertEqual(lyr.GetFeatureCount(), 1)

    # Upsert the feature without geometry
    f.SetGeometry(None)
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    # Upsert the feature with another geometry
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (7 8)'))
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 1)
    lyr.SetSpatialFilterRect(7, 8, 7, 8)
    self.assertEqual(lyr.GetFeatureCount(), 1)

    # Upsert the feature with empty geometry
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT EMPTY'))
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    # Upsert the feature with a geometry
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (7 8)'))
    self.assertEqual(lyr.UpsertFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 1)
    lyr.SetSpatialFilterRect(7, 8, 7, 8)
    self.assertEqual(lyr.GetFeatureCount(), 1)

    # Remove the geometry
    f.SetGeometry(None)
    self.assertEqual(lyr.SetFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    # Delete the geometry
    self.assertEqual(lyr.DeleteFeature(f.GetFID()), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    # Create a feature with a geometry
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (9 10)'))
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 1)
    lyr.SetSpatialFilterRect(9, 10, 9, 10)
    self.assertEqual(lyr.GetFeatureCount(), 1)

    # Delete the geometry
    self.assertEqual(lyr.DeleteFeature(f.GetFID()), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    # Create a feature with an empty geometry
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT EMPTY'))
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    # Delete the geometry
    self.assertEqual(lyr.DeleteFeature(f.GetFID()), ogr.OGRERR_NONE)
    self.assertEqual(get_rtree_entry_count(ds), 0)

    ds = None

  def test_ogr_gpkg_1_4_relaxed_datetime_format(self) -> None:
    dbname = str(
        self.vsimem_dir / 'test_ogr_gpkg_1_4_relaxed_datetime_format.gpkg'
    )
    ds = self.driver.CreateDataSource(dbname, options=['VERSION=1.4'])
    assert ds is not None
    lyr = ds.CreateLayer('test')
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('dt', ogr.OFTDateTime))
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('dt', '2023-11-07T16:03:34Z')
    lyr.CreateFeature(f)
    f = None

    # Check we have written without milliseconds
    with ds.ExecuteSQL('SELECT CAST(dt AS VARCHAR) AS dt FROM test') as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertEqual(f['dt'], '2023-11-07T16:03:34Z')

    # Test datetime without seconds
    ds.ExecuteSQL("INSERT INTO test (dt) VALUES ('2023-11-07T16:03Z')")
    ds = None

    self.validate(dbname)

    ds = ogr.Open(dbname)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None
    f = lyr.GetNextFeature()
    assert f is not None
    self.assertEqual(f['dt'], '2023/11/07 16:03:34+00')
    f = lyr.GetNextFeature()
    assert f is not None
    self.assertEqual(f['dt'], '2023/11/07 16:03:00+00')
    ds = None

  @parameterized.parameters(
      ('1.4', 'AUTO', '2023-11-07T16:03:34.123Z', '2023-11-07T16:03:34.123Z'),
      ('1.4', 'AUTO', '2023-11-07T16:03:34Z', '2023-11-07T16:03:34Z'),
      ('1.3', 'AUTO', '2023-11-07T16:03:34.123Z', '2023-11-07T16:03:34.123Z'),
      ('1.3', 'AUTO', '2023-11-07T16:03:34Z', '2023-11-07T16:03:34.000Z'),
      (
          '1.4',
          'MILLISECOND',
          '2023-11-07T16:03:34.123Z',
          '2023-11-07T16:03:34.123Z',
      ),
      (
          '1.4',
          'MILLISECOND',
          '2023-11-07T16:03:34Z',
          '2023-11-07T16:03:34.000Z',
      ),
      ('1.4', 'SECOND', '2023-11-07T16:03:34.123Z', '2023-11-07T16:03:34Z'),
      ('1.4', 'MINUTE', '2023-11-07T16:03:34.123Z', '2023-11-07T16:03Z'),
      ('1.4', 'INVALID', None, None),
  )
  def test_ogr_gpkg_1_4_DATETIME_PRECISION(
      self,
      version: str,
      datetime_precision: str,
      dt_input: str | None,
      dt_output: str | None,
  ) -> None:
    dbname = str(self.vsimem_dir / 'test_ogr_gpkg_1_4_DATETIME_PRECISION.gpkg')
    with _enable_exceptions():
      ds = self.driver.CreateDataSource(dbname, options=['VERSION=' + version])
      assert ds is not None
      if datetime_precision == 'INVALID':
        with self.assertRaises(Exception), _quiet_error_handler():
          ds.CreateLayer(
              'test', options=['DATETIME_PRECISION=' + datetime_precision]
          )
      else:
        lyr = ds.CreateLayer(
            'test', options=['DATETIME_PRECISION=' + datetime_precision]
        )
        assert lyr is not None
        lyr.CreateField(ogr.FieldDefn('dt', ogr.OFTDateTime))
        f = ogr.Feature(lyr.GetLayerDefn())
        f.SetField('dt', dt_input)
        lyr.CreateFeature(f)
        f = None

        # Check we have written what we expected
        with ds.ExecuteSQL(
            'SELECT CAST(dt AS VARCHAR) AS dt FROM test'
        ) as sql_lyr:
          f = sql_lyr.GetNextFeature()
          assert f is not None
          self.assertEqual(f['dt'], dt_output)

      ds = None

  def test_ogr_gpkg_write_flushcache(self) -> None:
    filename = str(self.vsimem_dir / 'test_ogr_gpkg_write_flushcache.gpkg')

    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr1 = ds.CreateLayer('test1')
    assert lyr1 is not None
    lyr2 = ds.CreateLayer('test2')
    assert lyr2 is not None
    f = ogr.Feature(lyr1.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
    lyr1.CreateFeature(f)
    f = ogr.Feature(lyr2.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (3 4)'))
    lyr2.CreateFeature(f)
    ds.FlushCache()

    ds2 = ogr.Open(filename)
    assert ds2 is not None
    ds2_lyr0 = ds2.GetLayer(0)
    assert ds2_lyr0 is not None
    self.assertEqual(ds2_lyr0.GetFeatureCount(), 1)
    ds2_lyr1 = ds2.GetLayer(1)
    assert ds2_lyr1 is not None
    self.assertEqual(ds2_lyr1.GetFeatureCount(), 1)
    ds2 = None

    f = ogr.Feature(lyr1.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (3 4)'))
    lyr1.CreateFeature(f)
    ds = None

    ds2 = ogr.Open(filename)
    assert ds2 is not None
    ds2_lyr0 = ds2.GetLayer(0)
    assert ds2_lyr0 is not None
    self.assertEqual(ds2_lyr0.GetFeatureCount(), 2)
    ds2_lyr1 = ds2.GetLayer(1)
    assert ds2_lyr1 is not None
    self.assertEqual(ds2_lyr1.GetFeatureCount(), 1)
    ds2 = None

  def test_ogr_gpkg_write_arrow_fallback_types(self) -> None:
    with _enable_exceptions():
      src_ds = ogr.GetDriverByName('MEM').CreateDataSource('')
      assert src_ds is not None
      src_lyr = src_ds.CreateLayer('test')
      assert src_lyr is not None
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
      f.SetFID(10)
      f.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
      src_lyr.CreateFeature(f)

      filename = str(
          self.vsimem_dir / 'test_ogr_gpkg_write_arrow_fallback_types.gpkg'
      )
      ds = self.driver.CreateDataSource(filename)
      assert ds is not None
      lyr = ds.CreateLayer('test')
      assert lyr is not None

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
      assert f is not None
      self.assertEqual(f.GetFID(), 10)
      self.assertEqual(f['string'], 'foo')
      self.assertEqual(f['int'], 123)
      self.assertEqual(f['int64'], 12345678901234)
      self.assertEqual(f['real'], 1.5)
      self.assertEqual(f['date'], '2023/10/06')
      self.assertEqual(f['time'], '12:34:56')
      self.assertEqual(f['binary'], '0123465789ABCDEF')
      self.assertEqual(f['datetime'], '2023/10/06 19:43:00')
      self.assertEqual(f['stringlist'], '[ "foo", "bar" ]')
      self.assertEqual(f['intlist'], '[ 1, 2 ]')
      self.assertEqual(f['int64list'], '[ 12345678901234, 2 ]')
      self.assertEqual(f['reallist'], '[ 1.5, 2.5 ]')
      geom = f.GetGeometryRef()
      assert geom is not None
      self.assertEqual(geom.ExportToIsoWkt(), 'POINT (1 2)')
      ds = None
      src_ds = None

  def test_ogr_gpkg_sql_first_geom_null(self) -> None:
    ds = ogr.Open(self.getTestFilePath('first_geometry_null.gpkg'))
    assert ds is not None
    if not _has_spatialite_4_3_or_later(ds):
      ds = None
      self.skipTest('spatialite missing')
    with ds.ExecuteSQL('SELECT ST_Buffer(geom,0.1) FROM test') as sql_lyr:
      self.assertEqual(sql_lyr.GetGeometryColumn(), 'ST_Buffer(geom,0.1)')
    with ds.ExecuteSQL('SELECT ST_Buffer(geom,0.1), * FROM test') as sql_lyr:
      self.assertEqual(sql_lyr.GetGeometryColumn(), 'ST_Buffer(geom,0.1)')
    with ds.ExecuteSQL(
        'SELECT ST_Buffer(geom,0.5) AS geom FROM test'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetGeometryColumn(), 'geom')
      sql_lyr.GetNextFeature()
      f = sql_lyr.GetNextFeature()
      assert f is not None
      geom = f.GetGeometryRef()
      assert geom is not None
      self.assertEqual(geom.GetGeometryType(), ogr.wkbPolygon)
    with ds.ExecuteSQL(
        'SELECT ST_Buffer(geom,0.5) AS geom, * FROM test'
    ) as sql_lyr:
      self.assertEqual(sql_lyr.GetGeometryColumn(), 'geom')
      sql_lyr.GetNextFeature()
      f = sql_lyr.GetNextFeature()
      assert f is not None
      geom = f.GetGeometryRef()
      assert geom is not None
      self.assertEqual(geom.GetGeometryType(), ogr.wkbPolygon)
    ds = None

  def test_ogr_gpkg_sql_exact_spatial_filter_for_feature_count(self) -> None:
    if not _have_geos():
      self.skipTest('GEOS missing')

    filename = str(
        self.vsimem_dir
        / 'test_ogr_gpkg_sql_exact_spatial_filter_for_feature_count.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test')
    assert lyr is not None
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (0 0,1 1)'))
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (0.1 0.25,0.15 0.25)'))
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING (0.12 0.25,1 1)'))
    lyr.CreateFeature(f)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt('LINESTRING EMPTY'))
    lyr.CreateFeature(f)
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr is not None
    lyr.SetSpatialFilterRect(0.1, 0.2, 0.15, 0.3)
    self.assertEqual(lyr.GetFeatureCount(), 2)
    ds = None

  @parameterized.parameters('huge_string', 'huge_binary', 'geometry')
  def test_ogr_gpkg_arrow_stream_huge_array(self, too_big_field: str) -> None:
    if (
        not hasattr(gdal_array, '_RecordBatchAsNumpy')
        or importlib.util.find_spec('numpy') is None
    ):
      self.skipTest('gdal_array or numpy missing')

    filename = str(
        self.vsimem_dir / 'test_ogr_gpkg_arrow_stream_huge_array.gpkg'
    )
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('foo')
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('huge_string', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('huge_binary', ogr.OFTBinary))
    for i in range(50):
      f = ogr.Feature(lyr.GetLayerDefn())
      if too_big_field == 'huge_string':
        if i > 10:
          f['huge_string'] = 'x' * 10000
      elif too_big_field == 'huge_binary':
        if i > 10:
          f['huge_binary'] = b'x' * 10000
      else:
        geom = ogr.Geometry(ogr.wkbLineString)
        geom.SetPoint_2D(500, 0, 0)
        f.SetGeometry(geom)
      lyr.CreateFeature(f)
    ds.ExecuteSQL(
        'CREATE VIEW my_view AS SELECT fid AS my_fid, geom AS my_geom,'
        ' huge_string, huge_binary FROM foo'
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_contents (table_name, identifier, data_type, srs_id)'
        " VALUES ( 'my_view', 'my_view', 'features', 0 )"
    )
    ds.ExecuteSQL(
        'INSERT INTO gpkg_geometry_columns (table_name, column_name,'
        " geometry_type_name, srs_id, z, m) values ('my_view', 'my_geom',"
        " 'GEOMETRY', 0, 0, 0)"
    )
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    expected_fids = [i + 1 for i in range(50)]
    for lyr_name in ['foo', 'my_view']:
      lyr = ds.GetLayer(lyr_name)
      assert lyr is not None

      with gdal.config_option(
          'OGR_ARROW_MEM_LIMIT', '20000', thread_local=False
      ):
        stream = lyr.GetArrowStreamAsNumPy([
            'INCLUDE_FID=YES',
            'MAX_FEATURES_IN_BATCH=10',
            'USE_MASKED_ARRAYS=NO',
        ])
        batch_count = 0
        got_fids = []
        for batch in stream:
          batch_count += 1
          for fid in batch[lyr.GetFIDColumn()]:
            got_fids.append(fid)
        self.assertEqual(got_fids, expected_fids)
        self.assertEqual(
            batch_count, 25 if too_big_field == 'geometry' else 21, lyr_name
        )
        del stream

    with ds.ExecuteSQL('SELECT * FROM foo') as sql_lyr:
      with gdal.config_option(
          'OGR_ARROW_MEM_LIMIT', '20000', thread_local=False
      ):
        stream = sql_lyr.GetArrowStreamAsNumPy([
            'INCLUDE_FID=YES',
            'MAX_FEATURES_IN_BATCH=10',
            'USE_MASKED_ARRAYS=NO',
        ])
        batch_count = 0
        got_fids = []
        for batch in stream:
          batch_count += 1
          for fid in batch[sql_lyr.GetFIDColumn()]:
            got_fids.append(fid)
        self.assertEqual(got_fids, expected_fids)
        self.assertEqual(batch_count, 25 if too_big_field == 'geometry' else 21)
        del stream

    ds = None

  def test_ogr_gpkg_like_utf8(self) -> None:
    with _enable_exceptions():
      filename = str(self.vsimem_dir / 'test_ogr_gpkg_like_utf8.gpkg')
      ds = self.driver.CreateDataSource(filename)
      assert ds is not None
      lyr = ds.CreateLayer('test')
      assert lyr is not None
      lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))

      with ds.ExecuteSQL("SELECT * FROM test WHERE 'e' LIKE 'E'") as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)

      with ds.ExecuteSQL("SELECT * FROM test WHERE 'e' LIKE 'i'") as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 0)

      with ds.ExecuteSQL("SELECT * FROM test WHERE 'é' LIKE 'É'") as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)

      with ds.ExecuteSQL(
          "SELECT * FROM test WHERE 'éx' LIKE 'Éxx' ESCAPE 'x'"
      ) as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)

      with ds.ExecuteSQL("SELECT * FROM test WHERE NULL LIKE 'É'") as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 0)

      with ds.ExecuteSQL("SELECT * FROM test WHERE 'é' LIKE NULL") as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 0)

      with ds.ExecuteSQL(
          "SELECT * FROM test WHERE 'é' LIKE 'É' ESCAPE NULL"
      ) as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 0)

      with ds.ExecuteSQL(
          "SELECT * FROM test WHERE 'é' LIKE 'É' ESCAPE 'should be single char'"
      ) as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 0)

      self.assertIsNone(ds.ExecuteSQL('PRAGMA case_sensitive_like = 1'))

      with ds.ExecuteSQL("SELECT * FROM test WHERE 'e' LIKE 'E'") as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 0)

      self.assertIsNone(ds.ExecuteSQL('PRAGMA case_sensitive_like = 0'))

      with ds.ExecuteSQL("SELECT * FROM test WHERE 'e' LIKE 'E'") as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)

      ds = None

  @parameterized.parameters(
      (
          'POINT(0 1 2)',
          'POINT(1 2 3)',
          (0.0, 1.0, 1.0, 2.0),
          (0.0, 1.0, 1.0, 2.0, 2.0, 3.0),
      ),
      (
          'POINT(0 1)',
          'POINT(1 2 3)',
          (0.0, 1.0, 1.0, 2.0),
          (0.0, 1.0, 1.0, 2.0, 3.0, 3.0),
      ),
      (
          'POINT(0 1)',
          'POINT(1 2)',
          (0.0, 1.0, 1.0, 2.0),
          (0.0, 1.0, 1.0, 2.0, float('inf'), float('-inf')),
      ),
  )
  def test_ogr_gpkg_extent3d(
      self,
      geom1: str,
      geom2: str,
      extent2d: tuple[float, ...],
      extent3d: tuple[float, ...],
  ) -> None:
    """Tests 3D extent of a gpkg."""
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('foo', geom_type=ogr.wkbPoint25D)
    assert lyr is not None
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(geom1))
    lyr.CreateFeature(feat)
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(geom2))
    lyr.CreateFeature(feat)
    feat = None
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    lyr = ds.GetLayerByName('foo')
    assert lyr is not None
    ext3d = lyr.GetExtent3D()
    self.assertEqual(ext3d, extent3d)
    ext2d = lyr.GetExtent()
    self.assertEqual(ext2d, extent2d)
    ds = None

  @parameterized.parameters('no_envelope', '2d_envelope', '3d_envelope')
  def test_ogr_gpkg_extent3d_envelope_variants(self, file_name: str) -> None:
    """Tests all variants of envelope in gpkg."""
    ds = gdal.OpenEx(
        self.getTestFilePath(file_name + '.gpkg'),
        gdal.OF_VECTOR | gdal.OF_READONLY,
    )
    assert ds is not None
    lyr = ds.GetLayerByName('foo')
    assert lyr is not None
    ext3d = lyr.GetExtent3D()
    self.assertEqual(ext3d, (0.0, 3.0, 0.0, 3.0, 0.0, 3.0))

    lyr.SetAttributeFilter('1 = 1')
    ext3d = lyr.GetExtent3D()
    self.assertEqual(ext3d, (0.0, 3.0, 0.0, 3.0, 0.0, 3.0))

    lyr.SetAttributeFilter('1 = 0')
    ext3d = lyr.GetExtent3D()
    self.assertEqual(
        ext3d,
        (
            float('inf'),
            float('-inf'),
            float('inf'),
            float('-inf'),
            float('inf'),
            float('-inf'),
        ),
    )
    ds = None

  def test_ogr_gpkg_extent3d_on_2d_dataset_with_filters(self) -> None:
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('foo', geom_type=ogr.wkbPoint)
    assert lyr is not None
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 2)'))
    lyr.CreateFeature(feat)
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(3 4)'))
    lyr.CreateFeature(feat)
    feat = None
    ds = None

    ds = ogr.Open(filename)
    assert ds is not None
    lyr = ds.GetLayerByName('foo')
    assert lyr is not None
    ext3d = lyr.GetExtent3D()
    self.assertEqual(ext3d, (1, 3, 2, 4, float('inf'), float('-inf')))

    lyr.SetAttributeFilter('fid = 1')
    ext3d = lyr.GetExtent3D()
    lyr.SetAttributeFilter(None)
    self.assertEqual(ext3d, (1, 1, 2, 2, float('inf'), float('-inf')))

    lyr.SetSpatialFilterRect(2.5, 3.5, 3.5, 4.5)
    ext3d = lyr.GetExtent3D()
    lyr.SetSpatialFilter(None)
    self.assertEqual(ext3d, (3, 3, 4, 4, float('inf'), float('-inf')))

    ds = None

  def test_ogr_gpkg_creation_with_foreign_key_constraint_enabled(self) -> None:
    with (
        _enable_exceptions(),
        gdal.config_option('OGR_SQLITE_PRAGMA', 'FOREIGN_KEYS=1'),
    ):
      out_filename = str(self.vsimem_dir / 'out.gpkg')
      gdal.VectorTranslate(
          out_filename, ogr_util.GetTestFilePath('shape/poly/poly.shp')
      )

  @parameterized.parameters(
      (True, 'YES', 'YES'),
      (False, 'YES', 'NO'),
      (False, 'NO', 'NO'),
  )
  def test_ogr_gpkg_geom_coord_precision(
      self,
      with_metadata: bool,
      discard_coord_lsb: str,
      undo_discard_coord_lsb_on_reading: str,
  ) -> None:
    with _enable_exceptions():
      filename = str(self.vsimem_dir / 'test.gpkg')
      ds = self.driver.CreateDataSource(filename)
      assert ds is not None
      geom_fld = ogr.GeomFieldDefn('my_geom', ogr.wkbUnknown)
      prec = ogr.CreateGeomCoordinatePrecision()
      assert prec is not None
      prec.Set(1e-5, 1e-3, 1e-2)
      geom_fld.SetCoordinatePrecision(prec)
      lyr = ds.CreateLayerFromGeomFieldDefn(
          'test',
          geom_fld,
          [
              'DISCARD_COORD_LSB=' + discard_coord_lsb,
              'UNDO_DISCARD_COORD_LSB_ON_READING='
              + undo_discard_coord_lsb_on_reading,
          ],
      )
      assert lyr is not None
      geom_fld = lyr.GetLayerDefn().GetGeomFieldDefn(0)
      prec = geom_fld.GetCoordinatePrecision()
      assert prec is not None
      self.assertEqual(prec.GetXYResolution(), 1e-5)
      self.assertEqual(prec.GetZResolution(), 1e-3)
      self.assertEqual(prec.GetMResolution(), 1e-2)
      f = ogr.Feature(lyr.GetLayerDefn())
      f.SetGeometry(
          ogr.CreateGeometryFromWkt(
              'POINT ZM (1.23456789 2.34567891 9.87654321 -1.23456789)'
          )
      )
      lyr.CreateFeature(f)
      if with_metadata:
        lyr.SetMetadataItem('FOO', 'BAR')
      ds.Close()

      ds = ogr.Open(filename, update=1)
      assert ds is not None
      lyr = ds.GetLayer(0)
      assert lyr is not None
      geom_fld = lyr.GetLayerDefn().GetGeomFieldDefn(0)
      prec = geom_fld.GetCoordinatePrecision()
      assert prec is not None
      self.assertEqual(prec.GetXYResolution(), 1e-5)
      self.assertEqual(prec.GetZResolution(), 1e-3)
      self.assertEqual(prec.GetMResolution(), 1e-2)
      self.assertEqual(ds.GetMetadata(), {})
      self.assertEqual(
          lyr.GetMetadata(), {'FOO': 'BAR'} if with_metadata else {}
      )
      f = lyr.GetNextFeature()
      assert f is not None

      g = f.GetGeometryRef()
      assert g is not None
      self.assertAlmostEqual(g.GetX(0), 1.23456789, delta=1e-5)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetX(0), 1.23456789, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetX(0), 1.23457, delta=1e-8)
      else:
        self.assertNotAlmostEqual(g.GetX(0), 1.23457, delta=1e-8)

      self.assertAlmostEqual(g.GetY(0), 2.34567891, delta=1e-5)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetY(0), 2.34567891, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetY(0), 2.34568, delta=1e-8)

      self.assertAlmostEqual(g.GetZ(0), 9.87654321, delta=1e-3)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetZ(0), 9.87654321, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetZ(0), 9.876, delta=1e-8)

      self.assertAlmostEqual(g.GetM(0), -1.23456789, delta=1e-2)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetM(0), -1.23456789, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetM(0), -1.23, delta=1e-8)

      # Test Arrow interface
      lyr.ResetReading()
      mem_ds = ogr.GetDriverByName('MEM').CreateDataSource('')
      assert mem_ds is not None
      mem_lyr = mem_ds.CreateLayer('test', geom_type=ogr.wkbNone)
      assert mem_lyr is not None
      mem_lyr.CreateGeomField(ogr.GeomFieldDefn('my_geom'))
      mem_lyr.WriteArrow(lyr)
      f = mem_lyr.GetNextFeature()
      assert f is not None

      g = f.GetGeometryRef()
      assert g is not None
      self.assertAlmostEqual(g.GetX(0), 1.23456789, delta=1e-5)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetX(0), 1.23456789, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetX(0), 1.23457, delta=1e-8)
      else:
        self.assertNotAlmostEqual(g.GetX(0), 1.23457, delta=1e-8)

      self.assertAlmostEqual(g.GetY(0), 2.34567891, delta=1e-5)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetY(0), 2.34567891, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetY(0), 2.34568, delta=1e-8)

      self.assertAlmostEqual(g.GetZ(0), 9.87654321, delta=1e-3)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetZ(0), 9.87654321, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetZ(0), 9.876, delta=1e-8)

      self.assertAlmostEqual(g.GetM(0), -1.23456789, delta=1e-2)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetM(0), -1.23456789, delta=1e-8)
      if undo_discard_coord_lsb_on_reading == 'YES':
        self.assertAlmostEqual(g.GetM(0), -1.23, delta=1e-8)

      mem_ds = None

      lyr.ResetReading()
      f = lyr.GetNextFeature()
      assert f is not None
      # Update geometry to check existing precision settings are used
      f.SetGeometry(
          ogr.CreateGeometryFromWkt(
              'POINT ZM (-1.23456789 -2.34567891 -9.87654321 1.23456789)'
          )
      )
      lyr.SetFeature(f)

      lyr.SetMetadataItem('FOO', 'BAZ')

      new_geom_field_defn = ogr.GeomFieldDefn('new_geom_name', ogr.wkbNone)
      self.assertEqual(
          lyr.AlterGeomFieldDefn(
              0, new_geom_field_defn, ogr.ALTER_GEOM_FIELD_DEFN_NAME_FLAG
          ),
          ogr.OGRERR_NONE,
      )

      self.assertEqual(lyr.Rename('test_renamed'), ogr.OGRERR_NONE)

      ds.Close()

      ds = ogr.Open(filename, update=1)
      assert ds is not None
      lyr = ds.GetLayer(0)
      assert lyr is not None
      geom_fld = lyr.GetLayerDefn().GetGeomFieldDefn(0)
      prec = geom_fld.GetCoordinatePrecision()
      assert prec is not None
      self.assertEqual(prec.GetXYResolution(), 1e-5)
      self.assertEqual(prec.GetZResolution(), 1e-3)
      self.assertEqual(prec.GetMResolution(), 1e-2)
      self.assertEqual(lyr.GetMetadata(), {'FOO': 'BAZ'})
      f = lyr.GetNextFeature()
      assert f is not None
      g = f.GetGeometryRef()
      assert g is not None
      self.assertAlmostEqual(g.GetX(0), -1.23456789, delta=1e-5)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetX(0), -1.23456789, delta=1e-8)
      self.assertAlmostEqual(g.GetY(0), -2.34567891, delta=1e-5)
      self.assertAlmostEqual(g.GetZ(0), -9.87654321, delta=1e-3)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetZ(0), -9.87654321, delta=1e-8)
      self.assertAlmostEqual(g.GetM(0), 1.23456789, delta=1e-2)
      if discard_coord_lsb == 'YES':
        self.assertNotAlmostEqual(g.GetM(0), 1.23456789, delta=1e-8)

      ds.DeleteLayer(0)

      with ds.ExecuteSQL('SELECT * FROM gpkg_metadata') as sql_lyr:
        self.assertEqual(sql_lyr.GetFeatureCount(), 0)

      ds.Close()

  def test_ogr_gpkg_ST_Area_on_ellipsoid(self) -> None:
    tmpfilename = str(
        self.vsimem_dir / 'test_ogr_sql_ST_Area_on_ellipsoid.gpkg'
    )

    ds = self.driver.CreateDataSource(tmpfilename)
    assert ds is not None
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4258)
    lyr = ds.CreateLayer('my_layer', srs=srs)
    assert lyr is not None
    geom_colname = lyr.GetGeometryColumn()
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON((2 49,3 49,3 48,2 49))')
    )
    lyr.CreateFeature(feat)
    feat = None

    with ds.ExecuteSQL(
        f'SELECT ST_Area({geom_colname}, 1) FROM my_layer'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertAlmostEqual(
          f[0], 4068384291.8911743, delta=4068384291.8911743 * 1e-6
      )

    with _quiet_error_handler():
      with ds.ExecuteSQL(
          f'SELECT ST_Area({geom_colname}, 0) FROM my_layer'
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertAlmostEqual(
            f[0], 4068384291.8911743, delta=4068384291.8911743 * 1e-6
        )

    with ds.ExecuteSQL('SELECT ST_Area(null, 1) FROM my_layer') as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertIsNone(f[0])

    with _quiet_error_handler():
      with ds.ExecuteSQL("SELECT ST_Area(X'FF', 1) FROM my_layer") as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertIsNone(f[0])

    with _quiet_error_handler():
      with ds.ExecuteSQL(
          f'SELECT ST_Area(SetSRID({geom_colname}, -10), 0) FROM my_layer'
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertIsNone(f[0])

    ds = None

  def test_ogr_gpkg_ST_Length(self) -> None:
    tmpfilename = str(self.vsimem_dir / 'test_ogr_sql_ST_Length.gpkg')

    ds = self.driver.CreateDataSource(tmpfilename)
    assert ds is not None
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4258)
    lyr = ds.CreateLayer('my_layer', srs=srs)
    assert lyr is not None
    geom_colname = lyr.GetGeometryColumn()
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON((2 49,3 49,3 48,2 49))')
    )
    lyr.CreateFeature(feat)
    feat = None

    with ds.ExecuteSQL(
        f'SELECT ST_Length({geom_colname}) FROM my_layer'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertAlmostEqual(f[0], 3.414213562373095)

    with ds.ExecuteSQL('SELECT ST_Length(null) FROM my_layer') as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertIsNone(f[0])

    with _quiet_error_handler():
      with ds.ExecuteSQL("SELECT ST_Length(X'FF') FROM my_layer") as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertIsNone(f[0])

    ds = None

  def test_ogr_gpkg_ST_Length_on_ellipsoid(self) -> None:
    tmpfilename = str(
        self.vsimem_dir / 'test_ogr_sql_ST_Length_on_ellipsoid.gpkg'
    )

    ds = self.driver.CreateDataSource(tmpfilename)
    assert ds is not None
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4258)
    lyr = ds.CreateLayer('my_layer', srs=srs)
    assert lyr is not None
    geom_colname = lyr.GetGeometryColumn()
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometryDirectly(
        ogr.CreateGeometryFromWkt('POLYGON((2 49,3 49,3 48,2 49))')
    )
    lyr.CreateFeature(feat)
    feat = None

    with ds.ExecuteSQL(
        f'SELECT ST_Length({geom_colname}, 1) FROM my_layer'
    ) as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertAlmostEqual(
          f[0], 317885.7863996293, delta=317885.7863996293 * 1e-6
      )

    with _quiet_error_handler():
      with ds.ExecuteSQL(
          f'SELECT ST_Length({geom_colname}, 0) FROM my_layer'
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertAlmostEqual(
            f[0], 317885.7863996293, delta=317885.7863996293 * 1e-6
        )

    with ds.ExecuteSQL('SELECT ST_Length(null, 1) FROM my_layer') as sql_lyr:
      f = sql_lyr.GetNextFeature()
      assert f is not None
      self.assertIsNone(f[0])

    with _quiet_error_handler():
      with ds.ExecuteSQL("SELECT ST_Length(X'FF', 1) FROM my_layer") as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertIsNone(f[0])

    with _quiet_error_handler():
      with ds.ExecuteSQL(
          f'SELECT ST_Length(SetSRID({geom_colname}, -10), 0) FROM my_layer'
      ) as sql_lyr:
        f = sql_lyr.GetNextFeature()
        assert f is not None
        self.assertIsNone(f[0])

    ds = None

  def test_ogr_gpkg_CreateCopy(self) -> None:
    with _enable_exceptions():
      gpkg_ds = self._create_gpkg_ds()
      self._create_tpoly(gpkg_ds)
      tmpfilename = str(self.vsimem_dir / 'test_ogr_gpkg_CreateCopy.gpkg')

      out_ds = self.driver.CreateCopy(tmpfilename, gpkg_ds)
      assert out_ds is not None
      self.assertEqual(out_ds.GetLayerCount(), 1)
      out_ds = None
      gpkg_ds = None

  def test_ogr_gpkg_launder(self) -> None:
    with _enable_exceptions():
      tmpfilename = str(self.vsimem_dir / 'test_ogr_gpkg_launder.gpkg')

      ds = self.driver.CreateDataSource(tmpfilename)
      assert ds is not None
      lyr = ds.CreateLayer(
          'az+AZ09_',
          options=['FID=MY_FID', 'GEOMETRY_NAME=MY_GEOM', 'LAUNDER=YES'],
      )
      assert lyr is not None
      self.assertEqual(lyr.GetName(), 'az_az09_')
      self.assertEqual(lyr.GetFIDColumn(), 'my_fid')
      self.assertEqual(lyr.GetGeometryColumn(), 'my_geom')
      lyr.CreateField(ogr.FieldDefn('_'))
      self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetNameRef(), 'x_')
      ds = None

  def test_gpkg_rename_hidden_table(self) -> None:
    test_layer_path = str(
        self.vsimem_dir / 'test_gpkg_rename_hidden_table.gpkg'
    )

    src_ds = gdal.OpenEx(ogr_util.GetTestFilePath('shape/poly/poly.shp'))
    assert src_ds is not None
    gdal.VectorTranslate(test_layer_path, src_ds)
    src_ds = None

    dst_ds = gdal.OpenEx(
        test_layer_path, gdal.OF_UPDATE, open_options=['LIST_ALL_TABLES=NO']
    )
    assert dst_ds is not None
    dst_ds.ExecuteSQL('CREATE TABLE hidden_foo_table(id integer primary key);')
    dst_ds = None

    dst_ds = gdal.OpenEx(
        test_layer_path, gdal.OF_UPDATE, open_options=['LIST_ALL_TABLES=NO']
    )
    assert dst_ds is not None
    dst_ds.ExecuteSQL('ALTER TABLE hidden_foo_table RENAME TO hidden_bar_table')
    dst_ds.ExecuteSQL('VACUUM')
    dst_ds = None

    dst_ds = gdal.OpenEx(test_layer_path)
    assert dst_ds is not None
    # Verify that layer exists
    lyr = dst_ds.GetLayerByName('hidden_bar_table')
    self.assertIsNotNone(lyr)
    dst_ds = None

    # Check that there is no more any reference to the layer
    f = gdal.VSIFOpenL(test_layer_path, 'rb')
    assert f is not None
    content = gdal.VSIFReadL(1, 1000000, f).decode('latin1')
    gdal.VSIFCloseL(f)

    self.assertNotIn('hidden_foo_table', content)

  def test_gpkg_create_duplicate_field_names(self) -> None:
    with _enable_exceptions():
      filename = str(
          self.vsimem_dir / 'test_gpkg_create_duplicate_field_names.gpkg'
      )
      with self.driver.CreateDataSource(filename) as ds:
        assert ds is not None
        lyr = ds.CreateLayer('test')
        assert lyr is not None
        lyr.CreateField(ogr.FieldDefn('foo'))
        with self.assertRaisesRegex(
            Exception, 'A field with the same name already exists'
        ):
          lyr.CreateField(ogr.FieldDefn('foo'))
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        with self.assertRaisesRegex(
            Exception, 'A field with the same name already exists'
        ):
          lyr.CreateField(ogr.FieldDefn('FOO'))
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
        with self.assertRaisesRegex(
            Exception, 'It has the same name as the geometry field'
        ):
          lyr.CreateField(ogr.FieldDefn('geom'))
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)

  def test_gpkg_create_more_than_2000_fields(self) -> None:
    with _enable_exceptions():
      filename = str(
          self.vsimem_dir / 'test_gpkg_create_more_than_2000_fields.gpkg'
      )
      with self.driver.CreateDataSource(filename) as ds:
        assert ds is not None
        lyr = ds.CreateLayer('test')
        assert lyr is not None

        for i in range(2000 - 2):
          lyr.CreateField(ogr.FieldDefn(f'foo{i}'))
        with self.assertRaisesRegex(Exception, 'Limit of 2000 columns reached'):
          lyr.CreateField(ogr.FieldDefn('foo'))
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2000 - 2)

  def test_gpkg_secure_delete(self) -> None:
    with _enable_exceptions():
      filename = str(self.vsimem_dir / 'secure_delete.gpkg')
      with self.driver.CreateDataSource(filename) as ds:
        assert ds is not None
        with ds.ExecuteSQL('PRAGMA secure_delete') as sql_lyr:
          f = sql_lyr.GetNextFeature()
          assert f is not None
          self.assertEqual(f.GetField(0), 1)

        lyr = ds.CreateLayer('test')
        assert lyr is not None
        lyr.CreateField(ogr.FieldDefn('foo'))
        f = ogr.Feature(lyr.GetLayerDefn())
        f['foo'] = 'very_secret'
        lyr.CreateFeature(f)

      f = gdal.VSIFOpenL(filename, 'rb')
      assert f is not None
      data = gdal.VSIFReadL(1, 100000, f)
      gdal.VSIFCloseL(f)
      self.assertIn(b'very_secret', data)

      with ogr.Open(filename, update=1) as ds:
        assert ds is not None
        with ds.ExecuteSQL('PRAGMA secure_delete') as sql_lyr:
          f = sql_lyr.GetNextFeature()
          assert f is not None
          self.assertEqual(f.GetField(0), 1)

        lyr = ds.GetLayer(0)
        assert lyr is not None
        lyr.DeleteFeature(1)

      f = gdal.VSIFOpenL(filename, 'rb')
      assert f is not None
      data = gdal.VSIFReadL(1, 100000, f)
      gdal.VSIFCloseL(f)
      self.assertNotIn(b'very_secret', data)

      with gdal.config_option('OGR_SQLITE_PRAGMA', 'secure_delete=0'):
        with ogr.Open(filename, update=1) as ds:
          assert ds is not None
          with ds.ExecuteSQL('PRAGMA secure_delete') as sql_lyr:
            f = sql_lyr.GetNextFeature()
            assert f is not None
            self.assertEqual(f.GetField(0), 0)

  @parameterized.parameters(
      ('poly_golden.gpkg', ['VERSION=1.2']),
      ('poly_golden_gpkg_1_4.gpkg', ['VERSION=1.4']),
  )
  def test_ogr_gpkg_write_check_golden_file(
      self, src_name: str, options: list[str]
  ) -> None:
    src_filename = self.getTestFilePath(src_name)
    out_filename = str(self.tmp_dir / 'test.gpkg')
    with gdal.config_option('OGR_CURRENT_DATE', '2000-01-01T:00:00:00.000Z'):
      gdal.VectorTranslate(
          out_filename, src_filename, datasetCreationOptions=options
      )

    # Compare first sqlite3 dump if sqlite3 binary available
    try:
      golden_dump = subprocess.check_output(
          ['sqlite3', src_filename, '.dump']
      ).decode('utf-8')
      got_dump = subprocess.check_output(
          ['sqlite3', out_filename, '.dump']
      ).decode('utf-8')
      self.assertEqual(got_dump, golden_dump)
    except Exception:
      pass

    if _get_sqlite_version() >= (3, 46, 0):
      self.assertEqual(
          os.stat(src_filename).st_size, os.stat(out_filename).st_size
      )
      with open(src_filename, 'rb') as f_src:
        golden_data = bytearray(f_src.read())
      with open(out_filename, 'rb') as f_out:
        got_data = bytearray(f_out.read())
      # Zero out the SQLite version number at bytes 96-99.
      # Cf https://www.sqlite.org/fileformat.html
      golden_data[96] = golden_data[97] = golden_data[98] = golden_data[99] = 0
      got_data[96] = got_data[97] = got_data[98] = got_data[99] = 0
      self.assertEqual(got_data, golden_data)

  def test_ogr_gpkg_arrow_stream_numpy_datetime_as_string(self) -> None:
    if (
        not hasattr(gdal_array, '_RecordBatchAsNumpy')
        or importlib.util.find_spec('numpy') is None
    ):
      self.skipTest('gdal_array or numpy missing')

    filename = str(self.vsimem_dir / 'datetime_as_string.gpkg')
    ds = self.driver.CreateDataSource(filename)
    assert ds is not None
    lyr = ds.CreateLayer('test')
    assert lyr is not None

    field = ogr.FieldDefn('datetime', ogr.OFTDateTime)
    lyr.CreateField(field)

    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('datetime', '2022-05-31T12:34:56.789Z')
    lyr.CreateFeature(f)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('datetime', '2022-05-31T12:34:56.000')
    lyr.CreateFeature(f)

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('datetime', '2022-05-31T12:34:56.000+12:30')
    lyr.CreateFeature(f)

    # Test DATETIME_AS_STRING=YES
    stream = lyr.GetArrowStreamAsNumPy(
        options=['USE_MASKED_ARRAYS=NO', 'DATETIME_AS_STRING=YES']
    )
    batches = [batch for batch in stream]
    self.assertEqual(len(batches), 1)
    batch = batches[0]
    self.assertEqual(len(batch['datetime']), 4)
    self.assertEqual(batch['datetime'][0], b'')
    self.assertEqual(batch['datetime'][1], b'2022-05-31T12:34:56.789Z')
    self.assertEqual(batch['datetime'][2], b'2022-05-31T12:34:56')
    self.assertEqual(batch['datetime'][3], b'2022-05-31T12:34:56+12:30')

    # Setting a filter tests the use of the less optimized
    # OGRGeoPackageTableLayer::GetNextArray() implementation
    lyr.SetAttributeFilter('1 = 1')
    stream = lyr.GetArrowStreamAsNumPy(
        options=['USE_MASKED_ARRAYS=NO', 'DATETIME_AS_STRING=YES']
    )
    lyr.SetAttributeFilter(None)
    batches = [batch for batch in stream]
    self.assertEqual(len(batches), 1)
    batch = batches[0]
    self.assertEqual(len(batch['datetime']), 4)
    self.assertEqual(batch['datetime'][0], b'')
    self.assertEqual(batch['datetime'][1], b'2022-05-31T12:34:56.789Z')
    self.assertEqual(batch['datetime'][2], b'2022-05-31T12:34:56')
    self.assertEqual(batch['datetime'][3], b'2022-05-31T12:34:56+12:30')

    with ds.ExecuteSQL('SELECT * FROM test') as sql_lyr:
      stream = sql_lyr.GetArrowStreamAsNumPy(
          options=['USE_MASKED_ARRAYS=NO', 'DATETIME_AS_STRING=YES']
      )
      batches = [batch for batch in stream]
      self.assertEqual(len(batches), 1)
      batch = batches[0]
      self.assertEqual(len(batch['datetime']), 4)
      self.assertEqual(batch['datetime'][0], b'')
      self.assertEqual(batch['datetime'][1], b'2022-05-31T12:34:56.789Z')
      self.assertEqual(batch['datetime'][2], b'2022-05-31T12:34:56')
      self.assertEqual(batch['datetime'][3], b'2022-05-31T12:34:56+12:30')

    ds = None

  def _check_transaction_rollback(
      self, ds: gdal.Dataset, start_transaction: bool
  ) -> None:
    gdal.ErrorReset()

    lyr = ds.CreateLayer('test', options=['GEOMETRY_NAME=geom'])
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('fld1', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('fld2', ogr.OFTString))

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('fld1', 'value1')
    f.SetField('fld2', 'value2')
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
    f = None

    fld1 = lyr.GetLayerDefn().GetFieldDefn(0)
    fld2 = lyr.GetLayerDefn().GetFieldDefn(1)

    def verify() -> None:
      self.assertEqual(lyr.GetGeometryColumn(), 'geom')
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld1')
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
      )
      self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'fld2')
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(1).GetType(), ogr.OFTString
      )
      self.assertEqual(fld1.GetName(), 'fld1')
      self.assertEqual(fld2.GetName(), 'fld2')

    # Test deleting a field
    ds.StartTransaction() if start_transaction else ds.ExecuteSQL('BEGIN')
    lyr.DeleteField(0)
    self.assertEqual(fld1.GetName(), 'fld1')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    ds.RollbackTransaction() if start_transaction else ds.ExecuteSQL('ROLLBACK')
    verify()

    # Test deleting the second field
    ds.StartTransaction() if start_transaction else ds.ExecuteSQL('BEGIN')
    lyr.DeleteField(1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld1')
    ds.RollbackTransaction() if start_transaction else ds.ExecuteSQL('ROLLBACK')
    verify()

    # Test renaming and changing the type of a field
    fld1 = lyr.GetLayerDefn().GetFieldDefn(0)
    self.assertEqual(fld1.GetName(), 'fld1')
    ds.StartTransaction() if start_transaction else ds.ExecuteSQL('BEGIN')
    self.assertEqual(
        lyr.AlterFieldDefn(
            0, ogr.FieldDefn('fld1_renamed', ogr.OFTInteger), ogr.ALTER_ALL_FLAG
        ),
        ogr.OGRERR_NONE,
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld1_renamed'
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTInteger
    )
    self.assertEqual(fld1.GetName(), 'fld1_renamed')
    ds.RollbackTransaction() if start_transaction else ds.ExecuteSQL('ROLLBACK')
    verify()

    # Test adding a field
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    ds.StartTransaction() if start_transaction else ds.ExecuteSQL('BEGIN')
    fld = ogr.FieldDefn('fld3', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
    fld3 = lyr.GetLayerDefn().GetFieldDefn(2)
    self.assertEqual(fld3.GetName(), 'fld3')
    ds.RollbackTransaction() if start_transaction else ds.ExecuteSQL('ROLLBACK')
    verify()
    self.assertEqual(fld3.GetName(), 'fld3')

    # Test multiple operations
    ds.StartTransaction() if start_transaction else ds.ExecuteSQL('BEGIN')
    lyr.DeleteField(0)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    fld = ogr.FieldDefn('fld3', ogr.OFTInteger)
    self.assertEqual(lyr.CreateField(fld), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'fld3')
    self.assertEqual(
        lyr.AlterFieldDefn(
            0, ogr.FieldDefn('fld2_renamed', ogr.OFTInteger), ogr.ALTER_ALL_FLAG
        ),
        ogr.OGRERR_NONE,
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2_renamed'
    )
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTInteger
    )
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'fld3')
    self.assertEqual(
        lyr.GetLayerDefn().GetFieldDefn(1).GetType(), ogr.OFTInteger
    )
    ds.RollbackTransaction() if start_transaction else ds.ExecuteSQL('ROLLBACK')
    verify()

  @parameterized.parameters(False, True)
  def test_ogr_gpkg_field_operations_rollback(
      self, start_transaction: bool
  ) -> None:
    filename = str(self.vsimem_dir / 'test.gpkg')
    with self.driver.CreateDataSource(filename) as ds:
      assert ds is not None
      self._check_transaction_rollback(ds, start_transaction)

  def _check_transaction_rollback_with_savepoint(
      self, ds: gdal.Dataset, start_transaction: bool
  ) -> None:
    gdal.ErrorReset()

    self.assertEqual(gdal.GetLastErrorMsg(), '', gdal.GetLastErrorMsg())
    lyr = ds.CreateLayer(
        'test_rollback_with_savepoint', options=['GEOMETRY_NAME=geom']
    )
    assert lyr is not None
    lyr.CreateField(ogr.FieldDefn('fld1', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('fld2', ogr.OFTString))

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetField('fld1', 'value1')
    f.SetField('fld2', 'value2')
    self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)

    fld1 = lyr.GetLayerDefn().GetFieldDefn(0)
    fld2 = lyr.GetLayerDefn().GetFieldDefn(1)

    def verify() -> None:
      self.assertEqual(lyr.GetGeometryColumn(), 'geom')
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld1')
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTString
      )
      self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(1).GetName(), 'fld2')
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(1).GetType(), ogr.OFTString
      )
      self.assertEqual(fld1.GetName(), 'fld1')
      self.assertEqual(fld2.GetName(), 'fld2')
      self.assertEqual(gdal.GetLastErrorMsg(), '', gdal.GetLastErrorMsg())

    # Test SAVEPOINT
    ds.ExecuteSQL('SAVEPOINT test_savepoint1')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    ds.ExecuteSQL('ROLLBACK test_savepoint1')

    verify()
    ds.ExecuteSQL('ROLLBACK')

    # Test nested savepoints
    ds.ExecuteSQL('SAVEPOINT test_savepoint2')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    ds.ExecuteSQL('SAVEPOINT test_savepoint3')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 0)
    ds.ExecuteSQL('ROLLBACK test_savepoint3')
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    ds.ExecuteSQL('ROLLBACK test_savepoint2')

    verify()
    ds.ExecuteSQL('ROLLBACK')

    # Test rollback to first savepoint
    ds.ExecuteSQL('SAVEPOINT test_savepoint5')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    ds.ExecuteSQL('SAVEPOINT test_savepoint6')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 0)
    ds.ExecuteSQL('ROLLBACK test_savepoint5')

    verify()
    ds.ExecuteSQL('ROLLBACK')

    # Test rollback without specifying a savepoint
    ds.ExecuteSQL('SAVEPOINT test_savepoint7')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    ds.ExecuteSQL('SAVEPOINT test_savepoint8')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 0)
    ds.ExecuteSQL('ROLLBACK')

    verify()

    # Test error conditions
    self.assertEqual(gdal.GetLastErrorMsg(), '')

    # Test rollback to non-existing savepoint
    ds.ExecuteSQL('SAVEPOINT test_savepoint9')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')

    with _quiet_error_handler():
      ds.ExecuteSQL('ROLLBACK test_savepointXXX')
    self.assertTrue(
        gdal.GetLastErrorMsg().startswith(
            'Savepoint test_savepointXXX not found'
        )
    )
    gdal.ErrorReset()
    ds.ExecuteSQL('ROLLBACK test_savepoint9')

    verify()
    ds.ExecuteSQL('ROLLBACK')
    gdal.ErrorReset()

    # Test savepoint from within a transaction
    self.assertEqual(gdal.GetLastErrorMsg(), '', gdal.GetLastErrorMsg())
    ds.StartTransaction() if start_transaction else ds.ExecuteSQL('BEGIN')
    self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
    self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
    self.assertEqual(lyr.GetLayerDefn().GetFieldDefn(0).GetName(), 'fld2')
    ds.ExecuteSQL('SAVEPOINT test_savepoint10')
    self.assertEqual(gdal.GetLastErrorMsg(), '', gdal.GetLastErrorMsg())
    ds.RollbackTransaction() if start_transaction else ds.ExecuteSQL('ROLLBACK')
    verify()

  @parameterized.parameters(False, True)
  def test_ogr_gpkg_field_operations_savepoint_rollback(
      self, start_transaction: bool
  ) -> None:
    filename = str(self.vsimem_dir / 'test_savepoint.gpkg')
    with self.driver.CreateDataSource(filename) as ds:
      assert ds is not None
      self._check_transaction_rollback_with_savepoint(ds, start_transaction)

  @parameterized.product(
      auto_begin_transaction=(False, True),
      start_transaction=(False, True),
      savepoint_case=(
          ([1], [], ['fld3']),
          ([2], [], ['fld3']),
          ([3], [], ['fld3']),
          ([4], [], ['fld3']),
          ([], [1], ['fld1', 'fld2', 'fld3', 'fld4', 'fld5']),
          ([], [2], ['fld1', 'fld3', 'fld4', 'fld5']),
          ([], [3], ['fld1', 'fld3', 'fld5']),
          ([], [4], ['fld3', 'fld5']),
      ),
  )
  def test_ogr_gpkg_field_operations_savepoint_release(
      self,
      auto_begin_transaction: bool,
      start_transaction: bool,
      savepoint_case: tuple[list[int], list[int], list[str]],
  ) -> None:
    release_to, rollback_to, expected = savepoint_case
    filename = str(self.vsimem_dir / 'test_savepoint_release.gpkg')
    gdal.ErrorReset()

    def get_field_names(layer: ogr.Layer) -> list[str]:
      return [
          layer.GetLayerDefn().GetFieldDefn(i).GetName()
          for i in range(layer.GetLayerDefn().GetFieldCount())
      ]

    layer_name = 'svp_release_%s' % '_'.join(str(x) for x in release_to)
    with self.driver.CreateDataSource(filename) as ds:
      assert ds is not None
      lyr = ds.CreateLayer(layer_name, options=['GEOMETRY_NAME=geom'])
      assert lyr is not None
      for i in range(1, 6):
        lyr.CreateField(ogr.FieldDefn('fld%d' % i, ogr.OFTString))

      f = ogr.Feature(lyr.GetLayerDefn())
      for i in range(1, 6):
        f.SetField('fld%d' % i, 'value%d' % i)
      self.assertEqual(lyr.CreateFeature(f), ogr.OGRERR_NONE)
      f = None

      fld1 = lyr.GetLayerDefn().GetFieldDefn(0)
      fld2 = lyr.GetLayerDefn().GetFieldDefn(1)
      fld3 = lyr.GetLayerDefn().GetFieldDefn(2)
      fld4 = lyr.GetLayerDefn().GetFieldDefn(3)
      fld5 = lyr.GetLayerDefn().GetFieldDefn(4)

      self.assertEqual(fld1.GetName(), 'fld1')
      self.assertEqual(fld2.GetName(), 'fld2')
      self.assertEqual(fld3.GetName(), 'fld3')
      self.assertEqual(fld4.GetName(), 'fld4')
      self.assertEqual(fld5.GetName(), 'fld5')
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 5)
      self.assertEqual(
          get_field_names(lyr), ['fld1', 'fld2', 'fld3', 'fld4', 'fld5']
      )

      if auto_begin_transaction:
        ds.StartTransaction() if start_transaction else ds.ExecuteSQL('BEGIN')

      ds.ExecuteSQL('SAVEPOINT test_savepoint1')
      self.assertEqual(lyr.DeleteField(1), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 4)
      self.assertEqual(get_field_names(lyr), ['fld1', 'fld3', 'fld4', 'fld5'])

      ds.ExecuteSQL('SAVEPOINT test_savepoint2')
      self.assertEqual(lyr.DeleteField(2), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
      self.assertEqual(get_field_names(lyr), ['fld1', 'fld3', 'fld5'])

      ds.ExecuteSQL('SAVEPOINT test_savepoint3')
      self.assertEqual(lyr.DeleteField(0), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(get_field_names(lyr), ['fld3', 'fld5'])

      ds.ExecuteSQL('SAVEPOINT test_savepoint4')
      self.assertEqual(lyr.DeleteField(1), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
      self.assertEqual(get_field_names(lyr), ['fld3'])

      for i in release_to:
        ds.ExecuteSQL('RELEASE test_savepoint%d' % i)

      for i in rollback_to:
        ds.ExecuteSQL('ROLLBACK TO test_savepoint%d' % i)

      for i in release_to:
        with _quiet_error_handler():
          self.assertNotEqual(
              ds.ExecuteSQL('ROLLBACK test_savepoint%d' % i), ogr.OGRERR_NONE
          )
        self.assertTrue(
            gdal.GetLastErrorMsg().startswith(
                'Savepoint test_savepoint%d not found' % i
            )
        )
        gdal.ErrorReset()

      if auto_begin_transaction or 1 not in release_to:
        ds.CommitTransaction() if start_transaction else ds.ExecuteSQL('COMMIT')

      self.assertEqual(gdal.GetLastErrorMsg(), '', gdal.GetLastErrorMsg())

    with ogr.Open(filename) as ds:
      assert ds is not None
      lyr = ds.GetLayerByName(layer_name)
      assert lyr is not None
      self.assertEqual(lyr.GetGeometryColumn(), 'geom')
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)
      self.assertEqual(get_field_names(lyr), expected)
      self.assertIsNotNone(fld1)
      self.assertIsNotNone(fld2)
      self.assertIsNotNone(fld3)
      self.assertIsNotNone(fld4)
      self.assertIsNotNone(fld5)

  def test_ogr_gpkg_gdal_driver_gpkg_repack(self) -> None:
    with _enable_exceptions():
      poly_shp = ogr_util.GetTestFilePath('shape/poly/poly.shp')
      not_gpkg = str(self.tmp_dir / 'poly.shp')
      gdal.VectorTranslate(not_gpkg, poly_shp).Close()
      test_gpkg = str(self.tmp_dir / 'test.gpkg')

      alg = gdal.GetGlobalAlgorithmRegistry()['driver']
      self.assertEqual(alg.GetName(), 'driver')

      alg = gdal.GetGlobalAlgorithmRegistry()['driver']['gpkg']
      self.assertEqual(alg.GetName(), 'gpkg')

      alg = gdal.GetGlobalAlgorithmRegistry()['driver']['gpkg']['repack']
      self.assertEqual(alg.GetName(), 'repack')
      alg['dataset'] = not_gpkg
      with self.assertRaisesRegex(Exception, 'is not a GeoPackage'):
        alg.Run()

      ds = gdal.VectorTranslate(test_gpkg, poly_shp)
      assert ds is not None
      ds.ExecuteSQL('DELETE FROM poly')
      ds.Close()

      size_before = os.stat(test_gpkg).st_size

      alg = gdal.GetGlobalAlgorithmRegistry()['driver']['gpkg']['repack']
      self.assertEqual(alg.GetName(), 'repack')
      alg['dataset'] = test_gpkg
      self.assertTrue(alg.Run())

      size_after = os.stat(test_gpkg).st_size
      self.assertLess(size_after, size_before)

  def test_ogr_gpkg_append_to_layer_feature_count_int64_max(self) -> None:
    filename = str(self.vsimem_dir / 'tmp.gpkg')
    with _enable_exceptions():
      with self.driver.CreateVector(filename) as ds:
        assert ds is not None
        ds.CreateLayer('test')

      with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
        assert ds is not None
        ds.ExecuteSQL('UPDATE gpkg_ogr_contents SET feature_count = -2')

      with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
        assert ds is not None
        lyr = ds.GetLayer(0)
        assert lyr is not None
        self.assertEqual(lyr.GetFeatureCount(), 0)

      with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
        assert ds is not None
        ds.ExecuteSQL(
            'UPDATE gpkg_ogr_contents SET feature_count = 9223372036854775807'
        )

      with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
        assert ds is not None
        lyr = ds.GetLayer(0)
        assert lyr is not None
        self.assertEqual(lyr.GetFeatureCount(), 9223372036854775807)
        f = ogr.Feature(lyr.GetLayerDefn())
        lyr.CreateFeature(f)
        f = ogr.Feature(lyr.GetLayerDefn())
        lyr.CreateFeature(f)

      with gdal.OpenEx(filename, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
        assert ds is not None
        with ds.ExecuteSQL(
            'SELECT feature_count FROM gpkg_ogr_contents'
        ) as sql_lyr:
          f = sql_lyr.GetNextFeature()
          assert f is not None
          self.assertFalse(f.IsFieldSetAndNotNull('feature_count'))
        lyr = ds.GetLayer(0)
        assert lyr is not None
        self.assertEqual(lyr.GetFeatureCount(), 2)

  def test_ogr_gpkg_set_next_by_index(self) -> None:
    filename = str(self.vsimem_dir / 'set_next_by_index.gpkg')

    with self.driver.CreateDataSource(filename) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('foo'))
      lyr.SetNextByIndex(0)
      f = ogr.Feature(lyr.GetLayerDefn())
      f['foo'] = 'bar'
      lyr.CreateFeature(f)

    with ogr.Open(filename) as ds:
      lyr = ds.GetLayer(0)
      lyr.SetNextByIndex(0)
      f = lyr.GetNextFeature()
      self.assertEqual(f['foo'], 'bar')


if __name__ == '__main__':
  googletest.main()
