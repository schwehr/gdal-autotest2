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
# Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2012-2014, Even Rouault <even.rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT

"""Tests OGR handling of MapInfo TAB and MIF/MID files.

Format is described here:
  https://gdal.org/en/latest/drivers/vector/mitab.html

Rewrite of:
  https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_mitab.py
"""

from collections.abc import Iterator, Sequence
import contextlib
import os
import pathlib
import random
import shutil
import zipfile

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

DRIVER = ogr_util.MAPINFO_DRIVER

_NZGD49_WKT = (
    'PROJCS["NZGD49 / UTM zone 59S",GEOGCS["NZGD49",'
    'DATUM["NZGD49",SPHEROID["International 1924",6378388,297,'
    'AUTHORITY["EPSG","7022"]],TOWGS84[59.47,-5.04,187.44,0.47,'
    '-0.1,1.024,-4.5993],AUTHORITY["EPSG","6272"]],'
    'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
    'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
    'AUTHORITY["EPSG","4272"]],PROJECTION["Transverse_Mercator"],'
    'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",171],'
    'PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],'
    'PARAMETER["false_northing",10000000],UNIT["metre",1,'
    'AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],AXIS["Northing",NORTH],'
    'AUTHORITY["EPSG","27259"]]'
)

_LAMBERT93_FRENCH_BOUNDS_WKT = (
    'PROJCS["RGF93 / Lambert-93",'
    'GEOGCS["RGF93",'
    'DATUM["Reseau_Geodesique_Francais_1993",'
    'SPHEROID["GRS 80",6378137,298.257222101],'
    'TOWGS84[0,0,0,0,0,0,0]],'
    'PRIMEM["Greenwich",0],'
    'UNIT["degree",0.0174532925199433]],'
    'PROJECTION["Lambert_Conformal_Conic_2SP"],'
    'PARAMETER["standard_parallel_1",49.00000000002],'
    'PARAMETER["standard_parallel_2",44],'
    'PARAMETER["latitude_of_origin",46.5],'
    'PARAMETER["central_meridian",3],'
    'PARAMETER["false_easting",700000],'
    'PARAMETER["false_northing",6600000],'
    'UNIT["Meter",1.0],'
    'AUTHORITY["EPSG","2154"]]'
)

_LAMBERT93_EUROPE_BOUNDS_WKT = (
    'PROJCS["RGF93 / Lambert-93",'
    'GEOGCS["RGF93",'
    'DATUM["Reseau_Geodesique_Francais_1993",'
    'SPHEROID["GRS 80",6378137,298.257222101],'
    'TOWGS84[0,0,0,0,0,0,0]],'
    'PRIMEM["Greenwich",0],'
    'UNIT["degree",0.0174532925199433]],'
    'PROJECTION["Lambert_Conformal_Conic_2SP"],'
    'PARAMETER["standard_parallel_1",49.00000000001],'
    'PARAMETER["standard_parallel_2",44],'
    'PARAMETER["latitude_of_origin",46.5],'
    'PARAMETER["central_meridian",3],'
    'PARAMETER["false_easting",700000],'
    'PARAMETER["false_northing",6600000],'
    'UNIT["Meter",1.0],'
    'AUTHORITY["EPSG","2154"]]'
)

_MITAB_BOUNDS_TXT = (
    '# Map generic Lambert 93 to French Lambert 93, Europe bounds\n'
    'Source = CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, 49, '
    '700000, 6600000\n'
    'Destination=CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, '
    '49.00000000001, 700000, 6600000 Bounds (-792421, 5278231) '
    '(3520778, 9741029)\n\n'
    '# French Lambert 93, French bounds\n'
    'CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, 49.00000000002, '
    '700000, 6600000 Bounds (75000, 6000000) (1275000, 7200000)\n\n'
    '# French Lambert 93, Europe bounds\n'
    'CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, 49.00000000001, '
    '700000, 6600000 Bounds (-792421, 5278231) (3520778, 9741029)\n'
)

_TESTLYRDEF_GML = """<?xml version="1.0" encoding="utf-8" ?>
<ogr:FeatureCollection
     xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:schemaLocation="http://ogr.maptools.org/ testlyrdef.xsd"
     xmlns:ogr="http://ogr.maptools.org/"
     xmlns:gml="http://www.opengis.net/gml">
    <gml:featureMember>
        <gml:testlyrdef>
            <gml:AREA>12.4567</gml:AREA>
            <gml:VOLUME>12345.09876</gml:VOLUME>
            <gml:LENGTH>1234567890</gml:LENGTH>
            <gml:WIDTH>1234</gml:WIDTH>
        </gml:testlyrdef>
    </gml:featureMember>
</ogr:FeatureCollection>
"""

_TESTLYRDEF_XSD = """<?xml version="1.0" encoding="UTF-8"?>
<xs:schema targetNamespace="http://ogr.maptools.org/"
    xmlns:ogr="http://ogr.maptools.org/"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:gml="http://www.opengis.net/gml"
    elementFormDefault="qualified" version="1.0">
<xs:element name="FeatureCollection" type="ogr:FeatureCollectionType"
    substitutionGroup="gml:_FeatureCollection"/>
<xs:complexType name="FeatureCollectionType">
  <xs:complexContent>
    <xs:extension base="gml:AbstractFeatureCollectionType">
      <xs:attribute name="lockId" type="xs:string" use="optional"/>
      <xs:attribute name="scope" type="xs:string" use="optional"/>
    </xs:extension>
  </xs:complexContent>
</xs:complexType>
<xs:element name="testlyrdef" type="ogr:testlyrdef_Type"
    substitutionGroup="gml:_Feature"/>
<xs:complexType name="testlyrdef_Type">
  <xs:complexContent>
    <xs:extension base="gml:AbstractFeatureType">
      <xs:sequence>
        <xs:element name="geometryProperty" type="gml:GeometryPropertyType"
            nillable="true" minOccurs="0" maxOccurs="1"/>
        <xs:element name="AREA" nillable="true" minOccurs="0" maxOccurs="1">
          <xs:simpleType>
            <xs:restriction base="xs:decimal">
              <xs:totalDigits value="7"/>
              <xs:fractionDigits value="4"/>
            </xs:restriction>
          </xs:simpleType>
        </xs:element>
        <xs:element name="VOLUME" nillable="true" minOccurs="0" maxOccurs="1">
          <xs:simpleType>
            <xs:restriction base="xs:decimal"/>
          </xs:simpleType>
        </xs:element>
        <xs:element name="LENGTH" nillable="true" minOccurs="0" maxOccurs="1">
          <xs:simpleType>
            <xs:restriction base="xs:integer">
              <xs:totalDigits value="10"/>
            </xs:restriction>
          </xs:simpleType>
        </xs:element>
        <xs:element name="WIDTH" nillable="true" minOccurs="0" maxOccurs="1">
          <xs:simpleType>
            <xs:restriction base="xs:integer">
              <xs:totalDigits value="4"/>
            </xs:restriction>
          </xs:simpleType>
        </xs:element>
      </xs:sequence>
    </xs:extension>
  </xs:complexContent>
</xs:complexType>
</xs:schema>
"""

_MULTILINE_MIF = (
    'Version   410\n'
    'Charset "WindowsLatin1"\n'
    'Delimiter ","\n'
    'Index 1,2\n'
    'CoordSys Earth Projection 8, 12, "m", 153, 0, 0.9996, 500000, '
    '10000000 Bounds (-7745874.38492, 1999.40969607) '
    '(8745874.38492, 19998000.5903)\n'
    'Columns 3\n'
    '  Name Char(51)\n'
    '  Notes Char(100)\n'
    '  Awesome Logical\n'
    'Data\n\n'
    'Pline 3\n'
    '482281.14 6738543.72\n'
    '482278.29 6738548.02\n'
    '482276.11 6738551.17\n'
    '    Pen (4,104,8421504)\n\n'
    'Pline 3\n'
    '483281.14 6738543.72\n'
    '483278.29 6738548.02\n'
    '483276.11 6738551.17\n'
    '    Pen (4,104,8421504)\n'
)

_MULTILINE_MID = '"NAME1","MULTI\n\nLINE",F\n"NAME2","MULTI\nLINE2",F\n'

_SINGLE_FIELD_MIF = """Version 300
Charset "Neutral"
Delimiter ","
Columns 1
  foo Char(254)
Data

NONE
NONE
NONE
"""

_SINGLE_FIELD_MID = '"1"\n\n"3"\n'

_ALL_FIELDS_MIF = (
    'Version   900\n'
    'Charset "WindowsLatin1"\n'
    'Delimiter ","\n'
    'Columns 10\n'
    '  field1 Char(10)\n'
    '  Field2 Integer\n'
    '  Field3 Smallint\n'
    '  Field4 LargeInt\n'
    '  Field5 Float\n'
    '  Field6 Decimal(10, 2)\n'
    '  Field7 Date\n'
    '  Field8 Time\n'
    '  Field9 DateTime\n'
    '  Field10 Logical\n'
    'Data\n\n'
    'none\n'
    'none\n'
)

_ALL_FIELDS_MID = (
    '"test",120,12345,123456789012345,12.34,12.34,20221231,235900000,'
    '20220323145600000,T\n'
    '"test",120,12345,123456789012345,12.34,12.34,20221231,235900000,'
    '20220323145600000,F\n'
)

_PROJ_34_MIF = (
    'Version  1200\n'
    'Charset "WindowsLatin1"\n'
    'Delimiter ","\n'
    'CoordSys Earth Projection 34, 104, "m", -177, 0, 0.9996, 500000, 0 '
    'Bounds (-7919730.23343, -9997964.94302) '
    '(8919730.23343, 9997964.94302)\n'
    'Columns 1\n'
    '  aa Integer\n'
    'Data\n\n'
    'Pline 5\n'
    '499950.16 6677.74\n'
    '494302.31 4086.39\n'
    '495996.66 -265.79\n'
    '501710.93 66.45\n'
    '501910.24 -5049.84\n'
    '    Pen (1,2,0)\n'
)

_PROJ_35_MIF = (
    'Version  1252\n'
    'Charset "WindowsLatin1"\n'
    'Delimiter ","\n'
    'CoordSys Earth Projection 35, 164, "m", 102.25, 4, 323.025796466667, '
    '323.130102361111, 0.99984, 804671, 123456 '
    'Bounds (-1661180.69223, -10630474.7061) '
    '(3270522.69223, 10630474.7061)\n'
    'Columns 1\n'
    '  aa Char(10)\n'
    'Data\n\n'
    'Pline 4\n'
    '806313.58 7382.8\n'
    '800485.06 3002.58\n'
    '802710.48 -2402.06\n'
    '806136.95 -1201.03\n'
    '    Pen (1,2,0)\n'
)

_MIF_EARTH_PROJECTIONS = (
    'CoordSys Earth Projection 1, 104',
    'CoordSys Earth Projection 2, 104, "survey ft", 1, 2',
    'CoordSys Earth Projection 3, 104, "ft", 1, 2, 3, 4, 5, 6',
    'CoordSys Earth Projection 4, 104, "m", 1, 90, 90',
    'CoordSys Earth Projection 5, 104, "m", 1, 90, 90',
    'CoordSys Earth Projection 6, 104, "m", 1, 2, 3, 4, 5, 6',
    'CoordSys Earth Projection 7, 104, "m", 1, 2, 3, 4, 5, 6',
    'CoordSys Earth Projection 8, 104, "m", 1, 2, 3, 4, 5',
    'CoordSys Earth Projection 9, 104, "m", 1, 2, 3, 4, 5, 6',
    'CoordSys Earth Projection 10, 104, "m", 1',
    'CoordSys Earth Projection 11, 104, "m", 1',
    'CoordSys Earth Projection 12, 104, "m", 1',
    'CoordSys Earth Projection 13, 104, "m", 1',
    'CoordSys Earth Projection 14, 104, "m", 1',
    'CoordSys Earth Projection 15, 104, "m", 1',
    'CoordSys Earth Projection 16, 104, "m", 1',
    'CoordSys Earth Projection 17, 104, "m", 1',
    'CoordSys Earth Projection 18, 104, "m", 1, 2, 3, 4',
    'CoordSys Earth Projection 19, 104, "m", 1, 2, 3, 4, 5, 6',
    'CoordSys Earth Projection 20, 104, "m", 1, 2, 3, 4, 5',
    'CoordSys Earth Projection 25, 104, "m", 1, 2, 3, 4',
    'CoordSys Earth Projection 26, 104, "m", 1, 2',
    'CoordSys Earth Projection 27, 104, "m", 1, 2, 3, 4',
    'CoordSys Earth Projection 28, 104, "m", 1, 2, 90',
    'CoordSys Earth Projection 30, 104, "m", 1, 2, 3, 4',
    'CoordSys Earth Projection 31, 104, "m", 1, 2, 3, 4, 5',
    'CoordSys Earth Projection 32, 104, "m", 1, 2, 3, 4, 5, 6',
    'CoordSys Earth Projection 33, 104, "m", 1, 2, 3, 4',
)

_MIF_NONEARTH_UNITS = (
    'mi',
    'km',
    'in',
    'ft',
    'yd',
    'mm',
    'cm',
    'm',
    'survey ft',
    'nmi',
    'li',
    'ch',
    'rd',
)


@contextlib.contextmanager
def _attribute_filter(layer: ogr.Layer, query: str) -> Iterator[None]:
  layer.SetAttributeFilter(query)
  try:
    yield
  finally:
    layer.SetAttributeFilter(None)


@contextlib.contextmanager
def _spatial_filter_rect(
    layer: ogr.Layer, min_x: float, min_y: float, max_x: float, max_y: float
) -> Iterator[None]:
  layer.SetSpatialFilterRect(min_x, min_y, max_x, max_y)
  try:
    yield
  finally:
    layer.SetSpatialFilter(None)


@contextlib.contextmanager
def _capture_warnings() -> Iterator[list[str]]:
  warnings: list[str] = []

  def _handler(err_class: int, unused_err_num: int, msg: str) -> None:
    if err_class == gdal.CE_Warning:
      warnings.append(msg)

  gdal.PushErrorHandler(_handler)
  try:
    yield warnings
  finally:
    gdal.PopErrorHandler()


def _write_vsimem_text(path: pathlib.PurePosixPath, content: str) -> None:
  gdal.FileFromMemBuffer(str(path), content.encode('utf-8'))


def _read_vsimem_text(
    path: pathlib.PurePosixPath, max_size: int = 32768
) -> str:
  return gcore_util.vsi_read_file_as_string(str(path), max_size)


def _load_poly_records() -> list[tuple[float, int, str, ogr.Geometry]]:
  poly_path = ogr_util.GetTestFilePath('shape/poly/poly.shp')
  records: list[tuple[float, int, str, ogr.Geometry]] = []
  with gdal.OpenEx(poly_path, gdal.OF_VECTOR | gdal.OF_READONLY) as shp_ds:
    shp_lyr = shp_ds.GetLayer(0)
    for feat in shp_lyr:
      geom = feat.GetGeometryRef()
      if geom is None:
        raise AssertionError('Expected geometry in poly.shp feature')
      records.append((
          feat.GetField('AREA'),
          feat.GetField('EAS_ID'),
          feat.GetField('PRFEDEA'),
          geom.Clone(),
      ))
  return records


class MitabTest(parameterized.TestCase, gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(ogr.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())
    driver = gdal.GetDriverByName(DRIVER)
    if driver is None:
      self.fail(f'Missing OGR driver: {DRIVER}')
    self.driver: gdal.Driver = driver
    self.tmp_dir = pathlib.Path(self.create_tempdir().full_path)
    self.enter_context(gdal.config_option('CPL_TMPDIR', str(self.tmp_dir)))
    vsi_dir = self.enter_context(
        gcore_util.VsimemCleanupDir(
            f'/vsimem/mitab_test/{self._testMethodName}'
        )
    )
    self.vsimem_dir = pathlib.PurePosixPath(vsi_dir)
    self.assertEqual(gdal.MkdirRecursive(str(self.vsimem_dir), 0o755), 0)

  def _open_mitab(
      self,
      path: str | os.PathLike[str],
      *,
      update: bool = False,
  ) -> gdal.Dataset:
    flags = gdal.OF_VECTOR | (gdal.OF_UPDATE if update else gdal.OF_READONLY)
    return gdal.OpenEx(str(path), flags, allowed_drivers=[DRIVER])

  def _create_poly_dataset(self, ext: str) -> pathlib.PurePosixPath:
    ds_path = self.vsimem_dir / f'tpoly.{ext}'
    srs = osr.SpatialReference()
    srs.ImportFromWkt(_NZGD49_WKT)

    with self.driver.CreateVector(str(ds_path)) as ds:
      layer = ds.CreateLayer('tpoly', srs)
      self.assertEqual(layer.GetDataset().GetDescription(), ds.GetDescription())
      layer.CreateField(ogr.FieldDefn('AREA', ogr.OFTReal))
      layer.CreateField(ogr.FieldDefn('EAS_ID', ogr.OFTInteger))
      layer.CreateField(ogr.FieldDefn('PRFEDEA', ogr.OFTString))

      for area, eas_id, prfedea, geom in _load_poly_records():
        dst_feat = ogr.Feature(layer.GetLayerDefn())
        dst_feat.SetField('AREA', area)
        dst_feat.SetField('EAS_ID', eas_id)
        dst_feat.SetField('PRFEDEA', prfedea)
        dst_feat.SetGeometry(geom)
        self.assertEqual(layer.CreateFeature(dst_feat), ogr.OGRERR_NONE)
    return ds_path

  def _assert_geometry_equal(
      self,
      actual: ogr.Geometry | None,
      expected: ogr.Geometry | str | None,
      max_error: float = 1e-4,
  ) -> None:
    expected_geom = (
        ogr.CreateGeometryFromWkt(expected)
        if isinstance(expected, str)
        else expected
    )
    if expected_geom is None:
      self.assertIsNone(actual)
      return
    if actual is None:
      self.fail('Expected non-null geometry')
    self.assertEqual(actual.GetGeometryName(), expected_geom.GetGeometryName())
    self.assertEqual(
        actual.GetGeometryCount(), expected_geom.GetGeometryCount()
    )
    self.assertEqual(actual.GetPointCount(), expected_geom.GetPointCount())
    if actual.GetGeometryCount() > 0:
      for idx in range(actual.GetGeometryCount()):
        self._assert_geometry_equal(
            actual.GetGeometryRef(idx),
            expected_geom.GetGeometryRef(idx),
            max_error=max_error,
        )
    else:
      for idx in range(actual.GetPointCount()):
        self.assertAlmostEqual(
            actual.GetX(idx), expected_geom.GetX(idx), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetY(idx), expected_geom.GetY(idx), delta=max_error
        )
        self.assertAlmostEqual(
            actual.GetZ(idx), expected_geom.GetZ(idx), delta=max_error
        )

  def _assert_field_values(
      self, layer: ogr.Layer, field_name: str, expected: Sequence[object]
  ) -> None:
    actual = [feat.GetField(field_name) for feat in layer]
    self.assertEqual(actual, list(expected))

  def _copy_fixture_to_tmp(
      self, stem: str, extensions: Sequence[str]
  ) -> pathlib.Path:
    for ext in extensions:
      filename = f'{stem}.{ext}'
      src = pathlib.Path(ogr_util.GetTestFilePath(f'mitab/{filename}'))
      self.assertTrue(src.exists(), f'Missing fixture {src}')
      dst = self.tmp_dir / filename
      shutil.copy(src, dst)
      dst.chmod(0o664)
    return self.tmp_dir / f'{stem}.{extensions[0]}'

  def _srs_from_coordsys(self, coordsys: str) -> osr.SpatialReference:
    mif_path = self.vsimem_dir / 'coordsys_read.mif'
    mid_path = mif_path.with_suffix('.mid')
    mif_content = (
        'Version 300\n'
        'Charset "Neutral"\n'
        'Delimiter ","\n'
        f'{coordsys}\n'
        'Columns 1\n'
        '  foo Char(254)\n'
        'Data\n\n'
        'NONE\n'
    )
    _write_vsimem_text(mif_path, mif_content)
    _write_vsimem_text(mid_path, '""\n')
    try:
      with self._open_mitab(mif_path) as ds:
        srs = ds.GetLayer(0).GetSpatialRef()
        if srs is None:
          self.fail(f'Expected SpatialRef for {coordsys}')
        return srs.Clone()
    finally:
      gdal.Unlink(str(mif_path))
      gdal.Unlink(str(mid_path))

  def _coordsys_from_srs(self, srs: osr.SpatialReference) -> str:
    mif_path = self.vsimem_dir / 'coordsys_write.mif'
    mid_path = mif_path.with_suffix('.mid')
    try:
      with self.driver.CreateVector(str(mif_path)) as ds:
        lyr = ds.CreateLayer('foo', srs=srs)
        lyr.CreateField(ogr.FieldDefn('foo'))
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)
      text = _read_vsimem_text(mif_path)
      for line in text.splitlines():
        if line.startswith('CoordSys'):
          return line
      self.fail(f'CoordSys line missing from generated MIF:\n{text}')
    finally:
      gdal.Unlink(str(mif_path))
      gdal.Unlink(str(mid_path))

  def _reset_mitab_bounds_cache(self) -> None:
    """Clears MITAB's global MITAB_BOUNDS_FILE cache across tests."""
    with gdal.config_option('MITAB_BOUNDS_FILE', ''):
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(32631)
      tab_path = self.vsimem_dir / '_reset_bounds.tab'
      with self.driver.CreateVector(str(tab_path)) as ds:
        lyr = ds.CreateLayer('reset', srs=srs)
        lyr.CreateField(ogr.FieldDefn('id', ogr.OFTInteger))

  # Upstream: ogr_mitab_2, ogr_mitab_3
  @parameterized.named_parameters(
      dict(testcase_name='tab', ext='tab'),
      dict(testcase_name='mif', ext='mif'),
  )
  def test_write_and_read_poly(self, ext: str) -> None:
    ds_path = self._create_poly_dataset(ext)
    expected_records = _load_poly_records()

    with self._open_mitab(ds_path) as ds:
      layer = ds.GetLayer(0)
      with _attribute_filter(layer, 'EAS_ID < 170'):
        self._assert_field_values(layer, 'EAS_ID', [168, 169, 166, 158, 165])

      layer.ResetReading()
      for idx, (exp_area, exp_eas_id, exp_prfedea, exp_geom) in enumerate(
          expected_records
      ):
        with self.subTest(feature_index=idx):
          feat = layer.GetNextFeature()
          self.assertIsNotNone(feat)
          self._assert_geometry_equal(
              feat.GetGeometryRef(), exp_geom, max_error=0.02
          )
          self.assertEqual(feat.GetField('AREA'), exp_area)
          self.assertEqual(feat.GetField('EAS_ID'), exp_eas_id)
          self.assertEqual(feat.GetField('PRFEDEA'), exp_prfedea)
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: ogr_mitab_4
  def test_execute_sql_preserves_geometry(self) -> None:
    ds_path = self._create_poly_dataset('tab')
    with self._open_mitab(ds_path) as ds:
      with ds.ExecuteSQL(
          "SELECT * FROM tpoly WHERE prfedea = '35043413'"
      ) as sql_lyr:
        self._assert_field_values(sql_lyr, 'prfedea', ['35043413'])
        sql_lyr.ResetReading()
        feat = sql_lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self._assert_geometry_equal(
            feat.GetGeometryRef(),
            'POLYGON ((479750.688 4764702.000,479658.594 4764670.000,'
            '479640.094 4764721.000,479735.906 4764752.000,'
            '479750.688 4764702.000))',
            max_error=0.02,
        )

  # Upstream: ogr_mitab_5
  def test_spatial_filtering(self) -> None:
    ds_path = self._create_poly_dataset('tab')
    with self._open_mitab(ds_path) as ds:
      layer = ds.GetLayer(0)
      with _spatial_filter_rect(layer, 479505, 4763195, 480526, 4762819):
        self._assert_field_values(layer, 'eas_id', [158])
      self.assertEqual(layer.GetFeatureCount(), 10)

  # Upstream: ogr_mitab_6
  def test_non_wgs84_datum_is_populated(self) -> None:
    ds_path = self._create_poly_dataset('tab')
    with self._open_mitab(ds_path) as ds:
      srs = ds.GetLayer(0).GetSpatialRef()
      self.assertIsNotNone(srs)
      self.assertEqual(
          srs.GetAttrValue('PROJCS|GEOGCS|DATUM'), 'New_Zealand_GD49'
      )

  # Google3 addition: verifies single-file .mif creation (no upstream _7).
  def test_create_mif_single_file(self) -> None:
    filepath = self.vsimem_dir / 'test.mif'
    with self.driver.CreateVector(str(filepath), options=['FORMAT=MIF']) as ds:
      layer = ds.CreateLayer('testlayerdef')
      layer.CreateField(ogr.FieldDefn('AREA', ogr.OFTReal))
      self.assertIsNotNone(layer.GetLayerDefn())

    with self._open_mitab(filepath) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetLayerDefn().GetFieldCount(), 1)
      self.assertEqual(layer.GetLayerDefn().GetFieldDefn(0).GetName(), 'AREA')

  # Upstream: ogr_mitab_10
  def test_read_mif_with_two_char_delimiter(self) -> None:
    filepath = pathlib.Path(ogr_util.GetTestFilePath('mitab/small.mif'))
    self.assertTrue(filepath.exists())
    with self._open_mitab(filepath) as ds:
      layer = ds.GetLayer(0)
      feat1 = layer.GetNextFeature()
      self.assertIsNotNone(feat1)
      self.assertEqual(feat1.GetField('NAME'), ' S. 11th St.')
      self.assertEqual(feat1.GetField('FLOODZONE'), 10)
      self._assert_geometry_equal(
          feat1.GetGeometryRef(),
          'POLYGON ((407131.721 155322.441,407134.468 155329.616,'
          '407142.741 155327.242,407141.503 155322.467,'
          '407140.875 155320.049,407131.721 155322.441))',
          max_error=1e-9,
      )
      feat2 = layer.GetNextFeature()
      self.assertIsNotNone(feat2)
      self.assertEqual(feat2.GetField('OWNER'), 'Guarino "Chucky" Sandra')

  # Upstream: ogr_mitab_11
  def test_support_for_ntf_datum_with_non_greenwich_pm(self) -> None:
    filepath = pathlib.Path(ogr_util.GetTestFilePath('mitab/small_ntf.mif'))
    self.assertTrue(filepath.exists())
    with self._open_mitab(filepath) as ds:
      srs = ds.GetLayer(0).GetSpatialRef()
      self.assertIsNotNone(srs)
      pm_value = srs.GetAttrValue('PROJCS|GEOGCS|PRIMEM', 1)
      self.assertEqual(pm_value[:6], '2.3372')

  # Upstream: ogr_mitab_12
  def test_new_mif_layer_defn_non_null_before_fields(self) -> None:
    dir_path = self.vsimem_dir / 'mif_dir'
    with self.driver.CreateVector(str(dir_path), options=['FORMAT=MIF']) as ds:
      lyr = ds.CreateLayer('testlyrdef')
      self.assertIsNotNone(lyr.GetLayerDefn())
      lyr.CreateField(ogr.FieldDefn('AREA', ogr.OFTReal))

    with self._open_mitab(dir_path / 'testlyrdef.mif') as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(0).GetType(), ogr.OFTReal
      )

  # Upstream: ogr_mitab_13 (uses in-process VectorTranslate instead of ogr2ogr)
  @parameterized.named_parameters(
      dict(testcase_name='tab', fmt='TAB'),
      dict(testcase_name='mif', fmt='MIF'),
  )
  def test_field_widths_and_precisions_preserved(self, fmt: str) -> None:
    gml_path = self.vsimem_dir / 'testlyrdef.gml'
    xsd_path = self.vsimem_dir / 'testlyrdef.xsd'
    _write_vsimem_text(gml_path, _TESTLYRDEF_GML)
    _write_vsimem_text(xsd_path, _TESTLYRDEF_XSD)

    out_path = self.vsimem_dir / f'testlyrdef.{fmt.lower()}'
    options = ['FORMAT=MIF'] if fmt == 'MIF' else []
    with gdal.OpenEx(str(gml_path), gdal.OF_VECTOR | gdal.OF_READONLY) as src:
      with gdal.VectorTranslate(
          str(out_path), src, format=DRIVER, datasetCreationOptions=options
      ) as _:
        pass

    expected_fields = (
        ('AREA', ogr.OFTReal, 9, 4),
        ('VOLUME', ogr.OFTReal, 0, 0),
        ('LENGTH', ogr.OFTInteger, 0 if fmt == 'MIF' else 10, 0),
        ('WIDTH', ogr.OFTInteger, 0 if fmt == 'MIF' else 4, 0),
    )
    with self._open_mitab(out_path) as ds:
      lyr = ds.GetLayer('testlyrdef')
      self.assertIsNotNone(lyr)
      defn = lyr.GetLayerDefn()
      for name, exp_type, exp_width, exp_prec in expected_fields:
        with self.subTest(field=name):
          fld = defn.GetFieldDefn(defn.GetFieldIndex(name))
          self.assertEqual(fld.GetType(), exp_type)
          self.assertEqual(fld.GetWidth(), exp_width)
          self.assertEqual(fld.GetPrecision(), exp_prec)

  # Upstream: ogr_mitab_15
  def test_mif_without_mid(self) -> None:
    nomid_path = pathlib.Path(ogr_util.GetTestFilePath('mitab/nomid.mif'))
    self.assertTrue(nomid_path.exists())
    with self._open_mitab(nomid_path) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertIsNotNone(feat.GetGeometryRef())

    # Also verify opening .mif without .mid when attributes were declared.
    mif_path = self.vsimem_dir / 'nomid_created.mif'
    with self.driver.CreateVector(str(mif_path)) as ds:
      lyr = ds.CreateLayer('empty')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetField(0, 1)
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 2)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    gdal.Unlink(str(mif_path.with_suffix('.mid')))
    with self._open_mitab(mif_path) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertFalse(feat.IsFieldSet(0))
      self._assert_geometry_equal(feat.GetGeometryRef(), 'POINT (1 2)')

  # Upstream: ogr_mitab_16
  def test_empty_mif(self) -> None:
    mif_path = self.vsimem_dir / 'empty.mif'
    with self.driver.CreateVector(str(mif_path)) as ds:
      lyr = ds.CreateLayer('empty')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))

    with self._open_mitab(mif_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 1)
      self.assertIsNone(lyr.GetNextFeature())

  # Upstream: ogr_mitab_17 (test_ogrsf CLI excluded from //third_party/gdal)
  @parameterized.named_parameters(
      dict(testcase_name='tab', ext='tab'),
      dict(testcase_name='mif', ext='mif'),
  )
  def test_layer_contract_capabilities(self, ext: str) -> None:
    ds_path = self._create_poly_dataset(ext)
    with self._open_mitab(ds_path) as ds:
      self.assertEqual(ds.GetLayerCount(), 1)
      self.assertIsNone(ds.GetLayer(1))
      self.assertIsNone(ds.GetLayerByName('nonexistent_layer'))

      lyr = ds.GetLayer(0)
      self.assertIsNotNone(lyr)
      by_name = ds.GetLayerByName(lyr.GetName())
      self.assertIsNotNone(by_name)
      self.assertEqual(by_name.GetName(), lyr.GetName())

      # MIF defers scanning until PreParseFile(); TAB reads header directly.
      is_tab = ext == 'tab'
      self.assertEqual(lyr.GetFeatureCount(0), 10 if is_tab else -1)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastFeatureCount), is_tab)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastGetExtent), is_tab)
      self.assertEqual(lyr.TestCapability(ogr.OLCFastSpatialFilter), is_tab)

      self.assertEqual(lyr.GetFeatureCount(1), 10)
      self.assertTrue(lyr.TestCapability(ogr.OLCRandomRead))
      self.assertTrue(lyr.TestCapability(ogr.OLCFastGetExtent))
      self.assertTrue(lyr.TestCapability(ogr.OLCFastFeatureCount))
      for cap in (ogr.OLCRandomWrite, ogr.OLCDeleteFeature):
        self.assertFalse(lyr.TestCapability(cap))
      if is_tab:
        for cap in (ogr.OLCSequentialWrite, ogr.OLCCreateField):
          self.assertFalse(lyr.TestCapability(cap))

      extent = lyr.GetExtent(force=True)
      self.assertLen(extent, 4)
      self.assertLess(extent[0], extent[1])
      self.assertLess(extent[2], extent[3])
      self.assertBetween(extent[0], 478000, 479000)
      self.assertBetween(extent[1], 481000, 482000)
      self.assertBetween(extent[2], 4762000, 4763500)
      self.assertBetween(extent[3], 4765000, 4766000)

      features = list(lyr)
      self.assertLen(features, 10)
      # ResetReading() rewinds MIFFile after EOF before GetFeature(fid), since
      # MIFFile::GotoFeature (mitab_miffile.cpp:1164) leaves m_nPreloadedId==0
      # while m_nCurFeatureId != 0 at EOF.
      lyr.ResetReading()
      for feat in features:
        by_fid = lyr.GetFeature(feat.GetFID())
        self.assertIsNotNone(by_fid)
        self.assertEqual(by_fid.GetField('EAS_ID'), feat.GetField('EAS_ID'))
      if is_tab:
        self.assertIsNone(lyr.GetFeature(99999))
      else:
        with (
            gdal.quiet_errors(),
            self.assertRaisesRegex(RuntimeError, r'invalid feature id 99999'),
        ):
          lyr.GetFeature(99999)

      lyr.ResetReading()
      if lyr.TestCapability(ogr.OLCFastSetNextByIndex):
        self.assertEqual(lyr.SetNextByIndex(5), ogr.OGRERR_NONE)
        at_5 = lyr.GetNextFeature()
        self.assertIsNotNone(at_5)
        self.assertEqual(at_5.GetFID(), features[5].GetFID())

      with _attribute_filter(lyr, 'EAS_ID < 170'):
        self.assertEqual(lyr.GetFeatureCount(1), 5)
        self.assertLen(list(lyr), 5)

      with _spatial_filter_rect(lyr, 479505, 4763195, 480526, 4762819):
        self.assertEqual(lyr.GetFeatureCount(1), 1)
        self.assertLen(list(lyr), 1)

  # Upstream: ogr_mitab_18
  def test_epsg_2154_lambert93(self) -> None:
    created_tab = self.vsimem_dir / 'lambert93.tab'
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(2154)
    with self.driver.CreateVector(str(created_tab)) as ds:
      lyr = ds.CreateLayer('test', srs=srs)
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))

    mapinfo_tab = self._copy_fixture_to_tmp(
        'lambert93_francais', ('TAB', 'DAT', 'ID', 'MAP')
    )
    for target in (created_tab, mapinfo_tab):
      with self.subTest(path=target.name):
        with self._open_mitab(target, update=True) as ds:
          sr_got = ds.GetLayer(0).GetSpatialRef()
          self.assertIsNotNone(sr_got)
          self.assertIn('2154', sr_got.ExportToWkt())
          self.assertStartsWith(
              sr_got.ExportToProj4(),
              '+proj=lcc +lat_0=46.5 +lon_0=3 +lat_1=49 +lat_2=44 '
              '+x_0=700000 +y_0=6600000 +ellps=GRS80 +towgs84=0,0,0,0,0,0,0 '
              '+units=m +no_defs',
          )

  # Upstream: ogr_mitab_19
  def test_coordinate_precision_rounding(self) -> None:
    tab_path = pathlib.Path(ogr_util.GetTestFilePath('mitab/utm31.TAB'))
    self.assertTrue(tab_path.exists())
    with self._open_mitab(tab_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(
          feat.GetGeometryRef().ExportToWkt(), 'POINT (485248.12 2261.45)'
      )

  # Upstream: ogr_mitab_20
  @parameterized.named_parameters(
      dict(testcase_name='tab', fmt='tab'),
      dict(testcase_name='mif', fmt='mif'),
  )
  def test_user_bounds_file_and_bounds_lco(self, fmt: str) -> None:
    bounds_path = self.vsimem_dir / 'mitab_bounds.txt'
    _write_vsimem_text(bounds_path, _MITAB_BOUNDS_TXT)
    self.addCleanup(self._reset_mitab_bounds_cache)

    passes = (
        ('no_bounds_file', None, None, None, False),
        (
            'french_bounds_1',
            str(bounds_path),
            _LAMBERT93_FRENCH_BOUNDS_WKT,
            None,
            True,
        ),
        (
            'french_bounds_2',
            str(bounds_path),
            _LAMBERT93_FRENCH_BOUNDS_WKT,
            None,
            True,
        ),
        ('unloaded_bounds_file', None, None, None, False),
        (
            'lco_bounds',
            None,
            None,
            ['BOUNDS=75000,6000000,1275000,7200000'],
            True,
        ),
        (
            'european_bounds',
            str(bounds_path),
            _LAMBERT93_EUROPE_BOUNDS_WKT,
            None,
            False,
        ),
        ('generic_2154_europe_bounds', str(bounds_path), None, None, False),
    )

    # Sequential subTests intentionally exercise the process-global
    # MITAB_BOUNDS_FILE cache across load, reuse, bypass, LCO, and reload.
    for name, cfg_path, wkt, lco, expect_exact_coords in passes:
      with self.subTest(case=name):
        out_path = self.vsimem_dir / f'{name}.{fmt}'
        srs = osr.SpatialReference()
        if wkt is not None:
          srs.SetFromUserInput(wkt)
        else:
          srs.ImportFromEPSG(2154)

        cfg = (
            gdal.config_option('MITAB_BOUNDS_FILE', cfg_path)
            if cfg_path
            else contextlib.nullcontext()
        )
        with cfg:
          with self.driver.CreateVector(str(out_path)) as ds:
            lyr = ds.CreateLayer('test', srs=srs, options=lco or [])
            lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
            feat = ogr.Feature(lyr.GetLayerDefn())
            feat.SetGeometryDirectly(
                ogr.CreateGeometryFromWkt('POINT (700000.001 6600000.001)')
            )
            self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

        with self._open_mitab(out_path) as ds:
          lyr = ds.GetLayer(0)
          feat = lyr.GetNextFeature()
          self.assertIsNotNone(feat)
          if name == 'generic_2154_europe_bounds':
            self.assertIn('49.00000000001', lyr.GetSpatialRef().ExportToWkt())
          if fmt == 'tab':
            got_wkt = feat.GetGeometryRef().ExportToWkt()
            if expect_exact_coords:
              self.assertEqual(got_wkt, 'POINT (700000.001 6600000.001)')
            else:
              self.assertNotEqual(got_wkt, 'POINT (700000.001 6600000.001)')
          else:
            mif_text = _read_vsimem_text(out_path)
            french_bounds = 'Bounds (75000, 6000000) (1275000, 7200000)'
            euro_bounds = 'Bounds (-792421, 5278231) (3520778, 9741029)'
            if name in ('french_bounds_1', 'french_bounds_2', 'lco_bounds'):
              self.assertIn(french_bounds, mif_text)
            elif name in ('european_bounds', 'generic_2154_europe_bounds'):
              self.assertIn(euro_bounds, mif_text)
            else:
              self.assertNotIn(french_bounds, mif_text)
              self.assertNotIn(euro_bounds, mif_text)

  # Upstream: ogr_mitab_20bis
  @parameterized.named_parameters(
      dict(testcase_name='reload_on_mtime_change', reload_file=True),
      dict(testcase_name='cached_when_mtime_unchanged', reload_file=False),
  )
  def test_bounds_file_caching(self, reload_file: bool) -> None:
    bounds_path = self.tmp_dir / 'mitab_bounds.txt'
    self.addCleanup(self._reset_mitab_bounds_cache)
    tab_path = self.vsimem_dir / 'bounds_cache.tab'

    entries = (
        (
            (
                'Source = CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, '
                '49, 700000, 6600000\n'
                'Destination=CoordSys Earth Projection 3, 33, "m", 3, 46.5, '
                '44, 49.00000000001, 700000, 6600000 Bounds (-792421, 5278231) '
                '(3520778, 9741029)'
            ),
            1000,
            '49.00000000001',
        ),
        (
            (
                'Source = CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, '
                '49, 700000, 6600000\n'
                'Destination=CoordSys Earth Projection 3, 33, "m", 3, 46.5, '
                '44, 49.00000000002, 700000, 6600000 Bounds (75000, 6000000) '
                '(1275000, 7200000)'
            ),
            2000 if reload_file else 1000,
            '49.00000000002' if reload_file else '49.00000000001',
        ),
    )

    with gdal.config_option('MITAB_BOUNDS_FILE', str(bounds_path)):
      for content, mtime, expected_parallel in entries:
        bounds_path.write_text(content, encoding='ascii')
        os.utime(bounds_path, (mtime, mtime))

        with self.driver.CreateVector(str(tab_path)) as ds:
          srs = osr.SpatialReference()
          srs.ImportFromEPSG(2154)
          lyr = ds.CreateLayer('test', srs=srs)
          lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
          feat = ogr.Feature(lyr.GetLayerDefn())
          feat.SetGeometryDirectly(
              ogr.CreateGeometryFromWkt('POINT (700000.001 6600000.001)')
          )
          self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

        with self._open_mitab(tab_path) as ds:
          wkt = ds.GetLayer(0).GetSpatialRef().ExportToWkt()
          self.assertIn(expected_parallel, wkt)

  # Upstream: ogr_mitab_21
  def test_create_tab_without_explicit_field(self) -> None:
    tab_path = self.vsimem_dir / 'no_fields.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
      with _capture_warnings() as warnings:
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)
      self.assertTrue(
          any('dummy FID column' in msg for msg in warnings), warnings
      )

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('FID'), 1)

  # Upstream: ogr_mitab_22
  @parameterized.named_parameters(
      dict(
          testcase_name='2_features_seeded', nb_features=2, create_empty=False
      ),
      dict(
          testcase_name='2_features_empty_init',
          nb_features=2,
          create_empty=True,
      ),
      dict(testcase_name='1000_features', nb_features=1000, create_empty=False),
  )
  def test_append_in_update_mode(
      self, nb_features: int, create_empty: bool
  ) -> None:
    tab_path = self.vsimem_dir / 'append_update.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
      if not create_empty:
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('ID', 1)
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    start = 0 if create_empty else 1
    for idx in range(start, nb_features):
      with self._open_mitab(tab_path, update=True) as ds:
        lyr = ds.GetLayer(0)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('ID', idx + 1)
        feat.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt(f'POINT ({idx} {idx})')
        )
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), nb_features)
      for idx in range(nb_features):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('ID'), idx + 1)

  # Upstream: ogr_mitab_23
  @parameterized.named_parameters(
      dict(testcase_name='n0', nb_features=0),
      dict(testcase_name='n1', nb_features=1),
      dict(testcase_name='n2', nb_features=2),
      dict(testcase_name='n100', nb_features=100),
      dict(testcase_name='n1000', nb_features=1000),
  )
  def test_create_then_read_before_close(self, nb_features: int) -> None:
    tab_path = self.vsimem_dir / 'create_read.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
      for idx in range(nb_features):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('ID', idx + 1)
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

      lyr.ResetReading()
      for idx in range(nb_features):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('ID'), idx + 1)
      self.assertIsNone(lyr.GetNextFeature())

  # Upstream: ogr_mitab_24
  @parameterized.named_parameters(
      dict(testcase_name='n2', nb_features=2),
      dict(testcase_name='n100', nb_features=100),
      dict(testcase_name='n1000', nb_features=1000),
  )
  def test_create_read_create_read_before_close(self, nb_features: int) -> None:
    half = nb_features // 2
    tab_path = self.vsimem_dir / 'create_read_create_read.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
      for idx in range(half):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('ID', idx + 1)
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

      lyr.ResetReading()
      for idx in range(half):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('ID'), idx + 1)
      self.assertIsNone(lyr.GetNextFeature())

      for idx in range(half):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('ID', half + idx + 1)
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (0 0)'))
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

      lyr.ResetReading()
      for idx in range(nb_features):
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('ID'), idx + 1)
      self.assertIsNone(lyr.GetNextFeature())

  # Upstream: ogr_mitab_25
  @parameterized.named_parameters(
      dict(testcase_name='n2', nb_features=2),
      dict(testcase_name='n1000', nb_features=1000),
  )
  def test_open_update_readonly_ops_preserve_mtime(
      self, nb_features: int
  ) -> None:
    tab_path = self.tmp_dir / 'update_noop.tab'
    count = nb_features // 2
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      # TODO: b/335317901 - Fill all 11 bytes of TABDATFieldDef.szName
      # (mitab_datfile.cpp) to avoid MSan fwrite failure on short names.
      lyr.CreateField(ogr.FieldDefn('feature_id', ogr.OFTInteger))
      for idx in range(count):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('feature_id', idx + 1)
        feat.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt(f'POINT ({idx} {idx})')
        )
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    past_time = os.path.getmtime(tab_path) - 60.0
    mtimes: dict[str, float] = {}
    for ext in ('.map', '.tab', '.dat', '.id'):
      sidecar = tab_path.with_suffix(ext)
      os.utime(sidecar, (past_time, past_time))
      mtimes[ext] = sidecar.stat().st_mtime

    with self._open_mitab(tab_path, update=True) as _:
      pass
    for ext, expected_mtime in mtimes.items():
      self.assertEqual(
          tab_path.with_suffix(ext).stat().st_mtime, expected_mtime
      )

    with self._open_mitab(tab_path, update=True) as ds:
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(1), count)
    for ext, expected_mtime in mtimes.items():
      self.assertEqual(
          tab_path.with_suffix(ext).stat().st_mtime, expected_mtime
      )

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      with _spatial_filter_rect(lyr, 0.5, 0.5, 1.5, 1.5):
        self.assertEqual(lyr.GetFeatureCount(1), 1 if count > 1 else 0)
    for ext, expected_mtime in mtimes.items():
      self.assertEqual(
          tab_path.with_suffix(ext).stat().st_mtime, expected_mtime
      )

    # Read-only consistency check (in-process replacement for test_ogrsf -fsf).
    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(1), count)
      self.assertIsNotNone(lyr.GetFeature(1))
      self.assertIsNotNone(lyr.GetFeature(count))
      with _spatial_filter_rect(lyr, 0.5, 0.5, 1.5, 1.5):
        expected_filtered = 1 if count > 1 else 0
        self.assertEqual(lyr.GetFeatureCount(1), expected_filtered)
        self.assertLen(list(lyr), expected_filtered)
    for ext, expected_mtime in mtimes.items():
      self.assertEqual(
          tab_path.with_suffix(ext).stat().st_mtime, expected_mtime
      )

  # Upstream: ogr_mitab_26
  @parameterized.named_parameters(
      dict(testcase_name='n2_clean_close', nb_features=2, check_errors=False),
      dict(
          testcase_name='n2_after_error_checks',
          nb_features=2,
          check_errors=True,
      ),
      dict(testcase_name='n1000', nb_features=1000, check_errors=False),
  )
  def test_delete_feature(self, nb_features: int, check_errors: bool) -> None:
    tab_path = self.vsimem_dir / 'delete_feature.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
      for idx in range(nb_features):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('ID', idx + 1)
        feat.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt(f'POINT ({idx} {idx})')
        )
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

      if nb_features == 2:
        self.assertEqual(lyr.DeleteFeature(1), ogr.OGRERR_NONE)
      else:
        for k in range(nb_features // 2):
          self.assertEqual(
              lyr.DeleteFeature(nb_features // 4 + k), ogr.OGRERR_NONE
          )

      if check_errors:
        # SWIG's OGRErr typemap translates OGRERR_NON_EXISTING_FEATURE to
        # RuntimeError under ExceptionMgr.
        with self.assertRaisesRegex(RuntimeError, r'Non existing feature'):
          lyr.DeleteFeature(1)
        self.assertIsNone(lyr.GetFeature(1))
        with self.assertRaisesRegex(RuntimeError, r'Non existing feature'):
          lyr.DeleteFeature(nb_features + 1)

    with self._open_mitab(tab_path) as ds:
      self.assertEqual(ds.GetLayer(0).GetFeatureCount(), nb_features // 2)

    if nb_features == 1000:
      with self._open_mitab(tab_path, update=True) as ds:
        self.assertEqual(ds.GetLayer(0).DeleteFeature(245), ogr.OGRERR_NONE)
      with self._open_mitab(tab_path) as ds:
        self.assertEqual(ds.GetLayer(0).GetFeatureCount(), nb_features // 2 - 1)

  # Upstream: ogr_mitab_27
  def test_set_feature(self) -> None:
    tab_path = self.vsimem_dir / 'set_feature.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('intfield', ogr.OFTInteger))
      lyr.CreateField(ogr.FieldDefn('realfield', ogr.OFTReal))
      lyr.CreateField(ogr.FieldDefn('stringfield', ogr.OFTString))

      # Feature without FID posts CE_Failure.
      feat = ogr.Feature(lyr.GetLayerDefn())
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'feature with a FID'),
      ):
        lyr.SetFeature(feat)

      # FID <= 0 raises RuntimeError from OGRERR_NON_EXISTING_FEATURE.
      feat.SetFID(0)
      with self.assertRaisesRegex(RuntimeError, r'Non existing feature'):
        lyr.SetFeature(feat)

      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetField('intfield', 1)
      feat.SetField('realfield', 2.34)
      feat.SetField('stringfield', 'foo')
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (1 2)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)
      fid = feat.GetFID()

      # FID > feature_count raises OGRERR_NON_EXISTING_FEATURE as RuntimeError.
      out_of_range = ogr.Feature(lyr.GetLayerDefn())
      out_of_range.SetFID(2)
      with self.assertRaisesRegex(RuntimeError, r'Non existing feature'):
        lyr.SetFeature(out_of_range)

      # Overwrite created object with blank feature.
      blank = ogr.Feature(lyr.GetLayerDefn())
      blank.SetFID(fid)
      self.assertEqual(lyr.SetFeature(blank), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('intfield'), 0)
      self.assertEqual(feat.GetField('realfield'), 0.0)
      self.assertEqual(feat.GetField('stringfield'), '')
      self.assertIsNone(feat.GetGeometryRef())

      feat.SetField('intfield', 1)
      feat.SetField('realfield', 2.34)
      feat.SetField('stringfield', 'foo')
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (2 3)'))
      self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('intfield'), 1)
      self.assertAlmostEqual(feat.GetField('realfield'), 2.34)
      self.assertEqual(feat.GetField('stringfield'), 'foo')
      # Default NonEarth bounds quantize integer coordinates by up to ~0.015.
      self._assert_geometry_equal(
          feat.GetGeometryRef(), 'POINT (2 3)', max_error=0.02
      )
      self.assertEqual(lyr.DeleteFeature(feat.GetFID()), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      # SetFeature() on deleted feature restores it.
      self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)
      restored = lyr.GetFeature(1)
      self.assertIsNotNone(restored)
      self.assertEqual(restored.GetField('intfield'), 1)
      self.assertAlmostEqual(restored.GetField('realfield'), 2.34)
      self.assertEqual(restored.GetField('stringfield'), 'foo')
      self._assert_geometry_equal(
          restored.GetGeometryRef(), 'POINT (2 3)', max_error=0.02
      )

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetFeature(1)
      # No-op SetFeature with identical feature.
      self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)

    old_map_size = gdal.VSIStatL(str(tab_path.with_suffix('.map'))).size

    # Repeated SetFeature calls must not leak/bloat .map object blocks.
    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetFeature(1)
      for _ in range(100):
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (2 3)'))
        self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)

    self.assertEqual(
        gdal.VSIStatL(str(tab_path.with_suffix('.map'))).size, old_map_size
    )

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetFeature(1)
      feat.SetField('intfield', -1)
      self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)
      updated = lyr.GetFeature(1)
      self.assertEqual(updated.GetField('intfield'), -1)
      self.assertAlmostEqual(updated.GetField('realfield'), 2.34)
      self.assertEqual(updated.GetField('stringfield'), 'foo')
      self._assert_geometry_equal(
          updated.GetGeometryRef(), 'POINT (2 3)', max_error=0.02
      )

  # Upstream: ogr_mitab_28
  def test_update_object_blocks_with_deleted_objects(self) -> None:
    rng = random.Random(42)
    tab_path = self.vsimem_dir / 'deleted_blocks.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))

    grid_size = 10
    total = grid_size * grid_size
    insert_perm = list(range(total))
    rng.shuffle(insert_perm)
    update_perm = list(range(total))
    rng.shuffle(update_perm)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      for n in insert_perm:
        x, y = divmod(n, grid_size)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt(
                f'LINESTRING({x} {y},{x + 0.1} {y},{x + 0.2} {y})'
            )
        )
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

      for fid in range(1, total + 1):
        self.assertEqual(lyr.DeleteFeature(fid), ogr.OGRERR_NONE)

      for fid_idx, n in enumerate(update_perm, start=1):
        x, y = divmod(n, grid_size)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt(
                f'LINESTRING({x} {y},{x + 0.1} {y},{x + 0.2} {y})'
            )
        )
        feat.SetFID(fid_idx)
        self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), total)
      iterated = 0
      for idx, feat in enumerate(lyr):
        iterated += 1
        x, y, _ = feat.GetGeometryRef().GetPoint(0)
        exp_x, exp_y = divmod(update_perm[idx], grid_size)
        self.assertLessEqual(abs(x - exp_x) + abs(y - exp_y), 0.1)
      self.assertEqual(iterated, total)

      for n in range(total):
        x, y = divmod(n, grid_size)
        with _spatial_filter_rect(lyr, x - 0.5, y - 0.5, x + 0.5, y + 0.5):
          self.assertEqual(lyr.GetFeatureCount(), 1)

  # Upstream: ogr_mitab_29
  def test_update_compressed_geometries(self) -> None:
    zip_path = pathlib.Path(
        ogr_util.GetTestFilePath('mitab/compr_symb_deleted_records.zip')
    )
    self.assertTrue(zip_path.exists())
    with zipfile.ZipFile(zip_path) as zf:
      zf.extractall(self.tmp_dir)

    tab_path = self.tmp_dir / 'compr_symb_deleted_records.tab'
    self.assertTrue(tab_path.exists())

    rng = random.Random(42)
    interior = 98
    perm = list(range(interior * interior))
    rng.shuffle(perm)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      for n in perm:
        x = 1 + n // interior
        y = 1 + n % interior
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(f'POINT({x} {y})'))
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 100 * 100)
      for n in range(100 * 100):
        x, y = divmod(n, 100)
        with _spatial_filter_rect(lyr, x - 0.01, y - 0.01, x + 0.01, y + 0.01):
          self.assertEqual(lyr.GetFeatureCount(), 1)

  # Upstream: ogr_mitab_30
  @parameterized.named_parameters(
      dict(testcase_name='create_mode', reopen_update=False),
      dict(testcase_name='update_mode', reopen_update=True),
  )
  def test_sync_to_disk(self, reopen_update: bool) -> None:
    tab_path = self.tmp_dir / 'sync_to_disk.tab'
    with contextlib.ExitStack() as stack:
      ds = stack.enter_context(self.driver.CreateVector(str(tab_path)))
      lyr = ds.CreateLayer('test', options=['BOUNDS=0,0,100,100'])
      # TODO: b/335317901 - Fill all 11 bytes of TABDATFieldDef.szName
      # (mitab_datfile.cpp) to avoid MSan fwrite failure on short names.
      lyr.CreateField(ogr.FieldDefn('feature_id', ogr.OFTInteger))
      self.assertEqual(lyr.SyncToDisk(), ogr.OGRERR_NONE)

      with self._open_mitab(tab_path) as check_ds:
        check_lyr = check_ds.GetLayer(0)
        self.assertEqual(check_lyr.GetFeatureCount(), 0)
        self.assertEqual(check_lyr.GetLayerDefn().GetFieldCount(), 1)

      past_time = os.path.getmtime(tab_path) - 60.0
      before_stats: dict[str, tuple[int, float]] = {}
      for ext in ('.map', '.tab', '.dat', '.id'):
        sidecar = tab_path.with_suffix(ext)
        os.utime(sidecar, (past_time, past_time))
        st = sidecar.stat()
        before_stats[ext] = (st.st_size, st.st_mtime)

      self.assertEqual(lyr.SyncToDisk(), ogr.OGRERR_NONE)
      for ext, (exp_size, exp_mtime) in before_stats.items():
        st = tab_path.with_suffix(ext).stat()
        self.assertEqual(st.st_size, exp_size)
        self.assertEqual(st.st_mtime, exp_mtime)

      if reopen_update:
        stack.close()
        ds = stack.enter_context(self._open_mitab(tab_path, update=True))
        lyr = ds.GetLayer(0)

      for j in range(100):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField('feature_id', j + 1)
        feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt(f'POINT ({j} {j})'))
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

        if not (j <= 10 or (j % 5) == 0):
          continue

        sizes_first: dict[str, int] = {}
        for pass_idx in range(2):
          self.assertEqual(lyr.SyncToDisk(), ogr.OGRERR_NONE)
          if pass_idx == 0:
            for ext in ('.map', '.tab', '.dat', '.id'):
              sizes_first[ext] = tab_path.with_suffix(ext).stat().st_size
          else:
            for ext, first_size in sizes_first.items():
              self.assertEqual(
                  tab_path.with_suffix(ext).stat().st_size, first_size
              )

          with self._open_mitab(tab_path) as check_ds:
            check_lyr = check_ds.GetLayer(0)
            self.assertEqual(check_lyr.GetFeatureCount(), j + 1)
            by_fid = check_lyr.GetFeature(j + 1)
            self.assertEqual(by_fid.GetField('feature_id'), j + 1)
            self.assertEqual(
                by_fid.GetGeometryRef().ExportToWkt(), f'POINT ({j} {j})'
            )
            check_lyr.ResetReading()
            last_feat = None
            for _ in range(j + 1):
              last_feat = check_lyr.GetNextFeature()
            self.assertIsNotNone(last_feat)
            self.assertEqual(last_feat.GetField('feature_id'), j + 1)
            self.assertEqual(
                last_feat.GetGeometryRef().ExportToWkt(), f'POINT ({j} {j})'
            )

  # Upstream: ogr_mitab_32
  @parameterized.named_parameters(
      dict(testcase_name='readonly', update=False),
      dict(testcase_name='update', update=True),
  )
  def test_read_aspatial_table_without_map_or_id(self, update: bool) -> None:
    tab_path = self._copy_fixture_to_tmp('aspatial-table', ('tab', 'dat'))
    with self._open_mitab(tab_path, update=update) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 2)
      feat1 = lyr.GetNextFeature()
      self.assertEqual(feat1.GetField('a'), 1)
      self.assertEqual(feat1.GetField('b'), 2)
      self.assertEqual(feat1.GetField('d'), 'hello')
      feat2 = lyr.GetFeature(2)
      self.assertEqual(feat2.GetField('a'), 4)

  # Upstream: ogr_mitab_33
  def test_reading_and_modifying_single_point_mapinfo(self) -> None:
    ro_path = pathlib.Path(
        ogr_util.GetTestFilePath('mitab/single_point_mapinfo.tab')
    )
    self.assertTrue(ro_path.exists())
    with self._open_mitab(ro_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat.GetField('toto'), '')

    # Add a new feature to a writable copy.
    tab_path = self._copy_fixture_to_tmp(
        'single_point_mapinfo', ('tab', 'dat', 'id', 'map')
    )
    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      existing = lyr.GetNextFeature()
      self.assertIsNotNone(existing)
      self.assertEqual(existing.GetField('toto'), '')
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometryDirectly(
          ogr.CreateGeometryFromWkt('POINT(1363180 7509810)')
      )
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 2)
      self.assertIsNotNone(lyr.GetNextFeature())
      added = lyr.GetNextFeature()
      self.assertIsNotNone(added)
      self._assert_geometry_equal(
          added.GetGeometryRef(), 'POINT (1363180 7509810)', max_error=0.01
      )

    # Replace the existing feature on a fresh writable copy.
    tab_path = self._copy_fixture_to_tmp(
        'single_point_mapinfo', ('tab', 'dat', 'id', 'map')
    )
    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetFID(1)
      feat.SetGeometryDirectly(
          ogr.CreateGeometryFromWkt('POINT(1363180 7509810)')
      )
      self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      replaced = lyr.GetNextFeature()
      self.assertIsNotNone(replaced)
      self._assert_geometry_equal(
          replaced.GetGeometryRef(), 'POINT (1363180 7509810)', max_error=0.01
      )

  # Upstream: ogr_mitab_34
  def test_update_line_spanning_multiple_coord_blocks(self) -> None:
    tab_path = self.vsimem_dir / 'multiline_blocks.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('ogr_mitab_34', options=['BOUNDS=-1000,0,1000,3000'])
      lyr.CreateField(ogr.FieldDefn('dummy', ogr.OFTString))
      geom = ogr.Geometry(ogr.wkbLineString)
      for idx in range(1000):
        geom.AddPoint_2D(idx, idx)
      for _ in range(2):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(geom)
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()
      lyr.GetNextFeature()  # Seek to another object before SetFeature.
      geom = feat.GetGeometryRef()
      geom.SetPoint_2D(0, -1000, 3000)
      self.assertEqual(lyr.SetFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      feat1 = lyr.GetNextFeature()
      geom1 = feat1.GetGeometryRef()
      self.assertAlmostEqual(geom1.GetX(0), -1000, delta=1e-2)
      self.assertAlmostEqual(geom1.GetY(0), 3000, delta=1e-2)
      for idx in range(1, 1000):
        self.assertAlmostEqual(geom1.GetX(idx), idx, delta=1e-2)
        self.assertAlmostEqual(geom1.GetY(idx), idx, delta=1e-2)

      feat2 = lyr.GetNextFeature()
      geom2 = feat2.GetGeometryRef()
      for idx in range(1000):
        self.assertAlmostEqual(geom2.GetX(idx), idx, delta=1e-2)
        self.assertAlmostEqual(geom2.GetY(idx), idx, delta=1e-2)

  # Upstream: ogr_mitab_35 (NonEarth & units)
  def test_srs_nonearth_and_units_roundtrip(self) -> None:
    self.assertEqual(
        self._coordsys_from_srs(osr.SpatialReference()),
        'CoordSys NonEarth Units "m"',
    )

    local_srs = osr.SpatialReference('LOCAL_CS["foo"]')
    coordsys = self._coordsys_from_srs(local_srs)
    self.assertEqual(coordsys, 'CoordSys NonEarth Units "m"')
    roundtrip_wkt = self._srs_from_coordsys(coordsys).ExportToWkt()
    self.assertIn(
        roundtrip_wkt,
        (
            'LOCAL_CS["Nonearth",UNIT["Meter",1]]',
            (
                'LOCAL_CS["Nonearth",UNIT["Meter",1],'
                'AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
            ),
        ),
    )

    for mif_unit in _MIF_NONEARTH_UNITS:
      with self.subTest(unit=mif_unit):
        expected = f'CoordSys NonEarth Units "{mif_unit}"'
        srs = self._srs_from_coordsys(expected)
        self.assertEqual(self._coordsys_from_srs(srs), expected)

  # Upstream: ogr_mitab_35 (CoordSys projection methods 1..33)
  @parameterized.parameters(*_MIF_EARTH_PROJECTIONS)
  def test_srs_earth_projection_method_roundtrip(self, coordsys: str) -> None:
    srs = self._srs_from_coordsys(coordsys)
    self.assertEqual(self._coordsys_from_srs(srs), coordsys)

  # Upstream: ogr_mitab_35
  # (Geographic, UTM, TOWGS84, Lambert93, PseudoMercator, MIF 999/9999, KKJ)
  def test_srs_special_projections_and_custom_datums(self) -> None:
    # Geographic WGS 84 (EPSG:4326)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    self.assertEqual(
        self._coordsys_from_srs(srs), 'CoordSys Earth Projection 1, 104'
    )
    srs_4326 = self._srs_from_coordsys('CoordSys Earth Projection 1, 104')
    self.assertEqual(
        srs_4326.ExportToWkt(),
        'GEOGCS["unnamed",DATUM["WGS_1984",'
        'SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AXIS["Latitude",NORTH],AXIS["Longitude",EAST]]',
    )
    self.assertEqual(
        self._coordsys_from_srs(srs_4326), 'CoordSys Earth Projection 1, 104'
    )

    # Projected UTM 31N (EPSG:32631)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(32631)
    utm_cs = 'CoordSys Earth Projection 8, 104, "m", 3, 0, 0.9996, 500000, 0'
    self.assertEqual(self._coordsys_from_srs(srs), utm_cs)
    srs_32631 = self._srs_from_coordsys(utm_cs)
    self.assertEqual(
        srs_32631.ExportToWkt(),
        'PROJCS["unnamed",GEOGCS["unnamed",DATUM["WGS_1984",'
        'SPHEROID["WGS 84",6378137,298.257223563]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],'
        'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],'
        'PARAMETER["central_meridian",3],PARAMETER["scale_factor",0.9996],'
        'PARAMETER["false_easting",500000],PARAMETER["false_northing",0],'
        'UNIT["metre",1,AUTHORITY["EPSG","9001"]],'
        'AXIS["Easting",EAST],AXIS["Northing",NORTH]]',
    )
    self.assertEqual(self._coordsys_from_srs(srs_32631), utm_cs)

    # WGS 72 (EPSG:4322)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4322)
    self.assertEqual(
        self._coordsys_from_srs(srs), 'CoordSys Earth Projection 1, 103'
    )
    srs_4322 = self._srs_from_coordsys('CoordSys Earth Projection 1, 103')
    self.assertIn(
        srs_4322.ExportToWkt(),
        (
            (
                'GEOGCS["unnamed",DATUM["WGS_1972",'
                'SPHEROID["WGS 72",6378135,298.26]],PRIMEM["Greenwich",0],'
                'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
                'AXIS["Latitude",NORTH],AXIS["Longitude",EAST]]'
            ),
            (
                'GEOGCS["unnamed",DATUM["World_Geodetic_System_1972",'
                'SPHEROID["WGS 72",6378135,298.26]],PRIMEM["Greenwich",0],'
                'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
                'AXIS["Latitude",NORTH],AXIS["Longitude",EAST]]'
            ),
        ),
    )
    self.assertEqual(
        self._coordsys_from_srs(srs_4322), 'CoordSys Earth Projection 1, 103'
    )

    # Lambert 93 (EPSG:2154) with and without MITAB_BOUNDS_FILE
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(2154)
    l93_cs = (
        'CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, 49, 700000, 6600000'
    )
    self.assertEqual(self._coordsys_from_srs(srs), l93_cs)
    srs_2154 = self._srs_from_coordsys(l93_cs)
    self.assertEqual(srs_2154.GetAuthorityCode(None), '2154')
    self.assertEqual(self._coordsys_from_srs(srs_2154), l93_cs)

    srs_french = osr.SpatialReference(_LAMBERT93_FRENCH_BOUNDS_WKT)
    self.assertEqual(
        self._coordsys_from_srs(srs_french),
        'CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, 49.00000000002, '
        '700000, 6600000',
    )
    bounds_path = self.vsimem_dir / 'mitab_bounds.txt'
    _write_vsimem_text(bounds_path, _MITAB_BOUNDS_TXT)
    self.addCleanup(self._reset_mitab_bounds_cache)
    with gdal.config_option('MITAB_BOUNDS_FILE', str(bounds_path)):
      self.assertEqual(
          self._coordsys_from_srs(srs_french),
          'CoordSys Earth Projection 3, 33, "m", 3, 46.5, 44, 49.00000000002, '
          '700000, 6600000 Bounds (75000, 6000000) (1275000, 7200000)',
      )

    # Pseudo-Mercator (ticket #4115)
    srs_3857 = self._srs_from_coordsys(
        'CoordSys Earth Projection 10, 157, "m", 0'
    )
    self.assertEqual(
        srs_3857.ExportToWkt(),
        'PROJCS["WGS 84 / Pseudo-Mercator",GEOGCS["WGS 84",'
        'DATUM["WGS_1984",SPHEROID["WGS 84",6378137,298.257223563,'
        'AUTHORITY["EPSG","7030"]],AUTHORITY["EPSG","6326"]],'
        'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AUTHORITY["EPSG","4326"]],PROJECTION["Mercator_1SP"],'
        'PARAMETER["central_meridian",0],PARAMETER["scale_factor",1],'
        'PARAMETER["false_easting",0],PARAMETER["false_northing",0],'
        'UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],'
        'AXIS["Northing",NORTH],EXTENSION["PROJ4","+proj=merc +a=6378137 '
        '+b=6378137 +lat_ts=0 +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m '
        '+nadgrids=@null +wktext +no_defs"]]',
    )

    # MIF 999
    srs_999 = osr.SpatialReference(
        'GEOGCS["unnamed",DATUM["MIF 999,1,1,2,3",'
        'SPHEROID["WGS 72",6378135,298.26]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433]]'
    )
    self.assertEqual(
        self._coordsys_from_srs(srs_999),
        'CoordSys Earth Projection 1, 999, 1, 1, 2, 3',
    )
    self.assertEqual(
        self._srs_from_coordsys(
            'CoordSys Earth Projection 1, 999, 1, 1, 2, 3'
        ).ExportToWkt(),
        'GEOGCS["unnamed",DATUM["MIF 999,1,1,2,3",'
        'SPHEROID["WGS 72",6378135,298.26],TOWGS84[1,2,3,0,0,0,0]],'
        'PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AXIS["Latitude",NORTH],AXIS["Longitude",EAST]]',
    )

    # MIF 9999
    srs_9999 = osr.SpatialReference(
        'GEOGCS["unnamed",DATUM["MIF 9999,1,1,2,3,4,5,6,7,3",'
        'SPHEROID["WGS 72",6378135,298.26]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433]]'
    )
    self.assertEqual(
        self._coordsys_from_srs(srs_9999),
        'CoordSys Earth Projection 1, 9999, 1, 1, 2, 3, 4, 5, 6, 7, 3',
    )
    self.assertEqual(
        self._srs_from_coordsys(
            'CoordSys Earth Projection 1, 9999, 1, 1, 2, 3, 4, 5, 6, 7, 3'
        ).ExportToWkt(),
        'GEOGCS["unnamed",DATUM["MIF 9999,1,1,2,3,4,5,6,7,3",'
        'SPHEROID["WGS 72",6378135,298.26],TOWGS84[1,2,3,-4,-5,-6,7]],'
        'PRIMEM["non-Greenwich",3],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AXIS["Latitude",NORTH],AXIS["Longitude",EAST]]',
    )

    # EPSG:2393 / KKJ Finland Uniform Coordinate System
    srs_kkj = osr.SpatialReference(
        'PROJCS["KKJ / Finland Uniform Coordinate System",'
        'GEOGCS["KKJ",DATUM["Kartastokoordinaattijarjestelma_1966",'
        'SPHEROID["International 1924",6378388,297,AUTHORITY["EPSG","7022"]],'
        'AUTHORITY["EPSG","6123"]],PRIMEM["Greenwich",0,'
        'AUTHORITY["EPSG","8901"]],UNIT["degree",0.0174532925199433,'
        'AUTHORITY["EPSG","9122"]],AUTHORITY["EPSG","4123"]],'
        'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],'
        'PARAMETER["central_meridian",27],PARAMETER["scale_factor",1],'
        'PARAMETER["false_easting",3500000],PARAMETER["false_northing",0],'
        'UNIT["metre",1,AUTHORITY["EPSG","9001"]],AXIS["Northing",NORTH],'
        'AXIS["Easting",EAST],AUTHORITY["EPSG","2393"]]'
    )
    kkj_cs = 'CoordSys Earth Projection 24, 1016, "m", 27, 0, 1, 3500000, 0'
    self.assertEqual(self._coordsys_from_srs(srs_kkj), kkj_cs)
    srs_kkj_got = self._srs_from_coordsys(kkj_cs)
    self.assertEqual(
        srs_kkj_got.ExportToWkt(),
        'PROJCS["unnamed",GEOGCS["unnamed",'
        'DATUM["Kartastokoordinaattijarjestelma_1966",'
        'SPHEROID["International 1924",6378388,297]],PRIMEM["Greenwich",0],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],'
        'PROJECTION["Transverse_Mercator"],PARAMETER["latitude_of_origin",0],'
        'PARAMETER["central_meridian",27],PARAMETER["scale_factor",1],'
        'PARAMETER["false_easting",3500000],PARAMETER["false_northing",0],'
        'UNIT["metre",1,AUTHORITY["EPSG","9001"]],'
        'AXIS["Easting",EAST],AXIS["Northing",NORTH]]',
    )
    self.assertEqual(self._coordsys_from_srs(srs_kkj_got), kkj_cs)

  # Upstream: ogr_mitab_36
  def test_mapinfo_file_without_index(self) -> None:
    tab_path = self._copy_fixture_to_tmp(
        'polygon_without_index', ('tab', 'dat', 'id', 'map')
    )
    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      feature = lyr.GetNextFeature()
      geometry = feature.GetGeometryRef().Clone()
      ring = geometry.GetGeometryRef(0)
      orig_x1 = ring.GetX(1)
      ring.SetPoint_2D(1, orig_x1 + 100, ring.GetY(1))
      expected_geom = geometry.Clone()
      feature.SetGeometry(geometry)
      self.assertEqual(lyr.SetFeature(feature), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      feature = lyr.GetNextFeature()
      self.assertIsNotNone(feature)
      self._assert_geometry_equal(
          feature.GetGeometryRef(), expected_geom, max_error=0.1
      )
      # Ensure reading the remainder of the file succeeds cleanly.
      remaining = sum(1 for _ in iter(lyr.GetNextFeature, None))
      self.assertGreater(remaining, 0)

    self.driver.Delete(str(tab_path))

  # Upstream: ogr_mitab_37
  def test_simple_seamless_table(self) -> None:
    filepath = pathlib.Path(ogr_util.GetTestFilePath('mitab/seamless.tab'))
    self.assertTrue(filepath.exists())
    with self._open_mitab(filepath) as ds:
      layer = ds.GetLayer(0)
      self.assertEqual(layer.GetFeatureCount(), 4)

      expected = (
          (4294967297, '1'),
          (4294967298, '2'),
          (8589934593, '3'),
          (8589934594, '4'),
      )
      for expected_fid, expected_id in expected:
        feat = layer.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetFID(), expected_fid)
        self.assertEqual(feat.GetField('id'), expected_id)

      feat_first = layer.GetFeature(4294967297)
      self.assertEqual(feat_first.GetFID(), 4294967297)
      self.assertEqual(feat_first.GetField('id'), '1')

      feat_last = layer.GetFeature(8589934594)
      self.assertEqual(feat_last.GetFID(), 8589934594)
      self.assertEqual(feat_last.GetField('id'), '4')

      self.assertIsNone(layer.GetFeature(8589934594 + 1))
      self.assertIsNone(layer.GetFeature(4294967297 * 2 + 1))

  # Upstream: ogr_mitab_38
  def test_open_mif_with_tab_delimiter_and_empty_first_field(self) -> None:
    filepath = pathlib.Path(
        ogr_util.GetTestFilePath(
            'mitab/empty_first_field_with_tab_delimiter.mif'
        )
    )
    self.assertTrue(filepath.exists())
    with self._open_mitab(filepath) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('field1'), '')
      self.assertEqual(feat.GetField('field2'), 'foo')

  # Upstream: ogr_mitab_39, ogr_mitab_41, ogr_mitab_42
  @parameterized.named_parameters(
      dict(testcase_name='mif', rel_path='mitab/all_geoms.mif', vsizip=False),
      dict(testcase_name='tab', rel_path='mitab/all_geoms.tab', vsizip=False),
      dict(
          testcase_name='tab_block_32256',
          rel_path='mitab/all_geoms_block_32256.zip',
          vsizip=True,
      ),
  )
  def test_read_all_geometry_types_and_styles(
      self, rel_path: str, vsizip: bool
  ) -> None:
    src_file = pathlib.Path(ogr_util.GetTestFilePath(rel_path))
    self.assertTrue(src_file.exists())
    open_path = f'/vsizip/{src_file}' if vsizip else str(src_file)
    golden_path = pathlib.Path(
        ogr_util.GetTestFilePath('mitab/all_geoms.mif.golden.csv')
    )
    self.assertTrue(golden_path.exists())

    with (
        gdal.OpenEx(open_path, gdal.OF_VECTOR | gdal.OF_READONLY) as ds,
        gdal.OpenEx(
            str(golden_path), gdal.OF_VECTOR | gdal.OF_READONLY
        ) as ds_ref,
    ):
      lyr = ds.GetLayer(0)
      lyr_ref = ds_ref.GetLayer(0)
      idx = 0
      while True:
        feat = lyr.GetNextFeature()
        feat_ref = lyr_ref.GetNextFeature()
        if feat is None:
          self.assertIsNone(feat_ref)
          break
        self.assertIsNotNone(feat_ref)
        with self.subTest(feature_index=idx):
          self._assert_geometry_equal(
              feat.GetGeometryRef(), feat_ref.GetGeometryRef()
          )
          self.assertEqual(feat.GetStyleString(), feat_ref.GetStyleString())
        idx += 1
      self.assertGreater(idx, 0)

  # Upstream: ogr_mitab_40
  def test_read_truncated_mif_does_not_crash(self) -> None:
    src_path = pathlib.Path(ogr_util.GetTestFilePath('mitab/all_geoms.mif'))
    content = src_path.read_bytes()
    trunc_path = self.vsimem_dir / 'truncated.mif'

    opened_count = 0
    features_read = 0
    for length in range(len(content)):
      gdal.FileFromMemBuffer(str(trunc_path), content[:length])
      with gdal.quiet_errors():
        try:
          with gdal.OpenEx(
              str(trunc_path), gdal.OF_VECTOR | gdal.OF_READONLY
          ) as ds:
            if ds is not None:
              opened_count += 1
              lyr = ds.GetLayer(0)
              for _ in lyr:
                features_read += 1
        except RuntimeError:
          pass
    self.assertGreater(opened_count, 0)
    self.assertGreater(features_read, 0)

  # Upstream: ogr_mitab_43
  def test_create_tab_with_blocksize_32256(self) -> None:
    zip_path = pathlib.Path(
        ogr_util.GetTestFilePath('mitab/all_geoms_block_32256.zip')
    )
    self.assertTrue(zip_path.exists())
    tab_512 = self.vsimem_dir / 'all_geoms_512.tab'
    tab_32256 = self.vsimem_dir / 'all_geoms_32256.tab'
    tab_invalid = self.vsimem_dir / 'all_geoms_invalid.tab'

    with gdal.OpenEx(
        f'/vsizip/{zip_path}', gdal.OF_VECTOR | gdal.OF_READONLY
    ) as src_ds:
      with gdal.VectorTranslate(str(tab_512), src_ds, format=DRIVER) as _:
        pass
      with gdal.VectorTranslate(
          str(tab_32256),
          src_ds,
          format=DRIVER,
          datasetCreationOptions=['BLOCKSIZE=32256'],
      ) as _:
        pass
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'invalid block size: 32768'),
      ):
        gdal.VectorTranslate(
            str(tab_invalid),
            src_ds,
            format=DRIVER,
            datasetCreationOptions=['BLOCKSIZE=32768'],
        )

    self.assertEqual(gdal.VSIStatL(str(tab_512.with_suffix('.map'))).size, 7168)
    self.assertEqual(
        gdal.VSIStatL(str(tab_32256.with_suffix('.map'))).size, 161280
    )

    with self._open_mitab(tab_32256) as ds, self._open_mitab(tab_512) as ds_ref:
      lyr = ds.GetLayer(0)
      lyr_ref = ds_ref.GetLayer(0)
      feat_count = 0
      while True:
        feat = lyr.GetNextFeature()
        feat_ref = lyr_ref.GetNextFeature()
        if feat is None:
          self.assertIsNone(feat_ref)
          break
        self.assertIsNotNone(feat_ref)
        self._assert_geometry_equal(
            feat.GetGeometryRef(), feat_ref.GetGeometryRef()
        )
        self.assertEqual(feat.GetStyleString(), feat_ref.GetStyleString())
        feat_count += 1
      self.assertGreater(feat_count, 0)

  # Upstream: ogr_mitab_44
  def test_numeric_field_width_and_precision_limits(self) -> None:
    mif_path = self.vsimem_dir / 'numeric_limits.mif'
    with self.driver.CreateVector(str(mif_path)) as ds:
      lyr = ds.CreateLayer('test')
      fld = ogr.FieldDefn('test', ogr.OFTReal)
      fld.SetWidth(30)
      fld.SetPrecision(29)
      lyr.CreateField(fld)

    with self._open_mitab(mif_path) as ds:
      fld = ds.GetLayer(0).GetLayerDefn().GetFieldDefn(0)
      self.assertEqual(fld.GetWidth(), 20)
      self.assertEqual(fld.GetPrecision(), 16)

  # Upstream: ogr_mitab_45 (adapted for CPLRecodeStub without libiconv)
  @parameterized.named_parameters(
      dict(
          testcase_name='iso_8859_1_mif_1',
          fmt='MIF',
          layer_count=1,
          encoding='ISO-8859-1',
          field_names=('field1', 'café'),
          feature_names=('résumé', 'naïve', 'garçon'),
      ),
      dict(
          testcase_name='iso_8859_1_tab_1',
          fmt='TAB',
          layer_count=1,
          encoding='ISO-8859-1',
          field_names=('field1', 'café'),
          feature_names=('résumé', 'naïve', 'garçon'),
      ),
      dict(
          testcase_name='iso_8859_1_mif_2',
          fmt='MIF',
          layer_count=2,
          encoding='ISO-8859-1',
          field_names=('field1', 'café'),
          feature_names=('résumé', 'naïve', 'garçon'),
      ),
      dict(
          testcase_name='iso_8859_1_tab_2',
          fmt='TAB',
          layer_count=2,
          encoding='ISO-8859-1',
          field_names=('field1', 'café'),
          feature_names=('résumé', 'naïve', 'garçon'),
      ),
      # Note: MITAB truncates normalized field names at 31 bytes
      # (IMapInfoFile::NormalizeFieldName), so keep the Cyrillic field <= 31B.
      dict(
          testcase_name='utf8_mif_1',
          fmt='MIF',
          layer_count=1,
          encoding='UTF-8',
          field_names=('field1', 'абвгдежзийклмно'),
          feature_names=('аз', 'буки', 'веди'),
      ),
      dict(
          testcase_name='utf8_tab_1',
          fmt='TAB',
          layer_count=1,
          encoding='UTF-8',
          field_names=('field1', 'абвгдежзийклмно'),
          feature_names=('аз', 'буки', 'веди'),
      ),
      dict(
          testcase_name='utf8_mif_2',
          fmt='MIF',
          layer_count=2,
          encoding='UTF-8',
          field_names=('field1', 'абвгдежзийклмно'),
          feature_names=('аз', 'буки', 'веди'),
      ),
      dict(
          testcase_name='utf8_tab_2',
          fmt='TAB',
          layer_count=2,
          encoding='UTF-8',
          field_names=('field1', 'абвгдежзийклмно'),
          feature_names=('аз', 'буки', 'веди'),
      ),
  )
  def test_read_write_layers_with_encoding(
      self,
      fmt: str,
      layer_count: int,
      encoding: str,
      field_names: Sequence[str],
      feature_names: Sequence[str],
  ) -> None:
    layer_names = ('lyr1', 'lyr2')
    ext = f'.{fmt.lower()}' if layer_count == 1 else ''
    ds_path = self.vsimem_dir / f'enc_{fmt}_{layer_count}{ext}'

    with self.driver.CreateVector(
        str(ds_path), options=[f'FORMAT={fmt}']
    ) as ds:
      for lyr_idx in range(layer_count):
        layer = ds.CreateLayer(
            layer_names[lyr_idx], options=[f'ENCODING={encoding}']
        )
        self.assertIsNotNone(layer)
        self.assertTrue(layer.TestCapability(ogr.OLCStringsAsUTF8))

        for field_name in field_names:
          field_def = ogr.FieldDefn(field_name, ogr.OFTString)
          field_def.SetWidth(254)
          layer.CreateField(field_def)

        for feat_name in feature_names:
          feat = ogr.Feature(layer.GetLayerDefn())
          feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (25 72)'))
          for field_name in field_names:
            feat.SetField(field_name, f'{field_name} {feat_name}')
          self.assertEqual(layer.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(ds_path) as ds:
      self.assertEqual(ds.GetLayerCount(), layer_count)
      for lyr_idx in range(layer_count):
        layer = ds.GetLayer(lyr_idx)
        self.assertIsNotNone(layer)
        for fld_idx, expected_name in enumerate(field_names):
          self.assertEqual(
              layer.GetLayerDefn().GetFieldDefn(fld_idx).GetName(),
              expected_name,
          )
        for feat_name in feature_names:
          feat = layer.GetNextFeature()
          self.assertIsNotNone(feat)
          for fld_idx, field_name in enumerate(field_names):
            expected_val = f'{field_name} {feat_name}'
            self.assertEqual(feat.GetField(fld_idx), expected_val)
            self.assertEqual(feat.GetField(field_name), expected_val)

  def test_utf8_field_name_31_byte_truncation(self) -> None:
    # Upstream ogr_mitab_45 wrote 31 Cyrillic chars as 31 single-byte CP1251
    # chars; in UTF-8 (62 bytes), IMapInfoFile::NormalizeFieldName
    # (mitab_imapinfofile.cpp) truncates at 31 bytes mid-codepoint (dangling
    # 0xd0 leading byte of 'п'), so round-trip tests keep UTF-8 names <= 30B.
    long_name = 'абвгдежзийклмнопрстуфхцчшщьъэюя'
    tab_path = self.vsimem_dir / 'utf8_trunc_31b.tab'
    with gdal.quiet_warnings(), self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('lyr1', options=['ENCODING=UTF-8'])
      lyr.CreateField(ogr.FieldDefn(long_name, ogr.OFTString))

    with self._open_mitab(tab_path) as ds:
      got_name = ds.GetLayer(0).GetLayerDefn().GetFieldDefn(0).GetName()
      raw_name = got_name.encode('utf-8', 'surrogateescape')
      self.assertLen(raw_name, 31)
      self.assertEqual(raw_name, 'абвгдежзийклмно'.encode('utf-8') + b'\xd0')

  # Google3-specific test (pins CPLRecodeStub without HAVE_ICONV / gconv).
  def test_cp1251_write_stub_pinned(self) -> None:
    # Without gconv/iconv, writing UTF-8 strings to ENCODING=CP1251 leaves raw
    # UTF-8 bytes un-transcoded; CPLRecodeStub warns once per process
    # (bHaveWarned3 in cpl_recode_stub.cpp), so warnings are quieted here.
    tab_path = self.vsimem_dir / 'cp1251_stub.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('lyr1', options=['ENCODING=CP1251'])
      self.assertTrue(lyr.TestCapability(ogr.OLCStringsAsUTF8))
      lyr.CreateField(ogr.FieldDefn('field1', ogr.OFTString))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT (25 72)'))
      feat.SetField('field1', 'аз')
      with gdal.quiet_warnings():
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertIsNotNone(feat)
      # Reading treats un-recoded UTF-8 bytes b'\xd0\xb0\xd0\xb7' as CP1251.
      self.assertEqual(
          feat.GetField('field1'), 'аз'.encode('utf-8').decode('cp1251')
      )

  # Upstream: ogr_mitab_46
  @parameterized.named_parameters(
      dict(testcase_name='tab_win1251', filename='tab-win1251.TAB'),
      dict(testcase_name='mif_win1251', filename='win1251.mif'),
      dict(testcase_name='mif_utf8', filename='utf8.mif'),
  )
  def test_read_layers_with_encoding(self, filename: str) -> None:
    base_fields = ('Поле_А', 'Поле_Б', 'Поле_В', 'Поле_Г', 'Поле_Д')
    if filename == 'win1251.mif':
      # TODO: b/335317901 - Without HAVE_ICONV, MIFFile::AddFields recodes
      # CP1251->UTF-8, then IMapInfoFile::NormalizeFieldName runs UTF-8->CP1251
      # (stub no-op) and CP1251->UTF-8 again, double-encoding header names.
      expected_fields = tuple(
          name.encode('utf-8').decode('cp1251') for name in base_fields
      )
    else:
      expected_fields = base_fields
    expected_rows = (
        ('Значение А', 'Значение Б', 'Значение В', 'Значение Г', 'Значение Д'),
        ('Значение 1', 'Значение 2', 'Значение 3', 'Значение 4', 'Значение 5'),
        ('Полигон', 'Синий', 'Заливка', 'А а Б б', 'ЪЫЁЩ'),
    )
    ds_path = pathlib.Path(ogr_util.GetTestFilePath(f'mitab/{filename}'))
    self.assertTrue(ds_path.exists())

    with gdal.quiet_warnings(), self._open_mitab(ds_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertIsNotNone(lyr)
      self.assertTrue(lyr.TestCapability(ogr.OLCStringsAsUTF8))
      for idx, expected_name in enumerate(expected_fields):
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(idx).GetName(), expected_name
        )
      for expected_row in expected_rows:
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        for idx, field_name in enumerate(expected_fields):
          self.assertEqual(feat.GetField(idx), expected_row[idx])
          self.assertEqual(feat.GetField(field_name), expected_row[idx])

  # Upstream: ogr_mitab_47
  def test_open_dataset_with_and_without_ind(self) -> None:
    indexed_tab = pathlib.Path(
        ogr_util.GetTestFilePath('mitab/poly_indexed.tab')
    )
    self.assertTrue(indexed_tab.exists())
    with self._open_mitab(indexed_tab) as ds:
      lyr = ds.GetLayer(0)
      with _attribute_filter(lyr, "PRFEDEA = '35043413'"):
        self.assertEqual(lyr.GetFeatureCount(), 1)

    for ext in ('tab', 'dat', 'map', 'id'):
      src = pathlib.Path(ogr_util.GetTestFilePath(f'mitab/poly_indexed.{ext}'))
      self.assertTrue(src.exists())
      gdal.FileFromMemBuffer(
          str(self.vsimem_dir / f'poly_indexed.{ext}'), src.read_bytes()
      )
    with self._open_mitab(self.vsimem_dir / 'poly_indexed.tab') as ds:
      lyr = ds.GetLayer(0)
      with _attribute_filter(lyr, "PRFEDEA = '35043413'"):
        self.assertEqual(lyr.GetFeatureCount(), 1)

  # Upstream: ogr_mitab_48
  def test_write_and_read_lcc_1sp(self) -> None:
    mif_path = self.vsimem_dir / 'lcc1sp.mif'
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["NTF (Paris) / France IV (deprecated)",'
        'GEOGCS["NTF (Paris)",'
        'DATUM["Nouvelle_Triangulation_Francaise_Paris",'
        'SPHEROID["Clarke 1880 (IGN)",6378249.2,293.4660212936269,'
        'AUTHORITY["EPSG","7011"]],'
        'TOWGS84[-168,-60,320,0,0,0,0],'
        'AUTHORITY["EPSG","6807"]],'
        'PRIMEM["Paris",2.33722917,AUTHORITY["EPSG","8903"]],'
        'UNIT["grad",0.01570796326794897,AUTHORITY["EPSG","9105"]],'
        'AUTHORITY["EPSG","4807"]],'
        'PROJECTION["Lambert_Conformal_Conic_1SP"],'
        'PARAMETER["latitude_of_origin",46.85],'
        'PARAMETER["central_meridian",0],'
        'PARAMETER["scale_factor",0.99994471],'
        'PARAMETER["false_easting",234.358],'
        'PARAMETER["false_northing",4185861.369],'
        'UNIT["metre",1,AUTHORITY["EPSG","9001"]],'
        'AXIS["X",EAST],AXIS["Y",NORTH],AUTHORITY["EPSG","27584"]]'
    )
    with self.driver.CreateVector(str(mif_path)) as ds:
      lyr = ds.CreateLayer('foo', srs=srs)
      lyr.CreateField(ogr.FieldDefn('foo', ogr.OFTString))

    sr_expected = osr.SpatialReference()
    sr_expected.SetFromUserInput(
        'PROJCS["unnamed",GEOGCS["unnamed",'
        'DATUM["NTF_Paris_Meridian",'
        'SPHEROID["Clarke 1880 (modified for IGN)",6378249.2,293.4660213],'
        'TOWGS84[-168,-60,320,0,0,0,0]],'
        'PRIMEM["Paris",2.33722917],UNIT["degree",0.0174532925199433]],'
        'PROJECTION["Lambert_Conformal_Conic_1SP"],'
        'PARAMETER["latitude_of_origin",42.165],'
        'PARAMETER["central_meridian",0],'
        'PARAMETER["scale_factor",0.99994471],'
        'PARAMETER["false_easting",234.358],'
        'PARAMETER["false_northing",4185861.369],'
        'UNIT["metre",1]]'
    )
    with self._open_mitab(mif_path) as ds:
      sr_got = ds.GetLayer(0).GetSpatialRef()
      self.assertIsNotNone(sr_got)
      self.assertIsSame(sr_got, sr_expected)

  # Upstream: ogr_mitab_49_aspatial
  def test_read_aspatial_tab(self) -> None:
    filepath = pathlib.Path(ogr_util.GetTestFilePath('mitab/aspatial.tab'))
    self.assertTrue(filepath.exists())
    with self._open_mitab(filepath) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetGeomType(), ogr.wkbNone)
      self.assertIsNone(lyr.GetSpatialRef())
      self.assertIsNone(lyr.GetExtent(can_return_null=True))

  # Upstream: test_ogr_mitab_tab_field_index_creation
  def test_tab_field_index_creation(self) -> None:
    layer_name = 'tab_field_index'
    tab_path = self.vsimem_dir / f'{layer_name}.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer(layer_name)
      lyr.CreateField(ogr.FieldDefn('id', ogr.OFTInteger))
      lyr.CreateField(ogr.FieldDefn('other_field', ogr.OFTInteger))

      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r"no such layer as `foo'\."),
      ):
        ds.ExecuteSQL('CREATE INDEX ON foo USING id')
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'field not found'),
      ):
        ds.ExecuteSQL(f'CREATE INDEX ON {layer_name} USING foo')

      ds.ExecuteSQL(f'CREATE INDEX ON {layer_name} USING id')
      ds.ExecuteSQL(f'CREATE INDEX ON {layer_name} USING id')

      for val in (100, 200):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetField(0, val)
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    self.assertIsNotNone(gdal.VSIStatL(str(tab_path.with_suffix('.ind'))))

    with self._open_mitab(tab_path) as ds:
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r'SetFieldIndexed\(\) must be called after opening a new '
              r'dataset, but before writing the first feature',
          ),
      ):
        ds.ExecuteSQL(f'CREATE INDEX ON {layer_name} USING other_field')
      lyr = ds.GetLayer(0)
      with _attribute_filter(lyr, 'id = 200'):
        self.assertEqual(lyr.GetFeatureCount(), 1)
        feat = lyr.GetNextFeature()
        self.assertEqual(feat.GetField('id'), 200)

  # Upstream: test_ogr_mitab_tab_view
  def test_tab_view(self) -> None:
    view1 = pathlib.Path(
        ogr_util.GetTestFilePath('mitab/view_first_table_second_table.tab')
    )
    self.assertTrue(view1.exists())
    with self._open_mitab(view1) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat['ID'], 100)
      self.assertEqual(feat['foo'], 'foo')

    view2 = pathlib.Path(
        ogr_util.GetTestFilePath(
            'mitab/view_select_all_first_table_second_table.tab'
        )
    )
    self.assertTrue(view2.exists())
    with self._open_mitab(view2) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 3)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat['joint_field'], 1)
      self.assertEqual(feat['ID'], 100)
      self.assertEqual(feat['foo'], 'foo')

  # Upstream: test_ogr_mitab_style
  def test_brush_and_pen_styles_roundtrip(self) -> None:
    tab_path = self.vsimem_dir / 'styles.tab'
    input_styles = (
        'BRUSH(fc:#AABBCC,bc:#DDEEFF);PEN(c:#DDEEFF)',
        'BRUSH(fc:#AABBCC,id:"mapinfo-brush-1");PEN(w:2px)',
        'BRUSH(fc:#AABBCC00,bc:#ddeeff00);PEN(w:2.5pt)',
    )
    expected_styles = (
        (
            'BRUSH(fc:#aabbcc,bc:#ddeeff,id:"mapinfo-brush-2,ogr-brush-0");'
            'PEN(w:1px,c:#ddeeff,id:"mapinfo-pen-2,ogr-pen-0",cap:r,j:r)'
        ),
        (
            'BRUSH(fc:#aabbcc,id:"mapinfo-brush-1,ogr-brush-1");'
            'PEN(w:2px,c:#000000,id:"mapinfo-pen-2,ogr-pen-0",cap:r,j:r)'
        ),
        (
            'BRUSH(fc:#aabbcc,id:"mapinfo-brush-1,ogr-brush-1");'
            'PEN(w:2.5pt,c:#000000,id:"mapinfo-pen-2,ogr-pen-0",cap:r,j:r)'
        ),
    )

    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('id', ogr.OFTInteger))
      for style in input_styles:
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometryDirectly(
            ogr.CreateGeometryFromWkt('POLYGON((0 0,0 1,1 1,0 0))')
        )
        feat.SetStyleString(style)
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      for expected in expected_styles:
        feat = lyr.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetStyleString(), expected)

  # Upstream: test_ogr_mitab_tab_write_field_name_with_dot
  def test_tab_write_field_name_with_dot_laundered(self) -> None:
    tab_path = self.vsimem_dir / 'field_with_dot.tab'
    with gdal.quiet_warnings(), self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('with.dot', ogr.OFTInteger))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat['with.dot'] = 1
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(2 3)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(feat['with_dot'], 1)

  # Upstream: test_ogr_mitab_write_utf8_field_name
  @parameterized.named_parameters(
      dict(testcase_name='mif', ext='mif'),
      dict(testcase_name='tab', ext='tab'),
  )
  def test_write_utf8_field_name(self, ext: str) -> None:
    out_path = self.vsimem_dir / f'utf8_field_name.{ext}'
    with (
        gdal.quiet_warnings(),
        self.driver.CreateVector(
            str(out_path), options=['ENCODING=UTF-8', f'FORMAT={ext}']
        ) as ds,
    ):
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('地市', ogr.OFTInteger))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat['地市'] = 1
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(2 3)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(out_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(feat['地市'], 1)

  # Upstream: test_ogr_mitab_non_strict_fields_laundering
  @parameterized.named_parameters(
      dict(testcase_name='mif_layer_opt', ext='mif', ds_strict_opt=False),
      dict(testcase_name='mif_ds_opt', ext='mif', ds_strict_opt=True),
      dict(testcase_name='tab_layer_opt', ext='tab', ds_strict_opt=False),
      dict(testcase_name='tab_ds_opt', ext='tab', ds_strict_opt=True),
  )
  def test_non_strict_fields_laundering(
      self, ext: str, ds_strict_opt: bool
  ) -> None:
    out_path = self.vsimem_dir / f'non_strict.{ext}'
    ds_opt = [f'FORMAT={ext}']
    lyr_opt: list[str] = []
    if ds_strict_opt:
      ds_opt.append('STRICT_FIELDS_NAME_LAUNDERING=NO')
    else:
      lyr_opt.append('STRICT_FIELDS_NAME_LAUNDERING=NO')

    with (
        gdal.quiet_warnings(),
        self.driver.CreateVector(str(out_path), options=ds_opt) as ds,
    ):
      lyr = ds.CreateLayer('test', options=lyr_opt)
      lyr.CreateField(ogr.FieldDefn('dot.and space', ogr.OFTInteger))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat['dot.and space'] = 1
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(2 3)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(out_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertEqual(feat['dot.and_space'], 1)

  # Upstream: test_ogr_mitab_local_encoding_label
  @parameterized.named_parameters(
      dict(
          testcase_name='mif_win1251_text',
          filename='win1251_text.mif',
          expected_style=(
              'LABEL(t:"Поле",a:0.000000,s:2.070000g,c:#ff0000,p:2,'
              'f:"DejaVu Serif")'
          ),
      ),
      dict(
          testcase_name='tab_win1251_text',
          filename='tab-win1251_text.tab',
          expected_style=(
              'LABEL(t:"Поле",a:0.000000,s:0.015375g,c:#000000,p:1,'
              'f:"Times New Roman")'
          ),
      ),
  )
  def test_read_local_encoding_text_label(
      self, filename: str, expected_style: str
  ) -> None:
    ds_path = pathlib.Path(ogr_util.GetTestFilePath(f'mitab/{filename}'))
    self.assertTrue(ds_path.exists())
    with self._open_mitab(ds_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertTrue(lyr.TestCapability(ogr.OLCStringsAsUTF8))
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetStyleString(), expected_style)

  # Upstream: test_ogr_mitab_delete_feature_no_geometry (GH #1232)
  def test_delete_feature_no_geometry(self) -> None:
    tab_path = self.vsimem_dir / 'delete_no_geom.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)
      lyr.CreateField(ogr.FieldDefn('id', ogr.OFTInteger))
      for val in (1, 2):
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat['id'] = val
        self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path, update=True) as ds:
      self.assertEqual(ds.GetLayer(0).DeleteFeature(1), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat['id'], 2)

  # Upstream: test_ogr_mitab_too_large_value_for_decimal_field (GH #1636)
  def test_too_large_value_for_decimal_field(self) -> None:
    tab_path = self.vsimem_dir / 'decimal_overflow.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test', geom_type=ogr.wkbNone)
      fld = ogr.FieldDefn('f', ogr.OFTReal)
      fld.SetWidth(20)
      fld.SetPrecision(12)
      lyr.CreateField(fld)

      feat_ok = ogr.Feature(lyr.GetLayerDefn())
      feat_ok['f'] = 1234567.012
      self.assertEqual(lyr.CreateFeature(feat_ok), ogr.OGRERR_NONE)

      feat_overflow = ogr.Feature(lyr.GetLayerDefn())
      feat_overflow['f'] = 123456789.012
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'Cannot format .* as a 20\.12 field'
          ),
      ):
        lyr.CreateFeature(feat_overflow)

  # Upstream: test_ogr_mitab_custom_datum_export
  def test_custom_datum_export(self) -> None:
    srs = osr.SpatialReference()
    srs.SetGeogCS('Custom', 'Custom', 'Sphere', 6370997.0, 0.0)
    srs.SetTOWGS84(1, 2, 3, 4, 5, 6, 7)
    proj = srs.ExportToMICoordSys()
    self.assertEqual(
        proj, 'Earth Projection 1, 9999, 12, 1, 2, 3, -4, -5, -6, 7, 0'
    )

    srs_roundtrip = osr.SpatialReference()
    srs_roundtrip.ImportFromMICoordSys(proj)
    self.assertEqual(srs_roundtrip.GetTOWGS84(), (1, 2, 3, 4, 5, 6, 7))

    srs_utm = osr.SpatialReference()
    srs_utm.SetGeogCS('Custom', 'Custom', 'NWL-9D or WGS-66', 6378145.0, 298.25)
    srs_utm.SetTOWGS84(1, 2, 3, 4, 5, 6, 7)
    srs_utm.SetUTM(33)
    self.assertEqual(
        srs_utm.ExportToMICoordSys(),
        'Earth Projection 8, 9999, 42, 1, 2, 3, -4, -5, -6, 7, 0, '
        '"m", 15, 0, 0.9996, 500000, 0',
    )

  # Upstream: test_ogr_mitab_description (uses UTF-8 instead of CP1251 write)
  def test_tab_description_metadata(self) -> None:
    tab_path = self.vsimem_dir / 'description.tab'
    long_description = (
        'Состав данных: Топокарты (растр) 1:50К, 100К, 250К, 500К, '
        'Топокарты (вектор) 1:100К, 1:250К, ЦМР 10м, Реестр географических '
        'названий 1:100000, АТД 1:10000, лидарная съемка, ортофото. '
        'Лицензия: на геоданные - ограничительная, не соответствующая '
        'определению "открытых данных", так как запрещено распространение '
        'данных.'
    )
    expected_truncated = (
        'Состав данных: Топокарты (растр) 1:50К, 100К, 250К, 500К, '
        'Топокарты (вектор) 1:100К, 1:250К, ЦМР 10м, Реестр географических '
        'названий 1:100000, АТД 1:10000, лидарная съемка, ортофото. '
        'Лицензия: на геоданные - ограничительная, не соответствующая '
        'определению "открытых данных", так как запрещено распростр'
    )

    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer(
          'test_description',
          options=['ENCODING=UTF-8', f'DESCRIPTION={long_description}'],
      )
      lyr.CreateField(ogr.FieldDefn('feature_id', ogr.OFTInteger))
      lyr.CreateField(ogr.FieldDefn('other_field', ogr.OFTInteger))
      self.assertEqual(lyr.GetMetadataItem('DESCRIPTION'), expected_truncated)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetMetadataItem('DESCRIPTION'), expected_truncated)
      short_desc = 'Состав данных: Топокарты (растр) 1:50К, 100К, 250К, 500К'
      lyr.SetMetadataItem('DESCRIPTION', short_desc)

    with self._open_mitab(tab_path) as ds:
      self.assertEqual(
          ds.GetLayer(0).GetMetadataItem('DESCRIPTION'), short_desc
      )

    # Line breaks become spaces; quotes are preserved across round-trip.
    multiline_desc = (
        'Состав данных: "Топокарты (растр)"\n1:50К,\n100К,\n250К,\n'
        '500К\r\n"new line"'
    )
    expected_single_line = (
        'Состав данных: "Топокарты (растр)" 1:50К, 100К, 250К, 500К  "new line"'
    )
    with self._open_mitab(tab_path, update=True) as ds:
      ds.GetLayer(0).SetMetadataItem('DESCRIPTION', multiline_desc)

    with self._open_mitab(tab_path) as ds:
      self.assertEqual(
          ds.GetLayer(0).GetMetadataItem('DESCRIPTION'), expected_single_line
      )

  # Upstream: test_ogr_mitab_nulldatetime
  def test_null_date_time_datetime_roundtrip(self) -> None:
    tab_path = self.vsimem_dir / 'nulldatetime.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('nulldatetime')
      lyr.CreateField(ogr.FieldDefn('time', ogr.OFTTime))
      lyr.CreateField(ogr.FieldDefn('date', ogr.OFTDate))
      lyr.CreateField(ogr.FieldDefn('datetime', ogr.OFTDateTime))
      self.assertEqual(
          lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn())), ogr.OGRERR_NONE
      )

    tab_text = _read_vsimem_text(tab_path)
    self.assertIn('time Time', tab_text)
    self.assertIn('date Date', tab_text)
    self.assertIn('datetime DateTime', tab_text)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      # TODO: b/335317901 - Built without -DMITAB_USE_OFTDATETIME, so
      # Date/Time/DateTime columns read back as OFTString ('') and
      # IsFieldSet() is True instead of False as in upstream.
      for name in ('time', 'date', 'datetime'):
        idx = lyr.GetLayerDefn().GetFieldIndex(name)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(idx).GetType(), ogr.OFTString
        )
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      for name in ('time', 'date', 'datetime'):
        self.assertTrue(feat.IsFieldSet(name))
        self.assertFalse(feat.IsFieldNull(name))
        self.assertEqual(feat[name], '')

  # Upstream: test_ogr_mitab_read_multi_line_mid
  def test_read_multi_line_mid(self) -> None:
    mif_path = self.vsimem_dir / 'multilinemid.mif'
    _write_vsimem_text(mif_path, _MULTILINE_MIF)
    _write_vsimem_text(mif_path.with_suffix('.mid'), _MULTILINE_MID)

    with self._open_mitab(mif_path) as ds:
      lyr = ds.GetLayer(0)
      feat1 = lyr.GetNextFeature()
      self.assertEqual(feat1['Name'], 'NAME1')
      self.assertEqual(feat1['Notes'], 'MULTI\n\nLINE')
      self.assertIs(feat1['Awesome'], False)

      feat2 = lyr.GetNextFeature()
      self.assertEqual(feat2['Name'], 'NAME2')
      self.assertEqual(feat2['Notes'], 'MULTI\nLINE2')
      self.assertIs(feat2['Awesome'], False)

  # Upstream: test_ogr_mitab_read_single_field_mid
  def test_read_single_field_mid_with_empty_line(self) -> None:
    mif_path = self.vsimem_dir / 'single_field.mif'
    _write_vsimem_text(mif_path, _SINGLE_FIELD_MIF)
    _write_vsimem_text(mif_path.with_suffix('.mid'), _SINGLE_FIELD_MID)

    with self._open_mitab(mif_path) as ds:
      lyr = ds.GetLayer(0)
      self._assert_field_values(lyr, 'foo', ['1', '', '3'])

  # Upstream: test_ogr_mitab_read_write_all_data_types
  @parameterized.named_parameters(
      dict(testcase_name='mif', ext='mif'),
      dict(testcase_name='tab', ext='tab'),
  )
  def test_read_write_all_data_types(self, ext: str) -> None:
    def assert_expected_features(lyr: ogr.Layer) -> None:
      feat1 = lyr.GetNextFeature()
      self.assertIsNotNone(feat1)
      self.assertEqual(feat1['field1'], 'test')
      self.assertEqual(feat1['Field2'], 120)
      self.assertEqual(feat1['Field3'], 12345)
      field4_idx = lyr.GetLayerDefn().GetFieldIndex('Field4')
      self.assertEqual(
          lyr.GetLayerDefn().GetFieldDefn(field4_idx).GetType(),
          ogr.OFTInteger64,
      )
      self.assertEqual(feat1['Field4'], 123456789012345)
      self.assertEqual(feat1['Field5'], 12.34)
      self.assertEqual(feat1['Field6'], 12.34)
      # Google3 builds MITAB without -DMITAB_USE_OFTDATETIME, so Date/Time/
      # DateTime fields remain OFTString with raw YYYYMMDD / HHMMSSmmm /
      # YYYYMMDDHHMMSSmmm strings instead of formatted ISO strings.
      for name, expected_val in (
          ('Field7', '20221231'),
          ('Field8', '235900000'),
          ('Field9', '20220323145600000'),
      ):
        idx = lyr.GetLayerDefn().GetFieldIndex(name)
        self.assertEqual(
            lyr.GetLayerDefn().GetFieldDefn(idx).GetType(), ogr.OFTString
        )
        self.assertEqual(feat1[name], expected_val)
      self.assertIs(feat1['Field10'], True)

      feat2 = lyr.GetNextFeature()
      self.assertIsNotNone(feat2)
      self.assertIs(feat2['Field10'], False)

    if ext == 'mif':
      src_path = self.vsimem_dir / 'src_all_possible_fields.mif'
      _write_vsimem_text(src_path, _ALL_FIELDS_MIF)
      _write_vsimem_text(src_path.with_suffix('.mid'), _ALL_FIELDS_MID)
    else:
      src_path = pathlib.Path(
          ogr_util.GetTestFilePath('mitab/all_possible_fields.tab')
      )
      self.assertTrue(src_path.exists())
    out_path = self.vsimem_dir / f'all_possible_fields.{ext}'

    temporal_types = {
        'Field7': ogr.OFTDate,
        'Field8': ogr.OFTTime,
        'Field9': ogr.OFTDateTime,
    }
    with self._open_mitab(src_path) as src_ds:
      src_lyr = src_ds.GetLayer(0)
      assert_expected_features(src_lyr)

      src_lyr.ResetReading()
      with self.driver.CreateVector(str(out_path)) as out_ds:
        out_lyr = out_ds.CreateLayer('test', geom_type=ogr.wkbNone)
        src_defn = src_lyr.GetLayerDefn()
        for idx in range(src_defn.GetFieldCount()):
          fld = src_defn.GetFieldDefn(idx)
          if fld.GetName() in temporal_types:
            # Recreate native temporal schema explicitly because src_defn
            # exposes OFTString without -DMITAB_USE_OFTDATETIME.
            out_lyr.CreateField(
                ogr.FieldDefn(fld.GetName(), temporal_types[fld.GetName()])
            )
          else:
            out_lyr.CreateField(fld)
        for feat in src_lyr:
          out_feat = ogr.Feature(out_lyr.GetLayerDefn())
          out_feat.SetFrom(feat)
          self.assertEqual(out_lyr.CreateFeature(out_feat), ogr.OGRERR_NONE)

    out_header = _read_vsimem_text(out_path)
    self.assertIn('Field7 Date', out_header)
    self.assertIn('Field8 Time', out_header)
    self.assertIn('Field9 DateTime', out_header)

    with self._open_mitab(out_path) as dst_ds:
      assert_expected_features(dst_ds.GetLayer(0))

  def _assert_srs_roundtrip(
      self,
      *,
      expected_srs: osr.SpatialReference | str,
      input_srs: osr.SpatialReference | str | None = None,
      ext: str = 'tab',
  ) -> None:
    if isinstance(expected_srs, str):
      cmp_srs = osr.SpatialReference()
      cmp_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
      cmp_srs.SetFromUserInput(expected_srs)
    else:
      cmp_srs = expected_srs

    if isinstance(input_srs, str):
      write_srs = osr.SpatialReference()
      write_srs.SetFromUserInput(input_srs)
    elif input_srs is not None:
      write_srs = input_srs
    else:
      write_srs = cmp_srs

    out_path = self.vsimem_dir / f'test_srs.{ext}'
    with self.driver.CreateVector(str(out_path)) as ds:
      lyr = ds.CreateLayer('test', srs=write_srs, geom_type=ogr.wkbPoint)
      lyr.CreateField(ogr.FieldDefn('foo'))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(out_path) as ds:
      got_srs = ds.GetLayer(0).GetSpatialRef()
      self.assertIsNotNone(got_srs)
      self.assertIsSame(got_srs, cmp_srs)

    self.driver.Delete(str(out_path))

  # Upstream: test_ogr_mitab_write_etrs89_from_crs_epsg_code
  @parameterized.named_parameters(
      dict(testcase_name='tab', ext='tab'),
      dict(testcase_name='mif', ext='mif'),
  )
  def test_write_etrs89_from_epsg_code(self, ext: str) -> None:
    self._assert_srs_roundtrip(expected_srs='EPSG:25832', ext=ext)

  # Upstream: test_ogr_mitab_write_etrs89_from_crs_wkt1, wkt2
  @parameterized.named_parameters(
      dict(testcase_name='wkt1', wkt_format='FORMAT=WKT1'),
      dict(testcase_name='wkt2', wkt_format='FORMAT=WKT2'),
  )
  def test_write_etrs89_from_wkt(self, wkt_format: str) -> None:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(25832)
    srs.ImportFromWkt(srs.ExportToWkt([wkt_format]))
    self._assert_srs_roundtrip(expected_srs='EPSG:25832', input_srs=srs)

  # Upstream: test_ogr_mitab_write_etrs89_from_custom_wkt_geogcs_code
  def test_write_etrs89_from_custom_wkt_geogcs_code(self) -> None:
    srs = osr.SpatialReference()
    srs.ImportFromWkt(
        'PROJCS["ETRS89 / UTM zone 32N",GEOGCS["ETRS89",'
        'DATUM["European_Terrestrial_Reference_System_1989",'
        'SPHEROID["GRS 1980",6378137,298.257222101,AUTHORITY["EPSG","7019"]]],'
        'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AUTHORITY["EPSG","4258"]],PROJECTION["Transverse_Mercator"],'
        'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",9],'
        'PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],'
        'PARAMETER["false_northing",0],UNIT["metre",1,'
        'AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
    )
    self._assert_srs_roundtrip(expected_srs='EPSG:25832', input_srs=srs)

  # Upstream: test_ogr_mitab_write_etrs89_from_custom_wkt_no_geogcs_code
  def test_write_etrs89_from_custom_wkt_no_geogcs_code(self) -> None:
    srs = osr.SpatialReference()
    srs.ImportFromWkt(
        'PROJCS["ETRS89 / UTM zone 32N",GEOGCS["ETRS89",'
        'DATUM["European_Terrestrial_Reference_System_1989",'
        'SPHEROID["GRS 1980",6378137,298.257222101,AUTHORITY["EPSG","7019"]],'
        'AUTHORITY["EPSG","6258"]],'
        'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]]],'
        'PROJECTION["Transverse_Mercator"],'
        'PARAMETER["latitude_of_origin",0],PARAMETER["central_meridian",9],'
        'PARAMETER["scale_factor",0.9996],PARAMETER["false_easting",500000],'
        'PARAMETER["false_northing",0],UNIT["metre",1,'
        'AUTHORITY["EPSG","9001"]],AXIS["Easting",EAST],AXIS["Northing",NORTH]]'
    )
    self._assert_srs_roundtrip(expected_srs='EPSG:25832', input_srs=srs)

  # Upstream: test_ogr_mitab_write_estonia_1977 (EPSG:3301 Estonia 1997)
  def test_write_estonia_1997(self) -> None:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(3301)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    self._assert_srs_roundtrip(expected_srs=srs, input_srs='EPSG:3301')

  # Upstream: test_ogr_mitab_point_label
  def test_point_label_style_roundtrip(self) -> None:
    tab_path = self.vsimem_dir / 'point_label.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('test')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 2)'))
      feat.SetStyleString(
          'LABEL(t:"my text",a:2,s:1.5g,c:#123456,b:#234567,o:#234567,'
          'bo:1,it:1,un:1,p:2,f:"My Font")'
      )
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      feat = ds.GetLayer(0).GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(
          feat.GetStyleString(),
          'LABEL(t:"my text",a:2.000000,s:1.490400g,c:#123456,b:#234567,'
          'o:#234567,bo:1,it:1,un:1,p:2,f:"My Font")',
      )
      self.assertAlmostEqual(feat.GetGeometryRef().GetX(0), 1, delta=0.02)
      self.assertAlmostEqual(feat.GetGeometryRef().GetY(0), 2, delta=0.02)

  # Upstream: test_ogr_mitab_write_epsg_3125_philippine_reference_system_1992
  def test_write_epsg_3125_philippine_reference_system_1992(self) -> None:
    self._assert_srs_roundtrip(expected_srs='EPSG:3125')

  # Upstream: test_ogr_mitab_read_read_extended_transverse_mercator
  def test_read_extended_transverse_mercator(self) -> None:
    mif_path = self.vsimem_dir / 'proj_34.mif'
    _write_vsimem_text(mif_path, _PROJ_34_MIF)
    with self._open_mitab(mif_path) as ds:
      srs = ds.GetLayer(0).GetSpatialRef()
      self.assertIsNotNone(srs)
      self.assertEqual(
          srs.ExportToProj4(),
          '+proj=utm +zone=1 +datum=WGS84 +units=m +no_defs',
      )

  # Upstream:
  # test_ogr_mitab_read_write_hotine_oblique_mercator_with_rectified_grid_angle
  @parameterized.named_parameters(
      dict(testcase_name='mif', ext='mif'),
      dict(testcase_name='tab', ext='tab'),
  )
  def test_read_write_hotine_oblique_mercator_with_rectified_grid_angle(
      self, ext: str
  ) -> None:
    in_mif = self.vsimem_dir / 'proj_35.mif'
    _write_vsimem_text(in_mif, _PROJ_35_MIF)
    expected_proj4 = (
        '+proj=omerc +no_uoff +lat_0=4 +lonc=102.25 '
        '+alpha=323.025796466667 +gamma=323.130102361111 +k=0.99984 '
        '+x_0=804671 +y_0=123456 +ellps=evrst69 +units=m +no_defs'
    )
    with self._open_mitab(in_mif) as ds:
      ref_srs = ds.GetLayer(0).GetSpatialRef().Clone()
      self.assertEqual(ref_srs.ExportToProj4(), expected_proj4)

    out_path = self.vsimem_dir / f'hotine.{ext}'
    with self.driver.CreateVector(str(out_path)) as ds:
      lyr = ds.CreateLayer('test', srs=ref_srs, geom_type=ogr.wkbPoint)
      lyr.CreateField(ogr.FieldDefn('foo'))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(0 0)'))
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(out_path) as ds:
      got_srs = ds.GetLayer(0).GetSpatialRef()
      self.assertIsNotNone(got_srs)
      self.assertEqual(got_srs.ExportToProj4(), expected_proj4)

  # Upstream: test_ogr_mitab_label_without_text
  def test_label_without_text(self) -> None:
    tab_path = self.vsimem_dir / 'label_no_text.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('label')
      lyr.CreateField(ogr.FieldDefn('ID', ogr.OFTInteger))
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat.SetGeometryDirectly(ogr.CreateGeometryFromWkt('POINT(1 2)'))
      feat.SetStyleString(
          'LABEL(f:DejaVu Sans,s:0.705557,c:#FF0000FF,b:#000000FF)'
      )
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetFeatureCount(), 1)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      style = feat.GetStyleString()
      self.assertStartsWith(style, 'LABEL(t:"",a:0.000000,')
      self.assertIn(',c:#ff0000,b:#000000,', style)
      self.assertEndsWith(style, ',f:"DejaVu Sans")')
      self.assertAlmostEqual(feat.GetGeometryRef().GetX(0), 1, delta=0.02)
      self.assertAlmostEqual(feat.GetGeometryRef().GetY(0), 2, delta=0.02)

  # Upstream: test_ogr_mitab_write_LCC_2SP_non_metre_unit (GH #7715)
  @parameterized.named_parameters(
      dict(testcase_name='tab', ext='tab'),
      dict(testcase_name='mif', ext='mif'),
  )
  def test_write_lcc_2sp_non_metre_unit(self, ext: str) -> None:
    self._assert_srs_roundtrip(expected_srs='EPSG:2277', ext=ext)

  # Upstream: test_ogr_mitab_alter_field_defn_integer_width_4
  def test_alter_field_defn_integer_width_4(self) -> None:
    tab_path = self.vsimem_dir / 'alter_int_width4.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('foo')
      fld_defn = ogr.FieldDefn('int_field', ogr.OFTInteger)
      fld_defn.SetWidth(5)
      lyr.CreateField(fld_defn)

      idx = lyr.GetLayerDefn().GetFieldIndex('int_field')

      new_real = ogr.FieldDefn('real_field', ogr.OFTReal)
      new_real.SetWidth(15)
      new_real.SetPrecision(6)
      self.assertEqual(
          lyr.AlterFieldDefn(idx, new_real, ogr.ALTER_ALL_FLAG), ogr.OGRERR_NONE
      )
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetType(), ogr.OFTReal)
      self.assertEqual(got_fld.GetWidth(), 15)
      self.assertEqual(got_fld.GetPrecision(), 6)

      clamped_real = ogr.FieldDefn('real_field', ogr.OFTReal)
      clamped_real.SetWidth(30)
      clamped_real.SetPrecision(6)
      self.assertEqual(
          lyr.AlterFieldDefn(idx, clamped_real, ogr.ALTER_ALL_FLAG),
          ogr.OGRERR_NONE,
      )
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetType(), ogr.OFTReal)
      self.assertEqual(got_fld.GetWidth(), 20)
      self.assertEqual(got_fld.GetPrecision(), 6)

      int_width4 = ogr.FieldDefn('int_field', ogr.OFTInteger)
      int_width4.SetWidth(4)
      self.assertEqual(
          lyr.AlterFieldDefn(idx, int_width4, ogr.ALTER_ALL_FLAG),
          ogr.OGRERR_NONE,
      )
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetType(), ogr.OFTInteger)
      self.assertEqual(got_fld.GetWidth(), 4)

      feat = ogr.Feature(lyr.GetLayerDefn())
      feat['int_field'] = 1234
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

      int64_fld = ogr.FieldDefn('int_field', ogr.OFTInteger64)
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError, r'Can only convert to OFTString'
          ),
      ):
        lyr.AlterFieldDefn(idx, int64_fld, ogr.ALTER_ALL_FLAG)
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetType(), ogr.OFTInteger)
      self.assertEqual(got_fld.GetWidth(), 4)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      idx = lyr.GetLayerDefn().GetFieldIndex('int_field')
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetWidth(), 4)
      self.assertEqual(
          lyr.AlterFieldDefn(idx, got_fld, ogr.ALTER_ALL_FLAG), ogr.OGRERR_NONE
      )
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetType(), ogr.OFTInteger)
      self.assertEqual(got_fld.GetWidth(), 4)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat['int_field'], 1234)

  # Upstream: test_ogr_mitab_alter_field_defn_to_string
  def test_alter_field_defn_to_string(self) -> None:
    tab_path = self.vsimem_dir / 'alter_to_string.tab'
    with self.driver.CreateVector(str(tab_path)) as ds:
      lyr = ds.CreateLayer('foo')
      fld_defn = ogr.FieldDefn('int_field', ogr.OFTInteger)
      fld_defn.SetWidth(5)
      lyr.CreateField(fld_defn)
      feat = ogr.Feature(lyr.GetLayerDefn())
      feat['int_field'] = 1234
      self.assertEqual(lyr.CreateFeature(feat), ogr.OGRERR_NONE)

    with self._open_mitab(tab_path, update=True) as ds:
      lyr = ds.GetLayer(0)
      idx = lyr.GetLayerDefn().GetFieldIndex('int_field')
      new_fld = ogr.FieldDefn('str_field', ogr.OFTString)
      self.assertEqual(
          lyr.AlterFieldDefn(
              idx, new_fld, ogr.ALTER_NAME_FLAG | ogr.ALTER_TYPE_FLAG
          ),
          ogr.OGRERR_NONE,
      )
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetType(), ogr.OFTString)
      self.assertEqual(got_fld.GetWidth(), 254)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat['str_field'], '1234')

    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      idx = lyr.GetLayerDefn().GetFieldIndex('str_field')
      self.assertGreaterEqual(idx, 0)
      got_fld = lyr.GetLayerDefn().GetFieldDefn(idx)
      self.assertEqual(got_fld.GetType(), ogr.OFTString)
      self.assertEqual(got_fld.GetWidth(), 254)
      feat = lyr.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat['str_field'], '1234')

  # Upstream: test_ogr_mitab_read_dbf_with_delete_column
  def test_read_dbf_with_delete_column(self) -> None:
    tab_path = pathlib.Path(
        ogr_util.GetTestFilePath('mitab/tab_with_dbf_with_delete_column.tab')
    )
    self.assertTrue(tab_path.exists())
    with self._open_mitab(tab_path) as ds:
      lyr = ds.GetLayer(0)
      self.assertEqual(lyr.GetLayerDefn().GetFieldCount(), 2)
      feat = lyr.GetNextFeature()
      self.assertEqual(feat['id'], 1)
      self.assertEqual(feat['str'], 'foo')

  # Upstream: test_ogr_mitab_creation_illegal_layer_name
  def test_creation_illegal_layer_name(self) -> None:
    dir_path = self.vsimem_dir / 'illegal_dir'
    with self.driver.CreateVector(str(dir_path)) as ds:
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(RuntimeError, r'Illegal character'),
      ):
        ds.CreateLayer('illegal/with/slash')


if __name__ == '__main__':
  googletest.main()
