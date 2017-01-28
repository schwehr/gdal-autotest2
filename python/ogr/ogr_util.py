# Copyright 2014 Google Inc. All Rights Reserved.
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

"""Support for the gdal raster driver tests.

Provides tools to simplify testing a driver, which drivers are
available, and where to find test files.

Rewrite of GDALTest class:

http://trac.osgeo.org/gdal/browser/trunk/autotest/pymod/gdaltest.py#L284
"""

import datetime
from optparse import OptionParser
import os
import sys
import unittest

from osgeo import gdal
from osgeo import ogr
import six
import gflags as flags
import logging
from autotest2.gcore import gcore_util

FLAGS = flags.FLAGS


drivers = [ogr.GetDriver(driver_num).name.lower()
           for driver_num in range(ogr.GetDriverCount())]

AERONAVFAA_DRIVER = 'aeronavfaa'
ARCGEN_DRIVER = 'arcgen'
AVCBIN_DRIVER = 'avcbin'
AVCE00_DRIVER = 'avce00'
BNA_DRIVER = 'bna'
CARTODB_DRIVER = 'cartodb'
COUCHDB_DRIVER = 'couchdb'
CSV_DRIVER = 'csv'
DGN_DRIVER = 'dgn'
DXF_DRIVER = 'dxf'
EDIGEO_DRIVER = 'edigeo'
ELASTICSEARCH_DRIVER = 'elasticsearch'
GEOCONCEPT_DRIVER = 'geoconcept'
GEOJSON_DRIVER = 'geojson'
GEOMEDIA_DRIVER = 'geomedia'
GEORSS_DRIVER = 'georss'
GFT_DRIVER = 'gft'
GME_DRIVER = 'gme'
GML_DRIVER = 'gml'
GMT_DRIVER = 'gmt'
GPKG_DRIVER = 'gpkg'
GPSBABEL_DRIVER = 'gpsbabel'
GPSTRACKMAKER_DRIVER = 'gpstrackmaker'
GPX_DRIVER = 'gpx'
HTF_DRIVER = 'htf'
IDRISI_DRIVER = 'idrisi'
INTERLIS1_DRIVER = 'interlis 1'
INTERLIS2_DRIVER = 'interlis 2'
KML_DRIVER = 'kml'
LIBKML_DRIVER = 'libkml'
MAPINFO_DRIVER = 'mapinfo file'
MEMORY_DRIVER = 'memory'
MSSQLSPATIAL_DRIVER = 'mssqlspatial'
MYSQL_DRIVER = 'mysql'
NAS_DRIVER = 'nas'
ODBC_DRIVER = 'odbc'
ODS_DRIVER = 'ods'
OPENAIR_DRIVER = 'openair'
OPENFILEGDB_DRIVER = 'openfilegdb'
OSM_DRIVER = 'osm'
PCIDSK_DRIVER = 'pcidsk'
PDF_DRIVER = 'pdf'
PDS_DRIVER = 'pds'
PGDUMP_DRIVER = 'pgdump'
PGEO_DRIVER = 'pgeo'
POSTGRESQL_DRIVER = 'postgresql'
REC_DRIVER = 'rec'
S57_DRIVER = 's57'
SDTS_DRIVER = 'sdts'
SEGUKOOA_DRIVER = 'segukooa'
SEGY_DRIVER = 'segy'
SHAPEFILE_DRIVER = 'esri shapefile'
SQLITE_DRIVER = 'sqlite'
SUA_DRIVER = 'sua'
SVG_DRIVER = 'svg'
SXF_DRIVER = 'sxf'
TIGER_DRIVER = 'tiger'
UK_NTF_DRIVER = 'uk .ntf'
VFK_DRIVER = 'vfk'
VRT_DRIVER = 'vrt'
WALK_DRIVER = 'walk'
WASP_DRIVER = 'wasp'
WFS_DRIVER = 'wfs'
XLSX_DRIVER = 'xlsx'
XPLANE_DRIVER = 'xplane'


# Copied from ogr_core.h
OGRERR_NONE = 0


def Features(layer):
  while True:
    feature = layer.GetNextFeature()
    if feature is None:
      break
    yield feature


def SkipIfDriverMissing(driver_name):
  """Decorator that only runs a test if a required driver is found.

  Args:
    driver_name: Lower case name of a driver.  e.g. 'kml'.

  Returns:
    A pass through function if the test should be run or the unittest skip
    function if the test or TestCase should not be run.
  """
  def _IdReturn(obj):
    return obj

  debug = gdal.GetConfigOption('CPL_DEBUG')
  if driver_name not in drivers:
    if debug:
      logging.info('Debug: Skipping test.  Driver not found: %s', driver_name)
    return unittest.case.skip('Skipping "%s" driver dependent test.' %
                              driver_name)
  if debug:
    logging.info('Debug: Running test.  Found driver: %s', driver_name)
  return _IdReturn


def GetTestFilePath(filename):
  return os.path.join(
      os.path.split(os.path.abspath(__file__))[0],
      'testdata',
      filename
  )


class DriverTestCase(unittest.TestCase):

  def setUp(self, driver_name, ext):
    super(DriverTestCase, self).setUp()

    assert driver_name
    self.driver_name = driver_name.lower()
    self.driver = ogr.GetDriverByName(driver_name)
    assert self.driver
    self.ext = ext
    gdal.ErrorReset()  # Start with a clean slate.

  def CheckOpen(self, filepath, check_driver=True, update=False):
    if not filepath.startswith('/vsi'):
      self.assertTrue(os.path.isfile(filepath), 'Does not exist: ' + filepath)
    self.src = ogr.Open(filepath, update)
    self.assertIsNotNone(self.src)
    if check_driver:
      self.assertEqual(self.src.GetDriver().name.lower(), self.driver_name)

    return self.src

  def CheckFeaturesAgainstList(self, layer, field_name, value_list):
    # This is a mutation of check_features_against_list in
    # http://trac.osgeo.org/gdal/browser/trunk/autotest/pymod/ogrtest.py
    field_index = layer.GetLayerDefn().GetFieldIndex(field_name)
    self.assertGreaterEqual(field_index, 0,
                            'Did not find required field ' + field_name)

    def DatetimeToIntList(dt):
      # Change a datetime object into a list of integers as returned by
      # OGR_F_GetFieldAsDateTime.
      return list(dt.utctimetuple()[0:6]) + [100,]  # append TZFlag 100=GMT

    for i, v in enumerate(value_list):
      feat = layer.GetNextFeature()
      self.assertIsNotNone(
          feat,
          'Got only %d features, not the expected %d features.' %
          (i, len(value_list)))
      if isinstance(v, six.string_types):
        self.assertEqual(feat.GetFieldAsString(field_index), v)
      elif isinstance(v, datetime.datetime):
        self.assertEqual(
            feat.GetFieldAsDateTime(field_index), DatetimeToIntList(v))
      else:
        self.assertEqual(feat.GetField(field_index), v)

    feat = layer.GetNextFeature()
    self.assertIsNone(feat, 'got more features than expected')

  def CheckFeatureGeometry(self, feat, geom, max_error=0.0001):
    # This is a mutation of check_feature_geometry in
    # http://trac.osgeo.org/gdal/browser/trunk/autotest/pymod/ogrtest.py

    try:
      f_geom = feat.GetGeometryRef()
    # The original check_feature_geometry has an "expect" for all types of
    # exceptions. I'm going to list exceptions as they appear.
    except TypeError:
      # ogr_gpx_4 this raised
      # TypeError: Geometry_GetGeometryRef() takes exactly 2 arguments (1 given)
      f_geom = feat

    if isinstance(geom, six.string_types):
      geom = ogr.CreateGeometryFromWkt(geom)
    else:
      geom = geom.Clone()

    if f_geom is not None and geom is None:
      self.fail('expected NULL geometry but got one.')

    if f_geom is None and geom is not None:
      self.fail('expected geometry but got NULL.')

    self.assertEquals(
        f_geom.GetGeometryName(), geom.GetGeometryName(),
        'geometry names do not match')

    self.assertEquals(
        f_geom.GetGeometryCount(), geom.GetGeometryCount(),
        'sub-geometry counts do not match')

    self.assertEqual(
        f_geom.GetPointCount(), geom.GetPointCount(),
        'point counts do not match')

    if f_geom.GetGeometryCount() > 0:
      count = f_geom.GetGeometryCount()
      for i in range(count):
        self.CheckFeatureGeometry(f_geom.GetGeometryRef(i),
                                  geom.GetGeometryRef(i),
                                  max_error)
    else:
      count = f_geom.GetPointCount()
      for i in range(count):
        self.assertAlmostEqual(f_geom.GetX(i), geom.GetX(i), delta=max_error)
        self.assertAlmostEqual(f_geom.GetY(i), geom.GetY(i), delta=max_error)
        self.assertAlmostEqual(f_geom.GetZ(i), geom.GetZ(i), delta=max_error)

  def CheckLayer(self, layer, name, feature_count, layer_type, fields_count,
                 bbox):
    """Validates the basics of a layer.

    Args:
      layer: OGR layer instance.
      name: Expected string name of the layer.
      feature_count: Expected number of features within the layer.
      layer_type: Expected OGR feature type.  e.g. ogr.wkbPoint.
      fields_count: Expected number of columns in the feature.  Geometry does
        not count.
      bbox: Expected bounding box as (xmin, xmax, ymin, ymax).
    """
    self.assertEqual(layer.GetName(), name)
    self.assertEqual(layer.GetFeatureCount(), feature_count)
    layer_defn = layer.GetLayerDefn()
    self.assertIsNotNone(layer_defn)
    self.assertEqual(layer_defn.GetGeomType(), layer_type)
    self.assertEqual(layer_defn.GetFieldCount(), fields_count)
    extent = layer.GetExtent()
    for i in range(4):
      self.assertAlmostEqual(extent[i], bbox[i])


def CreateParser():
  parser = OptionParser()
  parser.add_option('-t', '--temp-dir', default=os.getcwd(),
                    help='Where to put temporary files.',
                    metavar='DIR')
  parser.add_option('-p', '--pam-dir', default=None,
                    help='Where to store the .aux.xml files created '
                    'by the persistent auxiliary metadata system.  '
                    'Defaults to temp-directory/pam.',
                    metavar='DIR')
  parser.add_option('-v', '--verbose', default=False, action='store_true',
                    help='Put the unittest run into verbose mode.')
  return parser


def Setup(options):
  if options.verbose:
    logging.basicConfig(level=logging.INFO)

  options.temp_dir = os.path.abspath(options.temp_dir)
  gdal.SetConfigOption('CPL_TMPDIR', options.temp_dir)
  logging.info('CPL_TMPDIR: %s', options.temp_dir)

  options.pam_dir = options.pam_dir or os.path.join(options.temp_dir, 'pam')
  if not os.path.isdir(options.pam_dir):
    os.mkdir(options.pam_dir)
  gdal.SetConfigOption('GDAL_PAM_PROXY_DIR', options.pam_dir)
  logging.info('GDAL_PAM_PROXY_DIR: %s', options.pam_dir)


def SetupTestEnv():
  gcore_util.SetupTestEnv()
