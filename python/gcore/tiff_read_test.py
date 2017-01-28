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
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

"""Test geotiff reading.

Format is described here:

http://www.gdal.org/frmt_gtiff.html

Rewrite of tiff_read.py:

http://trac.osgeo.org/gdal/browser/trunk/autotest/gcore/tiff_read.py

TODO(schwehr): Collapse TestCases with same driver one class.
TODO(schwehr): Make keep temp directories be a command line flag
"""

import contextlib
import os
import shutil
import unittest


from osgeo import gdal
from osgeo import osr
import logging
import unittest
from autotest2.gcore import gcore_util
from autotest2.gdrivers import gdrivers_util
from autotest2.ogr import ogr_util

DRIVER = gdrivers_util.GTIFF_DRIVER
EXT = '.tif'

# Unit default transform:
# 0 - Top left x: 0
# 1 - E-W resolution: 1
# 2 - No rotation
# 3 - Top left y: 0
# 4 - No rotation
# 5 - N-S resolution: 1
EMPTY_GEOTRANSFORM = (0.0, 1.0, 0.0, 0.0, 0.0, 1.0)

# TODO(schwehr): Put this someplace that makes more sense.
if not gdal.GetConfigOption('TMPDIR'):
  gdal.SetConfigOption('TMPDIR',
                       os.path.join(os.path.dirname(__file__), 'tmp'))

# TODO(schwehr): Move these helpers into a utility file.


# DMD is Data Model Description or Driver MetaData.
def DmdCreationOptionList():
  driver = gdal.GetDriverByName(DRIVER)
  dmd_list = driver.GetMetadata()['DMD_CREATIONOPTIONLIST']
  return 'JPEG' not in dmd_list


@contextlib.contextmanager
def GdalConfigOptionMgr(name, value, original_value=None):
  if not original_value:
    original_value = gdal.GetConfigOption(name)
  gdal.SetConfigOption(name, value)
  yield
  gdal.SetConfigOption(name, original_value)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestReadGtiff(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestReadGtiff, self).setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gcore_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDriver(self):
    self.CheckDriver()

  def testInfo(self):
    for base in ('byte', 'int10', 'int12', 'int16', 'int24', 'int32'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()
    for base in ('uint16', 'uint32'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()
    for base in ('cint16', 'cint32'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()
    for base in ('float16', 'float24', 'float32', 'float64'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()
    for base in ('cfloat32', 'cfloat64'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()
    for base in ('float32_minwhite', 'minfloat'):
      filepath = self.getTestFilePath(base + EXT)
      self.CheckOpen(filepath)
      self.CheckInfo()
    for base in ('rgba-float32', 'rgba-float64', 'rgba-cfloat64'):
      filepath = self.getTestFilePath(base + '.tif')
      self.CheckOpen(filepath)
      self.CheckInfo()

    # Earth Engine export style.
    filepath = self.getTestFilePath('float32-guuu.tif')
    self.CheckOpen(filepath)
    self.CheckInfo()


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestReadOffset(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestReadOffset, self).setUp(DRIVER, EXT)

  def testReadOffset(self):
    self.CheckDriver()
    basename = gcore_util.GetTestFilePath('byte.tif')
    filepaths = [extra + basename for extra in ('', 'GTIFF_DIR:off:408:',
                                                'GTIFF_DIR:1:')]
    for filepath in filepaths:
      self.CheckOpen(filepath)
      self.CheckGeoTransform((440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0))
      self.CheckBand(1, 4672, gdal.GDT_Byte)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestCheckAlpha(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestCheckAlpha, self).setUp(DRIVER, EXT)

  def testGreyAlpha(self):
    self.CheckDriver()
    filepath = gcore_util.GetTestFilePath('stefan_full_greyalpha.tif')

    self.CheckOpen(filepath)
    self.CheckGeoTransform(EMPTY_GEOTRANSFORM)
    self.CheckBand(1, 1970, gdal.GDT_Byte)
    self.CheckBand(2, 10807, gdal.GDT_Byte)
    self.assertEqual(self.src.RasterCount, 2)
    self.assertEqual(self.src.GetRasterBand(2).GetRasterColorInterpretation(),
                     gdal.GCI_AlphaBand)

  def testRgbAlpha(self):
    filepath = gcore_util.GetTestFilePath('stefan_full_rgba.tif')

    self.CheckOpen(filepath)
    self.CheckGeoTransform(EMPTY_GEOTRANSFORM)
    for i, checksum in enumerate((12603, 58561, 36064, 10807)):
      band_num = i+1
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)

    self.assertEqual(self.src.RasterCount, 4)
    self.assertEqual(self.src.GetRasterBand(4).GetRasterColorInterpretation(),
                     gdal.GCI_AlphaBand)

  def testRgbUndefined(self):
    filename = 'stefan_full_rgba_photometric_rgb.tif'
    filepath = gcore_util.GetTestFilePath(filename)

    self.CheckOpen(filepath)
    self.CheckGeoTransform(EMPTY_GEOTRANSFORM)

    for i, checksum in enumerate((12603, 58561, 36064, 10807)):
      band_num = i+1
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)

    self.assertEqual(self.src.RasterCount, 4)
    self.assertEqual(self.src.GetRasterBand(4).GetRasterColorInterpretation(),
                     gdal.GCI_Undefined)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestCmyk(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestCmyk, self).setUp(DRIVER, EXT)

  def testCmykRgba(self):
    self.CheckDriver()
    filepath = gcore_util.GetTestFilePath('rgbsmall_cmyk.tif')

    self.CheckOpen(filepath)
    self.CheckGeoTransform(EMPTY_GEOTRANSFORM)

    # Make sure all bands are read correctly.
    for i, checksum in enumerate((23303, 25101, 8782, 30658)):
      band_num = i+1
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)
    self.assertEqual(self.src.RasterCount, 4)

    self.assertEqual(self.src.GetMetadata('IMAGE_STRUCTURE'),
                     {'INTERLEAVE': 'PIXEL', 'SOURCE_COLOR_SPACE': 'CMYK'})

    rgba_bands = (gdal.GCI_RedBand, gdal.GCI_GreenBand, gdal.GCI_BlueBand,
                  gdal.GCI_AlphaBand)
    for i, band_type in enumerate(rgba_bands):
      band_num = i+1
      band = self.src.GetRasterBand(band_num)
      self.assertEqual(band.GetRasterColorInterpretation(), band_type)

  def testCmykRaw(self):
    filepath = gcore_util.GetTestFilePath('rgbsmall_cmyk.tif')
    self.CheckOpen('GTIFF_RAW:' + filepath)
    self.CheckBand(1, 29430, gdal.GDT_Byte)

    cmyk_bands = (gdal.GCI_CyanBand, gdal.GCI_MagentaBand, gdal.GCI_YellowBand,
                  gdal.GCI_BlackBand)
    for i, band_type in enumerate(cmyk_bands):
      band_num = i+1
      band = self.src.GetRasterBand(band_num)
      self.assertEqual(band.GetRasterColorInterpretation(), band_type)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
@unittest.skipIf(DmdCreationOptionList(),
                 'JPEG not in DMD_CREATIONOPTIONLIST')
class TestOJpeg(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestOJpeg, self).setUp(DRIVER, EXT)

  def testOjpeg(self):
    filepath = gcore_util.GetTestFilePath('zackthecat.tif')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      self.CheckOpen(filepath)
      self.CheckGeoTransform(EMPTY_GEOTRANSFORM)
      self.CheckBand(1, 61570, gdal.GDT_Byte)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestGzip(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestGzip, self).setUp(DRIVER, EXT)

  def testGzip(self):
    self.CheckDriver()
    filepath = gcore_util.GetTestFilePath('byte.tif.gz')

    self.CheckOpen('/vsigzip/' + filepath)
    self.CheckGeoTransform((440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0))
    self.CheckBand(1, 4672, gdal.GDT_Byte)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestZip(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestZip, self).setUp(DRIVER, EXT)

  def testZips(self):
    for filename in ('byte.tif.zip/byte.tif', 'byte.tif.zip',
                     'onefileinsubdir.zip/'
                     'onefileinsubdir/byte.tif', 'onefileinsubdir.zip',
                     'twofileinsubdir.zip/twofileinsubdir/byte.tif'):
      filepath = gcore_util.GetTestFilePath(filename)

      self.CheckOpen('/vsizip/' + filepath)
      self.CheckGeoTransform((440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0))
      self.CheckBand(1, 4672, gdal.GDT_Byte)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestTar(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestTar, self).setUp(DRIVER, EXT)

  def testTars(self):
    for filename in ('byte.tar/byte.tif', 'byte.tar',
                     'byte.tgz/byte.tif', 'byte.tgz'):
      filepath = gcore_util.GetTestFilePath(filename)
      self.CheckOpen('/vsitar/' + filepath)
      self.CheckGeoTransform((440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0))
      self.CheckBand(1, 4672, gdal.GDT_Byte)


@gdrivers_util.SkipIfDriverMissing(DRIVER)
class TestCoordSys(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestCoordSys, self).setUp(DRIVER, EXT)

  def testGrads(self):
    """Non-degree angular units (#601)."""
    filepath = gcore_util.GetTestFilePath('test_gf.tif')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((827000.0, 0.5, 0.0, 2224000.0, 0.0, -0.5))
    self.CheckBand(1, 1218, gdal.GDT_Byte)
    # TODO(schwehr): difference between GetProjection and GetProjectionRef?
    prj = ('PROJCS["NTF (Paris) / France II",'
           '    GEOGCS["NTF (Paris)",'
           '        DATUM["Nouvelle_Triangulation_Francaise_Paris",'
           '          SPHEROID["Clarke 1880 (IGN)",6378249.2,293.4660212936265,'
           '                AUTHORITY["EPSG","7011"]],'
           '          AUTHORITY["EPSG","6807"]],'
           '        PRIMEM["Paris",2.5969213],'
           '        UNIT["grad",0.01570796326794897],'
           '        AUTHORITY["EPSG","4807"]],'
           '    PROJECTION["Lambert_Conformal_Conic_1SP"],'
           '    PARAMETER["latitude_of_origin",46.8],'
           '    PARAMETER["central_meridian",0],'
           '    PARAMETER["scale_factor",0.99987742],'
           '    PARAMETER["false_easting",600000],'
           '    PARAMETER["false_northing",2200000],'
           '    UNIT["metre",1,'
           '        AUTHORITY["EPSG","9001"]],'
           '    AUTHORITY["EPSG","27582"]]')
    self.CheckProjection(prj)

  @unittest.skipIf('ESRI_BUILD=YES' not in gdal.VersionInfo('BUILD_INFO'),
                   'Need ESRI')
  def testErdasCitationParsing(self):
    filepath = gcore_util.GetTestFilePath('citation_mixedcase.tif')
    self.CheckOpen(filepath)
    self.CheckGeoTransform((827000.0, 0.5, 0.0, 2224000.0, 0.0, -0.5))
    self.CheckBand(1, 1218, gdal.GDT_Byte)
    prj = ('PROJCS["NAD_1983_HARN_StatePlane_Oregon_North_FIPS_3601_Feet_Intl",'
           '    GEOGCS["GCS_North_American_1983_HARN",'
           '        DATUM["NAD83_High_Accuracy_Reference_Network",'
           '            SPHEROID["GRS_1980",6378137.0,298.257222101]],'
           '        PRIMEM["Greenwich",0.0],'
           '        UNIT["Degree",0.0174532925199433]],'
           '    PROJECTION["Lambert_Conformal_Conic_2SP"],'
           '    PARAMETER["False_Easting",8202099.737532808],'
           '    PARAMETER["False_Northing",0.0],'
           '    PARAMETER["Central_Meridian",-120.5],'
           '    PARAMETER["Standard_Parallel_1",44.33333333333334],'
           '    PARAMETER["Standard_Parallel_2",46.0],'
           '    PARAMETER["Latitude_Of_Origin",43.66666666666666],'
           '    UNIT["Foot",0.3048]]')
    self.CheckProjection(prj)

  def testLinearParmUnitsCorrect(self):
    filepath = gcore_util.GetTestFilePath('spaf27_correct.tif')
    self.CheckOpen(filepath)
    prj = ('PROJCS["NAD27 / California zone VI",'
           '    GEOGCS["NAD27",'
           '        DATUM["North_American_Datum_1927",'
           '            SPHEROID["Clarke 1866",6378206.4,294.9786982139006,'
           '                AUTHORITY["EPSG","7008"]],'
           '            AUTHORITY["EPSG","6267"]],'
           '        PRIMEM["Greenwich",0],'
           '        UNIT["degree",0.0174532925199433],'
           '        AUTHORITY["EPSG","4267"]],'
           '    PROJECTION["Lambert_Conformal_Conic_2SP"],'
           '    PARAMETER["standard_parallel_1",33.8833333333333],'
           '    PARAMETER["standard_parallel_2",32.7833333333333],'
           '    PARAMETER["latitude_of_origin",32.1666666666667],'
           '    PARAMETER["central_meridian",-116.25],'
           '    PARAMETER["false_easting",2000000],'
           '    PARAMETER["false_northing",0],'
           '    UNIT["US survey foot",0.3048006096012192,'
           '        AUTHORITY["EPSG","9003"]]]')
    self.CheckProjection(prj)
    srs = osr.SpatialReference(self.src.GetProjectionRef())
    self.assertAlmostEqual(srs.GetProjParm(osr.SRS_PP_FALSE_EASTING), 2000000)

  def testLinearParmUnitsBroken(self):
    filepath = gcore_util.GetTestFilePath('spaf27_brokengdal.tif')
    self.CheckOpen(filepath)
    prj = ('PROJCS["NAD27 / California zone VI",'
           '    GEOGCS["NAD27",'
           '        DATUM["North_American_Datum_1927",'
           '            SPHEROID["Clarke 1866",6378206.4,294.9786982138982,'
           '                AUTHORITY["EPSG","7008"]],'
           '            AUTHORITY["EPSG","6267"]],'
           '        PRIMEM["Greenwich",0],'
           '        UNIT["degree",0.0174532925199433],'
           '        AUTHORITY["EPSG","4267"]],'
           '    PROJECTION["Lambert_Conformal_Conic_2SP"],'
           '    PARAMETER["standard_parallel_1",33.8833333333333],'
           '    PARAMETER["standard_parallel_2",32.7833333333333],'
           '    PARAMETER["latitude_of_origin",32.1666666666667],'
           '    PARAMETER["central_meridian",-116.25],'
           '    PARAMETER["false_easting",609601.2192024381],'
           '    PARAMETER["false_northing",0],'
           '    UNIT["US survey foot",0.3048006096012192,'
           '        AUTHORITY["EPSG","9003"]]]')
    self.CheckProjection(prj)
    srs = osr.SpatialReference(self.src.GetProjectionRef())
    self.assertAlmostEqual(srs.GetProjParm(osr.SRS_PP_FALSE_EASTING),
                           609601.219202438)

  def testLinearParmUnitsEpsg(self):
    filepath = gcore_util.GetTestFilePath('spaf27_epsg.tif')
    self.CheckOpen(filepath)
    prj = ('PROJCS["NAD27 / California zone VI",'
           '    GEOGCS["NAD27",'
           '        DATUM["North_American_Datum_1927",'
           '            SPHEROID["Clarke 1866",6378206.4,294.9786982139006,'
           '                AUTHORITY["EPSG","7008"]],'
           '            AUTHORITY["EPSG","6267"]],'
           '        PRIMEM["Greenwich",0],'
           '        UNIT["degree",0.0174532925199433],'
           '        AUTHORITY["EPSG","4267"]],'
           '    PROJECTION["Lambert_Conformal_Conic_2SP"],'
           '    PARAMETER["standard_parallel_1",33.88333333333333],'
           '    PARAMETER["standard_parallel_2",32.78333333333333],'
           '    PARAMETER["latitude_of_origin",32.16666666666666],'
           '    PARAMETER["central_meridian",-116.25],'
           '    PARAMETER["false_easting",2000000],'
           '    PARAMETER["false_northing",0],'
           '    UNIT["US survey foot",0.3048006096012192,'
           '        AUTHORITY["EPSG","9003"]],'
           '    AUTHORITY["EPSG","26746"]]')
    self.CheckProjection(prj)

    srs = osr.SpatialReference(self.src.GetProjectionRef())
    self.assertAlmostEqual(srs.GetProjParm(osr.SRS_PP_FALSE_EASTING), 2000000)

  def testLinearParmUnits2(self):

    files = ('spaf27_markedcorrect.tif', 'spaf27_markedcorrect.tif',
             'spaf27_brokengdal.tif')
    with GdalConfigOptionMgr('GTIFF_LINEAR_UNITS', 'BROKEN', 'DEFAULT'):
      for filename in files:
        filepath = gcore_util.GetTestFilePath(filename)
        self.CheckOpen(filepath)
        srs = osr.SpatialReference(self.src.GetProjectionRef())
        self.assertAlmostEqual(srs.GetProjParm(osr.SRS_PP_FALSE_EASTING),
                               2000000)

  @unittest.skipIf('GetBlockSize' not in dir(gdal.Band), 'Missing GetBlockSize')
  def testG4Split(self):
    filepath = gcore_util.GetTestFilePath('slim_g4.tif')
    self.CheckOpen(filepath)
    self.CheckBand(1, 3322, gdal.GDT_Byte)
    block_size = self.src.GetRasterBand(1).GetBlockSize()
    self.assertEqual(block_size, [1000, 1])

  def testMultiImages(self):
    filepath = gcore_util.GetTestFilePath('twoimages.tif')
    self.CheckOpen(filepath)
    self.CheckBand(1, 4672, gdal.GDT_Byte)
    md_expected = {
        'SUBDATASET_1_NAME': 'GTIFF_DIR:1:'+filepath,
        'SUBDATASET_1_DESC': 'Page 1 (20P x 20L x 1B)',
        'SUBDATASET_2_NAME': 'GTIFF_DIR:2:'+filepath,
        'SUBDATASET_2_DESC': 'Page 2 (20P x 20L x 1B)'}
    self.assertEqual(self.src.GetMetadata('SUBDATASETS'), md_expected)

    for filenum in (1, 2):
      self.CheckOpen('GTIFF_DIR:%s:%s' % (filenum, filepath))
      self.CheckBand(1, 4672, gdal.GDT_Byte)

  @unittest.skipIf('FileFromMemBuffer' not in dir(gdal),
                   'requires FileFromMemBuffer')
  def testVsiMem(self):
    filepath = '/vsimem/tiffinmem'
    data = open(gcore_util.GetTestFilePath('byte.tif'), 'rb').read()
    gdal.FileFromMemBuffer(filepath, data)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      self.CheckOpen(filepath)
      # TODO(schwehr): Allow CheckOpen to take gdal.GA_Update.
      self.src = gdal.Open(filepath, gdal.GA_Update)
      self.CheckBand(1, 4672, gdal.GDT_Byte)
      self.src.GetRasterBand(1).Fill(0)
      # TODO(schwehr): need self.src = None?

      self.CheckOpen(filepath)
      self.CheckBand(1, 0, gdal.GDT_Byte)

      self.CheckOpen('/vsimem\\tiffinmem')
      self.CheckBand(1, 0, gdal.GDT_Byte)

  @unittest.skipIf('FileFromMemBuffer' not in dir(gdal),
                   'requires FileFromMemBuffer')
  def testVsiZipMem(self):
    filepath = '/vsimem/tiffinmem.zip'
    data = open(gcore_util.GetTestFilePath('byte.tif.zip'), 'rb').read()
    gdal.FileFromMemBuffer(filepath, data)
    with gcore_util.GdalUnlinkWhenDone(filepath):
      self.CheckOpen('/vsizip/' + filepath + '/byte.tif')
      self.CheckBand(1, 4672, gdal.GDT_Byte)

  def testProjectCsTypeGeoKeyTicket3019(self):
    filepath = gcore_util.GetTestFilePath('ticket3019.tif')
    self.CheckOpen(filepath)
    prj = ('PROJCS["WGS 84 / UTM zone 31N",'
           '  GEOGCS["WGS 84",'
           '    DATUM["WGS_1984",'
           '      SPHEROID["WGS 84",6378137,298.257223563,'
           '        AUTHORITY["EPSG","7030"]],'
           '      AUTHORITY["EPSG","6326"]],'
           '    PRIMEM["Greenwich",0],'
           '    UNIT["degree",0.0174532925199433],'
           '    AUTHORITY["EPSG","4326"]],'
           '  PROJECTION["Transverse_Mercator"],'
           '  PARAMETER["latitude_of_origin",0],'
           '  PARAMETER["central_meridian",3],'
           '  PARAMETER["scale_factor",0.9996],'
           '  PARAMETER["false_easting",500000],'
           '  PARAMETER["false_northing",0],'
           '  UNIT["metre",1,'
           '    AUTHORITY["EPSG","9001"]],'
           '  AUTHORITY["EPSG","32631"]]')
    self.CheckProjection(prj)
    self.assertIn('WGS 84 / UTM zone 31N', self.src.GetProjection())

  def testModelTypeGeoKeyOnly(self):
    filepath = gcore_util.GetTestFilePath('ModelTypeGeoKey_only.tif')
    self.CheckOpen(filepath)
    prj = ('LOCAL_CS["unnamed",'
           '  GEOGCS["unknown",'
           '   DATUM["unknown",'
           '    SPHEROID["unretrievable - using WGS84",6378137,298.257223563]],'
           '   PRIMEM["Greenwich",0],'
           '   UNIT[,0.0174532925199433]],'
           '  UNIT["unknown",1]]')
    self.CheckProjection(prj)

  # TODO(schwehr): Give this another go over.
  @unittest.skipIf('TIFF_JPEG12_ENABLED=YES' not in
                   gdal.VersionInfo('BUILD_INFO'),
                   'Need 12 bit jpeg support')
  def test12bJpeg(self):
    with GdalConfigOptionMgr('CPL_ACCUM_ERROR_MSG', 'ON'):
      filepath = gcore_util.GetTestFilePath('mandrilmini_12bitjpeg.tif')
      gdal.ErrorReset()  # TODO(schwehr): needed?
      with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
        with gcore_util.GdalUnlinkWhenDone('mandrilmini_12bitjpeg.tif.aux.xml'):
          self.CheckOpen(filepath)
          self.src.GetRasterBand(1).ReadRaster(0, 0, 1, 1)
          stats = self.src.GetRasterBand(1).GetStatistics(0, 1)
          self.assertTrue(stats[2] > 2150 and stats[2] < 2180)

  # TODO(schwehr): skipIf do not have PAM?
  def testReadStatsFromPam(self):
    with gdrivers_util.ConfigOption('GDAL_PAM_ENABLED', 'ON'):
      filepath = gcore_util.GetTestFilePath('byte.tif')
      proxy_dir = gdal.GetConfigOption('GDAL_PAM_PROXY_DIR')
      logging.info('GDAL_PAM_PROXY_DIR: %s', proxy_dir)
      if proxy_dir:
        logging.info('GDAL_PAM_PROXY_DIR list: %s', os.listdir(proxy_dir))
        pam_filename = os.path.basename(filepath) + '.aux.xml'
        pam_filepath = os.path.join(gdal.GetConfigOption('GDAL_PAM_PROXY_DIR'),
                                    pam_filename)
      else:
        pam_filepath = filepath + '.aux.xml'
      if os.path.exists(pam_filepath):
        os.remove(pam_filepath)

      self.CheckOpen(filepath)
      self.assertEqual(self.src.GetRasterBand(1).GetMetadata(), {})
      stats = self.src.GetRasterBand(1).GetStatistics(0, 1)
      self.assertIterAlmostEqual(stats, [74.0, 255.0, 126.765, 22.92847083])
      self.src = None  # Force flushing of the pam file.
      if proxy_dir:
        logging.info('GDAL_PAM_PROXY_DIR list: %s', os.listdir(proxy_dir))
        dir_files = gdal.ReadDir(gdal.GetConfigOption('GDAL_PAM_PROXY_DIR'))
        matches = [filename for filename in dir_files
                   if filename.endswith(pam_filename)]
        self.assertEqual(len(matches), 1)
      else:
        logging.warn('GDAL_PAM_PROXY_DIR not set')
        self.assertTrue(os.path.exists(pam_filepath),
                        'pam file missing: ' + pam_filepath)

      self.CheckOpen(filepath)
      stats = self.src.GetRasterBand(1).GetStatistics(0, 0)
      self.assertIterAlmostEqual(stats, [74.0, 255.0, 126.765, 22.928470838676])

  @ogr_util.SkipIfDriverMissing(ogr_util.MAPINFO_DRIVER)
  def testGeoRefFromTab(self):
    with gcore_util.TestTemporaryDirectory(prefix='GeoRefFromTab') as tmpdir:
      basepath = os.path.join(tmpdir, 'read_from_tab')
      filepath = basepath + '.tif'
      tabpath = basepath + '.tab'
      test_tif = self.driver.Create(filepath, 1, 1)
      self.assertTrue(test_tif)
      test_tif = None

      # TODO(schwehr): gdal autotest wanted mode of 'wt'
      with open(tabpath, 'w') as tabfile:
        tabfile.write('\n'.join(('!table',
                                 '!version 300',
                                 '!charset WindowsLatin1',
                                 '',
                                 'Definition Table',
                                 '  File "HP.TIF"',
                                 '  Type "RASTER"',
                                 '  (400000,1200000) (0,4000) Label "Pt 1",',
                                 '  (500000,1200000) (4000,4000) Label "Pt 2",',
                                 '  (500000,1300000) (4000,0) Label "Pt 3",',
                                 '  (400000,1300000) (0,0) Label "Pt 4"',
                                 '  CoordSys Earth Projection 8, 79, "m", -2, '
                                 '49, 0.9996012717, 400000, -100000',
                                 '  Units "m"')))
      self.CheckOpen(filepath)
      self.CheckGeoTransform((400000.0, 25.0, 0.0, 1300000.0, 0.0, -25.0))
      prj = ('PROJCS["unnamed",'
             '    GEOGCS["unnamed",'
             '        DATUM["OSGB_1936",'
             '            SPHEROID["Airy 1930",6377563.396,299.3249646],'
             '            TOWGS84[375,-111,431,-0,-0,-0,0]],'
             '        PRIMEM["Greenwich",0],'
             '        UNIT["degree",0.0174532925199433]],'
             '    PROJECTION["Transverse_Mercator"],'
             '    PARAMETER["latitude_of_origin",49],'
             '    PARAMETER["central_meridian",-2],'
             '    PARAMETER["scale_factor",0.9996012717],'
             '    PARAMETER["false_easting",400000],'
             '    PARAMETER["false_northing",-100000],'
             '    UNIT["Meter",1]]')
      self.CheckProjection(prj)
      self.assertIn('OSGB_1936', self.src.GetProjection())

  def testPixelIsPoint(self):
    filepath = gcore_util.GetTestFilePath('byte_point.tif')
    with GdalConfigOptionMgr('GTIFF_POINT_GEO_IGNORE', 'FALSE'):
      self.CheckOpen(filepath)
      self.CheckGeoTransform((440690.0, 60.0, 0.0, 3751350.0, 0.0, -60.0))
    with GdalConfigOptionMgr('GTIFF_POINT_GEO_IGNORE', 'TRUE'):
      self.CheckOpen(filepath)
      self.CheckGeoTransform((440720.0, 60.0, 0.0, 3751320.0, 0.0, -60.0))

  def testGeomatrix(self):
    filepath = gcore_util.GetTestFilePath('geomatrix.tif')
    with GdalConfigOptionMgr('GTIFF_POINT_GEO_IGNORE', 'FALSE'):
      self.CheckOpen(filepath)
      self.CheckGeoTransform((1841001.75, 1.5, -5.0, 1144003.25, -5.0, -1.5))
    with GdalConfigOptionMgr('GTIFF_POINT_GEO_IGNORE', 'TRUE'):
      self.CheckOpen(filepath)
      self.CheckGeoTransform((1841000.0, 1.5, -5.0, 1144000.0, -5.0, -1.5))

  def testCorrupted(self):
    filepath = gcore_util.GetTestFilePath('corrupted_gtiff_tags.tif')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      gdal.Open(filepath)
    err_msg = gdal.GetLastErrorMsg()
    self.assertEqual(err_msg, 'TIFFFetchNormalTag:IO error during reading of '
                     '"GeoASCIIParams"; tag ignored')
    self.assertTrue('IO error during' in err_msg)
    # TODO(schwehr): when does 'Error fetching data for field' occur?

  def testTagWithoutNull(self):
    filepath = gcore_util.GetTestFilePath('tag_without_null_byte.tif')
    # TODO(schwehr): why no error handler in autotest and not reseting errors?
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      gdal.Open(filepath)
    self.assertTrue(gdal.GetLastErrorType())
    # TODO(schwehr): where are the error numbers defined?  Are they stable?

  def testBuggyPackbits(self):
    filepath = gcore_util.GetTestFilePath('byte_buggy_packbits.tif')
    with GdalConfigOptionMgr('GTIFF_IGNORE_READ_ERRORS', None):
      self.CheckOpen(filepath)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      self.assertIsNone(self.src.ReadRaster(0, 0, 20, 20))

    with GdalConfigOptionMgr('GTIFF_IGNORE_READ_ERRORS', 'YES'):
      self.CheckOpen(filepath)
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      self.assertIsNotNone(self.src.ReadRaster(0, 0, 20, 20))

  def testGeoEyeRpcTxtTicket3639(self):
    orig_tif = gcore_util.GetTestFilePath('byte.tif')
    orig_txt = gcore_util.GetTestFilePath('test_rpc.txt')
    with gcore_util.TestTemporaryDirectory(prefix='GeoEyeRpcTxt') as tmpdir:
      tmp_tif = os.path.join(tmpdir, 'test.tif')
      tmp_txt = os.path.join(tmpdir, 'test_rpc.txt')
      shutil.copy(orig_tif, tmp_tif)
      shutil.copy(orig_txt, tmp_txt)
      self.CheckOpen(tmp_tif)
      metadata = self.src.GetMetadata('RPC')
      self.assertEqual(metadata['HEIGHT_OFF'], '+0300.000 meters')
      coefs = [float(val) for val in metadata['LINE_DEN_COEFF'].split()[:2]]
      self.assertAlmostEqual(coefs[0], 1.0)
      self.assertAlmostEqual(coefs[1], -5.207696939454288E-03)

  def testRpcTif(self):
    # TIFF with the RPC tag per http://geotiff.maptools.org/rpc_prop.html
    filepath = gcore_util.GetTestFilePath('byte_rpc.tif')
    self.CheckOpen(filepath)
    metadata = self.src.GetMetadata('RPC')
    self.assertIn('1 -0.00520769693945429', metadata['LINE_DEN_COEFF'])

  def testSmall(self):
    # Create a file in memory from this block of bytes.
    data = (b'\x49\x49\x2A\x00\x08\x00\x00\x00\x04\x00\x00\x01\x03\x00'
            b'\x01\x00\x00\x00\x01\x00\x00\x00\x01\x01\x03\x00\x01\x00'
            b'\x00\x00\x01\x00\x00\x00\x11\x01\x04\x00\x01\x00\x00\x00'
            b'\x00\x00\x00\x00\x17\x01\x04\x00\x01\x00\x00\x00\x01\x00'
            b'\x00\x00')
    gdal.FileFromMemBuffer('/vsimem/small.tif', data)
    filepath = '/vsimem/small.tif'
    with gcore_util.GdalUnlinkWhenDone(filepath):
      self.CheckOpen(filepath)
      self.CheckBand(1, 0, gdal.GDT_Byte)

  def testDosStrip(self):
    filepath = gcore_util.GetTestFilePath('dos_strip_chop.tif')
    with gcore_util.ErrorHandler('CPLQuietErrorHandler'):
      self.CheckOpen(filepath)
      err_msg = gdal.GetLastErrorMsg()
      self.assertIn('denial of service detected', err_msg)
    # TODO(schwehr): Do self.CheckBand(1, 0, gdal.GDT_Byte).

  def testExifAndGps(self):
    filepath = gcore_util.GetTestFilePath('exif_and_gps.tif')
    self.CheckOpen(filepath)
    exif = self.src.GetMetadata('EXIF')
    expected_exif = {'EXIF_SpectralSensitivity': 'EXIF Spectral Sensitivity',
                     'EXIF_ExifVersion': '0220',
                     'EXIF_GPSVersionID': '0x2 0x2 00 00',
                     'EXIF_GPSLongitude': '(34) (12) (0)',
                     'EXIF_ComponentsConfiguration': '0x1 0x2 0x3 00',
                     'EXIF_GPSLongitudeRef': 'E',
                     'EXIF_GPSLatitudeRef': 'S',
                     'EXIF_GPSLatitude': '(77) (5) (60)'}
    self.assertEqual(exif, expected_exif)

    self.assertEqual(self.src.GetMetadataItem('EXIF_GPSVersionID', 'EXIF'),
                     '0x2 0x2 00 00')

  def testNoExif(self):
    filepath = gcore_util.GetTestFilePath('byte.tif')
    self.CheckOpen(filepath)
    self.assertEqual(self.src.GetMetadata('EXIF'), {})


@gdrivers_util.SkipIfDriverMissing(DRIVER)
@unittest.skipIf(DmdCreationOptionList(),
                 'JPEG not in DMD_CREATIONOPTIONLIST')
class TestJpeg(gdrivers_util.DriverTestCase):

  def setUp(self):
    super(TestJpeg, self).setUp(DRIVER, EXT)

  def testRgbaPixelInterleaved(self):
    filepath = gcore_util.GetTestFilePath('stefan_full_rgba_jpeg_contig.tif')
    self.CheckOpen(filepath)
    self.assertEqual(self.src.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE'),
                     'PIXEL')
    for i, checksum in enumerate([16404, 62700, 37913, 14174]):
      band_num = i+1
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)
      interp = self.src.GetRasterBand(band_num).GetRasterColorInterpretation()
      self.assertEqual(interp, i + gdal.GCI_RedBand)

  def testRgbaBandInterleaved(self):
    filepath = gcore_util.GetTestFilePath('stefan_full_rgba_jpeg_separate.tif')
    self.CheckOpen(filepath)
    self.assertEqual(self.src.GetMetadataItem('INTERLEAVE', 'IMAGE_STRUCTURE'),
                     'BAND')
    for i, checksum in enumerate([16404, 62700, 37913, 14174]):
      band_num = i+1
      self.CheckBand(band_num, checksum, gdal.GDT_Byte)
      interp = self.src.GetRasterBand(band_num).GetRasterColorInterpretation()
      self.assertEqual(interp, i + gdal.GCI_RedBand)

  # TODO(schwehr): Why did this test require a network connection?
  def testYCbCr(self):
    # http://trac.osgeo.org/gdal/ticket/3259
    # http://trac.osgeo.org/gdal/ticket/3894
    filepath = gcore_util.GetTestFilePath('imgpb17.tif')
    self.CheckOpen(filepath)
    self.CheckBand(1, 62628, gdal.GDT_Byte)
    # TODO(schwehr): Why 2 possible checksums (62628 or 28554)?


# TODO(schwehr): Rewrite the network requiring utm.tif test.


if __name__ == '__main__':
  unittest.main()
