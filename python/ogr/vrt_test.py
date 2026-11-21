# Copyright 2026 Google LLC
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
# Copyright (c) 2009-2014, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT

"""Tests OGR handling of VRT virtual vector datasources.

Format is described here:
  https://gdal.org/en/latest/drivers/vector/vrt.html

Rewrite of:
  https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_vrt.py
"""

from collections.abc import Callable
from collections.abc import Sequence
import pathlib
import struct

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util


def _geometry_type_cases() -> list[dict[str, object]]:
  """Builds valid (type_str, base_type, qualifier) parameter cases."""
  # Keep base_type as a plain int constant at import time; OGR GT_SetZ/GT_SetM
  # helpers are invoked inside the test method once ExceptionMgr is active.
  base_types = (
      ('Point', ogr.wkbPoint),
      ('LineString', ogr.wkbLineString),
      ('Polygon', ogr.wkbPolygon),
      ('MultiPoint', ogr.wkbMultiPoint),
      ('MultiLineString', ogr.wkbMultiLineString),
      ('MultiPolygon', ogr.wkbMultiPolygon),
      ('GeometryCollection', ogr.wkbGeometryCollection),
      ('CircularString', ogr.wkbCircularString),
      ('CompoundCurve', ogr.wkbCompoundCurve),
      ('CurvePolygon', ogr.wkbCurvePolygon),
      ('MultiCurve', ogr.wkbMultiCurve),
      ('MultiSurface', ogr.wkbMultiSurface),
      ('Curve', ogr.wkbCurve),
      ('Surface', ogr.wkbSurface),
  )
  cases: list[dict[str, object]] = []
  for type_str, ogr_type in base_types:
    for qualifier in ('', 'Z', 'M', 'ZM', '25D'):
      if qualifier == 'Z' and ogr_type <= ogr.wkbGeometryCollection:
        continue
      if qualifier == '25D' and ogr_type > ogr.wkbGeometryCollection:
        continue
      cases.append(
          dict(
              testcase_name=f'{type_str}_{qualifier or "2d"}'.lower(),
              type_str=type_str,
              base_type=ogr_type,
              qualifier=qualifier,
          )
      )
  return cases


def _write_vsi_text(path: pathlib.Path, content: str) -> str:
  """Writes UTF-8 text to a /vsimem/ path and returns its string path."""
  path_str = str(path)
  gdal.FileFromMemBuffer(path_str, content.encode('utf-8'))
  return path_str


def _create_flat_dbf(dbf_path: str) -> None:
  """Creates a 2-row non-spatial DBF matching upstream flat.dbf."""
  shp_drv = gdal.GetDriverByName('ESRI Shapefile')
  with shp_drv.Create(dbf_path, 0, 0, 0, gdal.GDT_Unknown) as ds:
    lyr = ds.CreateLayer('flat', geom_type=ogr.wkbNone)
    for field_name, field_type, width, precision in (
        ('fid', ogr.OFTInteger, 8, 0),
        ('other', ogr.OFTString, 8, 0),
        ('x', ogr.OFTReal, 8, 3),
        ('y', ogr.OFTReal, 8, 3),
        ('z', ogr.OFTReal, 8, 3),
        ('wkt', ogr.OFTString, 30, 0),
    ):
      fld = ogr.FieldDefn(field_name, field_type)
      fld.SetWidth(width)
      fld.SetPrecision(precision)
      lyr.CreateField(fld)

    rows = (
        (1, 'First', 12.5, 17.0, 1.2, 'POINT(12.5 17 1.2)'),
        (2, 'Second', 100.0, 200.0, 0.0, 'POINT(100 200 0)'),
    )
    for fid_val, other_val, x_val, y_val, z_val, wkt_val in rows:
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat['fid'] = fid_val
      feat['other'] = other_val
      feat['x'] = x_val
      feat['y'] = y_val
      feat['z'] = z_val
      feat['wkt'] = wkt_val
      lyr.CreateFeature(feat)


def _create_vrt_test_files(vsi_dir: pathlib.Path) -> tuple[str, str]:
  """Creates flat.dbf and vrt_test.vrt inside vsi_dir."""
  dbf_path = str(vsi_dir / 'flat.dbf')
  _create_flat_dbf(dbf_path)

  # Note: HAVE_LIBXML2 is disabled in Google3 builds, so test5's invalid
  # <dummySrcDataSource> tag emits no XSD schema validation warning on OpenEx.

  vrt_xml = f"""<?xml version="1.0"?>
<OGRVRTDataSource>
  <OGRVRTLayer name="test2">
    <SrcDataSource relativeToVRT="1">flat.dbf</SrcDataSource>
    <SrcLayer>flat</SrcLayer>
    <GeometryType>wkbPoint</GeometryType>
    <GeometryField encoding="PointFromColumns" x="x" y="y" z="z"/>
  </OGRVRTLayer>
  <OGRVRTLayer name="test3">
    <SrcDataSource shared="TRUE">{dbf_path}</SrcDataSource>
    <SrcLayer>flat</SrcLayer>
    <LayerSRS>WGS84</LayerSRS>
    <FID>fid</FID>
    <GeometryType>wkbPoint</GeometryType>
    <GeometryField encoding="WKT" field="wkt"/>
  </OGRVRTLayer>
  <OGRVRTLayer name="test4">
    <SrcDataSource>{dbf_path}</SrcDataSource>
    <SrcSQL>SELECT * FROM flat</SrcSQL>
    <FID>fid</FID>
    <GeometryType>wkbPoint</GeometryType>
    <GeometryField encoding="WKT" field="wkt"/>
  </OGRVRTLayer>
  <OGRVRTLayer name="test5">
    <dummySrcDataSource shared="TRUE">{dbf_path}</dummySrcDataSource>
    <SrcLayer>flat</SrcLayer>
    <LayerSRS>WGS84</LayerSRS>
    <FID>fid</FID>
    <GeometryType>wkbPoint</GeometryType>
    <GeometryField encoding="WKT" field="wkt"/>
  </OGRVRTLayer>
  <OGRVRTLayer name="test6">
    <SrcDataSource shared="TRUE">{dbf_path}</SrcDataSource>
    <SrcLayer>flat</SrcLayer>
    <FID>fid</FID>
    <Field name="x"/>
  </OGRVRTLayer>
  <OGRVRTLayer name="test7">
    <SrcDataSource shared="TRUE">{dbf_path}</SrcDataSource>
    <SrcLayer>flat</SrcLayer>
    <FID name="bar">fid</FID>
    <Field name="x"/>
  </OGRVRTLayer>
  <OGRVRTLayer name="test8">
    <SrcDataSource shared="TRUE"><![CDATA[
      <OGRVRTDataSource>
        <OGRVRTLayer name="src_layer">
          <SrcDataSource shared="TRUE">{dbf_path}</SrcDataSource>
          <SrcLayer>flat</SrcLayer>
          <FID name="bar">fid</FID>
        </OGRVRTLayer>
      </OGRVRTDataSource>]]>
    </SrcDataSource>
    <SrcLayer>src_layer</SrcLayer>
    <FID/>
  </OGRVRTLayer>
</OGRVRTDataSource>
"""
  vrt_path = _write_vsi_text(vsi_dir / 'vrt_test.vrt', vrt_xml)
  return vrt_path, dbf_path


def _create_points_shapefile(
    shp_path: str,
    points: Sequence[tuple[float, float]],
    create_spatial_index: bool = True,
) -> None:
  """Creates a point shapefile with optional spatial index in /vsimem/."""
  shp_drv = gdal.GetDriverByName('ESRI Shapefile')
  layer_name = pathlib.Path(shp_path).stem
  with shp_drv.Create(shp_path, 0, 0, 0, gdal.GDT_Unknown) as ds:
    lyr = ds.CreateLayer(layer_name, geom_type=ogr.wkbPoint)
    for x_val, y_val in points:
      feat = ogr.Feature(lyr.GetLayerDefn())
      geom = ogr.Geometry(ogr.wkbPoint)
      geom.AddPoint_2D(x_val, y_val)
      feat.SetGeometryDirectly(geom)
      lyr.CreateFeature(feat)
    if create_spatial_index:
      ds.ExecuteSQL(f'CREATE SPATIAL INDEX ON {lyr.GetName()}')


def _create_point_grid_shapefile(
    shp_path: str,
    layer_name: str,
    x_origin: float,
    fields: Sequence[tuple[str, int, Callable[[int], int | str]]],
    create_spatial_index: bool = False,
) -> None:
  """Creates a 5x5 EPSG:4326 point grid shapefile in /vsimem/."""
  shp_drv = gdal.GetDriverByName('ESRI Shapefile')
  srs = osr.SpatialReference()
  srs.ImportFromEPSG(4326)
  with shp_drv.Create(shp_path, 0, 0, 0, gdal.GDT_Unknown) as ds:
    lyr = ds.CreateLayer(layer_name, srs=srs, geom_type=ogr.wkbPoint)
    for field_name, field_type, _ in fields:
      lyr.CreateField(ogr.FieldDefn(field_name, field_type))
    for i in range(5):
      for j in range(5):
        idx = i * 5 + j
        feat = ogr.Feature(lyr.GetLayerDefn())
        for field_name, _, value_fn in fields:
          feat[field_name] = value_fn(idx)
        geom = ogr.Geometry(ogr.wkbPoint)
        geom.AddPoint_2D(x_origin + i / 5.0, 49.0 + j / 5.0)
        feat.SetGeometryDirectly(geom)
        lyr.CreateFeature(feat)
    if create_spatial_index:
      ds.ExecuteSQL(f'CREATE SPATIAL INDEX ON {layer_name}')


def _create_multigeom_csv_dir(base_dir: pathlib.Path) -> str:
  """Creates a multi-geometry two-layer CSV directory in /vsimem/."""
  csv_dir = str(base_dir / 'multigeom')
  csv_drv = gdal.GetDriverByName('CSV')
  with csv_drv.Create(
      csv_dir, 0, 0, 0, gdal.GDT_Unknown, options=['GEOMETRY=AS_WKT']
  ) as ds:
    lyr = ds.CreateLayer(
        'test', geom_type=ogr.wkbNone, options=['CREATE_CSVT=YES']
    )
    lyr.CreateGeomField(
        ogr.GeomFieldDefn('geom__WKT_EPSG_4326_POINT', ogr.wkbPoint)
    )
    lyr.CreateGeomField(
        ogr.GeomFieldDefn('geom__WKT_EPSG_32632_POLYGON', ogr.wkbPolygon)
    )
    lyr.CreateGeomField(
        ogr.GeomFieldDefn('geom__WKT_EPSG_4326_LINESTRING', ogr.wkbLineString)
    )
    lyr.CreateField(ogr.FieldDefn('X', ogr.OFTReal))
    lyr.CreateField(ogr.FieldDefn('Y', ogr.OFTReal))

    lyr2 = ds.CreateLayer(
        'test2', geom_type=ogr.wkbNone, options=['CREATE_CSVT=YES']
    )
    lyr2.CreateGeomField(
        ogr.GeomFieldDefn('geom__WKT_EPSG_32632_POLYGON', ogr.wkbPolygon)
    )
    lyr2.CreateGeomField(
        ogr.GeomFieldDefn('geom__WKT_EPSG_4326_POINT', ogr.wkbPoint)
    )
    lyr2.CreateGeomField(
        ogr.GeomFieldDefn('geom__WKT_EPSG_32631_POINT', ogr.wkbPoint)
    )
    lyr2.CreateField(ogr.FieldDefn('Y', ogr.OFTReal))
    lyr2.CreateField(ogr.FieldDefn('X', ogr.OFTReal))

  with gdal.OpenEx(csv_dir, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
    lyr = ds.GetLayerByName('test')
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeomField(0, ogr.CreateGeometryFromWkt('POINT (1 2)'))
    feat.SetGeomField(
        1, ogr.CreateGeometryFromWkt('POLYGON ((0 0,0 1,1 1,1 0,0 0))')
    )
    feat['X'] = -1
    feat['Y'] = -2
    lyr.CreateFeature(feat)

    lyr2 = ds.GetLayerByName('test2')
    feat2 = ogr.Feature(lyr2.GetLayerDefn())
    feat2.SetGeomField(
        0, ogr.CreateGeometryFromWkt('POLYGON ((1 1,1 2,2 2,2 1,1 1))')
    )
    feat2.SetGeomField(1, ogr.CreateGeometryFromWkt('POINT (3 4)'))
    feat2.SetGeomField(2, ogr.CreateGeometryFromWkt('POINT (5 6)'))
    feat2['X'] = -3
    feat2['Y'] = -4
    lyr2.CreateFeature(feat2)

  return csv_dir


class VrtTest(parameterized.TestCase, gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(ogr.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())
    self.driver = gdal.GetDriverByName('OGR_VRT')
    self.assertIsNotNone(self.driver)

  def _make_vsi_dir(self) -> pathlib.Path:
    vsi_dir = self.enter_context(
        gcore_util.VsimemCleanupDir(
            f'/vsimem/ogr_vrt_test/{self._testMethodName}'
        )
    )
    self.assertEqual(gdal.MkdirRecursive(vsi_dir, 0o755), 0)
    return pathlib.Path(vsi_dir)

  def _srs_from_input(self, spec: str) -> osr.SpatialReference:
    srs = osr.SpatialReference()
    self.assertEqual(srs.SetFromUserInput(spec), ogr.OGRERR_NONE)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return srs

  def _assert_vrt_open_error(self, vrt_xml: str, error_regex: str) -> None:
    gdal.ErrorReset()
    with gdal.quiet_errors():
      with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
        err_msg = gdal.GetLastErrorMsg()
        self.assertRegex(err_msg, error_regex)
        self.assertEqual(ds.GetLayerCount(), 0)

  def _assert_geom_matches_wkt(
      self,
      actual: ogr.Geometry | None,
      expected_wkt: str,
      max_error: float = 1e-9,
  ) -> None:
    self.assertIsNotNone(actual)
    expected = ogr.CreateGeometryFromWkt(expected_wkt)
    self.assertIsNotNone(expected)
    assert actual is not None
    assert expected is not None
    self._assert_geom_tree_close(actual, expected, max_error)

  def _assert_geom_tree_close(
      self, actual: ogr.Geometry, expected: ogr.Geometry, max_error: float
  ) -> None:
    self.assertEqual(actual.GetGeometryName(), expected.GetGeometryName())
    self.assertEqual(actual.Is3D(), expected.Is3D())
    self.assertEqual(actual.IsMeasured(), expected.IsMeasured())
    self.assertEqual(actual.GetGeometryCount(), expected.GetGeometryCount())
    self.assertEqual(actual.GetPointCount(), expected.GetPointCount())
    if actual.GetGeometryCount() > 0:
      for i in range(actual.GetGeometryCount()):
        self._assert_geom_tree_close(
            actual.GetGeometryRef(i), expected.GetGeometryRef(i), max_error
        )
    else:
      for i in range(actual.GetPointCount()):
        self.assertAlmostEqual(
            actual.GetX(i), expected.GetX(i), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetY(i), expected.GetY(i), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetZ(i), expected.GetZ(i), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetM(i), expected.GetM(i), delta=max_error
        )

  def _assert_field_values(
      self, layer: ogr.Layer, field_name: str, expected: Sequence[object]
  ) -> None:
    layer.ResetReading()
    got = [feat[field_name] for feat in layer]
    self.assertEqual(got, list(expected))

  def _get_testdata_path(self, rel_path: str) -> str:
    filepath = ogr_util.GetTestFilePath(rel_path)
    self.assertTrue(pathlib.Path(filepath).exists(), f'Missing {filepath}')
    return filepath

  def test_point_from_columns_and_source_fid(self) -> None:
    # Upstream: test_ogr_vrt_2
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test2')
      self.assertIsNotNone(lyr)
      self.assertEqual(lyr.GetExtent(), (12.5, 100.0, 17.0, 200.0))
      self._assert_field_values(lyr, 'other', ['First', 'Second'])

      lyr.ResetReading()
      f0 = lyr.GetNextFeature()
      self.assertIsNotNone(f0)
      self.assertEqual(f0.GetFID(), 0)
      self._assert_geom_matches_wkt(
          f0.GetGeometryRef(), 'POINT Z (12.5 17 1.2)'
      )

      f1 = lyr.GetNextFeature()
      self.assertIsNotNone(f1)
      self.assertEqual(f1.GetFID(), 1)
      self._assert_geom_matches_wkt(f1.GetGeometryRef(), 'POINT Z (100 200 0)')
      self.assertIsNone(lyr.GetNextFeature())

  def test_relative_parent_path_on_disk(self) -> None:
    # Verifies relativeToVRT="1" parent directory traversal (not normalized in
    # /vsimem/, so tested against the local filesystem).
    tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    _create_flat_dbf(str(tmp_dir / 'flat.dbf'))
    vrt_dir = tmp_dir / 'vrt'
    vrt_dir.mkdir(parents=True, exist_ok=True)
    vrt_path = vrt_dir / 'vrt_test.vrt'
    vrt_path.write_text(
        '<OGRVRTDataSource>'
        '<OGRVRTLayer name="test2">'
        '<SrcDataSource relativeToVRT="1">../flat.dbf</SrcDataSource>'
        '<SrcLayer>flat</SrcLayer>'
        '<GeometryType>wkbPoint</GeometryType>'
        '<GeometryField encoding="PointFromColumns" x="x" y="y" z="z"/>'
        '</OGRVRTLayer>'
        '</OGRVRTDataSource>',
        encoding='utf-8',
    )
    with gdal.OpenEx(str(vrt_path), gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test2')
      self.assertIsNotNone(lyr)
      self.assertEqual(lyr.GetFeatureCount(), 2)
      self._assert_field_values(lyr, 'other', ['First', 'Second'])

  def test_wkt_column_and_fid_from_attribute(self) -> None:
    # Upstream: test_ogr_vrt_3
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    expected_srs = self._srs_from_input('WGS84')

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test3')
      self.assertIsNotNone(lyr)
      self.assertIsSame(lyr.GetSpatialRef(), expected_srs)
      self._assert_field_values(lyr, 'other', ['First', 'Second'])

      lyr.ResetReading()
      f1 = lyr.GetNextFeature()
      self.assertIsNotNone(f1)
      self.assertEqual(f1.GetFID(), 1)
      self._assert_geom_matches_wkt(
          f1.GetGeometryRef(), 'POINT Z (12.5 17 1.2)'
      )

      f2 = lyr.GetNextFeature()
      self.assertIsNotNone(f2)
      self.assertEqual(f2.GetFID(), 2)
      self._assert_geom_matches_wkt(f2.GetGeometryRef(), 'POINT Z (100 200 0)')

  def test_spatial_filter_on_wkt_layer(self) -> None:
    # Upstream: test_ogr_vrt_4
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test3')
      lyr.SetSpatialFilterRect(90, 90, 300, 300)
      self._assert_field_values(lyr, 'other', ['Second'])
      lyr.ResetReading()
      feat = lyr.GetNextFeature()
      self._assert_geom_matches_wkt(
          feat.GetGeometryRef(), 'POINT Z (100 200 0)'
      )
      lyr.SetSpatialFilter(None)
      self._assert_field_values(lyr, 'other', ['First', 'Second'])

  def test_attribute_filter_on_wkt_layer(self) -> None:
    # Upstream: test_ogr_vrt_5
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test3')
      self.assertEqual(lyr.SetAttributeFilter('x < 50'), ogr.OGRERR_NONE)
      self._assert_field_values(lyr, 'other', ['First'])
      lyr.ResetReading()
      feat = lyr.GetNextFeature()
      self._assert_geom_matches_wkt(
          feat.GetGeometryRef(), 'POINT Z (12.5 17 1.2)'
      )
      self.assertEqual(lyr.SetAttributeFilter(None), ogr.OGRERR_NONE)
      self._assert_field_values(lyr, 'other', ['First', 'Second'])

  def test_get_feature_by_custom_fid(self) -> None:
    # Upstream: test_ogr_vrt_6
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test3')
      feat = lyr.GetFeature(2)
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetFID(), 2)
      self.assertEqual(feat['other'], 'Second')

  def test_src_sql_layer(self) -> None:
    # Upstream: test_ogr_vrt_7
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test4')
      self._assert_field_values(lyr, 'other', ['First', 'Second'])
      lyr.ResetReading()
      f1 = lyr.GetNextFeature()
      self.assertEqual(f1.GetFID(), 1)
      self._assert_geom_matches_wkt(
          f1.GetGeometryRef(), 'POINT Z (12.5 17 1.2)'
      )
      f2 = lyr.GetNextFeature()
      self.assertEqual(f2.GetFID(), 2)
      self._assert_geom_matches_wkt(f2.GetGeometryRef(), 'POINT Z (100 200 0)')

  def test_inline_vrt_xml_string(self) -> None:
    # Upstream: test_ogr_vrt_8
    vsi_dir = self._make_vsi_dir()
    dbf_path = str(vsi_dir / 'flat.dbf')
    _create_flat_dbf(dbf_path)
    vrt_xml = (
        '<OGRVRTDataSource>'
        '<OGRVRTLayer name="test4">'
        f'<SrcDataSource relativeToVRT="0">{dbf_path}</SrcDataSource>'
        '<SrcSQL>SELECT * FROM flat</SrcSQL>'
        '<FID>fid</FID>'
        '<GeometryType>wkbPoint</GeometryType>'
        '<GeometryField encoding="PointFromColumns" x="x" y="y" z="z"/>'
        '</OGRVRTLayer>'
        '</OGRVRTDataSource>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test4')
      self._assert_field_values(lyr, 'other', ['First', 'Second'])
      lyr.ResetReading()
      f1 = lyr.GetNextFeature()
      self.assertEqual(f1.GetFID(), 1)
      self._assert_geom_matches_wkt(
          f1.GetGeometryRef(), 'POINT Z (12.5 17 1.2)'
      )
      f2 = lyr.GetNextFeature()
      self.assertEqual(f2.GetFID(), 2)
      self._assert_geom_matches_wkt(f2.GetGeometryRef(), 'POINT Z (100 200 0)')

  def test_attribute_filter_passthrough_with_shared_datasource(self) -> None:
    # Upstream: test_ogr_vrt_9
    vsi_dir = self._make_vsi_dir()
    vrt_path, dbf_path = _create_vrt_test_files(vsi_dir)
    # OpenEx injects GDAL_OF_VERBOSE_ERROR when exceptions are enabled,
    # whereas OGRVRTLayer opens shared="TRUE" sources without it. Because
    # GDALSharedDatasetEqualFunc compares nOpenFlags verbatim, temporarily
    # disable exceptions so the shared handle matches. Keep sub_ds open until
    # after ds closes because closing a shared handle while RefCount > 1 runs
    # GDALDatasetRunCloseWithoutDestroying under the VRT layer.
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test3')
      self.assertEqual(
          lyr.SetAttributeFilter("other = 'Second'"), ogr.OGRERR_NONE
      )
      lyr.ResetReading()
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['other'], 'Second')

      with (
          gdal.ExceptionMgr(useExceptions=False),
          ogr.ExceptionMgr(useExceptions=False),
      ):
        sub_ds = gdal.OpenEx(
            dbf_path, gdal.OF_VECTOR | gdal.OF_READONLY | gdal.OF_SHARED
        )
      assert sub_ds is not None
      self.addCleanup(sub_ds.Close)
      sub_layer = sub_ds.GetLayerByName('flat')
      sub_layer.ResetReading()
      self.assertEqual(sub_layer.GetFeatureCount(), 1)

      self.assertEqual(lyr.SetAttributeFilter(None), ogr.OGRERR_NONE)
      # OGRVRTLayer clears poSrcLayer's filter lazily on the next read/count.
      self.assertEqual(lyr.GetFeatureCount(), 2)
      self.assertEqual(sub_layer.GetFeatureCount(), 2)

  def test_layer_capabilities_match_source(self) -> None:
    # Upstream: test_ogr_vrt_10
    vsi_dir = self._make_vsi_dir()
    shp_path = str(vsi_dir / 'testpoly.shp')
    _create_points_shapefile(
        shp_path,
        [(0.0, 0.0), (1.0, 1.0), (2.0, 2.0)],
        create_spatial_index=True,
    )
    vrt_xml = (
        '<OGRVRTDataSource>'
        '<OGRVRTLayer name="test">'
        f'<SrcDataSource relativeToVRT="0">{shp_path}</SrcDataSource>'
        '<SrcLayer>testpoly</SrcLayer>'
        '</OGRVRTLayer>'
        '</OGRVRTDataSource>'
    )
    with (
        gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds,
        gdal.OpenEx(shp_path, gdal.OF_VECTOR | gdal.OF_READONLY) as src_ds,
    ):
      vrt_lyr = vrt_ds.GetLayerByName('test')
      src_lyr = src_ds.GetLayer(0)
      for cap in (
          ogr.OLCFastFeatureCount,
          ogr.OLCFastGetExtent,
          ogr.OLCRandomRead,
          ogr.OLCFastSpatialFilter,
      ):
        with self.subTest(capability=cap):
          self.assertEqual(src_lyr.TestCapability(cap), 1)
          self.assertEqual(vrt_lyr.TestCapability(cap), 1)

  def test_write_point_from_columns_style_and_spatial_subquery(self) -> None:
    # Upstream: test_ogr_vrt_11
    vsi_dir = self._make_vsi_dir()
    csv_path = _write_vsi_text(
        vsi_dir / 'test.csv',
        'x,val1,y,val2,style\n'
        '2,"val11",49,"val12","PEN(c:#FF0000,w:5pt,p:""2px 1pt"")"\n',
    )
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="0">{csv_path}</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    <GeometryField encoding="PointFromColumns" x="x" y="y"
                   reportSrcColumn="false"/>
    <Style>style</Style>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_UPDATE) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      defn = vrt_lyr.GetLayerDefn()
      self.assertEqual(defn.GetFieldCount(), 3)
      self.assertEqual(defn.GetFieldDefn(0).GetNameRef(), 'val1')
      self.assertEqual(defn.GetFieldDefn(1).GetNameRef(), 'val2')
      self.assertEqual(defn.GetFieldDefn(2).GetNameRef(), 'style')

      feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(
          feat.GetStyleString(), 'PEN(c:#FF0000,w:5pt,p:"2px 1pt")'
      )

      new_feat = ogr.Feature(defn)
      new_feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (3 50)'))
      new_feat['val1'] = 'val21'
      self.assertEqual(vrt_lyr.CreateFeature(new_feat), ogr.OGRERR_NONE)

      vrt_lyr.ResetReading()
      got_feat = vrt_lyr.GetFeature(2)
      self.assertIsNotNone(got_feat)
      self._assert_geom_matches_wkt(got_feat.GetGeometryRef(), 'POINT (3 50)')
      self.assertEqual(got_feat['val1'], 'val21')

      # Without .csvt, x and y are strings so spatial subquery emits a warning.
      gdal.ErrorReset()
      with gdal.quiet_warnings():
        vrt_lyr.SetSpatialFilterRect(0, 40, 10, 49.5)
        ret = vrt_lyr.GetFeatureCount()
      self.assertIn('not declared as numeric fields', gdal.GetLastErrorMsg())
      self.assertEqual(ret, 1)

    _write_vsi_text(vsi_dir / 'test.csvt', 'Real,String,Real,String\n')
    gdal.ErrorReset()
    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      # Verify the CreateFeature mutation persisted across close/reopen.
      self.assertEqual(vrt_lyr.GetFeatureCount(), 2)
      persisted = vrt_lyr.GetFeature(2)
      self.assertIsNotNone(persisted)
      self._assert_geom_matches_wkt(persisted.GetGeometryRef(), 'POINT (3 50)')
      self.assertEqual(persisted['val1'], 'val21')

      vrt_lyr.SetSpatialFilterRect(0, 40, 10, 49.5)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)
      self.assertEqual(gdal.GetLastErrorMsg(), '')

      self.assertEqual(vrt_lyr.SetAttributeFilter('1 = 1'), ogr.OGRERR_NONE)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)
      self.assertEqual(vrt_lyr.SetAttributeFilter('1 = 0'), ogr.OGRERR_NONE)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 0)

  @parameterized.named_parameters(
      dict(
          testcase_name='wkt',
          encoding='WKT',
          col_name='wkt_geom',
          header='wkt_geom,val1,val2\nPOINT (2 49),"val11","val12"\n',
          expected_fid=2,
      ),
      dict(
          testcase_name='wkb',
          encoding='WKB',
          col_name='wkb_geom',
          header='wkb_geom,val1,val2\n',
          expected_fid=1,
      ),
  )
  def test_write_encoded_geometry(
      self, encoding: str, col_name: str, header: str, expected_fid: int
  ) -> None:
    # Upstream: test_ogr_vrt_12, test_ogr_vrt_13
    vsi_dir = self._make_vsi_dir()
    csv_path = _write_vsi_text(vsi_dir / 'test.csv', header)
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="0">{csv_path}</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    <GeometryField encoding="{encoding}" field="{col_name}"/>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_UPDATE) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      feat = ogr.Feature(vrt_lyr.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (3 50)'))
      feat['val1'] = 'val21'
      self.assertEqual(vrt_lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      got = vrt_lyr.GetFeature(expected_fid)
      self.assertIsNotNone(got)
      self._assert_geom_matches_wkt(got.GetGeometryRef(), 'POINT (3 50)')
      self.assertEqual(got['val1'], 'val21')

  def test_src_region_direct_encoding(self) -> None:
    # Upstream: test_ogr_vrt_14
    vsi_dir = self._make_vsi_dir()
    shp_path = str(vsi_dir / 'test.shp')
    _create_points_shapefile(
        shp_path,
        [(-10.0, 49.0), (-10.0, 49.0), (2.0, 49.0), (-10.0, 49.0)],
        create_spatial_index=True,
    )
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="mytest">
    <SrcDataSource relativeToVRT="0">{shp_path}</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    <SrcRegion>POLYGON((0 40,0 50,10 50,10 40,0 40))</SrcRegion>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('mytest')
      self.assertEqual(vrt_lyr.TestCapability(ogr.OLCFastSpatialFilter), 1)
      self.assertEqual(vrt_lyr.GetExtent(), (2.0, 2.0, 49.0, 49.0))
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)

      feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetFID(), 2)
      self._assert_geom_matches_wkt(feat.GetGeometryRef(), 'POINT (2 49)')

      vrt_lyr.SetSpatialFilterRect(1, 41, 3, 49.5)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)

      vrt_lyr.SetSpatialFilterRect(1, 41, 3, 48.5)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 0)

      vrt_lyr.SetSpatialFilter(None)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)

  @parameterized.named_parameters(
      dict(
          testcase_name='wkt',
          csv_data=(
              'wkt_geom,val1,val2\n'
              'POINT (-10 49),,\n'
              'POINT (-10 49),,\n'
              'POINT (2 49),,\n'
              'POINT (-10 49),,\n'
          ),
          csvt_data=None,
          geom_field_xml='<GeometryField encoding="WKT" field="wkt_geom"/>',
      ),
      dict(
          testcase_name='point_from_columns',
          csv_data='x,y,val1,val2\n-10,49,,\n-10,49,,\n2,49,,\n-10,49,,\n',
          csvt_data='Real,Real,String,String\n',
          geom_field_xml=(
              '<GeometryField encoding="PointFromColumns" x="x" y="y"/>'
          ),
      ),
  )
  def test_src_region_non_direct_encodings(
      self, csv_data: str, csvt_data: str | None, geom_field_xml: str
  ) -> None:
    # Upstream: test_ogr_vrt_15, test_ogr_vrt_16
    vsi_dir = self._make_vsi_dir()
    csv_path = _write_vsi_text(vsi_dir / 'test.csv', csv_data)
    if csvt_data is not None:
      _write_vsi_text(vsi_dir / 'test.csvt', csvt_data)

    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="0">{csv_path}</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    {geom_field_xml}
    <SrcRegion>POLYGON((0 40,0 50,10 50,10 40,0 40))</SrcRegion>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      self.assertEqual(vrt_lyr.TestCapability(ogr.OLCFastSpatialFilter), 0)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)

      feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetFID(), 3)
      self._assert_geom_matches_wkt(feat.GetGeometryRef(), 'POINT (2 49)')

      vrt_lyr.SetSpatialFilterRect(1, 41, 3, 49.5)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)

      vrt_lyr.SetSpatialFilterRect(1, 41, 3, 48.5)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 0)

      vrt_lyr.SetSpatialFilter(None)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)

  def test_src_region_clip(self) -> None:
    # Upstream: test_ogr_vrt_SrcRegion_clip
    vsi_dir = self._make_vsi_dir()
    csv_path = _write_vsi_text(
        vsi_dir / 'test.csv',
        'wkt_geom,val1,val2\n"LINESTRING (-1 0.5,1.5 0.5)",,\n',
    )
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="0">{csv_path}</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    <GeometryField encoding="WKT" field="wkt_geom"/>
    <SrcRegion clip="true">POLYGON((0 0,0 1,1 1,1 0,0 0))</SrcRegion>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self._assert_geom_matches_wkt(
          feat.GetGeometryRef(), 'LINESTRING (0 0.5,1 0.5)'
      )

  def test_explicit_field_definitions(self) -> None:
    # Upstream: test_ogr_vrt_17
    pm_csv = self._get_testdata_path('prime_meridian.csv')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="0">{pm_csv}</SrcDataSource>
    <SrcLayer>prime_meridian</SrcLayer>
    <Field name="pm_code" src="PRIME_MERIDIAN_CODE" type="integer" width="4"/>
    <Field name="prime_meridian_name" width="24"/>
    <Field name="new_col" type="Real" width="12" precision="3"/>
    <Field name="DEPRECATED" type="Integer" subtype="Boolean"/>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      defn = vrt_lyr.GetLayerDefn()
      self.assertEqual(defn.GetFieldCount(), 4)

      expected_fields = (
          ('pm_code', ogr.OFTInteger, ogr.OFSTNone, 4, 0),
          ('prime_meridian_name', ogr.OFTString, ogr.OFSTNone, 24, 0),
          ('new_col', ogr.OFTReal, ogr.OFSTNone, 12, 3),
          ('DEPRECATED', ogr.OFTInteger, ogr.OFSTBoolean, 0, 0),
      )
      for idx, (name, ftype, subtype, width, prec) in enumerate(
          expected_fields
      ):
        with self.subTest(field=name):
          fld = defn.GetFieldDefn(idx)
          self.assertEqual(fld.GetName(), name)
          self.assertEqual(fld.GetType(), ftype)
          self.assertEqual(fld.GetSubType(), subtype)
          self.assertEqual(fld.GetWidth(), width)
          self.assertEqual(fld.GetPrecision(), prec)

      feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField(0), 8901)
      self.assertEqual(feat.GetField(1), 'Greenwich')
      self.assertIsNone(feat.GetField(2))

  def test_attribute_filter_not_passed_through_explicit_fields(self) -> None:
    # Upstream: test_ogr_vrt_18
    pm_csv = self._get_testdata_path('prime_meridian.csv')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="0">{pm_csv}</SrcDataSource>
    <SrcLayer>prime_meridian</SrcLayer>
    <Field name="pm_code" src="PRIME_MERIDIAN_CODE" type="integer" width="4"/>
    <Field name="prime_meridian_name" width="24"/>
    <Field name="new_col" type="Real" width="12" precision="3"/>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('test')
      self.assertEqual(
          vrt_lyr.SetAttributeFilter('pm_code=8904'), ogr.OGRERR_NONE
      )
      feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField(0), 8904)
      self.assertIsNone(vrt_lyr.GetNextFeature())

  @parameterized.named_parameters(
      dict(testcase_name='optimized_path', layer_name='poly', src_layer=''),
      dict(
          testcase_name='non_optimized_path',
          layer_name='mypoly',
          src_layer='<SrcLayer>poly</SrcLayer>',
      ),
  )
  def test_poly_vrt_optimized_and_nonoptimized(
      self, layer_name: str, src_layer: str
  ) -> None:
    # Upstream: test_ogr_vrt_19_optimized, test_ogr_vrt_19_nonoptimized
    # test_ogrsf is not built in google3, so verify core read/filter/extent
    # behavior directly against poly.shp in both paths.
    poly_shp = self._get_testdata_path('shape/poly/poly.shp')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="{layer_name}">
    <SrcDataSource relativeToVRT="0" shared="1">{poly_shp}</SrcDataSource>
    {src_layer}
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with (
        gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds,
        gdal.OpenEx(poly_shp, gdal.OF_VECTOR | gdal.OF_READONLY) as src_ds,
    ):
      vrt_lyr = vrt_ds.GetLayerByName(layer_name)
      src_lyr = src_ds.GetLayer(0)
      expected_count = src_lyr.GetFeatureCount()
      self.assertEqual(expected_count, 10)
      self.assertEqual(vrt_lyr.GetFeatureCount(), expected_count)
      self.assertEqual(vrt_lyr.GetExtent(), src_lyr.GetExtent())
      self.assertEqual(
          vrt_lyr.GetLayerDefn().GetFieldCount(),
          src_lyr.GetLayerDefn().GetFieldCount(),
      )
      for cap in (ogr.OLCFastFeatureCount, ogr.OLCRandomRead):
        with self.subTest(capability=cap):
          self.assertEqual(vrt_lyr.TestCapability(cap), 1)

      vrt_lyr.ResetReading()
      src_lyr.ResetReading()
      seen = 0
      for vrt_feat, src_feat in zip(vrt_lyr, src_lyr, strict=True):
        seen += 1
        self.assertEqual(vrt_feat.GetFID(), src_feat.GetFID())
        self.assertEqual(vrt_feat['EAS_ID'], src_feat['EAS_ID'])
        self.assertAlmostEqual(vrt_feat['AREA'], src_feat['AREA'], delta=1e-6)
        self.assertTrue(
            vrt_feat.GetGeometryRef().Equals(src_feat.GetGeometryRef())
        )
      self.assertEqual(seen, expected_count)

      # Spatial and attribute filtering parity against poly.shp.
      vrt_lyr.SetSpatialFilterRect(479000, 4763000, 480500, 4765000)
      src_lyr.SetSpatialFilterRect(479000, 4763000, 480500, 4765000)
      self.assertGreater(vrt_lyr.GetFeatureCount(), 0)
      self.assertEqual(vrt_lyr.GetFeatureCount(), src_lyr.GetFeatureCount())
      vrt_lyr.SetSpatialFilter(None)
      src_lyr.SetSpatialFilter(None)

      self.assertEqual(
          vrt_lyr.SetAttributeFilter('EAS_ID = 170'), ogr.OGRERR_NONE
      )
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)
      vrt_lyr.ResetReading()
      filtered_feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(filtered_feat)
      self.assertEqual(filtered_feat['EAS_ID'], 170)
      self.assertEqual(vrt_lyr.SetAttributeFilter(None), ogr.OGRERR_NONE)

      self.assertEqual(vrt_lyr.SetIgnoredFields(['AREA']), ogr.OGRERR_NONE)
      vrt_lyr.ResetReading()
      ignored_feat = vrt_lyr.GetNextFeature()
      self.assertIsNotNone(ignored_feat)
      self.assertFalse(ignored_feat.IsFieldSet('AREA'))
      self.assertTrue(ignored_feat.IsFieldSet('EAS_ID'))
      self.assertEqual(vrt_lyr.SetIgnoredFields([]), ogr.OGRERR_NONE)

  def test_direct_geometry_filters_and_update(self) -> None:
    # Upstream: test_ogr_vrt_20
    vsi_dir = self._make_vsi_dir()
    shp_path = str(vsi_dir / 'test.shp')
    _create_points_shapefile(
        shp_path,
        [(-10.0, 45.0), (-10.0, 49.0), (2.0, 49.0), (-10.0, 49.0)],
        create_spatial_index=True,
    )
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="mytest">
    <SrcDataSource relativeToVRT="0">{shp_path}</SrcDataSource>
    <SrcLayer>test</SrcLayer>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_UPDATE) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('mytest')
      self.assertEqual(vrt_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
      self.assertEqual(vrt_lyr.TestCapability(ogr.OLCFastSpatialFilter), 1)
      self.assertEqual(vrt_lyr.TestCapability(ogr.OLCFastGetExtent), 1)
      self.assertEqual(vrt_lyr.GetExtent(), (-10.0, 2.0, 45.0, 49.0))
      self.assertEqual(vrt_lyr.GetFeatureCount(), 4)

      vrt_lyr.SetSpatialFilterRect(1, 48.5, 3, 49.5)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 1)
      self.assertEqual(vrt_lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
      self.assertEqual(vrt_lyr.TestCapability(ogr.OLCFastGetExtent), 1)
      self.assertEqual(vrt_lyr.GetExtent(), (-10.0, 2.0, 45.0, 49.0))

      vrt_lyr.SetSpatialFilterRect(1, 48, 3, 48.5)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 0)

      vrt_lyr.SetSpatialFilter(None)
      self.assertEqual(vrt_lyr.GetFeatureCount(), 4)

      vrt_lyr.ResetReading()
      feat = vrt_lyr.GetNextFeature()
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (1 2)'))
      self.assertEqual(vrt_lyr.SetFeature(feat), ogr.OGRERR_NONE)

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      vrt_lyr = vrt_ds.GetLayerByName('mytest')
      feat = vrt_lyr.GetNextFeature()
      self._assert_geom_matches_wkt(feat.GetGeometryRef(), 'POINT (1 2)')

  def test_lazy_initialization_valid_layer(self) -> None:
    # Upstream: test_ogr_vrt_21
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)

    def _check_next_feature(lyr: ogr.Layer) -> None:
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetFID(), 1)
      self.assertEqual(feat['other'], 'First')

    def _check_get_feature(lyr: ogr.Layer) -> None:
      feat = lyr.GetFeature(1)
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetFID(), 1)
      self.assertEqual(feat['other'], 'First')

    checks: tuple[tuple[str, Callable[[ogr.Layer], None]], ...] = (
        ('GetName', lambda lyr: self.assertEqual(lyr.GetName(), 'test3')),
        (
            'GetGeomType',
            lambda lyr: self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint),
        ),
        (
            'GetSpatialRef',
            lambda lyr: self.assertIn('84', lyr.GetSpatialRef().ExportToWkt()),
        ),
        ('ResetReading', lambda lyr: lyr.ResetReading()),
        ('GetNextFeature', _check_next_feature),
        ('GetFeature', _check_get_feature),
        (
            'GetFeatureCount',
            lambda lyr: self.assertEqual(lyr.GetFeatureCount(), 2),
        ),
        (
            'SetNextByIndex',
            lambda lyr: self.assertEqual(
                lyr.SetNextByIndex(1), ogr.OGRERR_NONE
            ),
        ),
        (
            'GetLayerDefn',
            lambda lyr: self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 6),
        ),
        (
            'SetAttributeFilter',
            lambda lyr: self.assertEqual(
                lyr.SetAttributeFilter(''), ogr.OGRERR_NONE
            ),
        ),
        ('SetSpatialFilter', lambda lyr: lyr.SetSpatialFilter(None)),
        (
            'TestCapability',
            lambda lyr: self.assertEqual(
                lyr.TestCapability(ogr.OLCFastFeatureCount), 1
            ),
        ),
        (
            'GetExtent',
            lambda lyr: self.assertEqual(
                lyr.GetExtent(), (12.5, 100.0, 17.0, 200.0)
            ),
        ),
        (
            'GetFIDColumn',
            lambda lyr: self.assertEqual(lyr.GetFIDColumn(), 'fid'),
        ),
        (
            'SyncToDisk',
            lambda lyr: self.assertEqual(lyr.SyncToDisk(), ogr.OGRERR_NONE),
        ),
    )
    for method_name, check_fn in checks:
      with (
          self.subTest(method=method_name),
          gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
      ):
        check_fn(ds.GetLayerByName('test3'))

    feat = ogr.Feature(ogr.FeatureDefn())
    for method_name, op in (
        ('CreateFeature', lambda lyr: lyr.CreateFeature(feat)),
        ('SetFeature', lambda lyr: lyr.SetFeature(feat)),
        ('DeleteFeature', lambda lyr: lyr.DeleteFeature(0)),
    ):
      with (
          self.subTest(method=method_name),
          gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
      ):
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(RuntimeError, r'read-only'),
        ):
          op(ds.GetLayerByName('test3'))

  def test_lazy_initialization_invalid_layer(self) -> None:
    # Upstream: test_ogr_vrt_22
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test5')
      self.assertEqual(lyr.GetName(), 'test5')
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint)
      lyr.ResetReading()

    # First call triggering FullInitialize() raises Missing SrcDataSource.
    triggers: tuple[tuple[str, Callable[[ogr.Layer], object]], ...] = (
        ('GetSpatialRef', lambda lyr: lyr.GetSpatialRef()),
        ('GetNextFeature', lambda lyr: lyr.GetNextFeature()),
        ('GetFeature', lambda lyr: lyr.GetFeature(1)),
        ('GetFeatureCount', lambda lyr: lyr.GetFeatureCount()),
        ('SetNextByIndex', lambda lyr: lyr.SetNextByIndex(1)),
        ('GetLayerDefn', lambda lyr: lyr.GetLayerDefn()),
        ('SetAttributeFilter', lambda lyr: lyr.SetAttributeFilter('')),
        ('SetSpatialFilter', lambda lyr: lyr.SetSpatialFilter(None)),
        (
            'TestCapability',
            lambda lyr: lyr.TestCapability(ogr.OLCFastFeatureCount),
        ),
        ('GetFIDColumn', lambda lyr: lyr.GetFIDColumn()),
        ('GetExtent', lambda lyr: lyr.GetExtent()),
        (
            'CreateFeature',
            lambda lyr: lyr.CreateFeature(ogr.Feature(ogr.FeatureDefn())),
        ),
        (
            'SetFeature',
            lambda lyr: lyr.SetFeature(ogr.Feature(ogr.FeatureDefn())),
        ),
        ('DeleteFeature', lambda lyr: lyr.DeleteFeature(0)),
        ('SyncToDisk', lambda lyr: lyr.SyncToDisk()),
        ('StartTransaction', lambda lyr: lyr.StartTransaction()),
        ('CommitTransaction', lambda lyr: lyr.CommitTransaction()),
        ('RollbackTransaction', lambda lyr: lyr.RollbackTransaction()),
    )
    for method_name, trigger_fn in triggers:
      with (
          self.subTest(method=method_name),
          gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
      ):
        lyr = ds.GetLayerByName('test5')
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(RuntimeError, r'Missing SrcDataSource'),
        ):
          trigger_fn(lyr)

    # Once FullInitialize() has already failed, subsequent calls see empty
    # fallback state or OGRERR_FAILURE.
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test5')
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'Missing SrcDataSource'),
      ):
        lyr.GetLayerDefn()
      self.assertIsNone(lyr.GetSpatialRef())
      self.assertIsNone(lyr.GetNextFeature())
      self.assertIsNone(lyr.GetFeature(1))
      self.assertEqual(lyr.GetFeatureCount(), 0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 0)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
      self.assertEqual(lyr.GetFIDColumn(), '')

  @parameterized.named_parameters(
      dict(
          testcase_name='non_shared',
          shared_attr='',
          error_regex=r'\[32 times\] a VRT!',
      ),
      dict(
          testcase_name='shared',
          shared_attr=' shared="1"',
          error_regex=r'Cyclic VRT opening detected!',
      ),
  )
  def test_anti_recursion_layer(
      self, shared_attr: str, error_regex: str
  ) -> None:
    # Upstream: test_ogr_vrt_23, test_ogr_vrt_24
    vsi_dir = self._make_vsi_dir()
    rec1_path = str(vsi_dir / 'rec1.vrt')
    rec2_path = str(vsi_dir / 'rec2.vrt')
    _write_vsi_text(
        pathlib.Path(rec1_path),
        f"""<OGRVRTDataSource>
  <OGRVRTLayer name="rec1">
    <SrcDataSource{shared_attr}>{rec2_path}</SrcDataSource>
    <SrcLayer>rec2</SrcLayer>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )
    _write_vsi_text(
        pathlib.Path(rec2_path),
        f"""<OGRVRTDataSource>
  <OGRVRTLayer name="rec2">
    <SrcDataSource{shared_attr}>{rec1_path}</SrcDataSource>
    <SrcLayer>rec1</SrcLayer>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )

    with gdal.OpenEx(rec1_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertIsNotNone(ds)
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, error_regex),
      ):
        ds.GetLayer(0).GetLayerDefn()
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

  def test_anti_recursion_indirect_self_cycle_shared(self) -> None:
    # Upstream: test_ogr_vrt_24 (rec1 -> rec2 -> rec2)
    vsi_dir = self._make_vsi_dir()
    rec1_path = str(vsi_dir / 'rec1.vrt')
    rec2_path = str(vsi_dir / 'rec2.vrt')
    _write_vsi_text(
        pathlib.Path(rec1_path),
        f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource shared="1">{rec2_path}</SrcDataSource>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )
    _write_vsi_text(
        pathlib.Path(rec2_path),
        f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource shared="1">{rec2_path}</SrcDataSource>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )

    with gdal.OpenEx(rec1_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'Cyclic VRT opening detected!'),
      ):
        ds.GetLayer(0).GetLayerDefn()
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

  def test_get_fid_column(self) -> None:
    # Upstream: test_ogr_vrt_25
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetLayerByName('test3').GetFIDColumn(), 'fid')
      self.assertEqual(ds.GetLayerByName('test6').GetFIDColumn(), 'fid')
      self.assertEqual(ds.GetLayerByName('test7').GetFIDColumn(), 'bar')
      self.assertEqual(ds.GetLayerByName('test2').GetFIDColumn(), '')
      self.assertEqual(ds.GetLayerByName('test8').GetFIDColumn(), '')

  def test_transactions_on_sqlite_source(self) -> None:
    # Upstream: test_ogr_vrt_26
    vsi_dir = self._make_vsi_dir()
    db_path = str(vsi_dir / 'ogr_vrt_26.db')
    sqlite_drv = gdal.GetDriverByName('SQLite')
    self.assertIsNotNone(sqlite_drv)

    with sqlite_drv.Create(db_path, 0, 0, 0, gdal.GDT_Unknown) as sqlite_ds:
      lyr = sqlite_ds.CreateLayer('test')
      self.assertEqual(
          lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString)), ogr.OGRERR_NONE
      )

    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource>{db_path}</SrcDataSource>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_UPDATE) as vrt_ds:
      lyr = vrt_ds.GetLayer(0)
      self.assertEqual(lyr.TestCapability(ogr.OLCTransactions), 1)

      self.assertEqual(lyr.StartTransaction(), ogr.OGRERR_NONE)
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetField(0, 'foo')
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      self.assertEqual(lyr.RollbackTransaction(), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetFeatureCount(), 0)

      self.assertEqual(lyr.StartTransaction(), ogr.OGRERR_NONE)
      feat2 = ogr.Feature(lyr.GetLayerDefn())
      feat2.SetField(0, 'bar')
      self.assertEqual(lyr.CreateFeature(feat2), ogr.OGRERR_NONE)
      self.assertEqual(lyr.CommitTransaction(), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetFeatureCount(), 1)

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as vrt_ds:
      self._assert_field_values(vrt_ds.GetLayer(0), 'foo', ['bar'])

  def test_shape_binary_geometry_encoding(self) -> None:
    # Upstream: test_ogr_vrt_27
    vsi_dir = self._make_vsi_dir()
    pt_hex = struct.pack('<i2d', 1, 1.0, 2.0).hex().upper()
    line_hex = (
        struct.pack(
            '<i4d3i4d', 3, 3.0, 4.0, 5.0, 6.0, 1, 2, 0, 3.0, 4.0, 5.0, 6.0
        )
        .hex()
        .upper()
    )
    poly_hex = (
        struct.pack(
            '<i4d3i10d',
            5,
            0.0,
            0.0,
            1.0,
            1.0,
            1,
            5,
            0,
            0.0,
            0.0,
            0.0,
            1.0,
            1.0,
            1.0,
            1.0,
            0.0,
            0.0,
            0.0,
        )
        .hex()
        .upper()
    )
    csv_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_27.csv',
        'dummy,shapebin\n'
        f'"dummy","{pt_hex}"\n'
        f'"dummy","{line_hex}"\n'
        f'"dummy","{poly_hex}"\n',
    )
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="ogr_vrt_27">
    <SrcDataSource relativeToVRT="0" shared="0">{csv_path}</SrcDataSource>
    <GeometryField encoding="shape" field="shapebin"/>
    <Field name="foo"/>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      expected_wkts = (
          'POINT (1 2)',
          'LINESTRING (3 4,5 6)',
          'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
      )
      features = list(lyr)
      self.assertLen(features, len(expected_wkts))
      for feat, expected_wkt in zip(features, expected_wkts, strict=True):
        self._assert_geom_matches_wkt(feat.GetGeometryRef(), expected_wkt)

  def test_invalid_vrt_inputs(self) -> None:
    # Upstream: test_ogr_vrt_28
    vsi_dir = self._make_vsi_dir()
    poly_shp = self._get_testdata_path('shape/poly/poly.shp')

    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(RuntimeError, r"doesn't have matching <foo>"),
    ):
      with gdal.OpenEx(
          '<OGRVRTDataSource></foo>', gdal.OF_VECTOR | gdal.OF_READONLY
      ) as _:
        pass

    bad_root = _write_vsi_text(
        vsi_dir / 'invalid_root.vrt',
        '<bla><OGRVRTDataSource></OGRVRTDataSource></bla>',
    )
    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(RuntimeError, r'<OGRVRTDataSource>'),
    ):
      with gdal.OpenEx(bad_root, gdal.OF_VECTOR | gdal.OF_READONLY) as _:
        pass

    nonexistent_src = str(vsi_dir / 'nonexistent_file')
    invalid_layer_specs = (
        ('missing_src', '<OGRVRTLayer name="foo"/>'),
        ('empty_src', '<OGRVRTLayer name="foo"><SrcDataSource/></OGRVRTLayer>'),
        (
            'nonexistent_src',
            (
                '<OGRVRTLayer name="foo">'
                f'<SrcDataSource>{nonexistent_src}</SrcDataSource>'
                '</OGRVRTLayer>'
            ),
        ),
        (
            'wrong_layer_name',
            (
                '<OGRVRTLayer'
                f' name="foo"><SrcDataSource>{poly_shp}</SrcDataSource>'
                '</OGRVRTLayer>'
            ),
        ),
        (
            'missing_src_layer',
            (
                '<OGRVRTLayer'
                f' name="foo"><SrcDataSource>{poly_shp}</SrcDataSource>'
                '<SrcLayer>bar</SrcLayer></OGRVRTLayer>'
            ),
        ),
        (
            'invalid_src_sql',
            (
                '<OGRVRTLayer'
                f' name="foo"><SrcDataSource>{poly_shp}</SrcDataSource>'
                '<SrcSQL>bar</SrcSQL></OGRVRTLayer>'
            ),
        ),
        (
            'missing_fid_col',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource><FID>foo</FID>'
                '</OGRVRTLayer>'
            ),
        ),
        (
            'missing_style_col',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource><Style>foo</Style>'
                '</OGRVRTLayer>'
            ),
        ),
        (
            'invalid_geom_encoding',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<GeometryField encoding="foo"/></OGRVRTLayer>'
            ),
        ),
        (
            'point_from_cols_no_xy',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<GeometryField encoding="PointFromColumns"/></OGRVRTLayer>'
            ),
        ),
        (
            'wkt_no_field',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<GeometryField encoding="WKT"/></OGRVRTLayer>'
            ),
        ),
        (
            'field_missing_name',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource><Field/></OGRVRTLayer>'
            ),
        ),
        (
            'field_bad_type',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<Field name="AREA" type="foo"/></OGRVRTLayer>'
            ),
        ),
        (
            'field_bad_subtype',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<Field name="AREA" subtype="foo"/></OGRVRTLayer>'
            ),
        ),
        (
            'field_incompatible_subtype',
            (
                '<OGRVRTLayer'
                f' name="poly"><SrcDataSource>{poly_shp}</SrcDataSource><Field'
                ' name="AREA" type="Real" subtype="Boolean"/></OGRVRTLayer>'
            ),
        ),
        (
            'field_negative_width',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<Field name="AREA" width="-1"/></OGRVRTLayer>'
            ),
        ),
        (
            'field_negative_precision',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<Field name="AREA" precision="-1"/></OGRVRTLayer>'
            ),
        ),
        (
            'field_missing_src',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<Field name="AREA" src="foo"/></OGRVRTLayer>'
            ),
        ),
        (
            'invalid_src_region',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<SrcRegion>foo</SrcRegion></OGRVRTLayer>'
            ),
        ),
        (
            'missing_geom_src_field',
            (
                '<OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<GeometryField name="foo" field="bar"/></OGRVRTLayer>'
            ),
        ),
    )
    layers_xml = '\n  '.join(xml for _, xml in invalid_layer_specs)
    invalid_layers_vrt = _write_vsi_text(
        vsi_dir / 'invalid.vrt',
        f'<OGRVRTDataSource>\n  {layers_xml}\n</OGRVRTDataSource>',
    )
    hard_error_cases = (
        (0, 'missing_src', r'Missing SrcDataSource for layer foo'),
        (1, 'empty_src', r'Missing SrcDataSource for layer foo'),
        (
            2,
            'nonexistent_src',
            rf"Failed to open datasource `{nonexistent_src}'",
        ),
        (3, 'wrong_layer_name', r"Failed to find layer 'foo' on datasource"),
        (4, 'missing_src_layer', r"Failed to find layer 'bar' on datasource"),
        (
            5,
            'invalid_src_sql',
            r'SQL statement failed, or returned no layer result',
        ),
        (6, 'missing_fid_col', r"Unable to identify FID field 'foo'"),
        (7, 'missing_style_col', r"Unable to identify Style field 'foo'"),
        (8, 'invalid_geom_encoding', r'encoding="foo" not recognised'),
        (9, 'point_from_cols_no_xy', r'Unable to identify source X or Y field'),
        (10, 'wkt_no_field', r'Unable to identify source field'),
        (11, 'field_missing_name', r'Unable to identify Field name'),
        (12, 'field_bad_type', r"Unable to identify Field type 'foo'"),
        (13, 'field_bad_subtype', r"Unable to identify Field subtype 'foo'"),
        (
            14,
            'field_incompatible_subtype',
            r"Invalid subtype 'Boolean' for type 'Real'",
        ),
        (15, 'field_negative_width', r'Invalid width for field AREA'),
        (16, 'field_negative_precision', r'Invalid precision for field AREA'),
        (17, 'field_missing_src', r"Unable to find source field 'foo'"),
        (
            19,
            'missing_geom_src_field',
            r"Unable to identify source geometry field 'bar'",
        ),
    )
    with gdal.OpenEx(
        invalid_layers_vrt, gdal.OF_VECTOR | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(ds.GetLayerCount(), 20)
      for idx, case_name, err_regex in hard_error_cases:
        with self.subTest(case=case_name):
          lyr = ds.GetLayer(idx)
          with (
              gdal.quiet_errors(),
              self.assertRaisesRegex(RuntimeError, err_regex),
          ):
            lyr.GetNextFeature()

      with self.subTest(case='invalid_src_region'):
        # Keep Feature alive until after GetLastErrorMsg() so Feature.__del__
        # does not reset CPL error state under ExceptionMgr.
        lyr = ds.GetLayer(18)
        gdal.ErrorReset()
        with gdal.quiet_errors():
          feat = lyr.GetNextFeature()
          err_type = gdal.GetLastErrorType()
          err_msg = gdal.GetLastErrorMsg()
        self.assertEqual(err_type, gdal.CE_Warning)
        self.assertRegex(err_msg, r'Ignoring SrcRegion')
        self.assertIsNotNone(feat)
        del feat

    for bad_layer_xml, err_regex in (
        (
            '<OGRVRTDataSource><OGRVRTLayer/></OGRVRTDataSource>',
            r'Missing name attribute',
        ),
        (
            (
                '<OGRVRTDataSource><OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<GeometryType>foo</GeometryType>'
                '</OGRVRTLayer></OGRVRTDataSource>'
            ),
            r'GeometryType foo not recognised',
        ),
        (
            (
                '<OGRVRTDataSource><OGRVRTLayer name="poly">'
                f'<SrcDataSource>{poly_shp}</SrcDataSource>'
                '<LayerSRS>foo</LayerSRS>'
                '</OGRVRTLayer></OGRVRTDataSource>'
            ),
            r'Failed to import LayerSRS',
        ),
    ):
      with self.subTest(expected_error=err_regex):
        self._assert_vrt_open_error(bad_layer_xml, err_regex)

  @parameterized.named_parameters(
      dict(
          testcase_name='invalid_source_layer',
          vrt_body=(
              '<OGRVRTLayer name="ogr_vrt_29">'
              '<SrcDataSource>/vsimem/non_existing.shp</SrcDataSource>'
              '</OGRVRTLayer>'
              '<TargetSRS>EPSG:32631</TargetSRS>'
          ),
          error_regex=r'Failed to import source SRS',
      ),
      dict(
          testcase_name='missing_target_srs',
          vrt_body=(
              '<OGRVRTLayer name="ogr_vrt_29">'
              '<SrcDataSource>@dummy@</SrcDataSource>'
              '<SrcLayer>@dummy@</SrcLayer>'
              '<LayerSRS>EPSG:4326</LayerSRS>'
              '</OGRVRTLayer>'
          ),
          error_regex=r'Missing TargetSRS element',
      ),
      dict(
          testcase_name='invalid_target_srs',
          vrt_body=(
              '<OGRVRTLayer name="ogr_vrt_29">'
              '<SrcDataSource>@dummy@</SrcDataSource>'
              '<SrcLayer>@dummy@</SrcLayer>'
              '<LayerSRS>EPSG:4326</LayerSRS>'
              '</OGRVRTLayer>'
              '<TargetSRS>foo</TargetSRS>'
          ),
          error_regex=r'Failed to import target SRS',
      ),
      dict(
          testcase_name='invalid_src_srs',
          vrt_body=(
              '<OGRVRTLayer name="ogr_vrt_29">'
              '<SrcDataSource>@dummy@</SrcDataSource>'
              '<SrcLayer>@dummy@</SrcLayer>'
              '</OGRVRTLayer>'
              '<SrcSRS>foo</SrcSRS>'
              '<TargetSRS>EPSG:32631</TargetSRS>'
          ),
          error_regex=r'Failed to import source SRS',
      ),
  )
  def test_warped_layer_errors(self, vrt_body: str, error_regex: str) -> None:
    # Upstream: test_ogr_vrt_29 (error cases)
    vrt_xml = (
        '<OGRVRTDataSource><OGRVRTWarpedLayer>'
        f'{vrt_body}'
        '</OGRVRTWarpedLayer></OGRVRTDataSource>'
    )
    self._assert_vrt_open_error(vrt_xml, error_regex)

  def test_warped_layer_non_spatial_source_fails(self) -> None:
    # Upstream: test_ogr_vrt_29 (non-spatial layer)
    vsi_dir = self._make_vsi_dir()
    dbf_path = str(vsi_dir / 'flat.dbf')
    _create_flat_dbf(dbf_path)
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="flat">
      <SrcDataSource>{dbf_path}</SrcDataSource>
    </OGRVRTLayer>
    <TargetSRS>EPSG:32631</TargetSRS>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>"""
    self._assert_vrt_open_error(vrt_xml, r'Failed to import source SRS')

  def test_warped_layer_reprojection_and_read_write(self) -> None:
    # Upstream: test_ogr_vrt_29
    vsi_dir = self._make_vsi_dir()
    shp_path = str(vsi_dir / 'ogr_vrt_29.shp')
    _create_point_grid_shapefile(
        shp_path,
        'ogr_vrt_29',
        x_origin=2.0,
        fields=(
            ('id', ogr.OFTInteger, lambda idx: idx),
            ('str', ogr.OFTString, lambda idx: f'{idx % 5}-{idx // 5}'),
        ),
    )

    # TargetSRS == source SRS returns source layer directly.
    same_srs_xml = f"""<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="ogr_vrt_29">
      <SrcDataSource>{shp_path}</SrcDataSource>
    </OGRVRTLayer>
    <TargetSRS>EPSG:4326</TargetSRS>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>"""
    wgs84_srs = self._srs_from_input('EPSG:4326')
    with gdal.OpenEx(same_srs_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetName(), 'ogr_vrt_29')
      self.assertIsSame(lyr.GetSpatialRef(), wgs84_srs)
      self.assertEqual(lyr.GetFeatureCount(), 25)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self._assert_geom_matches_wkt(feat.GetGeometryRef(), 'POINT (2 49)')

    # Forced extent overrides computed extent.
    forced_extent_xml = f"""<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="ogr_vrt_29">
      <SrcDataSource>{shp_path}</SrcDataSource>
    </OGRVRTLayer>
    <TargetSRS>EPSG:32631</TargetSRS>
    <ExtentXMin>426857</ExtentXMin>
    <ExtentYMin>5427475</ExtentYMin>
    <ExtentXMax>485608</ExtentXMax>
    <ExtentYMax>5516874</ExtentYMax>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>"""
    with gdal.OpenEx(
        forced_extent_xml, gdal.OF_VECTOR | gdal.OF_READONLY
    ) as ds:
      self.assertEqual(
          ds.GetLayer(0).GetExtent(), (426857, 485608, 5427475, 5516874)
      )

    vrt_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_29.vrt',
        """<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="ogr_vrt_29">
      <SrcDataSource relativeToVRT="1">ogr_vrt_29.shp</SrcDataSource>
    </OGRVRTLayer>
    <TargetSRS>EPSG:32631</TargetSRS>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>
""",
    )
    expected_srs = self._srs_from_input('EPSG:32631')

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)
      self.assertIsSame(lyr.GetSpatialRef(), expected_srs)

      # OGRWarpedLayer::GetExtent() samples a 20x20 grid along the source
      # envelope perimeter via ReprojectEnvelope(), so extent precision
      # (~1 m) is looser than point-wise reprojection (1e-4 m).
      bb = lyr.GetExtent()
      expected_bb = (426857.9877, 485607.2165, 5427475.0501, 5516873.8591)
      for got_coord, exp_coord in zip(bb, expected_bb, strict=True):
        self.assertAlmostEqual(got_coord, exp_coord, delta=1.0)

      feat = lyr.GetNextFeature()
      self.assertEqual(feat['id'], 0)
      self.assertEqual(feat['str'], '0-0')
      self._assert_geom_matches_wkt(
          feat.GetGeometryRef(),
          'POINT(426857.987717275274917 5427937.523466162383556)',
          max_error=1e-4,
      )

      feat2 = lyr.GetNextFeature()
      self.assertEqual(feat2['id'], 1)
      self.assertEqual(feat2['str'], '1-0')
      feat2.SetGeometry(None)
      self.assertEqual(lyr.SetFeature(feat2), ogr.OGRERR_NONE)
      feat2.SetGeometry(ogr.CreateGeometryFromWkt('POINT(500000 0)'))
      self.assertEqual(lyr.SetFeature(feat2), ogr.OGRERR_NONE)

      lyr.SetSpatialFilterRect(499999, -1, 500001, 1)
      lyr.ResetReading()
      filtered = lyr.GetNextFeature()
      self._assert_geom_matches_wkt(
          filtered.GetGeometryRef(), 'POINT(500000 0)', max_error=1e-4
      )

      new_feat = ogr.Feature(lyr.GetLayerDefn())
      new_feat['id'] = -99
      new_feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(500000 0)'))
      self.assertEqual(lyr.CreateFeature(new_feat), ogr.OGRERR_NONE)

      null_geom_feat = ogr.Feature(lyr.GetLayerDefn())
      null_geom_feat['id'] = -100
      self.assertEqual(lyr.CreateFeature(null_geom_feat), ogr.OGRERR_NONE)

    # Read-only operations reject writes.
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'read-only'),
      ):
        lyr.DeleteFeature(1)
      ro_feat = lyr.GetNextFeature()
      ro_feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(500000 0)'))
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'read-only'),
      ):
        lyr.SetFeature(ro_feat)
      create_feat = ogr.Feature(lyr.GetLayerDefn())
      create_feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(500000 0)'))
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'read-only'),
      ):
        lyr.CreateFeature(create_feat)

    # Verify inverse reprojection into shapefile, then add an unprojectable pt.
    with gdal.OpenEx(shp_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)
      lyr.GetNextFeature()
      f1 = lyr.GetNextFeature()
      self.assertIsNotNone(f1)
      self._assert_geom_matches_wkt(
          f1.GetGeometryRef(), 'POINT(3.0 0.0)', max_error=1e-4
      )
      self.assertEqual(lyr.SetAttributeFilter('id = -99'), ogr.OGRERR_NONE)
      lyr.ResetReading()
      f_neg99 = lyr.GetNextFeature()
      self.assertIsNotNone(f_neg99)
      self._assert_geom_matches_wkt(
          f_neg99.GetGeometryRef(), 'POINT(3.0 0.0)', max_error=1e-4
      )
      self.assertEqual(lyr.SetAttributeFilter('id = -100'), ogr.OGRERR_NONE)
      lyr.ResetReading()
      f_neg100 = lyr.GetNextFeature()
      self.assertIsNotNone(f_neg100)
      self.assertIsNone(f_neg100.GetGeometryRef())

      bad_feat = ogr.Feature(lyr.GetLayerDefn())
      bad_feat['id'] = 1000
      bad_feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(-180 91)'))
      self.assertEqual(lyr.CreateFeature(bad_feat), ogr.OGRERR_NONE)

    # Failed reprojection when reading through VRT leaves geometry None.
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.SetAttributeFilter('id = 1000'), ogr.OGRERR_NONE)
      # Disable exceptions while reading the unprojectable feature so SWIG
      # returns the Feature (with None geometry) instead of raising on the PROJ
      # failure message.
      gdal.ErrorReset()
      with (
          gdal.ExceptionMgr(useExceptions=False),
          ogr.ExceptionMgr(useExceptions=False),
          gdal.quiet_errors(),
      ):
        feat = lyr.GetNextFeature()
        err_type = gdal.GetLastErrorType()
        err_msg = gdal.GetLastErrorMsg()
      self.assertEqual(err_type, gdal.CE_Failure)
      self.assertRegex(err_msg, r'PROJ: utm: Invalid latitude')
      self.assertIsNotNone(feat)
      fid = feat.GetFID()
      self.assertIsNone(feat.GetGeometryRef())
      gdal.ErrorReset()
      with (
          gdal.ExceptionMgr(useExceptions=False),
          ogr.ExceptionMgr(useExceptions=False),
          gdal.quiet_errors(),
      ):
        feat_by_id = lyr.GetFeature(fid)
        err_type = gdal.GetLastErrorType()
        err_msg = gdal.GetLastErrorMsg()
      self.assertEqual(err_type, gdal.CE_Failure)
      self.assertRegex(err_msg, r'PROJ: utm: Invalid latitude')
      self.assertIsNotNone(feat_by_id)
      self.assertIsNone(feat_by_id.GetGeometryRef())
      self.assertEqual(lyr.DeleteFeature(fid), ogr.OGRERR_NONE)

    vrt2_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_29_2.vrt',
        """<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="ogr_vrt_29">
      <SrcDataSource relativeToVRT="1">ogr_vrt_29.vrt</SrcDataSource>
    </OGRVRTLayer>
    <TargetSRS>EPSG:4326</TargetSRS>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>
""",
    )
    with gdal.OpenEx(vrt2_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT(-180 91)'))
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'PROJ: utm: Invalid latitude'),
      ):
        lyr.SetFeature(feat)
      bad_create = ogr.Feature(lyr.GetLayerDefn())
      bad_create.SetGeometry(ogr.CreateGeometryFromWkt('POINT(-180 91)'))
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'PROJ: utm: Invalid latitude'),
      ):
        lyr.CreateFeature(bad_create)

    with gdal.OpenEx(shp_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      ds.ExecuteSQL('REPACK ogr_vrt_29')
      ds.ExecuteSQL('RECOMPUTE EXTENT ON ogr_vrt_29')

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 27)

  def _create_union_source_shapefiles(self, vsi_dir: pathlib.Path) -> None:
    """Creates two 25-point shapefiles for union layer tests."""
    _create_point_grid_shapefile(
        str(vsi_dir / 'ogr_vrt_30_1.shp'),
        'ogr_vrt_30_1',
        2.0,
        (
            ('id1', ogr.OFTInteger, lambda idx: idx),
            ('id2', ogr.OFTInteger, lambda idx: 100 + idx),
        ),
        create_spatial_index=True,
    )
    _create_point_grid_shapefile(
        str(vsi_dir / 'ogr_vrt_30_2.shp'),
        'ogr_vrt_30_2',
        4.0,
        (
            ('id2', ogr.OFTInteger64, lambda idx: 200 + idx),
            ('id3', ogr.OFTInteger, lambda idx: 300 + idx),
        ),
        create_spatial_index=True,
    )

  def test_union_layer_default_union_strategy(self) -> None:
    # Upstream: test_ogr_vrt_30 (part 1)
    vsi_dir = self._make_vsi_dir()
    self._create_union_source_shapefiles(vsi_dir)
    vrt_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_30.vrt',
        """<OGRVRTDataSource>
  <OGRVRTUnionLayer name="union_layer">
    <OGRVRTLayer name="ogr_vrt_30_1">
      <SrcDataSource relativeToVRT="1">ogr_vrt_30_1.shp</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="ogr_vrt_30_2">
      <SrcDataSource relativeToVRT="1">ogr_vrt_30_2.shp</SrcDataSource>
    </OGRVRTLayer>
  </OGRVRTUnionLayer>
</OGRVRTDataSource>
""",
    )
    expected_srs = self._srs_from_input('EPSG:4326')

    # Verify with both default and proxied (OGR_VRT_MAX_OPENED=1) pool sizes.
    for max_opened in ('100', '1'):
      with (
          self.subTest(max_opened=max_opened),
          gdal.config_option('OGR_VRT_MAX_OPENED', max_opened),
          gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds,
      ):
        lyr = ds.GetLayer(0)
        self.assertIsSame(lyr.GetSpatialRef(), expected_srs)
        bb = lyr.GetExtent()
        for got_c, exp_c in zip(bb, (2.0, 4.8, 49.0, 49.8), strict=True):
          self.assertAlmostEqual(got_c, exp_c, delta=1e-4)
        self.assertEqual(lyr.GetFeatureCount(), 50)
        self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(1).GetType(), ogr.OFTInteger64
        )
        self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint)
        for cap, expected_val in (
            (ogr.OLCFastFeatureCount, 1),
            (ogr.OLCFastGetExtent, 1),
            (ogr.OLCFastSpatialFilter, 1),
            (ogr.OLCStringsAsUTF8, 1),
            (ogr.OLCIgnoreFields, 1),
            (ogr.OLCRandomWrite, 0),
            (ogr.OLCSequentialWrite, 0),
        ):
          self.assertEqual(lyr.TestCapability(cap), expected_val)

        # Iterate all 50 features across both layers to exercise pool eviction
        # and reopen when OGR_VRT_MAX_OPENED=1.
        count = 0
        for idx, feat in enumerate(lyr):
          count += 1
          self.assertEqual(feat.GetFID(), idx)
          if idx < 25:
            self.assertEqual(feat.GetFieldAsInteger('id1'), idx)
            self.assertEqual(feat.GetFieldAsInteger('id2'), 100 + idx)
            self.assertFalse(feat.IsFieldSet('id3'))
            self._assert_geom_matches_wkt(
                feat.GetGeometryRef(),
                f'POINT({2 + idx // 5 / 5.0} {49 + idx % 5 / 5.0})',
                max_error=1e-4,
            )
          else:
            local_idx = idx - 25
            self.assertFalse(feat.IsFieldSet('id1'))
            self.assertEqual(feat.GetFieldAsInteger('id2'), 200 + local_idx)
            self.assertEqual(feat.GetFieldAsInteger('id3'), 300 + local_idx)
            self._assert_geom_matches_wkt(
                feat.GetGeometryRef(),
                f'POINT({4 + local_idx // 5 / 5.0} {49 + local_idx % 5 / 5.0})',
                max_error=1e-4,
            )
        self.assertEqual(count, 50)

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)
      lyr.SetSpatialFilterRect(2.49, 49.29, 4.49, 49.69)
      self.assertEqual(lyr.GetFeatureCount(), 10)
      lyr.SetSpatialFilter(None)
      self.assertEqual(lyr.SetAttributeFilter('id1 = 0'), ogr.OGRERR_NONE)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      self.assertEqual(lyr.SetAttributeFilter(None), ogr.OGRERR_NONE)

      # Write operations fail without SourceLayerFieldName.
      new_feat = ogr.Feature(lyr.GetLayerDefn())
      new_feat['id2'] = 12345
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r'CreateFeature\(\) not supported when SourceLayerFieldName '
              r'is not set',
          ),
      ):
        lyr.CreateFeature(new_feat)

      lyr.ResetReading()
      existing = lyr.GetNextFeature()
      existing['id2'] = 45321
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r'SetFeature\(\) not supported when PreserveSrcFID is OFF',
          ),
      ):
        lyr.SetFeature(existing)

      self.assertEqual(
          lyr.SetAttributeFilter('id2 = 12345 or id2 = 45321'), ogr.OGRERR_NONE
      )
      lyr.ResetReading()
      self.assertIsNone(lyr.GetNextFeature())

  def test_union_layer_optional_attributes_and_writes(self) -> None:
    # Upstream: test_ogr_vrt_30 (part 2)
    vsi_dir = self._make_vsi_dir()
    self._create_union_source_shapefiles(vsi_dir)
    vrt_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_30_opts.vrt',
        """<OGRVRTDataSource>
  <OGRVRTUnionLayer name="union_layer">
    <OGRVRTLayer name="ogr_vrt_30_1">
      <SrcDataSource relativeToVRT="1">ogr_vrt_30_1.shp</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="ogr_vrt_30_2">
      <SrcDataSource relativeToVRT="1">ogr_vrt_30_2.shp</SrcDataSource>
    </OGRVRTLayer>
    <SourceLayerFieldName>source_layer</SourceLayerFieldName>
    <PreserveSrcFID>ON</PreserveSrcFID>
    <FieldStrategy>Intersection</FieldStrategy>
    <GeometryType>wkbPoint25D</GeometryType>
    <LayerSRS>WGS72</LayerSRS>
    <FeatureCount>100</FeatureCount>
    <ExtentXMin>-180</ExtentXMin>
    <ExtentYMin>-90</ExtentYMin>
    <ExtentXMax>180</ExtentXMax>
    <ExtentYMax>90</ExtentYMax>
  </OGRVRTUnionLayer>
</OGRVRTDataSource>
""",
    )
    expected_srs = self._srs_from_input('WGS72')

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)
      self.assertIsSame(lyr.GetSpatialRef(), expected_srs)
      self.assertEqual(lyr.GetExtent(), (-180.0, 180.0, -90.0, 90.0))
      self.assertEqual(lyr.GetFeatureCount(), 100)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint25D)
      for cap in (
          ogr.OLCFastFeatureCount,
          ogr.OLCFastGetExtent,
          ogr.OLCFastSpatialFilter,
          ogr.OLCStringsAsUTF8,
          ogr.OLCIgnoreFields,
          ogr.OLCRandomWrite,
          ogr.OLCSequentialWrite,
      ):
        with self.subTest(capability=cap):
          self.assertEqual(lyr.TestCapability(cap), 1)

      count = 0
      for idx, feat in enumerate(lyr):
        count += 1
        if idx < 25:
          self.assertEqual(feat.GetFID(), idx)
          self.assertEqual(
              feat.GetFieldAsString('source_layer'), 'ogr_vrt_30_1'
          )
          self.assertEqual(feat.GetFieldAsInteger('id2'), 100 + idx)
        else:
          self.assertEqual(feat.GetFID(), idx - 25)
          self.assertEqual(
              feat.GetFieldAsString('source_layer'), 'ogr_vrt_30_2'
          )
          self.assertEqual(feat.GetFieldAsInteger('id2'), 200 + idx - 25)
      self.assertEqual(count, 50)

      lyr.SetSpatialFilterRect(2.49, 49.29, 4.49, 49.69)
      self.assertEqual(lyr.GetFeatureCount(), 10)
      lyr.SetSpatialFilter(None)

    # Reopen on a fresh handle so OGRVRTLayer does not retain lazy spatial
    # filter state across the SetFeature and OLCFastFeatureCount checks.
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)

      # Invalid CreateFeature cases.
      bad_src = ogr.Feature(lyr.GetLayerDefn())
      bad_src['source_layer'] = 'random_name'
      bad_src['id2'] = 12345
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r"'random_name' source layer does not exist"
          ),
      ):
        lyr.CreateFeature(bad_src)

      unset_src = ogr.Feature(lyr.GetLayerDefn())
      unset_src['id2'] = 12345
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r"when 'source_layer' field is not set"
          ),
      ):
        lyr.CreateFeature(unset_src)

      preset_fid = ogr.Feature(lyr.GetLayerDefn())
      preset_fid.SetFID(999999)
      preset_fid['source_layer'] = 'ogr_vrt_30_2'
      preset_fid['id2'] = 12345
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'CreateFeature\(\) not supported when FID is set'
          ),
      ):
        lyr.CreateFeature(preset_fid)

      # Valid CreateFeature & SetFeature.
      ok_feat = ogr.Feature(lyr.GetLayerDefn())
      ok_feat['source_layer'] = 'ogr_vrt_30_2'
      ok_feat['id2'] = 12345
      self.assertEqual(lyr.CreateFeature(ok_feat), ogr.OGRERR_NONE)
      ok_feat['id2'] = 45321
      self.assertEqual(lyr.SetFeature(ok_feat), ogr.OGRERR_NONE)

      # Invalid SetFeature cases.
      ok_feat['source_layer'] = 'random_name'
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r"'random_name' source layer does not exist"
          ),
      ):
        lyr.SetFeature(ok_feat)

      ok_feat.UnsetField('source_layer')
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r"when 'source_layer' field is not set"
          ),
      ):
        lyr.SetFeature(ok_feat)

      unset_fid = ogr.Feature(lyr.GetLayerDefn())
      unset_fid['source_layer'] = 'ogr_vrt_30_2'
      unset_fid['id2'] = 12345
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'SetFeature\(\) not supported when FID is not set'
          ),
      ):
        lyr.SetFeature(unset_fid)

      # Attribute filter with passthrough vs without passthrough.
      self.assertEqual(
          lyr.SetAttributeFilter('id2 = 45321 AND OGR_GEOMETRY IS NULL'),
          ogr.OGRERR_NONE,
      )
      self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 1)
      lyr.ResetReading()
      got_feat = lyr.GetNextFeature()
      self.assertIsNotNone(got_feat)
      self.assertEqual(got_feat['id2'], 45321)
      self.assertEqual(got_feat['source_layer'], 'ogr_vrt_30_2')
      self.assertIsNone(got_feat.GetGeometryRef())
      self.assertIsNone(lyr.GetNextFeature())

      self.assertEqual(
          lyr.SetAttributeFilter(
              'id2 = 45321 AND OGR_GEOMETRY IS NULL '
              "AND source_layer = 'ogr_vrt_30_2'"
          ),
          ogr.OGRERR_NONE,
      )
      self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
      lyr.ResetReading()
      got_feat2 = lyr.GetNextFeature()
      self.assertIsNotNone(got_feat2)
      self.assertEqual(got_feat2['id2'], 45321)
      self.assertEqual(got_feat2['source_layer'], 'ogr_vrt_30_2')
      self.assertIsNone(got_feat2.GetGeometryRef())
      self.assertIsNone(lyr.GetNextFeature())

      self.assertEqual(lyr.SyncToDisk(), ogr.OGRERR_NONE)

    # Reopen read-only to verify CreateFeature + SetFeature persisted.
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(
          lyr.SetAttributeFilter(
              "id2 = 45321 AND source_layer = 'ogr_vrt_30_2'"
          ),
          ogr.OGRERR_NONE,
      )
      persisted = lyr.GetNextFeature()
      self.assertIsNotNone(persisted)
      self.assertEqual(persisted['id2'], 45321)
      self.assertIsNone(persisted.GetGeometryRef())
      self.assertIsNone(lyr.GetNextFeature())

  @parameterized.named_parameters(
      dict(
          testcase_name='non_shared',
          shared_attr='',
          error_regex=r'\[32 times\] a VRT!',
      ),
      dict(
          testcase_name='shared',
          shared_attr=' shared="1"',
          error_regex=r'Cyclic VRT opening detected!',
      ),
  )
  def test_anti_recursion_union_layer(
      self, shared_attr: str, error_regex: str
  ) -> None:
    # Upstream: test_ogr_vrt_31, test_ogr_vrt_32
    vsi_dir = self._make_vsi_dir()
    rec1_path = str(vsi_dir / 'rec1.vrt')
    rec2_path = str(vsi_dir / 'rec2.vrt')
    _write_vsi_text(
        pathlib.Path(rec1_path),
        f"""<OGRVRTDataSource>
  <OGRVRTUnionLayer name="rec1">
    <OGRVRTLayer name="rec2">
      <SrcDataSource{shared_attr}>{rec2_path}</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="rec1">
      <SrcDataSource{shared_attr}>{rec1_path}</SrcDataSource>
    </OGRVRTLayer>
  </OGRVRTUnionLayer>
</OGRVRTDataSource>""",
    )
    _write_vsi_text(
        pathlib.Path(rec2_path),
        f"""<OGRVRTDataSource>
  <OGRVRTUnionLayer name="rec2">
    <OGRVRTLayer name="rec1">
      <SrcDataSource{shared_attr}>{rec1_path}</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="rec2">
      <SrcDataSource{shared_attr}>{rec2_path}</SrcDataSource>
    </OGRVRTLayer>
  </OGRVRTUnionLayer>
</OGRVRTDataSource>""",
    )

    with gdal.OpenEx(rec1_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertIsNotNone(ds)
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, error_regex),
      ):
        ds.GetLayer(0).GetLayerDefn()
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), 0)

  @parameterized.named_parameters(
      dict(
          testcase_name='minimal',
          geom_fields_xml='',
          expected_geom_count=3,
      ),
      dict(
          testcase_name='explicit_fields',
          geom_fields_xml=(
              '<GeometryField name="geom__WKT_EPSG_4326_POINT"/>'
              '<GeometryField name="geom__WKT_EPSG_32632_POLYGON"/>'
          ),
          expected_geom_count=2,
      ),
  )
  def test_multigeom_minimal_and_explicit(
      self, geom_fields_xml: str, expected_geom_count: int
  ) -> None:
    # Upstream: test_ogr_vrt_33
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource>{csv_dir}</SrcDataSource>
    {geom_fields_xml}
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPoint)
      self.assertTrue(lyr.GetSpatialRef().ExportToWkt().startswith('GEOGCS'))
      defn = lyr.GetLayerDefn()
      self.assertEqual(defn.GetGeomFieldCount(), expected_geom_count)
      self.assertEqual(defn.GetGeomFieldDefn(1).GetType(), ogr.wkbPolygon)
      self.assertEqual(lyr.GetExtent(geom_field=1), (0, 1, 0, 1))
      self.assertTrue(
          defn.GetGeomFieldDefn(1)
          .GetSpatialRef()
          .ExportToWkt()
          .startswith('PROJCS')
      )
      feat = lyr.GetNextFeature()
      self._assert_geom_matches_wkt(feat.GetGeomFieldRef(0), 'POINT (1 2)')
      self._assert_geom_matches_wkt(
          feat.GetGeomFieldRef(1), 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'
      )

  def test_multigeom_select_second_field_and_clip(self) -> None:
    # Upstream: test_ogr_vrt_33a
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    merc_proj4 = (
        '+proj=merc +a=6378137 +b=6378137 +lat_ts=0.0 +lon_0=0.0 '
        '+x_0=0.0 +y_0=0 +k=1.0 +units=m +nadgrids=@null +wktext +no_defs'
    )
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource>{csv_dir}</SrcDataSource>
    <GeometryField name="foo" field="geom__WKT_EPSG_32632_POLYGON">
      <GeometryType>wkbPolygon25D</GeometryType>
      <SRS>{merc_proj4}</SRS>
      <ExtentXMin>1</ExtentXMin>
      <ExtentYMin>2</ExtentYMin>
      <ExtentXMax>3</ExtentXMax>
      <ExtentYMax>4</ExtentYMax>
      <SrcRegion clip="true">POLYGON((0.5 0.5,1.5 0.5,1.5 1.5,0.5 1.5,0.5 0.5))</SrcRegion>
    </GeometryField>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPolygon25D)
      expected_srs = self._srs_from_input(merc_proj4)
      self.assertIsSame(lyr.GetSpatialRef(), expected_srs)
      self.assertEqual(lyr.GetGeometryColumn(), 'foo')
      self.assertEqual(lyr.TestCapability(ogr.OLCFastGetExtent), 1)
      self.assertEqual(lyr.GetExtent(geom_field=0), (1, 3, 2, 4))
      self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 1)
      feat = lyr.GetNextFeature()
      got_geom = feat.GetGeomFieldRef(0)
      self.assertIsNotNone(got_geom)
      expected_geom = ogr.CreateGeometryFromWkt(
          'POLYGON ((0.5 0.5,1 0.5,1 1,0.5 1,0.5 0.5))'
      )
      self.assertTrue(got_geom.Within(expected_geom))
      self.assertTrue(expected_geom.Within(got_geom))
      self.assertAlmostEqual(got_geom.GetArea(), 0.25)

  def test_multigeom_wkb_none(self) -> None:
    # Upstream: test_ogr_vrt_33b
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource>{csv_dir}</SrcDataSource>
    <GeometryType>wkbNone</GeometryType>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    # Open independently for each lazy-init entry point, matching upstream.
    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetLayer(0).GetGeomType(), ogr.wkbNone)

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertIsNone(ds.GetLayer(0).GetSpatialRef())

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), 0)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastGetExtent), 0)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastSpatialFilter), 0)

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertIsNone(feat.GetGeomFieldRef(0))

  def test_multigeom_mixed_wkt_and_point_from_columns(self) -> None:
    # Upstream: test_ogr_vrt_33c
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource>{csv_dir}</SrcDataSource>
    <GeometryField name="geom__WKT_EPSG_4326_POINT"/>
    <GeometryField name="foo" encoding="PointFromColumns" x="X" y="Y"
                   reportSrcColumn="false">
      <SRS>EPSG:4326</SRS>
    </GeometryField>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      geom1_defn = lyr.GetLayerDefn().GetGeomFieldDefn(1)
      self.assertEqual(geom1_defn.GetType(), ogr.wkbPoint)
      self.assertIn('4326', geom1_defn.GetSpatialRef().ExportToWkt())

      feat = lyr.GetNextFeature()
      self._assert_geom_matches_wkt(feat.GetGeomFieldRef(0), 'POINT (1 2)')
      self._assert_geom_matches_wkt(feat.GetGeomFieldRef(1), 'POINT (-1 -2)')

      lyr.SetSpatialFilterRect(1, 0, 0, 0, 0)
      self.assertEqual(lyr.GetFeatureCount(), 0)
      lyr.SetSpatialFilterRect(1, -1.1, -2.1, -0.9, -1.9)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      lyr.SetSpatialFilter(1, None)
      self.assertEqual(lyr.GetFeatureCount(), 1)

      self.assertEqual(
          lyr.SetIgnoredFields(['geom__WKT_EPSG_4326_POINT']), ogr.OGRERR_NONE
      )
      lyr.ResetReading()
      feat = lyr.GetNextFeature()
      self.assertIsNone(feat.GetGeomFieldRef(0))
      self._assert_geom_matches_wkt(feat.GetGeomFieldRef(1), 'POINT (-1 -2)')

      self.assertEqual(lyr.SetIgnoredFields(['foo']), ogr.OGRERR_NONE)
      lyr.ResetReading()
      feat = lyr.GetNextFeature()
      self.assertIsNone(feat.GetGeomFieldRef(1))
      self._assert_geom_matches_wkt(feat.GetGeomFieldRef(0), 'POINT (1 2)')

  @parameterized.named_parameters(
      dict(
          testcase_name='implicit_first_field',
          warped_field_xml='',
          src_srs='EPSG:4326',
          src_wkt='POINT (1 2)',
          target_srs='EPSG:32631',
          warped_idx=0,
          untouched_idx=1,
          untouched_wkt='POLYGON ((0 0,0 1,1 1,1 0,0 0))',
      ),
      dict(
          testcase_name='explicit_first_field',
          warped_field_xml=(
              '<WarpedGeomFieldName>geom__WKT_EPSG_4326_POINT'
              '</WarpedGeomFieldName>'
          ),
          src_srs='EPSG:4326',
          src_wkt='POINT (1 2)',
          target_srs='EPSG:32631',
          warped_idx=0,
          untouched_idx=1,
          untouched_wkt='POLYGON ((0 0,0 1,1 1,1 0,0 0))',
      ),
      dict(
          testcase_name='explicit_second_field',
          warped_field_xml=(
              '<WarpedGeomFieldName>geom__WKT_EPSG_32632_POLYGON'
              '</WarpedGeomFieldName>'
          ),
          src_srs='EPSG:32632',
          src_wkt='POLYGON ((0 0,0 1,1 1,1 0,0 0))',
          target_srs='EPSG:4326',
          warped_idx=1,
          untouched_idx=0,
          untouched_wkt='POINT (1 2)',
      ),
  )
  def test_multigeom_warped_layer(
      self,
      warped_field_xml: str,
      src_srs: str,
      src_wkt: str,
      target_srs: str,
      warped_idx: int,
      untouched_idx: int,
      untouched_wkt: str,
  ) -> None:
    # Upstream: test_ogr_vrt_33d, test_ogr_vrt_33e, test_ogr_vrt_33g
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="test">
      <SrcDataSource>{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    {warped_field_xml}
    <TargetSRS>{target_srs}</TargetSRS>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>"""

    expected_srs = self._srs_from_input(target_srs)
    ct = osr.CoordinateTransformation(
        self._srs_from_input(src_srs), expected_srs
    )
    expected_warped_geom = ogr.CreateGeometryFromWkt(src_wkt)
    self.assertIsNotNone(expected_warped_geom)
    assert expected_warped_geom is not None
    self.assertEqual(expected_warped_geom.Transform(ct), ogr.OGRERR_NONE)

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      warped_srs = (
          lyr.GetLayerDefn().GetGeomFieldDefn(warped_idx).GetSpatialRef()
      )
      self.assertIsSame(warped_srs, expected_srs)
      feat = lyr.GetNextFeature()
      warped_geom = feat.GetGeomFieldRef(warped_idx)
      self.assertIsNotNone(warped_geom)
      self._assert_geom_tree_close(warped_geom, expected_warped_geom, 1e-4)
      self._assert_geom_matches_wkt(
          feat.GetGeomFieldRef(untouched_idx), untouched_wkt
      )

  def test_multigeom_warped_missing_field_fails(self) -> None:
    # Upstream: test_ogr_vrt_33f
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="test">
      <SrcDataSource>{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <WarpedGeomFieldName>foo</WarpedGeomFieldName>
    <TargetSRS>EPSG:32631</TargetSRS>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>"""
    self._assert_vrt_open_error(
        ds_xml, r"Cannot find source geometry field 'foo'"
    )

  def _assert_feature_geom_fields(
      self, feat: ogr.Feature, expected_wkts: Sequence[str | None]
  ) -> None:
    self.assertIsNotNone(feat)
    for idx, expected_wkt in enumerate(expected_wkts):
      with self.subTest(geom_field_index=idx):
        got_geom = feat.GetGeomFieldRef(idx)
        if expected_wkt is None:
          self.assertIsNone(got_geom)
        else:
          self._assert_geom_matches_wkt(got_geom, expected_wkt)

  @parameterized.named_parameters(
      dict(
          testcase_name='union',
          strategy_xml='',
          expected_geom_fields=(
              ('geom__WKT_EPSG_4326_POINT', ogr.wkbPoint, '4326'),
              ('geom__WKT_EPSG_32632_POLYGON', ogr.wkbPolygon, '32632'),
              ('geom__WKT_EPSG_4326_LINESTRING', ogr.wkbLineString, '4326'),
              ('geom__WKT_EPSG_32631_POINT', ogr.wkbPoint, '32631'),
          ),
          expected_f0_wkts=(
              'POINT (1 2)',
              'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
              None,
              None,
          ),
          expected_f1_wkts=(
              'POINT (3 4)',
              'POLYGON ((1 1,1 2,2 2,2 1,1 1))',
              None,
              'POINT (5 6)',
          ),
      ),
      dict(
          testcase_name='intersection',
          strategy_xml='<FieldStrategy>Intersection</FieldStrategy>',
          expected_geom_fields=(
              ('geom__WKT_EPSG_4326_POINT', ogr.wkbPoint, '4326'),
              ('geom__WKT_EPSG_32632_POLYGON', ogr.wkbPolygon, '32632'),
          ),
          expected_f0_wkts=('POINT (1 2)', 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'),
          expected_f1_wkts=('POINT (3 4)', 'POLYGON ((1 1,1 2,2 2,2 1,1 1))'),
      ),
      dict(
          testcase_name='first_layer',
          strategy_xml='<FieldStrategy>FirstLayer</FieldStrategy>',
          expected_geom_fields=(
              ('geom__WKT_EPSG_4326_POINT', ogr.wkbPoint, '4326'),
              ('geom__WKT_EPSG_32632_POLYGON', ogr.wkbPolygon, '32632'),
              ('geom__WKT_EPSG_4326_LINESTRING', ogr.wkbLineString, '4326'),
          ),
          expected_f0_wkts=(
              'POINT (1 2)',
              'POLYGON ((0 0,0 1,1 1,1 0,0 0))',
              None,
          ),
          expected_f1_wkts=(
              'POINT (3 4)',
              'POLYGON ((1 1,1 2,2 2,2 1,1 1))',
              None,
          ),
      ),
  )
  def test_multigeom_union_strategies(
      self,
      strategy_xml: str,
      expected_geom_fields: Sequence[tuple[str, int, str]],
      expected_f0_wkts: Sequence[str | None],
      expected_f1_wkts: Sequence[str | None],
  ) -> None:
    # Upstream: test_ogr_vrt_33h, test_ogr_vrt_33i, test_ogr_vrt_33j
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTUnionLayer name="union_layer">
    <OGRVRTLayer name="test">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="test2">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    {strategy_xml}
  </OGRVRTUnionLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      defn = lyr.GetLayerDefn()
      expected_count = len(expected_geom_fields)
      self.assertEqual(defn.GetGeomFieldCount(), expected_count)
      for idx, (gname, gtype, epsg) in enumerate(expected_geom_fields):
        with self.subTest(geom_field=gname):
          gf = defn.GetGeomFieldDefn(idx)
          self.assertEqual(gf.GetName(), gname)
          self.assertEqual(gf.GetType(), gtype)
          self.assertIn(epsg, gf.GetSpatialRef().ExportToWkt())

      self._assert_feature_geom_fields(lyr.GetNextFeature(), expected_f0_wkts)
      self._assert_feature_geom_fields(lyr.GetNextFeature(), expected_f1_wkts)

  def test_multigeom_union_explicit_geom_fields(self) -> None:
    # Upstream: test_ogr_vrt_33k, test_ogr_vrt_33l
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)

    explicit_xml = f"""<OGRVRTDataSource>
  <OGRVRTUnionLayer name="union_layer">
    <OGRVRTLayer name="test">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="test2">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <GeometryField name="geom__WKT_EPSG_32632_POLYGON"/>
    <GeometryField name="geom__WKT_EPSG_4326_POINT"/>
  </OGRVRTUnionLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(explicit_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      defn = lyr.GetLayerDefn()
      self.assertEqual(defn.GetGeomFieldCount(), 2)
      self.assertEqual(
          defn.GetGeomFieldDefn(0).GetName(), 'geom__WKT_EPSG_32632_POLYGON'
      )
      self.assertEqual(defn.GetGeomFieldDefn(0).GetType(), ogr.wkbPolygon)
      self.assertIn(
          '32632', defn.GetGeomFieldDefn(0).GetSpatialRef().ExportToWkt()
      )
      self.assertEqual(
          defn.GetGeomFieldDefn(1).GetName(), 'geom__WKT_EPSG_4326_POINT'
      )
      self.assertEqual(defn.GetGeomFieldDefn(1).GetType(), ogr.wkbPoint)
      self.assertIn(
          '4326', defn.GetGeomFieldDefn(1).GetSpatialRef().ExportToWkt()
      )

      feat = lyr.GetNextFeature()
      self._assert_geom_matches_wkt(
          feat.GetGeomFieldRef(0), 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'
      )
      self._assert_geom_matches_wkt(feat.GetGeomFieldRef(1), 'POINT (1 2)')

    reprojected_xml = f"""<OGRVRTDataSource>
  <OGRVRTUnionLayer name="union_layer">
    <OGRVRTLayer name="test">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="test2">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <GeometryField name="geom__WKT_EPSG_32632_POLYGON">
      <GeometryType>wkbPolygon25D</GeometryType>
    </GeometryField>
    <GeometryField name="geom__WKT_EPSG_4326_POINT">
      <SRS>EPSG:4322</SRS>
      <ExtentXMin>1</ExtentXMin>
      <ExtentYMin>2</ExtentYMin>
      <ExtentXMax>3</ExtentXMax>
      <ExtentYMax>4</ExtentYMax>
    </GeometryField>
  </OGRVRTUnionLayer>
</OGRVRTDataSource>"""

    ct_4326_to_4322 = osr.CoordinateTransformation(
        self._srs_from_input('EPSG:4326'), self._srs_from_input('EPSG:4322')
    )
    expected_pt = ogr.CreateGeometryFromWkt('POINT (1 2)')
    self.assertIsNotNone(expected_pt)
    assert expected_pt is not None
    self.assertEqual(expected_pt.Transform(ct_4326_to_4322), ogr.OGRERR_NONE)

    with gdal.OpenEx(reprojected_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      defn = lyr.GetLayerDefn()
      self.assertEqual(defn.GetGeomFieldCount(), 2)
      self.assertEqual(lyr.GetExtent(geom_field=1), (1, 3, 2, 4))
      self.assertEqual(defn.GetGeomFieldDefn(0).GetType(), ogr.wkbPolygon25D)
      self.assertIn(
          '32632', defn.GetGeomFieldDefn(0).GetSpatialRef().ExportToWkt()
      )
      self.assertEqual(defn.GetGeomFieldDefn(1).GetType(), ogr.wkbPoint)
      self.assertIn(
          '4322', defn.GetGeomFieldDefn(1).GetSpatialRef().ExportToWkt()
      )
      feat = lyr.GetNextFeature()
      self._assert_geom_matches_wkt(
          feat.GetGeomFieldRef(0), 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'
      )
      pt = feat.GetGeomFieldRef(1)
      self.assertIsNotNone(pt)
      self._assert_geom_tree_close(pt, expected_pt, 1e-4)

  def test_multigeom_union_wkb_none_and_execute_sql(self) -> None:
    # Upstream: test_ogr_vrt_33m
    vsi_dir = self._make_vsi_dir()
    csv_dir = _create_multigeom_csv_dir(vsi_dir)
    ds_xml = f"""<OGRVRTDataSource>
  <OGRVRTUnionLayer name="union_layer">
    <OGRVRTLayer name="test">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <OGRVRTLayer name="test2">
      <SrcDataSource shared="1">{csv_dir}</SrcDataSource>
    </OGRVRTLayer>
    <GeometryType>wkbNone</GeometryType>
  </OGRVRTUnionLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(ds_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 6)
      self.assertIsNotNone(lyr.GetNextFeature())

    expected_sql_geom_fields = (
        ('geom__WKT_EPSG_4326_POINT', ogr.wkbPoint, '4326'),
        ('geom__WKT_EPSG_32632_POLYGON', ogr.wkbPolygon, '32632'),
        ('geom__WKT_EPSG_4326_LINESTRING', ogr.wkbLineString, '4326'),
        ('geom__WKT_EPSG_32631_POINT', ogr.wkbPoint, '32631'),
    )
    with gdal.OpenEx(csv_dir, gdal.OF_VECTOR | gdal.OF_READONLY) as src_ds:
      with src_ds.ExecuteSQL(
          'SELECT * FROM test UNION ALL SELECT * FROM test2'
      ) as sql_lyr:
        defn = sql_lyr.GetLayerDefn()
        expected_count = len(expected_sql_geom_fields)
        self.assertEqual(defn.GetGeomFieldCount(), expected_count)
        for idx, (gname, gtype, epsg) in enumerate(expected_sql_geom_fields):
          with self.subTest(geom_field=gname):
            gf = defn.GetGeomFieldDefn(idx)
            self.assertEqual(gf.GetName(), gname)
            self.assertEqual(gf.GetType(), gtype)
            self.assertIn(epsg, gf.GetSpatialRef().ExportToWkt())

        self._assert_feature_geom_fields(
            sql_lyr.GetNextFeature(),
            ('POINT (1 2)', 'POLYGON ((0 0,0 1,1 1,1 0,0 0))', None, None),
        )
        self._assert_feature_geom_fields(
            sql_lyr.GetNextFeature(),
            (
                'POINT (3 4)',
                'POLYGON ((1 1,1 2,2 2,2 1,1 1))',
                None,
                'POINT (5 6)',
            ),
        )

  def test_set_ignored_fields_with_point_from_columns(self) -> None:
    # Upstream: test_ogr_vrt_34
    vsi_dir = self._make_vsi_dir()
    csv_path = _write_vsi_text(vsi_dir / 'test.csv', 'x,y\n2,49\n')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="0">{csv_path}</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    <GeometryField encoding="PointFromColumns" x="x" y="y"/>
    <Field name="x" type="Real"/>
    <Field name="y" type="Real"/>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test')
      self.assertEqual(lyr.SetIgnoredFields(['x', 'y']), ogr.OGRERR_NONE)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertFalse(feat.IsFieldSet('x'))
      self.assertFalse(feat.IsFieldSet('y'))
      self._assert_geom_matches_wkt(feat.GetGeometryRef(), 'POINT (2 49)')

  def test_nullable_fields_and_geom_fields(self) -> None:
    # Upstream: test_ogr_vrt_35
    vsi_dir = self._make_vsi_dir()
    _write_vsi_text(vsi_dir / 'test.csv', 'c1,c2,WKT,WKT2\n1,,"POINT(2 49),"\n')
    vrt1_path = _write_vsi_text(
        vsi_dir / 'test.vrt',
        """<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="1">test.csv</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    <GeometryField encoding="WKT" field="WKT" name="g1" nullable="false"/>
    <GeometryField encoding="WKT" field="WKT2" name="g2"/>
    <Field name="c1" type="Integer" nullable="false"/>
    <Field name="c2" type="Integer"/>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )
    vrt2_path = _write_vsi_text(
        vsi_dir / 'test2.vrt',
        """<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="1">test.vrt</SrcDataSource>
    <SrcLayer>test</SrcLayer>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )

    for vrt_path in (vrt1_path, vrt2_path):
      with (
          self.subTest(vrt_path=vrt_path),
          gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
      ):
        defn = ds.GetLayerByName('test').GetLayerDefn()
        self.assertEqual(defn.GetFieldDefn(0).IsNullable(), 0)
        self.assertEqual(defn.GetFieldDefn(1).IsNullable(), 1)
        self.assertEqual(defn.GetGeomFieldDefn(0).IsNullable(), 0)
        self.assertEqual(defn.GetGeomFieldDefn(1).IsNullable(), 1)

  def test_field_alternative_name_and_comment(self) -> None:
    # Upstream: test_ogr_vrt_alternative_name_comment
    vsi_dir = self._make_vsi_dir()
    _write_vsi_text(vsi_dir / 'test.csv', 'c1,c2\n1,2\n')
    vrt_path = _write_vsi_text(
        vsi_dir / 'test.vrt',
        """<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource relativeToVRT="1">test.csv</SrcDataSource>
    <SrcLayer>test</SrcLayer>
    <Field name="c1" type="Integer" alternativeName="alternative_name"
           comment="this is a comment"/>
    <Field name="c2" type="Integer"/>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      defn = ds.GetLayerByName('test').GetLayerDefn()
      self.assertEqual(defn.GetFieldDefn(0).GetName(), 'c1')
      self.assertEqual(
          defn.GetFieldDefn(0).GetAlternativeName(), 'alternative_name'
      )
      self.assertEqual(defn.GetFieldDefn(0).GetComment(), 'this is a comment')
      self.assertEqual(defn.GetFieldDefn(1).GetName(), 'c2')
      self.assertEqual(defn.GetFieldDefn(1).GetAlternativeName(), '')
      self.assertEqual(defn.GetFieldDefn(1).GetComment(), '')

  def test_edit_direct_geometry_with_layer_srs(self) -> None:
    # Upstream: test_ogr_vrt_36
    vsi_dir = self._make_vsi_dir()
    shp_path = str(vsi_dir / 'ogr_vrt_36.shp')
    shp_drv = gdal.GetDriverByName('ESRI Shapefile')
    with shp_drv.Create(shp_path, 0, 0, 0, gdal.GDT_Unknown) as ds:
      lyr = ds.CreateLayer('ogr_vrt_36', geom_type=ogr.wkbPoint)
      lyr.CreateField(ogr.FieldDefn('id', ogr.OFTString))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat['id'] = '1'
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 1)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    vrt_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_36.vrt',
        """<OGRVRTDataSource>
  <OGRVRTLayer name="ogr_vrt_36">
    <SrcDataSource relativeToVRT="1">ogr_vrt_36.shp</SrcDataSource>
    <GeometryType>wkbPoint</GeometryType>
    <LayerSRS>WGS84</LayerSRS>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_UPDATE) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)

    with gdal.OpenEx(shp_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['id'], '1')
      self._assert_geom_matches_wkt(feat.GetGeometryRef(), 'POINT (0 1)')

  def test_implicit_non_spatial_layer(self) -> None:
    # Upstream: test_ogr_vrt_37
    vsi_dir = self._make_vsi_dir()
    vrt_path, _ = _create_vrt_test_files(vsi_dir)
    # Open twice so GetGeomType() and GetLayerDefn().GetGeomFieldCount() each
    # trigger FullInitialize() independently (#6336).
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test6')
      self.assertEqual(lyr.GetGeomType(), ogr.wkbNone)
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayerByName('test6')
      self.assertEqual(lyr.GetLayerDefn().GetGeomFieldCount(), 0)

  @parameterized.named_parameters(*_geometry_type_cases())
  def test_geometry_types_and_qualifiers(
      self, type_str: str, base_type: int, qualifier: str
  ) -> None:
    # Upstream: test_ogr_vrt_38
    vsi_dir = self._make_vsi_dir()
    # ogr_vrt_38.shp is intentionally omitted because FastInitialize() parses
    # <GeometryType> lazily without opening the source dataset.
    vrt_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_38.vrt',
        f"""<OGRVRTDataSource>
  <OGRVRTLayer name="ogr_vrt_38">
    <SrcDataSource relativeToVRT="1">ogr_vrt_38.shp</SrcDataSource>
    <GeometryType>wkb{type_str}{qualifier}</GeometryType>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )
    expected_type = base_type
    if qualifier in ('Z', 'ZM', '25D'):
      expected_type = ogr.GT_SetZ(expected_type)
    if qualifier in ('M', 'ZM'):
      expected_type = ogr.GT_SetM(expected_type)

    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), expected_type)

  def test_attribute_filter_with_fid_column(self) -> None:
    # Upstream: test_ogr_vrt_39
    vsi_dir = self._make_vsi_dir()
    _write_vsi_text(vsi_dir / 'ogr_vrt_39.csv', 'my_fid,val\n30,1\n25,2\n')
    _write_vsi_text(vsi_dir / 'ogr_vrt_39.csvt', 'Integer,Integer\n')
    vrt_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_39.vrt',
        """<OGRVRTDataSource>
  <OGRVRTLayer name="ogr_vrt_39">
    <SrcDataSource relativeToVRT="1">ogr_vrt_39.csv</SrcDataSource>
    <GeometryType>wkbNone</GeometryType>
    <FID>my_fid</FID>
    <Field name="val" type="Integer" src="val"/>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.SetAttributeFilter('fid = 25'), ogr.OGRERR_NONE)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['val'], 2)
      self.assertIsNone(lyr.GetNextFeature())

  def test_point_zm_from_columns(self) -> None:
    # Upstream: test_ogr_vrt_40
    vsi_dir = self._make_vsi_dir()
    _write_vsi_text(vsi_dir / 'ogr_vrt_40.csv', 'id,x,y,z,m\n1,1,2,3,4\n')
    vrt_path = _write_vsi_text(
        vsi_dir / 'ogr_vrt_40.vrt',
        """<OGRVRTDataSource>
  <OGRVRTLayer name="ogr_vrt_40">
    <SrcDataSource relativeToVRT="1">ogr_vrt_40.csv</SrcDataSource>
    <SrcLayer>ogr_vrt_40</SrcLayer>
    <GeometryField encoding="PointFromColumns" x="x" y="y" z="z" m="m"/>
  </OGRVRTLayer>
</OGRVRTDataSource>""",
    )
    with gdal.OpenEx(vrt_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetGeomType(), ogr.wkbPointZM)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      geom = feat.GetGeometryRef()
      self.assertEqual(geom.ExportToIsoWkt(), 'POINT ZM (1 2 3 4)')

  def test_get_extent_on_missing_source(self) -> None:
    # Upstream: test_ogr_vrt_41
    vsi_dir = self._make_vsi_dir()
    missing_path = str(vsi_dir / 'missing.shp')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource>{missing_path}</SrcDataSource>
  </OGRVRTLayer>
</OGRVRTDataSource>"""
    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'missing\.shp'),
      ):
        lyr.GetExtent()

  def test_nullable_and_unique_fields(self) -> None:
    # Upstream: test_ogr_vrt_nullable_unique
    poly_shp = self._get_testdata_path('shape/poly/poly.shp')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="poly">
    <SrcDataSource>{poly_shp}</SrcDataSource>
    <SrcLayer>poly</SrcLayer>
    <GeometryType>wkbPolygon</GeometryType>
    <Field name="area" type="Real"/>
    <Field name="eas_id" type="Integer64" width="11" nullable="false"
           unique="true"/>
    <Field name="prfedea" type="String"/>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      feat_defn = ds.GetLayer(0).GetLayerDefn()
      self.assertTrue(feat_defn.GetFieldDefn(0).IsNullable())
      self.assertFalse(feat_defn.GetFieldDefn(0).IsUnique())
      self.assertFalse(feat_defn.GetFieldDefn(1).IsNullable())
      self.assertTrue(feat_defn.GetFieldDefn(1).IsUnique())

  def test_field_names_case_sensitivity(self) -> None:
    # Upstream: test_ogr_vrt_field_names_same_case
    geojson_src = (
        '{"type":"Feature","id":"foo","geometry":null,'
        '"properties":{"ID":"bar"}}'
    )
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="test">
    <SrcDataSource>{geojson_src}</SrcDataSource>
    <SrcLayer>OGRGeoJSON</SrcLayer>
    <Field name="id" type="String" src="id"/>
    <Field name="id_from_uc" type="String" src="ID"/>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['id'], 'foo')
      self.assertEqual(feat['id_from_uc'], 'bar')

  def test_geometry_coordinate_precision(self) -> None:
    # Upstream: test_ogr_vrt_geom_coordinate_precision
    poly_shp = self._get_testdata_path('shape/poly/poly.shp')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="poly">
    <SrcDataSource>{poly_shp}</SrcDataSource>
    <GeometryField>
      <GeometryType>wkbPolygon</GeometryType>
      <XYResolution>1e-5</XYResolution>
      <ZResolution>1e-3</ZResolution>
      <MResolution>1e-2</MResolution>
    </GeometryField>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      geom_fld = ds.GetLayer(0).GetLayerDefn().GetGeomFieldDefn(0)
      prec = geom_fld.GetCoordinatePrecision()
      self.assertEqual(prec.GetXYResolution(), 1e-5)
      self.assertEqual(prec.GetZResolution(), 1e-3)
      self.assertEqual(prec.GetMResolution(), 1e-2)

  def test_warped_layer_arrow_stream(self) -> None:
    # Upstream: test_ogr_vrt_warped_arrow
    vsi_dir = self._make_vsi_dir()
    gpkg_path = self._get_testdata_path('gpkg/2d_envelope.gpkg')
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTWarpedLayer>
    <OGRVRTLayer name="foo">
      <SrcDataSource>{gpkg_path}</SrcDataSource>
    </OGRVRTLayer>
    <TargetSRS>EPSG:32631</TargetSRS>
  </OGRVRTWarpedLayer>
</OGRVRTDataSource>"""

    out_filename = str(vsi_dir / 'out.gpkg')
    with (
        gdal.config_option('OGR2OGR_USE_ARROW_API', 'YES'),
        gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as src_ds,
    ):
      with gdal.VectorTranslate(out_filename, src_ds) as _:
        pass

    with gdal.OpenEx(out_filename, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      extent = ds.GetLayer(0).GetExtent()
      expected = (166021.443080541, 500000.0, 0.0, 331593.179548329)
      for got_c, exp_c in zip(extent, expected, strict=True):
        self.assertAlmostEqual(got_c, exp_c, delta=1e-4)

  def test_src_region_multipolygon_with_point_spatial_filter(self) -> None:
    # Upstream: test_ogr_vrt_srcregion_and_point_filter
    poly_shp = self._get_testdata_path('shape/poly/poly.shp')
    src_region_wkt = (
        'MULTIPOLYGON(((478315 4762880,478315 4765610,'
        '481645 4765610,481645 4762880,478315 4762880)))'
    )
    vrt_xml = f"""<OGRVRTDataSource>
  <OGRVRTLayer name="poly">
    <SrcDataSource>{poly_shp}</SrcDataSource>
    <SrcRegion>{src_region_wkt}</SrcRegion>
  </OGRVRTLayer>
</OGRVRTDataSource>"""

    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as src_ds:
      lyr = src_ds.GetLayer(0)

      lyr.SetSpatialFilter(ogr.CreateGeometryFromWkt('POINT(479751 4764703)'))
      lyr.ResetReading()
      self.assertIsNotNone(lyr.GetNextFeature())
      self.assertIsNone(lyr.GetNextFeature())
      self.assertEqual(lyr.GetFeatureCount(), 1)

      lyr.SetSpatialFilter(ogr.CreateGeometryFromWkt('POINT(-479751 -4764703)'))
      lyr.ResetReading()
      self.assertIsNone(lyr.GetNextFeature())
      self.assertEqual(lyr.GetFeatureCount(), 0)

      lyr.SetSpatialFilter(None)
      self.assertEqual(lyr.GetFeatureCount(), 10)

  def test_union_layer_getfeature_optimization(self) -> None:
    # Upstream: test_ogr_vrt_getfeature_optimization
    vsi_dir = self._make_vsi_dir()
    shp_drv = gdal.GetDriverByName('ESRI Shapefile')
    src_layers_xml: list[str] = []
    for src_idx in range(5):
      dbf_path = str(vsi_dir / f'src{src_idx}.dbf')
      with shp_drv.Create(dbf_path, 0, 0, 0, gdal.GDT_Unknown) as ds:
        lyr = ds.CreateLayer(f'src{src_idx}', geom_type=ogr.wkbNone)
        lyr.CreateField(ogr.FieldDefn('i', ogr.OFTInteger))
        for item_idx in range(10 - src_idx):
          feat = ogr.Feature(lyr.GetLayerDefn())
          feat['i'] = src_idx * 10 + item_idx
          lyr.CreateFeature(feat)
      src_layers_xml.append(
          f'<OGRVRTLayer name="src{src_idx}">'
          f'<SrcDataSource>{dbf_path}</SrcDataSource>'
          '</OGRVRTLayer>'
      )

    vrt_xml = (
        '<OGRVRTDataSource><OGRVRTUnionLayer name="test">'
        + ''.join(src_layers_xml)
        + '</OGRVRTUnionLayer></OGRVRTDataSource>'
    )
    with gdal.OpenEx(vrt_xml, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.TestCapability(ogr.OLCRandomRead), 0)
      fid_by_val = {feat['i']: feat.GetFID() for feat in lyr}
      self.assertLen(fid_by_val, 40)
      self.assertEqual(lyr.TestCapability(ogr.OLCRandomRead), 1)
      for fid in range(lyr.GetFeatureCount() - 1, -1, -1):
        feat = lyr.GetFeature(fid)
        self.assertIsNotNone(feat)
        self.assertEqual(fid_by_val[feat['i']], feat.GetFID())
      self.assertEqual(lyr.SetAttributeFilter('1=0'), ogr.OGRERR_NONE)
      self.assertEqual(lyr.TestCapability(ogr.OLCRandomRead), 0)


if __name__ == '__main__':
  googletest.main()
