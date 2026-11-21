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
# Copyright (c) 2012-2014, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT

"""Tests OGR handling of OpenStreetMap (OSM) XML and PBF datasets.

Format is described here:
  https://gdal.org/en/latest/drivers/vector/osm.html

This is a rewrite of:
  https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_osm.py
"""

import collections
from collections.abc import Mapping, Sequence
import configparser
import json
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

_SAMPLE_OSM_XML = textwrap.dedent("""\
    <?xml version="1.0" encoding="UTF-8"?>
    <osm version="0.6" generator="hand">
     <bounds minlat="49" minlon="2" maxlat="50" maxlon="3"/>
     <node id="1" lat="49" lon="2" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <node id="2" lat="50" lon="3" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <node id="3" lat="49.5" lon="3" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <tag k="name" v="Some interesting point"/>
      <tag k="foo" v="bar"/>
      <tag k="bar" v="baz"/>
     </node>
     <node id="4" lat="49" lon="3" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <node id="5" lat="50" lon="2" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <node id="6" lat="49.1" lon="2.1" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <node id="7" lat="49.1" lon="2.2" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <node id="8" lat="49.2" lon="2.2" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <node id="9" lat="49.2" lon="2.1" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z"/>
     <way id="1" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <nd ref="1"/>
      <nd ref="2"/>
      <tag k="highway" v="motorway"/>
      <tag k="foo" v="bar"/>
     </way>
     <way id="2" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <tag k="area" v="yes"/>
      <tag k="natural" v="wood"/>
      <nd ref="1"/>
      <nd ref="4"/>
      <nd ref="2"/>
      <nd ref="5"/>
      <nd ref="1"/>
     </way>
     <way id="3" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <tag k="area" v="yes"/>
      <nd ref="6"/>
      <nd ref="7"/>
      <nd ref="8"/>
      <nd ref="9"/>
      <nd ref="6"/>
     </way>
     <way id="4" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <tag k="name" v="id_4"/>
      <nd ref="600"/>
      <nd ref="700"/>
      <nd ref="800"/>
      <nd ref="900"/>
     </way>
     <way id="5" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <tag k="name" v="id_5"/>
      <nd ref="1"/>
     </way>
     <way id="6" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <tag k="name" v="id_6"/>
      <nd ref="1"/>
      <nd ref="4"/>
      <nd ref="2"/>
      <nd ref="5"/>
      <nd ref="900"/>
      <nd ref="1"/>
     </way>
     <way id="7" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <nd ref="1"/>
      <nd ref="2"/>
     </way>
     <way id="8" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <tag k="area" v="yes"/>
      <tag k="name" v="standalone_polygon"/>
      <nd ref="1"/>
      <nd ref="4"/>
      <nd ref="2"/>
      <nd ref="5"/>
      <nd ref="1"/>
     </way>
     <relation id="1" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <member type="way" ref="2" role="outer"/>
      <member type="way" ref="3" role="inner"/>
      <tag k="type" v="multipolygon"/>
      <tag k="natural" v="forest"/>
     </relation>
     <relation id="2" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <member type="way" ref="2" role="outer"/>
      <member type="way" ref="300" role="inner"/>
      <tag k="type" v="multipolygon"/>
     </relation>
     <relation id="3" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <member type="way" ref="1" role=""/>
      <tag k="type" v="route"/>
     </relation>
     <relation id="4" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <member type="node" ref="1" role=""/>
      <member type="way" ref="1" role=""/>
      <tag k="type" v="other_type"/>
     </relation>
     <relation id="5" user="some_user" uid="1" version="1" changeset="1" timestamp="2012-07-10T00:00:00Z">
      <member type="way" ref="2" role="outer"/>
      <member type="way" ref="3" role="inner"/>
      <tag k="type" v="multipolygon"/>
     </relation>
    </osm>
    """)

_BILLION_LAUGH_OSM_XML = textwrap.dedent("""\
    <?xml version="1.0"?>
    <!DOCTYPE lolz [
     <!ENTITY lol "lol">
     <!ENTITY lol1 "&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;">
     <!ENTITY lol2 "&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;&lol1;">
     <!ENTITY lol3 "&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;&lol2;">
     <!ENTITY lol4 "&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;&lol3;">
     <!ENTITY lol5 "&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;&lol4;">
     <!ENTITY lol6 "&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;&lol5;">
     <!ENTITY lol7 "&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;&lol6;">
     <!ENTITY lol8 "&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;&lol7;">
     <!ENTITY lol9 "&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;&lol8;">
    ]>
    <osm>&lol9;</osm>
    """)

_ALLTAGS_OSMCONF_INI = textwrap.dedent("""\
    [general]
    closed_ways_are_polygons=aeroway,amenity,boundary,building,natural

    [points]
    osm_id=yes
    attributes=name,barrier,highway,ref,address,is_in,place,man_made
    unsignificant=created_by,converted_by,source,time,ele
    ignore=created_by,converted_by,source,time,ele,note,openGeoDB:,fixme,FIXME
    all_tags=yes

    [lines]
    osm_id=yes
    attributes=name,highway,waterway,aerialway,barrier,man_made
    ignore=created_by,converted_by,source,time,ele,note,openGeoDB:,fixme,FIXME
    all_tags=yes
    computed_attributes=z_order
    z_order_type=Integer
    # Non-default SQL exercises the SQLite computed-attribute path.
    z_order_sql="SELECT (CASE [highway] WHEN 'motorway' THEN 42 ELSE 0 END)"

    [multipolygons]
    osm_id=yes
    attributes=name,type,natural

    [multilinestrings]
    osm_id=yes
    attributes=name,type

    [other_relations]
    osm_id=yes
    attributes=name,type
    """)


class OsmTest(parameterized.TestCase, gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(ogr.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())
    self.driver = gdal.GetDriverByName('OSM')
    self.assertIsNotNone(self.driver)

  def _testdata_path(self, rel_path: str) -> str:
    path = ogr_util.GetTestFilePath(rel_path)
    self.assertTrue(pathlib.Path(path).exists(), f'Missing testdata: {path}')
    return path

  def _vsimem_dir(self) -> pathlib.Path:
    return pathlib.Path(
        self.enter_context(
            gcore_util.VsimemCleanupDir(
                f'/vsimem/osm_test/{self._testMethodName}'
            )
        )
    )

  def _write_vsimem(self, path: pathlib.Path, content: str) -> str:
    path_str = str(path)
    gdal.FileFromMemBuffer(path_str, content)
    return path_str

  def _read_features(self, layer: ogr.Layer) -> list[ogr.Feature]:
    layer.ResetReading()
    return list(ogr_util.Features(layer))

  def _assert_geom_close(
      self,
      actual: ogr.Geometry | None,
      expected_wkt: str,
      max_error: float = 1e-9,
  ) -> None:
    self.assertIsNotNone(actual)
    expected = ogr.CreateGeometryFromWkt(expected_wkt)
    self._assert_geom_tree_close(actual, expected, max_error)

  def _assert_geom_tree_close(
      self,
      actual: ogr.Geometry,
      expected: ogr.Geometry,
      max_error: float,
  ) -> None:
    self.assertEqual(actual.GetGeometryName(), expected.GetGeometryName())
    self.assertEqual(actual.Is3D(), expected.Is3D())
    self.assertEqual(actual.IsMeasured(), expected.IsMeasured())
    self.assertEqual(actual.IsEmpty(), expected.IsEmpty())
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

  def _assert_sample_dataset(
      self,
      ds: gdal.Dataset,
      *,
      is_shapefile: bool = False,
      expected_points_extent: tuple[float, float, float, float] | None = None,
  ) -> None:
    wgs84 = osr.SpatialReference()
    wgs84.ImportFromEPSG(4326)
    wgs84.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)

    # Points layer
    points_lyr = ds.GetLayerByName('points')
    self.assertIsNotNone(points_lyr)
    self.assertEqual(points_lyr.GetGeomType(), ogr.wkbPoint)
    self.assertIsSame(points_lyr.GetSpatialRef(), wgs84)
    if expected_points_extent is not None:
      self.assertEqual(points_lyr.GetExtent(), expected_points_extent)

    point_feats = self._read_features(points_lyr)
    self.assertLen(point_feats, 1)
    pt = point_feats[0]
    self.assertEqual(pt.GetFieldAsString('osm_id'), '3')
    self.assertEqual(pt.GetFieldAsString('name'), 'Some interesting point')
    self.assertEqual(
        pt.GetFieldAsString('other_tags'), '"foo"=>"bar","bar"=>"baz"'
    )
    self._assert_geom_close(pt.GetGeometryRef(), 'POINT (3.0 49.5)')

    # Lines layer
    lines_lyr = ds.GetLayerByName('lines')
    self.assertIsNotNone(lines_lyr)
    self.assertEqual(lines_lyr.GetGeomType(), ogr.wkbLineString)
    line_feats = self._read_features(lines_lyr)
    self.assertLen(line_feats, 2)
    self.assertEqual(line_feats[0].GetFieldAsString('osm_id'), '1')
    self.assertEqual(line_feats[0].GetFieldAsString('highway'), 'motorway')
    self.assertEqual(line_feats[0].GetFieldAsInteger('z_order'), 9)
    self.assertEqual(
        line_feats[0].GetFieldAsString('other_tags'), '"foo"=>"bar"'
    )
    self._assert_geom_close(
        line_feats[0].GetGeometryRef(), 'LINESTRING (2 49,3 50)'
    )
    self.assertEqual(line_feats[1].GetFieldAsString('osm_id'), '6')
    self.assertEqual(line_feats[1].GetFieldAsString('name'), 'id_6')
    self._assert_geom_close(
        line_feats[1].GetGeometryRef(), 'LINESTRING (2 49,3 49,3 50,2 50,2 49)'
    )

    # Multipolygons layer (Shapefile emits Polygon and clockwise exterior ring)
    # TODO: b/335317901 - Pass SHAPE_PROMOTE_TO_MULTI=NO once
    # //third_party/gdal passes upstream commit fad10f7fc6.
    mp_lyr = ds.GetLayerByName('multipolygons')
    self.assertIsNotNone(mp_lyr)
    expected_mp_type = ogr.wkbPolygon if is_shapefile else ogr.wkbMultiPolygon
    self.assertEqual(mp_lyr.GetGeomType(), expected_mp_type)
    mp_feats = self._read_features(mp_lyr)
    self.assertLen(mp_feats, 3)

    ring_outer = (
        '(2 49,2 50,3 50,3 49,2 49)'
        if is_shapefile
        else '(2 49,3 49,3 50,2 50,2 49)'
    )
    ring_inner = '(2.1 49.1,2.2 49.1,2.2 49.2,2.1 49.2,2.1 49.1)'
    donut_wkt = (
        f'POLYGON ({ring_outer},{ring_inner})'
        if is_shapefile
        else f'MULTIPOLYGON (({ring_outer},{ring_inner}))'
    )
    standalone_wkt = (
        f'POLYGON ({ring_outer})'
        if is_shapefile
        else f'MULTIPOLYGON (({ring_outer}))'
    )

    self.assertEqual(mp_feats[0].GetFieldAsString('osm_id'), '1')
    self.assertEqual(mp_feats[0].GetFieldAsString('type'), 'multipolygon')
    self.assertEqual(mp_feats[0].GetFieldAsString('natural'), 'forest')
    self._assert_geom_close(mp_feats[0].GetGeometryRef(), donut_wkt)

    self.assertEqual(mp_feats[1].GetFieldAsString('osm_id'), '5')
    self.assertEqual(mp_feats[1].GetFieldAsString('type'), 'multipolygon')
    self.assertEqual(mp_feats[1].GetFieldAsString('natural'), 'wood')
    self._assert_geom_close(mp_feats[1].GetGeometryRef(), donut_wkt)

    self.assertEqual(mp_feats[2].GetFieldAsString('osm_way_id'), '8')
    self.assertEqual(mp_feats[2].GetFieldAsString('name'), 'standalone_polygon')
    self._assert_geom_close(mp_feats[2].GetGeometryRef(), standalone_wkt)

    # Multilinestrings layer
    mls_lyr = ds.GetLayerByName('multilinestrings')
    self.assertIsNotNone(mls_lyr)
    expected_mls_type = (
        ogr.wkbLineString if is_shapefile else ogr.wkbMultiLineString
    )
    self.assertEqual(mls_lyr.GetGeomType(), expected_mls_type)
    mls_feats = self._read_features(mls_lyr)
    self.assertLen(mls_feats, 1)
    self.assertEqual(mls_feats[0].GetFieldAsString('osm_id'), '3')
    self.assertEqual(mls_feats[0].GetFieldAsString('type'), 'route')
    expected_mls_wkt = (
        'LINESTRING (2 49,3 50)'
        if is_shapefile
        else 'MULTILINESTRING ((2 49,3 50))'
    )
    self._assert_geom_close(mls_feats[0].GetGeometryRef(), expected_mls_wkt)

    # Other relations layer
    other_lyr = ds.GetLayerByName('other_relations')
    if is_shapefile:
      self.assertIsNone(other_lyr)
    else:
      self.assertIsNotNone(other_lyr)
      self.assertEqual(other_lyr.GetGeomType(), ogr.wkbGeometryCollection)
      other_feats = self._read_features(other_lyr)
      self.assertLen(other_feats, 1)
      self.assertEqual(other_feats[0].GetFieldAsString('osm_id'), '4')
      self.assertEqual(other_feats[0].GetFieldAsString('type'), 'other_type')
      self._assert_geom_close(
          other_feats[0].GetGeometryRef(),
          'GEOMETRYCOLLECTION (POINT (2 49),LINESTRING (2 49,3 50))',
      )

      with ds.ExecuteSQL('GetBytesRead()') as sql_lyr:
        self.assertIsNotNone(sql_lyr)
        self.assertIsNotNone(sql_lyr.GetLayerDefn())
        self.assertFalse(sql_lyr.TestCapability('foo'))
        bytes_feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(bytes_feat)
        self.assertGreater(int(bytes_feat.GetFieldAsString(0)), 0)
        self.assertIsNone(sql_lyr.GetNextFeature())
        sql_lyr.ResetReading()
        self.assertIsNotNone(sql_lyr.GetNextFeature())

  def _assert_monotonic_progress(self, pcts: Sequence[float]) -> None:
    self.assertNotEmpty(pcts)
    self.assertGreaterEqual(pcts[0], 0.0)
    self.assertEqual(pcts[-1], 1.0)
    for prev, curr in zip(pcts, pcts[1:]):
      self.assertLessEqual(prev, curr)

  # Upstream: test_ogr_osm_18
  def test_two_points_pbf(self) -> None:
    filepath = self._testdata_path('osm/two_points.pbf')
    with gdal.OpenEx(filepath, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertEqual(ds.GetDriver().ShortName, 'OSM')
      self.assertEqual(ds.GetLayerCount(), 5)
      points_lyr = ds.GetLayerByName('points')
      self.assertIsNotNone(points_lyr)
      feats = self._read_features(points_lyr)
      self.assertLen(feats, 2)
      self.assertEqual(feats[0].GetFieldAsString('osm_id'), '1')
      self.assertEqual(feats[0].GetFieldAsString('other_tags'), '"foo"=>"bar"')
      self._assert_geom_close(feats[0].GetGeometryRef(), 'POINT (2 49)')
      self.assertEqual(feats[1].GetFieldAsString('osm_id'), '2')
      self.assertEqual(feats[1].GetFieldAsString('other_tags'), '"foo"=>"baz"')
      self._assert_geom_close(feats[1].GetGeometryRef(), 'POINT (2 49.1)')

  # Upstream: test_ogr_osm_1, test_ogr_osm_2, test_ogr_osm_limit_keys,
  # test_ogr_osm_limit_values_per_key,
  # test_ogr_osm_test_uncompressed_dense_true_nometadata_pbf,
  # test_ogr_osm_test_uncompressed_dense_false_pbf
  @parameterized.named_parameters(
      dict(
          testcase_name='pbf_default',
          pbf_relpath='osm/test.pbf',
          config_options=None,
          expected_points_extent=None,
      ),
      dict(
          testcase_name='xml_default',
          pbf_relpath=None,
          config_options=None,
          expected_points_extent=(2.0, 3.0, 49.0, 50.0),
      ),
      dict(
          testcase_name='xml_max_indexed_keys_zero',
          pbf_relpath=None,
          config_options={'OSM_MAX_INDEXED_KEYS': '0'},
          expected_points_extent=(2.0, 3.0, 49.0, 50.0),
      ),
      dict(
          testcase_name='xml_max_indexed_values_zero',
          pbf_relpath=None,
          config_options={'OSM_MAX_INDEXED_VALUES_PER_KEY': '0'},
          expected_points_extent=(2.0, 3.0, 49.0, 50.0),
      ),
      dict(
          testcase_name='pbf_uncompressed_dense_no_metadata',
          pbf_relpath='osm/test_uncompressed_dense_true_nometadata.pbf',
          config_options=None,
          expected_points_extent=None,
      ),
      dict(
          testcase_name='pbf_uncompressed_non_dense',
          pbf_relpath='osm/test_uncompressed_dense_false.pbf',
          config_options=None,
          expected_points_extent=None,
      ),
  )
  def test_read_sample_dataset(
      self,
      pbf_relpath: str | None,
      config_options: Mapping[str, str] | None,
      expected_points_extent: tuple[float, float, float, float] | None,
  ) -> None:
    if pbf_relpath is not None:
      filepath = self._testdata_path(pbf_relpath)
    else:
      filepath = self._write_vsimem(
          self._vsimem_dir() / 'sample.osm', _SAMPLE_OSM_XML
      )

    with (
        gdal.config_options(config_options or {}),
        gdal.OpenEx(filepath, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
    ):
      self.assertEqual(ds.GetDriver().ShortName, 'OSM')
      self._assert_sample_dataset(
          ds, expected_points_extent=expected_points_extent
      )

  # Upstream: test_ogr_osm_3, test_ogr_osm_3_sqlite_nodes,
  # test_ogr_osm_3_custom_compress_nodes, test_ogr_osm_3_all_layers
  @parameterized.named_parameters(
      dict(
          testcase_name='selected_layers',
          layers=('points', 'lines', 'multipolygons', 'multilinestrings'),
          skip_failures=False,
          config_options=None,
      ),
      dict(
          testcase_name='sqlite_node_indexing',
          layers=('points', 'lines', 'multipolygons', 'multilinestrings'),
          skip_failures=False,
          config_options={'OSM_USE_CUSTOM_INDEXING': 'NO'},
      ),
      dict(
          testcase_name='compressed_custom_nodes',
          layers=('points', 'lines', 'multipolygons', 'multilinestrings'),
          skip_failures=False,
          config_options={'OSM_COMPRESS_NODES': 'YES'},
      ),
      dict(
          testcase_name='all_layers_skip_failures',
          layers=None,
          skip_failures=True,
          config_options=None,
      ),
  )
  def test_vector_translate_to_shapefile(
      self,
      layers: Sequence[str] | None,
      skip_failures: bool,
      config_options: Mapping[str, str] | None,
  ) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    out_dir = str(self._vsimem_dir() / 'out_shp')

    err_ctx = gdal.quiet_errors() if skip_failures else gdal.quiet_warnings()
    with (
        gdal.config_options(config_options or {}),
        err_ctx,
        gdal.VectorTranslate(
            out_dir,
            src_path,
            format='ESRI Shapefile',
            layers=list(layers) if layers is not None else None,
            skipFailures=skip_failures,
        ) as out_ds,
    ):
      self.assertIsNotNone(out_ds)

    with gdal.OpenEx(out_dir, gdal.OF_VECTOR | gdal.OF_READONLY) as reopened:
      self._assert_sample_dataset(reopened, is_shapefile=True)

  # Upstream: test_ogr_osm_4
  def test_sql_layer_switching_and_spatial_filter(self) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    with gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      with ds.ExecuteSQL('SELECT * FROM points') as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetFieldAsString('osm_id'), '3')

      points_lyr = ds.GetLayerByName('points')
      self.assertIsNotNone(points_lyr)
      points_lyr.SetSpatialFilterRect(0, 0, 0, 0)
      points_lyr.ResetReading()
      self.assertIsNone(points_lyr.GetNextFeature())

      points_lyr.SetSpatialFilter(None)
      with ds.ExecuteSQL('SELECT * FROM lines') as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetFieldAsString('osm_id'), '1')

      with ds.ExecuteSQL('SELECT * FROM points') as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetFieldAsString('osm_id'), '3')

  # Upstream: test_ogr_osm_5
  def test_sql_early_attribute_filter(self) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    cases = (
        ('points', '3', True),
        ('points', 'foo', False),
        ('lines', '1', True),
        ('lines', 'foo', False),
        ('multipolygons', '1', True),
        ('multipolygons', 'foo', False),
        ('multilinestrings', '3', True),
        ('multilinestrings', 'foo', False),
    )
    with gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      for layer_name, osm_id, expect_match in cases:
        with self.subTest(layer=layer_name, osm_id=osm_id):
          with ds.ExecuteSQL(
              f"SELECT * FROM {layer_name} WHERE osm_id = '{osm_id}'"
          ) as sql_lyr:
            feats = self._read_features(sql_lyr)
            if expect_match:
              self.assertLen(feats, 1)
              self.assertEqual(feats[0].GetFieldAsString('osm_id'), osm_id)
            else:
              self.assertEmpty(feats)

      with self.subTest(layer='multipolygons', attr='type'):
        with ds.ExecuteSQL(
            "SELECT * FROM multipolygons WHERE type = 'multipolygon'"
        ) as sql_lyr:
          feats = self._read_features(sql_lyr)
          self.assertEqual(
              [
                  (
                      f.GetFieldAsString('osm_id'),
                      f.GetFieldAsString('natural'),
                  )
                  for f in feats
              ],
              [('1', 'forest'), ('5', 'wood')],
          )

  # Upstream: test_ogr_osm_6
  def test_vector_translate_sql_with_progress(self) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    out_dir = str(self._vsimem_dir() / 'out_sql_shp')
    progress_values: list[float] = []

    def _progress_cb(pct: float, unused_msg: str, unused_data: object) -> int:
      progress_values.append(pct)
      return 1

    with (
        gdal.quiet_warnings(),
        gdal.VectorTranslate(
            out_dir,
            src_path,
            format='ESRI Shapefile',
            SQLStatement='select * from multipolygons',
            callback=_progress_cb,
        ) as out_ds,
    ):
      self.assertIsNotNone(out_ds)

    self._assert_monotonic_progress(progress_values)

    with gdal.OpenEx(out_dir, gdal.OF_VECTOR | gdal.OF_READONLY) as reopened:
      self.assertEqual(reopened.GetLayerCount(), 1)
      lyr = reopened.GetLayer(0)
      feats = self._read_features(lyr)
      self.assertLen(feats, 3)
      self.assertEqual(feats[0].GetFieldAsString('osm_id'), '1')
      self.assertEqual(feats[0].GetFieldAsString('natural'), 'forest')
      self.assertEqual(feats[1].GetFieldAsString('osm_id'), '5')
      self.assertEqual(feats[1].GetFieldAsString('natural'), 'wood')
      self.assertEqual(feats[2].GetFieldAsString('osm_way_id'), '8')
      self.assertEqual(feats[2].GetFieldAsString('name'), 'standalone_polygon')

  # Upstream: test_ogr_osm_7
  def test_sqlite_dialect_limit_query(self) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    with gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      with ds.ExecuteSQL(
          'SELECT * FROM points LIMIT 10', dialect='SQLite'
      ) as sql_lyr:
        self.assertIsNotNone(sql_lyr)
        self.assertEqual(sql_lyr.GetFeatureCount(), 1)
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetFieldAsString('osm_id'), '3')

    # Confirm GetFeatureCount()'s SQLite subquery reopened src_path and drained
    # the process-global oListDSToBeOpened entry rather than leaking
    # interest_layers=points to the next open of src_path.
    with gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as reopened:
      self.assertLen(self._read_features(reopened.GetLayerByName('lines')), 2)

  # Upstream: test_ogr_osm_8, test_ogr_osm_9
  @parameterized.named_parameters(
      dict(testcase_name='custom_indexing', use_custom_indexing='YES'),
      dict(testcase_name='sqlite_indexing', use_custom_indexing='NO'),
  )
  def test_64bit_ids(self, use_custom_indexing: str) -> None:
    src_path = self._testdata_path('osm/base-64.osm.pbf')
    with (
        gdal.config_option('OSM_USE_CUSTOM_INDEXING', use_custom_indexing),
        gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
    ):
      points_lyr = ds.GetLayerByName('points')
      self.assertEqual(
          points_lyr.SetAttributeFilter("osm_id = '4294967934'"),
          ogr.OGRERR_NONE,
      )
      pt = points_lyr.GetNextFeature()
      self.assertIsNotNone(pt)
      self.assertEqual(pt.GetField('name'), 'Treetops')
      self._assert_geom_close(
          pt.GetGeometryRef(), 'POINT (-61.7964321 17.1498319)'
      )

      mp_lyr = ds.GetLayerByName('multipolygons')
      mp = mp_lyr.GetFeature(1113)
      self.assertIsNotNone(mp)
      self._assert_geom_close(
          mp.GetGeometryRef(),
          'MULTIPOLYGON (((-61.7780345 17.140634,-61.7777002 17.1406069,'
          '-61.7776854 17.1407739,-61.7779131 17.1407923,'
          '-61.7779158 17.1407624,-61.7780224 17.140771,'
          '-61.7780345 17.140634)))',
      )

  # Upstream: test_ogr_osm_10
  def test_error_conditions(self) -> None:
    vsi_dir = self._vsimem_dir()

    with self.subTest(case='nonexistent_file'):
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'No such file or directory'),
      ):
        gdal.OpenEx(
            str(vsi_dir / 'missing.osm'), gdal.OF_VECTOR | gdal.OF_READONLY
        )

    for filename in ('empty.osm', 'empty.pbf'):
      with self.subTest(case=filename):
        empty_path = self._write_vsimem(vsi_dir / filename, '')
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(RuntimeError, r'not recognized'),
        ):
          gdal.OpenEx(empty_path, gdal.OF_VECTOR | gdal.OF_READONLY)

    with self.subTest(case='truncated_xml'):
      bad_osm = self._write_vsimem(vsi_dir / 'truncated.osm', '<osm>')
      with gdal.OpenEx(bad_osm, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
        lyr = ds.GetLayer(0)
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(
                RuntimeError,
                r'An error occurred during the parsing[\s\S]*'
                r'XML parsing of OSM file failed',
            ),
        ):
          lyr.GetNextFeature()

    with self.subTest(case='invalid_pbf'):
      bad_pbf = self._write_vsimem(vsi_dir / 'bad.pbf', 'OSMHeader\n')
      with gdal.OpenEx(bad_pbf, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
        lyr = ds.GetLayer(0)
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(
                RuntimeError, r'An error occurred during the parsing'
            ),
        ):
          lyr.GetNextFeature()

    with self.subTest(case='billion_laughs_entity_expansion'):
      laughs_osm = self._write_vsimem(
          vsi_dir / 'billionlaugh.osm', _BILLION_LAUGH_OSM_XML
      )
      with gdal.OpenEx(laughs_osm, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
        lyr = ds.GetLayer(0)
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(
                RuntimeError,
                # Confirms osm_parser.cpp's nDataHandlerCounter guard fires.
                r'An error occurred during the parsing[\s\S]*'
                r'million laugh pattern',
            ),
        ):
          lyr.GetNextFeature()

  # Upstream: test_ogr_osm_11
  def test_all_tags_config_file(self) -> None:
    ini_path = self._write_vsimem(
        self._vsimem_dir() / 'osmconf_alltags.ini', _ALLTAGS_OSMCONF_INI
    )
    src_path = self._testdata_path('osm/test.pbf')

    with (
        gdal.config_option('OSM_CONFIG_FILE', ini_path),
        gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
    ):
      points_lyr = ds.GetLayerByName('points')
      self.assertEqual(
          points_lyr.GetLayerDefn().GetFieldIndex('other_tags'), -1
      )
      pt = points_lyr.GetNextFeature()
      self.assertIsNotNone(pt)
      self.assertEqual(pt.GetFieldAsString('osm_id'), '3')
      self.assertEqual(pt.GetFieldAsString('name'), 'Some interesting point')
      self.assertEqual(
          pt.GetFieldAsString('all_tags'),
          '"name"=>"Some interesting point","foo"=>"bar","bar"=>"baz"',
      )

      lines_lyr = ds.GetLayerByName('lines')
      self.assertEqual(lines_lyr.GetLayerDefn().GetFieldIndex('other_tags'), -1)
      line_feats = self._read_features(lines_lyr)
      self.assertLen(line_feats, 2)
      self.assertEqual(line_feats[0].GetFieldAsString('osm_id'), '1')
      self.assertEqual(line_feats[0].GetFieldAsInteger('z_order'), 42)
      self.assertEqual(
          line_feats[0].GetFieldAsString('all_tags'),
          '"highway"=>"motorway","foo"=>"bar"',
      )
      self.assertEqual(line_feats[1].GetFieldAsString('osm_id'), '6')
      self.assertEqual(line_feats[1].GetFieldAsInteger('z_order'), 0)
      self.assertEqual(
          line_feats[1].GetFieldAsString('all_tags'), '"name"=>"id_6"'
      )

  # Upstream: test_ogr_osm_12
  def test_attribute_filter_on_lines_layer(self) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    with gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      points_lyr = ds.GetLayerByIndex(0)
      lines_lyr = ds.GetLayerByIndex(1)
      self.assertEqual(
          points_lyr.SetAttributeFilter('highway IS NOT NULL'), ogr.OGRERR_NONE
      )
      self.assertEqual(
          lines_lyr.SetAttributeFilter('highway IS NOT NULL'), ogr.OGRERR_NONE
      )
      self.assertEmpty(self._read_features(points_lyr))
      lines_feats = self._read_features(lines_lyr)
      self.assertLen(lines_feats, 1)
      self.assertEqual(lines_feats[0].GetFieldAsString('osm_id'), '1')
      self.assertEqual(lines_feats[0].GetFieldAsString('highway'), 'motorway')

  # Upstream: test_ogr_osm_13
  def test_osm_id_tag_preserved_in_other_tags(self) -> None:
    osm_path = self._write_vsimem(
        self._vsimem_dir() / 'osm_id_tag.osm',
        '<osm version="0.6">'
        '<node id="123" lon="2" lat="49"><tag k="osm_id" v="0"/></node>'
        '</osm>',
    )
    with gdal.OpenEx(osm_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['osm_id'], '123')
      self.assertEqual(feat['other_tags'], '"osm_id"=>"0"')

  # Upstream: test_ogr_osm_14
  def test_polygon_in_other_relations(self) -> None:
    osm_path = self._write_vsimem(
        self._vsimem_dir() / 'polygon_rel.osm',
        textwrap.dedent("""\
            <osm version="0.6">
              <node id="1" lat="2" lon="49"/>
              <node id="2" lat="2.1" lon="49"/>
              <node id="3" lat="2.1" lon="49.1"/>
              <way id="1">
                <nd ref="1"/>
                <nd ref="2"/>
                <nd ref="3"/>
                <nd ref="1"/>
                <tag k="area" v="yes"/>
                <tag k="bus" v="yes"/>
              </way>
              <relation id="1">
                <member type="way" ref="1" role=""/>
                <tag k="route" v="bus"/>
              </relation>
            </osm>
            """),
    )
    with gdal.OpenEx(osm_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      with ds.ExecuteSQL('SELECT * FROM other_relations') as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self._assert_geom_close(
            feat.GetGeometryRef(),
            'GEOMETRYCOLLECTION (POLYGON ((49 2,49.0 2.1,49.1 2.1,49 2)))',
        )

  # Upstream: test_ogr_osm_15
  def test_dataset_get_next_feature_interleaved_and_callbacks(self) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    with gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      self.assertTrue(ds.TestCapability(ogr.ODsCRandomLayerRead))

      count = 0
      last_pct = 0.0
      layer_counts: collections.Counter[str] = collections.Counter()
      while True:
        feat, lyr, pct = ds.GetNextFeature(include_pct=True)
        self.assertGreaterEqual(pct, last_pct)
        last_pct = pct
        if feat is None:
          self.assertIsNone(lyr)
          break
        self.assertIsNotNone(lyr)
        self.assertEqual(feat.GetDefnRef().GetName(), lyr.GetName())
        layer_counts[lyr.GetName()] += 1
        count += 1

      self.assertEqual(count, 8)
      self.assertEqual(
          layer_counts,
          {
              'points': 1,
              'lines': 2,
              'multipolygons': 3,
              'multilinestrings': 1,
              'other_relations': 1,
          },
      )
      self.assertEqual(last_pct, 1.0)
      self.assertEqual(ds.GetNextFeature(include_pct=True), [None, None, 1.0])

      ds.ResetReading()
      for _ in range(count):
        feat, lyr = ds.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertIsNotNone(lyr)
      self.assertEqual(ds.GetNextFeature(), [None, None])

      def _cancel_cb(
          unused_pct: float, unused_msg: str, unused_data: object
      ) -> int:
        return 0

      ds.ResetReading()
      self.assertEqual(ds.GetNextFeature(callback=_cancel_cb), [None, None])

      recorded_pcts: list[float] = []

      def _record_cb(
          pct: float, unused_msg: str, user_data: list[float]
      ) -> int:
        user_data.append(pct)
        return 1

      ds.ResetReading()
      feat, lyr = ds.GetNextFeature(
          callback=_record_cb, callback_data=recorded_pcts
      )
      self.assertIsNotNone(feat)
      self.assertIsNotNone(lyr)
      self.assertEqual(lyr.GetName(), 'points')
      self._assert_monotonic_progress(recorded_pcts)

  # Upstream: test_ogr_osm_16
  def test_attribute_name_laundering_open_option_config(self) -> None:
    vsi_dir = self._vsimem_dir()
    osm_path = self._write_vsimem(
        vsi_dir / 'launder.osm',
        textwrap.dedent("""\
            <osm version="0.6">
              <node id="1" lat="2" lon="49">
                <tag k="foo:baar" v="val"/>
                <tag k="foo:bar" v="val2"/>
              </node>
            </osm>
            """),
    )
    conf_path = self._write_vsimem(
        vsi_dir / 'conf.ini',
        textwrap.dedent("""\
            attribute_name_laundering=yes

            [points]
            attributes=foo:baar,foo:bar
            """),
    )
    with gdal.OpenEx(
        osm_path,
        gdal.OF_VECTOR | gdal.OF_READONLY,
        open_options=[f'CONFIG_FILE={conf_path}'],
    ) as ds:
      lyr = ds.GetLayerByName('points')
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['foo_baar'], 'val')
      self.assertEqual(feat['foo_bar'], 'val2')

  # Upstream: test_ogr_osm_17
  def test_translate_empty_osm(self) -> None:
    vsi_dir = self._vsimem_dir()
    empty_osm = self._write_vsimem(
        vsi_dir / 'empty.osm', '<osm version="0.6"/>'
    )
    out_dir = str(vsi_dir / 'out_empty_shp')

    with (
        gdal.quiet_errors(),
        gdal.VectorTranslate(
            out_dir, empty_osm, format='ESRI Shapefile', skipFailures=True
        ) as out_ds,
    ):
      self.assertIsNotNone(out_ds)

    with gdal.OpenEx(out_dir, gdal.OF_VECTOR | gdal.OF_READONLY) as reopened:
      layer_names = [
          reopened.GetLayer(idx).GetName()
          for idx in range(reopened.GetLayerCount())
      ]
      self.assertCountEqual(
          layer_names,
          ['points', 'lines', 'multilinestrings', 'multipolygons'],
      )
      for name in layer_names:
        with self.subTest(layer=name):
          self.assertEqual(reopened.GetLayerByName(name).GetFeatureCount(), 0)

  # Upstream: test_ogr_osm_tags_json
  def test_tags_format_json(self) -> None:
    src_path = self._testdata_path('osm/test.pbf')
    with gdal.OpenEx(
        src_path,
        gdal.OF_VECTOR | gdal.OF_READONLY,
        open_options=['TAGS_FORMAT=JSON'],
    ) as ds:
      points_lyr = ds.GetLayerByName('points')
      points_idx = points_lyr.GetLayerDefn().GetFieldIndex('other_tags')
      self.assertGreaterEqual(points_idx, 0)
      points_fld = points_lyr.GetLayerDefn().GetFieldDefn(points_idx)
      self.assertEqual(points_fld.GetType(), ogr.OFTString)
      self.assertEqual(points_fld.GetSubType(), ogr.OFSTJSON)
      pt = points_lyr.GetNextFeature()
      self.assertIsNotNone(pt)
      self.assertEqual(pt['other_tags'], '{"foo":"bar","bar":"baz"}')
      self.assertEqual(
          json.loads(pt['other_tags']), {'foo': 'bar', 'bar': 'baz'}
      )

      lines_lyr = ds.GetLayerByName('lines')
      lines_idx = lines_lyr.GetLayerDefn().GetFieldIndex('other_tags')
      self.assertGreaterEqual(lines_idx, 0)
      lines_fld = lines_lyr.GetLayerDefn().GetFieldDefn(lines_idx)
      self.assertEqual(lines_fld.GetType(), ogr.OFTString)
      self.assertEqual(lines_fld.GetSubType(), ogr.OFSTJSON)
      line = lines_lyr.GetNextFeature()
      self.assertIsNotNone(line)
      self.assertEqual(line['other_tags'], '{"foo":"bar"}')
      self.assertEqual(json.loads(line['other_tags']), {'foo': 'bar'})

  # Upstream: test_ogr_osm_tags_json_special_characters
  def test_tags_format_json_special_characters(self) -> None:
    src_path = self._testdata_path('osm/test_json.pbf')
    with gdal.OpenEx(
        src_path,
        gdal.OF_VECTOR | gdal.OF_READONLY,
        open_options=['TAGS_FORMAT=JSON'],
    ) as ds:
      lyr = ds.GetLayerByName('points')
      other_idx = lyr.GetLayerDefn().GetFieldIndex('other_tags')
      self.assertGreaterEqual(other_idx, 0)
      fld = lyr.GetLayerDefn().GetFieldDefn(other_idx)
      self.assertEqual(fld.GetType(), ogr.OFTString)
      self.assertEqual(fld.GetSubType(), ogr.OFSTJSON)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['other_tags'], r"""{"foo":"x'\\\"\t\n\ry"}""")
      self.assertEqual(json.loads(feat['other_tags']), {'foo': 'x\'\\"\t\n\ry'})

  # Upstream: test_ogr_osmconf_ini
  # Google3 embeds data/osmconf.ini in /vsimem/gdal_data/ via
  # GoogleGdalInitSupportFiles() instead of EMBED_RESOURCE_FILES, so read it
  # through GDAL's VSI helper rather than Python's open().
  def test_default_osmconf_ini_is_valid(self) -> None:
    src_path = self._testdata_path('osm/test_json.pbf')
    with gdal.OpenEx(src_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds:
      with ds.ExecuteSQL('SHOW config_file_path') as sql_lyr:
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        osmconf_path = feat.GetFieldAsString(0)
        self.assertEqual(osmconf_path, '/vsimem/gdal_data/osmconf.ini')
        ini_text = gcore_util.vsi_read_file_as_string(osmconf_path, 65536)
        self.assertLess(len(ini_text), 65536)
        config = configparser.ConfigParser()
        config.read_string(ini_text)
        self.assertEqual(
            set(config.sections()),
            {
                'general',
                'points',
                'lines',
                'multipolygons',
                'multilinestrings',
                'other_relations',
            },
        )
        self.assertIn('closed_ways_are_polygons', config['general'])


# TODO: b/335317901 - Port test_ogr_osm_parse_complex_multipolygon (gh13610)
# and test_ogr_osm_parse_fix_gh14532 once //third_party/gdal is past upstream
# commits 0afc377d25 and 993ce15f94 (ReadOSMInfo upper bound at
# osm_parser.cpp:1045 is unfixed at c5a57bae).


if __name__ == '__main__':
  googletest.main()
