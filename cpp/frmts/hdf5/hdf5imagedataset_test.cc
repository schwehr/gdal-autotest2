// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests the HDF5Image subdataset raster driver (hdf5imagedataset.cpp).
//
// See also:
//   https://gdal.org/drivers/raster/hdf5.html
//   https://github.com/OSGeo/gdal/blob/master/frmts/hdf5/hdf5imagedataset.cpp
//   https://github.com/OSGeo/gdal/blob/master/autotest/gdrivers/hdf5.py

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>  // NOLINT
#include <memory>
#include <optional>
#include <span>  // NOLINT
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf5/hdf5_test_util.h"
#include "third_party/gdal/frmts/hdf5/hdf5_api.h"
#include "third_party/gdal/frmts/hdf5/hdf5drivercore.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/hdf5/src/H5Ipublic.h"
#include "third_party/hdf5/src/H5Spublic.h"
#include "third_party/hdf5/src/H5Tpublic.h"
#include "third_party/hdf5/src/H5public.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;

struct ComplexFloat32 {
  float r;
  float i;
};

#ifdef HDF5_HAVE_FLOAT16
struct ComplexFloat16 {
  _Float16 r;
  _Float16 i;
};
#endif

void Write1DVarStringAttr(hid_t loc_id, const char* name,
                          std::span<const char* const> vals) {
  const hsize_t n = vals.size();
  ScopedH5Dataspace sp(H5Screate_simple(1, &n, nullptr));
  ASSERT_GE(sp.get(), 0);
  ScopedH5Datatype vstr_t(H5Tcopy(H5T_C_S1));
  ASSERT_GE(H5Tset_size(vstr_t, H5T_VARIABLE), 0);
  ScopedH5Attribute attr(H5Acreate(loc_id, name, vstr_t, sp, H5P_DEFAULT));
  ASSERT_GE(attr.get(), 0);
  ASSERT_GE(H5Awrite(attr, vstr_t, vals.data()), 0);
}

std::unique_ptr<GDALDataset> OpenImageDataset(
    const std::string& uri,
    unsigned int flags = GDAL_OF_READONLY | GDAL_OF_RASTER) {
  return std::unique_ptr<GDALDataset>(GDALDataset::Open(uri.c_str(), flags));
}

TEST(Hdf5ImageDatasetTest, DriverRegistrationIdentifyAndErrorCases) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;

  // 1. Register HDF5Image driver idempotently.
  GDALRegister_HDF5Image();
  GDALDriver* drv =
      GDALDriver::FromHandle(GDALGetDriverByName(HDF5_IMAGE_DRIVER_NAME));
  ASSERT_NE(nullptr, drv);
  GDALRegister_HDF5Image();

  // 2. Non-HDF5: filenames fail Identify immediately.
  {
    GDALOpenInfo open_info("/does_not_exist.h5", GDAL_OF_READONLY);
    EXPECT_EQ(FALSE, HDF5ImageDatasetIdentify(&open_info));
    EXPECT_EQ(nullptr, OpenImageDataset("/does_not_exist.h5"));
  }

  // Build a minimal in-memory HDF5 file with valid, 0D scalar, and string
  // datasets for error-path testing.
  ScopedVsimemFile file("errors.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(file.path()));
    ASSERT_GE(fid.get(), 0);
    const hsize_t dims[2] = {4, 4};
    ScopedH5Dataspace sp2d(H5Screate_simple(2, dims, nullptr));
    ASSERT_GE(sp2d.get(), 0);
    ScopedH5Dataset d2d(
        H5Dcreate(fid, "band2d", H5T_NATIVE_INT16, sp2d, H5P_DEFAULT));
    ASSERT_GE(d2d.get(), 0);

    ScopedH5Dataspace sp0d(H5Screate(H5S_SCALAR));
    ASSERT_GE(sp0d.get(), 0);
    ScopedH5Dataset d0d(
        H5Dcreate(fid, "scalar0d", H5T_NATIVE_INT16, sp0d, H5P_DEFAULT));
    ASSERT_GE(d0d.get(), 0);

    ScopedH5Dataspace sp_str(H5Screate_simple(2, dims, nullptr));
    ASSERT_GE(sp_str.get(), 0);
    ScopedH5Datatype str_t(H5Tcopy(H5T_C_S1));
    ASSERT_GE(H5Tset_size(str_t, 4), 0);
    ScopedH5Dataset dstr(H5Dcreate(fid, "str2d", str_t, sp_str, H5P_DEFAULT));
    ASSERT_GE(dstr.get(), 0);
  }

  // 3. GA_Update is rejected.
  {
    ScopedQuietHdf5Errors quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              OpenImageDataset(std::format("HDF5:\"{}\"://band2d", file.path()),
                               GDAL_OF_UPDATE | GDAL_OF_RASTER));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // 4. Invalid colon token counts (< 3 or > 4) fail cleanly.
  EXPECT_EQ(nullptr, OpenImageDataset(std::format("HDF5:{}", file.path())));
  EXPECT_EQ(nullptr, OpenImageDataset("HDF5:a:b:c:d"));

  // 5. Windows drive-letter colon reassembly ("HDF5:C:\\...://band").
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_EQ(nullptr, OpenImageDataset("HDF5:C:\\missing_drive.h5://band2d"));
  }

  // 6. /vsicurl/http colon reassembly ("HDF5:/vsicurl/http://...").
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_EQ(nullptr,
              OpenImageDataset("HDF5:/vsicurl/http://localhost:8080/m.h5://b"));
  }

  // 7. Missing physical file, missing subdataset path, 0D scalar dataset,
  //    and unsupported string datatype all return nullptr cleanly.
  {
    ScopedQuietHdf5Errors quiet;
    EXPECT_EQ(nullptr,
              OpenImageDataset("HDF5:\"/vsimem/no_such_file.h5\"://b"));
  }
  EXPECT_EQ(nullptr, OpenImageDataset(std::format("HDF5:\"{}\"://no_such_sds",
                                                  file.path())));
  EXPECT_EQ(nullptr, OpenImageDataset(
                         std::format("HDF5:\"{}\"://scalar0d", file.path())));
  {
    ScopedQuietHdf5Errors quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, OpenImageDataset(
                           std::format("HDF5:\"{}\"://str2d", file.path())));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unhandled HDF5 data type"));
    CPLErrorReset();
  }
}

TEST(Hdf5ImageDatasetTest, Chunked3DWholeBandCacheAndRasterIO) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5Image();

  // Create a 3-band x 10-row x 12-col Int16 HDF5 dataset with [2, 4, 5] chunks
  // (partial edge blocks at row 8..9 and col 10..11) and DEFLATE compression.
  constexpr int kBands = 3;
  constexpr int kRows = 10;
  constexpr int kCols = 12;
  ScopedVsimemFile file("chunked3d.h5");
  std::vector<int16_t> expected(kBands * kRows * kCols);
  for (int b = 0; b < kBands; ++b) {
    for (int y = 0; y < kRows; ++y) {
      for (int x = 0; x < kCols; ++x) {
        expected[b * kRows * kCols + y * kCols + x] =
            static_cast<int16_t>((b + 1) * 1000 + y * 20 + x);
      }
    }
  }

  unsigned int szip_info = 0;
  const bool has_szip = H5Zfilter_avail(H5Z_FILTER_SZIP) > 0 &&
                        H5Zget_filter_info(H5Z_FILTER_SZIP, &szip_info) >= 0 &&
                        (szip_info & H5Z_FILTER_CONFIG_ENCODE_ENABLED) != 0;

  {
    ScopedH5File fid(CreateVsimemHdf5File(file.path()));
    ASSERT_GE(fid.get(), 0);
    const hsize_t dims[3] = {kBands, kRows, kCols};
    const hsize_t chunks[3] = {2, 4, 5};
    ScopedH5Dataspace sp(H5Screate_simple(3, dims, nullptr));
    ASSERT_GE(sp.get(), 0);
    ScopedH5PropertyList dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_GE(H5Pset_chunk(dcpl, 3, chunks), 0);
    ASSERT_GE(H5Pset_deflate(dcpl, 4), 0);

    ScopedH5Dataset dset(H5Dcreate(fid, "cube", H5T_NATIVE_INT16, sp, dcpl));
    ASSERT_GE(dset.get(), 0);
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_INT16, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       expected.data()),
              0);

    ASSERT_TRUE(
        WriteScalarAttr<double>(dset, "_FillValue", H5T_NATIVE_DOUBLE, -999.0));
    ASSERT_TRUE(
        WriteScalarAttr<double>(dset, "add_offset", H5T_NATIVE_DOUBLE, 12.5));
    ASSERT_TRUE(
        WriteScalarAttr<double>(dset, "scale_factor", H5T_NATIVE_DOUBLE, 0.25));
    ASSERT_TRUE(WriteScalarStringAttr(dset, "long_name", "Test Cube"));
    const double gains[3] = {1.5, 2.5, 3.5};
    ASSERT_TRUE(Write1DAttr<double>(dset, "gain_coefficients",
                                    H5T_NATIVE_DOUBLE, gains));
    const double waves[3] = {450.0, 550.0, 650.0};
    ASSERT_TRUE(Write1DAttr<double>(dset, "center_wavelengths",
                                    H5T_NATIVE_DOUBLE, waves));
    const char* bnames[3] = {"Blue", "Green", "Red"};
    Write1DVarStringAttr(dset, "band_names_list", bnames);

    // 2D dataset without nodata/offset/scale (uses SZIP if compiled in).
    const hsize_t dims2d[2] = {6, 8};
    ScopedH5Dataspace sp2d(H5Screate_simple(2, dims2d, nullptr));
    ASSERT_GE(sp2d.get(), 0);
    ScopedH5PropertyList dcpl2d(H5Pcreate(H5P_DATASET_CREATE));
    if (has_szip) {
      const hsize_t chunks2d[2] = {2, 8};
      ASSERT_GE(H5Pset_chunk(dcpl2d, 2, chunks2d), 0);
      ASSERT_GE(H5Pset_szip(dcpl2d, H5_SZIP_NN_OPTION_MASK, 8), 0);
    }
    ScopedH5Dataset d2d(
        H5Dcreate(fid, "plain2d", H5T_NATIVE_UINT8, sp2d, dcpl2d));
    ASSERT_GE(d2d.get(), 0);
    std::vector<uint8_t> plain_vals(6 * 8, 42);
    ASSERT_GE(H5Dwrite(d2d, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       plain_vals.data()),
              0);
  }

  // 1. Inspect 2D plain dataset defaults (GetNoDataValue/GetOffset/GetScale).
  {
    auto ds2d =
        OpenImageDataset(std::format("HDF5:\"{}\"://plain2d", file.path()));
    ASSERT_NE(nullptr, ds2d);
    if (has_szip) {
      EXPECT_STREQ("SZIP",
                   ds2d->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));
    }
    GDALRasterBand* b = ds2d->GetRasterBand(1);
    ASSERT_NE(nullptr, b);
    int has_nodata = 1;
    b->GetNoDataValue(&has_nodata);
    EXPECT_EQ(FALSE, has_nodata);
    int has_offset = 1;
    b->GetOffset(&has_offset);
    EXPECT_EQ(FALSE, has_offset);
    int has_scale = 1;
    b->GetScale(&has_scale);
    EXPECT_EQ(FALSE, has_scale);

    std::vector<uint8_t> buf(6 * 8, 0);
    ASSERT_EQ(CE_None, b->RasterIO(GF_Read, 0, 0, 8, 6, buf.data(), 8, 6,
                                   GDT_Byte, 0, 0, nullptr));
    EXPECT_EQ(42, buf[0]);
  }

  // 2. Inspect 3D chunked dataset metadata and band properties.
  const std::string uri3d = std::format("HDF5:\"{}\"://cube", file.path());
  {
    auto ds = OpenImageDataset(uri3d);
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(kCols, ds->GetRasterXSize());
    EXPECT_EQ(kRows, ds->GetRasterYSize());
    EXPECT_EQ(kBands, ds->GetRasterCount());
    EXPECT_STREQ("2",
                 ds->GetMetadataItem("BAND_CHUNK_SIZE", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("DEFLATE",
                 ds->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"));

    for (int b = 1; b <= kBands; ++b) {
      GDALRasterBand* band = ds->GetRasterBand(b);
      ASSERT_NE(nullptr, band);
      int bx = 0;
      int by = 0;
      band->GetBlockSize(&bx, &by);
      EXPECT_EQ(5, bx);
      EXPECT_EQ(4, by);
      EXPECT_EQ(GDT_Int16, band->GetRasterDataType());

      int success = 0;
      EXPECT_DOUBLE_EQ(-999.0, band->GetNoDataValue(&success));
      EXPECT_EQ(TRUE, success);
      success = 0;
      EXPECT_DOUBLE_EQ(12.5, band->GetOffset(&success));
      EXPECT_EQ(TRUE, success);
      success = 0;
      EXPECT_DOUBLE_EQ(0.25, band->GetScale(&success));
      EXPECT_EQ(TRUE, success);
      EXPECT_STREQ("Test Cube", band->GetMetadataItem("long_name"));
    }

    EXPECT_STREQ("1.5",
                 ds->GetRasterBand(1)->GetMetadataItem("gain_coefficient"));
    EXPECT_STREQ("2.5",
                 ds->GetRasterBand(2)->GetMetadataItem("gain_coefficient"));
    EXPECT_STREQ("3.5",
                 ds->GetRasterBand(3)->GetMetadataItem("gain_coefficient"));
    EXPECT_STREQ("Blue", ds->GetRasterBand(1)->GetMetadataItem("band_names"));
    EXPECT_STREQ("Red", ds->GetRasterBand(3)->GetMetadataItem("band_names"));

    // 3. Whole-band chunk cache state machine:
    //    DETECTION_IN_PROGRESS -> ENABLED (via full band 1 strip-by-strip then
    //    band 2) -> cache hit across chunk boundary (band 3) -> IReadBlock
    //    delegation -> DISABLED (via resampled read).
    EXPECT_STREQ("DETECTION_IN_PROGRESS",
                 ds->GetMetadataItem("WholeBandChunkOptim", "__DEBUG__"));

    std::vector<int16_t> strip1(5 * kCols, 0);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kCols, 5, strip1.data(), kCols, 5,
                           GDT_Int16, 0, 0, nullptr));
    EXPECT_EQ(expected[0], strip1[0]);

    std::vector<int16_t> strip2(5 * kCols, 0);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 5, kCols, 5, strip2.data(), kCols, 5,
                           GDT_Int16, 0, 0, nullptr));
    EXPECT_EQ(expected[5 * kCols], strip2[0]);

    // Switching to band 2 after reading all of band 1 enables WBC cache.
    std::vector<int16_t> b2_strip(4 * kCols, 0);
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                           GF_Read, 0, 0, kCols, 4, b2_strip.data(), kCols, 4,
                           GDT_Int16, 0, 0, nullptr));
    EXPECT_STREQ("ENABLED",
                 ds->GetMetadataItem("WholeBandChunkOptim", "__DEBUG__"));
    EXPECT_EQ(expected[kRows * kCols], b2_strip[0]);

    // Read band 3 (loads next band chunk iBandChunk = 1).
    std::vector<int16_t> b3_strip(3 * kCols, 0);
    ASSERT_EQ(CE_None, ds->GetRasterBand(3)->RasterIO(
                           GF_Read, 0, 0, kCols, 3, b3_strip.data(), kCols, 3,
                           GDT_Int16, 0, 0, nullptr));
    EXPECT_EQ(expected[2 * kRows * kCols], b3_strip[0]);

    // IReadBlock while WBC_ENABLED delegates to IRasterIO.
    std::vector<int16_t> block_buf(5 * 4, -1);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(0, 0, block_buf.data()));
    EXPECT_EQ(expected[0], block_buf[0]);

    // Resampled read transitions WBC state to DISABLED.
    std::vector<int16_t> resampled(6 * 5, 0);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kCols, kRows, resampled.data(), 6, 5,
                           GDT_Int16, 0, 0, nullptr));
    EXPECT_STREQ("DISABLED",
                 ds->GetMetadataItem("WholeBandChunkOptim", "__DEBUG__"));

    // 4. With WBC disabled: test partial bottom-right edge block IReadBlock,
    //    band IRasterIO direct hyperslab, and band IRasterIO MEMDataset type
    //    conversion (GDT_Float32).
    std::fill(block_buf.begin(), block_buf.end(), -1);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->ReadBlock(2, 2, block_buf.data()));
    EXPECT_EQ(expected[8 * kCols + 10], block_buf[0]);
    EXPECT_EQ(0, block_buf[4]);  // Zero-padded beyond kCols=12.

    std::vector<int16_t> sub_i16(4 * 6, 0);
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                           GF_Read, 2, 3, 6, 4, sub_i16.data(), 6, 4, GDT_Int16,
                           0, 0, nullptr));
    EXPECT_EQ(expected[kRows * kCols + 3 * kCols + 2], sub_i16[0]);

    std::vector<float> sub_f32(4 * 6, 0.0f);
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                           GF_Read, 2, 3, 6, 4, sub_f32.data(), 6, 4,
                           GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(static_cast<float>(expected[kRows * kCols + 3 * kCols + 2]),
                    sub_f32[0]);

    // 5. Dataset-level 3D IRasterIO:
    //    (a) Band-sequential fast hyperslab (panBandMap = {1, 2, 3}).
    std::vector<int16_t> all_bsq(kBands * kRows * kCols, 0);
    std::array<int, 3> band_map = {1, 2, 3};
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kCols, kRows, all_bsq.data(),
                                    kCols, kRows, GDT_Int16, 3, band_map.data(),
                                    sizeof(int16_t), kCols * sizeof(int16_t),
                                    kCols * kRows * sizeof(int16_t), nullptr));
    EXPECT_EQ(expected, all_bsq);

    //    (b) MEMDataset multi-band type conversion (GDT_Float32).
    std::vector<float> all_f32(kBands * kRows * kCols, 0.0f);
    ASSERT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, kCols, kRows, all_f32.data(), kCols,
                           kRows, GDT_Float32, 3, band_map.data(),
                           sizeof(float), kCols * sizeof(float),
                           kCols * kRows * sizeof(float), nullptr));
    EXPECT_FLOAT_EQ(static_cast<float>(expected[2 * kRows * kCols + 15]),
                    all_f32[2 * kRows * kCols + 15]);

    //    (c) Non-consecutive bands fallback (panBandMap = {1, 3}).
    std::array<int, 2> non_consec = {1, 3};
    std::vector<int16_t> two_bands(2 * kRows * kCols, 0);
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, kCols, kRows,
                                    two_bands.data(), kCols, kRows, GDT_Int16,
                                    2, non_consec.data(), 0, 0, 0, nullptr));
    EXPECT_EQ(expected[2 * kRows * kCols], two_bands[kRows * kCols]);
  }

  // 6. Exercise WBC detection invalidation branches (non-contiguous band 1,
  //    incomplete band 1 before band 2, and jumping to band 3).
  struct WbcInvalidationCase {
    int second_band;
    int yoff;
  };
  constexpr WbcInvalidationCase kInvalidations[] = {
      {.second_band = 1, .yoff = 1},
      {.second_band = 2, .yoff = 0},
      {.second_band = 3, .yoff = 0},
  };
  for (const WbcInvalidationCase& tc : kInvalidations) {
    auto ds = OpenImageDataset(uri3d);
    ASSERT_NE(nullptr, ds);
    std::vector<int16_t> buf(4 * kCols, 0);
    ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, kCols, 4, buf.data(), kCols, 4,
                           GDT_Int16, 0, 0, nullptr));
    ASSERT_EQ(CE_None, ds->GetRasterBand(tc.second_band)
                           ->RasterIO(GF_Read, 0, tc.yoff, kCols, 4, buf.data(),
                                      kCols, 4, GDT_Int16, 0, 0, nullptr));
    EXPECT_STREQ("DISABLED",
                 ds->GetMetadataItem("WholeBandChunkOptim", "__DEBUG__"));
  }
}

#ifdef HDF5_HAVE_FLOAT16
TEST(Hdf5ImageDatasetTest, Float16AndComplexFloat16Conversion) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5Image();

  // 1. Create scalar _Float16 and compound ComplexFloat16 datasets.
  ScopedVsimemFile file("float16.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(file.path()));
    ASSERT_GE(fid.get(), 0);

    const hsize_t dims[2] = {3, 4};
    ScopedH5Dataspace sp(H5Screate_simple(2, dims, nullptr));
    ASSERT_GE(sp.get(), 0);

    ScopedH5Dataset d_f16(
        H5Dcreate(fid, "f16", H5T_NATIVE_FLOAT16, sp, H5P_DEFAULT));
    ASSERT_GE(d_f16.get(), 0);
    std::vector<_Float16> f16_vals(12);
    for (int i = 0; i < 12; ++i) {
      f16_vals[i] = static_cast<_Float16>((i - 4) * 0.5f);
    }
    ASSERT_GE(H5Dwrite(d_f16, H5T_NATIVE_FLOAT16, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       f16_vals.data()),
              0);

    ScopedH5Datatype cf16_t(CreateComplexH5Type(H5T_NATIVE_FLOAT16));
    ScopedH5Dataset d_cf16(H5Dcreate(fid, "cf16", cf16_t, sp, H5P_DEFAULT));
    ASSERT_GE(d_cf16.get(), 0);
    std::vector<ComplexFloat16> cf16_vals(12);
    for (int i = 0; i < 12; ++i) {
      cf16_vals[i] = {static_cast<_Float16>(i * 0.25f),
                      static_cast<_Float16>(-i * 0.5f)};
    }
    ASSERT_GE(H5Dwrite(d_cf16, cf16_t, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       cf16_vals.data()),
              0);
  }

  // 2. Test scalar _Float16 via both band and dataset RasterIO.
  {
    auto ds = OpenImageDataset(std::format("HDF5:\"{}\"://f16", file.path()));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GDT_Float32, band->GetRasterDataType());

    std::vector<float> out(12, 0.0f);
    ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 4, 3, out.data(), 4, 3,
                                      GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(-2.0f, out[0]);
    EXPECT_FLOAT_EQ(3.5f, out[11]);

    std::fill(out.begin(), out.end(), 0.0f);
    int b1 = 1;
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 4, 3, out.data(), 4, 3,
                                    GDT_Float32, 1, &b1, 0, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(-1.5f, out[1]);
  }

  // 3. Test complex _Float16 via both band and dataset RasterIO.
  {
    auto ds = OpenImageDataset(std::format("HDF5:\"{}\"://cf16", file.path()));
    ASSERT_NE(nullptr, ds);
    GDALRasterBand* band = ds->GetRasterBand(1);
    ASSERT_NE(nullptr, band);
    EXPECT_EQ(GDT_CFloat32, band->GetRasterDataType());

    std::vector<ComplexFloat32> out(12);
    ASSERT_EQ(CE_None, band->RasterIO(GF_Read, 0, 0, 4, 3, out.data(), 4, 3,
                                      GDT_CFloat32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(0.5f, out[2].r);
    EXPECT_FLOAT_EQ(-1.0f, out[2].i);

    std::fill(out.begin(), out.end(), ComplexFloat32{0.0f, 0.0f});
    int b1 = 1;
    ASSERT_EQ(CE_None, ds->RasterIO(GF_Read, 0, 0, 4, 3, out.data(), 4, 3,
                                    GDT_CFloat32, 1, &b1, 0, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(0.75f, out[3].r);
    EXPECT_FLOAT_EQ(-1.5f, out[3].i);
  }
}
#endif

TEST(Hdf5ImageDatasetTest, HdfEos5GridAndSwathOpenAndRead) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5Image();

  // 1. HDF-EOS5 GRID with permuted DimList=("YDim","Band","XDim") + projection.
  ScopedVsimemFile grid_file("eos_grid.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(grid_file.path()));
    ASSERT_GE(fid.get(), 0);
    ASSERT_TRUE(WriteStructMetadata(fid, R"(
GROUP=GridStructure
  GROUP=GRID_1
    GridName="GeoGrid"
    Projection="HE5_GCTP_GEO"
    GridOrigin="HE5_HDFE_GD_UL"
    UpperLeftPointMtrs=(10000000.0,45000000.0)
    LowerRightMtrs=(20000000.0,35000000.0)
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="Band"
        Size=2
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="YDim"
        Size=4
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="XDim"
        Size=5
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="GridCube"
        DimList=("YDim","Band","XDim")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=GRID_1
END_GROUP=GridStructure
END
)"));
    ScopedH5Group g_eos(H5Gcreate(fid, "HDFEOS", 0));
    ASSERT_GE(g_eos.get(), 0);
    ScopedH5Group g_grids(H5Gcreate(g_eos, "GRIDS", 0));
    ASSERT_GE(g_grids.get(), 0);
    ScopedH5Group g_gg(H5Gcreate(g_grids, "GeoGrid", 0));
    ASSERT_GE(g_gg.get(), 0);
    ScopedH5Group g_df(H5Gcreate(g_gg, "Data Fields", 0));
    ASSERT_GE(g_df.get(), 0);

    const hsize_t dims[3] = {4, 2, 5};
    ScopedH5Dataspace sp(H5Screate_simple(3, dims, nullptr));
    ASSERT_GE(sp.get(), 0);
    ScopedH5Dataset dset(
        H5Dcreate(g_df, "GridCube", H5T_NATIVE_FLOAT, sp, H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    std::vector<float> vals(4 * 2 * 5, 0.0f);
    for (int y = 0; y < 4; ++y) {
      for (int b = 0; b < 2; ++b) {
        for (int x = 0; x < 5; ++x) {
          vals[y * 10 + b * 5 + x] =
              static_cast<float>((b + 1) * 10 + y * 5 + x);
        }
      }
    }
    ASSERT_GE(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       vals.data()),
              0);
  }

  {
    auto ds = OpenImageDataset(
        std::format("HDF5:\"{}\"://HDFEOS/GRIDS/GeoGrid/Data_Fields/GridCube",
                    grid_file.path()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(5, ds->GetRasterXSize());
    EXPECT_EQ(4, ds->GetRasterYSize());
    EXPECT_EQ(2, ds->GetRasterCount());
    EXPECT_NE(nullptr, ds->GetSpatialRef());

    double gt[6] = {};
    ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
    EXPECT_NEAR(10.0, gt[0], 1e-6);
    EXPECT_NEAR(2.0, gt[1], 1e-6);
    EXPECT_NEAR(45.0, gt[3], 1e-6);
    EXPECT_NEAR(-2.5, gt[5], 1e-6);

    // Non-standard axis order (YDim, Band, XDim) exercises the fallback paths
    // in both HDF5ImageRasterBand::IRasterIO and HDF5ImageDataset::IRasterIO.
    std::vector<float> b2_vals(4 * 5, 0.0f);
    ASSERT_EQ(CE_None, ds->GetRasterBand(2)->RasterIO(
                           GF_Read, 0, 0, 5, 4, b2_vals.data(), 5, 4,
                           GDT_Float32, 0, 0, nullptr));
    EXPECT_FLOAT_EQ(20.0f, b2_vals[0]);
    EXPECT_FLOAT_EQ(39.0f, b2_vals[19]);
  }

  // 2. HDF-EOS5 SWATH populates GEOLOCATION metadata domain.
  ScopedVsimemFile swath_file("eos_swath.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(swath_file.path()));
    ASSERT_GE(fid.get(), 0);
    ASSERT_TRUE(WriteStructMetadata(fid, R"(
GROUP=SwathStructure
  GROUP=SWATH_1
    SwathName="ScanSwath"
    GROUP=Dimension
      OBJECT=Dimension_1
        DimensionName="GeoTrack"
        Size=4
      END_OBJECT=Dimension_1
      OBJECT=Dimension_2
        DimensionName="GeoXtrack"
        Size=6
      END_OBJECT=Dimension_2
      OBJECT=Dimension_3
        DimensionName="DataTrack"
        Size=8
      END_OBJECT=Dimension_3
    END_GROUP=Dimension
    GROUP=DimensionMap
      OBJECT=DimensionMap_1
        GeoDimension="GeoTrack"
        DataDimension="DataTrack"
        Offset=1
        Increment=2
      END_OBJECT=DimensionMap_1
    END_GROUP=DimensionMap
    GROUP=GeoField
      OBJECT=GeoField_1
        GeoFieldName="Latitude"
        DimList=("GeoTrack","GeoXtrack")
      END_OBJECT=GeoField_1
      OBJECT=GeoField_2
        GeoFieldName="Longitude"
        DimList=("GeoTrack","GeoXtrack")
      END_OBJECT=GeoField_2
    END_GROUP=GeoField
    GROUP=DataField
      OBJECT=DataField_1
        DataFieldName="Radiance"
        DimList=("DataTrack","GeoXtrack")
      END_OBJECT=DataField_1
    END_GROUP=DataField
  END_GROUP=SWATH_1
END_GROUP=SwathStructure
END
)"));
    ScopedH5Group g_eos(H5Gcreate(fid, "HDFEOS", 0));
    ASSERT_GE(g_eos.get(), 0);
    ScopedH5Group g_swaths(H5Gcreate(g_eos, "SWATHS", 0));
    ASSERT_GE(g_swaths.get(), 0);
    ScopedH5Group g_sw(H5Gcreate(g_swaths, "ScanSwath", 0));
    ASSERT_GE(g_sw.get(), 0);
    ScopedH5Group g_gf(H5Gcreate(g_sw, "Geolocation Fields", 0));
    ASSERT_GE(g_gf.get(), 0);
    ScopedH5Group g_df(H5Gcreate(g_sw, "Data Fields", 0));
    ASSERT_GE(g_df.get(), 0);

    const hsize_t gdims[2] = {4, 6};
    ScopedH5Dataspace gsp(H5Screate_simple(2, gdims, nullptr));
    ASSERT_GE(gsp.get(), 0);
    ScopedH5Dataset d_lat(
        H5Dcreate(g_gf, "Latitude", H5T_NATIVE_FLOAT, gsp, H5P_DEFAULT));
    ASSERT_GE(d_lat.get(), 0);
    ScopedH5Dataset d_lon(
        H5Dcreate(g_gf, "Longitude", H5T_NATIVE_FLOAT, gsp, H5P_DEFAULT));
    ASSERT_GE(d_lon.get(), 0);

    const hsize_t ddims[2] = {8, 6};
    ScopedH5Dataspace dsp(H5Screate_simple(2, ddims, nullptr));
    ASSERT_GE(dsp.get(), 0);
    ScopedH5Dataset d_rad(
        H5Dcreate(g_df, "Radiance", H5T_NATIVE_INT16, dsp, H5P_DEFAULT));
    ASSERT_GE(d_rad.get(), 0);
  }

  {
    auto ds = OpenImageDataset(std::format(
        "HDF5:\"{}\"://HDFEOS/SWATHS/ScanSwath/Data_Fields/Radiance",
        swath_file.path()));
    ASSERT_NE(nullptr, ds);
    CSLConstList geoloc = ds->GetMetadata("GEOLOCATION");
    ASSERT_NE(nullptr, geoloc);
    EXPECT_STREQ("1", CSLFetchNameValue(geoloc, "LINE_OFFSET"));
    EXPECT_STREQ("2", CSLFetchNameValue(geoloc, "LINE_STEP"));
    EXPECT_STREQ("PIXEL_CENTER",
                 CSLFetchNameValue(geoloc, "GEOREFERENCING_CONVENTION"));
    EXPECT_THAT(CSLFetchNameValueDef(geoloc, "X_DATASET", ""),
                HasSubstr("Longitude"));
  }
}

TEST(Hdf5ImageDatasetTest, LatLon2DGCPsAndODIMH5Projections) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5Image();

  // 1. 2D Latitude/Longitude GCP grid crossing the antimeridian + _FillValue.
  constexpr int kLatRows = 100;
  constexpr int kLonCols = 30;
  ScopedVsimemFile latlon_file("latlon_gcps.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(latlon_file.path()));
    ASSERT_GE(fid.get(), 0);
    const hsize_t dims[2] = {kLatRows, kLonCols};
    ScopedH5Dataspace sp(H5Screate_simple(2, dims, nullptr));
    ASSERT_GE(sp.get(), 0);

    ScopedH5Dataset d_data(
        H5Dcreate(fid, "data", H5T_NATIVE_UINT8, sp, H5P_DEFAULT));
    ASSERT_GE(d_data.get(), 0);

    std::vector<float> lats(kLatRows * kLonCols, 25.0f);
    std::vector<float> lons(kLatRows * kLonCols, 178.0f);
    lons[0] = 175.0f;
    lons[1] = -175.0f;
    lats[2] = -999.0f;  // Nodata sample skipped in GCP generation.
    lons[3] = -999.0f;

    ScopedH5Dataset d_lat(
        H5Dcreate(fid, "Latitude", H5T_NATIVE_FLOAT, sp, H5P_DEFAULT));
    ASSERT_GE(d_lat.get(), 0);
    ASSERT_TRUE(WriteScalarAttr<double>(d_lat, "_FillValue", H5T_NATIVE_DOUBLE,
                                        -999.0));
    ASSERT_GE(H5Dwrite(d_lat, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       lats.data()),
              0);

    ScopedH5Dataset d_lon(
        H5Dcreate(fid, "Longitude", H5T_NATIVE_FLOAT, sp, H5P_DEFAULT));
    ASSERT_GE(d_lon.get(), 0);
    ASSERT_TRUE(WriteScalarAttr<double>(d_lon, "_FillValue", H5T_NATIVE_DOUBLE,
                                        -999.0));
    ASSERT_GE(H5Dwrite(d_lon, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                       lons.data()),
              0);
  }

  // Default antimeridian heuristic shifts -175 to +5 and 175 to 355.
  {
    auto ds =
        OpenImageDataset(std::format("HDF5:\"{}\"://data", latlon_file.path()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(kLatRows * kLonCols - 2, ds->GetGCPCount());
    ASSERT_NE(nullptr, ds->GetGCPs());
    EXPECT_NE(nullptr, ds->GetGCPSpatialRef());
    EXPECT_DOUBLE_EQ(355.0, ds->GetGCPs()[0].dfGCPX);
    EXPECT_DOUBLE_EQ(5.0, ds->GetGCPs()[1].dfGCPX);
  }

  // Override via HDF5_SHIFT_GCPX_BY_180=NO keeps -175 unshifted.
  {
    CPLConfigOptionSetter no_shift("HDF5_SHIFT_GCPX_BY_180", "NO", false);
    auto ds =
        OpenImageDataset(std::format("HDF5:\"{}\"://data", latlon_file.path()));
    ASSERT_NE(nullptr, ds);
    ASSERT_GT(ds->GetGCPCount(), 2);
    EXPECT_DOUBLE_EQ(-175.0, ds->GetGCPs()[1].dfGCPX);
  }

  // Mismatched Longitude rank/dimensions skips GCP generation cleanly.
  ScopedVsimemFile mismatch_latlon("mismatch_latlon.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(mismatch_latlon.path()));
    ASSERT_GE(fid.get(), 0);
    const hsize_t dims[2] = {kLatRows, kLonCols};
    ScopedH5Dataspace sp(H5Screate_simple(2, dims, nullptr));
    ASSERT_GE(sp.get(), 0);
    ScopedH5Dataset d1(
        H5Dcreate(fid, "data", H5T_NATIVE_UINT8, sp, H5P_DEFAULT));
    ASSERT_GE(d1.get(), 0);
    ScopedH5Dataset d2(
        H5Dcreate(fid, "Latitude", H5T_NATIVE_FLOAT, sp, H5P_DEFAULT));
    ASSERT_GE(d2.get(), 0);

    const hsize_t bad_dims[2] = {5, 5};
    ScopedH5Dataspace bad_sp(H5Screate_simple(2, bad_dims, nullptr));
    ASSERT_GE(bad_sp.get(), 0);
    ScopedH5Dataset d3(
        H5Dcreate(fid, "Longitude", H5T_NATIVE_FLOAT, bad_sp, H5P_DEFAULT));
    ASSERT_GE(d3.get(), 0);
  }
  {
    auto ds = OpenImageDataset(
        std::format("HDF5:\"{}\"://data", mismatch_latlon.path()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(0, ds->GetGCPCount());
    EXPECT_EQ(nullptr, ds->GetGCPSpatialRef());
  }

  // 2. ODIM_H5 projection: valid UTM, missing corner, invalid proj4, and
  //    out-of-horizon transform failure.
  struct OdimCase {
    const char* name;
    const char* projdef;
    const char* ll_lon;
    std::optional<const char*> ll_lat;
    std::optional<const char*> ur_lon;
    std::optional<const char*> ur_lat;
    bool expect_valid_gt;
  };
  constexpr OdimCase kOdimCases[] = {
      {.name = "utm_valid",
       .projdef = "+proj=utm +zone=32 +datum=WGS84 +units=m +no_defs",
       .ll_lon = "6.0",
       .ll_lat = "45.0",
       .ur_lon = "10.0",
       .ur_lat = "48.0",
       .expect_valid_gt = true},
      {.name = "missing_corner",
       .projdef = "+proj=longlat +datum=WGS84",
       .ll_lon = "6.0",
       .ll_lat = std::nullopt,
       .ur_lon = std::nullopt,
       .ur_lat = std::nullopt,
       .expect_valid_gt = false},
      {.name = "bad_proj4",
       .projdef = "+proj=nonexistent_proj_xyz",
       .ll_lon = "6.0",
       .ll_lat = "45.0",
       .ur_lon = "10.0",
       .ur_lat = "48.0",
       .expect_valid_gt = false},
      {.name = "geos_out_of_disk",
       .projdef = "+proj=geos +h=35785831 +lon_0=0 +datum=WGS84",
       .ll_lon = "175.0",
       .ll_lat = "0.0",
       .ur_lon = "178.0",
       .ur_lat = "10.0",
       .expect_valid_gt = false},
  };

  for (const OdimCase& tc : kOdimCases) {
    ScopedVsimemFile odim_file(std::format("odim_{}.h5", tc.name));
    {
      ScopedH5File fid(CreateVsimemHdf5File(odim_file.path()));
      ASSERT_GE(fid.get(), 0);
      const hsize_t dims[2] = {kLatRows, kLonCols};
      ScopedH5Dataspace sp(H5Screate_simple(2, dims, nullptr));
      ASSERT_GE(sp.get(), 0);
      ScopedH5Dataset dset(
          H5Dcreate(fid, "dataset1", H5T_NATIVE_UINT8, sp, H5P_DEFAULT));
      ASSERT_GE(dset.get(), 0);

      ASSERT_TRUE(WriteScalarStringAttr(fid, "where_projdef", tc.projdef,
                                        H5T_STR_NULLPAD));
      ASSERT_TRUE(WriteScalarStringAttr(fid, "where_LL_lon", tc.ll_lon,
                                        H5T_STR_NULLPAD));
      if (tc.ll_lat.has_value()) {
        ASSERT_TRUE(WriteScalarStringAttr(fid, "where_LL_lat", *tc.ll_lat,
                                          H5T_STR_NULLPAD));
      }
      if (tc.ur_lon.has_value()) {
        ASSERT_TRUE(WriteScalarStringAttr(fid, "where_UR_lon", *tc.ur_lon,
                                          H5T_STR_NULLPAD));
      }
      if (tc.ur_lat.has_value()) {
        ASSERT_TRUE(WriteScalarStringAttr(fid, "where_UR_lat", *tc.ur_lat,
                                          H5T_STR_NULLPAD));
      }
    }

    auto ds = OpenImageDataset(
        std::format("HDF5:\"{}\"://dataset1", odim_file.path()));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {};
    if (tc.expect_valid_gt) {
      EXPECT_NE(nullptr, ds->GetSpatialRef());
      EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
      EXPECT_GT(gt[1], 0.0);
      EXPECT_LT(gt[5], 0.0);
    } else {
      ScopedQuietHdf5Errors quiet;
      EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
    }
  }
}

TEST(Hdf5ImageDatasetTest, CosmoSkyMedProductVariants) {
  ScopedHdf5DriverManagerCleanup cleanup_dm;
  GDALRegister_HDF5Image();

  // 1. CSK L1A (Product_Type="SCS" for 3D pixel-interleaved complex RasterIO).
  ScopedVsimemFile csk_scs_file("csk_scs.h5");
  {
    ScopedH5File fid(CreateVsimemHdf5File(csk_scs_file.path()));
    ASSERT_GE(fid.get(), 0);
    ASSERT_TRUE(
        WriteScalarStringAttr(fid, "Mission_ID", "CSK", H5T_STR_NULLPAD));
    ASSERT_TRUE(
        WriteScalarStringAttr(fid, "Product_Type", "SCS", H5T_STR_NULLPAD));

    const hsize_t dims[3] = {4, 5, 2};
    ScopedH5Dataspace sp(H5Screate_simple(3, dims, nullptr));
    ASSERT_GE(sp.get(), 0);
    ScopedH5Datatype cf32_t(CreateComplexH5Type(H5T_NATIVE_FLOAT));
    ScopedH5Dataset dset(H5Dcreate(fid, "SBI", cf32_t, sp, H5P_DEFAULT));
    ASSERT_GE(dset.get(), 0);
    std::vector<ComplexFloat32> vals(4 * 5 * 2);
    for (int i = 0; i < 4 * 5 * 2; ++i) {
      vals[i] = {static_cast<float>(i + 1), static_cast<float>(-(i + 1))};
    }
    ASSERT_GE(
        H5Dwrite(dset, cf32_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals.data()), 0);
  }
  {
    auto ds =
        OpenImageDataset(std::format("HDF5:\"{}\"://SBI", csk_scs_file.path()));
    ASSERT_NE(nullptr, ds);
    EXPECT_EQ(5, ds->GetRasterXSize());
    EXPECT_EQ(4, ds->GetRasterYSize());
    EXPECT_EQ(2, ds->GetRasterCount());

    std::vector<ComplexFloat32> bip(4 * 5 * 2);
    std::array<int, 2> bmap = {1, 2};
    ASSERT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 5, 4, bip.data(), 5, 4, GDT_CFloat32,
                           2, bmap.data(), 2 * sizeof(ComplexFloat32),
                           5 * 2 * sizeof(ComplexFloat32),
                           sizeof(ComplexFloat32), nullptr));
    EXPECT_FLOAT_EQ(1.0f, bip[0].r);
    EXPECT_FLOAT_EQ(-1.0f, bip[0].i);
    EXPECT_FLOAT_EQ(2.0f, bip[1].r);

    std::vector<ComplexFloat32> bsq(4 * 5 * 2);
    ASSERT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 5, 4, bsq.data(), 5, 4, GDT_CFloat32,
                           2, bmap.data(), sizeof(ComplexFloat32),
                           5 * sizeof(ComplexFloat32),
                           4 * 5 * sizeof(ComplexFloat32), nullptr));
    EXPECT_FLOAT_EQ(2.0f, bsq[4 * 5].r);
    EXPECT_FLOAT_EQ(-2.0f, bsq[4 * 5].i);
  }

  // 2. CSK L1A ("SSC") valid 4-corner GCPs + L1B ("DGM") error path.
  for (bool valid_corners : {true, false}) {
    ScopedVsimemFile csk_gcp_file(valid_corners ? "csk_ssc_ok.h5"
                                                : "csk_dgm_err.h5");
    {
      ScopedH5File fid(CreateVsimemHdf5File(csk_gcp_file.path()));
      ASSERT_GE(fid.get(), 0);
      ASSERT_TRUE(
          WriteScalarStringAttr(fid, "Mission_ID", "CSK", H5T_STR_NULLPAD));
      ASSERT_TRUE(WriteScalarStringAttr(
          fid, "Product_Type", valid_corners ? "SSC" : "DGM", H5T_STR_NULLPAD));

      const hsize_t dims[2] = {10, 20};
      ScopedH5Dataspace sp(H5Screate_simple(2, dims, nullptr));
      ASSERT_GE(sp.get(), 0);
      ScopedH5Dataset dset(
          H5Dcreate(fid, "MBI", H5T_NATIVE_UINT16, sp, H5P_DEFAULT));
      ASSERT_GE(dset.get(), 0);
      const double tl[3] = {45.5, 9.1, 100.0};
      const double tr[3] = {45.5, 9.5, 105.0};
      const double bl[3] = {45.1, 9.1, 110.0};
      ASSERT_TRUE(Write1DAttr<double>(dset, "Top Left Geodetic Coordinates",
                                      H5T_NATIVE_DOUBLE, tl));
      ASSERT_TRUE(Write1DAttr<double>(dset, "Top Right Geodetic Coordinates",
                                      H5T_NATIVE_DOUBLE, tr));
      ASSERT_TRUE(Write1DAttr<double>(dset, "Bottom Left Geodetic Coordinates",
                                      H5T_NATIVE_DOUBLE, bl));
      if (valid_corners) {
        const double br[3] = {45.1, 9.5, 115.0};
        ASSERT_TRUE(Write1DAttr<double>(
            dset, "Bottom Right Geodetic Coordinates", H5T_NATIVE_DOUBLE, br));
      }
    }

    auto ds =
        OpenImageDataset(std::format("HDF5:\"{}\"://MBI", csk_gcp_file.path()));
    ASSERT_NE(nullptr, ds);
    if (valid_corners) {
      EXPECT_EQ(4, ds->GetGCPCount());
      EXPECT_NE(nullptr, ds->GetGCPSpatialRef());
      EXPECT_DOUBLE_EQ(9.1, ds->GetGCPs()[0].dfGCPX);
      EXPECT_DOUBLE_EQ(45.5, ds->GetGCPs()[0].dfGCPY);
      EXPECT_DOUBLE_EQ(100.0, ds->GetGCPs()[0].dfGCPZ);
    } else {
      EXPECT_EQ(0, ds->GetGCPCount());
    }
  }

  // 3. CSK/KMPS/CSG L1C/L1D (GEC UTM, GTC UPS + missing spacing, malformed,
  //    RAW L0, and QLK quicklook exclusion).
  enum class ExpectedSrsKind {
    kNull,
    kProjected,
    kGeographicWgs84,
  };
  struct CskGeorefVariant {
    const char* tag;
    const char* ds_name;
    const char* mission_id;
    const char* product_type;
    std::optional<const char*> proj_id;
    std::optional<std::array<double, 2>> false_en;
    std::optional<double> scale_factor;
    std::optional<std::array<double, 2>> proj_centre;
    std::optional<std::array<double, 2>> top_left_en;
    std::optional<double> spacing;
    ExpectedSrsKind expected_srs;
    bool expect_gt;
  };
  constexpr CskGeorefVariant kVariants[] = {
      {.tag = "csk_gec_utm",
       .ds_name = "IMG",
       .mission_id = "CSK",
       .product_type = "GEC",
       .proj_id = "UTM",
       .false_en = std::array<double, 2>{500000.0, 0.0},
       .scale_factor = 0.9996,
       .proj_centre = std::array<double, 2>{0.0, 9.0},
       .top_left_en = std::array<double, 2>{400000.0, 5000000.0},
       .spacing = 2.5,
       .expected_srs = ExpectedSrsKind::kProjected,
       .expect_gt = true},
      {.tag = "kmps_gtc_ups",
       .ds_name = "IMG",
       .mission_id = "KMPS",
       .product_type = "GTC",
       .proj_id = "UPS",
       .false_en = std::array<double, 2>{2000000.0, 2000000.0},
       .scale_factor = 0.994,
       .proj_centre = std::array<double, 2>{71.0, 0.0},
       .top_left_en = std::nullopt,
       .spacing = std::nullopt,
       .expected_srs = ExpectedSrsKind::kProjected,
       .expect_gt = false},
      {.tag = "csg_gec_bad",
       .ds_name = "IMG",
       .mission_id = "CSG",
       .product_type = "GEC",
       .proj_id = std::nullopt,
       .false_en = std::nullopt,
       .scale_factor = std::nullopt,
       .proj_centre = std::nullopt,
       .top_left_en = std::nullopt,
       .spacing = std::nullopt,
       .expected_srs = ExpectedSrsKind::kNull,
       .expect_gt = false},
      {.tag = "csk_raw",
       .ds_name = "IMG",
       .mission_id = "CSK",
       .product_type = "RAW",
       .proj_id = std::nullopt,
       .false_en = std::nullopt,
       .scale_factor = std::nullopt,
       .proj_centre = std::nullopt,
       .top_left_en = std::nullopt,
       .spacing = std::nullopt,
       .expected_srs = ExpectedSrsKind::kGeographicWgs84,
       .expect_gt = false},
      {.tag = "csk_QLK",
       .ds_name = "QLK",
       .mission_id = "CSK",
       .product_type = "RAW",
       .proj_id = std::nullopt,
       .false_en = std::nullopt,
       .scale_factor = std::nullopt,
       .proj_centre = std::nullopt,
       .top_left_en = std::nullopt,
       .spacing = std::nullopt,
       .expected_srs = ExpectedSrsKind::kNull,
       .expect_gt = false},
  };

  for (const CskGeorefVariant& v : kVariants) {
    ScopedVsimemFile file(std::format("{}.h5", v.tag));
    {
      ScopedH5File fid(CreateVsimemHdf5File(file.path()));
      ASSERT_GE(fid.get(), 0);

      const hsize_t dims[2] = {8, 8};
      ScopedH5Dataspace sp(H5Screate_simple(2, dims, nullptr));
      ASSERT_GE(sp.get(), 0);
      ScopedH5Dataset dset(
          H5Dcreate(fid, v.ds_name, H5T_NATIVE_UINT16, sp, H5P_DEFAULT));
      ASSERT_GE(dset.get(), 0);

      ASSERT_TRUE(WriteScalarStringAttr(fid, "Mission_ID", v.mission_id,
                                        H5T_STR_NULLPAD));
      ASSERT_TRUE(WriteScalarStringAttr(fid, "Product_Type", v.product_type,
                                        H5T_STR_NULLPAD));
      if (v.proj_id.has_value()) {
        ASSERT_TRUE(WriteScalarStringAttr(fid, "Projection_ID", *v.proj_id,
                                          H5T_STR_NULLPAD));
      }
      if (v.false_en.has_value()) {
        ASSERT_TRUE(Write1DAttr<double>(fid, "Map Projection False East-North",
                                        H5T_NATIVE_DOUBLE, *v.false_en));
      }
      if (v.scale_factor.has_value()) {
        const double sf = *v.scale_factor;
        ASSERT_TRUE(Write1DAttr<double>(fid, "Map Projection Scale Factor",
                                        H5T_NATIVE_DOUBLE, std::span(&sf, 1)));
      }
      if (v.proj_centre.has_value()) {
        ASSERT_TRUE(Write1DAttr<double>(fid, "Map Projection Centre",
                                        H5T_NATIVE_DOUBLE, *v.proj_centre));
      }
      if (v.top_left_en.has_value()) {
        ASSERT_TRUE(Write1DAttr<double>(dset, "Top Left East-North",
                                        H5T_NATIVE_DOUBLE, *v.top_left_en));
      }
      if (v.spacing.has_value()) {
        const double spc = *v.spacing;
        ASSERT_TRUE(Write1DAttr<double>(dset, "Line Spacing", H5T_NATIVE_DOUBLE,
                                        std::span(&spc, 1)));
        ASSERT_TRUE(Write1DAttr<double>(dset, "Column Spacing",
                                        H5T_NATIVE_DOUBLE, std::span(&spc, 1)));
      }
    }

    auto ds = OpenImageDataset(
        std::format("HDF5:\"{}\"://{}", file.path(), v.ds_name));
    ASSERT_NE(nullptr, ds);
    double gt[6] = {};
    switch (v.expected_srs) {
      case ExpectedSrsKind::kNull:
        EXPECT_EQ(nullptr, ds->GetSpatialRef());
        break;
      case ExpectedSrsKind::kProjected:
        ASSERT_NE(nullptr, ds->GetSpatialRef());
        EXPECT_EQ(TRUE, ds->GetSpatialRef()->IsProjected());
        break;
      case ExpectedSrsKind::kGeographicWgs84:
        ASSERT_NE(nullptr, ds->GetSpatialRef());
        EXPECT_EQ(TRUE, ds->GetSpatialRef()->IsGeographic());
        EXPECT_EQ(FALSE, ds->GetSpatialRef()->IsProjected());
        break;
    }
    if (v.expect_gt) {
      ASSERT_EQ(CE_None, ds->GetGeoTransform(gt));
      EXPECT_THAT(gt, ElementsAre(400000.0, 2.5, 0.0, 5000000.0, 0.0, -2.5));
    } else {
      ScopedQuietHdf5Errors quiet;
      EXPECT_EQ(CE_Failure, ds->GetGeoTransform(gt));
    }
  }
}

// TODO: b/335317901 - Cover PAM .aux.xml relative GEOLOCATION dataset path
// patching in HDF5ImageDataset::Open.

}  // namespace
}  // namespace autotest2
