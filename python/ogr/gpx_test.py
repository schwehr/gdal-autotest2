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
# Copyright (c) 2007-2010, Even Rouault
# <even dot rouault at mines-paris dot org>
#
# SPDX-License-Identifier: MIT

"""Tests OGR handling of GPX files.

Format is described here:
  https://gdal.org/en/latest/drivers/vector/gpx.html

Rewrite of:
  https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_gpx.py
"""

from collections.abc import Sequence
import datetime
import pathlib
import textwrap

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

DRIVER = ogr_util.GPX_DRIVER

_CSV_FOR_GPX = textwrap.dedent("""\
    WKT,Primary ID,Secondary ID,Third ID
    "POINT (0 1)",PID1,SID1,TID1
    "POINT (2 3)",PID2,SID2,
""")

_TRACK_WITH_TIME_EXTENSION_GPX = textwrap.dedent("""\
    <?xml version="1.0" encoding="UTF-8"?>
    <gpx creator="EasyGPS 4.45" version="1.1"
      xmlns="http://www.topografix.com/GPX/1/1"
      xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
      xsi:schemaLocation="http://www.topografix.com/GPX/1/1
        http://www.topografix.com/GPX/1/1/gpx.xsd
        http://www.topografix.com/GPX/gpx_modified/0/1
        http://www.topografix.com/GPX/gpx_modified/0/1/gpx_modified.xsd">
      <metadata>
        <bounds maxlat="31.17016200" maxlon="-7.91258400"
          minlat="31.05567200" minlon="-8.04354900"/>
        <extensions>
          <time
            xmlns="http://www.topografix.com/GPX/gpx_modified/0/1"
          >2015-10-11T15:16:34.147Z</time>
        </extensions>
      </metadata>
      <trk>
        <name>Track one</name>
        <extensions>
          <time
            xmlns="http://www.topografix.com/GPX/gpx_modified/0/1"
          >2015-10-11T15:06:33Z</time>
          <label xmlns="http://www.topografix.com/GPX/gpx_overlay/0/3"/>
        </extensions>
        <trkseg>
          <trkpt lat="31.16503000" lon="-8.02949100">
            <ele>1162.900</ele>
            <time>2015-10-11T16:56:58Z</time>
          </trkpt>
          <trkpt lat="31.16519100" lon="-8.02948100">
            <ele>1165.300</ele>
            <time>2015-10-11T16:57:38Z</time>
          </trkpt>
          <trkpt lat="31.16535400" lon="-8.02936200">
            <ele>1173.900</ele>
            <time>2015-10-11T16:58:18Z</time>
          </trkpt>
        </trkseg>
      </trk>
      <extensions/>
    </gpx>
""")

_METADATA_GPX = textwrap.dedent("""\
    <?xml version="1.0"?>
    <gpx creator="E. Rouault" version="1.1"
      xmlns="http://www.topografix.com/GPX/1/1">
      <metadata>
        <name>metadata name</name>
        <desc>metadata desc</desc>
        <author>
          <name>metadata author name</name>
          <email id="foo" domain="example.com"/>
          <link href="author_href">
            <text>author_text</text>
            <type>author_type</type>
          </link>
        </author>
        <copyright author="copyright author">
          <year>2023</year>
          <license>my license</license>
        </copyright>
        <link href="href"><text>text</text><type>type</type></link>
        <link href="href2"><text>text2</text><type>type2</type></link>
        <time>2007-11-25T17:58:00+01:00</time>
        <keywords>kw</keywords>
        <bounds minlat="-90" minlon="-180" maxlat="90" maxlon="179.9999999"/>
      </metadata>
    </gpx>
""")

_TRKPT_EXTENSIONS_GPX = textwrap.dedent("""\
    <?xml version="1.0" encoding="UTF-8"?>
    <gpx xmlns="http://www.topografix.com/GPX/1/1"
      xmlns:gpxdata="http://www.cluetrust.com/XML/GPXDATA/1/0"
      creator="test" version="1.1">
      <trk>
        <trkseg>
          <trkpt lat="49" lon="2">
            <extensions>
              <gpxdata:hr>91</gpxdata:hr>
              <gpxdata:cadence>81</gpxdata:cadence>
            </extensions>
          </trkpt>
          <trkpt lat="50" lon="3">
            <extensions>
              <gpxdata:hr>92</gpxdata:hr>
              <gpxdata:cadence>82</gpxdata:cadence>
            </extensions>
          </trkpt>
        </trkseg>
      </trk>
    </gpx>
""")

_TRKPT_EXTENSIONS_WITH_OGR_NS_GPX = _TRKPT_EXTENSIONS_GPX.replace(
    'creator="test"',
    'xmlns:ogr="http://osgeo.org/gdal" creator="test"',
)


class GpxTest(parameterized.TestCase, gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(ogr.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())
    driver = gdal.GetDriverByName(DRIVER)
    if driver is None:
      self.fail(f'Missing OGR driver: {DRIVER}')
    self.driver = driver
    self.vsimem_dir = pathlib.PurePosixPath(
        self.enter_context(
            gcore_util.VsimemCleanupDir(
                f'/vsimem/gpx_test/{self._testMethodName}'
            )
        )
    )
    self.assertEqual(gdal.MkdirRecursive(str(self.vsimem_dir), 0o755), 0)

  def _open_gpx(
      self,
      path: str,
      open_options: Sequence[str] | None = None,
  ) -> gdal.Dataset:
    if not path.startswith('/vsi'):
      self.assertTrue(
          pathlib.Path(path).exists(), f'Test file does not exist: {path}'
      )
    ds = gdal.OpenEx(
        path,
        gdal.OF_VECTOR | gdal.OF_READONLY,
        allowed_drivers=[DRIVER],
        open_options=list(open_options or ()),
    )
    if ds is None:
      self.fail(f'Failed to open GPX dataset: {path}')
    return ds

  def _write_vsimem_file(self, filename: str, content: str) -> str:
    path = str(self.vsimem_dir / filename)
    self.assertEqual(gdal.FileFromMemBuffer(path, content), 0)
    return path

  def _copy_features(self, src_lyr: ogr.Layer, dst_lyr: ogr.Layer) -> None:
    dst_feat = ogr.Feature(feature_def=dst_lyr.GetLayerDefn())
    for feat in src_lyr:
      dst_feat.SetFrom(feat)
      self.assertEqual(dst_lyr.CreateFeature(dst_feat), ogr.OGRERR_NONE)

  def _check_wgs84_srs(self, layer: ogr.Layer) -> None:
    expected_srs = osr.SpatialReference()
    expected_srs.ImportFromEPSG(4326)
    expected_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    actual_srs = layer.GetSpatialRef()
    if actual_srs is None:
      self.fail(f'Layer {layer.GetName()} is missing spatial reference')
    self.assertIsSame(actual_srs, expected_srs)

  def _check_features_against_list(
      self,
      layer: ogr.Layer,
      field_name: str,
      expected_values: Sequence[str | int | float | datetime.datetime | None],
  ) -> None:
    field_index = layer.GetLayerDefn().GetFieldIndex(field_name)
    self.assertGreaterEqual(
        field_index, 0, f'Did not find required field {field_name}'
    )
    layer.ResetReading()
    for i, expected in enumerate(expected_values):
      feat = layer.GetNextFeature()
      if feat is None:
        self.fail(
            f'Got only {i} features for field {field_name}, expected'
            f' {len(expected_values)}'
        )
      with self.subTest(field=field_name, feature_index=i):
        if isinstance(expected, str):
          self.assertTrue(feat.IsFieldSetAndNotNull(field_index))
          self.assertEqual(feat.GetFieldAsString(field_index), expected)
        elif isinstance(expected, datetime.datetime):
          self.assertTrue(feat.IsFieldSetAndNotNull(field_index))
          self.assertIsNone(expected.tzinfo, 'Expected naive UTC datetime')
          # utctimetuple() assumes naive datetime is UTC; TZFlag 100 is UTC.
          expected_dt = list(expected.utctimetuple()[0:6]) + [100]
          self.assertEqual(feat.GetFieldAsDateTime(field_index), expected_dt)
        else:
          self.assertEqual(feat.GetField(field_index), expected)

    self.assertIsNone(
        layer.GetNextFeature(),
        f'Got more than {len(expected_values)} features for {field_name}',
    )

  def _check_feature_geometry(
      self,
      actual: ogr.Feature | ogr.Geometry | None,
      expected: ogr.Geometry | str | None,
      # 1e-9 separates fixture points (~1e-5 spacing) while absorbing binary
      # floating-point decimal parsing differences.
      max_error: float = 1e-9,
  ) -> None:
    if isinstance(actual, ogr.Feature):
      actual_geom: ogr.Geometry | None = actual.GetGeometryRef()
    else:
      actual_geom = actual

    if isinstance(expected, str):
      expected_geom: ogr.Geometry | None = ogr.CreateGeometryFromWkt(expected)
      if expected_geom is None:
        self.fail(f'Failed to parse expected WKT: {expected}')
    else:
      expected_geom = expected

    if expected_geom is None:
      self.assertIsNone(actual_geom)
      return
    if actual_geom is None:
      self.fail('Expected geometry but got None')

    self.assertEqual(
        actual_geom.GetGeometryName(), expected_geom.GetGeometryName()
    )
    self.assertEqual(
        actual_geom.GetGeometryCount(), expected_geom.GetGeometryCount()
    )
    self.assertEqual(actual_geom.GetPointCount(), expected_geom.GetPointCount())
    self.assertEqual(actual_geom.IsEmpty(), expected_geom.IsEmpty())
    self.assertEqual(actual_geom.Is3D(), expected_geom.Is3D())

    if actual_geom.GetGeometryCount() > 0:
      for i in range(actual_geom.GetGeometryCount()):
        self._check_feature_geometry(
            actual_geom.GetGeometryRef(i),
            expected_geom.GetGeometryRef(i),
            max_error=max_error,
        )
    else:
      for i in range(actual_geom.GetPointCount()):
        self.assertAlmostEqual(
            actual_geom.GetX(i), expected_geom.GetX(i), delta=max_error
        )
        self.assertAlmostEqual(
            actual_geom.GetY(i), expected_geom.GetY(i), delta=max_error
        )
        if actual_geom.Is3D():
          self.assertAlmostEqual(
              actual_geom.GetZ(i), expected_geom.GetZ(i), delta=max_error
          )

  def _check_layer_geometries(
      self,
      layer: ogr.Layer,
      expected_wkts: Sequence[str],
      max_error: float = 1e-9,
  ) -> None:
    layer.ResetReading()
    for i, expected_wkt in enumerate(expected_wkts):
      with self.subTest(layer=layer.GetName(), feature_index=i):
        self._check_feature_geometry(
            layer.GetNextFeature(), expected_wkt, max_error=max_error
        )
    self.assertIsNone(
        layer.GetNextFeature(),
        f'Got more than {len(expected_wkts)} features in {layer.GetName()}',
    )

  def _check_waypoints(self, gpx_ds: gdal.Dataset) -> None:
    # Upstream: test_ogr_gpx_1
    lyr = gpx_ds.GetLayerByName('waypoints')
    self._check_wgs84_srs(lyr)
    self.assertEqual(lyr.GetDataset().GetDescription(), gpx_ds.GetDescription())

    # Suppress the one-time "more than N_MAX_LINKS=2 links" warning triggered
    # on test.gpx waypoint 0 (gated by static int once in ogrgpxlayer.cpp, so
    # it fires in whichever test reads test.gpx waypoints first).
    with gdal.quiet_warnings():
      self._check_features_against_list(lyr, 'ele', [2, None])
    self._check_features_against_list(lyr, 'name', ['waypoint name', None])
    self._check_features_against_list(lyr, 'cmt', ['waypoint comment', None])
    self._check_features_against_list(
        lyr, 'desc', ['waypoint description', None]
    )
    self._check_features_against_list(lyr, 'src', ['waypoint source', None])
    self._check_features_against_list(lyr, 'link1_href', ['href', None])
    self._check_features_against_list(lyr, 'link1_text', ['text', None])
    self._check_features_against_list(lyr, 'link1_type', ['type', None])
    self._check_features_against_list(lyr, 'link2_href', ['href2', None])
    self._check_features_against_list(lyr, 'link2_text', ['text2', None])
    self._check_features_against_list(lyr, 'link2_type', ['type2', None])
    self._check_features_against_list(
        lyr, 'time', ['2007/11/25 17:58:00+01', None]
    )
    self._check_layer_geometries(lyr, ('POINT (1 0)', 'POINT (4 3)'))

  def _check_routes(self, gpx_ds: gdal.Dataset) -> None:
    # Upstream: test_ogr_gpx_2
    lyr = gpx_ds.GetLayerByName('routes')
    self._check_wgs84_srs(lyr)
    self._check_features_against_list(
        lyr, 'name', ['route name', 'empty route']
    )
    self._check_layer_geometries(
        lyr, ('LINESTRING (6 5,9 8,12 11)', 'LINESTRING EMPTY')
    )

  def _check_route_points(self, gpx_ds: gdal.Dataset) -> None:
    # Upstream: test_ogr_gpx_3
    lyr = gpx_ds.GetLayerByName('route_points')
    self._check_wgs84_srs(lyr)
    self._check_features_against_list(
        lyr, 'name', ['route point name', None, None]
    )
    self._check_features_against_list(lyr, 'ele', [7, 10, 13])
    self._check_features_against_list(lyr, 'route_fid', [0, 0, 0])
    self._check_features_against_list(lyr, 'route_point_id', [0, 1, 2])
    self._check_features_against_list(
        lyr, 'time', [datetime.datetime(2007, 11, 25, 17, 58, 0), None, None]
    )
    self._check_layer_geometries(
        lyr, ('POINT (6 5)', 'POINT (9 8)', 'POINT (12 11)')
    )

  def _check_tracks(self, gpx_ds: gdal.Dataset) -> None:
    # Upstream: test_ogr_gpx_4
    lyr = gpx_ds.GetLayerByName('tracks')
    self._check_wgs84_srs(lyr)
    self._check_features_against_list(
        lyr, 'name', ['track name', 'empty track', 'empty track 2']
    )

    lyr.ResetReading()
    self._check_feature_geometry(
        lyr.GetNextFeature(), 'MULTILINESTRING ((15 14,18 17),(21 20,24 23))'
    )
    self._check_feature_geometry(lyr.GetNextFeature(), 'MULTILINESTRING EMPTY')
    # "empty track 2" has an empty <trkseg></trkseg>, producing a
    # MultiLineString with 1 empty LineString child (WKT is still
    # MULTILINESTRING EMPTY).
    feat = lyr.GetNextFeature()
    if feat is None:
      self.fail('Missing third track feature')
    geom = feat.GetGeometryRef()
    if geom is None:
      self.fail('Missing geometry on third track feature')
    self.assertEqual(geom.ExportToWkt(), 'MULTILINESTRING EMPTY')
    self.assertEqual(geom.GetGeometryCount(), 1)
    self.assertTrue(geom.IsEmpty())
    self.assertIsNone(lyr.GetNextFeature())

  def _check_track_points(self, gpx_ds: gdal.Dataset) -> None:
    # Upstream: test_ogr_gpx_5
    lyr = gpx_ds.GetLayerByName('track_points')
    self._check_wgs84_srs(lyr)
    self._check_features_against_list(
        lyr, 'name', ['track point name', None, None, None]
    )
    self._check_features_against_list(lyr, 'ele', [16, 19, 22, 25])
    self._check_features_against_list(lyr, 'track_fid', [0, 0, 0, 0])
    self._check_features_against_list(lyr, 'track_seg_id', [0, 0, 1, 1])
    self._check_features_against_list(lyr, 'track_seg_point_id', [0, 1, 0, 1])
    self._check_features_against_list(
        lyr,
        'time',
        [
            None,
            None,
            datetime.datetime(2014, 10, 12, 7, 12, 55),
            datetime.datetime(2014, 10, 12, 7, 12, 52),
        ],
    )
    self._check_layer_geometries(
        lyr,
        ('POINT (15 14)', 'POINT (18 17)', 'POINT (21 20)', 'POINT (24 23)'),
    )

  def test_read(self) -> None:
    # Upstream: test_ogr_gpx_1..5
    with self._open_gpx(ogr_util.GetTestFilePath('test.gpx')) as gpx_ds:
      self.assertEqual(gpx_ds.GetLayerCount(), 5)
      self._check_waypoints(gpx_ds)
      self._check_routes(gpx_ds)
      self._check_route_points(gpx_ds)
      self._check_tracks(gpx_ds)
      self._check_track_points(gpx_ds)

  def test_copy_and_read(self) -> None:
    # Upstream: test_ogr_gpx_6
    out_path = str(self.vsimem_dir / 'copy.gpx')
    # Suppress the one-time "more than N_MAX_LINKS=2 links" warning triggered
    # on test.gpx waypoint 0 (static int once in ogrgpxlayer.cpp).
    with (
        gdal.quiet_warnings(),
        self._open_gpx(ogr_util.GetTestFilePath('test.gpx')) as src_ds,
        self.driver.Create(out_path, 0, 0, 0, gdal.GDT_Unknown) as dst_ds,
    ):
      self.assertEqual(src_ds.GetLayerCount(), 5)
      for lyr_name, geom_type in (
          ('waypoints', ogr.wkbPoint),
          ('routes', ogr.wkbLineString),
          ('tracks', ogr.wkbMultiLineString),
      ):
        dst_lyr = dst_ds.CreateLayer(lyr_name, geom_type=geom_type)
        self._copy_features(src_ds.GetLayerByName(lyr_name), dst_lyr)

    copy_xml = gcore_util.vsi_read_file_as_string(out_path, max_size=16384)
    self.assertRegex(
        copy_xml,
        r'<bounds minlat="0\.0+" minlon="1\.0+"'
        r' maxlat="23\.0+" maxlon="24\.0+"/>',
    )

    with self._open_gpx(out_path) as reopened_ds:
      self._check_waypoints(reopened_ds)
      self._check_routes(reopened_ds)
      self._check_tracks(reopened_ds)

  def test_copy_waypoint_extra_fields(self) -> None:
    # Upstream: test_ogr_gpx_7 (migrated from BNA to CSV).
    csv_path = self._write_vsimem_file('csv_for_gpx.csv', _CSV_FOR_GPX)
    out_path = str(self.vsimem_dir / 'ogr_gpx_7.gpx')

    with (
        gdal.OpenEx(
            csv_path,
            gdal.OF_VECTOR | gdal.OF_READONLY,
            allowed_drivers=[ogr_util.CSV_DRIVER],
        ) as csv_ds,
        self.driver.Create(
            out_path,
            0,
            0,
            0,
            gdal.GDT_Unknown,
            options=['GPX_USE_EXTENSIONS=YES'],
        ) as gpx_ds,
    ):
      csv_lyr = csv_ds.GetLayerByName('csv_for_gpx')
      gpx_lyr = gpx_ds.CreateLayer('waypoints', geom_type=ogr.wkbPoint)
      csv_defn = csv_lyr.GetLayerDefn()
      for i in range(csv_defn.GetFieldCount()):
        self.assertEqual(
            gpx_lyr.CreateField(csv_defn.GetFieldDefn(i)), ogr.OGRERR_NONE
        )
      self._copy_features(csv_lyr, gpx_lyr)

    with self._open_gpx(out_path) as gpx_ds:
      gpx_lyr = gpx_ds.GetLayerByName('waypoints')
      self._check_features_against_list(
          gpx_lyr, 'ogr_Primary_ID', ['PID1', 'PID2']
      )
      self._check_features_against_list(
          gpx_lyr, 'ogr_Secondary_ID', ['SID1', 'SID2']
      )
      self._check_features_against_list(gpx_lyr, 'ogr_Third_ID', ['TID1', None])
      self._check_layer_geometries(gpx_lyr, ('POINT (0 1)', 'POINT (2 3)'))

  def test_create_field_without_extensions_fails(self) -> None:
    # Added for google3 to pin CreateField rejection without GPX_USE_EXTENSIONS
    # (ogrgpxlayer.cpp:1790-1797).
    no_ext_path = str(self.vsimem_dir / 'no_extensions.gpx')
    with self.driver.Create(no_ext_path, 0, 0, 0, gdal.GDT_Unknown) as gpx_ds:
      gpx_lyr = gpx_ds.CreateLayer('waypoints', geom_type=ogr.wkbPoint)
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r"Field of name 'extra_field' is not supported in GPX schema",
          ),
      ):
        gpx_lyr.CreateField(ogr.FieldDefn('extra_field', ogr.OFTString))

  def test_write_route_and_track_points(self) -> None:
    # Upstream: test_ogr_gpx_8
    out_path = str(self.vsimem_dir / 'ogr_gpx_8.gpx')
    ref_path = ogr_util.GetTestFilePath('ogr_gpx_8_ref.txt')
    self.assertTrue(pathlib.Path(ref_path).exists())

    route_points_data = (
        ('POINT (2 49)', 0, 'ROUTE_NAME'),
        ('POINT (3 50)', 0, '--ignored--'),
        ('POINT (3 51)', 1, 'ROUTE_NAME2'),
        ('POINT (3 49)', 1, None),
    )
    track_points_data = (
        ('POINT (2 49)', 0, 0, 'TRACK_NAME'),
        ('POINT (3 50)', 0, 0, '--ignored--'),
        ('POINT (3 51)', 0, 1, None),
        ('POINT (3 49)', 1, 0, 'TRACK_NAME2'),
    )

    with self.driver.Create(
        out_path, 0, 0, 0, gdal.GDT_Unknown, options=['LINEFORMAT=LF']
    ) as gpx_ds:
      rtept_lyr = gpx_ds.CreateLayer('route_points', geom_type=ogr.wkbPoint)
      for wkt, route_fid, route_name in route_points_data:
        feat = ogr.Feature(rtept_lyr.GetLayerDefn())
        feat.SetField('route_fid', route_fid)
        if route_name is not None:
          feat.SetField('route_name', route_name)
        feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        self.assertEqual(rtept_lyr.CreateFeature(feat), ogr.OGRERR_NONE)

      trkpt_lyr = gpx_ds.CreateLayer('track_points', geom_type=ogr.wkbPoint)
      for wkt, track_fid, track_seg_id, track_name in track_points_data:
        feat = ogr.Feature(trkpt_lyr.GetLayerDefn())
        feat.SetField('track_fid', track_fid)
        feat.SetField('track_seg_id', track_seg_id)
        if track_name is not None:
          feat.SetField('track_name', track_name)
        feat.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        self.assertEqual(trkpt_lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    actual_content = gcore_util.vsi_read_file_as_string(
        out_path, max_size=16384
    )
    ref_content = pathlib.Path(ref_path).read_text(encoding='utf-8')
    self.assertIn(ref_content, actual_content)

    with self._open_gpx(out_path) as reopened_ds:
      self._check_features_against_list(
          reopened_ds.GetLayerByName('routes'),
          'name',
          ['ROUTE_NAME', 'ROUTE_NAME2'],
      )
      self._check_features_against_list(
          reopened_ds.GetLayerByName('tracks'),
          'name',
          ['TRACK_NAME', 'TRACK_NAME2'],
      )
      trkpts = reopened_ds.GetLayerByName('track_points')
      self._check_features_against_list(trkpts, 'track_fid', [0, 0, 0, 1])
      self._check_features_against_list(trkpts, 'track_seg_id', [0, 0, 1, 0])

  def test_track_time_extension(self) -> None:
    # Upstream: test_ogr_gpx_9 (OSGeo/gdal#6237).
    gpx_path = self._write_vsimem_file(
        'track_with_time_extension.gpx', _TRACK_WITH_TIME_EXTENSION_GPX
    )
    with self._open_gpx(gpx_path) as ds:
      tracks_lyr = ds.GetLayerByName('tracks')
      self._check_features_against_list(tracks_lyr, 'name', ['Track one'])
      self._check_features_against_list(
          tracks_lyr, 'time', ['2015-10-11T15:06:33Z']
      )
      tracks_lyr.ResetReading()
      self._check_feature_geometry(
          tracks_lyr.GetNextFeature(),
          'MULTILINESTRING ((-8.029491 31.16503,'
          '-8.029481 31.165191,-8.029362 31.165354))',
      )

      trkpt_lyr = ds.GetLayerByName('track_points')
      self._check_features_against_list(
          trkpt_lyr, 'ele', [1162.9, 1165.3, 1173.9]
      )
      self._check_features_against_list(
          trkpt_lyr,
          'time',
          [
              datetime.datetime(2015, 10, 11, 16, 56, 58),
              datetime.datetime(2015, 10, 11, 16, 57, 38),
              datetime.datetime(2015, 10, 11, 16, 58, 18),
          ],
      )

  def test_metadata_read(self) -> None:
    # Upstream: test_ogr_gpx_metadata_read.
    # Uses inline _METADATA_GPX because local testdata/test.gpx predates
    # upstream's <metadata> addition, and omits <wpt><link> elements after
    # <metadata> to avoid ogrgpxdatasource.cpp's m_bInMetadataLink quirk.
    gpx_path = self._write_vsimem_file('metadata_read.gpx', _METADATA_GPX)
    expected_md = {
        'AUTHOR_EMAIL': 'foo@example.com',
        'AUTHOR_LINK_HREF': 'author_href',
        'AUTHOR_LINK_TEXT': 'author_text',
        'AUTHOR_LINK_TYPE': 'author_type',
        'AUTHOR_NAME': 'metadata author name',
        'COPYRIGHT_AUTHOR': 'copyright author',
        'COPYRIGHT_LICENSE': 'my license',
        'COPYRIGHT_YEAR': '2023',
        'DESCRIPTION': 'metadata desc',
        'KEYWORDS': 'kw',
        'LINK_1_HREF': 'href',
        'LINK_1_TEXT': 'text',
        'LINK_1_TYPE': 'type',
        'LINK_2_HREF': 'href2',
        'LINK_2_TEXT': 'text2',
        'LINK_2_TYPE': 'type2',
        'NAME': 'metadata name',
        'TIME': '2007-11-25T17:58:00+01:00',
    }
    with self._open_gpx(gpx_path) as ds:
      self.assertDictEqual(ds.GetMetadata(), expected_md)

  def test_metadata_write(self) -> None:
    # Upstream: test_ogr_gpx_metadata_write
    expected_md = {
        'AUTHOR_EMAIL': 'foo@example.com',
        'AUTHOR_LINK_HREF': 'author_href',
        'AUTHOR_LINK_TEXT': 'author_text',
        'AUTHOR_LINK_TYPE': 'author_type',
        'AUTHOR_NAME': 'metadata author name',
        'COPYRIGHT_AUTHOR': 'copyright author',
        'COPYRIGHT_LICENSE': 'my license',
        'COPYRIGHT_YEAR': '2023',
        'DESCRIPTION': 'metadata desc & <special>',
        'KEYWORDS': 'kw',
        'LINK_1_HREF': 'href',
        'LINK_1_TEXT': 'text',
        'LINK_1_TYPE': 'type',
        'LINK_2_HREF': 'href2',
        'LINK_2_TEXT': 'text2',
        'LINK_2_TYPE': 'type2',
        'NAME': 'metadata name',
        'TIME': '2007-11-25T17:58:00+01:00',
    }
    options = [f'METADATA_{key}={value}' for key, value in expected_md.items()]
    out_path = str(self.vsimem_dir / 'metadata_write.gpx')
    with self.driver.Create(
        out_path, 0, 0, 0, gdal.GDT_Unknown, options=options
    ) as ds:
      self.assertEqual(ds.GetDriver().ShortName, 'GPX')

    with self._open_gpx(out_path) as reopened_ds:
      self.assertDictEqual(reopened_ds.GetMetadata(), expected_md)

  @parameterized.named_parameters(
      dict(
          testcase_name='default',
          options=[],
          expected_prefix=' creator="GDAL ',
      ),
      dict(
          testcase_name='custom',
          options=['CREATOR=the_creator'],
          expected_prefix=' creator="the_creator" ',
      ),
      dict(
          testcase_name='xml_escaped',
          options=['CREATOR=My<App>&1'],
          expected_prefix=' creator="My&lt;App&gt;&amp;1" ',
      ),
  )
  def test_creator_option(
      self, options: Sequence[str], expected_prefix: str
  ) -> None:
    # Upstream: test_ogr_gpx_creator
    out_path = str(self.vsimem_dir / 'creator.gpx')
    with self.driver.Create(
        out_path, 0, 0, 0, gdal.GDT_Unknown, options=list(options)
    ) as ds:
      self.assertEqual(ds.GetDriver().ShortName, 'GPX')

    with self._open_gpx(out_path) as reopened_ds:
      self.assertEqual(reopened_ds.GetLayerCount(), 5)

    content = gcore_util.vsi_read_file_as_string(out_path, max_size=4096)
    self.assertIn(expected_prefix, content)

  @parameterized.named_parameters(
      dict(testcase_name='open_option', use_open_option=True),
      dict(testcase_name='config_option', use_open_option=False),
  )
  def test_ele_as_25d(self, use_open_option: bool) -> None:
    # Upstream: test_ogr_gpx_ELE_AS_25D
    test_gpx = ogr_util.GetTestFilePath('test.gpx')
    open_opts = ['ELE_AS_25D=YES'] if use_open_option else []
    config_val = None if use_open_option else 'YES'

    expected_first_features = (
        ('waypoints', ogr.wkbPoint25D, 'POINT Z (1 0 2)'),
        (
            'routes',
            ogr.wkbLineString25D,
            'LINESTRING Z (6 5 7,9 8 10,12 11 13)',
        ),
        (
            'tracks',
            ogr.wkbMultiLineString25D,
            'MULTILINESTRING Z ((15 14 16,18 17 19),(21 20 22,24 23 25))',
        ),
        ('route_points', ogr.wkbPoint25D, 'POINT Z (6 5 7)'),
        ('track_points', ogr.wkbPoint25D, 'POINT Z (15 14 16)'),
    )

    # Suppress the one-time "more than N_MAX_LINKS=2 links" warning triggered
    # on test.gpx waypoint 0 (static int once in ogrgpxlayer.cpp).
    with (
        gdal.quiet_warnings(),
        gdal.config_option('GPX_ELE_AS_25D', config_val),
        self._open_gpx(test_gpx, open_options=open_opts) as ds,
    ):
      for lyr_name, expected_geom_type, expected_wkt in expected_first_features:
        with self.subTest(layer=lyr_name):
          lyr = ds.GetLayerByName(lyr_name)
          self.assertEqual(lyr.GetGeomType(), expected_geom_type)
          feat = lyr.GetNextFeature()
          if feat is None:
            self.fail(f'Missing first feature in {lyr_name}')
          self.assertEqual(feat.GetGeometryRef().ExportToIsoWkt(), expected_wkt)

      # Waypoint feature 1 has no <ele>, so its geometry stays 2D.
      wpt_lyr = ds.GetLayerByName('waypoints')
      wpt_feat2 = wpt_lyr.GetNextFeature()
      if wpt_feat2 is None:
        self.fail('Missing second waypoint feature')
      self.assertEqual(
          wpt_feat2.GetGeometryRef().ExportToIsoWkt(), 'POINT (4 3)'
      )

  @parameterized.named_parameters(
      dict(testcase_name='open_option', use_open_option=True),
      dict(testcase_name='config_option', use_open_option=False),
  )
  def test_short_names(self, use_open_option: bool) -> None:
    # Upstream: test_ogr_gpx_SHORT_NAMES
    test_gpx = ogr_util.GetTestFilePath('test.gpx')
    open_opts = ['SHORT_NAMES=YES'] if use_open_option else []
    config_val = None if use_open_option else 'YES'

    with (
        gdal.config_option('GPX_SHORT_NAMES', config_val),
        self._open_gpx(test_gpx, open_options=open_opts) as ds,
    ):
      trkpt_lyr = ds.GetLayerByName('track_points')
      trkpt_defn = trkpt_lyr.GetLayerDefn()
      self.assertEqual(trkpt_defn.GetFieldIndex('track_seg_id'), -1)
      self.assertEqual(trkpt_defn.GetFieldIndex('track_seg_point_id'), -1)
      self._check_features_against_list(trkpt_lyr, 'trksegid', [0, 0, 1, 1])
      self._check_features_against_list(trkpt_lyr, 'trksegptid', [0, 1, 0, 1])

      rtept_lyr = ds.GetLayerByName('route_points')
      rtept_defn = rtept_lyr.GetLayerDefn()
      self.assertEqual(rtept_defn.GetFieldIndex('route_point_id'), -1)
      self._check_features_against_list(rtept_lyr, 'rteptid', [0, 1, 2])

  @parameterized.named_parameters(
      dict(testcase_name='open_option', use_open_option=True),
      dict(testcase_name='config_option', use_open_option=False),
  )
  def test_n_max_links(self, use_open_option: bool) -> None:
    # Upstream: test_ogr_gpx_N_MAX_LINKS
    test_gpx = ogr_util.GetTestFilePath('test.gpx')

    def _open_with_links(n_links: int) -> gdal.Dataset:
      open_opts = [f'N_MAX_LINKS={n_links}'] if use_open_option else []
      return self._open_gpx(test_gpx, open_options=open_opts)

    def _cfg_val(n_links: int) -> str | None:
      return None if use_open_option else str(n_links)

    with (
        gdal.config_option('GPX_N_MAX_LINKS', _cfg_val(3)),
        _open_with_links(3) as ds,
    ):
      wpt_lyr = ds.GetLayerByName('waypoints')
      self._check_features_against_list(wpt_lyr, 'link3_href', ['href3', None])
      self._check_features_against_list(wpt_lyr, 'link3_text', ['text3', None])
      self._check_features_against_list(wpt_lyr, 'link3_type', ['type3', None])

      trkpt_lyr = ds.GetLayerByName('track_points')
      self.assertGreaterEqual(
          trkpt_lyr.GetLayerDefn().GetFieldIndex('link3_href'), 0
      )
      self._check_features_against_list(
          trkpt_lyr, 'link3_href', [None, None, None, None]
      )

    with (
        gdal.config_option('GPX_N_MAX_LINKS', _cfg_val(0)),
        _open_with_links(0) as ds,
    ):
      wpt_defn = ds.GetLayerByName('waypoints').GetLayerDefn()
      self.assertEqual(wpt_defn.GetFieldIndex('link1_href'), -1)

    with (
        gdal.config_option('GPX_N_MAX_LINKS', _cfg_val(-1)),
        _open_with_links(-1) as ds,
    ):
      # Negative values fall back to default 2 links.
      wpt_defn = ds.GetLayerByName('waypoints').GetLayerDefn()
      self.assertGreaterEqual(wpt_defn.GetFieldIndex('link2_href'), 0)
      self.assertEqual(wpt_defn.GetFieldIndex('link3_href'), -1)

    with (
        gdal.config_option('GPX_N_MAX_LINKS', _cfg_val(101)),
        _open_with_links(101) as ds,
    ):
      # Values above 100 clamp to 100 links.
      wpt_defn = ds.GetLayerByName('waypoints').GetLayerDefn()
      self.assertGreaterEqual(wpt_defn.GetFieldIndex('link100_href'), 0)
      self.assertEqual(wpt_defn.GetFieldIndex('link101_href'), -1)

  def test_convert_to_gpkg(self) -> None:
    # Upstream: test_ogr_gpx_convert_to_gpkg (OSGeo/gdal#9225).
    out_path = str(self.vsimem_dir / 'out.gpkg')
    # Suppress the one-time "more than N_MAX_LINKS=2 links" warning triggered
    # on test.gpx waypoint 0 (static int once in ogrgpxlayer.cpp).
    with (
        gdal.quiet_warnings(),
        gdal.VectorTranslate(
            out_path, ogr_util.GetTestFilePath('test.gpx')
        ) as translated_ds,
    ):
      self.assertEqual(translated_ds.GetDriver().ShortName, 'GPKG')

    with gdal.OpenEx(
        out_path,
        gdal.OF_VECTOR | gdal.OF_READONLY,
        allowed_drivers=[ogr_util.GPKG_DRIVER],
    ) as ds:
      trk_lyr = ds.GetLayerByName('tracks')
      fids = [feat.GetFID() for feat in trk_lyr]
      self.assertEqual(fids, [0, 1, 2])
      self._check_features_against_list(
          trk_lyr, 'name', ['track name', 'empty track', 'empty track 2']
      )

      wpt_lyr = ds.GetLayerByName('waypoints')
      # Suppress GPKG's "Non-conformant content for record 0 in column time"
      # warning when reading back the +01:00 timezone timestamp.
      with gdal.quiet_warnings():
        gdal.ErrorReset()
        feat0 = wpt_lyr.GetNextFeature()
        self.assertIn('Non-conformant content', gdal.GetLastErrorMsg())
        self.assertIsNotNone(feat0)
        wpt_lyr.ResetReading()
        self.assertEqual([feat.GetFID() for feat in wpt_lyr], [0, 1])
        self._check_features_against_list(
            wpt_lyr, 'name', ['waypoint name', None]
        )
        self._check_features_against_list(
            wpt_lyr, 'time', ['2007/11/25 17:58:00+01', None]
        )

  def test_trkpt_extensions(self) -> None:
    # Upstream: test_ogr_gpx_trkpt_extensions (OSGeo/gdal#15443).
    with self.subTest(case='with_ogr_ns'):
      ns_gpx_path = self._write_vsimem_file(
          'trkpt_extensions_ns.gpx', _TRKPT_EXTENSIONS_WITH_OGR_NS_GPX
      )
      with self._open_gpx(ns_gpx_path) as ds:
        lyr = ds.GetLayerByName('track_points')
        lyr_defn = lyr.GetLayerDefn()
        hr_idx = lyr_defn.GetFieldIndex('gpxdata_hr')
        cadence_idx = lyr_defn.GetFieldIndex('gpxdata_cadence')
        self.assertGreaterEqual(hr_idx, 0)
        self.assertGreaterEqual(cadence_idx, 0)
        self.assertEqual(
            lyr_defn.GetFieldDefn(hr_idx).GetType(), ogr.OFTInteger
        )
        self.assertEqual(
            lyr_defn.GetFieldDefn(cadence_idx).GetType(), ogr.OFTInteger
        )
        self._check_features_against_list(lyr, 'gpxdata_hr', [91, 92])
        self._check_features_against_list(lyr, 'gpxdata_cadence', [81, 82])
        self._check_layer_geometries(lyr, ('POINT (2 49)', 'POINT (3 50)'))

    # Pin pre-OSGeo/gdal#15445 baseline behavior (ogrgpxdatasource.cpp only sets
    # m_bUseExtensions for depth-2 <extensions>, whereas ogrgpxlayer.cpp parses
    # <extensions> values only when m_poDS->GetUseExtensions() is true).
    # TODO: b/335317901 - Expect [91, 92] and [81, 82] for both None and 'YES'
    # once third_party/gdal imports OSGeo/gdal#15443 / #15445 (4ad9b065).
    gpx_path = self._write_vsimem_file(
        'trkpt_extensions.gpx', _TRKPT_EXTENSIONS_GPX
    )
    with (
        self.subTest(case='use_extensions_yes_pin'),
        gdal.config_option('GPX_USE_EXTENSIONS', 'YES'),
        self._open_gpx(gpx_path) as ds,
    ):
      lyr = ds.GetLayerByName('track_points')
      lyr_defn = lyr.GetLayerDefn()
      hr_idx = lyr_defn.GetFieldIndex('gpxdata_hr')
      cadence_idx = lyr_defn.GetFieldIndex('gpxdata_cadence')
      self.assertGreaterEqual(hr_idx, 0)
      self.assertGreaterEqual(cadence_idx, 0)
      self.assertEqual(lyr_defn.GetFieldDefn(hr_idx).GetType(), ogr.OFTInteger)
      self.assertEqual(
          lyr_defn.GetFieldDefn(cadence_idx).GetType(), ogr.OFTInteger
      )
      self._check_features_against_list(lyr, 'gpxdata_hr', [None, None])
      self._check_features_against_list(lyr, 'gpxdata_cadence', [None, None])

    with (
        self.subTest(case='use_extensions_default_pin'),
        gdal.config_option('GPX_USE_EXTENSIONS', None),
        self._open_gpx(gpx_path) as ds,
    ):
      lyr_defn = ds.GetLayerByName('track_points').GetLayerDefn()
      self.assertEqual(lyr_defn.GetFieldIndex('gpxdata_hr'), -1)
      self.assertEqual(lyr_defn.GetFieldIndex('gpxdata_cadence'), -1)


if __name__ == '__main__':
  googletest.main()
