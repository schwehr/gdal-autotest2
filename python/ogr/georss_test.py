# MOE:insert #!/usr/bin/env python
# Copyright 2018 Google Inc. All Rights Reserved.
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
# Copyright (c) 2008-2013, Even Rouault <even dot rouault at mines-paris dot org>
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
"""Test OGR handling of GeoRSS files.

This is a rewrite of:

  https://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_georss.py
"""

import json
import os
import sys
import unittest

import google3
from osgeo import ogr
from osgeo import osr
from osgeo import gdal

from autotest2.gcore import gcore_util
from autotest2.ogr import ogr_util

DRIVER = ogr_util.GEORSS_DRIVER
EXT = '.xml'

DEFAULT_LAYER_NAME = 'OGRGeoRSS'

# Values used in some of the atom tests.
ATOM_FIELD_VALUES = [
    ('title', 'Atom draft-07 snapshot',
     ogr.OFTString), ('link_rel', 'alternate',
                      ogr.OFTString), ('link_type', 'text/html', ogr.OFTString),
    ('link_href', 'http://example.org/2005/04/02/atom',
     ogr.OFTString), ('link2_rel', 'enclosure',
                      ogr.OFTString), ('link2_type', 'audio/mpeg',
                                       ogr.OFTString), ('link2_length', '1337',
                                                        ogr.OFTInteger),
    ('link2_href', 'http://example.org/audio/ph34r_my_podcast.mp3',
     ogr.OFTString), ('id', 'tag:example.org,2003:3.2397',
                      ogr.OFTString), ('updated', '2005/07/31 12:29:29+00',
                                       ogr.OFTDateTime),
    ('published', '2003/12/13 08:29:29-04',
     ogr.OFTDateTime), ('author_name', 'Mark Pilgrim',
                        ogr.OFTString), ('author_uri', 'http://example.org/',
                                         ogr.OFTString),
    ('author_email', 'f8dy@example.com',
     ogr.OFTString), ('contributor_name', 'Sam Ruby',
                      ogr.OFTString), ('contributor2_name', 'Joe Gregorio',
                                       ogr.OFTString), ('content_type', 'xhtml',
                                                        ogr.OFTString),
    ('content_xml_lang', 'en',
     ogr.OFTString), ('content_xml_base', 'http://diveintomark.org/',
                      ogr.OFTString)
]


def setUpModule():
  ogr_util.SetupTestEnv()


def CreateField(layer, name, field_type=ogr.OFTString):
  field_definition = ogr.FieldDefn(name, field_type)
  layer.CreateField(field_definition)
  field_definition.Destroy()


@ogr_util.SkipIfDriverMissing(DRIVER)
class OgrGeoRSSTest(ogr_util.DriverTestCase):

  def setUp(self):
    super(OgrGeoRSSTest, self).setUp(DRIVER, EXT)

  # Helper for GeoRSS tests. Used by GeoRss1x.
  def ogrGeoRssTestAtom(self, ogr_filepath):
    ds = self.CheckOpen(ogr_filepath)
    lyr = ds.GetLayerByIndex(0)

    self.assertIsNone(lyr.GetSpatialRef())

    feat = lyr.GetNextFeature()

    for field_value in ATOM_FIELD_VALUES:
      self.assertEquals(feat.GetFieldAsString(field_value[0]), field_value[1])

    self.assertIn('<div xmlns="http://www.w3.org/1999/xhtml">',
                  feat.GetFieldAsString('content'))

  # Helper for GeoRSS tests. Used by GeoRss2~9.
  def ogrGeoRssTest(self, ogr_filepath, only_first_feature):
    ds = self.CheckOpen(ogr_filepath)
    lyr = ds.GetLayerByIndex(0)

    srs = osr.SpatialReference()
    srs.SetWellKnownGeogCS('WGS84')

    self.assertIsNotNone(lyr.GetSpatialRef())
    self.assertTrue(lyr.GetSpatialRef().IsSame(srs))

    self.assertNotIn('AXIS["Latitude",NORTH],AXIS["Longitude",EAST]',
                     lyr.GetSpatialRef().ExportToWkt())

    feat = lyr.GetNextFeature()
    expected_wkt = 'POINT (2 49)'
    self.assertEquals(feat.GetGeometryRef().ExportToWkt(), expected_wkt)
    self.assertEquals(feat.GetFieldAsString('title'), 'A point')
    self.assertEquals(feat.GetFieldAsString('author'), 'Author')
    self.assertEquals(feat.GetFieldAsString('link'), 'http://gdal.org')
    self.assertEquals(
        feat.GetFieldAsString('pubDate'), '2008/12/07 20:13:00+02')
    self.assertEquals(feat.GetFieldAsString('category'), 'First category')
    self.assertEquals(feat.GetFieldAsString('category_domain'), 'first_domain')
    self.assertEquals(feat.GetFieldAsString('category2'), 'Second category')
    self.assertEquals(
        feat.GetFieldAsString('category2_domain'), 'second_domain')

    feat = lyr.GetNextFeature()
    expected_wkt = 'LINESTRING (2 48,2.1 48.1,2.2 48.0)'
    if only_first_feature is False:
      self.assertEquals(feat.GetGeometryRef().ExportToWkt(), expected_wkt)
    self.assertEquals(feat.GetFieldAsString('title'), 'A line')

    feat = lyr.GetNextFeature()
    expected_wkt = 'POLYGON ((2 50,2.1 50.1,2.2 48.1,2.1 46.1,2 50))'
    if only_first_feature is False:
      self.assertEquals(feat.GetGeometryRef().ExportToWkt(), expected_wkt)
    self.assertEquals(feat.GetFieldAsString('title'), 'A polygon')

    feat = lyr.GetNextFeature()
    expected_wkt = 'POLYGON ((2 49,2.0 49.5,2.2 49.5,2.2 49.0,2 49))'
    if only_first_feature is False:
      self.assertEquals(feat.GetGeometryRef().ExportToWkt(), expected_wkt)
    self.assertEquals(feat.GetFieldAsString('title'), 'A box')

  # Creates a RSS 2.0 document
  def ogrGeoRssCreate(self, ogr_filepath, options):
    ds = self.driver.CreateDataSource(ogr_filepath, options=options)
    lyr = ds.CreateLayer('georss')

    lyr.CreateField(ogr.FieldDefn('title', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('author', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('link', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('pubDate', ogr.OFTDateTime))
    lyr.CreateField(ogr.FieldDefn('description', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('category', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('category_domain', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('category2', ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn('category2_domain', ogr.OFTString))

    dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
    dst_feat.SetField('title', 'A point')
    dst_feat.SetField('author', 'Author')
    dst_feat.SetField('link', 'http://gdal.org')
    dst_feat.SetField('pubDate', '2008/12/07 20:13:00+02')
    dst_feat.SetField('category', 'First category')
    dst_feat.SetField('category_domain', 'first_domain')
    dst_feat.SetField('category2', 'Second category')
    dst_feat.SetField('category2_domain', 'second_domain')
    dst_feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (2 49)'))

    self.assertEqual(lyr.CreateFeature(dst_feat), 0)

    dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
    dst_feat.SetField('title', 'A line')
    dst_feat.SetField('author', 'Author')
    dst_feat.SetField('link', 'http://gdal.org')
    dst_feat.SetField('pubDate', '2008/12/07 20:13:00+02')
    dst_feat.SetGeometry(
        ogr.CreateGeometryFromWkt('LINESTRING (2 48,2.1 48.1,2.2 48.0)'))

    self.assertEqual(lyr.CreateFeature(dst_feat), 0)

    dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
    dst_feat.SetField('title', 'A polygon')
    dst_feat.SetField('author', 'Author')
    dst_feat.SetField('link', 'http://gdal.org')
    dst_feat.SetField('pubDate', '2008/12/07 20:13:00+02')
    dst_feat.SetGeometry(
        ogr.CreateGeometryFromWkt(
            'POLYGON ((2 50,2.1 50.1,2.2 48.1,2.1 46.1,2 50))'))

    self.assertEqual(lyr.CreateFeature(dst_feat), 0)

    dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
    dst_feat.SetField('title', 'A box')
    dst_feat.SetField('author', 'Author')
    dst_feat.SetField('link', 'http://gdal.org')
    dst_feat.SetField('pubDate', '2008/12/07 20:13:00+02')
    dst_feat.SetGeometry(
        ogr.CreateGeometryFromWkt(
            'POLYGON ((2 49,2.0 49.5,2.2 49.5,2.2 49.0,2 49))'))

    self.assertEqual(lyr.CreateFeature(dst_feat), 0)

    ds = None

  def testOgrGeorss1(self):
    filepath = ogr_util.GetTestFilePath('georss/atom_rfc_sample.xml')
    self.ogrGeoRssTestAtom(filepath)

  def testOgrGeorss1AtomNs(self):
    filepath = ogr_util.GetTestFilePath('georss/atom_rfc_sample_atom_ns.xml')
    self.ogrGeoRssTestAtom(filepath)

  def testOgrGeorss1bis(self):
    filepath = ogr_util.GetTestFilePath('/vsimem/test_atom.xml')
    ds = self.driver.CreateDataSource(filepath, options=['FORMAT=ATOM'])
    lyr = ds.CreateLayer('georss')

    for field_value in ATOM_FIELD_VALUES:
      lyr.CreateField(ogr.FieldDefn(field_value[0], field_value[2]))
    lyr.CreateField(ogr.FieldDefn('content', ogr.OFTString))

    dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
    for field_value in ATOM_FIELD_VALUES:
      dst_feat.SetField(field_value[0], field_value[1])
    dst_feat.SetField(
        'content', '<div xmlns="http://www.w3.org/1999/xhtml">'
        '<p><i>[Update: The Atom draft is finished.]</i></p></div>')

    self.assertEqual(lyr.CreateFeature(dst_feat), 0)

  def testOgrGeorss1ter(self):
    filepath = ogr_util.GetTestFilePath('/vsimem/test_atom.xml')
    self.ogrGeoRssTestAtom(filepath)

  # Test reading a RSS 2.0 document with GeoRSS simple geometries
  def testOgrGeorss2(self):
    filepath = ogr_util.GetTestFilePath('georss/test_georss_simple.xml')
    self.ogrGeoRssTest(filepath, False)

  # Test reading a RSS 2.0 document with GeoRSS GML geometries
  def testOgrGeorss3(self):
    filepath = ogr_util.GetTestFilePath('georss/test_georss_gml.xml')
    self.ogrGeoRssTest(filepath, False)

  # Test writing a RSS 2.0 document in Simple dialect
  # (doesn't need read support)
  def testOgrGeorss4and5(self):
    filepath = ogr_util.GetTestFilePath('/vsimem/ogr_georss_4.xml')
    with gcore_util.GdalUnlinkWhenDone(filepath):
      self.ogrGeoRssCreate(filepath, [])
      src = self.CheckOpen(filepath)
      lyr = src.GetLayerByName('georss')
      self.assertIsNotNone(lyr)

      # Portion that was in 5.
      self.ogrGeoRssTest(filepath, False)

  # Test writing a RSS 2.0 document in GML dialect
  # (doesn't need read support)
  def testOgrGeorss6and7(self):
    filepath = ogr_util.GetTestFilePath('/vsimem/ogr_georss_6.xml')
    with gcore_util.GdalUnlinkWhenDone(filepath):
      self.ogrGeoRssCreate(filepath, ['GEOM_DIALECT=GML'])
      src = self.CheckOpen(filepath)
      lyr = src.GetLayerByName('georss')
      self.assertIsNotNone(lyr)

      # Portion that was in 7.
      self.ogrGeoRssTest(filepath, False)

  # Test writing a RSS 2.0 document in W3C Geo dialect
  # (doesn't need read support)
  def testOgrGeorss8and9(self):
    filepath = ogr_util.GetTestFilePath('/vsimem/ogr_georss_8.xml')
    with gcore_util.GdalUnlinkWhenDone(filepath):
      self.ogrGeoRssCreate(filepath, ['GEOM_DIALECT=W3C_GEO'])
      src = self.CheckOpen(filepath)
      lyr = src.GetLayerByName('georss')
      self.assertIsNotNone(lyr)

      # Portion that was in 9.
      self.ogrGeoRssTest(filepath, True)

  # Test writing a RSS 2.0 document in GML dialect with EPSG:32631
  def testOgrGeorss10and11(self):
    filepath = ogr_util.GetTestFilePath('/vsimem/test32631.rss')

    with gcore_util.GdalUnlinkWhenDone(filepath):
      srs = osr.SpatialReference()
      srs.ImportFromEPSG(32631)

      ds = self.driver.CreateDataSource(filepath)
      with gcore_util.GdalUnlinkWhenDone(filepath):
        with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
          lyr = ds.CreateLayer('georss', srs=srs)
          self.assertIsNone(lyr)

      ds = self.driver.CreateDataSource(filepath, options=['GEOM_DIALECT=GML'])
      lyr = ds.CreateLayer('georss', srs=srs)

      dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
      dst_feat.SetGeometry(ogr.CreateGeometryFromWkt('POINT (500000 4000000)'))

      self.assertEqual(lyr.CreateFeature(dst_feat), 0)

      # Close the files and force a flush to the filesystem.
      lyr = None
      ds = None
      src = self.CheckOpen(filepath)
      lyr = src.GetLayerByName('georss')
      self.assertIsNotNone(lyr)

      # Portion that was in 11.
      ds = self.CheckOpen(filepath)
      lyr = ds.GetLayer(0)

      srs = osr.SpatialReference()
      srs.ImportFromEPSG(32631)

      self.assertIsNotNone(lyr.GetSpatialRef())
      self.assertTrue(lyr.GetSpatialRef().IsSame(srs))

      self.assertIn('AXIS["Latitude",NORTH],AXIS["Longitude",EAST]',
                    lyr.GetSpatialRef().ExportToWkt())

      feat = lyr.GetNextFeature()
      expected_wkt = 'POINT (500000 4000000)'
      self.assertEqual(feat.GetGeometryRef().ExportToWkt(), expected_wkt)

  # TODO(b/71817518): ogr_georss_12

  def testOgrGeorss13and14(self):
    filepath = ogr_util.GetTestFilePath('/vsimem/test32631.rss')

    with gcore_util.GdalUnlinkWhenDone(filepath):
      ds = self.driver.CreateDataSource(
          filepath, options=['USE_EXTENSIONS=YES'])
      lyr = ds.CreateLayer('georss')

      lyr.CreateField(ogr.FieldDefn('myns_field', ogr.OFTString))
      lyr.CreateField(ogr.FieldDefn('field2', ogr.OFTString))
      lyr.CreateField(ogr.FieldDefn('ogr_field3', ogr.OFTString))

      dst_feat = ogr.Feature(feature_def=lyr.GetLayerDefn())
      dst_feat.SetField('myns_field', 'val')
      dst_feat.SetField('field2', 'val2')
      dst_feat.SetField('ogr_field3', 'val3')

      self.assertEqual(lyr.CreateFeature(dst_feat), 0)

      ds = None

      src = self.CheckOpen(filepath)
      lyr = src.GetLayerByName('georss')
      self.assertIsNotNone(lyr)

      # Portion that was in 14.
      ds = self.CheckOpen(filepath)
      lyr = ds.GetLayer(0)
      feat = lyr.GetNextFeature()

      self.assertEquals(feat.GetFieldAsString('myns_field'), 'val')
      self.assertEquals(feat.GetFieldAsString('ogr_field2'), 'val2')
      self.assertEquals(feat.GetFieldAsString('ogr_field3'), 'val3')

  # ogr_georss_15 redundant as all temp files were tested with in memory file.


if __name__ == '__main__':
  unittest.main()
