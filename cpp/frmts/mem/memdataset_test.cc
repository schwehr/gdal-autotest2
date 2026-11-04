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
// Tests MEMDataset, MEMRasterBand, and MEM multidimensional implementation (memdataset.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/mem/memdataset.cpp
//   https://gdal.org/drivers/raster/mem.html

#include "third_party/gdal/frmts/mem/memdataset.h"
#include "third_party/gdal/frmts/mem/memmultidim.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_cpp_functions.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/ogr/ogr_geometry.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/ogr/ogrsf_frmts/ogrsf_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"

namespace autotest2 {
namespace {

using ::testing::IsNull;
using ::testing::NotNull;

OGRSpatialReference GetWgs84Srs() {
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  return srs;
}

std::string FormatPointer(void* ptr) {
  char szBuf[64] = {};
  CPLPrintPointer(szBuf, ptr, sizeof(szBuf));
  return szBuf;
}

class MemDatasetTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }
};

// -----------------------------------------------------------------------------
// 1. Driver Registration, Metadata, and Delete
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, DriverRegistrationAndDelete) {
  GDALRegister_MEM();  // Idempotent

  GDALDriver* poDriver = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_THAT(poDriver, NotNull());

  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_RASTER));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_MULTIDIM_RASTER));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_VECTOR));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_FIELD_DOMAINS));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_CREATE_LAYER));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_DELETE_LAYER));
  EXPECT_STREQ("YES", poDriver->GetMetadataItem(GDAL_DCAP_UPSERT));

  EXPECT_EQ(CE_None, poDriver->Delete("MEM:::"));
}

TEST_F(MemDatasetTest, IdentifyDoesNotExist) {
  GDALOpenInfo open_info("/does_not_exist", GDAL_OF_RASTER);
  EXPECT_THAT(MEMDataset::Open(&open_info), IsNull());
  EXPECT_THAT(
      GDALOpenEx("/does_not_exist", GDAL_OF_RASTER, nullptr, nullptr, nullptr),
      IsNull());
}

// -----------------------------------------------------------------------------
// 2. Create and Open Dataset Options
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, CreateDatasetOptions) {
  // 1. Default creation (INTERLEAVE=BAND)
  {
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(10, ds->GetRasterXSize());
    EXPECT_EQ(10, ds->GetRasterYSize());
    EXPECT_EQ(1, ds->GetRasterCount());
    EXPECT_STREQ("BAND", ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));
  }

  // 2. PIXEL interleaved and SIGNEDBYTE pixel type
  {
    const char* const apszOptions[] = {"INTERLEAVE=PIXEL", "PIXELTYPE=SIGNEDBYTE",
                                       nullptr};
    std::unique_ptr<MEMDataset> ds(MEMDataset::Create(
        "", 10, 10, 2, GDT_Byte, const_cast<char**>(apszOptions)));
    ASSERT_THAT(ds, NotNull());
    EXPECT_STREQ("PIXEL", ds->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE"));
    EXPECT_STREQ("SIGNEDBYTE",
                 ds->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"));
  }

  // 3. 0-band dataset
  {
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 10, 10, 0, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    EXPECT_EQ(0, ds->GetRasterCount());
  }

  // 4. Multiplication overflow error
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", INT_MAX, INT_MAX, 100, GDT_Float64, nullptr));
    EXPECT_THAT(ds, IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(MemDatasetTest, OpenDatasetOptions) {
  // 1. Opening without GDAL_MEM_ENABLE_OPEN=YES fails for security reasons
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        "MEM:::PIXELS=10,LINES=10,DATAPOINTER=1", GDAL_OF_RASTER));
    EXPECT_THAT(ds, IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Enable GDAL_MEM_ENABLE_OPEN for remainder of test
  CPLSetThreadLocalConfigOption("GDAL_MEM_ENABLE_OPEN", "YES");
  absl::Cleanup reset_config = [] {
    CPLSetThreadLocalConfigOption("GDAL_MEM_ENABLE_OPEN", nullptr);
  };

  // 2. Missing required fields fails
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        "MEM:::PIXELS=10,LINES=10", GDAL_OF_RASTER));
    EXPECT_THAT(ds, IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Invalid dimensions fail
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        "MEM:::PIXELS=-1,LINES=10,DATAPOINTER=1", GDAL_OF_RASTER));
    EXPECT_THAT(ds, IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Unrecognized datatype fails
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    std::unique_ptr<GDALDataset> ds(GDALDataset::Open(
        "MEM:::PIXELS=10,LINES=10,DATAPOINTER=1,DATATYPE=InvalidType",
        GDAL_OF_RASTER));
    EXPECT_THAT(ds, IsNull());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. Valid open with explicit offsets, geotransform, and spatial reference
  std::vector<uint8_t> buffer(10 * 10 * sizeof(uint8_t), 0);
  const std::string mem_desc =
      std::string("MEM:::PIXELS=10,LINES=10,BANDS=1,DATATYPE=Byte,DATAPOINTER=") +
      FormatPointer(buffer.data()) +
      ",PIXELOFFSET=1,LINEOFFSET=10,BANDOFFSET=100,"
      "GEOTRANSFORM=100/1/0/200/0/-1,SPATIALREFERENCE=EPSG:4326";

  std::unique_ptr<GDALDataset> ds(
      GDALDataset::Open(mem_desc.c_str(), GDAL_OF_RASTER));
  ASSERT_THAT(ds, NotNull());
  EXPECT_EQ(10, ds->GetRasterXSize());
  EXPECT_EQ(10, ds->GetRasterYSize());
  EXPECT_EQ(1, ds->GetRasterCount());

  GDALGeoTransform gt;
  EXPECT_EQ(CE_None, ds->GetGeoTransform(gt));
  EXPECT_DOUBLE_EQ(100.0, gt[0]);
  EXPECT_DOUBLE_EQ(1.0, gt[1]);
  EXPECT_DOUBLE_EQ(200.0, gt[3]);
  EXPECT_DOUBLE_EQ(-1.0, gt[5]);

  const auto* srs = ds->GetSpatialRef();
  ASSERT_THAT(srs, NotNull());
  EXPECT_TRUE(srs->IsGeographic());

  // 6. Numeric DATATYPE=1 and warning on invalid CRS
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    const std::string mem_desc_num_dt =
        std::string("MEM:::PIXELS=5,LINES=5,DATAPOINTER=") +
        FormatPointer(buffer.data()) +
        ",DATATYPE=1,SPATIALREFERENCE=INVALID_CRS";
    std::unique_ptr<GDALDataset> ds_num(
        GDALDataset::Open(mem_desc_num_dt.c_str(), GDAL_OF_RASTER));
    ASSERT_THAT(ds_num, NotNull());
    EXPECT_EQ(GDT_Byte, ds_num->GetRasterBand(1)->GetRasterDataType());
  }
}

// -----------------------------------------------------------------------------
// 3. GeoTransform, SpatialRef, GCPs, and InternalHandle
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, GeoTransformSpatialRefGcpsAndInternalHandle) {
  MEMDataset ds;

  // 1. GeoTransform before and after set
  GDALGeoTransform gt_out;
  EXPECT_EQ(CE_Failure, ds.GetGeoTransform(gt_out));

  GDALGeoTransform gt_in = {10.0, 2.0, 0.0, 50.0, 0.0, -2.0};
  EXPECT_EQ(CE_None, ds.SetGeoTransform(gt_in));
  EXPECT_EQ(CE_None, ds.GetGeoTransform(gt_out));
  EXPECT_DOUBLE_EQ(10.0, gt_out[0]);
  EXPECT_DOUBLE_EQ(2.0, gt_out[1]);

  // 2. Spatial reference raster only
  EXPECT_THAT(ds.GetSpatialRefRasterOnly(), IsNull());
  const OGRSpatialReference wgs84 = GetWgs84Srs();
  EXPECT_EQ(CE_None, ds.SetSpatialRef(&wgs84));
  ASSERT_THAT(ds.GetSpatialRefRasterOnly(), NotNull());
  EXPECT_TRUE(ds.GetSpatialRefRasterOnly()->IsGeographic());
  EXPECT_EQ(CE_None, ds.SetSpatialRef(nullptr));
  EXPECT_THAT(ds.GetSpatialRefRasterOnly(), IsNull());

  // 3. Spatial reference with layer count > 0 delegates to GDALDataset
  EXPECT_EQ(CE_None, ds.SetSpatialRef(&wgs84));
  ds.CreateLayer("dummy_layer", &wgs84, wkbPoint, nullptr);
  EXPECT_EQ(1, ds.GetLayerCount());
  ASSERT_THAT(ds.GetSpatialRef(), NotNull());

  // 4. GCPs
  EXPECT_EQ(0, ds.GetGCPCount());
  EXPECT_THAT(ds.GetGCPSpatialRef(), IsNull());
  EXPECT_THAT(ds.GetGCPs(), IsNull());

  GDAL_GCP gcp;
  GDALInitGCPs(1, &gcp);
  absl::Cleanup gcp_cleanup = [&gcp] { GDALDeinitGCPs(1, &gcp); };
  gcp.dfGCPX = 100.0;
  gcp.dfGCPY = 200.0;
  gcp.dfGCPPixel = 1.0;
  gcp.dfGCPLine = 2.0;

  EXPECT_EQ(CE_None, ds.SetGCPs(1, &gcp, &wgs84));
  EXPECT_EQ(1, ds.GetGCPCount());
  ASSERT_THAT(ds.GetGCPs(), NotNull());
  EXPECT_DOUBLE_EQ(100.0, ds.GetGCPs()[0].dfGCPX);
  ASSERT_THAT(ds.GetGCPSpatialRef(), NotNull());

  // 5. GetInternalHandle
  std::unique_ptr<MEMDataset> ds_handle(
      MEMDataset::Create("", 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_THAT(ds_handle, NotNull());
  void* handle = ds_handle->GetInternalHandle("MEMORY1");
  ASSERT_THAT(handle, NotNull());

  EXPECT_THAT(ds_handle->GetInternalHandle("MEMORY99"), IsNull());
  EXPECT_THAT(ds_handle->GetInternalHandle("OTHER"), IsNull());
}

// -----------------------------------------------------------------------------
// 4. AddBand and MEMCreateRasterBand C Functions
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, AddBandAndMEMCreateRasterBandCFunctions) {
  std::unique_ptr<MEMDataset> ds(
      MEMDataset::Create("", 10, 10, 0, GDT_Byte, nullptr));
  ASSERT_THAT(ds, NotNull());

  // 1. AddBand with invalid datatype fails
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, ds->AddBand(GDT_Unknown, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. AddBand self-allocated
  EXPECT_EQ(CE_None, ds->AddBand(GDT_Byte, nullptr));
  EXPECT_EQ(1, ds->GetRasterCount());

  // 3. AddBand with DATAPOINTER and explicit offsets
  std::vector<uint16_t> buffer(10 * 10, 0);
  const std::string ptr_str = FormatPointer(buffer.data());
  const char* const options[] = {
      CPLSPrintf("DATAPOINTER=%s", ptr_str.c_str()),
      "PIXELOFFSET=2",
      "LINEOFFSET=20",
      nullptr,
  };
  EXPECT_EQ(CE_None, ds->AddBand(GDT_UInt16, const_cast<char**>(options)));
  EXPECT_EQ(2, ds->GetRasterCount());

  // 4. MEMCreateRasterBand and MEMCreateRasterBandEx
  std::vector<uint8_t> band_buf1(10 * 10, 0);
  GDALRasterBandH hBand1 =
      MEMCreateRasterBand(ds.get(), 3, band_buf1.data(), GDT_Byte, 1, 10, FALSE);
  ASSERT_THAT(hBand1, NotNull());
  ds->AddMEMBand(hBand1);
  EXPECT_EQ(3, ds->GetRasterCount());

  std::vector<uint8_t> band_buf2(10 * 10, 0);
  GDALRasterBandH hBand2 =
      MEMCreateRasterBandEx(ds.get(), 4, band_buf2.data(), GDT_Byte, 1, 10, FALSE);
  ASSERT_THAT(hBand2, NotNull());
  ds->AddMEMBand(hBand2);
  EXPECT_EQ(4, ds->GetRasterCount());

  // 5. MEMRasterBand with SIGNEDBYTE metadata
  auto signed_band = std::make_unique<MEMRasterBand>(
      ds.get(), 5, band_buf2.data(), GDT_Byte, 1, 10, FALSE, "SIGNEDBYTE");
  EXPECT_STREQ("SIGNEDBYTE",
               signed_band->GetMetadataItem("PIXELTYPE", "IMAGE_STRUCTURE"));
}

// -----------------------------------------------------------------------------
// 5. Band Read/Write Block and IRasterIO
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, BandReadWriteBlockAndIRasterIO) {
  // 1. Contiguous band: IReadBlock and IWriteBlock
  {
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 4, 4, 1, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    auto* mem_band = cpl::down_cast<MEMRasterBand*>(ds->GetRasterBand(1));

    std::array<uint8_t, 4> write_line = {10, 20, 30, 40};
    EXPECT_EQ(CE_None, mem_band->IWriteBlock(0, 1, write_line.data()));

    std::array<uint8_t, 4> read_line = {0, 0, 0, 0};
    EXPECT_EQ(CE_None, mem_band->IReadBlock(0, 1, read_line.data()));
    EXPECT_EQ(write_line, read_line);
  }

  // 2. Strided band: nPixelOffset > nWordSize
  {
    std::vector<uint8_t> interleaved(4 * 4 * 2, 0);
    for (size_t i = 0; i < 16; ++i) {
      interleaved[i * 2] = static_cast<uint8_t>(i);
    }

    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 4, 4, 0, GDT_Byte, nullptr));
    const std::string ptr_str = FormatPointer(interleaved.data());
    const char* const options[] = {
        CPLSPrintf("DATAPOINTER=%s", ptr_str.c_str()),
        "PIXELOFFSET=2",
        "LINEOFFSET=8",
        nullptr,
    };
    ASSERT_EQ(CE_None, ds->AddBand(GDT_Byte, const_cast<char**>(options)));

    auto* mem_band = cpl::down_cast<MEMRasterBand*>(ds->GetRasterBand(1));
    std::array<uint8_t, 4> read_line = {0, 0, 0, 0};
    EXPECT_EQ(CE_None, mem_band->IReadBlock(0, 2, read_line.data()));
    EXPECT_EQ(8, read_line[0]);
    EXPECT_EQ(9, read_line[1]);
    EXPECT_EQ(10, read_line[2]);
    EXPECT_EQ(11, read_line[3]);

    std::array<uint8_t, 4> write_line = {99, 98, 97, 96};
    EXPECT_EQ(CE_None, mem_band->IWriteBlock(0, 2, write_line.data()));
    EXPECT_EQ(99, interleaved[8 * 2]);
    EXPECT_EQ(98, interleaved[9 * 2]);
  }

  // 3. MEMRasterBand::IRasterIO
  {
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    GDALRasterBand* band = ds->GetRasterBand(1);

    // Full-buffer contiguous write
    std::vector<uint8_t> full_write(100, 77);
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Write, 0, 0, 10, 10, full_write.data(), 10, 10,
                             GDT_Byte, 1, 10, nullptr));

    // Full-buffer read
    std::vector<uint8_t> full_read(100, 0);
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 10, 10, full_read.data(), 10, 10,
                             GDT_Byte, 1, 10, nullptr));
    EXPECT_EQ(full_write, full_read);

    // Sub-window write (line-by-line fallback)
    std::vector<uint8_t> sub_write(4 * 4, 88);
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Write, 2, 2, 4, 4, sub_write.data(), 4, 4,
                             GDT_Byte, 1, 4, nullptr));

    // Resampled read (nXSize != nBufXSize)
    std::vector<uint8_t> resampled_read(5 * 5, 0);
    EXPECT_EQ(CE_None,
              band->RasterIO(GF_Read, 0, 0, 10, 10, resampled_read.data(), 5,
                             5, GDT_Byte, 1, 5, nullptr));
    EXPECT_EQ(77, resampled_read[0]);
  }
}

// -----------------------------------------------------------------------------
// 6. Dataset IRasterIO Interleave and Deinterleave Paths
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, DatasetIRasterIOInterleaveAndDeinterleavePaths) {
  // 1. Pixel-interleaved dataset: read/write pixel-interleaved buffer
  {
    const char* const options[] = {"INTERLEAVE=PIXEL", nullptr};
    std::unique_ptr<MEMDataset> ds(MEMDataset::Create(
        "", 4, 4, 3, GDT_Byte, const_cast<char**>(options)));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> pix_buf(4 * 4 * 3, 0);
    for (size_t i = 0; i < pix_buf.size(); ++i) {
      pix_buf[i] = static_cast<uint8_t>(i);
    }

    // Write pixel-interleaved buffer to pixel-interleaved dataset
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 4, 4, pix_buf.data(), 4, 4,
                           GDT_Byte, 3, nullptr, 3, 12, 1, nullptr));

    // Read back pixel-interleaved buffer
    std::vector<uint8_t> pix_read(4 * 4 * 3, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 4, 4, pix_read.data(), 4, 4,
                           GDT_Byte, 3, nullptr, 3, 12, 1, nullptr));
    EXPECT_EQ(pix_buf, pix_read);

    // Write band-interleaved buffer to pixel-interleaved dataset (GDALTranspose2D path)
    std::vector<uint8_t> band_buf(4 * 4 * 3, 42);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 4, 4, band_buf.data(), 4, 4,
                           GDT_Byte, 3, nullptr, 1, 4, 16, nullptr));
  }

  // 2. Band-separated dataset: write pixel-interleaved buffer (GDALDeinterleave)
  {
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 4, 4, 3, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());

    std::vector<uint8_t> pix_buf(4 * 4 * 3, 55);

    // Full strip write (GDALDeinterleave whole strip)
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 0, 0, 4, 4, pix_buf.data(), 4, 4,
                           GDT_Byte, 3, nullptr, 3, 12, 1, nullptr));

    // Sub-window write (GDALDeinterleave line-by-line)
    std::vector<uint8_t> sub_pix_buf(2 * 2 * 3, 66);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Write, 1, 1, 2, 2, sub_pix_buf.data(), 2, 2,
                           GDT_Byte, 3, nullptr, 3, 6, 1, nullptr));

    // Resampled dataset RasterIO
    std::vector<uint8_t> resamp_buf(2 * 2 * 3, 0);
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 4, 4, resamp_buf.data(), 2, 2,
                           GDT_Byte, 3, nullptr, 1, 2, 4, nullptr));
    EXPECT_EQ(66, resamp_buf[0]);

    // Band-based RasterIO fallback (subset of bands)
    std::vector<uint8_t> single_band_buf(4 * 4, 0);
    int band_map = 2;
    EXPECT_EQ(CE_None,
              ds->RasterIO(GF_Read, 0, 0, 4, 4, single_band_buf.data(), 4, 4,
                           GDT_Byte, 1, &band_map, 1, 4, 0, nullptr));
    EXPECT_EQ(55, single_band_buf[0]);
    EXPECT_EQ(66, single_band_buf[1 * 4 + 1]);
  }
}

// -----------------------------------------------------------------------------
// 7. Mask Bands, Overviews, and Clone
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, MaskBandsOverviewsAndClone) {
  // 1. Mask bands
  {
    std::unique_ptr<MEMDataset> ds_empty(
        MEMDataset::Create("", 10, 10, 0, GDT_Byte, nullptr));
    EXPECT_EQ(CE_Failure, ds_empty->CreateMaskBand(0));

    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 10, 10, 2, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());

    // Per-dataset mask band
    EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
    GDALRasterBand* band1 = ds->GetRasterBand(1);
    GDALRasterBand* band2 = ds->GetRasterBand(2);
    ASSERT_THAT(band1->GetMaskBand(), NotNull());
    EXPECT_TRUE(band1->GetMaskBand()->IsMaskBand());
    ASSERT_THAT(band2->GetMaskBand(), NotNull());
    EXPECT_TRUE(band2->GetMaskBand()->IsMaskBand());

    // CreateMaskBand on band 2 with GMF_PER_DATASET
    EXPECT_EQ(CE_None, band2->CreateMaskBand(GMF_PER_DATASET));
  }

  // 2. IBuildOverviews
  {
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 16, 16, 2, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());

    // Dataset has 0 bands error
    std::unique_ptr<MEMDataset> ds_zero(
        MEMDataset::Create("", 16, 16, 0, GDT_Byte, nullptr));
    constexpr int ov_list[] = {2};
    constexpr int band_list[] = {1};
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                ds_zero->IBuildOverviews("NEAREST", 1, ov_list, 1, band_list,
                                         nullptr, nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // List bands != nBands error
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(CE_Failure,
                ds->IBuildOverviews("NEAREST", 1, ov_list, 1, band_list,
                                    nullptr, nullptr, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }

    // Build overview with mask band
    EXPECT_EQ(CE_None, ds->CreateMaskBand(GMF_PER_DATASET));
    constexpr int all_bands[] = {1, 2};
    EXPECT_EQ(CE_None,
              ds->IBuildOverviews("NEAREST", 1, ov_list, 2, all_bands,
                                  nullptr, nullptr, nullptr));
    EXPECT_EQ(1, ds->GetRasterBand(1)->GetOverviewCount());
    ASSERT_THAT(ds->GetRasterBand(1)->GetOverview(0), NotNull());
    EXPECT_THAT(ds->GetRasterBand(1)->GetOverview(-1), IsNull());
    EXPECT_THAT(ds->GetRasterBand(1)->GetOverview(5), IsNull());

    // Rebuild existing overview
    EXPECT_EQ(CE_None,
              ds->IBuildOverviews("NEAREST", 1, ov_list, 2, all_bands,
                                  nullptr, nullptr, nullptr));

    // Cascading overviews (nOverviews > 1 with AVERAGE)
    constexpr int multi_ov[] = {2, 4};
    EXPECT_EQ(CE_None,
              ds->IBuildOverviews("AVERAGE", 2, multi_ov, 2, all_bands,
                                  nullptr, nullptr, nullptr));

    // Cascading overviews with GAUSS
    EXPECT_EQ(CE_None,
              ds->IBuildOverviews("GAUSS", 2, multi_ov, 2, all_bands,
                                  nullptr, nullptr, nullptr));

    // Clear overviews (nOverviews = 0)
    EXPECT_EQ(CE_None,
              ds->IBuildOverviews("NEAREST", 0, nullptr, 2, all_bands,
                                  nullptr, nullptr, nullptr));
    EXPECT_EQ(0, ds->GetRasterBand(1)->GetOverviewCount());
  }

  // 3. Thread-safe dataset cloning (CanBeCloned and Clone)
  {
    std::unique_ptr<MEMDataset> ds(
        MEMDataset::Create("", 16, 16, 1, GDT_Byte, nullptr));
    ASSERT_THAT(ds, NotNull());
    const OGRSpatialReference wgs84 = GetWgs84Srs();
    ds->SetSpatialRef(&wgs84);
    GDALGeoTransform gt = {10.0, 1.0, 0.0, 20.0, 0.0, -1.0};
    ds->SetGeoTransform(gt);
    ds->SetDescription("mem_clone_source");
    ds->GetRasterBand(1)->SetUnitType("meters");
    ds->GetRasterBand(1)->SetScale(2.0);
    ds->CreateMaskBand(0);
    ds->GetRasterBand(1)->CreateMaskBand(0);

    constexpr int ov_list[] = {2};
    constexpr int band_list[] = {1};
    ds->IBuildOverviews("NEAREST", 1, ov_list, 1, band_list, nullptr, nullptr,
                        nullptr);

    // GDALGetThreadSafeDataset exercises CanBeCloned and Clone
    std::unique_ptr<GDALDataset> cloned_ds(
        GDALGetThreadSafeDataset(ds.get(), GDAL_OF_RASTER));
    ASSERT_THAT(cloned_ds, NotNull());
    EXPECT_EQ(16, cloned_ds->GetRasterXSize());
    EXPECT_EQ(16, cloned_ds->GetRasterYSize());
    EXPECT_EQ(1, cloned_ds->GetRasterCount());
    EXPECT_STREQ("meters", cloned_ds->GetRasterBand(1)->GetUnitType());
    EXPECT_EQ(1, cloned_ds->GetRasterBand(1)->GetOverviewCount());

    GByte pixel = 0;
    EXPECT_EQ(CE_None, cloned_ds->GetRasterBand(1)->RasterIO(
                           GF_Read, 0, 0, 1, 1, &pixel, 1, 1, GDT_Byte, 0, 0, nullptr));
    ASSERT_THAT(cloned_ds->GetRasterBand(1)->GetMaskBand(), NotNull());
    EXPECT_EQ(CE_None, cloned_ds->GetRasterBand(1)->GetMaskBand()->RasterIO(
                           GF_Read, 0, 0, 1, 1, &pixel, 1, 1, GDT_Byte, 0, 0, nullptr));
    EXPECT_DOUBLE_EQ(2.0, cloned_ds->GetRasterBand(1)->GetScale());

    // Vector scope returns nullptr
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_THAT(GDALGetThreadSafeDataset(ds.get(), GDAL_OF_VECTOR), IsNull());
    }
  }
}

// -----------------------------------------------------------------------------
// 8. MultiDimensional Groups, Dimensions, and Attributes
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, MultiDimensionalGroupsDimensionsAndAttributes) {
  std::unique_ptr<GDALDataset> ds(
      MEMDataset::CreateMultiDimensional("mem_multidim", nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());

  auto root = ds->GetRootGroup();
  ASSERT_THAT(root, NotNull());
  EXPECT_STREQ("/", root->GetName().c_str());

  // 1. Group creation, error branches, and renaming
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateGroup("", nullptr), IsNull());
  }
  auto sub1 = root->CreateGroup("sub1");
  ASSERT_THAT(sub1, NotNull());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateGroup("sub1"), IsNull());  // Duplicate
  }
  EXPECT_THAT(root->OpenGroup("sub1"), NotNull());
  EXPECT_THAT(root->OpenGroup("non_existent"), IsNull());
  EXPECT_EQ(1, root->GetGroupNames().size());

  // Rename root fails
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->Rename("new_root"));
  }
  // Rename empty fails
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub1->Rename(""));
  }
  EXPECT_TRUE(sub1->Rename("sub1_renamed"));
  EXPECT_STREQ("sub1_renamed", sub1->GetName().c_str());

  // 2. Dimensions
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateDimension("", "", "", 10), IsNull());
  }
  auto dim_x = root->CreateDimension("x", "HORIZONTAL_X", "", 10);
  ASSERT_THAT(dim_x, NotNull());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateDimension("x", "", "", 10), IsNull());  // Duplicate
  }
  EXPECT_EQ(1, root->GetDimensions().size());
  EXPECT_TRUE(dim_x->Rename("x_renamed"));
  EXPECT_STREQ("x_renamed", dim_x->GetName().c_str());

  // 3. Group Attributes
  auto dt_int = GDALExtendedDataType::Create(GDT_Int32);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateAttribute("", {}, dt_int), IsNull());
  }
  auto attr = root->CreateAttribute("author", {},
                                    GDALExtendedDataType::CreateString());
  ASSERT_THAT(attr, NotNull());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateAttribute("author", {}, dt_int), IsNull());
  }
  EXPECT_THAT(root->GetAttribute("author"), NotNull());
  EXPECT_THAT(root->GetAttribute("missing"), IsNull());
  EXPECT_EQ(1, root->GetAttributes().size());

  EXPECT_TRUE(attr->Rename("author_new"));
  EXPECT_STREQ("author_new", attr->GetName().c_str());
  EXPECT_TRUE(root->DeleteAttribute("author_new"));
  EXPECT_FALSE(root->DeleteAttribute("missing"));

  // 4. Subgroup rename notification to children and deletion invalidating children
  auto sub_dim = sub1->CreateDimension("s_dim", "", "", 5);
  ASSERT_THAT(sub_dim, NotNull());
  auto sub_dim2 = sub1->CreateDimension("s_dim2", "", "", 5);
  ASSERT_THAT(sub_dim2, NotNull());

  auto sub_attr = sub1->CreateAttribute("s_attr", {}, dt_int);
  ASSERT_THAT(sub_attr, NotNull());
  auto sub_attr2 = sub1->CreateAttribute("s_attr2", {}, dt_int);
  ASSERT_THAT(sub_attr2, NotNull());

  auto sub_arr = sub1->CreateMDArray("s_arr", {sub_dim}, dt_int, nullptr);
  ASSERT_THAT(sub_arr, NotNull());
  auto sub_arr2 = sub1->CreateMDArray("s_arr2", {sub_dim}, dt_int, nullptr);
  ASSERT_THAT(sub_arr2, NotNull());

  auto sub_child = sub1->CreateGroup("child");
  ASSERT_THAT(sub_child, NotNull());

  // Renaming parent notifies all children (ParentRenamed)
  EXPECT_TRUE(sub1->Rename("sub1_renamed_again"));

  // Reject empty or duplicate renames on children
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub_attr->Rename(""));
    EXPECT_FALSE(sub_attr->Rename("s_attr2"));
    EXPECT_FALSE(sub_dim->Rename(""));
    EXPECT_FALSE(sub_dim->Rename("s_dim2"));
    EXPECT_FALSE(sub_arr->Rename(""));
    EXPECT_FALSE(sub_arr->Rename("s_arr2"));
  }

  // Deleting group invalidates children (ParentDeleted)
  EXPECT_TRUE(root->DeleteGroup("sub1_renamed_again"));
  EXPECT_FALSE(root->DeleteGroup("missing"));
  EXPECT_EQ(0, root->GetGroupNames().size());
  sub1.reset();

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub_attr->Rename("new_name"));
    EXPECT_TRUE(sub_dim->Rename("new_name"));
    EXPECT_FALSE(sub_arr->Rename("new_name"));
    EXPECT_TRUE(sub_arr->GetCoordinateVariables().empty());
  }
}

// -----------------------------------------------------------------------------
// 9. MultiDimensional Arrays, Read/Write, FastCopy, and Recursion
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, MultiDimensionalArraysReadWriteAndFastCopy) {
  std::unique_ptr<GDALDataset> ds(
      MEMDataset::CreateMultiDimensional("mem_md_arrays", nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  auto root = ds->GetRootGroup();
  ASSERT_THAT(root, NotNull());

  auto dim_x = root->CreateDimension("x", "", "", 10);
  auto dim_y = root->CreateDimension("y", "", "", 10);
  auto dt_byte = GDALExtendedDataType::Create(GDT_Byte);

  // 1. Array creation and error branches
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateMDArray("", {dim_x}, dt_byte, nullptr), IsNull());
  }
  auto arr1 = root->CreateMDArray("arr1", {dim_x}, dt_byte, nullptr);
  ASSERT_THAT(arr1, NotNull());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(root->CreateMDArray("arr1", {dim_x}, dt_byte, nullptr),
                IsNull());
  }
  EXPECT_THAT(root->OpenMDArray("arr1"), NotNull());
  EXPECT_THAT(root->OpenMDArray("missing"), IsNull());
  EXPECT_EQ(1, root->GetMDArrayNames().size());

  // MEMGroupCreateMDArray
  auto arr_c = MEMGroupCreateMDArray(root.get(), "arr_c", {dim_x}, dt_byte,
                                     nullptr, nullptr);
  ASSERT_THAT(arr_c, NotNull());

  // 2. 0D scalar array (Read and Write)
  auto arr_0d =
      root->CreateMDArray("scalar", {}, GDALExtendedDataType::Create(GDT_Int32),
                          nullptr);
  ASSERT_THAT(arr_0d, NotNull());
  int val_write = 12345;
  EXPECT_TRUE(arr_0d->Write(nullptr, nullptr, nullptr, nullptr,
                            arr_0d->GetDataType(), &val_write));
  int val_read = 0;
  EXPECT_TRUE(arr_0d->Read(nullptr, nullptr, nullptr, nullptr,
                           arr_0d->GetDataType(), &val_read));
  EXPECT_EQ(12345, val_read);

  // 3. FastCopy branches: element sizes 1, 2, 4, 8, 16 with count=8 and count=9
  auto test_fast_copy = [&](GDALDataType dt, size_t count) {
    const std::string name =
        std::string("fc_") + GDALGetDataTypeName(dt) + "_" + std::to_string(count);
    auto dim = root->CreateDimension(name + "_dim", "", "", count * 2);
    auto edt = GDALExtendedDataType::Create(dt);
    auto arr = root->CreateMDArray(name, {dim}, edt, nullptr);
    ASSERT_THAT(arr, NotNull());

    const size_t dt_size = GDALGetDataTypeSizeBytes(dt);
    std::vector<uint8_t> src(count * dt_size * 2, 0xAB);
    const GUInt64 start_idx = 0;
    const GInt64 step = 2;  // Strided: triggers FastCopy
    const GPtrDiff_t stride = 1;
    EXPECT_TRUE(arr->Write(&start_idx, &count, &step, &stride, edt, src.data()));

    std::vector<uint8_t> dst(count * dt_size, 0);
    EXPECT_TRUE(arr->Read(&start_idx, &count, &step, &stride, edt, dst.data()));
    EXPECT_EQ(std::vector<uint8_t>(count * dt_size, 0xAB), dst);
  };

  for (size_t count : {8, 9}) {
    test_fast_copy(GDT_Byte, count);      // FastCopy<1>
    test_fast_copy(GDT_UInt16, count);    // FastCopy<2>
    test_fast_copy(GDT_Int32, count);     // FastCopy<4>
    test_fast_copy(GDT_Float64, count);   // FastCopy<8>
    test_fast_copy(GDT_CFloat64, count);  // FastCopy<16>
  }

  // 4. Cross-type numeric copy (Int32 <-> Float64 via GDALCopyWords64)
  {
    auto dim = root->CreateDimension("cross_dim", "", "", 10);
    auto arr_int = root->CreateMDArray(
        "arr_int", {dim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    std::vector<int32_t> int_data = {1, 2, 3, 4, 5};
    const GUInt64 start = 0;
    const size_t count = 5;
    const GInt64 step = 1;
    const GPtrDiff_t stride = 1;
    EXPECT_TRUE(arr_int->Write(&start, &count, &step, &stride,
                               arr_int->GetDataType(), int_data.data()));

    std::vector<double> dbl_data(5, 0.0);
    EXPECT_TRUE(arr_int->Read(&start, &count, &step, &stride,
                              GDALExtendedDataType::Create(GDT_Float64),
                              dbl_data.data()));
    EXPECT_DOUBLE_EQ(1.0, dbl_data[0]);
    EXPECT_DOUBLE_EQ(5.0, dbl_data[4]);
  }

  // 5. 1D string array read and write
  {
    auto dim = root->CreateDimension("str_dim", "", "", 3);
    auto edt_str = GDALExtendedDataType::CreateString();
    auto arr_str = root->CreateMDArray("arr_str", {dim}, edt_str, nullptr);
    const char* const str_data[] = {"first", "second", "third"};
    const GUInt64 start = 0;
    const size_t count = 3;
    const GInt64 step = 1;
    const GPtrDiff_t stride = 1;
    EXPECT_TRUE(arr_str->Write(&start, &count, &step, &stride, edt_str, str_data));

    // Overwrite to exercise FreeDynamicMemory
    const char* const overwrite_data[] = {"new1", "new2", "new3"};
    EXPECT_TRUE(arr_str->Write(&start, &count, &step, &stride, edt_str,
                               overwrite_data));
  }

  // 6. 2D, 3D, and 4D array read and write (state machine loops)
  auto dim2 = root->CreateDimension("d2", "", "", 2);
  auto arr_2d =
      root->CreateMDArray("arr_2d", {dim2, dim2}, dt_byte, nullptr);
  std::vector<uint8_t> data_2d(4, 1);
  const GUInt64 start_2d[2] = {0, 0};
  const size_t count_2d[2] = {2, 2};
  const GInt64 step_2d[2] = {1, 1};
  const GPtrDiff_t stride_2d[2] = {2, 1};
  EXPECT_TRUE(arr_2d->Write(start_2d, count_2d, step_2d, stride_2d, dt_byte,
                            data_2d.data()));

  auto arr_3d =
      root->CreateMDArray("arr_3d", {dim2, dim2, dim2}, dt_byte, nullptr);
  std::vector<uint8_t> data_3d(8, 2);
  const GUInt64 start_3d[3] = {0, 0, 0};
  const size_t count_3d[3] = {2, 2, 2};
  const GInt64 step_3d[3] = {1, 1, 1};
  const GPtrDiff_t stride_3d[3] = {4, 2, 1};
  EXPECT_TRUE(arr_3d->Write(start_3d, count_3d, step_3d, stride_3d, dt_byte,
                            data_3d.data()));

  auto arr_4d = root->CreateMDArray("arr_4d", {dim2, dim2, dim2, dim2},
                                    dt_byte, nullptr);
  std::vector<uint8_t> data_4d(16, 3);
  const GUInt64 start_4d[4] = {0, 0, 0, 0};
  const size_t count_4d[4] = {2, 2, 2, 2};
  const GInt64 step_4d[4] = {1, 1, 1, 1};
  const GPtrDiff_t stride_4d[4] = {8, 4, 2, 1};
  EXPECT_TRUE(arr_4d->Write(start_4d, count_4d, step_4d, stride_4d, dt_byte,
                            data_4d.data()));

  // 7. Non-writable array write rejection
  auto arr_ro = root->CreateMDArray(
      "arr_ro", {dim_x}, GDALExtendedDataType::Create(GDT_Byte), nullptr);
  auto mem_arr_ro = std::dynamic_pointer_cast<MEMMDArray>(arr_ro);
  ASSERT_THAT(mem_arr_ro, NotNull());
  mem_arr_ro->SetWritable(false);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    uint8_t val = 0;
    const GUInt64 st = 0;
    const size_t cnt = 1;
    const GInt64 sp = 1;
    const GPtrDiff_t str = 1;
    EXPECT_FALSE(arr_ro->Write(&st, &cnt, &sp, &str, arr_ro->GetDataType(), &val));
  }

  // 8. External memory array with DATAPOINTER and STRIDES
  {
    std::vector<uint8_t> ext_buf(12, 42);
    const std::string ptr_str = FormatPointer(ext_buf.data());
    const std::string dp_opt = "DATAPOINTER=" + ptr_str;

    auto dim_ext_a = root->CreateDimension("dim_ext_a", "", "", 3);
    auto dim_ext_b = root->CreateDimension("dim_ext_b", "", "", 4);

    // Invalid STRIDES count (1 stride for 2D array)
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      const char* const bad_opts[] = {dp_opt.c_str(), "STRIDES=1", nullptr};
      EXPECT_THAT(root->CreateMDArray("arr_bad_strides", {dim_ext_a, dim_ext_b},
                                      dt_byte, bad_opts),
                  IsNull());
    }

    // Valid STRIDES
    const char* const good_opts[] = {dp_opt.c_str(), "STRIDES=4,1", nullptr};
    auto ext_arr = root->CreateMDArray("arr_ext", {dim_ext_a, dim_ext_b},
                                       dt_byte, good_opts);
    ASSERT_THAT(ext_arr, NotNull());

    // Non-owning array rejects Resize
    {
      WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(ext_arr->Resize({4, 4}, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    }
  }

  EXPECT_TRUE(root->DeleteMDArray("arr1"));
  EXPECT_FALSE(root->DeleteMDArray("missing"));
}

// -----------------------------------------------------------------------------
// 10. MultiDimensional Metadata, Coordinates, Rename, and Resize
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, MultiDimensionalMetadataCoordinatesRenameAndResize) {
  std::unique_ptr<GDALDataset> ds(
      MEMDataset::CreateMultiDimensional("mem_meta", nullptr, nullptr));
  ASSERT_THAT(ds, NotNull());
  auto root = ds->GetRootGroup();
  ASSERT_THAT(root, NotNull());

  auto dim1 = root->CreateDimension("dim1", "", "", 10);
  auto dim2 = root->CreateDimension("dim2", "", "", 5);
  auto edt_float = GDALExtendedDataType::Create(GDT_Float32);
  auto arr = root->CreateMDArray("var", {dim1, dim2}, edt_float, nullptr);
  ASSERT_THAT(arr, NotNull());

  // 1. Metadata accessors
  EXPECT_TRUE(arr->SetUnit("km"));
  EXPECT_EQ("km", arr->GetUnit());

  const OGRSpatialReference wgs84 = GetWgs84Srs();
  EXPECT_TRUE(arr->SetSpatialRef(&wgs84));
  ASSERT_THAT(arr->GetSpatialRef(), NotNull());
  EXPECT_TRUE(arr->GetSpatialRef()->IsGeographic());

  EXPECT_TRUE(arr->SetOffset(10.5, GDT_Float64));
  bool has_offset = false;
  GDALDataType offset_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(10.5, arr->GetOffset(&has_offset, &offset_dt));
  EXPECT_TRUE(has_offset);
  EXPECT_EQ(GDT_Float64, offset_dt);

  EXPECT_TRUE(arr->SetScale(2.0, GDT_Float64));
  bool has_scale = false;
  GDALDataType scale_dt = GDT_Unknown;
  EXPECT_DOUBLE_EQ(2.0, arr->GetScale(&has_scale, &scale_dt));
  EXPECT_TRUE(has_scale);

  // 2. Raw NoData value
  float nodata_in = -999.0f;
  EXPECT_TRUE(arr->SetRawNoDataValue(&nodata_in));
  ASSERT_THAT(arr->GetRawNoDataValue(), NotNull());
  EXPECT_FLOAT_EQ(-999.0f, *static_cast<const float*>(arr->GetRawNoDataValue()));
  EXPECT_TRUE(arr->SetRawNoDataValue(nullptr));
  EXPECT_THAT(arr->GetRawNoDataValue(), IsNull());

  // 3. Array Attributes and Coordinate Variables
  auto attr_coord =
      arr->CreateAttribute("coordinates", {}, GDALExtendedDataType::CreateString());
  ASSERT_THAT(attr_coord, NotNull());
  const char* coord_val = "coord_var missing_var";
  EXPECT_TRUE(attr_coord->Write(coord_val));

  auto coord_var = root->CreateMDArray("coord_var", {dim1}, edt_float, nullptr);
  ASSERT_THAT(coord_var, NotNull());

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    auto coords = arr->GetCoordinateVariables();
    EXPECT_EQ(1, coords.size());
    EXPECT_STREQ("coord_var", coords[0]->GetName().c_str());
  }

  // Array attributes: error branches, GetAttributes, DeleteAttribute
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_THAT(arr->CreateAttribute("", {}, edt_float, nullptr), IsNull());
    EXPECT_THAT(arr->CreateAttribute("coordinates", {}, edt_float, nullptr),
                IsNull());
  }
  EXPECT_EQ(1, arr->GetAttributes(nullptr).size());
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->DeleteAttribute("missing_attr", nullptr));
  }
  EXPECT_TRUE(arr->DeleteAttribute("coordinates", nullptr));
  EXPECT_TRUE(arr->GetAttributes(nullptr).empty());

  // 4. Rename array
  EXPECT_TRUE(arr->Rename("renamed_var"));
  EXPECT_STREQ("renamed_var", arr->GetName().c_str());

  // 5. Resize
  // Shrink first dimension
  EXPECT_TRUE(arr->Resize({6, 5}, nullptr));
  EXPECT_EQ(6, dim1->GetSize());
  EXPECT_EQ(5, dim2->GetSize());

  // Grow first dimension
  EXPECT_TRUE(arr->Resize({12, 5}, nullptr));
  EXPECT_EQ(12, dim1->GetSize());

  // Resize second dimension (general temp array copy)
  EXPECT_TRUE(arr->Resize({12, 8}, nullptr));
  EXPECT_EQ(8, dim2->GetSize());

  // Error branches: dimension count mismatch
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->Resize({10}, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. String array NoData and shrinking Resize
  {
    auto str_dim = root->CreateDimension("str_shrink_dim", "", "", 4);
    auto edt_str = GDALExtendedDataType::CreateString();
    auto str_arr =
        root->CreateMDArray("str_shrink_arr", {str_dim}, edt_str, nullptr);
    ASSERT_THAT(str_arr, NotNull());

    const char* nodata_str = "NA";
    EXPECT_TRUE(str_arr->SetRawNoDataValue(&nodata_str));
    EXPECT_THAT(str_arr->GetRawNoDataValue(), NotNull());

    const char* const str_vals[] = {"val1", "val2", "val3", "val4"};
    const GUInt64 start = 0;
    const size_t count = 4;
    const GInt64 step = 1;
    const GPtrDiff_t stride = 1;
    EXPECT_TRUE(
        str_arr->Write(&start, &count, &step, &stride, edt_str, str_vals));

    // Shrinking triggers FreeDynamicMemory on trailing elements
    EXPECT_TRUE(str_arr->Resize({2}, nullptr));
    EXPECT_EQ(2, str_dim->GetSize());
  }
}

// -----------------------------------------------------------------------------
// 11. Vector Layers, Field Domains, and ExecuteSQL
// -----------------------------------------------------------------------------

TEST_F(MemDatasetTest, VectorLayersFieldDomainsAndExecuteSQL) {
  MEMDataset ds;

  // 1. ICreateLayer with geometry field and options
  const OGRSpatialReference wgs84 = GetWgs84Srs();
  OGRLayer* layer = nullptr;
  {
    OGRGeomFieldDefn geom_fld("geom", wkbPoint);
    geom_fld.SetSpatialRef(&wgs84);
    const char* const options[] = {"ADVERTIZE_UTF8=YES", "FID=my_fid", nullptr};
    layer = ds.ICreateLayer("layer1", &geom_fld, options);
    geom_fld.SetSpatialRef(nullptr);
  }
  ASSERT_THAT(layer, NotNull());
  EXPECT_STREQ("my_fid", layer->GetFIDColumn());

  // CreateLayer from OGRFeatureDefn
  const OGRFeatureDefn defn("layer2");
  OGRMemLayer* mem_layer2 = ds.CreateLayer(defn, nullptr);
  ASSERT_THAT(mem_layer2, NotNull());

  EXPECT_EQ(2, ds.GetLayerCount());
  EXPECT_THAT(ds.GetLayer(0), NotNull());
  EXPECT_THAT(ds.GetLayer(1), NotNull());
  EXPECT_THAT(ds.GetLayer(-1), IsNull());
  EXPECT_THAT(ds.GetLayer(10), IsNull());

  // 2. TestCapability
  constexpr struct {
    const char* cap;
    int expected;
  } kDatasetCaps[] = {
      {ODsCCreateLayer, TRUE},
      {ODsCDeleteLayer, TRUE},
      {ODsCCreateGeomFieldAfterCreateLayer, TRUE},
      {ODsCCurveGeometries, TRUE},
      {ODsCMeasuredGeometries, TRUE},
      {ODsCZGeometries, TRUE},
      {ODsCRandomLayerWrite, TRUE},
      {ODsCAddFieldDomain, TRUE},
      {ODsCDeleteFieldDomain, TRUE},
      {ODsCUpdateFieldDomain, TRUE},
      {"UnsupportedCapability", FALSE},
  };
  for (const auto& entry : kDatasetCaps) {
    SCOPED_TRACE(entry.cap);
    EXPECT_EQ(entry.expected, ds.TestCapability(entry.cap));
  }

  // 3. Field Domains
  std::string failure_reason;
  auto domain1 = std::make_unique<OGRCodedFieldDomain>(
      "domain1", "description", OFTInteger, OFSTNone,
      std::vector<OGRCodedValue>{});
  EXPECT_TRUE(ds.AddFieldDomain(std::move(domain1), failure_reason));

  // Duplicate add fails
  auto domain1_dup = std::make_unique<OGRCodedFieldDomain>(
      "domain1", "description", OFTInteger, OFSTNone,
      std::vector<OGRCodedValue>{});
  EXPECT_FALSE(ds.AddFieldDomain(std::move(domain1_dup), failure_reason));

  // Update field domain
  auto domain1_up = std::make_unique<OGRCodedFieldDomain>(
      "domain1", "new_description", OFTInteger, OFSTNone,
      std::vector<OGRCodedValue>{});
  EXPECT_TRUE(ds.UpdateFieldDomain(std::move(domain1_up), failure_reason));

  auto missing_domain = std::make_unique<OGRCodedFieldDomain>(
      "missing", "", OFTInteger, OFSTNone, std::vector<OGRCodedValue>{});
  EXPECT_FALSE(ds.UpdateFieldDomain(std::move(missing_domain), failure_reason));

  // Delete field domain unseals layer field and clears domain name
  OGRFieldDefn fld_with_dom("dom_fld", OFTInteger);
  fld_with_dom.SetDomainName("domain1");
  ASSERT_EQ(OGRERR_NONE, layer->CreateField(&fld_with_dom));
  EXPECT_TRUE(ds.DeleteFieldDomain("domain1", failure_reason));
  EXPECT_FALSE(ds.DeleteFieldDomain("missing", failure_reason));

  // 4. ExecuteSQL
  // PRAGMA read_only=1 sets all layers to non-updatable
  EXPECT_THAT(ds.ExecuteSQL("PRAGMA read_only=1", nullptr, nullptr), IsNull());
  EXPECT_EQ(FALSE, layer->TestCapability(OLCSequentialWrite));

  // Regular ExecuteSQL query
  OGRLayer* sql_layer = ds.ExecuteSQL("SELECT * FROM layer1", nullptr, nullptr);
  ASSERT_THAT(sql_layer, NotNull());
  absl::Cleanup sql_cleanup = [&ds, sql_layer] {
    ds.ReleaseResultSet(sql_layer);
  };
  EXPECT_EQ(0, sql_layer->GetFeatureCount(TRUE));

  // 5. DeleteLayer
  EXPECT_EQ(OGRERR_NONE, ds.DeleteLayer(1));
  EXPECT_EQ(1, ds.GetLayerCount());
  EXPECT_EQ(OGRERR_FAILURE, ds.DeleteLayer(10));
}

}  // namespace
}  // namespace autotest2
