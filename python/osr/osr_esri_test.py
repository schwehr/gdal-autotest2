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
# Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
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

"""Test of ESRI specific translation issues.

Rewrite of

http://trac.osgeo.org/gdal/browser/trunk/autotest/osr/osr_esri.py
"""
import contextlib
import unittest


from osgeo import gdal
from osgeo import osr
import unittest


@contextlib.contextmanager
def GdalConfig(option, value, default=None):
  orig = gdal.GetConfigOption(option, default)
  gdal.SetConfigOption(option, value)
  yield
  if orig is not None:
    gdal.SetConfigOption(option, orig)


class OsrEsri(unittest.TestCase):

  def testEsri01MorphToEsri(self):
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4202)
    self.assertEqual(srs.GetAttrValue('DATUM'),
                     'Australian_Geodetic_Datum_1966')

    srs.MorphToESRI()
    self.assertEqual(srs.GetAttrValue('DATUM'),
                     'D_Australian_1966')

    srs.MorphFromESRI()
    self.assertEqual(srs.GetAttrValue('DATUM'),
                     'Australian_Geodetic_Datum_1966')

  def testEsri02UtmNames(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput('+proj=utm +zone=11 +south +datum=WGS84')
    srs.MorphToESRI()

    self.assertEqual(srs.GetAttrValue('GEOGCS'), 'GCS_WGS_1984')
    self.assertEqual(srs.GetAttrValue('PROJCS'), 'WGS_1984_UTM_Zone_11S')

  def testEsri03Unnamed(self):
    # TODO(schwehr): The original test said that this would convert
    #   Unnamed to Unknown, but there is no reference to Unknown in the
    #   WKT after converting to ESRI.
    srs = osr.SpatialReference()
    srs.SetFromUserInput('+proj=mill +datum=WGS84')
    self.assertEqual(srs.GetAttrValue('PROJCS'), 'unnamed')

    srs.MorphToESRI()
    self.assertEqual(srs.GetAttrValue('PROJCS'), 'Miller_Cylindrical')

  def testEsri04PolarStereographic(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["PS Test",'
        '    GEOGCS["WGS 84",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS 84",6378137,298.257223563]],'
        '        PRIMEM["Greenwich",0],'
        '        UNIT["degree",0.0174532925199433]],'
        '    PROJECTION["Polar_Stereographic"],'
        '    PARAMETER["latitude_of_origin",-80.2333],'
        '    PARAMETER["central_meridian",171],'
        '    PARAMETER["scale_factor",0.9999],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["metre",1]]'
    )

    srs.MorphToESRI()
    self.assertEqual(srs.GetAttrValue('PROJECTION'), 'Stereographic_South_Pole')
    self.assertEqual(srs.GetProjParm('standard_parallel_1'), -80.2333)

  def testEsri05PolarStereographicFromEsri(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["PS Test",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["D_WGS_1984",'
        '            SPHEROID["WGS_1984",6378137,298.257223563]],'
        '        PRIMEM["Greenwich",0],'
        '        UNIT["Degree",0.017453292519943295]],'
        '    PROJECTION["Stereographic_South_Pole"],'
        '    PARAMETER["standard_parallel_1",-80.2333],'
        '    PARAMETER["central_meridian",171],'
        '    PARAMETER["scale_factor",0.9999],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["Meter",1]]'
    )

    srs.MorphFromESRI()
    self.assertEqual(srs.GetAttrValue('PROJECTION'), 'Polar_Stereographic')

  def testEsri06Lambert2SPTo2SP(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["Texas Centric Mapping System/Lambert Conformal",GEOGCS['
        '"GCS_North_American_1983",DATUM["D_North_American_1983",SPHEROID['
        '"GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich",0.0],'
        'UNIT["Degree",0.0174532925199433]],PROJECTION['
        '"Lambert_Conformal_Conic"],PARAMETER["False_Easting",1500000.0],'
        'PARAMETER["False_Northing",5000000.0],PARAMETER["Central_Meridian",'
        '-100.0],PARAMETER["Standard_Parallel_1",27.5],PARAMETER['
        '"Standard_Parallel_2",35.0],PARAMETER["Scale_Factor",1.0],'
        'PARAMETER["Latitude_Of_Origin",18.0],UNIT["Meter",1.0]]'
    )

    srs.MorphFromESRI()
    self.assertEqual(srs.GetAttrValue('PROJECTION'),
                     'Lambert_Conformal_Conic_2SP')

  def testEsri07FeetAsServeyFeet(self):
    # http://trac.osgeo.org/gdal/ticket/1533
    prj = [
        'Projection    STATEPLANE',
        'Fipszone      903',
        'Datum         NAD83',
        'Spheroid      GRS80',
        'Units         FEET',
        'Zunits        NO',
        'Xshift        0.0',
        'Yshift        0.0',
        'Parameters    ',
    ]

    srs = osr.SpatialReference()
    srs.ImportFromESRI(prj)

    wkt = '\n'.join((
        'PROJCS["NAD83 / Florida North",',
        '    GEOGCS["NAD83",',
        '        DATUM["North_American_Datum_1983",',
        '            SPHEROID["GRS 1980",6378137,298.257222101,',
        '                AUTHORITY["EPSG","7019"]],',
        '            AUTHORITY["EPSG","6269"]],',
        '        PRIMEM["Greenwich",0,',
        '            AUTHORITY["EPSG","8901"]],',
        '        UNIT["degree",0.01745329251994328,',
        '            AUTHORITY["EPSG","9122"]],',
        '        AUTHORITY["EPSG","4269"]],',
        '    UNIT["Foot_US",0.3048006096012192],',
        '    PROJECTION["Lambert_Conformal_Conic_2SP"],',
        '    PARAMETER["standard_parallel_1",30.75],',
        '    PARAMETER["standard_parallel_2",29.58333333333333],',
        '    PARAMETER["latitude_of_origin",29],',
        '    PARAMETER["central_meridian",-84.5],',
        '    PARAMETER["false_easting",1968500],',
        '    PARAMETER["false_northing",0],',
        '    AXIS["X",EAST],',
        '    AXIS["Y",NORTH]]'
    ))
    srs_wkt = osr.SpatialReference(wkt=wkt)
    self.assertTrue(srs.IsSame(srs_wkt))

  def testEsri08NumericUnits(self):
    # http://trac.osgeo.org/gdal/ticket/1533
    prj = [
        'Projection    STATEPLANE',
        'Fipszone      903',
        'Datum         NAD83',
        'Spheroid      GRS80',
        'Units         3.280839895013123',
        'Zunits        NO',
        'Xshift        0.0',
        'Yshift        0.0',
        'Parameters    '
    ]

    srs = osr.SpatialReference()
    srs.ImportFromESRI(prj)

    wkt = '\n'.join((
        'PROJCS["NAD83 / Florida North",',
        '    GEOGCS["NAD83",',
        '        DATUM["North_American_Datum_1983",',
        '            SPHEROID["GRS 1980",6378137,298.257222101,',
        '                AUTHORITY["EPSG","7019"]],',
        '            AUTHORITY["EPSG","6269"]],',
        '        PRIMEM["Greenwich",0,',
        '            AUTHORITY["EPSG","8901"]],',
        '        UNIT["degree",0.01745329251994328,',
        '            AUTHORITY["EPSG","9122"]],',
        '        AUTHORITY["EPSG","4269"]],',
        '    PROJECTION["Lambert_Conformal_Conic_2SP"],',
        '    PARAMETER["standard_parallel_1",30.75],',
        '    PARAMETER["standard_parallel_2",29.58333333333333],',
        '    PARAMETER["latitude_of_origin",29],',
        '    PARAMETER["central_meridian",-84.5],',
        '    PARAMETER["false_easting",1968503.937007874],',
        '    PARAMETER["false_northing",0],',
        '    UNIT["user-defined",0.3048],',
        '    AUTHORITY["EPSG","26960"]]'
    ))

    srs_wkt = osr.SpatialReference(wkt=wkt)
    self.assertTrue(srs.IsSame(srs_wkt))

  def testEsri09EquidistantConic(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["edc",GEOGCS["GCS_North_American_1983",DATUM["D_North_American_'
        '1983",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich"'
        ',0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Equidistant_Conic'
        '"],PARAMETER["False_Easting",0.0],PARAMETER["False_Northing",0.0],PARA'
        'METER["Central_Meridian",-96.0],PARAMETER["Standard_Parallel_1",29.5],'
        'PARAMETER["Standard_Parallel_2",45.5],PARAMETER["Latitude_Of_Origin",3'
        '7.5],UNIT["Meter",1.0]]'
    )

    expected = (
        'PROJCS["edc",GEOGCS["GCS_North_American_1983",DATUM["North_American_Da'
        'tum_1983",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenw'
        'ich",0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Equidistant_C'
        'onic"],PARAMETER["False_Easting",0.0],PARAMETER["False_Northing",0.0],'
        'PARAMETER["longitude_of_center",-96.0],PARAMETER["Standard_Parallel_1"'
        ',29.5],PARAMETER["Standard_Parallel_2",45.5],PARAMETER["latitude_of_ce'
        'nter",37.5],UNIT["Meter",1.0]]'
    )

    srs.MorphFromESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

    expected = (
        'PROJCS["edc",GEOGCS["GCS_North_American_1983",DATUM["D_North_American_'
        '1983",SPHEROID["GRS_1980",6378137.0,298.257222101]],PRIMEM["Greenwich"'
        ',0.0],UNIT["Degree",0.017453292519943295]],PROJECTION["Equidistant_Con'
        'ic"],PARAMETER["False_Easting",0.0],PARAMETER["False_Northing",0.0],PA'
        'RAMETER["central_meridian",-96.0],PARAMETER["Standard_Parallel_1",29.5'
        '],PARAMETER["Standard_Parallel_2",45.5],PARAMETER["latitude_of_origin"'
        ',37.5],UNIT["Meter",1.0]]'
    )

    srs.MorphToESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

  def testEsri10PlateCarree(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["Sphere_Plate_Carree",GEOGCS["GCS_Sphere",DATUM["D_Sphere",SPHE'
        'ROID["Sphere",6371000.0,0.0]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.'
        '0174532925199433]],PROJECTION["Plate_Carree"],PARAMETER["False_Easting'
        '",0.0],PARAMETER["False_Northing",0.0],PARAMETER["Central_Meridian",0.'
        '0],UNIT["Meter",1.0]]'
    )

    expected = (
        'PROJCS["Sphere_Plate_Carree",GEOGCS["GCS_Sphere",DATUM["Not_specified_'
        'based_on_Authalic_Sphere",SPHEROID["Sphere",6371000.0,0.0]],PRIMEM["Gr'
        'eenwich",0.0],UNIT["Degree",0.0174532925199433]],PROJECTION["Equirecta'
        'ngular"],PARAMETER["False_Easting",0.0],PARAMETER["False_Northing",0.0'
        '],PARAMETER["Central_Meridian",0.0],UNIT["Meter",1.0]]'
    )
    srs.MorphFromESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

    expected = (
        'PROJCS["Sphere_Plate_Carree",GEOGCS["GCS_Sphere",DATUM["D_Sphere",SPHE'
        'ROID["Sphere",6371000.0,0.0]],PRIMEM["Greenwich",0.0],UNIT["Degree",0.'
        '017453292519943295]],PROJECTION["Equidistant_Cylindrical"],PARAMETER["'
        'False_Easting",0.0],PARAMETER["False_Northing",0.0],PARAMETER["Central'
        '_Meridian",0.0],UNIT["Meter",1.0]]'
    )
    srs.MorphToESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

  def testEsri11(self):
    srs = osr.SpatialReference()
    srs.ImportFromESRI([
        'Projection    TRANSVERSE',
        'Datum         NAD27',
        'Spheroid      CLARKE1866',
        'Units         METERS',
        'Zunits        NO',
        'Xshift        0.0',
        'Yshift        0.0',
        'Parameters   ',
        '1.0 /* scale factor at central meridian',
        '-106 56  0.5 /* longitude of central meridian',
        '  39 33 30 /* latitude of origin',
        '0.0 /* false easting (meters)',
        '0.0 /* false northing (meters)'
    ])

    expected = (
        'PROJCS["unnamed",GEOGCS["NAD27",DATUM["North_American_Datum_1927",'
        'SPHEROID["Clarke 1866",6378206.4,294.9786982138982,'
        'AUTHORITY["EPSG","7008"]],AUTHORITY["EPSG","6267"]],'
        'PRIMEM["Greenwich",0,AUTHORITY["EPSG","8901"]],'
        'UNIT["degree",0.0174532925199433,AUTHORITY["EPSG","9122"]],'
        'AUTHORITY["EPSG","4267"]],PROJECTION["Transverse_Mercator"],'
        'PARAMETER["latitude_of_origin",39.55833333333333],'
        'PARAMETER["central_meridian",-106.9334722222222],'
        'PARAMETER["scale_factor",1],PARAMETER["false_easting",0],'
        'PARAMETER["false_northing",0],UNIT["METERS",1]]'
    )

    srs.MorphFromESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

  def testEsri12(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'ESRI::PROJCS["Lambert Conformal Conic",GEOGCS["grs80",DATUM["D_North_'
        'American_1983",SPHEROID["Geodetic_Reference_System_1980",6378137,298.'
        '257222101]],PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]'
        '],PROJECTION["Lambert_Conformal_Conic"],PARAMETER["standard_parallel_'
        '1",34.33333333333334],PARAMETER["standard_parallel_2",36.166666666666'
        '66],PARAMETER["latitude_of_origin",33.75],PARAMETER["central_meridian'
        '",-79],PARAMETER["false_easting",609601.22],PARAMETER["false_northing'
        '",0],UNIT["Meter",1]]'
    )

    # No MorphFromESRI() is required.
    self.assertEqual(srs.GetAttrValue('PROJECTION'),
                     'Lambert_Conformal_Conic_2SP')
    self.assertEqual(srs.GetProjParm('standard_parallel_1'), 34.33333333333334)
    self.assertEqual(srs.GetAttrValue('DATUM'), 'North_American_Datum_1983')
    self.assertEqual(srs.GetAttrValue('UNIT'), 'Meter')

  def testEsri13AutoMorphLccWktWithEsriPrefix(self):
    # TODO(schwehr): Does the input reall need to come from a file?
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'ESRI::PROJCS["Lambert Conformal Conic",GEOGCS["grs80",DATUM["D_North_A'
        'merican_1983",SPHEROID["Geodetic_Reference_System_1980",6378137,298.25'
        '7222101]],PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],P'
        'ROJECTION["Lambert_Conformal_Conic"],PARAMETER["standard_parallel_1",3'
        '4.33333333333334],PARAMETER["standard_parallel_2",36.16666666666666],P'
        'ARAMETER["latitude_of_origin",33.75],PARAMETER["central_meridian",-79]'
        ',PARAMETER["false_easting",609601.22],PARAMETER["false_northing",0],UN'
        'IT["Meter",1]]'
    )
    # No MorphFromESRI() is required.
    self.assertEqual(srs.GetAttrValue('PROJECTION'),
                     'Lambert_Conformal_Conic_2SP')
    self.assertEqual(srs.GetProjParm('standard_parallel_1'), 34.33333333333334)
    self.assertEqual(srs.GetAttrValue('DATUM'), 'North_American_Datum_1983')
    self.assertEqual(srs.GetAttrValue('UNIT'), 'Meter')

  def testEsri14StatePlaneNoApplyIfOldStyleLinearUnites(self):
    # http://trac.osgeo.org/gdal/ticket/1697
    srs = osr.SpatialReference()
    srs.ImportFromESRI([
        'PROJECTION STATEPLANE',
        'UNITS feet',
        'FIPSZONE 2600',
        'DATUM NAD83',
        'PARAMETERS'
    ])
    self.assertIsNone(srs.GetAuthorityCode('PROJCS'))

    srs = osr.SpatialReference()
    srs.ImportFromESRI([
        'PROJECTION STATEPLANE',
        'UNITS meter',
        'FIPSZONE 2600',
        'DATUM NAD83',
        'PARAMETERS'
    ])
    self.assertEqual(srs.GetAuthorityCode('PROJCS'), '32104')

  def testEsri15HotineObliqueMercatorRectifiedGridAngle(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["Bern_1898_Bern_LV03C",GEOGCS["GCS_Bern_1898_Bern",DATUM["D_Be'
        'rn_1898",SPHEROID["Bessel_1841",6377397.155,299.1528128]],PRIMEM["Ber'
        'n",7.439583333333333],UNIT["Degree",0.0174532925199433]],PROJECTION["'
        'Hotine_Oblique_Mercator_Azimuth_Center"],PARAMETER["False_Easting",0.'
        '0],PARAMETER["False_Northing",0.0],PARAMETER["Scale_Factor",1.0],PARA'
        'METER["Azimuth",90.0],PARAMETER["Longitude_Of_Center",0.0],PARAMETER['
        '"Latitude_Of_Center",46.95240555555556],UNIT["Meter",1.0]]'
    )

    expected = (
        'PROJCS["Bern_1898_Bern_LV03C",GEOGCS["GCS_Bern_1898_Bern",DATUM["D_Be'
        'rn_1898",SPHEROID["Bessel_1841",6377397.155,299.1528128]],PRIMEM["Ber'
        'n",7.439583333333333],UNIT["Degree",0.017453292519943295]],PROJECTION'
        '["Hotine_Oblique_Mercator_Azimuth_Center"],PARAMETER["False_Easting",'
        '0.0],PARAMETER["False_Northing",0.0],PARAMETER["Scale_Factor",1.0],PA'
        'RAMETER["Azimuth",90.0],PARAMETER["Longitude_Of_Center",0.0],PARAMETE'
        'R["Latitude_Of_Center",46.95240555555556],UNIT["Meter",1.0]]'
    )

    srs.MorphFromESRI()
    self.assertIn('rectified_grid_angle', srs.ExportToWkt())

    srs.MorphToESRI()
    wkt = srs.ExportToWkt()
    self.assertNotIn('rectified_grid_angle', wkt)
    self.assertEqual(wkt, expected)

  def testEsri16EquirectangularToEquidistantCylindrical(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput('+proj=eqc +lat_0=0 +lat_ts=-10 +lon_0=2 '
                         '+x=100000 +y_0=200000 +ellps=sphere')

    expected = (
        'PROJCS["Equidistant_Cylindrical",GEOGCS["GCS_Normal Sphere (r=6370997)'
        '",DATUM["D_unknown",SPHEROID["sphere",6370997,0]],PRIMEM["Greenwich",0'
        '],UNIT["Degree",0.017453292519943295]],PROJECTION["Equidistant_Cylindr'
        'ical"],PARAMETER["central_meridian",2],PARAMETER["standard_parallel_1"'
        ',-10],PARAMETER["false_easting",0],PARAMETER["false_northing",200000],'
        'UNIT["Meter",1]]'
    )

    srs.MorphToESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

  def testEsri17Laea(self):
    # http://trac.osgeo.org/gdal/ticket/3017
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["ETRS89 / ETRS-LAEA",GEOGCS["ETRS89",DATUM["European_Terrestri'
        'al_Reference_System_1989",SPHEROID["GRS 1980",6378137,298.257222101]]'
        ',PRIMEM["Greenwich",0],UNIT["degree",0.01745329251994328]],UNIT["metr'
        'e",1],PROJECTION["Lambert_Azimuthal_Equal_Area"],PARAMETER["latitude_'
        'of_center",52],PARAMETER["longitude_of_center",10],PARAMETER["false_e'
        'asting",4321000],PARAMETER["false_northing",3210000]]'
    )

    expected = (
        'PROJCS["ETRS89_ETRS_LAEA",GEOGCS["GCS_ETRS_1989",DATUM["D_ETRS_1989",'
        'SPHEROID["GRS_1980",6378137,298.257222101]],PRIMEM["Greenwich",0],UNI'
        'T["Degree",0.017453292519943295]],PROJECTION["Lambert_Azimuthal_Equal'
        '_Area"],PARAMETER["latitude_of_origin",52],PARAMETER["central_meridia'
        'n",10],PARAMETER["false_easting",4321000],PARAMETER["false_northing",'
        '3210000],UNIT["Meter",1]]'
    )
    srs.MorphToESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

    expected = (
        'PROJCS["ETRS89_ETRS_LAEA",GEOGCS["GCS_ETRS_1989",DATUM["European_Terr'
        'estrial_Reference_System_1989",SPHEROID["GRS_1980",6378137,298.257222'
        '101]],PRIMEM["Greenwich",0],UNIT["Degree",0.017453292519943295]],PROJ'
        'ECTION["Lambert_Azimuthal_Equal_Area"],PARAMETER["latitude_of_center"'
        ',52],PARAMETER["longitude_of_center",10],PARAMETER["false_easting",43'
        '21000],PARAMETER["false_northing",3210000],UNIT["Meter",1]]'
    )
    srs.MorphFromESRI()
    self.assertEqual(srs.ExportToWkt(), expected)

  def testEsri18EcMorphing(self):
    wkt = '\n'.join((
        'PROJCS["World_Equidistant_Cylindrical",',
        '    GEOGCS["GCS_WGS_1984",',
        '      DATUM["D_WGS_1984",',
        '        SPHEROID["WGS_1984",6378137,298.257223563]],',
        '      PRIMEM["Greenwich",0],',
        '      UNIT["Degree",0.017453292519943295]],',
        '    PROJECTION["Equidistant_Cylindrical"],',
        '    PARAMETER["False_Easting",0],',
        '    PARAMETER["False_Northing",0],',
        '    PARAMETER["Central_Meridian",0],',
        '    PARAMETER["Standard_Parallel_1",60],',
        '    UNIT["Meter",1]]'
    ))
    srs = osr.SpatialReference()
    srs.SetFromUserInput(wkt)

    expected = (
        'PROJCS["World_Equidistant_Cylindrical",GEOGCS["GCS_WGS_1984",DATUM["W'
        'GS_1984",SPHEROID["WGS_1984",6378137,298.257223563]],PRIMEM["Greenwic'
        'h",0],UNIT["Degree",0.017453292519943295]],PROJECTION["Equirectangula'
        'r"],PARAMETER["False_Easting",0],PARAMETER["False_Northing",0],PARAME'
        'TER["Central_Meridian",0],PARAMETER["standard_parallel_1",60],UNIT["M'
        'eter",1]]'
    )
    srs.MorphFromESRI()
    srs_expected = osr.SpatialReference(wkt=expected)

    self.assertTrue(srs.IsSame(srs_expected))
    srs.MorphToESRI()
    srs_expected = osr.SpatialReference(wkt=wkt)

    self.assertTrue(srs.IsSame(srs_expected))

  def testEsri19SphereoidRemapping(self):
    # http://trac.osgeo.org/gdal/ticket/3904
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'GEOGCS["GCS_South_American_1969",DATUM["D_South_American_1969",SPHERO'
        'ID["GRS_1967_Truncated",6378160.0,298.25]],PRIMEM["Greenwich",0.0],UN'
        'IT["Degree",0.0174532925199433]]'
    )

    srs.MorphFromESRI()
    self.assertEqual(srs.GetAttrValue('SPHEROID'), 'GRS_1967_Modified')

    srs.MorphToESRI()
    self.assertEqual(srs.GetAttrValue('SPHEROID'), 'GRS_1967_Truncated')

  def CheckOsrEsriProj(self, wkt_esri, wkt_ogc, proj4):
    # TODO(schwehr): make sure this really does what the original function does.

    # esri->ogc, esri->proj
    srs_esri = osr.SpatialReference(wkt_esri)
    srs_esri.SetAttrValue('PROJCS|GEOGCS|DATUM', 'unknown')
    srs_esri.MorphFromESRI()

    srs_ogc = osr.SpatialReference(wkt_ogc)
    srs_ogc.SetAttrValue('PROJCS|GEOGCS|DATUM', 'unknown')

    self.assertTrue(srs_esri.IsSame(srs_ogc))
    self.assertEqual(proj4, srs_esri.ExportToProj4())

    # ogc->esri, ogc->proj
    srs_esri = osr.SpatialReference(wkt_esri)
    srs_esri.SetAttrValue('PROJCS|GEOGCS|DATUM', 'unknown')

    srs_ogc = osr.SpatialReference(wkt_ogc)
    self.assertEqual(proj4, srs_ogc.ExportToProj4())
    srs_ogc.MorphToESRI()
    srs_ogc.SetAttrValue('PROJCS|GEOGCS|DATUM', 'unknown')

    self.assertTrue(srs_esri.IsSame(srs_ogc))

    # proj->esri, proj->ogc
    srs_esri = osr.SpatialReference()
    srs_esri.SetFromUserInput(proj4)
    srs_esri.MorphFromESRI()
    srs_esri.SetAttrValue('PROJCS|GEOGCS|DATUM', 'unknown')

    srs_ogc = osr.SpatialReference()
    srs_ogc.SetFromUserInput(proj4)
    srs_ogc.SetAttrValue('PROJCS|GEOGCS|DATUM', 'unknown')

    self.assertEqual(srs_esri.ExportToProj4(), proj4)
    self.assertEqual(srs_ogc.ExportToProj4(), proj4)

  def testEsri20AStereographic(self):
    # Stereographic / Stereographic / +proj=stere +lat_0=0 +lon_0=0
    # Modified definitions from ESRI 'Stereographic (world).prj'.
    wkt_esri = (
        'PROJCS["World_Stereographic",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS_1984",6378137.0,298.257223563]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.0174532925199433]],'
        '    PROJECTION["Stereographic"],'
        '    PARAMETER["False_Easting",0.0],'
        '    PARAMETER["False_Northing",0.0],'
        '    PARAMETER["Central_Meridian",0.0],'
        '    PARAMETER["Scale_Factor",1.0],'
        '    PARAMETER["Latitude_Of_Origin",0.0],'
        '    UNIT["Meter",1.0]]'
        )
    wkt_ogc = (
        'PROJCS["World_Stereographic",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["WGS_84",'
        '            SPHEROID["WGS_84",6378137.0,298.257223563]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.0174532925199433]],'
        '    PROJECTION["Stereographic"],'
        '    PARAMETER["False_Easting",0.0],'
        '    PARAMETER["False_Northing",0.0],'
        '    PARAMETER["Central_Meridian",0.0],'
        '    PARAMETER["Scale_Factor",1.0],'
        '    PARAMETER["Latitude_Of_Origin",0.0],'
        '    UNIT["Meter",1.0]]'
        )
    proj4 = (
        '+proj=stere +lat_0=0 +lon_0=0 +k=1 +x_0=0 +y_0=0 '
        '+ellps=WGS84 +units=m +no_defs '
        )

    self.assertEqual(type(wkt_esri), str)
    self.CheckOsrEsriProj(wkt_esri, wkt_ogc, proj4)

  def testEsri20BDoubleAndObliqueStereographic(self):
    # http://trac.osgeo.org/gdal/ticket/1428
    # http://trac.osgeo.org/gdal/ticket/4267
    # Modified definitions from ESRI 'Stereo 1970.prj'.
    wkt_esri = (
        'PROJCS["Stereo_70",'
        '    GEOGCS["GCS_Dealul_Piscului_1970",'
        '        DATUM["D_Dealul_Piscului_1970",'
        '            SPHEROID["Krasovsky_1940",6378245.0,298.3]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.0174532925199433]],'
        '    PROJECTION["Double_Stereographic"],'
        '    PARAMETER["False_Easting",500000.0],'
        '    PARAMETER["False_Northing",500000.0],'
        '    PARAMETER["Central_Meridian",25.0],'
        '    PARAMETER["Scale_Factor",0.99975],'
        '    PARAMETER["Latitude_Of_Origin",46.0],'
        '    UNIT["Meter",1.0]]'
    )
    wkt_ogc = (
        'PROJCS["Stereo_70",'
        '    GEOGCS["GCS_Dealul_Piscului_1970",'
        '        DATUM["Dealul_Piscului_1970",'
        '            SPHEROID["Krasovsky_1940",6378245.0,298.3]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.0174532925199433]],'
        '    PROJECTION["Oblique_Stereographic"],'
        '    PARAMETER["False_Easting",500000.0],'
        '    PARAMETER["False_Northing",500000.0],'
        '    PARAMETER["Central_Meridian",25.0],'
        '    PARAMETER["Scale_Factor",0.99975],'
        '    PARAMETER["Latitude_Of_Origin",46.0],'
        '    UNIT["Meter",1.0]]'
        )
    proj4 = (
        '+proj=sterea +lat_0=46 +lon_0=25 +k=0.99975 +x_0=500000 +y_0=500000 '
        '+ellps=krass +units=m +no_defs '
        )

    self.CheckOsrEsriProj(wkt_esri, wkt_ogc, proj4)

  # TODO(schwehr): Track down this issue.
  @unittest.skip('Fix this test')
  def testEsri20CStereographicNorthPoleAndPolarStereographic(self):
    # Modified from ESRI 'WGS 1984 NSIDC Sea Ice Polar Stereographic North.prj'.
    wkt_esri = (
        'PROJCS["WGS_1984_NSIDC_Sea_Ice_Polar_Stereographic_North",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["D_WGS_1984",'
        '            SPHEROID["WGS_1984",6378137.0,298.257223563]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.0174532925199433]],'
        '    PROJECTION["Stereographic_North_Pole"],'
        '    PARAMETER["False_Easting",0.0],'
        '    PARAMETER["False_Northing",0.0],'
        '    PARAMETER["Central_Meridian",-45.0],'
        '    PARAMETER["Standard_Parallel_1",70.0],'
        '    UNIT["Meter",1.0]]'
    )
    wkt_ogc = (
        'PROJCS["WGS_1984_NSIDC_Sea_Ice_Polar_Stereographic_North",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS_84",6378137.0,298.257223563]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.0174532925199433]],'
        '    PROJECTION["Polar_Stereographic"],'
        '    PARAMETER["False_Easting",0.0],'
        '    PARAMETER["False_Northing",0.0],'
        '    PARAMETER["Central_Meridian",-45.0],'
        '    PARAMETER["latitude_of_origin",70.0],'
        '    UNIT["Meter",1.0]]'
        )
    proj4 = (
        '+proj=stere +lat_0=90 +lat_ts=70 +lon_0=-45 +k=1 +x_0=0 +y_0=0 '
        '+ellps=WGS84 +units=m +no_defs '
        )

    # Why are they differing?
    #  +y_0=0 +ellps=WGS84 +units=m +no_defs
    #  +y_0=0 +datum=WGS84 +units=m +no_defs
    self.CheckOsrEsriProj(wkt_esri, wkt_ogc, proj4)

  def testEsri20DOrthographics(self):
    # http://trac.osgeo.org/gdal/ticket/4249
    wkt_esri = (
        'PROJCS["unnamed",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["unknown",'
        '            SPHEROID["WGS84",6378137,298.257223563]],'
        '        PRIMEM["Greenwich",0],'
        '        UNIT["Degree",0.017453292519943295]],'
        '    PROJECTION["Orthographic"],'
        '    PARAMETER["Latitude_Of_Center",-37],'
        '    PARAMETER["Longitude_Of_Center",145],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["Meter",1]]'
    )
    wkt_ogc = (
        'PROJCS["unnamed",'
        '    GEOGCS["WGS 84",'
        '        DATUM["unknown",'
        '            SPHEROID["WGS84",6378137,298.257223563]],'
        '        PRIMEM["Greenwich",0],'
        '        UNIT["degree",0.0174532925199433]],'
        '    PROJECTION["Orthographic"],'
        '    PARAMETER["latitude_of_origin",-37],'
        '    PARAMETER["central_meridian",145],'
        '    PARAMETER["false_easting",0],'
        '    PARAMETER["false_northing",0],'
        '    UNIT["Meter",1]]'
    )
    proj4 = (
        '+proj=ortho +lat_0=-37 +lon_0=145 +x_0=0 +y_0=0 '
        '+ellps=WGS84 +units=m +no_defs '
        )

    self.CheckOsrEsriProj(wkt_esri, wkt_ogc, proj4)

  # Tests 21-23 require figuring out a confusing helper function in autotest.
  def testEsri21(self):
    # TODO(schwehr): Write this test.
    pass

  def testEsri22(self):
    # TODO(schwehr): Write this test.
    pass

  def testEsri23(self):
    # http://trac.osgeo.org/gdal/ticket/4378
    # http://trac.osgeo.org/gdal/ticket/4345
    # TODO(schwehr): Write this test.
    pass

  # TODO(schwehr): Track down this issue.
  @unittest.skip('Fix this test')
  def testEsri24Lcc2CentralParallelToLatitudeOfOrigin(self):
    # http://trac.osgeo.org/gdal/ticket/3191
    srs = osr.SpatialReference(
        'PROJCS["Custom",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["WGS_1984",'
        '            SPHEROID["WGS_1984",6378137.0,298.257223563]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.017453292519943295]],'
        '    PROJECTION["Lambert_Conformal_Conic_2SP"],'
        '    PARAMETER["False_Easting",0.0],'
        '    PARAMETER["False_Northing",0.0],'
        '    PARAMETER["Central_Meridian",10.5],'
        '    PARAMETER["Standard_Parallel_1",48.66666666666666],'
        '    PARAMETER["Standard_Parallel_2",53.66666666666666],'
        '    PARAMETER["Central_Parallel",51.0],'
        '    UNIT["Meter",1.0]]'
    )
    srs.MorphFromESRI()
    self.assertNotAlmostEqual(srs.GetProjParm(osr.SRS_PP_LATITUDE_OF_ORIGIN,
                                              1000.0),
                              1000.0)
    self.assertAlmostEqual(srs.GetProjParm(osr.SRS_PP_LATITUDE_OF_ORIGIN), 51.0)

  def testEsri25MercatorAuxiliarySphere(self):
    srs = osr.SpatialReference()
    srs.SetFromUserInput(
        'PROJCS["WGS_1984_Web_Mercator_Auxiliary_Sphere",'
        '    GEOGCS["GCS_WGS_1984",'
        '        DATUM["D_WGS_1984",'
        '            SPHEROID["WGS_1984",6378137.0,298.257223563]],'
        '        PRIMEM["Greenwich",0.0],'
        '        UNIT["Degree",0.017453292519943295]],'
        '    PROJECTION["Mercator_Auxiliary_Sphere"],'
        '    PARAMETER["False_Easting",0.0],'
        '    PARAMETER["False_Northing",0.0],'
        '    PARAMETER["Central_Meridian",0.0],'
        '    PARAMETER["Standard_Parallel_1",0.0],'
        '    PARAMETER["Auxiliary_Sphere_Type",0.0],'
        '    UNIT["Meter",1.0]]'
    )

    target_srs = osr.SpatialReference()
    target_srs.ImportFromEPSG(4326)
    transformer = osr.CoordinateTransformation(srs, target_srs)
    expected_proj4_string  = ('+a=6378137 +b=6378137 +proj=merc +lat_ts=0'
                              ' +lon_0=0 +x_0=0 +y_0=0 +k=1 +units=m +no_defs')
    self.assertEqual(expected_proj4_string.split(' ').sort(),
                     srs.ExportToProj4().split(' ').sort())
    (x, y, z) = transformer.TransformPoint(7000000, 7000000, 0)
    self.assertAlmostEqual(62.882069888366, x)
    self.assertAlmostEqual(53.091818769596, y)
    self.assertEqual(0.0, z)


if __name__ == '__main__':
  unittest.main()
