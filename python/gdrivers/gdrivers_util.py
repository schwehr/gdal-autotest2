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

import contextlib
import json
from optparse import OptionParser
import os
import unittest

from osgeo import gdal
from osgeo import osr

import gflags as flags
import logging
from autotest2.gcore import gcore_util

FLAGS = flags.FLAGS

drivers = [gdal.GetDriver(i).ShortName.lower()
           for i in range(gdal.GetDriverCount())]

AAIGRID_DRIVER = 'aaigrid'
ACE2_DRIVER = 'ace2'
ADRG_DRIVER = 'adrg'
AIG_DRIVER = 'aig'
AIRSAR_DRIVER = 'airsar'
ARG_DRIVER = 'arg'
BAG_DRIVER = 'bag'
BIGGIF_DRIVER = 'biggif'
BLX_DRIVER = 'blx'
BMP_DRIVER = 'bmp'
BSB_DRIVER = 'bsb'
BT_DRIVER = 'bt'
CEOS_DRIVER = 'ceos'
COASP_DRIVER = 'coasp'
COSAR_DRIVER = 'cosar'
CPG_DRIVER = 'cpg'
CTABLE2_DRIVER = 'ctable2'
CTG_DRIVER = 'ctg'
DIMAP_DRIVER = 'dimap'
DIPEX_DRIVER = 'dipex'
DOQ1_DRIVER = 'doq1'
DOQ2_DRIVER = 'doq2'
DTED_DRIVER = 'dted'
E00GRID_DRIVER = 'e00grid'
ECRGTOC_DRIVER = 'ecrgtoc'
ECW_DRIVER = 'ecw'
EHDR_DRIVER = 'ehdr'
EIR_DRIVER = 'eir'
ELAS_DRIVER = 'elas'
ENVI_DRIVER = 'envi'
ERS_DRIVER = 'ers'
ESAT_DRIVER = 'esat'
FAST_DRIVER = 'fast'
FIT_DRIVER = 'fit'
FITS_DRIVER = 'fits'
FUJIBAS_DRIVER = 'fujibas'
GENBIN_DRIVER = 'genbin'
GFF_DRIVER = 'gff'
GIF_DRIVER = 'gif'
GMT_DRIVER = 'gmt'
GRASS_DRIVER = 'grass'
GRASSASCIIGRID_DRIVER = 'grassasciigrid'
GRIB_DRIVER = 'grib'
GS7BG_DRIVER = 'gs7bg'
GSAG_DRIVER = 'gsag'
GSBG_DRIVER = 'gsbg'
GSC_DRIVER = 'gsc'
GTIFF_DRIVER = 'gtiff'
GTX_DRIVER = 'gtx'
GXF_DRIVER = 'gxf'
HDF4_DRIVER = 'hdf4'
HDF5_DRIVER = 'hdf5'
HDF4IMAGE_DRIVER = 'hdf4image'
HDF5IMAGE_DRIVER = 'hdf5image'
HF2_DRIVER = 'hf2'
HFA_DRIVER = 'hfa'
HTTP_DRIVER = 'http'
IDA_DRIVER = 'ida'
ILWIS_DRIVER = 'ilwis'
INGR_DRIVER = 'ingr'
IRIS_DRIVER = 'iris'
ISIS2_DRIVER = 'isis2'
ISIS3_DRIVER = 'isis3'
JAXAPALSAR_DRIVER = 'jaxapalsar'
JDEM_DRIVER = 'jdem'
JP2ECW_DRIVER = 'jp2ecw'
JP2KAK_DRIVER = 'jp2kak'
JPEG2000_DRIVER = 'jpeg2000'
JP2MRSID = 'jp2mrsid'
JP2OPENJPEG = 'jp2openjpeg'
JPEG_DRIVER = 'jpeg'
JPIPKAK_DRIVER = 'jpipkak'
KMLSUPEROVERLAY_DRIVER = 'kmlsuperoverlay'
KRO_DRIVER = 'kro'
L1B_DRIVER = 'l1b'
LAN_DRIVER = 'lan'
LCP_DRIVER = 'lcp'
LEVELLER_DRIVER = 'leveller'
LOSLAS_DRIVER = 'loslas'
MAP_DRIVER = 'map'
MBTILES_DRIVER = 'mbtiles'
MEM_DRIVER = 'mem'
MFF_DRIVER = 'mff'
MFF2_DRIVER = 'mff2'
MG4LIDAR_DRIVER = 'mg4lidar'
MRSID_DRIVER = 'mrsid'
MSGN_DRIVER = 'msgn'
NDF_DRIVER = 'ndf'
NETCDF_DRIVER = 'netcdf'
NGSGEOID_DRIVER = 'ngsgeoid'
NITF_DRIVER = 'nitf'
NTV2_DRIVER = 'ntv2'
NWT_GRC_DRIVER = 'nwt_grc'
NWT_GRD_DRIVER = 'nwt_grd'
OZI_DRIVER = 'ozi'
PAUX_DRIVER = 'paux'
PCIDSK_DRIVER = 'pcidsk'
PCRASTER_DRIVER = 'pcraster'
PDF_DRIVER = 'pdf'
PDS_DRIVER = 'pds'
PNG_DRIVER = 'png'
PNM_DRIVER = 'pnm'
POSTGISRASTER_DRIVER = 'postgisraster'
R_DRIVER = 'r'
RASTERLITE_DRIVER = 'rasterlite'
RIK_DRIVER = 'rik'
RMF_DRIVER = 'rmf'
RPFTOC_DRIVER = 'rpftoc'
RS2_DRIVER = 'rs2'
RST_DRIVER = 'rst'
SAGA_DRIVER = 'saga'
SAR_CEOS_DRIVER = 'sar_ceos'
SDTS_DRIVER = 'sdts'
SGI_DRIVER = 'sgi'
SNODAS_DRIVER = 'snodas'
SRP_DRIVER = 'srp'
SRTMHGT_DRIVER = 'srtmhgt'
TERRAGEN_DRIVER = 'terragen'
TIL_DRIVER = 'til'
TSX_DRIVER = 'tsx'
USGSDEM_DRIVER = 'usgsdem'
VRT_DRIVER = 'vrt'
WCS_DRIVER = 'wcs'
WEBP_DRIVER = 'webp'
WMS_DRIVER = 'wms'
XPM_DRIVER = 'xpm'
XYZ_DRIVER = 'xyz'
ZMAP_DRIVER = 'zmap'

# A string copy of byte.tif so that tests do not need to depend on all of the
# tiff data to get a simple tiff for basic tests.
TIFF_BYTE_FILE = (
    '\x49\x49\x2a\x00\x98\x01\x00\x00\x6b\x7b\x84\x73\x84\x84\x8c\x84\x84'
    '\x84\x6b\x84\x6b\x84\x84\x6b\x7b\x73\x9c\x94\x73\x84\x6b\x7b\x94\x73'
    '\xa5\x73\x8c\x6b\x7b\x7b\x63\x84\x7b\x84\x84\x84\x63\x9c\x73\x84\x8c'
    '\x84\x7b\x73\x8c\x6b\x8c\x73\x84\x7b\x6b\x84\x84\x73\x73\x6b\x73\x6b'
    '\x94\x84\x7b\x7b\x73\x84\x84\x7b\x73\x7b\x73\x7b\x6b\x73\x94\x6b\x73'
    '\x8c\x73\x84\x84\x9c\x84\x8c\x84\x84\x73\x73\x73\x7b\x94\x7b\xa5\x7b'
    '\x84\x6b\x6b\x84\x9c\x7b\xbd\xad\xad\x94\x94\x73\x94\x7b\x6b\x84\x73'
    '\x84\x9c\x63\x7b\x73\x84\x84\xce\x6b\xc5\xad\x94\x8c\x8c\x84\x63\x84'
    '\x7b\x73\x8c\x84\x84\x63\x84\x7b\x84\xad\x7b\x73\x94\x7b\x94\x73\x94'
    '\x7b\x8c\x7b\x6b\x73\x84\x73\x6b\x73\x63\x7b\x63\xb5\x63\x6b\x7b\x73'
    '\x84\x73\x7b\x84\x73\x84\x84\x7b\x7b\x84\x63\x73\x63\x7b\x84\x73\x73'
    '\x6b\x8c\x8c\x63\x8c\x63\x73\x7b\x6b\x84\x6b\x73\x6b\x73\x7b\x84\x7b'
    '\x6b\x7b\x84\x84\x84\x84\x84\x7b\x63\x84\x7b\x6b\x94\x63\x73\x7b\x8c'
    '\xad\x7b\x6b\x7b\x7b\x7b\x6b\x7b\x7b\x7b\x6b\x8c\x7b\x7b\x73\x73\x5a'
    '\x6b\xad\x6b\x6b\x6b\x6b\x63\x84\x7b\x73\xad\x94\x63\x7b\x7b\x6b\x7b'
    '\x63\x6b\xbd\xad\x6b\x73\x73\x6b\x63\x8c\x6b\xad\x8c\x94\x84\x84\x6b'
    '\x7b\x63\x63\x73\x63\x84\x63\x8c\x73\x94\x7b\x63\x84\x7b\x94\x8c\x8c'
    '\x6b\x8c\x5a\x6b\x73\x6b\x5a\x63\x7b\x73\x73\x73\x7b\x7b\x94\x73\x94'
    '\x63\x84\xa5\x94\x9c\x7b\x6b\x6b\x6b\x73\x8c\x63\x73\x63\x63\x6b\x73'
    '\x84\x73\x5a\x7b\x73\xbd\xad\x8c\x8c\xa5\x73\x84\x5a\x63\x73\x5a\x63'
    '\x63\x6b\x63\x84\x63\x6b\x84\x84\x9c\xb5\x8c\xad\x7b\x84\x63\x73\x7b'
    '\x4a\x73\x63\x7b\x8c\x9c\x84\xa5\x8c\x8c\x63\xad\xf7\xff\xce\x84\x6b'
    '\x8c\x7b\x94\x84\xa5\xa5\x94\x8c\x84\x7b\x6b\x7b\x6b\x7b\xb5\xb5\x9c'
    '\x94\x9c\x9c\x9c\xb5\x84\x94\x73\x84\x6b\x6b\x6b\x6b\x6b\x73\x63\x6b'
    '\x0f\x00\x00\x01\x03\x00\x01\x00\x00\x00\x14\x00\x00\x00\x01\x01\x03'
    '\x00\x01\x00\x00\x00\x14\x00\x00\x00\x02\x01\x03\x00\x01\x00\x00\x00'
    '\x08\x00\x00\x00\x03\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x06'
    '\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x11\x01\x04\x00\x01\x00'
    '\x00\x00\x08\x00\x00\x00\x15\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00'
    '\x00\x16\x01\x03\x00\x01\x00\x00\x00\x14\x00\x00\x00\x17\x01\x04\x00'
    '\x01\x00\x00\x00\x90\x01\x00\x00\x1c\x01\x03\x00\x01\x00\x00\x00\x01'
    '\x00\x00\x00\x53\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00\x0e\x83'
    '\x0c\x00\x03\x00\x00\x00\x52\x02\x00\x00\x82\x84\x0c\x00\x06\x00\x00'
    '\x00\x6a\x02\x00\x00\xaf\x87\x03\x00\x18\x00\x00\x00\x9a\x02\x00\x00'
    '\xb1\x87\x02\x00\x16\x00\x00\x00\xca\x02\x00\x00\x00\x00\x00\x00\x00'
    '\x00\x00\x00\x00\x00\x4e\x40\x00\x00\x00\x00\x00\x00\x4e\x40\x00\x00'
    '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    '\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00'
    '\x40\xe6\x1a\x41\x00\x00\x00\x00\xcc\x9e\x4c\x41\x00\x00\x00\x00\x00'
    '\x00\x00\x00\x01\x00\x01\x00\x00\x00\x05\x00\x00\x04\x00\x00\x01\x00'
    '\x01\x00\x01\x04\x00\x00\x01\x00\x01\x00\x02\x04\xb1\x87\x15\x00\x00'
    '\x00\x00\x0c\x00\x00\x01\x00\x57\x68\x04\x0c\x00\x00\x01\x00\x29\x23'
    '\x4e\x41\x44\x32\x37\x20\x2f\x20\x55\x54\x4d\x20\x7a\x6f\x6e\x65\x20'
    '\x31\x31\x4e\x7c\x00')


def SkipIfDriverMissing(driver_name):
  """Decorator that only runs a test if a required driver is found.

  Args:
    driver_name: Lower case short name of a driver.  e.g. 'dted'.

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
      FLAGS.test_srcdir,
      'autotest2/gdrivers/testdata',
             os.path.split(os.path.abspath(__file__))[0],
             'testdata',
      filename
      )


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


class TempFiles(object):

  def __init__(self):
    self.count = 0
    self.tmp_dir = None

  def TempFile(self, basename, ext=''):
    if not self.tmp_dir:
      self.tmp_dir = gdal.GetConfigOption('TMPDIR')
      if not self.tmp_dir:
        logging.fatal('Do not have a tmp_dir!!!')
    filepath = os.path.join(self.tmp_dir,
                            basename + '%03d' % self.count + ext)
    self.count += 1
    return filepath


_temp_files = TempFiles()


@contextlib.contextmanager
def ConfigOption(key, value, default=None):
  """Set a gdal config option and when the context closes, try to revert it.

  TODO(schwehr): This would be better as part of gcore_util.py.

  Args:
    key: String naming the config option.
    value: String value to set the option to.
    default: String value to reset the option to if no starting value.

  Yields:
    None
  """
  original_value = gdal.GetConfigOption(key, default)

  gdal.SetConfigOption(key, value)
  try:
    yield
  finally:
    gdal.SetConfigOption(key, original_value)


class DriverTestCase(unittest.TestCase):
  """Checks the basic functioning of a single raster driver.

  Assumes that only one driver is registered for the file type.

  CheckOpen has a critical side effect that it puts the open data
  source in the src attribute.  Checks below CheckOpen in this class
  assume that self.src is the original open file.
  """

  def setUp(self, driver_name, ext):
    super(DriverTestCase, self).setUp()

    gcore_util.SetupTestEnv()

    assert driver_name
    self.driver_name = driver_name.lower()
    self.driver = gdal.GetDriverByName(driver_name)
    assert self.driver
    self.ext = ext
    # Start with a clean slate.
    gdal.ErrorReset()

    # Allow details and custom message.
    self.longMessage = True

  def assertIterAlmostEqual(self, first, second, places=None, msg=None,
                            delta=None):
    msg = msg or ''
    self.assertEqual(len(first), len(second), 'lists not same length ' + msg)
    for a, b in zip(first, second):
      self.assertAlmostEqual(a, b, places=places, msg=msg, delta=delta)

  def CheckDriver(self):
    self.assertEqual(self.driver_name, self.driver.ShortName.lower())

  def CheckOpen(self, filepath, check_driver=True):
    """Open the test file and keep it open as self.src.

    Args:
      filepath: str, Path to a file to open with GDAL.
      check_driver: If True, make sure that the file opened with the
        default driver for this test.  If it is a str, then check that
        the driver used matches the string.  If False, then do not
        check the driver.
    """
    if filepath.startswith(os.path.sep) and not filepath.startswith('/vsi'):
      self.assertTrue(os.path.isfile(filepath), 'Does not exist: ' + filepath)
    self.src = gdal.Open(filepath, gdal.GA_ReadOnly)
    self.assertTrue(self.src, '%s driver unable to open %s' % (self.driver_name,
                                                               filepath))
    if check_driver:
      driver_name = self.src.GetDriver().ShortName.lower()
      if isinstance(check_driver, str) or isinstance(check_driver, unicode):
        self.assertEqual(check_driver, driver_name)
      else:
        self.assertEqual(self.driver_name, driver_name)
    self.filepath = filepath

  def CheckGeoTransform(self, gt_expected, gt_delta=None):
    gt = self.src.GetGeoTransform()
    if not gt and not gt_expected:
      return
    self.assertEqual(len(gt_expected), 6)
    gt_delta = gt_delta or ((abs(gt_expected[1]) + abs(gt_expected[2])) / 100.0)
    for idx in range(6):
      self.assertAlmostEqual(gt[idx], gt_expected[idx], delta=gt_delta)

  def CheckProjection(self, prj_expected):
    prj = self.src.GetProjection()
    if not prj and not prj_expected:
      return
    src_osr = osr.SpatialReference(wkt=prj)
    prj2 = osr.SpatialReference()
    prj2.SetFromUserInput(prj_expected)
    msg = 'Projection mismatch:\nGot:\n%s\nExpected:\n%s' % (prj, prj_expected)
    self.assertTrue(src_osr.IsSame(prj2), msg=msg)

  def CheckShape(self, width, height, num_bands):
    self.assertEqual(width, self.src.RasterXSize)
    self.assertEqual(height, self.src.RasterYSize)
    self.assertEqual(num_bands, self.src.RasterCount)

  def CheckBand(self, band_num, checksum, gdal_type=None, nodata=None,
                min_val=None, max_val=None):
    band = self.src.GetRasterBand(band_num)
    self.assertEqual(band.Checksum(), checksum)
    if gdal_type is not None:
      self.assertEqual(gdal_type, band.DataType)
    if nodata is not None:
      self.assertEqual(nodata, band.GetNoDataValue())
    if min_val is not None or max_val is not None:
      stats = band.GetStatistics(False, True)
      if min_val is not None:
        self.assertAlmostEqual(min_val, stats[0])
      if max_val is not None:
        self.assertAlmostEqual(max_val, stats[1])

  def CheckBandSubRegion(self, band_num, checksum, xoff, yoff, xsize, ysize):
    band = self.src.GetRasterBand(band_num)
    self.assertEqual(checksum, band.Checksum(xoff, yoff, xsize, ysize))

  # TODO(schwehr): Add assertCreateCopyInterrupt method.
  def CheckCreateCopy(self,
                      check_checksums=True,
                      check_stats=True,
                      check_geotransform=True,
                      check_projection=True,
                      options=None,
                      strict=True,
                      vsimem=False,
                      remove_result=False,
                      checksums=None,
                      stats=None,
                      metadata=None):
    """Compare a copy to the currently open file.

    Args:
      check_checksums: Set to False to not check checksums.  Or a list of one
          checksum per band.
      check_stats: Compare band statistics if true.  Or a list of one
          (min, max) tuple per band.
      check_geotransform: Set to False to skip checking the geotransform.
      check_projection: Set to False to skip checking the projection.
      options: List of options to pass to CreateCopy.
      strict: Set to False to have the CreateCopy operation in loose mode.
      vsimem: If true, copy to memory.
      remove_result: If true, remove the copy when done.
      checksums: Optional list of checksums.  If left out, uses the checksums
          from the input file will be used.
      stats: Optional list of min/max tuples to compare for each band.  If
          left out, uses the stats from the input file.
      metadata: A dictionary of metadata fields to verify.
    Returns:
      Open gdal raster Dataset.
    """
    # TODO(schwehr): Complain if options is a str or unicode.
    # TODO(schwehr): Use gdal.GetConfigOption('TMPDIR') if available.
    options = options or []
    basename = os.path.basename(self.src.GetFileList()[0])
    if vsimem:
      dst_file = os.path.join('/vsimem/', basename + self.ext)
    else:
      dst_file = _temp_files.TempFile(basename, self.ext)
    dst = self.driver.CreateCopy(dst_file, self.src, strict=strict,
                                 options=options)
    self.assertTrue(dst)
    self.assertEqual(dst.GetDriver().ShortName.lower(), self.driver_name)
    # TODO(schwehr): Pre-close tests.
    del dst  # Flush the file.

    self.dst = gdal.Open(dst_file)
    self.assertTrue(self.dst)
    self.assertEqual(self.dst.RasterCount, self.src.RasterCount)
    for band_num in range(1, self.dst.RasterCount + 1):
      src_band = self.src.GetRasterBand(band_num)
      dst_band = self.dst.GetRasterBand(band_num)
      if check_checksums:
        dst_checksum = dst_band.Checksum()
        if checksums:
          self.assertEqual(dst_checksum, checksums[band_num - 1])
        else:
          self.assertEqual(dst_checksum, src_band.Checksum())

        if check_stats:
          dst_stats = dst_band.ComputeRasterMinMax()
          if stats:
            self.assertEqual(dst_stats, stats[band_num - 1])
          else:
            self.assertEqual(dst_stats, src_band.ComputeRasterMinMax())

    if check_geotransform:
      self.CheckGeoTransform(self.dst.GetGeoTransform())
    if check_projection:
      self.CheckProjection(self.dst.GetProjection())

    if metadata:
      result_metadata = self.dst.GetMetadata()
      for key in metadata:
        self.assertEqual(metadata[key], result_metadata[key])

    if remove_result:
      self.dst = None
      self.driver.Delete(dst_file)

    return self.dst

  def CheckCreateCopySimple(self, data):
    """Try to make a copy from vsimem to the format under test in vsimem.

    Args:
      data: Contents of the source tif file to write to vsimem.
    """
    filepath = '/vsimem/create_copy_simple.tif'

    with gcore_util.GdalUnlinkWhenDone(filepath):
      dst = gdal.VSIFOpenL(filepath, 'wb')
      gdal.VSIFWriteL(data, 1, len(data), dst)
      gdal.VSIFCloseL(dst)

      self.CheckOpen(filepath, check_driver=GTIFF_DRIVER)
      self.CheckCreateCopy(vsimem=True, remove_result=True)

  def CheckInfo(self):
    """Use a golden json dump to see if the current read matches.

    May need addition work in the future to keep the checks from being brittle.

    Must call CheckOpen before using this.
    """
    expect = json.load(open(self.filepath + '.json'))
    options = gdal.InfoOptions(
        format='json', computeMinMax=True, stats=True, computeChecksum=True)
    result = gdal.Info(self.src, options=options)
    # Save in case of failure.
    result_json = json.dumps(result)
    basename_json = os.path.basename(self.filepath) + '.json'

    # Some drivers include the version number and a difference is okay as long
    # as the driverShortName is the same, it's okay.
    expect.pop('driverLongName')
    result.pop('driverLongName')

    description_expect = expect.pop('description')
    description_result = result.pop('description')
    self.assertEqual(os.path.basename(description_result), description_expect)

    files_expect = expect.pop('files')
    files_result = result.pop('files')
    self.assertEqual(len(files_result), len(files_expect),
                     '%s versus %s' % (files_result, files_expect))
    for filepath_result, filepath_expect in zip(files_result, files_expect):
      self.assertEqual(os.path.basename(filepath_result), filepath_expect)

    extent_expect_field = _GetExtentField(expect)
    extent_result_field = _GetExtentField(result)

    if not extent_expect_field or extent_expect_field != extent_result_field:
      MaybeWriteOutputFile(basename_json, result_json)
      self.assertEqual(extent_expect_field, extent_result_field, self.filepath)

    extent_expect = expect.pop(extent_expect_field)['coordinates'][0]
    extent_result = result.pop(extent_result_field)['coordinates'][0]

    self.assertEqual(len(extent_result), len(extent_expect))
    for a, b in zip(extent_result, extent_expect):
      self.assertAlmostEqual(a[0], b[0], places=2, msg=self.filepath)

    bands_expect = expect.pop('bands')
    bands_result = result.pop('bands')
    if bands_result != bands_expect:
      MaybeWriteOutputFile(basename_json, result_json)
      self.assertEqual(bands_result, bands_expect, self.filepath)

    srs_wkt_expect = expect.pop('coordinateSystem')['wkt']
    srs_wkt_result = result.pop('coordinateSystem')['wkt']
    if srs_wkt_expect:
      srs_expect = osr.SpatialReference(wkt=str(srs_wkt_expect))
      srs_result = osr.SpatialReference(wkt=str(srs_wkt_result))
      if not srs_expect.IsSame(srs_result):
        MaybeWriteOutputFile(basename_json, result_json)
        self.assertTrue(srs_expect.IsSame(srs_result), self.filepath)

    if result != expect:
      MaybeWriteOutputFile(basename_json, result_json)
      self.assertEqual(result, expect, self.filepath)


def _GetExtentField(json_info):
  """The extent field must be only one of extent or wgs84Extent."""
  has_extent = 'extent' in json_info
  has_wgs84 = 'wgs84Extent' in json_info

  if has_extent and not has_wgs84:
    return 'extent'
  if not has_extent and has_wgs84:
    return 'wgs84Extent'

  return None


def MaybeWriteOutputFile(filename, data):
  """Write a file from a test if allowed."""

  if 'TEST_UNDECLARED_OUTPUTS_DIR' not in os.environ:
    logging.error('Not allowed to write from the test.')
    return

  output_dir = os.environ['TEST_UNDECLARED_OUTPUTS_DIR']
  filepath = os.path.join(output_dir, os.path.basename(filename))
  open(filepath, 'w').write(data)
