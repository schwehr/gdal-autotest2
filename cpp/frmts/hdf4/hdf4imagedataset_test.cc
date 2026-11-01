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

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/gdal/autotest2/cpp/frmts/hdf4/hdf4_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
// clang-format off
#include "third_party/gdal/frmts/hdf4/hdf-eos/HdfEosDef.h"
// clang-format on
#include "third_party/gdal/frmts/hdf4/hdf4drivercore.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class ScopedConfigOption {
 public:
  ScopedConfigOption(const char* key, const char* value) : key_(key) {
    CPLSetThreadLocalConfigOption(key_, value);
  }
  ~ScopedConfigOption() { CPLSetThreadLocalConfigOption(key_, nullptr); }

 private:
  const char* key_;
};

// libhdf4's SDend() names the root CDF0.0 Vgroup after the full file path,
// while HDF-EOS SWattach()/GDattach() read Vgroup names into char name[80].
// Shortening CDF0.0's name prevents stack buffer overflows when TempDir() is
// long.
void ShortenCdfVgroupName(const char* path) {
  int32_t hdf_id = Hopen(path, DFACC_RDWR, 0);
  ASSERT_NE(hdf_id, -1);
  ASSERT_EQ(Vstart(hdf_id), 0);
  int32_t cdf_ref = Vfindclass(hdf_id, "CDF0.0");
  if (cdf_ref != -1) {
    int32_t vg_cdf = Vattach(hdf_id, cdf_ref, "w");
    if (vg_cdf != -1) {
      Vsetname(vg_cdf, "cdf");
      Vdetach(vg_cdf);
    }
  }
  ASSERT_EQ(Vend(hdf_id), 0);
  ASSERT_EQ(Hclose(hdf_id), 0);
}

// libhdf4's HXsetdir() (hextelt.c:1144) leaks strdup(newdir) whenever it is
// called consecutively with the same directory string. Resetting
// HXsetdir(nullptr) after each read avoids that upstream leak, provided the
// read does not span multiple blocks (which would invoke IReadBlock ->
// HXsetdir multiple times before HXsetdir(nullptr) runs).
CPLErr ReadRasterZeroLeak(GDALRasterBand* band, int xoff, int yoff, int xsize,
                          int ysize, void* data, int bufx, int bufy,
                          GDALDataType dtype) {
  int block_x = 0;
  int block_y = 0;
  band->GetBlockSize(&block_x, &block_y);
  if (block_x > 0 && block_y > 0 && xsize > 0 && ysize > 0) {
    EXPECT_EQ(xoff / block_x, (xoff + xsize - 1) / block_x);
    EXPECT_EQ(yoff / block_y, (yoff + ysize - 1) / block_y);
  }
  CPLErr err = band->RasterIO(GF_Read, xoff, yoff, xsize, ysize, data, bufx,
                              bufy, dtype, 0, 0);
  HXsetdir(nullptr);
  return err;
}

TEST(Hdf4ImageCreateTest, RejectsInvalidArgsAndHandlesAllDataTypes) {
  GDALAllRegister();
  WithQuietHandler quiet;
  GDALDriver* driver =
      GetGDALDriverManager()->GetDriverByName(HDF4_IMAGE_DRIVER_NAME);
  ASSERT_NE(driver, nullptr);

  // Zero bands is unsupported.
  TempFile tmp_zero("create_zero.hdf");
  EXPECT_EQ(driver->Create(tmp_zero.path(), 4, 3, 0, GDT_Byte, nullptr),
            nullptr);

  // Unwritable directory path fails cleanly.
  EXPECT_EQ(driver->Create("/nonexistent_dir_for_hdf4_test/out.hdf", 4, 3, 1,
                           GDT_Byte, nullptr),
            nullptr);

  // VSI path passes VSIFOpenL check but fails in SDstart (no OS path).
  EXPECT_EQ(driver->Create("/vsimem/cannot_create_via_sdstart.hdf", 4, 3, 1,
                           GDT_Byte, nullptr),
            nullptr);

  // Unsupported complex type falls back to Byte with a warning.
  {
    TempFile tmp_cfloat("create_cfloat.hdf");
    auto ds = std::unique_ptr<GDALDataset>(
        driver->Create(tmp_cfloat.path(), 4, 3, 1, GDT_CFloat32, nullptr));
    ASSERT_NE(ds, nullptr);
  }

  // Supported GDAL data types succeed.
  const GDALDataType supported_types[] = {
      GDT_Byte,  GDT_Int8,   GDT_Int16,   GDT_UInt16,
      GDT_Int32, GDT_UInt32, GDT_Float32, GDT_Float64,
  };
  for (GDALDataType dt : supported_types) {
    TempFile tmp_dt(absl::StrCat("create_dt_", static_cast<int>(dt), ".hdf"));
    auto ds = std::unique_ptr<GDALDataset>(
        driver->Create(tmp_dt.path(), 4, 3, 1, dt, nullptr));
    ASSERT_NE(ds, nullptr);
  }
}

TEST(Hdf4ImageCreateTest, CreateRank3AndRank2RoundTripMetadataAndPixels) {
  GDALAllRegister();
  GDALDriver* driver =
      GetGDALDriverManager()->GetDriverByName(HDF4_IMAGE_DRIVER_NAME);
  ASSERT_NE(driver, nullptr);

  // Rank 3 (default) with 2 bands, geotransform, SRS, metadata, nodata, desc.
  TempFile tmp_r3("create_rank3.hdf");
  {
    auto ds = std::unique_ptr<GDALDataset>(
        driver->Create(tmp_r3.path(), 4, 3, 2, GDT_Int16, nullptr));
    ASSERT_NE(ds, nullptr);

    // Before setting geotransform / SRS / GCPs.
    GDALGeoTransform gt_unset;
    EXPECT_EQ(ds->GetGeoTransform(gt_unset), CE_Failure);
    EXPECT_EQ(ds->GetSpatialRef(), nullptr);
    EXPECT_EQ(ds->GetGCPCount(), 0);
    EXPECT_EQ(ds->GetGCPSpatialRef(), nullptr);
    EXPECT_EQ(ds->GetGCPs(), nullptr);

    // Reading while in GA_Update mode zero-fills the buffer.
    std::vector<int16_t> update_read(12, 99);
    ASSERT_EQ(
        ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, 4, 3, update_read.data(),
                                       4, 3, GDT_Int16, 0, 0),
        CE_None);
    for (int16_t v : update_read) {
      EXPECT_EQ(v, 0);
    }

    GDALGeoTransform gt(100.0, 2.5, 0.0, 200.0, 0.0, -2.5);
    ASSERT_EQ(ds->SetGeoTransform(gt), CE_None);
    OGRSpatialReference srs;
    srs.SetWellKnownGeogCS("WGS84");
    ASSERT_EQ(ds->SetSpatialRef(&srs), CE_None);
    ds->SetMetadataItem("CUSTOM_TAG", "hello_hdf4");

    GDALRasterBand* b1 = ds->GetRasterBand(1);
    GDALRasterBand* b2 = ds->GetRasterBand(2);
    ASSERT_EQ(b1->SetNoDataValue(-999.0), CE_None);
    b1->SetDescription("First Band");
    b2->SetDescription("Second Band");

    int success = FALSE;
    EXPECT_DOUBLE_EQ(b1->GetNoDataValue(&success), -999.0);
    EXPECT_TRUE(success);
    EXPECT_DOUBLE_EQ(b2->GetNoDataValue(&success), -9999.0);
    EXPECT_FALSE(success);
    b1->GetScale(&success);
    EXPECT_FALSE(success);
    b1->GetOffset(&success);
    EXPECT_FALSE(success);
    EXPECT_STREQ(b1->GetUnitType(), "");
    EXPECT_EQ(b1->GetColorTable(), nullptr);
    EXPECT_EQ(b1->GetColorInterpretation(), GCI_GrayIndex);

    std::vector<int16_t> data1(12);
    std::vector<int16_t> data2(12);
    for (int i = 0; i < 12; ++i) {
      data1[i] = static_cast<int16_t>(10 + i);
      data2[i] = static_cast<int16_t>(100 + i);
    }
    ASSERT_EQ(
        b1->RasterIO(GF_Write, 0, 0, 4, 3, data1.data(), 4, 3, GDT_Int16, 0, 0),
        CE_None);
    ASSERT_EQ(
        b2->RasterIO(GF_Write, 0, 0, 4, 3, data2.data(), 4, 3, GDT_Int16, 0, 0),
        CE_None);
  }

  // Reopen the Rank 3 GDAL-created HDF4 dataset and verify round-trip.
  {
    const std::string sub_name =
        absl::StrCat("HDF4_SDS:GDAL_HDF4:\"", tmp_r3.str(), "\":0");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_name.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterXSize(), 4);
    EXPECT_EQ(ds->GetRasterYSize(), 3);
    EXPECT_EQ(ds->GetRasterCount(), 2);

    GDALGeoTransform gt;
    ASSERT_EQ(ds->GetGeoTransform(gt), CE_None);
    EXPECT_DOUBLE_EQ(gt[0], 100.0);
    EXPECT_DOUBLE_EQ(gt[1], 2.5);
    EXPECT_DOUBLE_EQ(gt[3], 200.0);
    EXPECT_DOUBLE_EQ(gt[5], -2.5);
    EXPECT_NE(ds->GetSpatialRef(), nullptr);
    EXPECT_STREQ(ds->GetMetadataItem("CUSTOM_TAG"), "hello_hdf4");

    GDALRasterBand* b1 = ds->GetRasterBand(1);
    EXPECT_STREQ(b1->GetDescription(), "First Band");
    int success = FALSE;
    EXPECT_DOUBLE_EQ(b1->GetNoDataValue(&success), -999.0);
    EXPECT_TRUE(success);

    std::vector<int16_t> out1(12, 0);
    ASSERT_EQ(ReadRasterZeroLeak(b1, 0, 0, 4, 3, out1.data(), 4, 3, GDT_Int16),
              CE_None);
    for (int i = 0; i < 12; ++i) {
      EXPECT_EQ(out1[i], 10 + i);
    }
  }

  // Rank 2 creation with 2 bands -> creates Band0 and Band1 SDSs.
  TempFile tmp_r2("create_rank2.hdf");
  {
    const char* opts[] = {"RANK=2", nullptr};
    auto ds = std::unique_ptr<GDALDataset>(driver->Create(
        tmp_r2.path(), 4, 3, 2, GDT_Float32, const_cast<char**>(opts)));
    ASSERT_NE(ds, nullptr);
    std::vector<float> buf(12, 3.25f);
    ASSERT_EQ(ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 3, buf.data(),
                                             4, 3, GDT_Float32, 0, 0),
              CE_None);
    ASSERT_EQ(ds->GetRasterBand(2)->RasterIO(GF_Write, 0, 0, 4, 3, buf.data(),
                                             4, 3, GDT_Float32, 0, 0),
              CE_None);
  }
  {
    const std::string sub_name =
        absl::StrCat("HDF4_SDS:GDAL_HDF4:\"", tmp_r2.str(), "\":1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_name.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterCount(), 1);
    std::vector<float> out(12, 0.0f);
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(1), 0, 0, 4, 3, out.data(),
                                 4, 3, GDT_Float32),
              CE_None);
    EXPECT_FLOAT_EQ(out[0], 3.25f);
  }
}

TEST(Hdf4ImageOpenErrorsTest, InvalidSubdatasetStringsAndOutOfRangeIndex) {
  GDALAllRegister();
  WithQuietHandler quiet;

  // Non-HDF4 string fails Identify.
  EXPECT_EQ(GDALDataset::Open("not_an_hdf4_subdataset",
                              GDAL_OF_RASTER | GDAL_OF_READONLY),
            nullptr);

  // Wrong token count (3 tokens instead of 4/5/6).
  EXPECT_EQ(GDALDataset::Open("HDF4_SDS:UNKNOWN:only_three",
                              GDAL_OF_RASTER | GDAL_OF_READONLY),
            nullptr);

  // Windows drive-letter style path (1-char component) + non-existent file.
  EXPECT_EQ(GDALDataset::Open("HDF4_SDS:UNKNOWN:C:\\nonexistent\\test.hdf:0",
                              GDAL_OF_RASTER | GDAL_OF_READONLY),
            nullptr);

  TempFile tmp("open_errors.hdf");
  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    int32_t dims[2] = {3, 4};
    int32_t sds = SDcreate(sd_id, "grid2d", DFNT_UINT8, 2, dims);
    int32_t start[2] = {0, 0};
    std::vector<uint8_t> buf(12, 1);
    SDwritedata(sds, start, nullptr, dims, buf.data());
    SDendaccess(sds);
    ASSERT_EQ(SDend(sd_id), 0);
  }

  // Out-of-range SDS index.
  const std::string bad_idx =
      absl::StrCat("HDF4_SDS:UNKNOWN:\"", tmp.str(), "\":99");
  EXPECT_EQ(
      GDALDataset::Open(bad_idx.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY),
      nullptr);
}

TEST(Hdf4ImageSdsTest, RanksDimensionOrdersAndBlockPixels) {
  GDALAllRegister();
  TempFile tmp("sds_ranks.hdf");

  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);

    // 0: Rank 1 [6]
    {
      int32_t dims[1] = {6};
      int32_t sds = SDcreate(sd_id, "sds_1d", DFNT_INT16, 1, dims);
      int32_t start[1] = {0};
      int16_t buf[6] = {10, 20, 30, 40, 50, 60};
      SDwritedata(sds, start, nullptr, dims, buf);
      SDendaccess(sds);
    }
    // 1: Rank 2 [4, 6]
    {
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "sds_2d", DFNT_UINT8, 2, dims);
      int32_t start[2] = {0, 0};
      std::vector<uint8_t> buf(24, 7);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      SDendaccess(sds);
    }
    // 2: Rank 3 [2, 4, 6] -> dim[0] < dim[2] => band=0, y=1, x=2
    {
      int32_t dims[3] = {2, 4, 6};
      int32_t sds = SDcreate(sd_id, "sds_3d_byx", DFNT_UINT8, 3, dims);
      int32_t start[3] = {0, 0, 0};
      std::vector<uint8_t> buf(48, 3);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      SDendaccess(sds);
    }
    // 3: Rank 3 [5, 2, 4] -> dim[0] >= dim[2] and dim[1] <= dim[0],dim[2] =>
    // band=1, y=0, x=2
    {
      int32_t dims[3] = {5, 2, 4};
      int32_t sds = SDcreate(sd_id, "sds_3d_ybx", DFNT_UINT8, 3, dims);
      int32_t start[3] = {0, 0, 0};
      std::vector<uint8_t> buf(40, 4);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      SDendaccess(sds);
    }
    // 4: Rank 3 [5, 6, 2] -> dim[0] >= dim[2] and dim[2] < dim[1] =>
    // band=2, y=0, x=1
    {
      int32_t dims[3] = {5, 6, 2};
      int32_t sds = SDcreate(sd_id, "sds_3d_yxb", DFNT_UINT8, 3, dims);
      int32_t start[3] = {0, 0, 0};
      std::vector<uint8_t> buf(60, 5);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      SDendaccess(sds);
    }
    // 5: Rank 4 [4, 4, 2, 2] -> nXSize=4, nYSize=4, nBands=4
    {
      int32_t dims[4] = {4, 4, 2, 2};
      int32_t sds = SDcreate(sd_id, "sds_4d", DFNT_UINT8, 4, dims);
      int32_t start[4] = {0, 0, 0, 0};
      std::vector<uint8_t> buf(64, 9);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      SDendaccess(sds);
    }
    ASSERT_EQ(SDend(sd_id), 0);
  }

  // Test Rank 1
  {
    const std::string sub =
        absl::StrCat("HDF4_SDS:UNKNOWN:\"", tmp.str(), "\":0");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterXSize(), 6);
    EXPECT_EQ(ds->GetRasterYSize(), 1);
    int16_t out[6] = {};
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(1), 0, 0, 6, 1, out, 6, 1,
                                 GDT_Int16),
              CE_None);
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[5], 60);
  }

  // Test Rank 2 with HDF4_BLOCK_PIXELS=12 (2 rows per block) + GA_Update open.
  {
    ScopedConfigOption block_pixels("HDF4_BLOCK_PIXELS", "12");
    const std::string sub =
        absl::StrCat("HDF4_SDS:UNKNOWN:\"", tmp.str(), "\":1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    int bx = 0;
    int by = 0;
    ds->GetRasterBand(1)->GetBlockSize(&bx, &by);
    EXPECT_EQ(bx, 6);
    EXPECT_EQ(by, 2);
    std::vector<uint8_t> out(24, 0);
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(1), 0, 0, 6, 2, out.data(),
                                 6, 2, GDT_Byte),
              CE_None);
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(1), 0, 2, 6, 2,
                                 out.data() + 12, 6, 2, GDT_Byte),
              CE_None);
    EXPECT_EQ(out[0], 7);
    EXPECT_EQ(out[23], 7);

    auto ds_upd = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(ds_upd, nullptr);
  }

  // Test Rank 3 dimension orders (indices 2, 3, 4) and Rank 4 (index 5).
  const int expected_bands[] = {2, 2, 2, 4};
  const int expected_x[] = {6, 4, 6, 4};
  const int expected_y[] = {4, 5, 5, 4};
  for (int idx = 2; idx <= 5; ++idx) {
    const std::string sub =
        absl::StrCat("HDF4_SDS:UNKNOWN:\"", tmp.str(), "\":", idx);
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterCount(), expected_bands[idx - 2]);
    EXPECT_EQ(ds->GetRasterXSize(), expected_x[idx - 2]);
    EXPECT_EQ(ds->GetRasterYSize(), expected_y[idx - 2]);
    uint8_t val = 0;
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(1), 0, 0, 1, 1, &val, 1, 1,
                                 GDT_Byte),
              CE_None);
    EXPECT_GT(val, 0);
  }
}

TEST(Hdf4ImageSdsProductsTest, SeaWiFSHyperionCoastWatchNrlModisAndLandsat) {
  GDALAllRegister();

  // 1. SeaWiFS L3 (Equidistant Cylindrical, northern hemisphere).
  {
    TempFile tmp("seawifs_eqc.hdf");
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "Title", "SeaWiFS Level-3 Standard Mapped Image");
    SetCharAttr(sd_id, "Parameter", "Chlorophyll a");
    SetCharAttr(sd_id, "Map Projection", "Equidistant Cylindrical");
    SetCharAttr(sd_id, "Westernmost Longitude", "-10.0");
    SetCharAttr(sd_id, "Easternmost Longitude", "10.0");
    SetCharAttr(sd_id, "Northernmost Latitude", "20.0");
    SetCharAttr(sd_id, "Southernmost Latitude", "-20.0");
    int32_t dims[2] = {4, 8};
    int32_t sds = SDcreate(sd_id, "l3m_data", DFNT_FLOAT32, 2, dims);
    int32_t start[2] = {0, 0};
    std::vector<float> buf(32, 1.5f);
    SDwritedata(sds, start, nullptr, dims, buf.data());
    SDendaccess(sds);
    ASSERT_EQ(SDend(sd_id), 0);

    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_STREQ(ds->GetRasterBand(1)->GetDescription(), "Chlorophyll a");
    GDALGeoTransform gt;
    ASSERT_EQ(ds->GetGeoTransform(gt), CE_None);
    EXPECT_LT(gt[5], 0.0);
  }

  // 2. Hyperion L1 (Rank 3 and Rank 2).
  {
    TempFile tmp("hyperion_l1.hdf");
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "Title", "Hyperion Level 1");
    int32_t dims3[3] = {4, 3, 6};  // Height x Bands x Width
    int32_t sds3 = SDcreate(sd_id, "hyp3", DFNT_INT16, 3, dims3);
    int32_t start3[3] = {0, 0, 0};
    std::vector<int16_t> buf3(72, 11);
    SDwritedata(sds3, start3, nullptr, dims3, buf3.data());
    SDendaccess(sds3);

    int32_t dims2[2] = {3, 6};  // Bands x Width (Height = 1)
    int32_t sds2 = SDcreate(sd_id, "hyp2", DFNT_INT16, 2, dims2);
    int32_t start2[2] = {0, 0};
    std::vector<int16_t> buf2(18, 22);
    SDwritedata(sds2, start2, nullptr, dims2, buf2.data());
    SDendaccess(sds2);
    ASSERT_EQ(SDend(sd_id), 0);

    const std::string sub3 =
        absl::StrCat("HDF4_SDS:HYPERION_L1:\"", tmp.str(), "\":0");
    auto ds3 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub3.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds3, nullptr);
    EXPECT_EQ(ds3->GetRasterYSize(), 4);
    EXPECT_EQ(ds3->GetRasterCount(), 3);
    EXPECT_EQ(ds3->GetRasterXSize(), 6);

    const std::string sub2 =
        absl::StrCat("HDF4_SDS:HYPERION_L1:\"", tmp.str(), "\":1");
    auto ds2 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub2.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds2, nullptr);
    EXPECT_EQ(ds2->GetRasterYSize(), 1);
    EXPECT_EQ(ds2->GetRasterCount(), 3);
    EXPECT_EQ(ds2->GetRasterXSize(), 6);
  }

  // 3. CoastWatch + ACT MODIS conventions + NRL(USGS) + MODIS SDS Geolocation.
  {
    TempFile tmp("coastwatch_nrl_modis.hdf");
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "mapProjectionSystem", "NRL(USGS)");
    SetCharAttr(sd_id, "mapUpperLeft", "31, -99");
    SetCharAttr(sd_id, "mapUpperRight", "31, -79");
    SetCharAttr(sd_id, "mapLowerLeft", "15, -99");
    SetCharAttr(sd_id, "mapLowerRight", "15, -79");
    SetCharAttr(sd_id, "mapProjection", "Gomex");

    SetCharAttr(sd_id, "gctp_sys", "16");
    SetCharAttr(sd_id, "gctp_zone", "62");
    SetCharAttr(sd_id, "gctp_parm",
                "6378137, 0, 0, 0, -89000000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0");
    SetCharAttr(sd_id, "gctp_datum", "12");
    SetCharAttr(sd_id, "et_affine", "0, -1000, 1000, 0, -950000, 3440000");

    // 0: Main data SDS with CoastWatch + ACT attributes.
    int32_t dims[2] = {4, 6};
    int32_t sds = SDcreate(sd_id, "sst", DFNT_INT16, 2, dims);
    SetCharAttr(sds, "missing_value", "-32768");
    SetCharAttr(sds, "scale_factor", "0.01");
    SetCharAttr(sds, "add_offset", "5.0");
    SetCharAttr(sds, "scalingSlope", "0.25");
    SetCharAttr(sds, "scalingIntercept", "2.0");
    SetCharAttr(sds, "productUnits", "degC");
    int32_t start[2] = {0, 0};
    std::vector<int16_t> buf(24, 100);
    SDwritedata(sds, start, nullptr, dims, buf.data());
    SDendaccess(sds);

    // 1 & 2: latitude and longitude SDSs for ProcessModisSDSGeolocation.
    int32_t gdims[2] = {2, 3};
    int32_t sds_lat = SDcreate(sd_id, "latitude", DFNT_FLOAT32, 2, gdims);
    int32_t gstart[2] = {0, 0};
    std::vector<float> gbuf(6, 25.0f);
    SDwritedata(sds_lat, gstart, nullptr, gdims, gbuf.data());
    SDendaccess(sds_lat);

    int32_t sds_lon = SDcreate(sd_id, "longitude", DFNT_FLOAT32, 2, gdims);
    SDwritedata(sds_lon, gstart, nullptr, gdims, gbuf.data());
    SDendaccess(sds_lon);

    // 3: 1D 29-double GCTP parameter SDS named "Gomex" for NRL projection.
    int32_t pdims[1] = {29};
    int32_t sds_proj = SDcreate(sd_id, "Gomex", DFNT_FLOAT64, 1, pdims);
    int32_t pstart[1] = {0};
    std::vector<double> gctp_params(29, 0.0);
    gctp_params[1] = 1.0;   // UTM
    gctp_params[2] = 16.0;  // Zone 16
    gctp_params[3] = 8.0;   // WGS84
    SDwritedata(sds_proj, pstart, nullptr, pdims, gctp_params.data());
    SDendaccess(sds_proj);

    ASSERT_EQ(SDend(sd_id), 0);

    const std::string sub =
        absl::StrCat("HDF4_SDS:UNKNOWN:\"", tmp.str(), "\":0");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);

    GDALRasterBand* band = ds->GetRasterBand(1);
    int success = FALSE;
    EXPECT_DOUBLE_EQ(band->GetNoDataValue(&success), -32768.0);
    EXPECT_TRUE(success);
    EXPECT_DOUBLE_EQ(band->GetScale(&success), 0.25);
    EXPECT_TRUE(success);
    EXPECT_DOUBLE_EQ(band->GetOffset(&success), 2.0);
    EXPECT_TRUE(success);
    EXPECT_STREQ(band->GetUnitType(), "degC");

    GDALGeoTransform gt;
    EXPECT_EQ(ds->GetGeoTransform(gt), CE_None);
    EXPECT_NE(ds->GetSpatialRef(), nullptr);
    EXPECT_NE(ds->GetMetadata("GEOLOCATION"), nullptr);
    EXPECT_STREQ(ds->GetMetadataItem("PIXEL_STEP", "GEOLOCATION"), "2");
    EXPECT_STREQ(ds->GetMetadataItem("LINE_STEP", "GEOLOCATION"), "2");

    // Opening the "latitude" SDS itself skips MODIS geolocation attachment.
    const std::string sub_lat =
        absl::StrCat("HDF4_SDS:UNKNOWN:\"", tmp.str(), "\":1");
    auto ds_lat = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_lat.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_lat, nullptr);
    EXPECT_EQ(ds_lat->GetMetadata("GEOLOCATION"), nullptr);
  }

  // 3b. Edge cases for NRL, CoastWatch, and MODIS geolocation dimensions.
  {
    // NRL missing mapUpperLeft.
    TempFile tmp_nrl1("nrl_missing_ul.hdf");
    {
      int32_t sd_id = SDstart(tmp_nrl1.path(), DFACC_CREATE);
      SetCharAttr(sd_id, "mapProjectionSystem", "NRL(USGS)");
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "d", DFNT_UINT8, 2, dims);
      SDendaccess(sds);
      SDend(sd_id);
    }
    auto ds_nrl1 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp_nrl1.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_nrl1, nullptr);
    GDALGeoTransform gt;
    EXPECT_NE(ds_nrl1->GetGeoTransform(gt), CE_None);

    // NRL 1-token mapUpperLeft.
    TempFile tmp_nrl2("nrl_bad_tokens.hdf");
    {
      int32_t sd_id = SDstart(tmp_nrl2.path(), DFACC_CREATE);
      SetCharAttr(sd_id, "mapProjectionSystem", "NRL(USGS)");
      SetCharAttr(sd_id, "mapUpperLeft", "31");
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "d", DFNT_UINT8, 2, dims);
      SDendaccess(sds);
      SDend(sd_id);
    }
    auto ds_nrl2 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(tmp_nrl2.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_nrl2, nullptr);

    // NRL out-of-range lat/lon corners + mismatched MODIS lat/lon dimensions.
    TempFile tmp_nrl3("nrl_oob_modis_mismatch.hdf");
    {
      int32_t sd_id = SDstart(tmp_nrl3.path(), DFACC_CREATE);
      SetCharAttr(sd_id, "mapProjectionSystem", "NRL(USGS)");
      SetCharAttr(sd_id, "mapUpperLeft", "500, 500");
      SetCharAttr(sd_id, "mapUpperRight", "500, 510");
      SetCharAttr(sd_id, "mapLowerLeft", "490, 500");
      SetCharAttr(sd_id, "mapLowerRight", "490, 510");
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "sst", DFNT_UINT8, 2, dims);
      SDendaccess(sds);
      int32_t lat_dims[2] = {2, 3};
      int32_t sds_lat = SDcreate(sd_id, "latitude", DFNT_FLOAT32, 2, lat_dims);
      SDendaccess(sds_lat);
      int32_t lon_dims[2] = {2, 4};
      int32_t sds_lon = SDcreate(sd_id, "longitude", DFNT_FLOAT32, 2, lon_dims);
      SDendaccess(sds_lon);
      SDend(sd_id);
    }
    const std::string sub_nrl3 =
        absl::StrCat("HDF4_SDS:UNKNOWN:\"", tmp_nrl3.str(), "\":0");
    auto ds_nrl3 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_nrl3.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_nrl3, nullptr);
    EXPECT_NE(ds_nrl3->GetMetadata("GEOLOCATION"), nullptr);

    // CoastWatch error branches: missing attr, short gctp_parm, invalid
    // gctp_sys, short et_affine, non-zero et_affine[0].
    const char* cw_variants[][5] = {
        {"16", nullptr, "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0", "12", "0,-1,1,0,0,0"},
        {"16", "62", "0,0,0", "12", "0,-1,1,0,0,0"},
        {"999", "62", "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0", "12", "0,-1,1,0,0,0"},
        {"16", "62", "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0", "12", "0,-1,1"},
        {"16", "62", "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0", "12", "1,-1,1,0,0,0"},
    };
    for (int i = 0; i < 5; ++i) {
      TempFile tmp_cw(absl::StrCat("cw_err_", i, ".hdf"));
      int32_t sd_id = SDstart(tmp_cw.path(), DFACC_CREATE);
      SetCharAttr(sd_id, "gctp_sys", cw_variants[i][0]);
      if (cw_variants[i][1] != nullptr) {
        SetCharAttr(sd_id, "gctp_zone", cw_variants[i][1]);
      }
      SetCharAttr(sd_id, "gctp_parm", cw_variants[i][2]);
      SetCharAttr(sd_id, "gctp_datum", cw_variants[i][3]);
      SetCharAttr(sd_id, "et_affine", cw_variants[i][4]);
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "d", DFNT_UINT8, 2, dims);
      SDendaccess(sds);
      SDend(sd_id);
      auto ds_cw = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(tmp_cw.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds_cw, nullptr);
    }
  }

  // 4. Landsat L1G (_HDF.L1G + _MTL.L1G with LPGS_METADATA_FILE &
  // L1_METADATA_FILE).
  {
    TempFile tmp_hdf("landsat_test_HDF.L1G");
    TempFile tmp_mtl("landsat_test_MTL.L1G");

    int32_t sd_id = SDstart(tmp_hdf.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    int32_t dims[2] = {4, 6};
    int32_t sds = SDcreate(sd_id, "band1", DFNT_UINT8, 2, dims);
    int32_t start[2] = {0, 0};
    std::vector<uint8_t> buf(24, 50);
    SDwritedata(sds, start, nullptr, dims, buf.data());
    SDendaccess(sds);
    ASSERT_EQ(SDend(sd_id), 0);

    // Missing _MTL.L1G file.
    {
      auto ds = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(tmp_hdf.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 0);
    }

    // Empty _MTL.L1G file (Ingest fails).
    {
      VSILFILE* fp_empty = VSIFOpenL(tmp_mtl.path(), "wb");
      ASSERT_NE(fp_empty, nullptr);
      VSIFCloseL(fp_empty);
      auto ds = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(tmp_hdf.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 0);
    }

    // _MTL.L1G without PRODUCT_UL_CORNER_LON.
    {
      const std::string mtl_no_ul =
          "GROUP = OTHER_METADATA\n"
          "END_GROUP = OTHER_METADATA\n"
          "END\n";
      VSILFILE* fp_other = VSIFOpenL(tmp_mtl.path(), "wb");
      ASSERT_NE(fp_other, nullptr);
      VSIFWriteL(mtl_no_ul.data(), 1, mtl_no_ul.size(), fp_other);
      VSIFCloseL(fp_other);
      auto ds = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(tmp_hdf.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 0);
    }

    const std::string mtl_lpgs =
        "GROUP = LPGS_METADATA_FILE\n"
        "  GROUP = PRODUCT_METADATA\n"
        "    PRODUCT_UL_CORNER_LAT = 51.27\n"
        "    PRODUCT_UL_CORNER_LON = -53.89\n"
        "    PRODUCT_UR_CORNER_LAT = 50.84\n"
        "    PRODUCT_UR_CORNER_LON = -50.98\n"
        "    PRODUCT_LL_CORNER_LAT = 49.69\n"
        "    PRODUCT_LL_CORNER_LON = -54.40\n"
        "    PRODUCT_LR_CORNER_LAT = 49.28\n"
        "    PRODUCT_LR_CORNER_LON = -51.59\n"
        "  END_GROUP = PRODUCT_METADATA\n"
        "END_GROUP = LPGS_METADATA_FILE\n"
        "END\n";
    VSILFILE* fp = VSIFOpenL(tmp_mtl.path(), "wb");
    ASSERT_NE(fp, nullptr);
    ASSERT_EQ(VSIFWriteL(mtl_lpgs.data(), 1, mtl_lpgs.size(), fp),
              mtl_lpgs.size());
    VSIFCloseL(fp);

    {
      auto ds = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(tmp_hdf.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 4);
      EXPECT_NE(ds->GetGCPs(), nullptr);
      // Note: GetGCPSpatialRef() checks m_oSRS.IsEmpty() || m_aoGCPs.empty()
      // (returns nullptr when m_oSRS is empty, or non-null when m_oSRS is set).
      EXPECT_EQ(ds->GetGCPSpatialRef(), nullptr);
    }

    // Also test L1_METADATA_FILE variant.
    const std::string mtl_l1 =
        "GROUP = L1_METADATA_FILE\n"
        "  GROUP = PRODUCT_METADATA\n"
        "    PRODUCT_UL_CORNER_LAT = 41.0\n"
        "    PRODUCT_UL_CORNER_LON = -73.0\n"
        "    PRODUCT_UR_CORNER_LAT = 41.0\n"
        "    PRODUCT_UR_CORNER_LON = -72.0\n"
        "    PRODUCT_LL_CORNER_LAT = 40.0\n"
        "    PRODUCT_LL_CORNER_LON = -73.0\n"
        "    PRODUCT_LR_CORNER_LAT = 40.0\n"
        "    PRODUCT_LR_CORNER_LON = -72.0\n"
        "  END_GROUP = PRODUCT_METADATA\n"
        "END_GROUP = L1_METADATA_FILE\n"
        "END\n";
    fp = VSIFOpenL(tmp_mtl.path(), "wb");
    ASSERT_NE(fp, nullptr);
    ASSERT_EQ(VSIFWriteL(mtl_l1.data(), 1, mtl_l1.size(), fp), mtl_l1.size());
    VSIFCloseL(fp);

    {
      auto ds = std::unique_ptr<GDALDataset>(
          GDALDataset::Open(tmp_hdf.path(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds, nullptr);
      EXPECT_EQ(ds->GetGCPCount(), 4);
    }
  }
}

TEST(Hdf4ImageGrTest, GrayMultiBandAndPalettedGeneralRasters) {
  GDALAllRegister();
  TempFile tmp("gr_images.hdf");

  {
    int32_t hdf_id = Hopen(tmp.path(), DFACC_CREATE, 0);
    ASSERT_NE(hdf_id, -1);
    int32_t gr_id = GRstart(hdf_id);
    ASSERT_NE(gr_id, -1);

    // 0: 1-band grayscale GR.
    {
      int32_t dims[2] = {4, 3};
      int32_t ri =
          GRcreate(gr_id, "gray_gr", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
      ASSERT_NE(ri, -1);
      int32_t start[2] = {0, 0};
      std::vector<uint8_t> data(12, 42);
      ASSERT_EQ(GRwriteimage(ri, start, nullptr, dims, data.data()), 0);
      ASSERT_EQ(GRendaccess(ri), 0);
    }

    // 1: 5-band GR (Red, Green, Blue, Alpha, Undefined).
    {
      int32_t dims[2] = {4, 3};
      int32_t ri = GRcreate(gr_id, "rgba_plus_gr", 5, DFNT_UINT16,
                            MFGR_INTERLACE_PIXEL, dims);
      ASSERT_NE(ri, -1);
      int32_t start[2] = {0, 0};
      std::vector<uint16_t> data(12 * 5);
      for (int i = 0; i < 12; ++i) {
        data[i * 5 + 0] = static_cast<uint16_t>(10 + i);
        data[i * 5 + 1] = static_cast<uint16_t>(20 + i);
        data[i * 5 + 2] = static_cast<uint16_t>(30 + i);
        data[i * 5 + 3] = 255;
        data[i * 5 + 4] = 99;
      }
      ASSERT_EQ(GRwriteimage(ri, start, nullptr, dims, data.data()), 0);
      ASSERT_EQ(GRendaccess(ri), 0);
    }

    // 2: 1-band paletted GR with 256-entry RGB LUT.
    {
      int32_t dims[2] = {4, 3};
      int32_t ri =
          GRcreate(gr_id, "pal_gr", 1, DFNT_UINT8, MFGR_INTERLACE_PIXEL, dims);
      ASSERT_NE(ri, -1);
      int32_t start[2] = {0, 0};
      std::vector<uint8_t> data(12, 5);
      ASSERT_EQ(GRwriteimage(ri, start, nullptr, dims, data.data()), 0);

      int32_t lut_id = GRgetlutid(ri, 0);
      ASSERT_NE(lut_id, -1);
      uint8_t pal[256][3];
      for (int i = 0; i < 256; ++i) {
        pal[i][0] = static_cast<uint8_t>(i);
        pal[i][1] = static_cast<uint8_t>(255 - i);
        pal[i][2] = static_cast<uint8_t>(i / 2);
      }
      ASSERT_EQ(
          GRwritelut(lut_id, 3, DFNT_UINT8, MFGR_INTERLACE_PIXEL, 256, pal), 0);
      ASSERT_EQ(GRendaccess(ri), 0);
    }

    ASSERT_EQ(GRend(gr_id), 0);
    ASSERT_EQ(Hclose(hdf_id), 0);
  }

  // Verify 0: 1-band grayscale.
  {
    const std::string sub =
        absl::StrCat("HDF4_GR:UNKNOWN:\"", tmp.str(), "\":0");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetRasterBand(1)->GetColorInterpretation(), GCI_GrayIndex);
  }

  // Verify 1: 5-band GR + GA_Update open.
  {
    const std::string sub =
        absl::StrCat("HDF4_GR:UNKNOWN:\"", tmp.str(), "\":1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    ASSERT_EQ(ds->GetRasterCount(), 5);
    EXPECT_EQ(ds->GetRasterBand(1)->GetColorInterpretation(), GCI_RedBand);
    EXPECT_EQ(ds->GetRasterBand(2)->GetColorInterpretation(), GCI_GreenBand);
    EXPECT_EQ(ds->GetRasterBand(3)->GetColorInterpretation(), GCI_BlueBand);
    EXPECT_EQ(ds->GetRasterBand(4)->GetColorInterpretation(), GCI_AlphaBand);
    EXPECT_EQ(ds->GetRasterBand(5)->GetColorInterpretation(), GCI_Undefined);

    uint16_t b3_row0[4] = {};
    uint16_t b3_row2[4] = {};
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(3), 0, 0, 4, 1, b3_row0, 4,
                                 1, GDT_UInt16),
              CE_None);
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(3), 0, 2, 4, 1, b3_row2, 4,
                                 1, GDT_UInt16),
              CE_None);
    EXPECT_EQ(b3_row0[0], 30);
    EXPECT_EQ(b3_row2[3], 41);

    auto ds_upd = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE));
    ASSERT_NE(ds_upd, nullptr);
  }

  // Verify 2: paletted GR.
  {
    const std::string sub =
        absl::StrCat("HDF4_GR:UNKNOWN:\"", tmp.str(), "\":2");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    GDALRasterBand* band = ds->GetRasterBand(1);
    EXPECT_EQ(band->GetColorInterpretation(), GCI_PaletteIndex);
    GDALColorTable* ct = band->GetColorTable();
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(ct->GetColorEntryCount(), 256);
    const GDALColorEntry* entry = ct->GetColorEntry(5);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->c1, 5);
    EXPECT_EQ(entry->c2, 250);
  }

  // Out-of-range GR subdataset index and HDF4_UNKNOWN dataset type.
  {
    WithQuietHandler quiet;
    const std::string bad_gr =
        absl::StrCat("HDF4_GR:UNKNOWN:\"", tmp.str(), "\":99");
    EXPECT_EQ(
        GDALDataset::Open(bad_gr.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY),
        nullptr);
    const std::string unknown_type =
        absl::StrCat("HDF4_UNKNOWN:UNKNOWN:\"", tmp.str(), "\":0");
    EXPECT_EQ(GDALDataset::Open(unknown_type.c_str(),
                                GDAL_OF_RASTER | GDAL_OF_READONLY),
              nullptr);
  }
}

TEST(Hdf4ImageEosGridTest, ProjectedTiled3D4DAndGeographicGrids) {
  GDALAllRegister();
  TempFile tmp("eos_grids.hdf");

  const std::string struct_meta =
      "GROUP=SwathStructure\n"
      "END_GROUP=SwathStructure\n"
      "GROUP=GridStructure\n"
      "\tGROUP=GRID_1\n"
      "\t\tGridName=\"UTMGrid\"\n"
      "\t\tXDim=6\n"
      "\t\tYDim=4\n"
      "\t\tUpperLeftPointMtrs=(500000.000000,4000000.000000)\n"
      "\t\tLowerRightMtrs=(506000.000000,3996000.000000)\n"
      "\t\tProjection=GCTP_UTM\n"
      "\t\tZoneCode=16\n"
      "\t\tSphereCode=8\n"
      "\t\tGROUP=Dimension\n"
      "\t\t\tOBJECT=Dimension_1\n"
      "\t\t\t\tDimensionName=\"band_dim\"\n"
      "\t\t\t\tSize=2\n"
      "\t\t\tEND_OBJECT=Dimension_1\n"
      "\t\t\tOBJECT=Dimension_2\n"
      "\t\t\t\tDimensionName=\"TimeDim\"\n"
      "\t\t\t\tSize=2\n"
      "\t\t\tEND_OBJECT=Dimension_2\n"
      "\t\tEND_GROUP=Dimension\n"
      "\t\tGROUP=DataField\n"
      "\t\t\tOBJECT=DataField_1\n"
      "\t\t\t\tDataFieldName=\"Tiled2D\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_1\n"
      "\t\t\tOBJECT=DataField_2\n"
      "\t\t\t\tDataFieldName=\"RowTiled2D\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_2\n"
      "\t\t\tOBJECT=DataField_3\n"
      "\t\t\t\tDataFieldName=\"Field3D\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"band_dim\",\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_3\n"
      "\t\t\tOBJECT=DataField_4\n"
      "\t\t\t\tDataFieldName=\"Field4D\"\n"
      "\t\t\t\tDataType=DFNT_UINT8\n"
      "\t\t\t\tDimList=(\"band_dim\",\"TimeDim\",\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_4\n"
      "\t\t\tOBJECT=DataField_5\n"
      "\t\t\t\tDataFieldName=\"Field3DMid\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"YDim\",\"TimeDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_5\n"
      "\t\t\tOBJECT=DataField_6\n"
      "\t\t\t\tDataFieldName=\"Field3DLast\"\n"
      "\t\t\t\tDataType=DFNT_FLOAT32\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\",\"TimeDim\")\n"
      "\t\t\tEND_OBJECT=DataField_6\n"
      "\t\tEND_GROUP=DataField\n"
      "\t\tGROUP=MergedFields\n"
      "\t\tEND_GROUP=MergedFields\n"
      "\tEND_GROUP=GRID_1\n"
      "\tGROUP=GRID_2\n"
      "\t\tGridName=\"GeoGrid\"\n"
      "\t\tXDim=6\n"
      "\t\tYDim=4\n"
      "\t\tUpperLeftPointMtrs=(-100000000.000000,45000000.000000)\n"
      "\t\tLowerRightMtrs=(-90000000.000000,35000000.000000)\n"
      "\t\tProjection=GCTP_GEO\n"
      "\t\tGROUP=Dimension\n"
      "\t\tEND_GROUP=Dimension\n"
      "\t\tGROUP=DataField\n"
      "\t\t\tOBJECT=DataField_1\n"
      "\t\t\t\tDataFieldName=\"Geo2D\"\n"
      "\t\t\t\tDataType=DFNT_INT16\n"
      "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
      "\t\t\tEND_OBJECT=DataField_1\n"
      "\t\tEND_GROUP=DataField\n"
      "\t\tGROUP=MergedFields\n"
      "\t\tEND_GROUP=MergedFields\n"
      "\tEND_GROUP=GRID_2\n"
      "END_GROUP=GridStructure\n"
      "GROUP=PointStructure\n"
      "END_GROUP=PointStructure\n"
      "END\n";

  {
    int32_t sd_id = SDstart(tmp.path(), DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "HDFEOSVersion", "HDFEOS_V2.12");

    int32_t hdf_id = Hopen(tmp.path(), DFACC_RDWR, 0);
    ASSERT_NE(hdf_id, -1);
    ASSERT_EQ(Vstart(hdf_id), 0);

    std::string padded = struct_meta;
    padded.resize(32000, '\0');
    ASSERT_EQ(
        SDsetattr(sd_id, "StructMetadata.0", DFNT_CHAR8, 32000, padded.data()),
        0);

    // 1. Tiled2D: 4x6 with 2x6 chunks (matches nBlockXSize=6, tile height=2).
    int32_t tiled_ref = 0;
    {
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "Tiled2D", DFNT_INT16, 2, dims);
      HDF_CHUNK_DEF cdef;
      cdef.chunk_lengths[0] = 2;
      cdef.chunk_lengths[1] = 6;
      ASSERT_EQ(SDsetchunk(sds, cdef, HDF_CHUNK), 0);
      SetCharAttr(sds, "_FillValue", "-999");
      SetCharAttr(sds, "scale_factor", "0");  // Tests scale_factor=0 -> 1.0
      SetCharAttr(sds, "add_offset", "12.5");
      SetCharAttr(sds, "units", "Kelvin");
      SetCharAttr(sds, "long_name", "Tiled Temperature");
      int32_t start[2] = {0, 0};
      std::vector<int16_t> buf(24, 77);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      tiled_ref = SDidtoref(sds);
      SDendaccess(sds);
    }

    // 2. RowTiled2D: 4x6 with 1x6 chunks (nBlockPreferredYSize == 1).
    int32_t row_tiled_ref = 0;
    {
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "RowTiled2D", DFNT_INT16, 2, dims);
      HDF_CHUNK_DEF cdef;
      cdef.chunk_lengths[0] = 1;
      cdef.chunk_lengths[1] = 6;
      ASSERT_EQ(SDsetchunk(sds, cdef, HDF_CHUNK), 0);
      int32_t start[2] = {0, 0};
      std::vector<int16_t> buf(24, 33);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      row_tiled_ref = SDidtoref(sds);
      SDendaccess(sds);
    }

    // 3. Field3D: 2x4x6
    int32_t f3d_ref = 0;
    {
      int32_t dims[3] = {2, 4, 6};
      int32_t sds = SDcreate(sd_id, "Field3D", DFNT_FLOAT32, 3, dims);
      int32_t start[3] = {0, 0, 0};
      std::vector<float> buf(48, 4.5f);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      f3d_ref = SDidtoref(sds);
      SDendaccess(sds);
    }

    // 4. Field4D: 2x2x4x6
    int32_t f4d_ref = 0;
    {
      int32_t dims[4] = {2, 2, 4, 6};
      int32_t sds = SDcreate(sd_id, "Field4D", DFNT_UINT8, 4, dims);
      int32_t start[4] = {0, 0, 0, 0};
      std::vector<uint8_t> buf(96, 8);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      f4d_ref = SDidtoref(sds);
      SDendaccess(sds);
    }

    // 5. Geo2D: 4x6
    int32_t geo_ref = 0;
    {
      int32_t dims[2] = {4, 6};
      int32_t sds = SDcreate(sd_id, "Geo2D", DFNT_INT16, 2, dims);
      int32_t start[2] = {0, 0};
      std::vector<int16_t> buf(24, 55);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      geo_ref = SDidtoref(sds);
      SDendaccess(sds);
    }

    // 6. Field3DMid: 4x2x6 (band dim in middle)
    int32_t f3d_mid_ref = 0;
    {
      int32_t dims[3] = {4, 2, 6};
      int32_t sds = SDcreate(sd_id, "Field3DMid", DFNT_FLOAT32, 3, dims);
      int32_t start[3] = {0, 0, 0};
      std::vector<float> buf(48, 2.5f);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      f3d_mid_ref = SDidtoref(sds);
      SDendaccess(sds);
    }

    // 7. Field3DLast: 4x6x2 (band dim last)
    int32_t f3d_last_ref = 0;
    {
      int32_t dims[3] = {4, 6, 2};
      int32_t sds = SDcreate(sd_id, "Field3DLast", DFNT_FLOAT32, 3, dims);
      int32_t start[3] = {0, 0, 0};
      std::vector<float> buf(48, 3.5f);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      f3d_last_ref = SDidtoref(sds);
      SDendaccess(sds);
    }

    // Vgroups for UTMGrid
    int32_t vg_gd1 = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd1, "UTMGrid");
    Vsetclass(vg_gd1, "GRID");

    int32_t vg_gd1_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd1_data, "Data Fields");
    Vsetclass(vg_gd1_data, "GRID Vgroup");
    Vaddtagref(vg_gd1_data, DFTAG_NDG, tiled_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, row_tiled_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, f3d_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, f4d_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, f3d_mid_ref);
    Vaddtagref(vg_gd1_data, DFTAG_NDG, f3d_last_ref);

    int32_t vg_gd1_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd1_attr, "Grid Attributes");
    Vsetclass(vg_gd1_attr, "GRID Vgroup");
    char grid_char_attr[] = "GridAttrText";
    ASSERT_EQ(EHattr(hdf_id, vg_gd1_attr, "GridCharAttr", DFNT_CHAR8,
                     strlen(grid_char_attr), "w", grid_char_attr),
              0);
    int32_t grid_int_attr[2] = {10, 20};
    ASSERT_EQ(EHattr(hdf_id, vg_gd1_attr, "GridIntAttr", DFNT_INT32, 2, "w",
                     grid_int_attr),
              0);

    Vinsert(vg_gd1, vg_gd1_data);
    Vinsert(vg_gd1, vg_gd1_attr);
    Vdetach(vg_gd1_attr);
    Vdetach(vg_gd1_data);
    Vdetach(vg_gd1);

    // Vgroups for GeoGrid
    int32_t vg_gd2 = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd2, "GeoGrid");
    Vsetclass(vg_gd2, "GRID");

    int32_t vg_gd2_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd2_data, "Data Fields");
    Vsetclass(vg_gd2_data, "GRID Vgroup");
    Vaddtagref(vg_gd2_data, DFTAG_NDG, geo_ref);

    int32_t vg_gd2_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_gd2_attr, "Grid Attributes");
    Vsetclass(vg_gd2_attr, "GRID Vgroup");

    Vinsert(vg_gd2, vg_gd2_data);
    Vinsert(vg_gd2, vg_gd2_attr);
    Vdetach(vg_gd2_attr);
    Vdetach(vg_gd2_data);
    Vdetach(vg_gd2);

    ASSERT_EQ(SDend(sd_id), 0);
    ASSERT_EQ(Vend(hdf_id), 0);
    ASSERT_EQ(Hclose(hdf_id), 0);
    ShortenCdfVgroupName(tmp.path());
  }

  // Test Tiled2D (tile reading + grid attrs + scale/offset/units/desc/nodata).
  {
    const std::string sub =
        absl::StrCat("HDF4_EOS:EOS_GRID:\"", tmp.str(), "\":UTMGrid:Tiled2D");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    int bx = 0;
    int by = 0;
    GDALRasterBand* band = ds->GetRasterBand(1);
    band->GetBlockSize(&bx, &by);
    EXPECT_EQ(bx, 6);
    EXPECT_EQ(by, 2);

    int success = FALSE;
    EXPECT_DOUBLE_EQ(band->GetNoDataValue(&success), -999.0);
    EXPECT_TRUE(success);
    EXPECT_DOUBLE_EQ(band->GetScale(&success), 1.0);
    EXPECT_TRUE(success);
    EXPECT_DOUBLE_EQ(band->GetOffset(&success), 12.5);
    EXPECT_TRUE(success);
    EXPECT_STREQ(band->GetUnitType(), "Kelvin");
    EXPECT_STREQ(band->GetDescription(), "Tiled Temperature");
    EXPECT_STREQ(ds->GetMetadataItem("GridCharAttr"), "GridAttrText");
    EXPECT_STREQ(ds->GetMetadataItem("GridIntAttr"), "10, 20");
    EXPECT_NE(ds->GetSpatialRef(), nullptr);

    GDALGeoTransform gt;
    ASSERT_EQ(ds->GetGeoTransform(gt), CE_None);
    EXPECT_DOUBLE_EQ(gt[0], 500000.0);
    EXPECT_DOUBLE_EQ(gt[1], 1000.0);

    std::vector<int16_t> out(24, 0);
    ASSERT_EQ(ReadRasterZeroLeak(band, 0, 0, 6, 2, out.data(), 6, 2, GDT_Int16),
              CE_None);
    ASSERT_EQ(
        ReadRasterZeroLeak(band, 0, 2, 6, 2, out.data() + 12, 6, 2, GDT_Int16),
        CE_None);
    EXPECT_EQ(out[0], 77);
    EXPECT_EQ(out[23], 77);
  }

  // Test RowTiled2D, Field3D, Field4D, Geo2D.
  {
    const std::string sub_row = absl::StrCat("HDF4_EOS:EOS_GRID:\"", tmp.str(),
                                             "\":UTMGrid:RowTiled2D");
    auto ds_row = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_row.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_row, nullptr);
    int16_t v = 0;
    ASSERT_EQ(ReadRasterZeroLeak(ds_row->GetRasterBand(1), 0, 0, 1, 1, &v, 1, 1,
                                 GDT_Int16),
              CE_None);
    EXPECT_EQ(v, 33);

    const std::string sub_3d =
        absl::StrCat("HDF4_EOS:EOS_GRID:\"", tmp.str(), "\":UTMGrid:Field3D");
    auto ds_3d = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_3d.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_3d, nullptr);
    ASSERT_EQ(ds_3d->GetRasterCount(), 2);
    float fv = 0.0f;
    ASSERT_EQ(ReadRasterZeroLeak(ds_3d->GetRasterBand(2), 0, 0, 1, 1, &fv, 1, 1,
                                 GDT_Float32),
              CE_None);
    EXPECT_FLOAT_EQ(fv, 4.5f);

    const std::string sub_4d =
        absl::StrCat("HDF4_EOS:EOS_GRID:\"", tmp.str(), "\":UTMGrid:Field4D");
    auto ds_4d = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_4d.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_4d, nullptr);
    ASSERT_EQ(ds_4d->GetRasterCount(), 4);
    uint8_t uv = 0;
    ASSERT_EQ(ReadRasterZeroLeak(ds_4d->GetRasterBand(4), 0, 0, 1, 1, &uv, 1, 1,
                                 GDT_Byte),
              CE_None);
    EXPECT_EQ(uv, 8);

    const std::string sub_geo =
        absl::StrCat("HDF4_EOS:EOS_GRID:\"", tmp.str(), "\":GeoGrid:Geo2D");
    auto ds_geo = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_geo.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_geo, nullptr);
    GDALGeoTransform gt;
    ASSERT_EQ(ds_geo->GetGeoTransform(gt), CE_None);
    EXPECT_DOUBLE_EQ(gt[0], -100.0);
    EXPECT_DOUBLE_EQ(gt[3], 45.0);

    // Quoted grid/field components + GA_Update open (EHopen rejects
    // DFACC_WRITE).
    const std::string sub_quoted = absl::StrCat(
        "HDF4_EOS:EOS_GRID:\"", tmp.str(), "\":\"UTMGrid\":\"Tiled2D\"");
    auto ds_quoted = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        sub_quoted.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_quoted, nullptr);
    {
      WithQuietHandler quiet;
      EXPECT_EQ(GDALDataset::Open(sub_quoted.c_str(),
                                  GDAL_OF_RASTER | GDAL_OF_UPDATE),
                nullptr);
    }

    // 3D grid fields with band dimension in middle and last positions.
    const std::string sub_mid = absl::StrCat("HDF4_EOS:EOS_GRID:\"", tmp.str(),
                                             "\":UTMGrid:Field3DMid");
    auto ds_mid = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_mid.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_mid, nullptr);
    EXPECT_EQ(ds_mid->GetRasterCount(), 2);

    const std::string sub_last = absl::StrCat("HDF4_EOS:EOS_GRID:\"", tmp.str(),
                                              "\":UTMGrid:Field3DLast");
    auto ds_last = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_last.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_last, nullptr);
    EXPECT_EQ(ds_last->GetRasterCount(), 2);
  }
}

TEST(Hdf4ImageEosSwathTest, SwathGeolocationLatticeProductsAndAttributes) {
  GDALAllRegister();

  // Helper to create an HDF-EOS Swath file with configurable product metadata,
  // optional dimension maps, and optional LatticePoint.
  auto create_swath_file = [](const char* path, std::string_view shortname,
                              std::string_view proj_mnemonic, bool with_dim_map,
                              bool with_lattice,
                              std::string_view scenecenter = "35.5, -115.2") {
    const std::string dim_map_block = with_dim_map
                                          ? "\t\tGROUP=DimensionMap\n"
                                            "\t\t\tOBJECT=DimensionMap_1\n"
                                            "\t\t\t\tGeoDimension=\"GeoY\"\n"
                                            "\t\t\t\tDataDimension=\"YDim\"\n"
                                            "\t\t\t\tOffset=0\n"
                                            "\t\t\t\tIncrement=2\n"
                                            "\t\t\tEND_OBJECT=DimensionMap_1\n"
                                            "\t\t\tOBJECT=DimensionMap_2\n"
                                            "\t\t\t\tGeoDimension=\"GeoX\"\n"
                                            "\t\t\t\tDataDimension=\"XDim\"\n"
                                            "\t\t\t\tOffset=0\n"
                                            "\t\t\t\tIncrement=2\n"
                                            "\t\t\tEND_OBJECT=DimensionMap_2\n"
                                            "\t\tEND_GROUP=DimensionMap\n"
                                          : "\t\tGROUP=DimensionMap\n"
                                            "\t\tEND_GROUP=DimensionMap\n";

    const std::string geo_dims =
        with_dim_map ? "(\"GeoY\",\"GeoX\")" : "(\"YDim\",\"XDim\")";
    const std::string lattice_obj =
        with_lattice ? absl::StrCat(
                           "\t\t\tOBJECT=GeoField_4\n"
                           "\t\t\t\tGeoFieldName=\"LatticePoint\"\n"
                           "\t\t\t\tDataType=DFNT_FLOAT32\n"
                           "\t\t\t\tDimList=",
                           with_dim_map ? "(\"GeoY\",\"GeoX\",\"TwoDim\")"
                                        : "(\"YDim\",\"XDim\",\"TwoDim\")",
                           "\n\t\t\tEND_OBJECT=GeoField_4\n")
                     : "";
    const std::string nomap_obj =
        with_dim_map ? "\t\t\tOBJECT=DataField_5\n"
                       "\t\t\t\tDataFieldName=\"NoMapField\"\n"
                       "\t\t\t\tDataType=DFNT_INT16\n"
                       "\t\t\t\tDimList=(\"OtherDim\",\"TwoDim\")\n"
                       "\t\t\tEND_OBJECT=DataField_5\n"
                     : "";

    const std::string struct_meta = absl::StrCat(
        "GROUP=SwathStructure\n"
        "\tGROUP=SWATH_1\n"
        "\t\tSwathName=\"TestSwath\"\n"
        "\t\tGROUP=Dimension\n"
        "\t\t\tOBJECT=Dimension_1\n"
        "\t\t\t\tDimensionName=\"YDim\"\n"
        "\t\t\t\tSize=4\n"
        "\t\t\tEND_OBJECT=Dimension_1\n"
        "\t\t\tOBJECT=Dimension_2\n"
        "\t\t\t\tDimensionName=\"XDim\"\n"
        "\t\t\t\tSize=6\n"
        "\t\t\tEND_OBJECT=Dimension_2\n"
        "\t\t\tOBJECT=Dimension_3\n"
        "\t\t\t\tDimensionName=\"GeoY\"\n"
        "\t\t\t\tSize=2\n"
        "\t\t\tEND_OBJECT=Dimension_3\n"
        "\t\t\tOBJECT=Dimension_4\n"
        "\t\t\t\tDimensionName=\"GeoX\"\n"
        "\t\t\t\tSize=3\n"
        "\t\t\tEND_OBJECT=Dimension_4\n"
        "\t\t\tOBJECT=Dimension_5\n"
        "\t\t\t\tDimensionName=\"OtherDim\"\n"
        "\t\t\t\tSize=2\n"
        "\t\t\tEND_OBJECT=Dimension_5\n"
        "\t\t\tOBJECT=Dimension_6\n"
        "\t\t\t\tDimensionName=\"TwoDim\"\n"
        "\t\t\t\tSize=2\n"
        "\t\t\tEND_OBJECT=Dimension_6\n"
        "\t\tEND_GROUP=Dimension\n",
        dim_map_block,
        "\t\tGROUP=IndexDimensionMap\n"
        "\t\tEND_GROUP=IndexDimensionMap\n"
        "\t\tGROUP=GeoField\n"
        "\t\t\tOBJECT=GeoField_1\n"
        "\t\t\t\tGeoFieldName=\"SceneLineNumber\"\n"
        "\t\t\t\tDataType=DFNT_INT32\n"
        "\t\t\t\tDimList=",
        geo_dims,
        "\n\t\t\tEND_OBJECT=GeoField_1\n"
        "\t\t\tOBJECT=GeoField_2\n"
        "\t\t\t\tGeoFieldName=\"Latitude\"\n"
        "\t\t\t\tDataType=DFNT_FLOAT32\n"
        "\t\t\t\tDimList=",
        geo_dims,
        "\n\t\t\tEND_OBJECT=GeoField_2\n"
        "\t\t\tOBJECT=GeoField_3\n"
        "\t\t\t\tGeoFieldName=\"Longitude\"\n"
        "\t\t\t\tDataType=DFNT_FLOAT32\n"
        "\t\t\t\tDimList=",
        geo_dims, "\n\t\t\tEND_OBJECT=GeoField_3\n", lattice_obj,
        "\t\tEND_GROUP=GeoField\n"
        "\t\tGROUP=DataField\n"
        "\t\t\tOBJECT=DataField_1\n"
        "\t\t\t\tDataFieldName=\"Band1\"\n"
        "\t\t\t\tDataType=DFNT_INT16\n"
        "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
        "\t\t\tEND_OBJECT=DataField_1\n"
        "\t\t\tOBJECT=DataField_2\n"
        "\t\t\t\tDataFieldName=\"Cube3D\"\n"
        "\t\t\t\tDataType=DFNT_INT16\n"
        "\t\t\t\tDimList=(\"YDim\",\"XDim\",\"OtherDim\")\n"
        "\t\t\tEND_OBJECT=DataField_2\n"
        "\t\t\tOBJECT=DataField_3\n"
        "\t\t\t\tDataFieldName=\"BandNoNum\"\n"
        "\t\t\t\tDataType=DFNT_INT16\n"
        "\t\t\t\tDimList=(\"YDim\",\"XDim\")\n"
        "\t\t\tEND_OBJECT=DataField_3\n"
        "\t\t\tOBJECT=DataField_4\n"
        "\t\t\t\tDataFieldName=\"NoGeoField\"\n"
        "\t\t\t\tDataType=DFNT_INT16\n"
        "\t\t\t\tDimList=(\"OtherDim\",\"XDim\")\n"
        "\t\t\tEND_OBJECT=DataField_4\n",
        nomap_obj,
        "\t\tEND_GROUP=DataField\n"
        "\t\tGROUP=MergedFields\n"
        "\t\tEND_GROUP=MergedFields\n"
        "\tEND_GROUP=SWATH_1\n"
        "END_GROUP=SwathStructure\n"
        "GROUP=GridStructure\n"
        "END_GROUP=GridStructure\n"
        "GROUP=PointStructure\n"
        "END_GROUP=PointStructure\n"
        "END\n");

    int32_t sd_id = SDstart(path, DFACC_CREATE);
    ASSERT_NE(sd_id, -1);
    SetCharAttr(sd_id, "HDFEOSVersion", "HDFEOS_V2.12");

    if (!shortname.empty()) {
      const std::string core_meta = absl::StrCat(
          "GROUP = INVENTORYMETADATA\n"
          "  OBJECT = SHORTNAME\n"
          "    VALUE = \"",
          shortname,
          "\"\n"
          "  END_OBJECT = SHORTNAME\n"
          "END_GROUP = INVENTORYMETADATA\n"
          "END\n");
      SetCharAttr(sd_id, "coremetadata.0", core_meta);
    }
    SetCharAttr(sd_id, "SCENECENTER", scenecenter);

    int32_t hdf_id = Hopen(path, DFACC_RDWR, 0);
    ASSERT_NE(hdf_id, -1);
    ASSERT_EQ(Vstart(hdf_id), 0);

    std::string padded = struct_meta;
    padded.resize(32000, '\0');
    ASSERT_EQ(
        SDsetattr(sd_id, "StructMetadata.0", DFNT_CHAR8, 32000, padded.data()),
        0);

    const int gy = with_dim_map ? 2 : 4;
    const int gx = with_dim_map ? 3 : 6;

    // Geolocation SDSs.
    int32_t lat_ref = 0;
    int32_t lon_ref = 0;
    int32_t sln_ref = 0;
    int32_t lat_pt_ref = 0;
    {
      int32_t gdims[2] = {gy, gx};
      int32_t gstart[2] = {0, 0};

      int32_t sds_sln =
          SDcreate(sd_id, "SceneLineNumber", DFNT_INT32, 2, gdims);
      std::vector<int32_t> sbuf(gy * gx, 1);
      SDwritedata(sds_sln, gstart, nullptr, gdims, sbuf.data());
      sln_ref = SDidtoref(sds_sln);
      SDendaccess(sds_sln);

      int32_t sds_lat = SDcreate(sd_id, "Latitude", DFNT_FLOAT32, 2, gdims);
      std::vector<float> lat_buf(gy * gx);
      for (int r = 0; r < gy; ++r) {
        for (int c = 0; c < gx; ++c) {
          lat_buf[r * gx + c] = 35.0f + 0.1f * static_cast<float>(r);
        }
      }
      lat_buf[0] = -999.0f;  // Matches _FV_Latitude to test fill-value skip.
      SDwritedata(sds_lat, gstart, nullptr, gdims, lat_buf.data());
      lat_ref = SDidtoref(sds_lat);
      SDendaccess(sds_lat);

      int32_t sds_lon = SDcreate(sd_id, "Longitude", DFNT_FLOAT32, 2, gdims);
      std::vector<float> lon_buf(gy * gx);
      for (int r = 0; r < gy; ++r) {
        for (int c = 0; c < gx; ++c) {
          lon_buf[r * gx + c] = -115.0f + 0.1f * static_cast<float>(c);
        }
      }
      lon_buf[1] = -999.0f;  // Matches _FV_Longitude to test fill-value skip.
      SDwritedata(sds_lon, gstart, nullptr, gdims, lon_buf.data());
      lon_ref = SDidtoref(sds_lon);
      SDendaccess(sds_lon);

      if (with_lattice) {
        int32_t ldims[3] = {gy, gx, 2};
        int32_t lstart[3] = {0, 0, 0};
        int32_t sds_lp =
            SDcreate(sd_id, "LatticePoint", DFNT_FLOAT32, 3, ldims);
        std::vector<float> lbuf(gy * gx * 2, 1.0f);
        SDwritedata(sds_lp, lstart, nullptr, ldims, lbuf.data());
        lat_pt_ref = SDidtoref(sds_lp);
        SDendaccess(sds_lp);
      }
    }

    // Data SDSs.
    int32_t b1_ref = 0;
    int32_t c3_ref = 0;
    int32_t bnonum_ref = 0;
    int32_t nogeo_ref = 0;
    int32_t nomap_ref = 0;
    {
      int32_t dims[2] = {4, 6};
      int32_t start[2] = {0, 0};
      int32_t sds = SDcreate(sd_id, "Band1", DFNT_INT16, 2, dims);
      SetCharAttr(sds, "_FillValue", "-32767");
      SetCharAttr(sds, "scale_factor", "0.5");
      SetCharAttr(sds, "add_offset", "1.0");
      SetCharAttr(sds, "units", "W/m2");
      SetCharAttr(sds, "long_name", "Swath Radiance");
      if (!proj_mnemonic.empty()) {
        SetCharAttr(sds, "MPMETHOD1", proj_mnemonic);
        SetCharAttr(sds, "UTMZONECODE1", "11");
        SetCharAttr(sds, "PROJECTIONPARAMETERS1",
                    "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
      }
      std::vector<int16_t> buf(24, 123);
      SDwritedata(sds, start, nullptr, dims, buf.data());
      b1_ref = SDidtoref(sds);
      SDendaccess(sds);

      int32_t dims3[3] = {4, 6, 2};
      int32_t start3[3] = {0, 0, 0};
      int32_t sds3 = SDcreate(sd_id, "Cube3D", DFNT_INT16, 3, dims3);
      std::vector<int16_t> buf3(48, 456);
      SDwritedata(sds3, start3, nullptr, dims3, buf3.data());
      c3_ref = SDidtoref(sds3);
      SDendaccess(sds3);

      int32_t sds_nonum = SDcreate(sd_id, "BandNoNum", DFNT_INT16, 2, dims);
      SDwritedata(sds_nonum, start, nullptr, dims, buf.data());
      bnonum_ref = SDidtoref(sds_nonum);
      SDendaccess(sds_nonum);

      int32_t ng_dims[2] = {2, 6};
      int32_t sds_nogeo = SDcreate(sd_id, "NoGeoField", DFNT_INT16, 2, ng_dims);
      SDwritedata(sds_nogeo, start, nullptr, ng_dims, buf.data());
      nogeo_ref = SDidtoref(sds_nogeo);
      SDendaccess(sds_nogeo);

      if (with_dim_map) {
        int32_t nm_dims[2] = {2, 2};
        int32_t sds_nomap =
            SDcreate(sd_id, "NoMapField", DFNT_INT16, 2, nm_dims);
        SDwritedata(sds_nomap, start, nullptr, nm_dims, buf.data());
        nomap_ref = SDidtoref(sds_nomap);
        SDendaccess(sds_nomap);
      }
    }

    // Build Swath Vgroups.
    int32_t vg_sw = Vattach(hdf_id, -1, "w");
    Vsetname(vg_sw, "TestSwath");
    Vsetclass(vg_sw, "SWATH");

    int32_t vg_geo = Vattach(hdf_id, -1, "w");
    Vsetname(vg_geo, "Geolocation Fields");
    Vsetclass(vg_geo, "SWATH Vgroup");
    Vaddtagref(vg_geo, DFTAG_NDG, sln_ref);
    Vaddtagref(vg_geo, DFTAG_NDG, lat_ref);
    Vaddtagref(vg_geo, DFTAG_NDG, lon_ref);
    if (with_lattice) {
      Vaddtagref(vg_geo, DFTAG_NDG, lat_pt_ref);
    }

    int32_t vg_data = Vattach(hdf_id, -1, "w");
    Vsetname(vg_data, "Data Fields");
    Vsetclass(vg_data, "SWATH Vgroup");
    Vaddtagref(vg_data, DFTAG_NDG, b1_ref);
    Vaddtagref(vg_data, DFTAG_NDG, c3_ref);
    Vaddtagref(vg_data, DFTAG_NDG, bnonum_ref);
    Vaddtagref(vg_data, DFTAG_NDG, nogeo_ref);
    if (with_dim_map) {
      Vaddtagref(vg_data, DFTAG_NDG, nomap_ref);
    }

    int32_t vg_attr = Vattach(hdf_id, -1, "w");
    Vsetname(vg_attr, "Swath Attributes");
    Vsetclass(vg_attr, "SWATH Vgroup");
    char sw_char_attr[] = "SwathAttrText";
    ASSERT_EQ(EHattr(hdf_id, vg_attr, "SwathCharAttr", DFNT_CHAR8,
                     strlen(sw_char_attr), "w", sw_char_attr),
              0);
    float sw_float_attr[2] = {1.5f, 2.5f};
    ASSERT_EQ(EHattr(hdf_id, vg_attr, "SwathFloatAttr", DFNT_FLOAT32, 2, "w",
                     sw_float_attr),
              0);
    float fv_lat = -999.0f;
    ASSERT_EQ(
        EHattr(hdf_id, vg_attr, "_FV_Latitude", DFNT_FLOAT32, 1, "w", &fv_lat),
        0);
    float fv_lon = -999.0f;
    ASSERT_EQ(
        EHattr(hdf_id, vg_attr, "_FV_Longitude", DFNT_FLOAT32, 1, "w", &fv_lon),
        0);
    int16_t fv_c3 = -999;
    ASSERT_EQ(EHattr(hdf_id, vg_attr, "_FV_Cube3D", DFNT_INT16, 1, "w", &fv_c3),
              0);

    Vinsert(vg_sw, vg_geo);
    Vinsert(vg_sw, vg_data);
    Vinsert(vg_sw, vg_attr);
    Vdetach(vg_attr);
    Vdetach(vg_data);
    Vdetach(vg_geo);
    Vdetach(vg_sw);

    ASSERT_EQ(SDend(sd_id), 0);
    ASSERT_EQ(Vend(hdf_id), 0);
    ASSERT_EQ(Hclose(hdf_id), 0);
    ShortenCdfVgroupName(path);
  };

  // 1. Swath with DimensionMap, LatticePoint, MODIS L1B, GEOL_AS_GCPS=FULL.
  {
    TempFile tmp("swath_lattice.hdf");
    create_swath_file(tmp.path(), "MOD021KM", "", /*with_dim_map=*/true,
                      /*with_lattice=*/true);

    ScopedConfigOption gcps_full("GEOL_AS_GCPS", "FULL");
    const std::string sub =
        absl::StrCat("HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:Band1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetGCPCount(), 4);  // 2x3 = 6 minus 2 nodata points
    EXPECT_STREQ(ds->GetMetadataItem("SwathCharAttr"), "SwathAttrText");
    EXPECT_STREQ(ds->GetMetadataItem("SwathFloatAttr"), "1.5, 2.5");
    int16_t val = 0;
    ASSERT_EQ(ReadRasterZeroLeak(ds->GetRasterBand(1), 0, 0, 1, 1, &val, 1, 1,
                                 GDT_Int16),
              CE_None);
    EXPECT_EQ(val, 123);

    // Also test GA_Update open (EHopen rejects DFACC_WRITE), missing swath,
    // NoGeoField, and NoMapField.
    {
      WithQuietHandler quiet;
      EXPECT_EQ(GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_UPDATE),
                nullptr);

      const std::string bad_sw = absl::StrCat(
          "HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":MissingSwath:Band1");
      EXPECT_EQ(
          GDALDataset::Open(bad_sw.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY),
          nullptr);
    }

    const std::string sub_nogeo = absl::StrCat(
        "HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:NoGeoField");
    auto ds_nogeo = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        sub_nogeo.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_nogeo, nullptr);
    EXPECT_EQ(ds_nogeo->GetGCPCount(), 0);

    const std::string sub_nomap = absl::StrCat(
        "HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:NoMapField");
    auto ds_nomap = std::unique_ptr<GDALDataset>(GDALDataset::Open(
        sub_nomap.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_nomap, nullptr);
    EXPECT_EQ(ds_nomap->GetGCPCount(), 0);

    // Also test Cube3D (3D swath field + SWgetfillvalue) and EOS_SWATH_GEOL.
    const std::string sub3 =
        absl::StrCat("HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:Cube3D");
    auto ds3 = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub3.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds3, nullptr);
    EXPECT_EQ(ds3->GetRasterCount(), 2);
    int success = FALSE;
    EXPECT_DOUBLE_EQ(ds3->GetRasterBand(1)->GetNoDataValue(&success), -999.0);
    EXPECT_TRUE(success);
    int16_t v3 = 0;
    ASSERT_EQ(ReadRasterZeroLeak(ds3->GetRasterBand(2), 0, 0, 1, 1, &v3, 1, 1,
                                 GDT_Int16),
              CE_None);
    EXPECT_EQ(v3, 456);

    const std::string sub_geol = absl::StrCat(
        "HDF4_EOS:EOS_SWATH_GEOL:\"", tmp.str(), "\":TestSwath:Latitude");
    auto ds_geol = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub_geol.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds_geol, nullptr);
    EXPECT_EQ(ds_geol->GetGCPCount(), 0);
  }

  // 2. Swath without DimensionMap or LatticePoint -> GEOLOCATION metadata +
  // GEOL_AS_GCPS=NONE.
  {
    TempFile tmp("swath_nodimmap.hdf");
    create_swath_file(tmp.path(), "MOD07_L2", "", /*with_dim_map=*/false,
                      /*with_lattice=*/false);
    ScopedConfigOption gcps_none("GEOL_AS_GCPS", "NONE");
    const std::string sub =
        absl::StrCat("HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:Band1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_EQ(ds->GetGCPCount(), 0);
    EXPECT_NE(ds->GetMetadata("GEOLOCATION"), nullptr);
    EXPECT_STREQ(ds->GetMetadataItem("PIXEL_STEP", "GEOLOCATION"), "1");
  }

  // 3. ASTER L1A, L1B (with all USGSMnemonicToCode values), L2, L3 (north +
  // south).
  const char* aster_products[] = {"ASTL1A", "ASTL1B", "AST_05", "AST14"};
  const char* mnemonics[] = {"UTM",    "LAMCC", "PS",  "PC",     "TM",
                             "EQRECT", "OM",    "SOM", "UNKNOWN"};
  for (const char* prod : aster_products) {
    TempFile tmp(absl::StrCat("swath_", prod, ".hdf"));
    create_swath_file(tmp.path(), prod, "UTM", /*with_dim_map=*/true,
                      /*with_lattice=*/false);
    const std::string sub =
        absl::StrCat("HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:Band1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_GT(ds->GetGCPCount(), 0);

    if (std::string_view(prod) == "ASTL1B") {
      const std::string sub_nonum = absl::StrCat(
          "HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:BandNoNum");
      auto ds_nonum = std::unique_ptr<GDALDataset>(GDALDataset::Open(
          sub_nonum.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
      ASSERT_NE(ds_nonum, nullptr);
    }
  }

  // Southern hemisphere AST14.
  {
    TempFile tmp("swath_ast14_south.hdf");
    create_swath_file(tmp.path(), "AST14", "UTM", /*with_dim_map=*/true,
                      /*with_lattice=*/false, "-35.5, -115.2");
    const std::string sub =
        absl::StrCat("HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:Band1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_NE(ds->GetSpatialRef(), nullptr);
  }

  for (const char* mnem : mnemonics) {
    TempFile tmp(absl::StrCat("swath_mnem_", mnem, ".hdf"));
    create_swath_file(tmp.path(), "ASTL1B", mnem, /*with_dim_map=*/true,
                      /*with_lattice=*/false);
    const std::string sub =
        absl::StrCat("HDF4_EOS:EOS_SWATH:\"", tmp.str(), "\":TestSwath:Band1");
    auto ds = std::unique_ptr<GDALDataset>(
        GDALDataset::Open(sub.c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY));
    ASSERT_NE(ds, nullptr);
    EXPECT_NE(ds->GetSpatialRef(), nullptr);
  }
}

}  // namespace
}  // namespace autotest2
