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
# Copyright (c) 2008-2013, Even Rouault <even dot rouault at mines-paris.org>
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
###############################################################################
"""Tests for the HDF5 driver.

Format is described here:

https://gdal.org/drivers/raster/hdf5.html

Rewrite of hdf5.py:

https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/hdf5.py
"""

import array
import glob
import os
import shutil
import struct
import unittest

from osgeo import gdal

from autotest2.gdrivers import gdrivers_util


DRIVER = 'hdf5'
EXT = '.h5'
OMI_FILE = 'OMI-Aura_L2-OMTO3_2005m0113t0224-o02648_v002-2005m0625t035355.he5'
SUBDATASET_COMPONENT = '//HDFEOS/SWATHS/OMI_Column_Amount_O3/Data_Fields/UVAerosolIndex'


class Hdf5Test(gdrivers_util.DriverTestCase):

  def setUp(self):  # pytype: disable=signature-mismatch
    super().setUp(DRIVER, EXT)

  def getTestFilePath(self, filename):
    return gdrivers_util.GetTestFilePath(os.path.join(DRIVER, filename))

  def testDriver(self):
    self.CheckDriver()

  def testWithSubdatasets(self):
    filename = 'groups.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(filepath)
    self.CheckShape(512, 512, 0)

    self.assertEqual(self.src.GetMetadataDomainList(), ['', 'SUBDATASETS'])
    expected = {
        'SUBDATASET_1_DESC': '[2x10] //MyGroup/Group_A/dset2 (32-bit integer)',
        'SUBDATASET_1_NAME': 'HDF5:"groups.h5"://MyGroup/Group_A/dset2',
        'SUBDATASET_2_DESC': '[3x3] //MyGroup/dset1 (32-bit integer)',
        'SUBDATASET_2_NAME': 'HDF5:"groups.h5"://MyGroup/dset1'
    }
    subdatasets = self.src.GetMetadata('SUBDATASETS')
    for k in expected:
      self.assertIn(k, subdatasets)
      if 'DESC' in k:
        self.assertEqual(subdatasets[k], expected[k])
      else:
        # The NAMES contain the path, so just check from // to the end.
        self.assertEqual(subdatasets[k].split(':')[-1],
                         expected[k].split(':')[-1])

  def testSubdataset1(self):
    filename = 'groups.h5'
    filepath = f'hdf5:{self.getTestFilePath(filename)}://MyGroup/dset1'
    self.CheckOpen(filepath, check_driver='hdf5image')  # pytype: disable=wrong-arg-types
    self.CheckShape(3, 3, 1)
    self.CheckBand(1, 18, gdal.GDT_Int32, None, 1, 3)

  def testSubdataset2(self):
    filename = 'groups.h5'
    filepath = (
        f'hdf5:{self.getTestFilePath(filename)}://MyGroup/Group_A/dset2')
    self.CheckOpen(filepath, check_driver='hdf5image')  # pytype: disable=wrong-arg-types
    self.CheckShape(10, 2, 1)
    self.CheckBand(1, 110, gdal.GDT_Int32, None, 1, 10)

  def testArray(self):
    filename = 'u8be.h5'
    filepath = f'hdf5:{self.getTestFilePath(filename)}://TestArray'
    self.CheckOpen(filepath, check_driver='hdf5image')  # pytype: disable=wrong-arg-types
    self.CheckShape(5, 6, 1)
    self.CheckBand(1, 135, gdal.GDT_Byte, None, 0, 9)

  def testMetadata(self):
    filename = 'metadata.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(filepath)
    metadata = self.src.GetMetadata()
    key = ('Group_with_spaces_and_underscores_Dataset_with_spaces_and_'
           'underscores_attribute_with_spaces_and_underscores')
    self.assertIn(key, metadata)
    self.assertEqual(metadata[key].strip(), '0.1')

    metadata_list = self.src.GetMetadata_List()
    self.assertEqual(len(metadata), len(metadata_list))
    list_keys = sorted([item.split('=', 1)[0] for item in metadata_list])
    self.assertEqual(sorted(metadata.keys()), list_keys)

  def testGoesFdcfDqf(self):
    # GOES geosyncronous satellite fire detection asset.
    filename = 'OR_ABI-L2-FDCF-M6_G17_s20192011300341_e20192011309408_c20192011309501.nc'
    filepath = f'HDF5:{self.getTestFilePath(filename)}://DQF'
    self.CheckOpen(filepath, False)  # Opens with the hdf5image driver.

    self.CheckShape(5424, 5424, 1)
    self.CheckGeoTransform((0.0, 1.0, 0.0, 0.0, 0.0, 1.0))
    self.CheckProjection('')
    self.CheckBand(1, 46173, gdal.GDT_Int8, None, 0, 5.0)
    metadata = self.src.GetMetadata()
    self.assertLen(metadata, 29)
    self.assertEqual('CF-1.7', metadata['Conventions'])

    band = self.src.GetRasterBand(1)
    metadata = band.GetMetadata_Dict()
    self.assertLen(metadata, 24)
    self.assertEqual('2.9075606453705', metadata['STATISTICS_MEAN'])
    self.assertEqual('6', metadata['number_of_qf_values'])
    self.assertEqual('true', metadata['_Unsigned'])

  def testInfo(self):
    globpath = f'{gdrivers_util.GetTestFilePath(DRIVER)}/*.json'
    for jsonfile in sorted(glob.glob(globpath)):
      filepath = os.path.splitext(jsonfile)[0]
      with self.subTest(input=os.path.basename(filepath)):
        self.CheckOpen(filepath, check_driver=False)  # Driver may be hdf5image.
        self.CheckInfo()

  # Confirm expected subdataset information.
  def test_hdf5_2(self):
    filename = 'groups.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(filepath)
    sds_list = self.src.GetMetadata('SUBDATASETS')
    self.assertLen(sds_list, 4, 'Did not get expected subdataset count.')
    self.assertEqual(
        sds_list['SUBDATASET_1_NAME'],
        f'HDF5:"{filepath}"://MyGroup/Group_A/dset2',
        'Did not get expected SUBDATASET_1_NAME.',
    )
    self.assertEqual(
        sds_list['SUBDATASET_2_NAME'],
        f'HDF5:"{filepath}"://MyGroup/dset1',
        'Did not get expected SUBDATASET_2_NAME.',
    )

  # Confirm that single variable files can be accessed directly without
  # subdataset stuff.
  def test_hdf5_3(self):
    filename = 'u8be.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(
        f'HDF5:"{filepath}"://TestArray', check_driver=False
    )
    self.CheckBand(1, 135)

  # Confirm subdataset access, and checksum.
  def test_hdf5_4(self):
    filename = 'u8be.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(
        f'HDF5:"{filepath}"://TestArray', check_driver=False
    )
    self.CheckBand(1, 135)

  # Similar check on a 16bit dataset.
  def test_hdf5_5(self):
    filename = 'groups.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(
        f'HDF5:"{filepath}"://MyGroup/dset1', check_driver=False
    )
    self.CheckBand(1, 18)

  # Test generating an overview on a subdataset.
  def test_hdf5_6(self):
    filename = 'groups.h5'
    filepath = self.getTestFilePath(filename)
    tmpdir = self.create_tempdir()
    tmpdir.create_file(filename)
    tmp_filepath = f'{tmpdir.full_path}/{os.path.basename(filename)}'
    shutil.copyfile(filepath, tmp_filepath)

    with gdal.Open(f'HDF5:"{tmp_filepath}"://MyGroup/dset1') as ds:
      ds.BuildOverviews(overviewlist=[2])
      self.assertEqual(
          ds.GetRasterBand(1).GetOverviewCount(),
          1,
          'failed to find overview',
      )

    # Test an alternate path to the same file.
    with gdal.Open(
        f'HDF5:"{tmpdir.full_path}/../{os.path.basename(tmpdir.full_path)}/groups.h5"://MyGroup/dset1'
    ) as ds:
      ds.BuildOverviews(overviewlist=[2])
      self.assertEqual(
          ds.GetRasterBand(1).GetOverviewCount(),
          1,
          'failed to find overview with alternate path',
      )
      ovfile = ds.GetMetadataItem('OVERVIEW_FILE', 'OVERVIEWS')
      self.assertEqual(
          ovfile[:11], tmp_filepath[:11], 'did not get expected OVERVIEW_FILE.'
      )
      os.remove(tmp_filepath)

  # Coarse metadata check.
  def test_hdf5_7(self):
    filename = 'metadata.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      metadata = ds.GetMetadata()
      metadata_list = ds.GetMetadata_List()

      self.assertLen(metadata, len(metadata_list))

      metadata_list = [item.split('=', 1)[0] for item in metadata_list]
      for key in metadata_list:
        try:
          metadata.pop(key)
        except KeyError:
          self.fail('unable to find "%s" key' % key)

  # Test metadata names.
  def test_hdf5_8(self):
    filename = 'metadata.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      metadata = ds.GetMetadata()
      self.assertTrue(metadata)

    h5groups = [
        'G1',
        'Group with spaces',
        'Group_with_underscores',
        'Group with spaces_and_underscores',
    ]
    h5datasets = [
        'D1',
        'Dataset with spaces',
        'Dataset_with_underscores',
        'Dataset with spaces_and_underscores',
    ]
    attributes = {
        'attribute': 'value',
        'attribute with spaces': 0,
        'attribute_with underscores': 0,
        'attribute with spaces_and_underscores': 0.1,
    }

    def scan_metadata(parts):
      for attr in attributes:
        name = '_'.join(parts + [attr])
        name = name.replace(' ', '_')
        self.assertIn(name, metadata, f'unable to find metadata: "{name}"')

        value = metadata.pop(name)

        value = value.strip(' d')
        value = type(attributes[attr])(value)
        self.assertEqual(
            value,
            attributes[attr],
            f'incorrect metadata value for "{name}": "{value}" !='
            f' "{attributes[attr]}"',
        )

    # level0
    self.assertIsNone(scan_metadata([]))

    # level1 datasets
    for h5dataset in h5datasets:
      self.assertIsNone(scan_metadata([h5dataset]))

    # level1 groups
    for h5group in h5groups:
      self.assertIsNone(scan_metadata([h5group]))

      # level2 datasets
      for h5dataset in h5datasets:
        self.assertIsNone(scan_metadata([h5group, h5dataset]))

  # Variable length string metadata check (regression test for #4228).
  def test_hdf5_9(self):
    if int(gdal.VersionInfo('VERSION_NUM')) < 1900:
      self.skipTest('would crash')

    filename = 'vlstr_metadata.h5'
    filepath = self.getTestFilePath(filename)

    with gdal.Open(filepath) as ds:
      metadata = ds.GetRasterBand(1).GetMetadata()

      ref_metadata = {
          'BANDNAMES': 'SAA',
          'CODING': '0.6666666667 0.0000000000 TRUE',
          'FLAGS': '255=noValue',
          'MAPPING': (
              'Geographic Lat/Lon 0.5000000000 0.5000000000 27.3154761905'
              ' -5.0833333333 0.0029761905 0.0029761905 WGS84 Degrees'
          ),
          'NOVALUE': '255',
          'RANGE': '0 255 0 255',
      }

      self.assertLen(metadata, len(ref_metadata))

      for key in metadata:
        self.assertIn(key, ref_metadata, f'unexpected metadata key "{key}"')

        self.assertEqual(metadata[key], ref_metadata[key])

  # Test CSK_DGM.h5
  def test_hdf5_10(self):
    filename = 'CSK_DGM.h5'
    filepath = self.getTestFilePath(filename)

    # Try opening the QLK subdataset to check that no error is generated
    gdal.ErrorReset()
    with gdal.Open(f'HDF5:"{filepath}"://S01/QLK') as ds:
      self.assertIsNotNone(ds)
      self.assertFalse(gdal.GetLastErrorMsg())

    with gdal.Open(f'HDF5:"{filepath}"://S01/SBI') as ds:
      start = 'GEOGCS["WGS 84",DATUM["WGS_1984"'
      self.assertIn(start, ds.GetGCPProjection())

      got_gcps = ds.GetGCPs()
      self.assertLen(got_gcps, 4)

      self.assertAlmostEqual(got_gcps[0].GCPPixel, 0, delta=1e-5)
      self.assertAlmostEqual(got_gcps[0].GCPLine, 0, delta=1e-5)
      self.assertAlmostEqual(got_gcps[0].GCPX, 12.2395902, delta=1e-5)
      self.assertAlmostEqual(got_gcps[0].GCPY, 44.7280047, delta=1e-5)

  # Test CSK_GEC.h5
  def test_hdf5_11(self):
    filename = 'CSK_GEC.h5'
    filepath = self.getTestFilePath(filename)

    # Try opening the QLK subdataset to check that no error is generated
    gdal.ErrorReset()
    with gdal.Open(f'HDF5:"{filepath}"://S01/QLK') as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(gdal.GetLastErrorMsg(), '')

    with gdal.Open(f'HDF5:"{filepath}"://S01/SBI') as ds:
      got_projection = ds.GetProjection()
      self.assertTrue(
          got_projection.startswith(
              'PROJCS["Transverse_Mercator",GEOGCS["WGS 84",DATUM["WGS_1984"'
          )
      )

      got_gt = ds.GetGeoTransform()
      expected_gt = (275592.5, 2.5, 0.0, 4998152.5, 0.0, -2.5)
      for i in range(6):
        self.assertAlmostEqual(got_gt[i], expected_gt[i], delta=1e-5)

  # TODO: b/430037021 - test_hdf5_12 (Test ODIM_H5 - Skipped, requires
  # download)
  # TODO: b/430037021 - test_hdf5_13 (Test MODIS L2 HDF5 GCPs - Skipped,
  # requires download)

  # Test complex data subsets
  def test_hdf5_14(self):
    filename = 'complex.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      sds_list = ds.GetMetadata('SUBDATASETS')

      self.assertLen(
          sds_list, 6, 'Did not get expected complex subdataset count.'
      )

      self.assertEqual(
          sds_list['SUBDATASET_1_NAME'], f'HDF5:"{filepath}"://f16'
      )
      self.assertEqual(
          sds_list['SUBDATASET_2_NAME'], f'HDF5:"{filepath}"://f32'
      )
      self.assertEqual(
          sds_list['SUBDATASET_3_NAME'], f'HDF5:"{filepath}"://f64'
      )

  # Confirm complex subset data access and checksum - Float32
  def test_hdf5_15(self):
    filename = 'complex.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(f'HDF5:"{filepath}"://f32', check_driver=False)
    self.CheckBand(1, 523)

  # Repeat for Float64
  def test_hdf5_16(self):
    filename = 'complex.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(f'HDF5:"{filepath}"://f64', check_driver=False)
    self.CheckBand(1, 511)

  # Repeat for Float16
  def test_hdf5_17(self):
    filename = 'complex.h5'
    filepath = self.getTestFilePath(filename)
    self.CheckOpen(f'HDF5:"{filepath}"://f16', check_driver=False)
    self.CheckBand(1, 412)

  def test_hdf5_single_char_varname(self):
    filename = 'single_char_varname.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(f'HDF5:"{filepath}"://e') as ds:
      self.assertIsNotNone(ds)

  def test_hdf5_attr_all_datatypes(self):
    filename = 'attr_all_datatypes.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(
          ds.GetMetadata(),
          {
              'attr_float16': '125',
              'attr_float32': '125',
              'attr_float64': '125',
              'attr_int16': '125',
              'attr_int32': '125',
              'attr_int8': '125',
              'attr_uint16': '125',
              'attr_uint32': '125',
              'attr_uint8': '125',
          },
      )

  # TODO: b/430037021 - test_hdf5_virtual_file - uffd_compare not available
  # TODO: b/430037021 - test_hdf5 - requires download from external source

  def test_hdf5_dimension_labels_with_null(self):
    filename = 'dimension_labels_with_null.h5'
    filepath = self.getTestFilePath(filename)
    self.assertIsNotNone(gdal.Open(filepath))

  def test_hdf5_recursive_groups(self):
    filename = 'recursive_groups.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertIsNotNone(ds)
      ds.GetSubDatasets()

  def test_hdf5_family_driver(self):
    filename = 'test_family_0.h5'
    filepath = self.getTestFilePath(filename)
    self.assertIsNotNone(gdal.Open(filepath))

  def test_hdf5_single_dim(self):
    filepath = gdrivers_util.GetTestFilePath('netcdf/byte_chunked_multiple.nc')
    with gdal.Open(f'HDF5:{filepath}://x') as ds:
      self.assertIsNotNone(ds)
      b = ds.GetRasterBand(1)
      self.assertEqual(b.YSize, 1)
      self.assertEqual(b.XSize, 20)
      self.assertEqual(b.GetBlockSize(), [20, 1])
      self.assertEqual(b.Checksum(), 231)

  # Test opening a file whose HDF5 signature is not at the beginning
  def test_hdf5_signature_not_at_beginning(self):
    filename = '/vsimem/test.h5'
    netcdf_filepath = gdrivers_util.GetTestFilePath(
        'netcdf/byte_hdf5_starting_at_offset_1024.nc'
    )
    gdal.FileFromMemBuffer(
        filename, open(netcdf_filepath, 'rb').read()
    )
    ds = gdal.Open(filename)
    self.assertIsNotNone(ds)
    gdal.Unlink(filename)

  # Test RasterIO() optimizations
  def test_hdf5_rasterio_optims(self):
    # Band-interleaved data
    filename = 'dummy_HDFEOS_swath.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(
        f'HDF5:"{filepath}"://HDFEOS/SWATHS/MySwath/Data_Fields/MyDataField'
    ) as ds:
      expected = array.array('B', range(2 * 3 * 4)).tobytes()
      self.assertEqual(ds.ReadRaster(), expected)
      self.assertEqual(
          ds.GetRasterBand(1).ReadRaster() + ds.GetRasterBand(2).ReadRaster(),
          expected,
      )

      # optimization through intermediate MEMDataset: non-natural interleaving
      self.assertEqual(
          ds.ReadRaster(buf_pixel_space=ds.RasterCount, buf_band_space=1),
          array.array(
              'B',
              [
                  0,
                  12,
                  1,
                  13,
                  2,
                  14,
                  3,
                  15,
                  4,
                  16,
                  5,
                  17,
                  6,
                  18,
                  7,
                  19,
                  8,
                  20,
                  9,
                  21,
                  10,
                  22,
                  11,
                  23,
              ],
          ).tobytes(),
      )

      # optimization through intermediate MEMDataset: non-natural data type
      expected = array.array('H', range(2 * 3 * 4)).tobytes()
      self.assertEqual(ds.ReadRaster(buf_type=gdal.GDT_UInt16), expected)
      self.assertEqual(
          ds.GetRasterBand(1).ReadRaster(buf_type=gdal.GDT_UInt16)
          + ds.GetRasterBand(2).ReadRaster(buf_type=gdal.GDT_UInt16),
          expected,
      )

      # non-optimized: out of order bands
      self.assertEqual(
          ds.ReadRaster(band_list=[2, 1]),
          ds.GetRasterBand(2).ReadRaster() + ds.GetRasterBand(1).ReadRaster(),
      )

      # non-optimized: resampling
      self.assertEqual(
          ds.GetRasterBand(1).ReadRaster(3, 2, 1, 1, buf_xsize=2, buf_ysize=2),
          b'\x0b' * 4,
      )

    # Pixel-interleaved data
    filename = 'dummy_HDFEOS_with_sinu_projection.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertEqual(
          ds.ReadRaster(buf_pixel_space=ds.RasterCount, buf_band_space=1),
          array.array('B', range(5 * 4 * 3)).tobytes(),
      )

      # optimization through intermediate MEMDataset: non-natural interleaving
      self.assertEqual(
          ds.ReadRaster(),
          array.array(
              'B',
              [
                  0,
                  3,
                  6,
                  9,
                  12,
                  15,
                  18,
                  21,
                  24,
                  27,
                  30,
                  33,
                  36,
                  39,
                  42,
                  45,
                  48,
                  51,
                  54,
                  57,
                  1,
                  4,
                  7,
                  10,
                  13,
                  16,
                  19,
                  22,
                  25,
                  28,
                  31,
                  34,
                  37,
                  40,
                  43,
                  46,
                  49,
                  52,
                  55,
                  58,
                  2,
                  5,
                  8,
                  11,
                  14,
                  17,
                  20,
                  23,
                  26,
                  29,
                  32,
                  35,
                  38,
                  41,
                  44,
                  47,
                  50,
                  53,
                  56,
                  59,
              ],
          ).tobytes(),
      )

  # Test opening a HDF5EOS grid file
  def test_hdf5_eos_grid_sinu_projection(self):
    filename = 'dummy_HDFEOS_with_sinu_projection.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.RasterXSize, 4)
      self.assertEqual(ds.RasterYSize, 5)
      self.assertEqual(ds.RasterCount, 3)
      expected_gt = (
          -1111950.519667,
          277987.62991675,
          0.0,
          5559752.598333,
          0.0,
          -222390.10393320007,
      )
      got_gt = ds.GetGeoTransform()
      for i in range(6):
        self.assertAlmostEqual(got_gt[i], expected_gt[i], delta=1e-5)

      self.assertIn(
          '+proj=sinu +lon_0=0 +x_0=0 +y_0=0 +R=6371007.181 +units=m +no_defs',
          ds.GetSpatialRef().ExportToProj4(),
      )
      self.assertEqual(
          list(
              struct.unpack(
                  'B' * (5 * 4 * 3),
                  ds.ReadRaster(buf_pixel_space=3, buf_band_space=1),
              )
          ),
          list(range(5 * 4 * 3)),
      )

  # Test opening a HDF5EOS grid file
  def test_hdf5_eos_grid_utm_projection(self):
    filename = 'dummy_HDFEOS_with_utm_projection.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.RasterXSize, 20)
      self.assertEqual(ds.RasterYSize, 20)
      self.assertEqual(ds.RasterCount, 1)
      expected_gt = (440720, 60, 0, 3751320, 0, -60)
      got_gt = ds.GetGeoTransform()
      for i in range(6):
        self.assertAlmostEqual(got_gt[i], expected_gt[i], delta=1e-5)
      self.assertEqual(ds.GetSpatialRef().GetUTMZone(), 11)
      # WGS 84
      self.assertAlmostEqual(
          ds.GetSpatialRef().GetSemiMajor(), 6378137, delta=1e-5
      )
      self.assertAlmostEqual(
          ds.GetSpatialRef().GetInvFlattening(), 298.257223563, delta=1e-5
      )

  # Test opening a HDF5EOS grid file
  def test_hdf5_eos_grid_utm_projection_empty_GROUP_dimension(self):
    filename = 'dummy_HDFEOS_with_utm_projection_empty_GROUP_dimension.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.RasterXSize, 10)
      self.assertEqual(ds.RasterYSize, 20)
      self.assertEqual(ds.RasterCount, 1)
      expected_gt = (440720, 120, 0, 3751320, 0, -60)
      got_gt = ds.GetGeoTransform()
      for i in range(6):
        self.assertAlmostEqual(got_gt[i], expected_gt[i], delta=1e-5)
      self.assertEqual(ds.GetSpatialRef().GetUTMZone(), 11)
      # WGS 84
      self.assertAlmostEqual(
          ds.GetSpatialRef().GetSemiMajor(), 6378137, delta=1e-1
      )
      self.assertAlmostEqual(
          ds.GetSpatialRef().GetInvFlattening(), 298.257, delta=1e-2
      )

  # Test opening a HDF5EOS grid file
  def test_hdf5_eos_grid_geo_projection(self):
    filename = 'dummy_HDFEOS_with_geo_projection.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertIsNotNone(ds)
      self.assertEqual(ds.RasterXSize, 20)
      self.assertEqual(ds.RasterYSize, 20)
      self.assertEqual(ds.RasterCount, 1)
      expected_gt = (
          -117.64116944444262,
          0.0006529166665345087,
          0.0,
          33.900602777778275,
          0.0,
          -0.00053777777781292,
      )
      got_gt = ds.GetGeoTransform()
      for i in range(6):
        self.assertAlmostEqual(got_gt[i], expected_gt[i], delta=1e-5)
      self.assertTrue(ds.GetSpatialRef().IsGeographic())
      # WGS 84
      self.assertAlmostEqual(
          ds.GetSpatialRef().GetSemiMajor(), 6378137, delta=1e-5
      )
      self.assertAlmostEqual(
          ds.GetSpatialRef().GetInvFlattening(), 298.257223563, delta=1e-5
      )

  # Test opening a HDF5EOS swatch file (the Swath.h5 file generated by
  # 'make check' on the hdfeos5 library)
  def test_hdf5_eos_swath_with_explicit_dimension_map(self):
    filename = 'hdfeos_sample_swath.h5'
    filepath = self.getTestFilePath(filename)

    # Using the DimensionMap
    # (the data field has more samples than the geo fields)
    with gdal.Open(
        f'HDF5:"{filepath}"://HDFEOS/SWATHS/Swath1/Data_Fields/Spectra'
    ) as ds:
      self.assertEqual(ds.RasterXSize, 20)
      self.assertEqual(ds.RasterYSize, 40)
      self.assertEqual(ds.RasterCount, 15)
      expected_meta = {
          'LINE_OFFSET': '0',
          'LINE_STEP': '2',
          'PIXEL_OFFSET': '1',
          'PIXEL_STEP': '2',
          'SRS': (
              'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],'
              'AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,'
              'AUTHORITY["EPSG","8901"]],UNIT["degree",'
              '0.0174532925199433,AUTHORITY["EPSG","9122"]],'
              'AXIS["Latitude",NORTH],AXIS["Longitude",EAST],'
              'AUTHORITY["EPSG","4326"]]'
          ),
          'X_BAND': '1',
          'X_DATASET': (
              f'HDF5:"{filepath}"://HDFEOS/SWATHS/Swath1/Geolocation_Fields/Longitude'
          ),
          'Y_BAND': '1',
          'Y_DATASET': (
              f'HDF5:"{filepath}"://HDFEOS/SWATHS/Swath1/Geolocation_Fields/Latitude'
          ),
          'GEOREFERENCING_CONVENTION': 'PIXEL_CENTER',
      }
      self.assertEqual(ds.GetMetadata('GEOLOCATION'), expected_meta)
      self.assertIsNotNone(
          gdal.Open(ds.GetMetadataItem('X_DATASET', 'GEOLOCATION'))
      )
      self.assertIsNotNone(
          gdal.Open(ds.GetMetadataItem('Y_DATASET', 'GEOLOCATION'))
      )

    # Not using the DimensionMap
    # (the data field uses the same dimensions as the geo fields)
    with gdal.Open(
        f'HDF5:"{filepath}"://HDFEOS/SWATHS/Swath1/Data_Fields/Temperature'
    ) as ds:
      self.assertEqual(ds.RasterXSize, 10)
      self.assertEqual(ds.RasterYSize, 20)
      self.assertEqual(ds.RasterCount, 1)
      expected_meta = {
          'LINE_OFFSET': '0',
          'LINE_STEP': '1',
          'PIXEL_OFFSET': '0',
          'PIXEL_STEP': '1',
          'SRS': (
              'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS'
              ' 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],'
              'AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,'
              'AUTHORITY["EPSG","8901"]],UNIT["degree",'
              '0.0174532925199433,AUTHORITY["EPSG","9122"]],'
              'AXIS["Latitude",NORTH],AXIS["Longitude",EAST],'
              'AUTHORITY["EPSG","4326"]]'
          ),
          'X_BAND': '1',
          'X_DATASET': (
              f'HDF5:"{filepath}"://HDFEOS/SWATHS/Swath1/Geolocation_Fields/'
              'Longitude'
          ),
          'Y_BAND': '1',
          'Y_DATASET': (
              f'HDF5:"{filepath}"://HDFEOS/SWATHS/Swath1/Geolocation_Fields/'
              'Latitude'
          ),
          'GEOREFERENCING_CONVENTION': 'PIXEL_CENTER',
      }
      self.assertEqual(ds.GetMetadata('GEOLOCATION'), expected_meta)
      self.assertIsNotNone(
          gdal.Open(ds.GetMetadataItem('X_DATASET', 'GEOLOCATION'))
      )
      self.assertIsNotNone(
          gdal.Open(ds.GetMetadataItem('Y_DATASET', 'GEOLOCATION'))
      )

  # Test opening a HDF5EOS swath file
  def test_hdf5_eos_swath_no_explicit_dimension_map(self):
    filename = 'dummy_HDFEOS_swath.h5'
    filepath = self.getTestFilePath(filename)

    with gdal.Open(filepath) as ds:
      subds = ds.GetSubDatasets()
      self.assertEqual(
          subds[0][0],
          f'HDF5:"{filepath}"://HDFEOS/SWATHS/MySwath/Data_Fields/MyDataField',
      )
      self.assertTrue(
          subds[0][1].startswith(
              '[(Band=2)x(AlongTrack=3)x(CrossTrack=4)]'
              ' //HDFEOS/SWATHS/MySwath/Data_Fields/MyDataField (8-bit'
          )
      )

      with gdal.Open(subds[0][0]) as ds2:
        self.assertEqual(ds2.RasterXSize, 4)
        self.assertEqual(ds2.RasterYSize, 3)
        self.assertEqual(ds2.RasterCount, 2)
        self.assertEmpty(ds2.GetGCPs())
        expected_meta = {
            'GEOREFERENCING_CONVENTION': 'PIXEL_CENTER',
            'LINE_OFFSET': '0',
            'LINE_STEP': '1',
            'PIXEL_OFFSET': '0',
            'PIXEL_STEP': '1',
            'SRS': (
                'GEOGCS["WGS 84",DATUM["WGS_1984",SPHEROID["WGS'
                ' 84",6378137,298.257223563,AUTHORITY["EPSG","7030"]],'
                'AUTHORITY["EPSG","6326"]],PRIMEM["Greenwich",0,'
                'AUTHORITY["EPSG","8901"]],UNIT["degree",'
                '0.0174532925199433,AUTHORITY["EPSG","9122"]],'
                'AXIS["Latitude",NORTH],AXIS["Longitude",EAST],'
                'AUTHORITY["EPSG","4326"]]'
            ),
            'X_BAND': '1',
            'X_DATASET': (
                f'HDF5:"{filepath}"://HDFEOS/SWATHS/MySwath/Geolocation_Fields/'
                'Longitude'
            ),
            'Y_BAND': '1',
            'Y_DATASET': (
                f'HDF5:"{filepath}"://HDFEOS/SWATHS/MySwath/Geolocation_Fields/'
                'Latitude'
            ),
        }
        self.assertEqual(ds2.GetMetadata('GEOLOCATION'), expected_meta)
        self.assertIsNotNone(
            gdal.Open(ds2.GetMetadataItem('X_DATASET', 'GEOLOCATION'))
        )
        self.assertIsNotNone(
            gdal.Open(ds2.GetMetadataItem('Y_DATASET', 'GEOLOCATION'))
        )

  # Test opening a file with band specific attributes
  def test_hdf5_band_specific_attribute(self):
    filename = 'fwhm.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      self.assertEqual(ds.RasterXSize, 4)
      self.assertEqual(ds.RasterYSize, 3)
      self.assertEqual(ds.RasterCount, 2)
      self.assertEqual(
          ds.GetRasterBand(1).GetMetadata_Dict(),
          {
              'fwhm': '0.01',
              'fwhm_units': 'Micrometers',
              'bad_band': '0',
              'center_wavelength': '300',
              'my_coefficient': '1',
          },
      )
      self.assertEqual(
          ds.GetRasterBand(2).GetMetadata_Dict(),
          {
              'fwhm': '0.02',
              'fwhm_units': 'Micrometers',
              'bad_band': '1',
              'center_wavelength': '400',
              'my_coefficient': '2',
          },
      )

  # Test gdal subdataset informational functions
  def test_gdal_subdataset_get_filename(self):
    test_cases = (
        (
            f'HDF5:"{OMI_FILE}":{SUBDATASET_COMPONENT}',
            f'{OMI_FILE}',
        ),
        (
            f'HDF5:{OMI_FILE}:{SUBDATASET_COMPONENT}',
            f'{OMI_FILE}',
        ),
        (
            fr'HDF5:"C:\{OMI_FILE}":{SUBDATASET_COMPONENT}',
            fr'C:\{OMI_FILE}',
        ),
        (
            fr'HDF5:"C:/{OMI_FILE}":{SUBDATASET_COMPONENT}',
            fr'C:/{OMI_FILE}',
        ),
        (
            fr'HDF5:"/vsicurl/http://www.my.com/{OMI_FILE}":{SUBDATASET_COMPONENT}',
            fr'/vsicurl/http://www.my.com/{OMI_FILE}',
        ),
        (
            fr'HDF5:a:{SUBDATASET_COMPONENT}',
            r'a',
        ),
        (
            fr'HDF5:a:/my/path:{SUBDATASET_COMPONENT}',
            r'a:/my/path',
        ),
        ('', ''),
    )
    for filename, path_component in test_cases:
      with self.subTest(filename=filename):
        info = gdal.GetSubdatasetInfo(filename)
        if not filename:
          self.assertIsNone(info)
        else:
          self.assertEqual(info.GetPathComponent(), path_component)
          self.assertEqual(
              info.GetSubdatasetComponent(),
              f'{SUBDATASET_COMPONENT}',
          )

  def test_gdal_subdataset_modify_filename(self):
    test_cases = (
        f'HDF5:"{OMI_FILE}":{SUBDATASET_COMPONENT}',
        fr'HDF5:"C:\{OMI_FILE}":{SUBDATASET_COMPONENT}',
        '',
    )
    for filename in test_cases:
      with self.subTest(filename=filename):
        info = gdal.GetSubdatasetInfo(filename)
        if not filename:
          self.assertIsNone(info)
        else:
          self.assertEqual(
              info.ModifyPathComponent('"/path/to.he5"'),
              f'HDF5:"/path/to.he5":{SUBDATASET_COMPONENT}',
          )

  def test_gdal_subdataset_bogus(self):
    """Test it doesn't crash."""
    bogus_cases = (
        'HDF5:a:c',
        'HDF5:a',
        'HDF5:',
    )
    for bogus in bogus_cases:
      with self.subTest(bogus=bogus):
        gdal.GetSubdatasetInfo(bogus)

  # Test opening a chunked HDF5EOS swath file
  def test_hdf5_eos_swath_chunking_optimization(self):
    filename = 'dummy_HDFEOS_swath_chunked.h5'
    filepath = self.getTestFilePath(filename)

    with gdal.Open(
        f'HDF5:"{filepath}"://HDFEOS/SWATHS/MySwath/Data_Fields/MyDataField'
    ) as ds:
      mem_ds = gdal.Translate('', ds, format='MEM')

      ds.GetRasterBand(1).ReadRaster(0, 0, 1, 1)
      self.assertEqual(
          ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'), 'DISABLED'
      )

      ds.GetRasterBand(1).ReadRaster(0, 0, ds.RasterXSize, 1)
      self.assertEqual(
          ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'),
          'DETECTION_IN_PROGRESS',
      )
      ds.GetRasterBand(1).ReadRaster(0, 2, ds.RasterXSize, 1)
      self.assertEqual(
          ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'), 'DISABLED'
      )

      ds.GetRasterBand(1).ReadRaster(0, 0, ds.RasterXSize, 1)
      self.assertEqual(
          ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'),
          'DETECTION_IN_PROGRESS',
      )
      ds.GetRasterBand(2).ReadRaster(0, 0, ds.RasterXSize, 1)
      self.assertEqual(
          ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'), 'DISABLED'
      )

    with gdal.Open(
        f'HDF5:"{filepath}"://HDFEOS/SWATHS/MySwath/Data_Fields/MyDataField'
    ) as ds:
      mem_ds = gdal.Translate('', ds, format='MEM')
      self.assertEqual(
          ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'),
          'DETECTION_IN_PROGRESS',
      )
      for i in range(ds.RasterCount):
        self.assertEqual(
            ds.GetRasterBand(i + 1).Checksum(),
            mem_ds.GetRasterBand(i + 1).Checksum(),
        )
        self.assertEqual(
            ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'),
            'DETECTION_IN_PROGRESS' if i == 0 else 'ENABLED',
        )
      for i in range(ds.RasterCount):
        self.assertEqual(
            ds.GetRasterBand(i + 1).ReadRaster(),
            mem_ds.GetRasterBand(i + 1).ReadRaster(),
        )
        self.assertEqual(
            ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'), 'ENABLED'
        )
        self.assertEqual(
            ds.GetRasterBand(i + 1).ReadRaster(1, 2, 3, 4),
            mem_ds.GetRasterBand(i + 1).ReadRaster(1, 2, 3, 4),
        )
        self.assertEqual(
            ds.GetMetadataItem('WholeBandChunkOptim', '__DEBUG__'), 'ENABLED'
        )

      blockxsize, blockysize = ds.GetRasterBand(1).GetBlockSize()
      self.assertEqual(
          ds.GetRasterBand(1).ReadBlock(1, 2),
          mem_ds.GetRasterBand(1).ReadRaster(
              1 * blockxsize, 2 * blockysize, blockxsize, blockysize
          ),
      )

  # Test GetNoDataValue(), GetOffset(), GetScale()
  def test_hdf5_read_netcdf_nodata_scale_offset(self):
    filename = 'scale_offset.h5'
    filepath = self.getTestFilePath(filename)
    with gdal.Open(filepath) as ds:
      band = ds.GetRasterBand(1)
      self.assertAlmostEqual(band.GetNoDataValue(), 9.96921e36, delta=1e30)
      self.assertEqual(band.GetOffset(), 1.5)
      self.assertEqual(band.GetScale(), 0.01)

  # Test force opening a netCDF file with HDF5 driver
  def test_hdf5_force_opening_netcdf_file(self):
    filepath = gdrivers_util.GetTestFilePath('netcdf/trmm-nc4.nc')
    with gdal.OpenEx(filepath, allowed_drivers=['HDF5']) as ds:
      self.assertEqual(ds.GetDriver().GetDescription(), 'HDF5Image')

    filepath = gdrivers_util.GetTestFilePath(
        'netcdf/byte_hdf5_starting_at_offset_1024.nc'
    )
    with gdal.OpenEx(filepath, allowed_drivers=['HDF5']) as ds:
      self.assertEqual(ds.GetDriver().GetDescription(), 'HDF5Image')

  # Test force opening, but provided file is still not recognized
  # (for good reasons)
  def test_hdf5_force_opening_no_match(self):
    filepath = gdrivers_util.GetTestFilePath('byte.tif')
    drv = gdal.IdentifyDriverEx(filepath, allowed_drivers=['HDF5'])
    self.assertIsNone(drv)

  def test_hdf5_open_larger_than_INT_MAX_pixels(self):
    gdal.UseExceptions()
    filepath = gdrivers_util.GetTestFilePath(
        'bag/larger_than_INT_MAX_pixels.bag'
    )

    with gdal.OpenEx(filepath, allowed_drivers=['HDF5']) as ds:
      self.assertEmpty(ds.GetSubDatasets())

    with self.assertRaisesRegex(
        Exception, 'At least one dimension size exceeds INT_MAX'
    ):
      gdal.Open(f'HDF5:"{filepath}"://BAG_root/elevation')
  # TODO: b/430037021 - check for more tests in upstream


if __name__ == '__main__':
  unittest.main()
