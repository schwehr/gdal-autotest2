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
# Copyright (c) 2007-2010, Even Rouault <even dot rouault at mines-paris dot org>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

"""Test OGR handling of GPX files.

This is a mutation of the ogr_gpx.py test from:
  http://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_gpx.py.
"""

import datetime
import os


from osgeo import ogr
import gflags as flags
import unittest
from autotest2.ogr import ogr_util

FLAGS = flags.FLAGS

EXT = '.gpx'


def setUpModule():
  ogr_util.SetupTestEnv()


@ogr_util.SkipIfDriverMissing(ogr_util.GPX_DRIVER)
class OgrGpxTest(ogr_util.DriverTestCase):

  def setUp(self):
    super(OgrGpxTest, self).setUp(ogr_util.GPX_DRIVER, EXT)

  def checkWaypoints(self, gpx_ds):
    """Test waypoints gpx layer, based on ogr_gpx_1."""
    lyr = gpx_ds.GetLayerByName('waypoints')
    expect = [2, None]
    self.CheckFeaturesAgainstList(lyr, 'ele', expect)
    lyr.ResetReading()
    expect = ['waypoint name', None]
    self.CheckFeaturesAgainstList(lyr, 'name', expect)
    lyr.ResetReading()
    expect = ['href', None]
    self.CheckFeaturesAgainstList(lyr, 'link1_href', expect)
    lyr.ResetReading()
    expect = ['text', None]
    self.CheckFeaturesAgainstList(lyr, 'link1_text', expect)
    lyr.ResetReading()
    expect = ['type', None]
    self.CheckFeaturesAgainstList(lyr, 'link1_type', expect)
    lyr.ResetReading()
    expect = ['href2', None]
    self.CheckFeaturesAgainstList(lyr, 'link2_href', expect)
    lyr.ResetReading()
    expect = ['text2', None]
    self.CheckFeaturesAgainstList(lyr, 'link2_text', expect)
    lyr.ResetReading()
    expect = ['type2', None]
    self.CheckFeaturesAgainstList(lyr, 'link2_type', expect)
    lyr.ResetReading()
    expect = ['2007/11/25 17:58:00+01', None]
    self.CheckFeaturesAgainstList(lyr, 'time', expect)
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(feat, 'POINT (1 0)', max_error=0.0001)
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(feat, 'POINT (4 3)', max_error=0.0001)

  def checkRoutes(self, gpx_ds):
    """Test routes gpx layer, based on ogr_gpx_2."""
    lyr = gpx_ds.GetLayerByName('routes')
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(
        feat, 'LINESTRING (6 5,9 8,12 11)', max_error=0.0001)
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(feat, 'LINESTRING EMPTY', max_error=0.0001)

  def checkRoutePoints(self, gpx_ds):
    """Test route_points gpx layer, based on ogr_gpx_3."""
    lyr = gpx_ds.GetLayerByName('route_points')
    expect = ['route point name', None, None]
    self.CheckFeaturesAgainstList(lyr, 'name', expect)
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(feat, 'POINT (6 5)', max_error=0.0001)

  def checkTracks(self, gpx_ds):
    """Test tracks gpx layer, based on ogr_gpx_4."""
    lyr = gpx_ds.GetLayerByName('tracks')
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(
        feat, 'MULTILINESTRING ((15 14,18 17),(21 20,24 23))',
        max_error=0.0001)
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(feat, 'MULTILINESTRING EMPTY', max_error=0.0001)
    feat = lyr.GetNextFeature()
    f_geom = feat.GetGeometryRef()
    self.assertEqual(f_geom.ExportToWkt(), 'MULTILINESTRING EMPTY')

  def checkTrackPoints(self, gpx_ds):
    """Test track_points gpx layer, based on ogr_gpx_5."""
    lyr = gpx_ds.GetLayerByName('track_points')
    expect = ['track point name', None, None, None]
    self.CheckFeaturesAgainstList(lyr, 'name', expect)
    lyr.ResetReading()
    expect = [None, None, datetime.datetime(2014, 10, 12, 7, 12, 55),
              datetime.datetime(2014, 10, 12, 7, 12, 52)]
    self.CheckFeaturesAgainstList(lyr, 'time', expect)
    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    self.CheckFeatureGeometry(feat, 'POINT (15 14)', max_error=0.0001)

  def testRead(self):
    """Run checks based on ogr_gpx_1...5 with the test.gpx data file."""
    gpx_ds = self.CheckOpen(ogr_util.GetTestFilePath('test.gpx'))
    self.assertEqual(gpx_ds.GetLayerCount(), 5)

    self.checkWaypoints(gpx_ds)
    self.checkRoutes(gpx_ds)
    self.checkRoutePoints(gpx_ds)
    self.checkTracks(gpx_ds)
    self.checkTrackPoints(gpx_ds)

  def testCopyAndRead(self):
    """Copy test.gpx to a new gpx file and run checks based on ogr_gpx_1,2,4."""
    gpx_ds = self.CheckOpen(ogr_util.GetTestFilePath('test.gpx'))
    self.assertEqual(gpx_ds.GetLayerCount(), 5)

    # Duplicate waypoints
    gpx_lyr = gpx_ds.GetLayerByName('waypoints')

    test_tmpfile_path = os.path.join(FLAGS.test_tmpdir, 'gpx.gpx')
    gpx2_ds = ogr.GetDriverByName(
        ogr_util.GPX_DRIVER).CreateDataSource(test_tmpfile_path)
    gpx2_lyr = gpx2_ds.CreateLayer('waypoints', geom_type=ogr.wkbPoint)

    gpx_lyr.ResetReading()
    dst_feat = ogr.Feature(feature_def=gpx2_lyr.GetLayerDefn())
    feat = gpx_lyr.GetNextFeature()
    while feat is not None:
      dst_feat.SetFrom(feat)
      self.assertEquals(gpx2_lyr.CreateFeature(dst_feat), ogr_util.OGRERR_NONE)
      feat = gpx_lyr.GetNextFeature()

    # Duplicate routes
    gpx_lyr = gpx_ds.GetLayerByName('routes')
    gpx2_lyr = gpx2_ds.CreateLayer('routes', geom_type=ogr.wkbLineString)
    gpx_lyr.ResetReading()

    dst_feat = ogr.Feature(feature_def=gpx2_lyr.GetLayerDefn())

    feat = gpx_lyr.GetNextFeature()
    while feat is not None:
      dst_feat.SetFrom(feat)
      self.assertEquals(gpx2_lyr.CreateFeature(dst_feat), ogr_util.OGRERR_NONE)
      feat = gpx_lyr.GetNextFeature()

    # Duplicate tracks
    gpx_lyr = gpx_ds.GetLayerByName('tracks')

    gpx2_lyr = gpx2_ds.CreateLayer('tracks', geom_type=ogr.wkbMultiLineString)

    gpx_lyr.ResetReading()

    dst_feat = ogr.Feature(feature_def=gpx2_lyr.GetLayerDefn())

    feat = gpx_lyr.GetNextFeature()
    while feat is not None:
      dst_feat.SetFrom(feat)
      self.assertEquals(gpx2_lyr.CreateFeature(dst_feat), ogr_util.OGRERR_NONE)
      feat = gpx_lyr.GetNextFeature()

    # Explicitly delete the DataSource so it is flushed to the filesystem.
    gpx2_ds.Destroy()
    gpx2_ds = None

    gpx_ds = ogr.Open(test_tmpfile_path)
    self.checkWaypoints(gpx_ds)
    self.checkRoutes(gpx_ds)
    self.checkTracks(gpx_ds)

  @ogr_util.SkipIfDriverMissing(ogr_util.BNA_DRIVER)
  def testCopyWaypointExtraFields(self):
    """Output extra fields as <extensions>, based on ogr_gpx_7."""
    bna_ds = ogr.Open(ogr_util.GetTestFilePath('bna_for_gpx.bna'))

    co_opts = ['GPX_USE_EXTENSIONS=yes']

    # Duplicate waypoints
    bna_lyr = bna_ds.GetLayerByName('bna_for_gpx_points')

    test_tmpfile_path = os.path.join(FLAGS.test_tmpdir, 'ogr_gpx_7.gpx')
    gpx_ds = ogr.GetDriverByName(
        ogr_util.GPX_DRIVER).CreateDataSource(test_tmpfile_path,
                                              options=co_opts)

    gpx_lyr = gpx_ds.CreateLayer('waypoints', geom_type=ogr.wkbPoint)

    bna_lyr.ResetReading()

    for i in range(bna_lyr.GetLayerDefn().GetFieldCount()):
      field_defn = bna_lyr.GetLayerDefn().GetFieldDefn(i)
      gpx_lyr.CreateField(field_defn)

    dst_feat = ogr.Feature(feature_def=gpx_lyr.GetLayerDefn())

    feat = bna_lyr.GetNextFeature()
    while feat is not None:
      dst_feat.SetFrom(feat)
      self.assertEquals(gpx_lyr.CreateFeature(dst_feat), ogr_util.OGRERR_NONE)
      feat = bna_lyr.GetNextFeature()

    # Explicitly delete the DataSource so it is flushed to the filesystem.
    gpx_ds.Destroy()
    gpx_ds = None

    # Now check that the extensions fields have been well written
    gpx_ds = ogr.Open(test_tmpfile_path)
    gpx_lyr = gpx_ds.GetLayerByName('waypoints')

    expect = ['PID1', 'PID2']
    self.CheckFeaturesAgainstList(gpx_lyr, 'ogr_Primary_ID', expect)

    gpx_lyr.ResetReading()

    expect = ['SID1', 'SID2']

    self.CheckFeaturesAgainstList(gpx_lyr, 'ogr_Secondary_ID', expect)

    gpx_lyr.ResetReading()

    expect = ['TID1', None]

    self.CheckFeaturesAgainstList(gpx_lyr, 'ogr_Third_ID', expect)

  def testWriteRouteTrackExtraFields(self):
    """Output extra fields as <extensions>, based on ogr_gpx_8."""
    test_tmpfile_path = os.path.join(FLAGS.test_tmpdir, 'ogr_gpx_8.gpx')

    gpx_ds = ogr.GetDriverByName(ogr_util.GPX_DRIVER).CreateDataSource(
        test_tmpfile_path, options=['LINEFORMAT=LF'])

    lyr = gpx_ds.CreateLayer('route_points', geom_type=ogr.wkbPoint)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(2 49)')
    feat.SetField('route_name', 'ROUTE_NAME')
    feat.SetField('route_fid', 0)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(3 50)')
    feat.SetField('route_name', '--ignored--')
    feat.SetField('route_fid', 0)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(3 51)')
    feat.SetField('route_name', 'ROUTE_NAME2')
    feat.SetField('route_fid', 1)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(3 49)')
    feat.SetField('route_fid', 1)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    lyr = gpx_ds.CreateLayer('track_points', geom_type=ogr.wkbPoint)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(2 49)')
    feat.SetField('track_name', 'TRACK_NAME')
    feat.SetField('track_fid', 0)
    feat.SetField('track_seg_id', 0)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(3 50)')
    feat.SetField('track_name', '--ignored--')
    feat.SetField('track_fid', 0)
    feat.SetField('track_seg_id', 0)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(3 51)')
    feat.SetField('track_fid', 0)
    feat.SetField('track_seg_id', 1)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    feat = ogr.Feature(lyr.GetLayerDefn())
    geom = ogr.CreateGeometryFromWkt('POINT(3 49)')
    feat.SetField('track_name', 'TRACK_NAME2')
    feat.SetField('track_fid', 1)
    feat.SetField('track_seg_id', 0)
    feat.SetGeometry(geom)
    lyr.CreateFeature(feat)

    # Explicitly delete the DataSource so it is flushed to the filesystem.
    gpx_ds.Destroy()
    gpx_ds = None

    # Check that the test GPX file created above contains the contents of the
    # golden reference data file ogr_gpx_8_ref.txt.
    test_content = open(test_tmpfile_path).read()
    ref_content = open(ogr_util.GetTestFilePath('ogr_gpx_8_ref.txt')).read()
    self.assertIn(ref_content, test_content)


if __name__ == '__main__':
  unittest.main()
