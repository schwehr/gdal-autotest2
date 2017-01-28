#!/usr/bin/env python
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
#
# This is a complete rewrite of a file licensed as follows:
#
# Copyright (c) 2007, Matuesz Loskot <mateusz@loskot.net>
# Copyright (c) 2008-2014, Even Rouault <even dot rouault at mines-paris . org>
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

"""Test OGR handling of KML files using the KML driver.

This does not test libkml.

This is a rewrite of:

  http://trac.osgeo.org/gdal/browser/trunk/autotest/ogr/ogr_kml.py.
"""

import os


from osgeo import gdal
from osgeo import ogr
from osgeo import osr
import logging
import unittest
from autotest2.gcore import gcore_util
from autotest2.ogr import ogr_util


DRIVER = ogr_util.KML_DRIVER
EXT = '.kml'


def setUpModule():
  ogr_util.SetupTestEnv()


@ogr_util.SkipIfDriverMissing(DRIVER)
class OgrKmlTest(ogr_util.DriverTestCase):

  def setUp(self):
    super(OgrKmlTest, self).setUp(DRIVER, EXT)

  def testSimplePlacemark(self):
    filepath = ogr_util.GetTestFilePath('point.kml')
    src = ogr.Open(filepath)
    self.assertEqual(src.GetLayerCount(), 1)
    layer = src.GetLayer()
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('Name'), 'Clipperton Island')

  def testAttributes(self):
    filepath = ogr_util.GetTestFilePath('samples.kml')
    self.CheckOpen(filepath)

    layer = self.src.GetLayerByName('Placemarks')
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('Name'), 'Simple placemark')
    self.assertEqual(feature.GetField('description')[:23],
                     'Attached to the ground.')
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('Name'), 'Floating placemark')

    self.assertEqual(feature.GetField('description')[:25],
                     'Floats a defined distance')

    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('Name'), 'Extruded placemark')
    self.assertEqual(feature.GetField('description'),
                     'Tethered to the ground by a customizable \"tail\"')

    # 2
    layer = self.src.GetLayerByName('Highlighted Icon')
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('Name'), 'Roll over this icon')
    self.assertEqual(feature.GetField('description'), '')
    self.assertIsNone(layer.GetNextFeature())

    # 3
    layer = self.src.GetLayerByName('Paths')
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('Name'), 'Tessellated')
    self.assertEqual(feature.GetField('description'),
                     'If the <tessellate> tag has a value of 1, the line '
                     'will contour to the underlying terrain')
    feature = layer.GetNextFeature()
    self.assertEqual(feature.GetField('Name'), 'Untessellated')
    self.assertEqual(feature.GetField('description'),
                     'If the <tessellate> tag has a value of 0, the line '
                     'follow a simple straight-line path from point to '
                     'point')

    self.assertIsNotNone(layer.GetNextFeature())

    # 4
    layer = self.src.GetLayerByName('Google Campus')

    for i in range(40, 44):
      feature = layer.GetNextFeature()
      self.assertEqual(feature.GetField('Name'), 'Building %d' % i)
      self.assertEqual(feature.GetField('description'), '')

    # ogr_kml_point_read
    layer = self.src.GetLayerByName('Placemarks')
    layer.ResetReading()
    self.CheckFeatureGeometry(
        layer.GetNextFeature(), 'POINT(-122.0822035425683 37.42228990140251)')

    self.CheckFeatureGeometry(
        layer.GetNextFeature(), 'POINT(-122.084075 37.4220033612141 50)')

    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'POINT(-122.0857667006183 37.42156927867553 50)')

    # ogr_kml_linestring_read
    layer = self.src.GetLayerByName('Paths')
    layer.ResetReading()
    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'LINESTRING (-112.081423783034495 36.106778704771372 0, '
        '-112.087026775269294 36.0905099328766 0)')
    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'LINESTRING (-112.080622229594994 36.106734600079953 0, '
        '-112.085242575314993 36.090495986124218 0)')
    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'LINESTRING (-112.265654928602004 36.094476726025462 2357,'
        '-112.266038452823807 36.093426088386707 2357,'
        '-112.266813901345301 36.092510587768807 2357,'
        '-112.267782683444494 36.091898273579957 2357,'
        '-112.268855751095202 36.091313794118697 2357,'
        '-112.269481071721899 36.090367720752099 2357,'
        '-112.269526855561097 36.089321714872852 2357,'
        '-112.269014456727604 36.088509160604723 2357,'
        '-112.268152881533894 36.087538135979557 2357,'
        '-112.2670588176031 36.086826852625677 2357,'
        '-112.265737458732104 36.086463123013033 2357)')

    # ogr_kml_polygon_read
    layer = self.src.GetLayerByName('Google Campus')
    layer.ResetReading()
    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'POLYGON ((-122.084893845961204 37.422571240447859 17,'
        '-122.084958097919795 37.422119226268563 17,'
        '-122.084746957304702 37.42207183952619 17,'
        '-122.084572538096197 37.422090067296757 17,'
        '-122.084595488672306 37.422159327008949 17,'
        '-122.0838521118269 37.422272785643713 17,'
        '-122.083792243334997 37.422035391120843 17,'
        '-122.0835076656616 37.422090069571063 17,'
        '-122.083470946415204 37.422009873951609 17,'
        '-122.083122108574798 37.422104649494599 17,'
        '-122.082924737457205 37.422265039903863 17,'
        '-122.082933916938501 37.422312428430942 17,'
        '-122.083383735973698 37.422250460876178 17,'
        '-122.083360785424802 37.422341592287452 17,'
        '-122.083420455164202 37.42237075460644 17,'
        '-122.083659133885007 37.422512920110009 17,'
        '-122.083975843895203 37.422658730937812 17,'
        '-122.084237474333094 37.422651439725207 17,'
        '-122.0845036949503 37.422651438643499 17,'
        '-122.0848020460801 37.422611339163147 17,'
        '-122.084788275051494 37.422563950551208 17,'
        '-122.084893845961204 37.422571240447859 17))')
    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'POLYGON ((-122.085741277148301 37.422270331552568 17,'
        '-122.085816976848093 37.422314088323461 17,'
        '-122.085852582875006 37.422303374697442 17,'
        '-122.085879994563896 37.422256861387893 17,'
        '-122.085886010140896 37.422231107613797 17,'
        '-122.085806915728796 37.422202501738553 17,'
        '-122.085837954265301 37.42214027058678 17,'
        '-122.085673264051906 37.422086902144081 17,'
        '-122.085602292640701 37.42214885429042 17,'
        '-122.085590277843593 37.422128290487002 17,'
        '-122.085584167223701 37.422081719672462 17,'
        '-122.085485206574106 37.42210455874995 17,'
        '-122.085506726435199 37.422142679498243 17,'
        '-122.085443071291493 37.422127838461719 17,'
        '-122.085099071490404 37.42251282407603 17,'
        '-122.085676981863202 37.422818153236513 17,'
        '-122.086016227378295 37.422449188587223 17,'
        '-122.085726032700407 37.422292396042529 17,'
        '-122.085741277148301 37.422270331552568 17))')
    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'POLYGON ((-122.085786228724203 37.421362088869692 25,'
        '-122.085731299060299 37.421369359894811 25,'
        '-122.085731299291794 37.421409349109027 25,'
        '-122.085607707367899 37.421383901665649 25,'
        '-122.085580242651602 37.42137299550869 25,'
        '-122.085218622197104 37.421372995043157 25,'
        '-122.085227776563897 37.421616565082651 25,'
        '-122.085259818934702 37.421605658944031 25,'
        '-122.085259818549901 37.421682001560001 25,'
        '-122.085236931147804 37.421700178603459 25,'
        '-122.085264395782801 37.421761979825753 25,'
        '-122.085323903274599 37.421761980139067 25,'
        '-122.085355945432397 37.421852864451999 25,'
        '-122.085410875246296 37.421889218237339 25,'
        '-122.085479537935697 37.42189285337048 25,'
        '-122.085543622981902 37.421889217975462 25,'
        '-122.085626017804202 37.421860134999257 25,'
        '-122.085937287963006 37.421860134536047 25,'
        '-122.085942871866607 37.42160898590042 25,'
        '-122.085965546986102 37.421579927591438 25,'
        '-122.085864046234093 37.421471150029568 25,'
        '-122.0858548911215 37.421405713261841 25,'
        '-122.085809116276806 37.4214057134039 25,'
        '-122.085786228724203 37.421362088869692 25))')
    self.CheckFeatureGeometry(
        layer.GetNextFeature(),
        'POLYGON ((-122.084437112828397 37.421772530030907 19,'
        '-122.084511885574599 37.421911115428962 19,'
        '-122.0850470999805 37.421787551215353 19,'
        '-122.085071991339106 37.421436630231611 19,'
        '-122.084916406231997 37.421372378221157 19,'
        '-122.084219386816699 37.421372378016258 19,'
        '-122.084219386589993 37.421476171614962 19,'
        '-122.083808641999099 37.4214613409357 19,'
        '-122.083789972856394 37.421313064107963 19,'
        '-122.083279653469802 37.421293288405927 19,'
        '-122.083260981920702 37.421392139442979 19,'
        '-122.082937362173695 37.421372363998763 19,'
        '-122.082906242566693 37.421515697788713 19,'
        '-122.082850226966499 37.421762825764652 19,'
        '-122.082943578863507 37.421767769696352 19,'
        '-122.083217411188002 37.421792485526858 19,'
        '-122.0835970430103 37.421748007445601 19,'
        '-122.083945555677104 37.421693642376027 19,'
        '-122.084007789463698 37.421762838158529 19,'
        '-122.084113587521003 37.421748011043917 19,'
        '-122.084076247378405 37.421713412923751 19,'
        '-122.084144704773905 37.421678815345693 19,'
        '-122.084144704222993 37.421817206601972 19,'
        '-122.084250333307395 37.421817070044597 19,'
        '-122.084437112828397 37.421772530030907 19))')

  # ogr_kml_write_1
  def testWrite(self):
    with gcore_util.TestTemporaryDirectory(prefix='kml_write') as tmpdir:
      filepath = os.path.join(tmpdir, 'test_write.kml')
      dst = ogr.GetDriverByName(ogr_util.KML_DRIVER).CreateDataSource(filepath)

      point_xy_72 = 'POINT (2 29)'
      srs = osr.SpatialReference()
      layer = dst.CreateLayer('test_wgs72', srs=srs)
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetGeometry(ogr.CreateGeometryFromWkt(point_xy_72))
      self.assertEqual(feature.GetGeometryRef().ExportToWkt(), point_xy_72)
      self.assertEqual(layer.CreateFeature(feature), 0)

      point_xy = 'POINT (-3 -30)'
      layer = dst.CreateLayer('test_wgs84')
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetField('name', 'my_name')
      feature.SetField('description', 'my_description')
      feature.SetGeometry(ogr.CreateGeometryFromWkt(point_xy))
      self.assertEqual(feature.GetGeometryRef().ExportToWkt(), point_xy)
      self.assertEqual(layer.CreateFeature(feature), 0)

      wkt_strings = (
          'POINT (4 -49 -1)',
          'POINT (-5 50 1)',
          'LINESTRING (0 1,2 3)',
          'POLYGON ((0 1,2 3,4 5,0 1),(0 1,2 3,4 5,0 1))',
          'MULTIPOINT (2 49,2 49)',
          'MULTILINESTRING ((0 1,2 3),(0 1,2 3))',
          'MULTIPOLYGON (((0 1,2 3,4 5,0 1),(0 1,2 3,4 5,0 1)),'
          '((0 1,2 3,4 5,0 1),(0 1,2 3,4 5,0 1)))',
          'GEOMETRYCOLLECTION (POINT (2 49 1),LINESTRING (0 1 0,2 3 0))'
      )
      for wkt in wkt_strings:
        feature = ogr.Feature(layer.GetLayerDefn())
        feature.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        self.assertEqual(feature.GetGeometryRef().ExportToWkt(), wkt)
        self.assertEqual(layer.CreateFeature(feature), 0)

      dst = None

      # Verify the file that was written.

      self.assertNotIn('Schema', open(filepath).read())

      self.CheckOpen(filepath)
      layer = self.src.GetLayerByName('test_wgs84')
      self.assertEqual(layer.GetFeatureCount(), 9)

      feature = layer.GetNextFeature()
      self.assertEqual(feature.GetField('name'), 'my_name')
      self.assertEqual(feature.GetField('description'), 'my_description')
      self.assertEqual(feature.GetGeometryRef().ExportToWkt(), point_xy)

      for wkt in wkt_strings:
        feature = layer.GetNextFeature()
        self.assertEqual(feature.GetGeometryRef().ExportToWkt(), wkt)

  def testXmlAttributes(self):
    filepath = ogr_util.GetTestFilePath('description_with_xml.kml')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer(0)
    feature = layer.GetNextFeature()
    self.assertEqual(
        feature.GetField('description'),
        'Description<br></br><i attr="val">Interesting</i><br></br>')

  def testGeometryTypes(self):
    filepath = ogr_util.GetTestFilePath('geometries.kml')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer(0)
    self.assertEqual(layer.GetFeatureCount(), 22)
    wkt_info = (
        (ogr.wkbPolygon, 'POLYGON EMPTY'),
        (ogr.wkbPolygon, 'POLYGON EMPTY'),
        (ogr.wkbPolygon, 'POLYGON EMPTY'),
        (ogr.wkbPolygon, 'POLYGON EMPTY'),
        (ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'),
        (ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'),
        (ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'),
        (ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,1 0,0 0))'),
        (ogr.wkbPolygon, 'POLYGON ((0 0,0 1,1 1,1 0,0 0),'
         '(0 0,0 1,1 1,1 0,0 0))'),
        (ogr.wkbPoint, 'POINT EMPTY'),
        (ogr.wkbPoint, 'POINT EMPTY'),
        (ogr.wkbPoint, 'POINT (0 0)'),
        (ogr.wkbLineString, 'LINESTRING EMPTY'),
        (ogr.wkbLineString, 'LINESTRING EMPTY'),
        (ogr.wkbLineString, 'LINESTRING (0 0,1 1)'),
        (ogr.wkbGeometryCollection, 'GEOMETRYCOLLECTION EMPTY'),
        (ogr.wkbGeometryCollection, 'GEOMETRYCOLLECTION EMPTY'),
        (ogr.wkbMultiPoint, 'MULTIPOINT (0 0)'),
        (ogr.wkbMultiPoint, 'MULTIPOINT (0 0,1 1)'),
        (ogr.wkbMultiLineString, 'MULTILINESTRING ((0 0,1 1))'),
        (ogr.wkbMultiPolygon, 'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))'),
        (ogr.wkbGeometryCollection,
         'GEOMETRYCOLLECTION (POINT (0 0),LINESTRING (0 0,1 1))'),
        # TODO(schwehr): This last entry appears be be beyond the end.
        (ogr.wkbGeometryCollection, ''),
    )
    for feature_num, feature in enumerate(ogr_util.Features(layer)):
      geometry_type, wkt = wkt_info[feature_num]
      logging.info('check %d: %d %x %s', feature_num, geometry_type, geometry_type, wkt)
      self.assertEqual(feature.GetGeometryRef().ExportToWkt(), wkt)
      self.assertEqual(feature.GetGeometryRef().GetGeometryType(),
                       geometry_type)

  # TODO(schwehr): Write the command line test for ogrsf.

  def testInterleavedWriting(self):
    # 2772
    filepath = '/vsimem/interleaved_writing.kml'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      layer = dst.CreateLayer('layer1')
      dst.CreateLayer('layer2')
      feature = ogr.Feature(layer.GetLayerDefn())
      self.assertTrue(layer.CreateFeature(feature))
      dst = None

  def testReadPlacemark(self):
    filepath = ogr_util.GetTestFilePath('placemark.kml')
    self.CheckOpen(filepath)
    layer = self.src.GetLayer(0)
    feature = layer.GetNextFeature()
    self.assertIsNotNone(feature)

  def testReadEmpty(self):
    filepath = ogr_util.GetTestFilePath('empty.kml')
    src = ogr.Open(filepath)
    self.assertEqual(src.GetLayerCount(), 0)

  def testWriteSchema(self):
    filepath = '/vsimem/schema.kml'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      layer = dst.CreateLayer('lyr')
      layer.CreateField(ogr.FieldDefn('strfield', ogr.OFTString))
      layer.CreateField(ogr.FieldDefn('intfield', ogr.OFTInteger))
      layer.CreateField(ogr.FieldDefn('realfield', ogr.OFTReal))
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetField('strfield', 'strfield_val')
      feature.SetField('intfield', '1')
      feature.SetField('realfield', '2.34')
      layer.CreateFeature(feature)
      dst = None

      kml_file = gdal.VSIFOpenL(filepath, 'rb')
      content = gdal.VSIFReadL(1, 1000, kml_file).decode('ascii')
      gdal.VSIFCloseL(kml_file)

      expected = [line.strip() for line in (
          '<?xml version="1.0" encoding="utf-8" ?>',
          '<kml xmlns="http://www.opengis.net/kml/2.2">',
          '<Document id="root_doc">',
          '<Schema name="lyr" id="lyr">',
          '    <SimpleField name="strfield" type="string"></SimpleField>',
          '    <SimpleField name="intfield" type="int"></SimpleField>',
          '    <SimpleField name="realfield" type="float"></SimpleField>',
          '</Schema>',
          '<Folder><name>lyr</name>',
          '  <Placemark>',
          '    <ExtendedData><SchemaData schemaUrl="#lyr">',
          '        <SimpleData name="strfield">strfield_val</SimpleData>',
          '        <SimpleData name="intfield">1</SimpleData>',
          '        <SimpleData name="realfield">2.34</SimpleData>',
          '    </SchemaData></ExtendedData>',
          '  </Placemark>',
          '</Folder>',
          '</Document></kml>'
      )]

      logging.info('%s', content)
      lines = [str(line.strip()) for line in content.split('\n') if line]
      self.assertEqual(lines, expected)

  def testEmptyLayer(self):
    filepath = '/vsimem/empty-layer.kml'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      dst.CreateLayer('empty')
      dst = None

      kml_file = gdal.VSIFOpenL(filepath, 'rb')
      content = gdal.VSIFReadL(1, 1000, kml_file).decode('ascii')
      gdal.VSIFCloseL(kml_file)

      expected = [line.strip() for line in (
          '<?xml version="1.0" encoding="utf-8" ?>',
          '<kml xmlns="http://www.opengis.net/kml/2.2">',
          '<Document id="root_doc">',
          '<Folder><name>empty</name>',
          '</Folder>',
          '</Document></kml>',
      )]

      logging.info('%s', content)
      lines = [str(line.strip()) for line in content.split('\n') if line]
      self.assertEqual(lines, expected)

  def testTwoLayers(self):
    filepath = '/vsimem/two-layers.kml'
    dst = self.driver.CreateDataSource(filepath)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      dst.CreateLayer('empty')
      layer = dst.CreateLayer('lyr')
      layer.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetField('foo', 'bar')
      layer.CreateFeature(feature)
      dst = None

      kml_file = gdal.VSIFOpenL(filepath, 'rb')
      content = gdal.VSIFReadL(1, 1000, kml_file).decode('ascii')
      gdal.VSIFCloseL(kml_file)

      expected = [line.strip() for line in (
          '<?xml version="1.0" encoding="utf-8" ?>',
          '<kml xmlns="http://www.opengis.net/kml/2.2">',
          '<Document id="root_doc">',
          '<Folder><name>empty</name>',
          '</Folder>',
          '<Folder><name>lyr</name>',
          '  <Placemark>',
          '    <ExtendedData><SchemaData schemaUrl="#lyr">',
          '        <SimpleData name="foo">bar</SimpleData>',
          '    </SchemaData></ExtendedData>',
          '  </Placemark>',
          '</Folder>',
          '<Schema name="lyr" id="lyr">',
          '    <SimpleField name="foo" type="string"></SimpleField>',
          '</Schema>',
          '</Document></kml>',
      )]

      logging.info('%s', content)
      lines = [str(line.strip()) for line in content.split('\n') if line]
      self.assertEqual(lines, expected)


if __name__ == '__main__':
  unittest.main()
