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
// Tests for Zarr TIFF compressor (zarr_compressors.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_compressors.cpp

#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_compressor.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

class ZarrCompressorsTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_GTiff(); }
};

TEST_F(ZarrCompressorsTest, DecompressorMetadata) {
  const CPLCompressor* comp = ZarrGetTIFFDecompressor();
  ASSERT_NE(nullptr, comp);
  EXPECT_STREQ("imagecodecs_tiff", comp->pszId);
  EXPECT_EQ(CCT_COMPRESSOR, comp->eType);
}

TEST_F(ZarrCompressorsTest, DecompressValidByteTiff) {
  ScopedTestDir test_dir("zarr_comp");
  const std::string tif_path = test_dir.File("test_byte.tif");
  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  auto ds = GDALDatasetUniquePtr(
      drv->Create(tif_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);

  std::vector<uint8_t> in_pixels(16);
  std::iota(in_pixels.begin(), in_pixels.end(), 1);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 4, 4,
                                                    in_pixels.data(), 4, 4,
                                                    GDT_Byte, 0, 0, nullptr));
  ds.reset();

  const std::string tiff_bytes = ReadVsiFile(tif_path);
  ASSERT_FALSE(tiff_bytes.empty());

  const CPLCompressor* comp = ZarrGetTIFFDecompressor();
  ASSERT_NE(nullptr, comp);

  std::vector<uint8_t> out_buf(16, 0);
  void* out_ptr = out_buf.data();
  size_t out_size = out_buf.size();

  EXPECT_TRUE(comp->pfnFunc(tiff_bytes.data(), tiff_bytes.size(), &out_ptr,
                            &out_size, nullptr, nullptr));
  EXPECT_EQ(16, out_size);
  EXPECT_EQ(in_pixels, out_buf);
}

TEST_F(ZarrCompressorsTest, DecompressValidUInt16Tiff) {
  ScopedTestDir test_dir("zarr_comp");
  const std::string tif_path = test_dir.File("test_u16.tif");
  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  auto ds = GDALDatasetUniquePtr(
      drv->Create(tif_path.c_str(), 3, 3, 1, GDT_UInt16, nullptr));
  ASSERT_NE(nullptr, ds);

  std::vector<uint16_t> in_pixels(9);
  std::iota(in_pixels.begin(), in_pixels.end(), 1000);
  ASSERT_EQ(CE_None, ds->GetRasterBand(1)->RasterIO(GF_Write, 0, 0, 3, 3,
                                                    in_pixels.data(), 3, 3,
                                                    GDT_UInt16, 0, 0, nullptr));
  ds.reset();

  const std::string tiff_bytes = ReadVsiFile(tif_path);
  ASSERT_FALSE(tiff_bytes.empty());

  const CPLCompressor* comp = ZarrGetTIFFDecompressor();
  ASSERT_NE(nullptr, comp);

  std::vector<uint16_t> out_buf(9, 0);
  void* out_ptr = out_buf.data();
  size_t out_size = out_buf.size() * sizeof(uint16_t);

  EXPECT_TRUE(comp->pfnFunc(tiff_bytes.data(), tiff_bytes.size(), &out_ptr,
                            &out_size, nullptr, nullptr));
  EXPECT_EQ(9 * sizeof(uint16_t), out_size);
  EXPECT_EQ(in_pixels, out_buf);
}

TEST_F(ZarrCompressorsTest, DecompressCorruptTiff) {
  const uint8_t corrupt_bytes[] = {0x49, 0x49, 0x2a, 0x00, 0x00, 0x00};
  const CPLCompressor* comp = ZarrGetTIFFDecompressor();
  ASSERT_NE(nullptr, comp);

  std::vector<uint8_t> out_buf(16, 0);
  void* out_ptr = out_buf.data();
  size_t out_size = out_buf.size();

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(comp->pfnFunc(corrupt_bytes, sizeof(corrupt_bytes), &out_ptr,
                             &out_size, nullptr, nullptr));
  EXPECT_EQ(0, out_size);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrCompressorsTest, DecompressMultiBandRejected) {
  ScopedTestDir test_dir("zarr_comp");
  const std::string tif_path = test_dir.File("twoband.tif");
  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  auto ds = GDALDatasetUniquePtr(
      drv->Create(tif_path.c_str(), 4, 4, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  ds.reset();

  const std::string tiff_bytes = ReadVsiFile(tif_path);
  ASSERT_FALSE(tiff_bytes.empty());

  const CPLCompressor* comp = ZarrGetTIFFDecompressor();
  ASSERT_NE(nullptr, comp);

  std::vector<uint8_t> out_buf(32, 0);
  void* out_ptr = out_buf.data();
  size_t out_size = out_buf.size();

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(comp->pfnFunc(tiff_bytes.data(), tiff_bytes.size(), &out_ptr,
                             &out_size, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(0, out_size);
}

TEST_F(ZarrCompressorsTest, DecompressSizeMismatch) {
  ScopedTestDir test_dir("zarr_comp");
  const std::string tif_path = test_dir.File("test_size.tif");
  auto* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, drv);
  auto ds = GDALDatasetUniquePtr(
      drv->Create(tif_path.c_str(), 4, 4, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, ds);
  ds.reset();

  const std::string tiff_bytes = ReadVsiFile(tif_path);
  ASSERT_FALSE(tiff_bytes.empty());

  const CPLCompressor* comp = ZarrGetTIFFDecompressor();
  ASSERT_NE(nullptr, comp);

  std::vector<uint8_t> out_buf(15, 0);  // Expected 16, provide 15
  void* out_ptr = out_buf.data();
  size_t out_size = out_buf.size();

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(comp->pfnFunc(tiff_bytes.data(), tiff_bytes.size(), &out_ptr,
                             &out_size, nullptr, nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  EXPECT_EQ(0, out_size);
}

TEST_F(ZarrCompressorsTest, DecompressInvalidApi) {
  const CPLCompressor* comp = ZarrGetTIFFDecompressor();
  ASSERT_NE(nullptr, comp);

  const uint8_t dummy[1] = {0};
  void* out_ptr = nullptr;
  size_t out_size = 0;

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(comp->pfnFunc(dummy, sizeof(dummy), nullptr, &out_size, nullptr,
                             nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());

  CPLErrorReset();
  EXPECT_FALSE(comp->pfnFunc(dummy, sizeof(dummy), &out_ptr, &out_size, nullptr,
                             nullptr));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

}  // namespace
}  // namespace autotest2
