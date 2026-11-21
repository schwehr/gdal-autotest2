# Copyright 2014 Google Inc. All Rights Reserved.
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

https://github.com/OSGeo/gdal/blob/master/autotest/ogr/ogr_kml.py
"""

from collections.abc import Iterator
from collections.abc import Sequence
import contextlib
import pathlib

from osgeo import gdal
from osgeo import ogr
from osgeo import osr

from google3.testing.pybase import googletest
from google3.testing.pybase import parameterized
from google3.third_party.gdal.autotest2.python import gdaltest
from google3.third_party.gdal.autotest2.python.gcore import gcore_util
from google3.third_party.gdal.autotest2.python.ogr import ogr_util

DRIVER = ogr_util.KML_DRIVER

_TRUNCATED_KML = """\
<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://earth.google.com/kml/2.1">
  <Document>
    <name>KML Samples</name>
    <open>1</open>
    <description>Unleash your creativity with the help of these
      examples!</description>
    <Folder>
      <name>Polygons</name>
      <visibility>0</visibility>
      <description>Examples of polygon shapes</description>
      <Folder>
        <name>Google Campus</name>
        <visibility>0</visibility>
        <description>A collection showing how easy it is to create 3-dimensional
          buildings</description>
        <LookAt>
          <longitude>-122.084120030116</longitude>
          <latitude>37.42174011925477</latitude>
          <altitude>0</altitude>
          <range>276.7870053764046</range>
          <tilt>53.454348562403</tilt>
          <heading>-34.82469740081282</heading>
        </LookAt>
        <Placemark>
          <name>Building 40</name>
          <visibility>0</visibility>
          <styleUrl>#transRedPoly</styleUrl>
          <Polygon>
            <extrude>1</extrude>
            <altitudeMode>relativeToGround</altitudeMode>
            <outerBoundaryIs>
              <LinearRing>
                <coordinates> -122.0848938459612,37.42257124044786,17
                  -122.0849580979198,37.42211922626856,17
"""

# Exceeds the 8192-byte Expat read buffer in KML::checkValidity() so the first
# chunk validates as KML (with <kml...>) instead of failing silently without
# <?xml, letting KML::parse() hit trailing junk after </kml> when poCurrent_
# is already nullptr.
_JUNK_AFTER_VALID_DOC_KML = (
    '<kml xmlns="http://www.opengis.net/kml/2.2">\n'
    + (' ' * 9000)
    + '\n</kml><kml>'
)


class OgrKmlTest(parameterized.TestCase, gdaltest.TestCase):

  def setUp(self) -> None:
    super().setUp()
    self.enter_context(gdal.ExceptionMgr())
    self.enter_context(ogr.ExceptionMgr())
    self.enter_context(osr.ExceptionMgr())
    # Google3 builds GDAL with KML_ENABLED only (see //third_party/gdal/BUILD);
    # tests like opening raw .kmz rely on LIBKML being absent.
    self.assertIsNone(gdal.GetDriverByName(ogr_util.LIBKML_DRIVER))
    driver = gdal.GetDriverByName(DRIVER)
    self.assertIsNotNone(driver)
    assert driver is not None
    self.driver: gdal.Driver = driver
    vsi_dir = self.enter_context(
        gcore_util.VsimemCleanupDir(
            f'/vsimem/kml_test/{self._testMethodName}'
        )
    )
    self.vsimem_dir = pathlib.PurePosixPath(vsi_dir)
    self.assertEqual(gdal.MkdirRecursive(str(self.vsimem_dir), 0o755), 0)

  @contextlib.contextmanager
  def _open_kml(self, path: str | pathlib.PurePath) -> Iterator[gdal.Dataset]:
    path_str = str(path)
    if not path_str.startswith('/vsi'):
      self.assertTrue(
          pathlib.Path(path_str).exists(), f'Does not exist: {path_str}'
      )
    with gdal.OpenEx(
        path_str,
        gdal.OF_VECTOR | gdal.OF_READONLY,
        allowed_drivers=[DRIVER],
    ) as ds:
      self.assertIsNotNone(ds)
      assert ds is not None
      self.assertEqual(ds.GetDriver().ShortName.lower(), DRIVER)
      yield ds

  def _write_vsimem_kml(self, filename: str, content: str) -> str:
    filepath = str(self.vsimem_dir / filename)
    gdal.FileFromMemBuffer(filepath, content.encode('utf-8'))
    return filepath

  def _read_vsimem_text(self, filepath: str) -> str:
    with gdal.VSIFile(filepath, 'rb') as f:
      return f.read().decode('utf-8')

  def _assert_kml_lines_equal(
      self, filepath: str, expected_lines: Sequence[str]
  ) -> None:
    content = self._read_vsimem_text(filepath)
    actual = [line.strip() for line in content.strip().split('\n')]
    expected = [line.strip() for line in expected_lines]
    self.assertEqual(actual, expected)

  def _assert_geometry_equal(
      self,
      actual: ogr.Geometry | None,
      expected: ogr.Geometry | str,
      max_error: float = 1e-7,
  ) -> None:
    expected_geom = (
        ogr.CreateGeometryFromWkt(expected)
        if isinstance(expected, str)
        else expected
    )
    self.assertIsNotNone(expected_geom)
    self.assertIsNotNone(actual)
    assert actual is not None
    self.assertEqual(actual.GetGeometryType(), expected_geom.GetGeometryType())
    self.assertEqual(actual.IsEmpty(), expected_geom.IsEmpty())
    self.assertEqual(actual.GetGeometryName(), expected_geom.GetGeometryName())
    self.assertEqual(actual.Is3D(), expected_geom.Is3D())
    self.assertEqual(
        actual.GetGeometryCount(), expected_geom.GetGeometryCount()
    )
    self.assertEqual(actual.GetPointCount(), expected_geom.GetPointCount())
    if actual.GetGeometryCount() > 0:
      for i in range(actual.GetGeometryCount()):
        self._assert_geometry_equal(
            actual.GetGeometryRef(i),
            expected_geom.GetGeometryRef(i),
            max_error=max_error,
        )
    else:
      for i in range(actual.GetPointCount()):
        self.assertAlmostEqual(
            actual.GetX(i),
            expected_geom.GetX(i),
            delta=max_error,
            msg=f'point {i} X',
        )
        self.assertAlmostEqual(
            actual.GetY(i),
            expected_geom.GetY(i),
            delta=max_error,
            msg=f'point {i} Y',
        )
        self.assertAlmostEqual(
            actual.GetZ(i),
            expected_geom.GetZ(i),
            delta=max_error,
            msg=f'point {i} Z',
        )

  def _wgs84_srs(self) -> osr.SpatialReference:
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    return srs

  def testSimplePlacemark(self) -> None:
    filepath = ogr_util.GetTestFilePath('point.kml')
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'Layer #0')
      self.assertEqual(layer.GetGeomType(), ogr.wkbPoint)
      self.assertIsSame(layer.GetSpatialRef(), self._wgs84_srs())
      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertEqual(feature.GetField('Name'), 'Clipperton Island')
      self.assertEqual(feature.GetField('description'), '')
      self._assert_geometry_equal(
          feature.GetGeometryRef(), 'POINT (-109.27 10.3)'
      )
      self.assertIsNone(layer.GetNextFeature())

  def testSimplePlacemarkKmz(self) -> None:
    filepath = ogr_util.GetTestFilePath('kml/placemark.kmz')
    self.assertTrue(
        pathlib.Path(filepath).exists(), f'Does not exist: {filepath}'
    )
    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(
            RuntimeError, r'not recognized as being in a supported file format'
        ),
    ):
      gdal.OpenEx(
          filepath,
          gdal.OF_VECTOR | gdal.OF_READONLY,
          allowed_drivers=[DRIVER],
      )

    with self._open_kml('/vsizip/' + filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'placemark.kmz')
      self.assertEqual(layer.GetGeomType(), ogr.wkbPoint25D)
      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertEqual(feature.GetField('Name'), 'Hidden Villa')
      self.assertEqual(feature.GetField('description'), '')
      self._assert_geometry_equal(
          feature.GetGeometryRef(),
          'POINT (-122.1599702146545 37.35291923448307 0)',
      )
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_attributes_1..4, point_read, linestring_read,
  # polygon_read
  def testAttributes(self) -> None:
    filepath = ogr_util.GetTestFilePath('samples.kml')
    with self._open_kml(filepath) as src:
      layer = src.GetLayerByName('Placemarks')
      self.assertIsNotNone(layer)
      self.assertEqual(layer.GetGeomType(), ogr.wkbPoint25D)
      self.assertEqual(
          layer.GetDataset().GetDescription(), src.GetDescription()
      )

      placemark_expectations = (
          (
              'Simple placemark',
              (
                  'Attached to the ground. Intelligently places itself at the\n'
                  'height of the underlying terrain.'
              ),
              'POINT (-122.0822035425683 37.42228990140251 0)',
          ),
          (
              'Floating placemark',
              'Floats a defined distance above the ground.',
              'POINT (-122.084075 37.4220033612141 50)',
          ),
          (
              'Extruded placemark',
              'Tethered to the ground by a customizable\n"tail"',
              'POINT (-122.0857667006183 37.42156927867553 50)',
          ),
      )
      features = list(layer)
      self.assertLen(features, len(placemark_expectations))
      for idx, (feature, (exp_name, exp_desc, exp_wkt)) in enumerate(
          zip(features, placemark_expectations, strict=True)
      ):
        with self.subTest(placemark=idx):
          self.assertEqual(feature.GetField('Name'), exp_name)
          self.assertEqual(feature.GetField('description'), exp_desc)
          self._assert_geometry_equal(feature.GetGeometryRef(), exp_wkt)

      # Upstream: test_ogr_kml_attributes_2
      layer = src.GetLayerByName('Highlighted Icon')
      self.assertIsNotNone(layer)
      self.assertEqual(layer.GetGeomType(), ogr.wkbPoint25D)
      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertEqual(feature.GetField('Name'), 'Roll over this icon')
      self.assertEqual(feature.GetField('description'), '')
      self._assert_geometry_equal(
          feature.GetGeometryRef(),
          'POINT (-122.0856545755255 37.42243077405461 0)',
      )
      self.assertIsNone(layer.GetNextFeature())

      # Upstream: test_ogr_kml_attributes_3, test_ogr_kml_linestring_read
      layer = src.GetLayerByName('Paths')
      self.assertIsNotNone(layer)
      self.assertEqual(layer.GetGeomType(), ogr.wkbLineString25D)
      self.assertEqual(layer.GetFeatureCount(), 6)
      path_expectations = (
          (
              'Tessellated',
              (
                  'If the <tessellate> tag has a value of 1, the line '
                  'will contour to the underlying terrain'
              ),
              (
                  'LINESTRING (-112.081423783034495 36.106778704771372 0, '
                  '-112.087026775269294 36.0905099328766 0)'
              ),
          ),
          (
              'Untessellated',
              (
                  'If the <tessellate> tag has a value of 0, the line '
                  'follow a simple straight-line path from point to point'
              ),
              (
                  'LINESTRING (-112.080622229594994 36.106734600079953 0, '
                  '-112.085242575314993 36.090495986124218 0)'
              ),
          ),
          (
              'Absolute',
              'Transparent purple line',
              (
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
                  '-112.265737458732104 36.086463123013033 2357)'
              ),
          ),
          (
              'Absolute Extruded',
              'Transparent green wall with yellow outlines',
              (
                  'LINESTRING ('
                  '-112.2550785337791 36.07954952145647 2357,'
                  '-112.2549277039738 36.08117083492122 2357,'
                  '-112.2552505069063 36.08260761307279 2357,'
                  '-112.2564540158376 36.08395660588506 2357,'
                  '-112.2580238976449 36.08511401044813 2357,'
                  '-112.2595218489022 36.08584355239394 2357,'
                  '-112.2608216347552 36.08612634548589 2357,'
                  '-112.262073428656 36.08626019085147 2357,'
                  '-112.2633204928495 36.08621519860091 2357,'
                  '-112.2644963846444 36.08627897945274 2357,'
                  '-112.2656969554589 36.08649599090644 2357)'
              ),
          ),
          (
              'Relative',
              'Black line (10 pixels wide), height tracks terrain',
              (
                  'LINESTRING ('
                  '-112.2532845153347 36.09886943729116 645,'
                  '-112.2540466121145 36.09919570465255 645,'
                  '-112.254734666947 36.09984998366178 645,'
                  '-112.255493345654 36.10051310621746 645,'
                  '-112.2563157098468 36.10108441943419 645,'
                  '-112.2568033076439 36.10159722088088 645,'
                  '-112.257494011321 36.10204323542867 645,'
                  '-112.2584106072308 36.10229131995655 645,'
                  '-112.2596588987972 36.10240001286358 645,'
                  '-112.2610581199487 36.10213176873407 645,'
                  '-112.2626285262793 36.10157011437219 645)'
              ),
          ),
          (
              'Relative Extruded',
              'Opaque blue walls with red outline, height tracks terrain',
              (
                  'LINESTRING ('
                  '-112.2656634181359 36.09445214722695 630,'
                  '-112.2652238941097 36.09520916122063 630,'
                  '-112.2645079986395 36.09580763864907 630,'
                  '-112.2638827428817 36.09628572284063 630,'
                  '-112.2635746835406 36.09679275951239 630,'
                  '-112.2635711822407 36.09740038871899 630,'
                  '-112.2640296531825 36.09804913435539 630,'
                  '-112.264327720538 36.09880337400301 630,'
                  '-112.2642436562271 36.09963644790288 630,'
                  '-112.2639148687042 36.10055381117246 630,'
                  '-112.2626894973474 36.10149062823369 630)'
              ),
          ),
      )
      features = list(layer)
      self.assertLen(features, len(path_expectations))
      for idx, (feature, (exp_name, exp_desc, exp_wkt)) in enumerate(
          zip(features, path_expectations, strict=True)
      ):
        with self.subTest(path=idx):
          self.assertEqual(feature.GetField('Name'), exp_name)
          self.assertEqual(feature.GetField('description'), exp_desc)
          self._assert_geometry_equal(feature.GetGeometryRef(), exp_wkt)

      # Upstream: test_ogr_kml_attributes_4, test_ogr_kml_polygon_read
      layer = src.GetLayerByName('Google Campus')
      self.assertIsNotNone(layer)
      self.assertEqual(layer.GetGeomType(), ogr.wkbPolygon25D)
      campus_polygons = (
          (
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
              '-122.084893845961204 37.422571240447859 17))'
          ),
          (
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
              '-122.085741277148301 37.422270331552568 17))'
          ),
          (
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
              '-122.085786228724203 37.421362088869692 25))'
          ),
          (
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
              '-122.084437112828397 37.421772530030907 19))'
          ),
      )
      features = list(layer)
      self.assertLen(features, len(campus_polygons))
      for building_num, feature, exp_wkt in zip(
          range(40, 44), features, campus_polygons, strict=True
      ):
        with self.subTest(building=building_num):
          self.assertEqual(feature.GetField('Name'), f'Building {building_num}')
          self.assertEqual(feature.GetField('description'), '')
          self._assert_geometry_equal(feature.GetGeometryRef(), exp_wkt)

  # Upstream: test_ogr_kml_write_1
  def testWrite(self) -> None:
    filepath = str(self.vsimem_dir / 'test_write.kml')
    point_xy_72 = 'POINT (2 29)'
    point_xy = 'POINT (-3 -30)'
    wkt_strings = (
        'POINT (4 -49 -1)',
        'POINT (-5 50 1)',
        'LINESTRING (0 1,2 3)',
        'POLYGON ((0 1,2 3,4 5,0 1),(0 1,2 3,4 5,0 1))',
        'MULTIPOINT (2 49,2 49)',
        'MULTILINESTRING ((0 1,2 3),(0 1,2 3))',
        (
            'MULTIPOLYGON (((0 1,2 3,4 5,0 1),(0 1,2 3,4 5,0 1)),'
            '((0 1,2 3,4 5,0 1),(0 1,2 3,4 5,0 1)))'
        ),
        'GEOMETRYCOLLECTION (POINT (2 49 1),LINESTRING (0 1 0,2 3 0))',
    )

    wgs72_srs = osr.SpatialReference()
    wgs72_srs.SetWellKnownGeogCS('WGS72')
    wgs72_srs.SetAxisMappingStrategy(osr.OAMS_TRADITIONAL_GIS_ORDER)
    ct = osr.CoordinateTransformation(wgs72_srs, self._wgs84_srs())
    expected_wgs84 = ogr.CreateGeometryFromWkt(point_xy_72)
    self.assertIsNotNone(expected_wgs84)
    assert expected_wgs84 is not None
    self.assertEqual(expected_wgs84.Transform(ct), ogr.OGRERR_NONE)

    with self.driver.Create(filepath, 0, 0, 0, gdal.GDT_Unknown) as dst:
      layer = dst.CreateLayer('test_wgs72', srs=wgs72_srs)
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetGeometry(ogr.CreateGeometryFromWkt(point_xy_72))
      self.assertEqual(layer.CreateFeature(feature), ogr.OGRERR_NONE)
      # CreateFeature must clone before reprojecting, leaving feature intact.
      self.assertEqual(feature.GetGeometryRef().ExportToWkt(), point_xy_72)

      layer = dst.CreateLayer('test_wgs84')
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetField('name', 'my_name')
      feature.SetField('description', 'my_description')
      feature.SetGeometry(ogr.CreateGeometryFromWkt(point_xy))
      self.assertEqual(layer.CreateFeature(feature), ogr.OGRERR_NONE)
      self.assertEqual(feature.GetGeometryRef().ExportToWkt(), point_xy)

      for wkt in wkt_strings:
        feature = ogr.Feature(layer.GetLayerDefn())
        feature.SetGeometry(ogr.CreateGeometryFromWkt(wkt))
        self.assertEqual(layer.CreateFeature(feature), ogr.OGRERR_NONE)
        self.assertEqual(feature.GetGeometryRef().ExportToWkt(), wkt)

    content = self._read_vsimem_text(filepath)
    self.assertNotIn('Schema', content)

    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 2)
      wgs72_layer = src.GetLayerByName('test_wgs72')
      self.assertIsNotNone(wgs72_layer)
      self.assertEqual(wgs72_layer.GetFeatureCount(), 1)
      wgs72_feat = wgs72_layer.GetNextFeature()
      self.assertIsNotNone(wgs72_feat)
      wgs72_geom = wgs72_feat.GetGeometryRef()
      self.assertIsNotNone(wgs72_geom)
      assert wgs72_geom is not None
      self.assertGreater(abs(wgs72_geom.GetX() - 2.0), 1e-4)
      self._assert_geometry_equal(wgs72_geom, expected_wgs84, max_error=1e-6)
      self.assertIsNone(wgs72_layer.GetNextFeature())

      layer = src.GetLayerByName('test_wgs84')
      self.assertIsNotNone(layer)
      features = list(layer)
      self.assertLen(features, len(wkt_strings) + 1)

      self.assertEqual(features[0].GetField('name'), 'my_name')
      self.assertEqual(features[0].GetField('description'), 'my_description')
      self.assertEqual(features[0].GetGeometryRef().ExportToWkt(), point_xy)

      for feature, wkt in zip(features[1:], wkt_strings, strict=True):
        with self.subTest(wkt=wkt):
          self.assertEqual(feature.GetGeometryRef().ExportToWkt(), wkt)

  # Upstream: test_ogr_kml_xml_attributes
  def testXmlAttributes(self) -> None:
    filepath = ogr_util.GetTestFilePath('description_with_xml.kml')
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertEqual(feature.GetField('Name'), 'Simple placemark')
      self.assertEqual(
          feature.GetField('description'),
          'Description<br></br><i attr="val">Interesting</i><br></br>',
      )
      self._assert_geometry_equal(feature.GetGeometryRef(), 'POINT (2 49 0)')
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_read_geometries
  def testGeometryTypes(self) -> None:
    filepath = ogr_util.GetTestFilePath('geometries.kml')
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'APTBoundary')
      self.assertEqual(layer.GetGeomType(), ogr.wkbUnknown)
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
          (
              ogr.wkbPolygon,
              'POLYGON ((0 0,0 1,1 1,1 0,0 0),(0 0,0 1,1 1,1 0,0 0))',
          ),
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
          (
              ogr.wkbGeometryCollection,
              'GEOMETRYCOLLECTION (POINT (0 0),LINESTRING (0 0,1 1))',
          ),
      )
      features = list(layer)
      self.assertLen(features, len(wkt_info))
      for feature_num, (feature, (geometry_type, wkt)) in enumerate(
          zip(features, wkt_info, strict=True)
      ):
        with self.subTest(feature_index=feature_num):
          geom = feature.GetGeometryRef()
          self.assertIsNotNone(geom)
          self.assertEqual(geom.ExportToWkt(), wkt)
          self.assertEqual(geom.GetGeometryType(), geometry_type)

  # Upstream: test_ogr_kml_test_ogrsf
  # Note: The test_ogrsf CLI binary is excluded from Google3's
  # //third_party/gdal build (copy.bara.sky), so we verify dataset metadata
  # and layer capabilities, extent, spatial filtering, and attribute filtering
  # directly via the API.
  def test_samples_layer_capabilities_and_filtering(self) -> None:
    filepath = ogr_util.GetTestFilePath('samples.kml')
    with self._open_kml(filepath) as src:
      layer_names = [
          src.GetLayer(i).GetName() for i in range(src.GetLayerCount())
      ]
      self.assertEqual(
          layer_names,
          [
              'Placemarks',
              'Highlighted Icon',
              'Paths',
              'Google Campus',
              'Extruded Polygon',
              'Absolute and Relative',
          ],
      )
      info = gdal.VectorInfo(src)
      self.assertIn('Placemarks', info)
      self.assertIn('Google Campus', info)

      layer = src.GetLayerByName('Placemarks')
      self.assertIsNotNone(layer)
      self.assertTrue(layer.TestCapability(ogr.OLCStringsAsUTF8))
      self.assertTrue(layer.TestCapability(ogr.OLCZGeometries))
      self.assertFalse(layer.TestCapability(ogr.OLCSequentialWrite))
      self.assertFalse(layer.TestCapability(ogr.OLCCreateField))
      self.assertFalse(layer.TestCapability(ogr.OLCFastFeatureCount))
      self.assertIsSame(layer.GetSpatialRef(), self._wgs84_srs())

      extent = layer.GetExtent()
      self.assertAlmostEqual(extent[0], -122.0857667006183, delta=1e-7)
      self.assertAlmostEqual(extent[1], -122.0822035425683, delta=1e-7)
      self.assertAlmostEqual(extent[2], 37.42156927867553, delta=1e-7)
      self.assertAlmostEqual(extent[3], 37.42228990140251, delta=1e-7)

      layer.SetSpatialFilterRect(-122.083, 37.4221, -122.081, 37.4224)
      try:
        self.assertEqual(layer.GetFeatureCount(), 1)
        layer.ResetReading()
        feat = layer.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('Name'), 'Simple placemark')
        self.assertIsNone(layer.GetNextFeature())
      finally:
        layer.SetSpatialFilter(None)
      self.assertEqual(layer.GetFeatureCount(), 3)

      self.assertEqual(
          layer.SetAttributeFilter("Name = 'Floating placemark'"),
          ogr.OGRERR_NONE,
      )
      try:
        self.assertEqual(layer.GetFeatureCount(), 1)
        layer.ResetReading()
        feat = layer.GetNextFeature()
        self.assertIsNotNone(feat)
        self.assertEqual(feat.GetField('Name'), 'Floating placemark')
        self.assertIsNone(layer.GetNextFeature())
      finally:
        layer.SetAttributeFilter(None)
      self.assertEqual(layer.GetFeatureCount(), 3)

  # Upstream: test_ogr_kml_interleaved_writing (#2772)
  def testInterleavedWriting(self) -> None:
    filepath = str(self.vsimem_dir / 'interleaved_writing.kml')
    with self.driver.Create(filepath, 0, 0, 0, gdal.GDT_Unknown) as dst:
      layer = dst.CreateLayer('layer1')
      dst.CreateLayer('layer2')
      feature = ogr.Feature(layer.GetLayerDefn())
      with (
          gdal.quiet_errors(),
          self.assertRaisesRegex(
              RuntimeError,
              r'Interleaved feature adding to different layers is not '
              r'supported',
          ),
      ):
        layer.CreateFeature(feature)

  # Upstream: test_ogr_kml_read_placemark
  def testReadPlacemark(self) -> None:
    filepath = ogr_util.GetTestFilePath('placemark.kml')
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'Layer #0')
      feature = layer.GetNextFeature()
      self.assertIsNotNone(feature)
      self.assertEqual(feature.GetField('Name'), 'Simple placemark')
      self.assertEqual(
          feature.GetField('description'),
          'Some<br></br>HTML<br></br>content',
      )
      self._assert_geometry_equal(feature.GetGeometryRef(), 'POINT (2 49 0)')
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_read_empty
  def testReadEmpty(self) -> None:
    filepath = ogr_util.GetTestFilePath('empty.kml')
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 0)

  # Upstream: test_ogr_kml_read_emptylayers
  def test_read_empty_layers(self) -> None:
    filepath = self._write_vsimem_kml(
        'emptylayers.kml',
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<kml xmlns="http://www.opengis.net/kml/2.2">\n'
        '  <Document>\n'
        '    <name>Test</name>\n'
        '    <Folder id="Layer1">\n'
        '      <name>Layer1</name>\n'
        '      <Placemark><name>Placemark1</name></Placemark>\n'
        '    </Folder>\n'
        '    <Folder id="Layer2">\n'
        '      <name>Layer2</name>\n'
        '    </Folder>\n'
        '  </Document>\n'
        '</kml>\n',
    )
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 2)
      self.assertEqual(src.GetLayer(0).GetName(), 'Layer1')
      self.assertEqual(src.GetLayer(0).GetFeatureCount(), 0)
      self.assertIsNone(src.GetLayer(0).GetNextFeature())
      self.assertEqual(src.GetLayer(1).GetName(), 'Layer2')
      self.assertEqual(src.GetLayer(1).GetFeatureCount(), 0)
      self.assertIsNone(src.GetLayer(1).GetNextFeature())

  # Upstream: test_ogr_kml_write_schema
  def testWriteSchema(self) -> None:
    filepath = str(self.vsimem_dir / 'schema.kml')
    with self.driver.Create(filepath, 0, 0, 0, gdal.GDT_Unknown) as dst:
      layer = dst.CreateLayer('lyr')
      layer.CreateField(ogr.FieldDefn('strfield', ogr.OFTString))
      layer.CreateField(ogr.FieldDefn('intfield', ogr.OFTInteger))
      layer.CreateField(ogr.FieldDefn('realfield', ogr.OFTReal))
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetField('strfield', 'strfield_val')
      feature.SetField('intfield', 1)
      feature.SetField('realfield', 2.34)
      feature.SetGeometry(ogr.CreateGeometryFromWkt('POINT (1 2)'))
      self.assertEqual(layer.CreateFeature(feature), ogr.OGRERR_NONE)

    self._assert_kml_lines_equal(
        filepath,
        (
            '<?xml version="1.0" encoding="utf-8" ?>',
            '<kml xmlns="http://www.opengis.net/kml/2.2">',
            '<Document id="root_doc">',
            '<Schema name="lyr" id="lyr">',
            '    <SimpleField name="strfield" type="string"></SimpleField>',
            '    <SimpleField name="intfield" type="int"></SimpleField>',
            '    <SimpleField name="realfield" type="float"></SimpleField>',
            '</Schema>',
            '<Folder><name>lyr</name>',
            '  <Placemark id="lyr.1">',
            '    <ExtendedData><SchemaData schemaUrl="#lyr">',
            '        <SimpleData name="strfield">strfield_val</SimpleData>',
            '        <SimpleData name="intfield">1</SimpleData>',
            '        <SimpleData name="realfield">2.34</SimpleData>',
            '    </SchemaData></ExtendedData>',
            '    <Point><coordinates>1,2</coordinates></Point>',
            '  </Placemark>',
            '</Folder>',
            '</Document></kml>',
        ),
    )

    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'lyr')
      # TODO: b/335317901 - Assert strfield/intfield/realfield once GDAL adds
      # KML SchemaData read support (current pinned driver ignores SchemaData).
      defn = layer.GetLayerDefn()
      field_names = [
          defn.GetFieldDefn(i).GetName() for i in range(defn.GetFieldCount())
      ]
      self.assertEqual(field_names, ['Name', 'Description'])
      self.assertEqual(layer.GetFeatureCount(), 1)
      feat = layer.GetNextFeature()
      self.assertIsNotNone(feat)
      self._assert_geometry_equal(feat.GetGeometryRef(), 'POINT (1 2)')
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_empty_layer
  def testEmptyLayer(self) -> None:
    filepath = str(self.vsimem_dir / 'empty-layer.kml')
    with self.driver.Create(filepath, 0, 0, 0, gdal.GDT_Unknown) as dst:
      dst.CreateLayer('empty')

    self._assert_kml_lines_equal(
        filepath,
        (
            '<?xml version="1.0" encoding="utf-8" ?>',
            '<kml xmlns="http://www.opengis.net/kml/2.2">',
            '<Document id="root_doc">',
            '<Folder><name>empty</name>',
            '</Folder>',
            '</Document></kml>',
        ),
    )

    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'empty')
      self.assertEqual(layer.GetFeatureCount(), 0)

  # Upstream: test_ogr_kml_two_layers
  def testTwoLayers(self) -> None:
    filepath = str(self.vsimem_dir / 'two-layers.kml')
    with self.driver.Create(filepath, 0, 0, 0, gdal.GDT_Unknown) as dst:
      dst.CreateLayer('empty')
      layer = dst.CreateLayer('lyr')
      layer.CreateField(ogr.FieldDefn('foo', ogr.OFTString))
      feature = ogr.Feature(layer.GetLayerDefn())
      feature.SetField('foo', 'bar')
      self.assertEqual(layer.CreateFeature(feature), ogr.OGRERR_NONE)

    self._assert_kml_lines_equal(
        filepath,
        (
            '<?xml version="1.0" encoding="utf-8" ?>',
            '<kml xmlns="http://www.opengis.net/kml/2.2">',
            '<Document id="root_doc">',
            '<Folder><name>empty</name>',
            '</Folder>',
            '<Folder><name>lyr</name>',
            '  <Placemark id="lyr.1">',
            '    <ExtendedData><SchemaData schemaUrl="#lyr">',
            '        <SimpleData name="foo">bar</SimpleData>',
            '    </SchemaData></ExtendedData>',
            '  </Placemark>',
            '</Folder>',
            '<Schema name="lyr" id="lyr">',
            '    <SimpleField name="foo" type="string"></SimpleField>',
            '</Schema>',
            '</Document></kml>',
        ),
    )

    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 2)
      self.assertEqual(src.GetLayer(0).GetName(), 'empty')
      self.assertEqual(src.GetLayer(0).GetFeatureCount(), 0)
      self.assertEqual(src.GetLayer(1).GetName(), 'lyr')
      # Geometry-less <Placemark> nodes classify as Empty and are dropped on
      # read by KMLVector::findLayers / eliminateEmpty (kmlvector.cpp:93).
      self.assertEqual(src.GetLayer(1).GetFeatureCount(), 0)
      self.assertIsNone(src.GetLayer(1).GetNextFeature())

  # Upstream: test_ogr_kml_read_folder_with_subfolder_placemark
  def test_read_folder_with_subfolder_placemark(self) -> None:
    filepath = self._write_vsimem_kml(
        'folder_with_subfolder_placemark.kml',
        '<kml xmlns="http://www.opengis.net/kml/2.2">\n'
        '<Folder>\n'
        '  <name>MainFolder</name>\n'
        '  <Folder/>\n'
        '  <Placemark/>\n'
        '</Folder>\n'
        '</kml>\n',
    )
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'MainFolder')
      self.assertEqual(layer.GetFeatureCount(), 0)
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_read_truncated (#6878)
  # Upstream: test_ogr_kml_read_junk_content_after_valid_doc (oss-fuzz 1683)
  @parameterized.named_parameters(
      dict(
          testcase_name='truncated',
          filename='truncated.kml',
          content=_TRUNCATED_KML,
          error_regex=r'XML parsing of KML file failed : no element found',
      ),
      dict(
          testcase_name='junk_content_after_valid_doc',
          filename='junk_content_after_valid_doc.kml',
          content=_JUNK_AFTER_VALID_DOC_KML,
          error_regex=(
              r'XML parsing of KML file failed.*junk after document element'
          ),
      ),
  )
  def test_read_invalid_xml_raises(
      self, filename: str, content: str, error_regex: str
  ) -> None:
    filepath = self._write_vsimem_kml(filename, content)
    with (
        gdal.quiet_errors(),
        self.assertRaisesRegex(RuntimeError, error_regex),
    ):
      gdal.OpenEx(
          filepath,
          gdal.OF_VECTOR | gdal.OF_READONLY,
          allowed_drivers=[DRIVER],
      )

  # Upstream: test_ogr_kml_read_weird_empty_folders (oss-fuzz 1591)
  def test_read_weird_empty_folders(self) -> None:
    filepath = self._write_vsimem_kml(
        'weird_empty_folders.kml',
        '<kml>\n'
        '  <Document>\n'
        '    <Folder>\n'
        '      <kml>\n'
        '        <Document><Folder></Folder></Document>\n'
        '      </kml>\n'
        '      <Placemark></Placemark>\n'
        '    </Folder>\n'
        '  </Document>\n'
        '</kml>\n',
    )
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'Layer #0')
      self.assertEqual(layer.GetFeatureCount(), 0)
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_read_placemark_with_kml_prefix
  def test_read_placemark_with_kml_prefix(self) -> None:
    filepath = self._write_vsimem_kml(
        'placemark_with_kml_prefix.kml',
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<kml:kml xmlns:kml="http://earth.google.com/kml/2.1">\n'
        '  <kml:Placemark>\n'
        '    <kml:name>Simple placemark</kml:name>\n'
        '    <kml:Point>\n'
        '      <kml:coordinates>2,49,0</kml:coordinates>\n'
        '    </kml:Point>\n'
        '  </kml:Placemark>\n'
        '</kml:kml>\n',
    )
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetName(), 'Layer #0')
      self.assertEqual(layer.GetFeatureCount(), 1)
      feat = layer.GetNextFeature()
      self.assertIsNotNone(feat)
      self.assertEqual(feat.GetField('Name'), 'Simple placemark')
      self._assert_geometry_equal(feat.GetGeometryRef(), 'POINT (2 49 0)')
      self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_read_duplicate_folder_name
  def test_read_duplicate_folder_name(self) -> None:
    filepath = self._write_vsimem_kml(
        'duplicate_folder_name.kml',
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<kml xmlns="http://www.opengis.net/kml/2.2">\n'
        '  <Document>\n'
        '    <name>doc</name>\n'
        '    <Folder>\n'
        '      <name>layer</name>\n'
        '      <Placemark>\n'
        '        <name>placemark1</name>\n'
        '        <Point><coordinates>2,49,0</coordinates></Point>\n'
        '      </Placemark>\n'
        '    </Folder>\n'
        '    <Folder>\n'
        '      <name>layer</name>\n'
        '      <Placemark>\n'
        '        <name>placemark2</name>\n'
        '        <Point><coordinates>3,50,0</coordinates></Point>\n'
        '      </Placemark>\n'
        '    </Folder>\n'
        '  </Document>\n'
        '</kml>\n',
    )
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 2)
      expected_layers = (
          ('layer', 'placemark1', 'POINT (2 49 0)'),
          ('layer (#2)', 'placemark2', 'POINT (3 50 0)'),
      )
      for idx, (layer_name, feat_name, wkt) in enumerate(expected_layers):
        with self.subTest(layer=layer_name):
          layer = src.GetLayer(idx)
          self.assertEqual(layer.GetName(), layer_name)
          self.assertEqual(layer.GetFeatureCount(), 1)
          feat = layer.GetNextFeature()
          self.assertIsNotNone(feat)
          self.assertEqual(feat.GetField('Name'), feat_name)
          self._assert_geometry_equal(feat.GetGeometryRef(), wkt)
          self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_read_placemark_in_root_and_subfolder (#7221)
  def test_read_placemark_in_root_and_subfolder(self) -> None:
    filepath = self._write_vsimem_kml(
        'placemark_in_root_and_subfolder.kml',
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>\n'
        '<kml xmlns="http://www.opengis.net/kml/2.2">\n'
        '  <Document>\n'
        '    <name>TopLevel</name>\n'
        '    <Placemark>\n'
        '      <name></name>\n'
        '      <LineString>\n'
        '        <altitudeMode>clampToGround</altitudeMode>\n'
        '        <coordinates>2,49 3,50</coordinates>\n'
        '      </LineString>\n'
        '    </Placemark>\n'
        '    <Folder>\n'
        '      <name>SubFolder1</name>\n'
        '      <Placemark>\n'
        '        <name></name>\n'
        '        <LineString>\n'
        '          <altitudeMode>clampToGround</altitudeMode>\n'
        '          <coordinates>3,50 4,51</coordinates>\n'
        '        </LineString>\n'
        '      </Placemark>\n'
        '    </Folder>\n'
        '  </Document>\n'
        '</kml>\n',
    )
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 2)
      expected = (
          ('TopLevel', 'LINESTRING (2 49,3 50)'),
          ('SubFolder1', 'LINESTRING (3 50,4 51)'),
      )
      for layer_name, expected_wkt in expected:
        with self.subTest(layer=layer_name):
          layer = src.GetLayerByName(layer_name)
          self.assertIsNotNone(layer)
          self.assertEqual(layer.GetFeatureCount(), 1)
          feat = layer.GetNextFeature()
          self.assertIsNotNone(feat)
          self._assert_geometry_equal(feat.GetGeometryRef(), expected_wkt)
          self.assertIsNone(layer.GetNextFeature())

  # Upstream: test_ogr_kml_read_non_conformant_multi_geometries (#4031)
  def test_read_non_conformant_multi_geometries(self) -> None:
    filepath = self._write_vsimem_kml(
        'non_conformant_multi.kml',
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<kml xmlns="http://www.opengis.net/kml/2.2">\n'
        '  <Document>\n'
        '    <name>Test</name>\n'
        '    <Placemark>\n'
        '      <MultiPolygon><Polygon><outerBoundaryIs><LinearRing>\n'
        '        <coordinates>0,0 0,1 1,1 1,0 0,0</coordinates>\n'
        '      </LinearRing></outerBoundaryIs></Polygon></MultiPolygon>\n'
        '    </Placemark>\n'
        '    <Placemark>\n'
        '      <MultiLineString><LineString>\n'
        '        <coordinates>0,0 1,1</coordinates>\n'
        '      </LineString></MultiLineString>\n'
        '    </Placemark>\n'
        '    <Placemark>\n'
        '      <MultiPoint><Point>\n'
        '        <coordinates>0,0</coordinates>\n'
        '      </Point></MultiPoint>\n'
        '    </Placemark>\n'
        '  </Document>\n'
        '</kml>\n',
    )
    with self._open_kml(filepath) as src:
      self.assertEqual(src.GetLayerCount(), 1)
      layer = src.GetLayer(0)
      self.assertEqual(layer.GetFeatureCount(), 3)
      expected_wkts = (
          'MULTIPOLYGON (((0 0,0 1,1 1,1 0,0 0)))',
          'MULTILINESTRING ((0 0,1 1))',
          'MULTIPOINT ((0 0))',
      )
      features = list(layer)
      self.assertLen(features, len(expected_wkts))
      for idx, (feat, expected_wkt) in enumerate(
          zip(features, expected_wkts, strict=True)
      ):
        with self.subTest(feature_index=idx):
          self._assert_geometry_equal(feat.GetGeometryRef(), expected_wkt)


if __name__ == '__main__':
  googletest.main()
