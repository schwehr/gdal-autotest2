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
// Tests for ZarrArray base class (zarr_array.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_array.cpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_cstringlist.h"
#include "third_party/gdal/autotest2/cpp/util/cpl_memory.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/matchers.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

// White-box accessor to test protected ZarrArray methods.
struct ZarrArrayWhiteBox : public ZarrArray {
  using ZarrArray::ComputeTileCount;
  using ZarrArray::EncodeElt;
  using ZarrArray::IsEmptyTile;

  static bool CallIsEmptyTile(const GDALMDArray* arr,
                              const ZarrByteVectorQuickResize& buf) {
    const auto* zarr = dynamic_cast<const ZarrArray*>(arr);
    if (zarr == nullptr) {
      ADD_FAILURE() << "Expected non-null ZarrArray";
      return false;
    }
    // &ZarrArrayWhiteBox::IsEmptyTile has type
    // bool (ZarrArray::*)(const ZarrByteVectorQuickResize&) const, so calling
    // it on zarr avoids undefined behavior from downcasting zarr to
    // const ZarrArrayWhiteBox*.
    return (zarr->*&ZarrArrayWhiteBox::IsEmptyTile)(buf);
  }
};

class ZarrArrayTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_Zarr(); }
};

// 1. Static helper functions: ParseChunkSize, ComputeTileCount, FillBlockSize,
// GetDimensionTypeDirection
TEST_F(ZarrArrayTest, StaticHelpers) {
  // ParseChunkSize
  {
    CPLJSONArray chunks;
    chunks.Add(2);
    chunks.Add(3);
    std::vector<GUInt64> block_size;
    EXPECT_TRUE(ZarrArray::ParseChunkSize(
        chunks, GDALExtendedDataType::Create(GDT_Float32), block_size));
    ASSERT_EQ(2, block_size.size());
    EXPECT_EQ(2, block_size[0]);
    EXPECT_EQ(3, block_size[1]);
  }
  {
    // Chunk size 0 is invalid
    CPLJSONArray chunks;
    chunks.Add(2);
    chunks.Add(0);
    std::vector<GUInt64> block_size;
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(ZarrArray::ParseChunkSize(
        chunks, GDALExtendedDataType::Create(GDT_Float32), block_size));
  }
  {
    // Too large chunk size (overflow)
    CPLJSONArray chunks;
    chunks.Add(static_cast<GInt64>(1ULL << 62));
    chunks.Add(static_cast<GInt64>(1ULL << 62));
    std::vector<GUInt64> block_size;
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(ZarrArray::ParseChunkSize(
        chunks, GDALExtendedDataType::Create(GDT_Float32), block_size));
  }

  // ComputeTileCount
  {
    auto dim_y =
        std::make_shared<GDALDimensionWeakIndexingVar>("", "y", "", "", 10);
    auto dim_x =
        std::make_shared<GDALDimensionWeakIndexingVar>("", "x", "", "", 20);
    std::vector<std::shared_ptr<GDALDimension>> dims = {dim_y, dim_x};
    std::vector<GUInt64> block_size = {4, 5};
    EXPECT_EQ(12, ZarrArrayWhiteBox::ComputeTileCount("arr", dims, block_size));
  }
  {
    // Overflow (>2^64 tiles)
    auto dim1 = std::make_shared<GDALDimensionWeakIndexingVar>("", "d1", "", "",
                                                               1ULL << 40);
    auto dim2 = std::make_shared<GDALDimensionWeakIndexingVar>("", "d2", "", "",
                                                               1ULL << 40);
    std::vector<std::shared_ptr<GDALDimension>> dims = {dim1, dim2};
    std::vector<GUInt64> block_size = {1, 1};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(0, ZarrArrayWhiteBox::ComputeTileCount("huge", dims, block_size));
  }

  // FillBlockSize
  {
    // 0D
    std::vector<std::shared_ptr<GDALDimension>> dims0;
    std::vector<GUInt64> block0;
    EXPECT_TRUE(ZarrArray::FillBlockSize(
        dims0, GDALExtendedDataType::Create(GDT_Byte), block0, nullptr));
    EXPECT_TRUE(block0.empty());

    // 1D
    auto dim1 =
        std::make_shared<GDALDimensionWeakIndexingVar>("", "d", "", "", 100);
    std::vector<std::shared_ptr<GDALDimension>> dims1 = {dim1};
    std::vector<GUInt64> block1;
    EXPECT_TRUE(ZarrArray::FillBlockSize(
        dims1, GDALExtendedDataType::Create(GDT_Byte), block1, nullptr));
    ASSERT_EQ(1, block1.size());
    EXPECT_EQ(100, block1[0]);

    // 2D default (capped at 256)
    auto dim_y =
        std::make_shared<GDALDimensionWeakIndexingVar>("", "y", "", "", 500);
    auto dim_x =
        std::make_shared<GDALDimensionWeakIndexingVar>("", "x", "", "", 500);
    std::vector<std::shared_ptr<GDALDimension>> dims2 = {dim_y, dim_x};
    std::vector<GUInt64> block2;
    EXPECT_TRUE(ZarrArray::FillBlockSize(
        dims2, GDALExtendedDataType::Create(GDT_Byte), block2, nullptr));
    ASSERT_EQ(2, block2.size());
    EXPECT_EQ(256, block2[0]);
    EXPECT_EQ(256, block2[1]);

    // 3D default
    auto dim_z =
        std::make_shared<GDALDimensionWeakIndexingVar>("", "z", "", "", 10);
    std::vector<std::shared_ptr<GDALDimension>> dims3 = {dim_z, dim_y, dim_x};
    std::vector<GUInt64> block3;
    EXPECT_TRUE(ZarrArray::FillBlockSize(
        dims3, GDALExtendedDataType::Create(GDT_Byte), block3, nullptr));
    ASSERT_EQ(3, block3.size());
    EXPECT_EQ(1, block3[0]);
    EXPECT_EQ(256, block3[1]);
    EXPECT_EQ(256, block3[2]);

    // Custom BLOCKSIZE option
    const char* const opts_valid[] = {"BLOCKSIZE=16,32", nullptr};
    EXPECT_TRUE(ZarrArray::FillBlockSize(
        dims2, GDALExtendedDataType::Create(GDT_Byte), block2, opts_valid));
    ASSERT_EQ(2, block2.size());
    EXPECT_EQ(16, block2[0]);
    EXPECT_EQ(32, block2[1]);

    // Custom BLOCKSIZE with 0 value keeps default
    const char* const opts_zero[] = {"BLOCKSIZE=0,64", nullptr};
    EXPECT_TRUE(ZarrArray::FillBlockSize(
        dims2, GDALExtendedDataType::Create(GDT_Byte), block2, opts_zero));
    ASSERT_EQ(2, block2.size());
    EXPECT_EQ(256, block2[0]);
    EXPECT_EQ(64, block2[1]);

    // Custom BLOCKSIZE invalid token count
    const char* const opts_bad_tokens[] = {"BLOCKSIZE=16", nullptr};
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_FALSE(ZarrArray::FillBlockSize(
          dims2, GDALExtendedDataType::Create(GDT_Byte), block2,
          opts_bad_tokens));
    }
  }

  // GetDimensionTypeDirection
  {
    std::string osType;
    std::string osDirection;

    // standard_name = projection_x_coordinate
    CPLJSONObject attrs_px;
    attrs_px.Add("standard_name", "projection_x_coordinate");
    ZarrArray::GetDimensionTypeDirection(attrs_px, osType, osDirection);
    EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_X, osType);

    // standard_name = longitude, units = degrees_east
    osType.clear();
    osDirection.clear();
    CPLJSONObject attrs_lon;
    attrs_lon.Add("standard_name", "longitude");
    attrs_lon.Add("units", "degrees_east");
    ZarrArray::GetDimensionTypeDirection(attrs_lon, osType, osDirection);
    EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_X, osType);
    EXPECT_EQ("EAST", osDirection);

    // standard_name = projection_y_coordinate
    osType.clear();
    osDirection.clear();
    CPLJSONObject attrs_py;
    attrs_py.Add("standard_name", "projection_y_coordinate");
    ZarrArray::GetDimensionTypeDirection(attrs_py, osType, osDirection);
    EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_Y, osType);

    // standard_name = latitude, units = degrees_north
    osType.clear();
    osDirection.clear();
    CPLJSONObject attrs_lat;
    attrs_lat.Add("standard_name", "latitude");
    attrs_lat.Add("units", "degrees_north");
    ZarrArray::GetDimensionTypeDirection(attrs_lat, osType, osDirection);
    EXPECT_EQ(GDAL_DIM_TYPE_HORIZONTAL_Y, osType);
    EXPECT_EQ("NORTH", osDirection);

    // standard_name = time
    osType.clear();
    osDirection.clear();
    CPLJSONObject attrs_time;
    attrs_time.Add("standard_name", "time");
    ZarrArray::GetDimensionTypeDirection(attrs_time, osType, osDirection);
    EXPECT_EQ(GDAL_DIM_TYPE_TEMPORAL, osType);

    // axis = Z, positive = up
    osType.clear();
    osDirection.clear();
    CPLJSONObject attrs_z_up;
    attrs_z_up.Add("axis", "Z");
    attrs_z_up.Add("positive", "up");
    ZarrArray::GetDimensionTypeDirection(attrs_z_up, osType, osDirection);
    EXPECT_EQ(GDAL_DIM_TYPE_VERTICAL, osType);
    EXPECT_EQ("UP", osDirection);

    // axis = Z, positive = down
    osType.clear();
    osDirection.clear();
    CPLJSONObject attrs_z_down;
    attrs_z_down.Add("axis", "Z");
    attrs_z_down.Add("positive", "down");
    ZarrArray::GetDimensionTypeDirection(attrs_z_down, osType, osDirection);
    EXPECT_EQ(GDAL_DIM_TYPE_VERTICAL, osType);
    EXPECT_EQ("DOWN", osDirection);
  }
}

// 2. Element encoding and decoding: EncodeElt, DecodeSourceElt, UTF8 <-> UCS4
TEST_F(ZarrArrayTest, ElementEncodingAndDecoding) {
  // STRING_UNICODE without byte swapping
  {
    DtypeElt elt;
    elt.nativeType = DtypeElt::NativeType::STRING_UNICODE;
    elt.nativeOffset = 0;
    elt.nativeSize = 16;  // 4 UCS4 chars
    elt.gdalOffset = 0;
    elt.gdalSize = sizeof(char*);
    elt.needByteSwapping = false;

    const char* str = "a\xC3\xA9";  // "aé" (2 unicode chars)
    const GByte* pSrc = reinterpret_cast<const GByte*>(&str);
    std::vector<GByte> dst(16, 0xFF);
    ZarrArrayWhiteBox::EncodeElt({elt}, pSrc, dst.data());

    std::vector<GByte> gdal_buf(sizeof(char*), 0);
    ZarrArray::DecodeSourceElt({elt}, dst.data(), gdal_buf.data());
    char* decoded_str = nullptr;
    std::memcpy(&decoded_str, gdal_buf.data(), sizeof(decoded_str));
    std::unique_ptr<char, CplFreeDeleter> decoded_guard(decoded_str);
    ASSERT_NE(nullptr, decoded_str);
    EXPECT_STREQ("a\xC3\xA9", decoded_str);

    // nullptr string encodes to zeros
    const char* null_str = nullptr;
    pSrc = reinterpret_cast<const GByte*>(&null_str);
    ZarrArrayWhiteBox::EncodeElt({elt}, pSrc, dst.data());
    for (GByte b : dst) {
      EXPECT_EQ(0, b);
    }

    // Overlong string triggers truncation warning
    const char* long_str = "abcdefghij";
    pSrc = reinterpret_cast<const GByte*>(&long_str);
    {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      ZarrArrayWhiteBox::EncodeElt({elt}, pSrc, dst.data());
      EXPECT_EQ(CE_Warning, CPLGetLastErrorType());
      EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Too long string truncated"));
    }
  }

  // STRING_UNICODE with byte swapping
  {
    DtypeElt elt;
    elt.nativeType = DtypeElt::NativeType::STRING_UNICODE;
    elt.nativeOffset = 0;
    elt.nativeSize = 16;
    elt.gdalOffset = 0;
    elt.gdalSize = sizeof(char*);
    elt.needByteSwapping = true;

    const char* str = "XY";
    const GByte* pSrc = reinterpret_cast<const GByte*>(&str);
    std::vector<GByte> dst(16, 0);
    ZarrArrayWhiteBox::EncodeElt({elt}, pSrc, dst.data());

    std::vector<GByte> gdal_buf(sizeof(char*), 0);
    ZarrArray::DecodeSourceElt({elt}, dst.data(), gdal_buf.data());
    char* decoded_str = nullptr;
    std::memcpy(&decoded_str, gdal_buf.data(), sizeof(decoded_str));
    std::unique_ptr<char, CplFreeDeleter> decoded_guard(decoded_str);
    ASSERT_NE(nullptr, decoded_str);
    EXPECT_STREQ("XY", decoded_str);
  }

  // STRING_ASCII
  {
    DtypeElt elt;
    elt.nativeType = DtypeElt::NativeType::STRING_ASCII;
    elt.nativeOffset = 0;
    elt.nativeSize = 8;
    elt.gdalOffset = 0;
    elt.gdalSize = sizeof(char*);

    const char* str = "hello";
    const GByte* pSrc = reinterpret_cast<const GByte*>(&str);
    std::vector<GByte> dst(8, 0xFF);
    ZarrArrayWhiteBox::EncodeElt({elt}, pSrc, dst.data());
    EXPECT_EQ('h', dst[0]);
    EXPECT_EQ(0, dst[5]);

    std::vector<GByte> gdal_buf(sizeof(char*), 0);
    ZarrArray::DecodeSourceElt({elt}, dst.data(), gdal_buf.data());
    char* decoded_str = nullptr;
    std::memcpy(&decoded_str, gdal_buf.data(), sizeof(decoded_str));
    std::unique_ptr<char, CplFreeDeleter> decoded_guard(decoded_str);
    ASSERT_NE(nullptr, decoded_str);
    EXPECT_STREQ("hello", decoded_str);

    // nullptr string encodes to zeros
    const char* null_str = nullptr;
    pSrc = reinterpret_cast<const GByte*>(&null_str);
    ZarrArrayWhiteBox::EncodeElt({elt}, pSrc, dst.data());
    for (GByte b : dst) {
      EXPECT_EQ(0, b);
    }
  }

  // Byte swapping for numeric types: 2, 4, 8, 16 bytes
  {
    // 2 bytes UInt16
    DtypeElt elt2;
    elt2.nativeType = DtypeElt::NativeType::UNSIGNED_INT;
    elt2.nativeSize = 2;
    elt2.gdalSize = 2;
    elt2.needByteSwapping = true;
    const uint16_t val2 = 0x1234;
    uint16_t dst2 = 0;
    ZarrArrayWhiteBox::EncodeElt({elt2}, reinterpret_cast<const GByte*>(&val2),
                                 reinterpret_cast<GByte*>(&dst2));
    EXPECT_EQ(0x3412, dst2);
    uint16_t dec2 = 0;
    ZarrArray::DecodeSourceElt({elt2}, reinterpret_cast<const GByte*>(&dst2),
                               reinterpret_cast<GByte*>(&dec2));
    EXPECT_EQ(val2, dec2);

    // 2 bytes Float16 <-> Float32 (gdalTypeIsApproxOfNative = true)
    DtypeElt elt_f16;
    elt_f16.nativeType = DtypeElt::NativeType::IEEEFP;
    elt_f16.nativeSize = 2;
    elt_f16.gdalSize = 4;
    elt_f16.gdalType = GDALExtendedDataType::Create(GDT_Float32);
    elt_f16.gdalTypeIsApproxOfNative = true;
    elt_f16.needByteSwapping = true;
    const float val_f32 = 1.5f;
    uint16_t dst_f16 = 0;
    ZarrArrayWhiteBox::EncodeElt({elt_f16},
                                 reinterpret_cast<const GByte*>(&val_f32),
                                 reinterpret_cast<GByte*>(&dst_f16));
    float dec_f32 = 0.0f;
    ZarrArray::DecodeSourceElt({elt_f16},
                               reinterpret_cast<const GByte*>(&dst_f16),
                               reinterpret_cast<GByte*>(&dec_f32));
    EXPECT_FLOAT_EQ(val_f32, dec_f32);

    // 2 bytes Float16 without byte swapping
    elt_f16.needByteSwapping = false;
    ZarrArrayWhiteBox::EncodeElt({elt_f16},
                                 reinterpret_cast<const GByte*>(&val_f32),
                                 reinterpret_cast<GByte*>(&dst_f16));
    dec_f32 = 0.0f;
    ZarrArray::DecodeSourceElt({elt_f16},
                               reinterpret_cast<const GByte*>(&dst_f16),
                               reinterpret_cast<GByte*>(&dec_f32));
    EXPECT_FLOAT_EQ(val_f32, dec_f32);

    // 4 bytes UInt32
    DtypeElt elt4;
    elt4.nativeType = DtypeElt::NativeType::UNSIGNED_INT;
    elt4.nativeSize = 4;
    elt4.gdalSize = 4;
    elt4.needByteSwapping = true;
    const uint32_t val4 = 0x12345678;
    uint32_t dst4 = 0;
    ZarrArrayWhiteBox::EncodeElt({elt4}, reinterpret_cast<const GByte*>(&val4),
                                 reinterpret_cast<GByte*>(&dst4));
    EXPECT_EQ(0x78563412, dst4);
    uint32_t dec4 = 0;
    ZarrArray::DecodeSourceElt({elt4}, reinterpret_cast<const GByte*>(&dst4),
                               reinterpret_cast<GByte*>(&dec4));
    EXPECT_EQ(val4, dec4);

    // 8 bytes UInt64
    DtypeElt elt8;
    elt8.nativeType = DtypeElt::NativeType::UNSIGNED_INT;
    elt8.nativeSize = 8;
    elt8.gdalSize = 8;
    elt8.needByteSwapping = true;
    const uint64_t val8 = 0x0123456789ABCDEFULL;
    uint64_t dst8 = 0;
    ZarrArrayWhiteBox::EncodeElt({elt8}, reinterpret_cast<const GByte*>(&val8),
                                 reinterpret_cast<GByte*>(&dst8));
    EXPECT_EQ(0xEFCDAB8967452301ULL, dst8);
    uint64_t dec8 = 0;
    ZarrArray::DecodeSourceElt({elt8}, reinterpret_cast<const GByte*>(&dst8),
                               reinterpret_cast<GByte*>(&dec8));
    EXPECT_EQ(val8, dec8);

    // 8 bytes COMPLEX_IEEEFP (CFloat32)
    DtypeElt elt_c8;
    elt_c8.nativeType = DtypeElt::NativeType::COMPLEX_IEEEFP;
    elt_c8.nativeSize = 8;
    elt_c8.gdalSize = 8;
    elt_c8.needByteSwapping = true;
    const float val_c8[2] = {1.0f, -2.0f};
    float dst_c8[2] = {0.0f, 0.0f};
    ZarrArrayWhiteBox::EncodeElt({elt_c8},
                                 reinterpret_cast<const GByte*>(val_c8),
                                 reinterpret_cast<GByte*>(dst_c8));
    float dec_c8[2] = {0.0f, 0.0f};
    ZarrArray::DecodeSourceElt({elt_c8}, reinterpret_cast<const GByte*>(dst_c8),
                               reinterpret_cast<GByte*>(dec_c8));
    EXPECT_FLOAT_EQ(1.0f, dec_c8[0]);
    EXPECT_FLOAT_EQ(-2.0f, dec_c8[1]);

    // 16 bytes COMPLEX_IEEEFP (CFloat64)
    DtypeElt elt_c16;
    elt_c16.nativeType = DtypeElt::NativeType::COMPLEX_IEEEFP;
    elt_c16.nativeSize = 16;
    elt_c16.gdalSize = 16;
    elt_c16.needByteSwapping = true;
    const double val_c16[2] = {3.5, -4.5};
    double dst_c16[2] = {0.0, 0.0};
    ZarrArrayWhiteBox::EncodeElt({elt_c16},
                                 reinterpret_cast<const GByte*>(val_c16),
                                 reinterpret_cast<GByte*>(dst_c16));
    double dec_c16[2] = {0.0, 0.0};
    ZarrArray::DecodeSourceElt({elt_c16},
                               reinterpret_cast<const GByte*>(dst_c16),
                               reinterpret_cast<GByte*>(dec_c16));
    EXPECT_DOUBLE_EQ(3.5, dec_c16[0]);
    EXPECT_DOUBLE_EQ(-4.5, dec_c16[1]);
  }
}

// 3. Optimized IRead and IWrite across all 5 sizes, empty tiles, and
// IsEmptyTile
TEST_F(ZarrArrayTest, OptimizedReadWriteAndEmptyTile) {
  const ScopedTestDir test_dir("zarr_array_opt");
  const std::string root_dir = test_dir.File("opt_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);

  const char* const ds_opts[] = {"FORMAT=ZARR_V2", nullptr};
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(root_dir.c_str(), nullptr, ds_opts));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  auto dim_y = grp->CreateDimension("y", "", "", 4, nullptr);
  auto dim_x = grp->CreateDimension("x", "", "", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  const char* const arr_opts[] = {"BLOCKSIZE=2,2", nullptr};

  // 1-byte GDT_Byte: single-element write, empty tile read with 0 nodata and
  // non-zero nodata
  {
    auto arr_b =
        grp->CreateMDArray("arr_b", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Byte), arr_opts);
    ASSERT_NE(nullptr, arr_b);
    // Read empty tile (nodata=0 default)
    uint8_t rdata[4] = {9, 9, 9, 9};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_b->Read(start, count, step, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(0, rdata[i]);
    }

    // Set non-zero nodata value
    EXPECT_TRUE(arr_b->SetNoDataValue(42.0));
    // Read empty tile (nodata=42)
    const GUInt64 start_t2[2] = {2, 2};
    EXPECT_TRUE(arr_b->Read(start_t2, count, step, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(42, rdata[i]);
    }

    // Single-element write (count = {1, 1} hits optimized 1-byte path)
    const size_t count1[2] = {1, 1};
    const uint8_t wval = 100;
    EXPECT_TRUE(arr_b->Write(start, count1, step, nullptr,
                             GDALExtendedDataType::Create(GDT_Byte), &wval));
    uint8_t rval = 0;
    EXPECT_TRUE(arr_b->Read(start, count1, step, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte), &rval));
    EXPECT_EQ(100, rval);
  }

  // 2-byte GDT_UInt16
  {
    auto arr_u16 =
        grp->CreateMDArray("arr_u16", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_UInt16), arr_opts);
    ASSERT_NE(nullptr, arr_u16);
    EXPECT_TRUE(arr_u16->SetNoDataValue(1000.0));
    uint16_t rdata[4] = {0};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_u16->Read(start, count, step, nullptr,
                              GDALExtendedDataType::Create(GDT_UInt16), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(1000, rdata[i]);
    }
    const size_t count1[2] = {1, 1};
    const uint16_t wval = 5000;
    EXPECT_TRUE(arr_u16->Write(start, count1, step, nullptr,
                               GDALExtendedDataType::Create(GDT_UInt16),
                               &wval));
  }

  // 4-byte GDT_Int32
  {
    auto arr_i32 =
        grp->CreateMDArray("arr_i32", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int32), arr_opts);
    ASSERT_NE(nullptr, arr_i32);
    EXPECT_TRUE(arr_i32->SetNoDataValue(-9999.0));
    int32_t rdata[4] = {0};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_i32->Read(start, count, step, nullptr,
                              GDALExtendedDataType::Create(GDT_Int32), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(-9999, rdata[i]);
    }
    const size_t count1[2] = {1, 1};
    const int32_t wval = 123456;
    EXPECT_TRUE(arr_i32->Write(start, count1, step, nullptr,
                               GDALExtendedDataType::Create(GDT_Int32), &wval));
  }

  // 8-byte GDT_Float64 (and SerializeNumericNoData with NaN, Inf, -Inf)
  {
    auto arr_f64 =
        grp->CreateMDArray("arr_f64", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Float64), arr_opts);
    ASSERT_NE(nullptr, arr_f64);
    EXPECT_TRUE(
        arr_f64->SetNoDataValue(std::numeric_limits<double>::quiet_NaN()));
    EXPECT_TRUE(
        arr_f64->SetNoDataValue(std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(
        arr_f64->SetNoDataValue(-std::numeric_limits<double>::infinity()));
    const GUInt64 start[2] = {0, 0};
    const size_t count1[2] = {1, 1};
    const GInt64 step[2] = {1, 1};
    const double wval = 3.1415926535;
    EXPECT_TRUE(arr_f64->Write(start, count1, step, nullptr,
                               GDALExtendedDataType::Create(GDT_Float64),
                               &wval));
  }

  // 8-byte GDT_Int64 and GDT_UInt64 NoData serialization
  {
    auto arr_i64 =
        grp->CreateMDArray("arr_i64", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int64), arr_opts);
    ASSERT_NE(nullptr, arr_i64);
    EXPECT_TRUE(
        arr_i64->SetNoDataValue(static_cast<int64_t>(-123456789012345LL)));

    auto arr_u64 =
        grp->CreateMDArray("arr_u64", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_UInt64), arr_opts);
    ASSERT_NE(nullptr, arr_u64);
    EXPECT_TRUE(
        arr_u64->SetNoDataValue(static_cast<uint64_t>(9876543210987654ULL)));
  }

  // 16-byte GDT_CFloat64
  {
    auto arr_cf64 = grp->CreateMDArray(
        "arr_cf64", {dim_y, dim_x}, GDALExtendedDataType::Create(GDT_CFloat64),
        arr_opts);
    ASSERT_NE(nullptr, arr_cf64);
    const GUInt64 start[2] = {0, 0};
    const size_t count1[2] = {1, 1};
    const GInt64 step[2] = {1, 1};
    const double wval[2] = {1.5, -2.5};
    EXPECT_TRUE(arr_cf64->Write(start, count1, step, nullptr,
                                GDALExtendedDataType::Create(GDT_CFloat64),
                                wval));
    double rval[2] = {0.0, 0.0};
    EXPECT_TRUE(arr_cf64->Read(start, count1, step, nullptr,
                               GDALExtendedDataType::Create(GDT_CFloat64),
                               rval));
    EXPECT_DOUBLE_EQ(1.5, rval[0]);
    EXPECT_DOUBLE_EQ(-2.5, rval[1]);
  }

  // Partial tile write on empty tile with partial array dimension (dim=3,
  // block=4)
  {
    auto dim3 = grp->CreateDimension("d3", "", "", 3, nullptr);
    const char* const opts_b4[] = {"BLOCKSIZE=4", nullptr};
    auto arr_part = grp->CreateMDArray(
        "arr_part", {dim3}, GDALExtendedDataType::Create(GDT_Byte), opts_b4);
    ASSERT_NE(nullptr, arr_part);
    const GUInt64 start[1] = {0};
    const size_t count[1] = {3};
    const GInt64 step[1] = {1};
    const uint8_t wdata[3] = {1, 2, 3};
    EXPECT_TRUE(arr_part->Write(start, count, step, nullptr,
                                GDALExtendedDataType::Create(GDT_Byte), wdata));
  }

  // White-box IsEmptyTile tests: word scan, tail bytes, and complex types
  {
    auto arr_emp_b =
        grp->CreateMDArray("arr_emp_b", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Byte), arr_opts);
    ASSERT_NE(nullptr, arr_emp_b);

    ZarrByteVectorQuickResize b16;
    b16.resize(16);
    memset(b16.data(), 0, 16);
    EXPECT_TRUE(ZarrArrayWhiteBox::CallIsEmptyTile(arr_emp_b.get(), b16));

    b16[0] = 1;
    EXPECT_FALSE(ZarrArrayWhiteBox::CallIsEmptyTile(arr_emp_b.get(), b16));

    ZarrByteVectorQuickResize b9;
    b9.resize(9);
    memset(b9.data(), 0, 9);
    EXPECT_TRUE(ZarrArrayWhiteBox::CallIsEmptyTile(arr_emp_b.get(), b9));

    b9[8] = 1;  // Tail byte non-zero
    EXPECT_FALSE(ZarrArrayWhiteBox::CallIsEmptyTile(arr_emp_b.get(), b9));

    // Non-zero NoData on signed int
    auto arr_emp_i32 =
        grp->CreateMDArray("arr_emp_i32", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int32), arr_opts);
    ASSERT_NE(nullptr, arr_emp_i32);
    EXPECT_TRUE(arr_emp_i32->SetNoDataValue(-99.0));
    ZarrByteVectorQuickResize b_i32;
    b_i32.resize(4 * sizeof(int32_t));
    int32_t* p_i32 = reinterpret_cast<int32_t*>(b_i32.data());
    for (int i = 0; i < 4; ++i) p_i32[i] = -99;
    EXPECT_TRUE(ZarrArrayWhiteBox::CallIsEmptyTile(arr_emp_i32.get(), b_i32));
    p_i32[0] = 0;
    EXPECT_FALSE(ZarrArrayWhiteBox::CallIsEmptyTile(arr_emp_i32.get(), b_i32));

    // Complex array with non-zero NoData returns false
    auto arr_emp_cf64 = grp->CreateMDArray(
        "arr_emp_cf64", {dim_y, dim_x},
        GDALExtendedDataType::Create(GDT_CFloat64), arr_opts);
    ASSERT_NE(nullptr, arr_emp_cf64);
    EXPECT_TRUE(arr_emp_cf64->SetNoDataValue(1.0));
    ZarrByteVectorQuickResize b_cf;
    b_cf.resize(32);
    memset(b_cf.data(), 0, 32);
    EXPECT_FALSE(ZarrArrayWhiteBox::CallIsEmptyTile(arr_emp_cf64.get(), b_cf));
  }

  dim_y.reset();
  dim_x.reset();
  grp.reset();
  ds.reset();
  EXPECT_THAT(ReadVsiFile(test_dir.File("opt_store/arr_f64/.zarray")),
              HasSubstr("-Infinity"));
}

// 4. BlockTranspose across element sizes 1, 2, 4, 8, 16 with
// CHUNK_MEMORY_LAYOUT=F
TEST_F(ZarrArrayTest, BlockTransposeAllSizes) {
  const ScopedTestDir test_dir("zarr_array_trans");
  const std::string root_dir = test_dir.File("trans_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);

  const char* const ds_opts[] = {"FORMAT=ZARR_V2", nullptr};
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(root_dir.c_str(), nullptr, ds_opts));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  auto dim_y = grp->CreateDimension("y", "", "", 4, nullptr);
  auto dim_x = grp->CreateDimension("x", "", "", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  const char* const f_opts[] = {"BLOCKSIZE=2,2", "CHUNK_MEMORY_LAYOUT=F",
                                nullptr};

  // Size 1: GDT_Byte
  auto a_b = grp->CreateMDArray("a_b", {dim_y, dim_x},
                                GDALExtendedDataType::Create(GDT_Byte), f_opts);
  ASSERT_NE(nullptr, a_b);
  const uint8_t wb[4] = {10, 20, 30, 40};
  const GUInt64 start[2] = {0, 0};
  const size_t count[2] = {2, 2};
  const GInt64 step[2] = {1, 1};
  EXPECT_TRUE(a_b->Write(start, count, step, nullptr,
                         GDALExtendedDataType::Create(GDT_Byte), wb));

  // Size 2: GDT_UInt16
  auto a_u16 =
      grp->CreateMDArray("a_u16", {dim_y, dim_x},
                         GDALExtendedDataType::Create(GDT_UInt16), f_opts);
  ASSERT_NE(nullptr, a_u16);
  const uint16_t wu16[4] = {100, 200, 300, 400};
  EXPECT_TRUE(a_u16->Write(start, count, step, nullptr,
                           GDALExtendedDataType::Create(GDT_UInt16), wu16));

  // Size 4: GDT_Int32
  auto a_i32 = grp->CreateMDArray(
      "a_i32", {dim_y, dim_x}, GDALExtendedDataType::Create(GDT_Int32), f_opts);
  ASSERT_NE(nullptr, a_i32);
  const int32_t wi32[4] = {1000, 2000, 3000, 4000};
  EXPECT_TRUE(a_i32->Write(start, count, step, nullptr,
                           GDALExtendedDataType::Create(GDT_Int32), wi32));

  // Size 8: GDT_Float64
  auto a_f64 =
      grp->CreateMDArray("a_f64", {dim_y, dim_x},
                         GDALExtendedDataType::Create(GDT_Float64), f_opts);
  ASSERT_NE(nullptr, a_f64);
  const double wf64[4] = {1.5, 2.5, 3.5, 4.5};
  EXPECT_TRUE(a_f64->Write(start, count, step, nullptr,
                           GDALExtendedDataType::Create(GDT_Float64), wf64));

  // Size 16: GDT_CFloat64
  auto a_cf64 =
      grp->CreateMDArray("a_cf64", {dim_y, dim_x},
                         GDALExtendedDataType::Create(GDT_CFloat64), f_opts);
  ASSERT_NE(nullptr, a_cf64);
  const double wcf64[8] = {1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 4.0, -4.0};
  EXPECT_TRUE(a_cf64->Write(start, count, step, nullptr,
                            GDALExtendedDataType::Create(GDT_CFloat64), wcf64));

  // Close dataset to flush Fortran layout chunks to disk
  ds.reset();

  // Re-open dataset and read back to exercise decode BlockTranspose for all
  // sizes!
  ds.reset(GDALDataset::Open(root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, ds);
  grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  {
    auto ra_b = grp->OpenMDArray("a_b", nullptr);
    ASSERT_NE(nullptr, ra_b);
    uint8_t rb[4] = {0};
    EXPECT_TRUE(ra_b->Read(start, count, step, nullptr,
                           GDALExtendedDataType::Create(GDT_Byte), rb));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(wb[i], rb[i]);
    }
  }
  {
    auto ra_u16 = grp->OpenMDArray("a_u16", nullptr);
    ASSERT_NE(nullptr, ra_u16);
    uint16_t ru16[4] = {0};
    EXPECT_TRUE(ra_u16->Read(start, count, step, nullptr,
                             GDALExtendedDataType::Create(GDT_UInt16), ru16));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(wu16[i], ru16[i]);
    }
  }
  {
    auto ra_i32 = grp->OpenMDArray("a_i32", nullptr);
    ASSERT_NE(nullptr, ra_i32);
    int32_t ri32[4] = {0};
    EXPECT_TRUE(ra_i32->Read(start, count, step, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32), ri32));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(wi32[i], ri32[i]);
    }
  }
  {
    auto ra_f64 = grp->OpenMDArray("a_f64", nullptr);
    ASSERT_NE(nullptr, ra_f64);
    double rf64[4] = {0};
    EXPECT_TRUE(ra_f64->Read(start, count, step, nullptr,
                             GDALExtendedDataType::Create(GDT_Float64), rf64));
    for (int i = 0; i < 4; ++i) {
      EXPECT_DOUBLE_EQ(wf64[i], rf64[i]);
    }
  }
  {
    auto ra_cf64 = grp->OpenMDArray("a_cf64", nullptr);
    ASSERT_NE(nullptr, ra_cf64);
    double rcf64[8] = {0};
    EXPECT_TRUE(ra_cf64->Read(start, count, step, nullptr,
                              GDALExtendedDataType::Create(GDT_CFloat64),
                              rcf64));
    for (int i = 0; i < 8; ++i) {
      EXPECT_DOUBLE_EQ(wcf64[i], rcf64[i]);
    }
  }
}

// 5. Unoptimized IRead/IWrite loops, 0D scalars, negative arrayStep, strings,
// and compounds
TEST_F(ZarrArrayTest, UnoptimizedReadWriteAndNegativeSteps) {
  const CPLConfigOptionSetter setter("GDAL_ZARR_USE_OPTIMIZED_CODE_PATHS", "NO",
                                     false);

  const ScopedTestDir test_dir("zarr_array_unopt");
  const std::string root_dir = test_dir.File("unopt_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);

  const char* const ds_opts[] = {"FORMAT=ZARR_V2", nullptr};
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(root_dir.c_str(), nullptr, ds_opts));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  // 0D scalar array
  {
    auto arr_scalar = grp->CreateMDArray(
        "arr_scalar", {}, GDALExtendedDataType::Create(GDT_Float32), nullptr);
    ASSERT_NE(nullptr, arr_scalar);
    const float val = 42.5f;
    EXPECT_TRUE(arr_scalar->Write(nullptr, nullptr, nullptr, nullptr,
                                  GDALExtendedDataType::Create(GDT_Float32),
                                  &val));
    float rval = 0.0f;
    EXPECT_TRUE(arr_scalar->Read(nullptr, nullptr, nullptr, nullptr,
                                 GDALExtendedDataType::Create(GDT_Float32),
                                 &rval));
    EXPECT_FLOAT_EQ(42.5f, rval);
  }

  // 2D arrays with unoptimized loops across sizes 1, 2, 4, 8, 16 with NoData
  auto dim_y = grp->CreateDimension("y", "", "", 4, nullptr);
  auto dim_x = grp->CreateDimension("x", "", "", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);
  const char* const arr_opts[] = {"BLOCKSIZE=2,2", nullptr};

  // GDT_Byte: unoptimized empty tile read with zero NoData and non-zero NoData
  {
    auto arr_b =
        grp->CreateMDArray("arr_b", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Byte), arr_opts);
    ASSERT_NE(nullptr, arr_b);
    uint8_t rdata[4] = {9, 9, 9, 9};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_b->Read(start, count, step, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(0, rdata[i]);
    }

    EXPECT_TRUE(arr_b->SetNoDataValue(77.0));
    EXPECT_TRUE(arr_b->Read(start, count, step, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(77, rdata[i]);
    }

    // Write non-empty tile
    const uint8_t wdata[4] = {1, 2, 3, 4};
    EXPECT_TRUE(arr_b->Write(start, count, step, nullptr,
                             GDALExtendedDataType::Create(GDT_Byte), wdata));
    EXPECT_TRUE(arr_b->Read(start, count, step, nullptr,
                            GDALExtendedDataType::Create(GDT_Byte), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(wdata[i], rdata[i]);
    }
  }

  // GDT_UInt16: unoptimized loops for size 2
  {
    auto arr_u16 =
        grp->CreateMDArray("arr_u16_unopt", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_UInt16), arr_opts);
    ASSERT_NE(nullptr, arr_u16);
    uint16_t rdata[4] = {9, 9, 9, 9};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_u16->Read(start, count, step, nullptr,
                              GDALExtendedDataType::Create(GDT_UInt16), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(0, rdata[i]);
    }
    EXPECT_TRUE(arr_u16->SetNoDataValue(1234.0));
    EXPECT_TRUE(arr_u16->Read(start, count, step, nullptr,
                              GDALExtendedDataType::Create(GDT_UInt16), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(1234, rdata[i]);
    }
    const uint16_t wdata[4] = {10, 20, 30, 40};
    EXPECT_TRUE(arr_u16->Write(start, count, step, nullptr,
                               GDALExtendedDataType::Create(GDT_UInt16),
                               wdata));
    EXPECT_TRUE(arr_u16->Read(start, count, step, nullptr,
                              GDALExtendedDataType::Create(GDT_UInt16), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(wdata[i], rdata[i]);
    }
  }

  // GDT_Int32: unoptimized loops for size 4
  {
    auto arr_i32_u =
        grp->CreateMDArray("arr_i32_u", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int32), arr_opts);
    ASSERT_NE(nullptr, arr_i32_u);
    int32_t rdata[4] = {9, 9, 9, 9};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_i32_u->Read(start, count, step, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32),
                                rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(0, rdata[i]);
    }
    EXPECT_TRUE(arr_i32_u->SetNoDataValue(-5555.0));
    EXPECT_TRUE(arr_i32_u->Read(start, count, step, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32),
                                rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(-5555, rdata[i]);
    }
    const int32_t wdata[4] = {100, 200, 300, 400};
    EXPECT_TRUE(arr_i32_u->Write(start, count, step, nullptr,
                                 GDALExtendedDataType::Create(GDT_Int32),
                                 wdata));
    EXPECT_TRUE(arr_i32_u->Read(start, count, step, nullptr,
                                GDALExtendedDataType::Create(GDT_Int32),
                                rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(wdata[i], rdata[i]);
    }
  }

  // GDT_Float64: unoptimized loops for size 8
  {
    auto arr_f64_u =
        grp->CreateMDArray("arr_f64_u", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Float64), arr_opts);
    ASSERT_NE(nullptr, arr_f64_u);
    double rdata[4] = {9.0, 9.0, 9.0, 9.0};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_f64_u->Read(start, count, step, nullptr,
                                GDALExtendedDataType::Create(GDT_Float64),
                                rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_DOUBLE_EQ(0.0, rdata[i]);
    }
    EXPECT_TRUE(arr_f64_u->SetNoDataValue(3.14159));
    EXPECT_TRUE(arr_f64_u->Read(start, count, step, nullptr,
                                GDALExtendedDataType::Create(GDT_Float64),
                                rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_DOUBLE_EQ(3.14159, rdata[i]);
    }
    const double wdata[4] = {1.1, 2.2, 3.3, 4.4};
    EXPECT_TRUE(arr_f64_u->Write(start, count, step, nullptr,
                                 GDALExtendedDataType::Create(GDT_Float64),
                                 wdata));
    EXPECT_TRUE(arr_f64_u->Read(start, count, step, nullptr,
                                GDALExtendedDataType::Create(GDT_Float64),
                                rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_DOUBLE_EQ(wdata[i], rdata[i]);
    }
  }

  // GDT_CFloat64: unoptimized loops for size 16
  {
    auto arr_cf64_u = grp->CreateMDArray(
        "arr_cf64_u", {dim_y, dim_x},
        GDALExtendedDataType::Create(GDT_CFloat64), arr_opts);
    ASSERT_NE(nullptr, arr_cf64_u);
    double rdata[8] = {9.0, 9.0, 9.0, 9.0, 9.0, 9.0, 9.0, 9.0};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_cf64_u->Read(start, count, step, nullptr,
                                 GDALExtendedDataType::Create(GDT_CFloat64),
                                 rdata));
    for (int i = 0; i < 8; ++i) {
      EXPECT_DOUBLE_EQ(0.0, rdata[i]);
    }
    const double wdata[8] = {1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 4.0, -4.0};
    EXPECT_TRUE(arr_cf64_u->Write(start, count, step, nullptr,
                                  GDALExtendedDataType::Create(GDT_CFloat64),
                                  wdata));
    EXPECT_TRUE(arr_cf64_u->Read(start, count, step, nullptr,
                                 GDALExtendedDataType::Create(GDT_CFloat64),
                                 rdata));
    for (int i = 0; i < 8; ++i) {
      EXPECT_DOUBLE_EQ(wdata[i], rdata[i]);
    }
  }

  // Cross-type read/write in unoptimized mode (GDT_Int32 array with GDT_Float64
  // buffer)
  {
    auto arr_i32 =
        grp->CreateMDArray("arr_i32", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Int32), arr_opts);
    ASSERT_NE(nullptr, arr_i32);
    const double wdf[4] = {10.0, 20.0, 30.0, 40.0};
    const GUInt64 start[2] = {0, 0};
    const size_t count[2] = {2, 2};
    const GInt64 step[2] = {1, 1};
    EXPECT_TRUE(arr_i32->Write(start, count, step, nullptr,
                               GDALExtendedDataType::Create(GDT_Float64), wdf));
    double rdf[4] = {0.0};
    EXPECT_TRUE(arr_i32->Read(start, count, step, nullptr,
                              GDALExtendedDataType::Create(GDT_Float64), rdf));
    for (int i = 0; i < 4; ++i) {
      EXPECT_DOUBLE_EQ(wdf[i], rdf[i]);
    }
  }

  // 3D array with strided stepping and negative stepping
  {
    auto dim_z = grp->CreateDimension("z", "", "", 4, nullptr);
    const char* const arr_opts_3d[] = {"BLOCKSIZE=2,2,2", nullptr};
    auto arr_3d = grp->CreateMDArray("arr_3d", {dim_z, dim_y, dim_x},
                                     GDALExtendedDataType::Create(GDT_Int32),
                                     arr_opts_3d);
    ASSERT_NE(nullptr, arr_3d);

    // Fill array sequentially
    std::vector<int32_t> full_cube(64);
    for (int i = 0; i < 64; ++i) full_cube[i] = i;
    const GUInt64 start_full[3] = {0, 0, 0};
    const size_t count_full[3] = {4, 4, 4};
    const GInt64 step_full[3] = {1, 1, 1};
    EXPECT_TRUE(arr_3d->Write(start_full, count_full, step_full, nullptr,
                              GDALExtendedDataType::Create(GDT_Int32),
                              full_cube.data()));

    // Positive strided step: arrayStep = {2, 2, 2}
    const size_t count_sub[3] = {2, 2, 2};
    const GInt64 step_pos[3] = {2, 2, 2};
    std::vector<int32_t> read_sub(8, 0);
    EXPECT_TRUE(arr_3d->Read(start_full, count_sub, step_pos, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32),
                             read_sub.data()));
    EXPECT_EQ(0, read_sub[0]);
    EXPECT_EQ(2, read_sub[1]);
    EXPECT_EQ(8, read_sub[2]);
    EXPECT_EQ(10, read_sub[3]);

    // Negative step: arrayStep = {-1, -1, -1}
    const GUInt64 start_rev[3] = {1, 1, 1};
    const GInt64 step_neg[3] = {-1, -1, -1};
    std::vector<int32_t> read_rev(8, 0);
    EXPECT_TRUE(arr_3d->Read(start_rev, count_sub, step_neg, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32),
                             read_rev.data()));
    EXPECT_EQ(21, read_rev[0]);  // index [1, 1, 1] = 1*16 + 1*4 + 1 = 21
    EXPECT_EQ(20, read_rev[1]);  // index [1, 1, 0] = 20

    // Negative step write
    const int32_t wdata_neg[8] = {111, 222, 333, 444, 555, 666, 777, 888};
    EXPECT_TRUE(arr_3d->Write(start_rev, count_sub, step_neg, nullptr,
                              GDALExtendedDataType::Create(GDT_Int32),
                              wdata_neg));
    const GUInt64 start_zero[3] = {0, 0, 0};
    const GInt64 step_one[3] = {1, 1, 1};
    std::vector<int32_t> verify_neg(8, 0);
    EXPECT_TRUE(arr_3d->Read(start_zero, count_sub, step_one, nullptr,
                             GDALExtendedDataType::Create(GDT_Int32),
                             verify_neg.data()));
    EXPECT_EQ(888, verify_neg[0]);  // offset [0, 0, 0]
    EXPECT_EQ(222, verify_neg[6]);  // offset [1, 1, 0]
    EXPECT_EQ(111, verify_neg[7]);  // offset [1, 1, 1]
  }

  // Fixed ASCII String array
  {
    auto str_dt = GDALExtendedDataType::CreateString(8);
    auto arr_str = grp->CreateMDArray("arr_str", {dim_y}, str_dt, nullptr);
    ASSERT_NE(nullptr, arr_str);
    const GUInt64 start[1] = {0};
    const size_t count[1] = {2};
    const GInt64 step[1] = {1};
    const char* wstrings[2] = {"first", "second"};
    EXPECT_TRUE(arr_str->Write(start, count, step, nullptr, str_dt, wstrings));

    char* rstrings[2] = {nullptr, nullptr};
    EXPECT_TRUE(arr_str->Read(start, count, step, nullptr, str_dt, rstrings));
    std::unique_ptr<char, CplFreeDeleter> r0_guard(rstrings[0]);
    std::unique_ptr<char, CplFreeDeleter> r1_guard(rstrings[1]);
    ASSERT_NE(nullptr, rstrings[0]);
    ASSERT_NE(nullptr, rstrings[1]);
    EXPECT_STREQ("first", rstrings[0]);
    EXPECT_STREQ("second", rstrings[1]);
  }

  // Fixed Unicode String array
  {
    const char* const u_opts[] = {"STRING_FORMAT=UNICODE", nullptr};
    auto ustr_dt = GDALExtendedDataType::CreateString(16);
    auto arr_ustr = grp->CreateMDArray("arr_ustr", {dim_y}, ustr_dt, u_opts);
    ASSERT_NE(nullptr, arr_ustr);
    const GUInt64 start[1] = {0};
    const size_t count[1] = {1};
    const GInt64 step[1] = {1};
    const char* wustr[1] = {"a\xC3\xA9"};  // "aé"
    EXPECT_TRUE(arr_ustr->Write(start, count, step, nullptr, ustr_dt, wustr));

    char* rustr[1] = {nullptr};
    EXPECT_TRUE(arr_ustr->Read(start, count, step, nullptr, ustr_dt, rustr));
    std::unique_ptr<char, CplFreeDeleter> ru_guard(rustr[0]);
    ASSERT_NE(nullptr, rustr[0]);
    EXPECT_STREQ("a\xC3\xA9", rustr[0]);
  }

  // Compound without dynamic memory
  {
    std::vector<std::unique_ptr<GDALEDTComponent>> comps;
    comps.push_back(std::make_unique<GDALEDTComponent>(
        "a", 0, GDALExtendedDataType::Create(GDT_Int32)));
    comps.push_back(std::make_unique<GDALEDTComponent>(
        "b", 4, GDALExtendedDataType::Create(GDT_Float32)));
    auto cmp_dt = GDALExtendedDataType::Create("my_cmp", 8, std::move(comps));

    auto arr_cmp = grp->CreateMDArray("arr_cmp", {dim_y}, cmp_dt, nullptr);
    ASSERT_NE(nullptr, arr_cmp);
    struct Elt {
      int32_t a;
      float b;
    };
    const Elt wcmp[2] = {{10, 1.5f}, {20, 2.5f}};
    const GUInt64 start[1] = {0};
    const size_t count[1] = {2};
    const GInt64 step[1] = {1};
    EXPECT_TRUE(arr_cmp->Write(start, count, step, nullptr, cmp_dt, wcmp));

    Elt rcmp[2] = {};
    EXPECT_TRUE(arr_cmp->Read(start, count, step, nullptr, cmp_dt, rcmp));
    EXPECT_EQ(10, rcmp[0].a);
    EXPECT_FLOAT_EQ(1.5f, rcmp[0].b);
    EXPECT_EQ(20, rcmp[1].a);
    EXPECT_FLOAT_EQ(2.5f, rcmp[1].b);

    // Compound partial tile write onto empty tile with NoData
    const char* const cmp_part_opts[] = {"BLOCKSIZE=2,2", nullptr};
    auto arr_cmp_p =
        grp->CreateMDArray("arr_cmp_p", {dim_y, dim_x}, cmp_dt, cmp_part_opts);
    ASSERT_NE(nullptr, arr_cmp_p);
    const Elt cmp_nd = {-999, -999.0f};
    EXPECT_TRUE(arr_cmp_p->SetRawNoDataValue(&cmp_nd));
    const GUInt64 start_p[2] = {0, 0};
    const size_t count_p[2] = {1, 1};
    const GInt64 step_p[2] = {1, 1};
    const Elt w_one = {1, 1.0f};
    EXPECT_TRUE(
        arr_cmp_p->Write(start_p, count_p, step_p, nullptr, cmp_dt, &w_one));
  }

  // DeallocateDecodedTileData: multi-tile 2D string array
  {
    auto str_dt = GDALExtendedDataType::CreateString(8);
    const char* const str_block_opts[] = {"BLOCKSIZE=1,2", nullptr};
    auto arr_str_multi = grp->CreateMDArray("arr_str_multi", {dim_y, dim_x},
                                            str_dt, str_block_opts);
    ASSERT_NE(nullptr, arr_str_multi);
    const char* s_t0[2] = {"tile0_a", "tile0_b"};
    const GUInt64 s0[2] = {0, 0};
    const size_t c0[2] = {1, 2};
    const GInt64 step0[2] = {1, 1};
    EXPECT_TRUE(arr_str_multi->Write(s0, c0, step0, nullptr, str_dt, s_t0));

    // Writing to tile 1 flushes tile 0 and calls DeallocateDecodedTileData()
    const char* s_t1[2] = {"tile1_a", "tile1_b"};
    const GUInt64 s1[2] = {1, 0};
    EXPECT_TRUE(arr_str_multi->Write(s1, c0, step0, nullptr, str_dt, s_t1));
  }
}

// 6. AdviseRead / IAdviseReadCommon
TEST_F(ZarrArrayTest, AdviseRead) {
  const ScopedTestDir test_dir("zarr_array_adv");
  const std::string root_dir = test_dir.File("adv_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);

  const char* const ds_opts[] = {"FORMAT=ZARR_V2", nullptr};
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(root_dir.c_str(), nullptr, ds_opts));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  auto dim_y = grp->CreateDimension("y", "", "", 4, nullptr);
  auto dim_x = grp->CreateDimension("x", "", "", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  const char* const arr_opts[] = {"BLOCKSIZE=2,2", nullptr};
  auto arr = grp->CreateMDArray(
      "arr", {dim_y, dim_x}, GDALExtendedDataType::Create(GDT_Int32), arr_opts);
  ASSERT_NE(nullptr, arr);

  // Write some data in tile [0, 0]
  const int32_t wdata[4] = {1, 2, 3, 4};
  const GUInt64 start[2] = {0, 0};
  const size_t count[2] = {2, 2};
  const GInt64 step[2] = {1, 1};
  EXPECT_TRUE(arr->Write(start, count, step, nullptr,
                         GDALExtendedDataType::Create(GDT_Int32), wdata));

  // Flush to disk so AdviseRead can load tiles from disk into cache
  ds.reset();
  ds.reset(GDALDataset::Open(root_dir.c_str(),
                             GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);
  arr = grp->OpenMDArray("arr", nullptr);
  ASSERT_NE(nullptr, arr);

  // CACHE_SIZE = -1 -> error
  {
    const char* const bad_cs[] = {"CACHE_SIZE=-1", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->AdviseRead(start, count, bad_cs));
  }

  // CACHE_SIZE too small -> error
  {
    const char* const tiny_cs[] = {"CACHE_SIZE=1", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->AdviseRead(start, count, tiny_cs));
  }

  // NUM_THREADS = 1 -> early return true
  {
    const char* const t1_opts[] = {"NUM_THREADS=1", nullptr};
    EXPECT_TRUE(arr->AdviseRead(start, count, t1_opts));
  }

  // NUM_THREADS = 2 on whole 4x4 array
  {
    const GUInt64 start_all[2] = {0, 0};
    const size_t count_all[2] = {4, 4};
    const char* const t2_opts[] = {"NUM_THREADS=2", "CACHE_SIZE=1000000",
                                   nullptr};
    EXPECT_TRUE(arr->AdviseRead(start_all, count_all, t2_opts));

    // Read cached tile [0,0] (written tile)
    int32_t rdata[4] = {0};
    EXPECT_TRUE(arr->Read(start, count, step, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(wdata[i], rdata[i]);
    }

    // Read cached tile [2,2] (empty tile)
    const GUInt64 start_t2[2] = {2, 2};
    EXPECT_TRUE(arr->Read(start_t2, count, step, nullptr,
                          GDALExtendedDataType::Create(GDT_Int32), rdata));
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(0, rdata[i]);
    }
  }

  // AdviseRead on 1D array
  {
    auto dim_1d = grp->CreateDimension("d1", "", "", 4, nullptr);
    const char* const b2_opts[] = {"BLOCKSIZE=2", nullptr};
    auto arr_1d = grp->CreateMDArray(
        "arr_1d", {dim_1d}, GDALExtendedDataType::Create(GDT_Int32), b2_opts);
    ASSERT_NE(nullptr, arr_1d);
    const GUInt64 s1[1] = {0};
    const size_t c1[1] = {4};
    const char* const t2_opts[] = {"NUM_THREADS=2", nullptr};
    EXPECT_TRUE(arr_1d->AdviseRead(s1, c1, t2_opts));
  }

  // AdviseRead on 3D array
  {
    auto dim_z = grp->CreateDimension("z", "", "", 2, nullptr);
    auto dim_y2 = grp->CreateDimension("y2", "", "", 2, nullptr);
    auto dim_x2 = grp->CreateDimension("x2", "", "", 2, nullptr);
    const char* const b1_opts[] = {"BLOCKSIZE=1,1,1", nullptr};
    auto arr_3d =
        grp->CreateMDArray("arr_3d", {dim_z, dim_y2, dim_x2},
                           GDALExtendedDataType::Create(GDT_Int32), b1_opts);
    ASSERT_NE(nullptr, arr_3d);
    const GUInt64 s3[3] = {0, 0, 0};
    const size_t c3[3] = {2, 2, 2};
    const char* const t2_opts[] = {"NUM_THREADS=2", nullptr};
    EXPECT_TRUE(arr_3d->AdviseRead(s3, c3, t2_opts));
  }
}

// 7. CacheTilePresence and OpenTilePresenceCache
TEST_F(ZarrArrayTest, CacheTilePresence) {
  const ScopedTestDir test_dir("zarr_array_presence");
  const std::string root_dir = test_dir.File("pres_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);

  const char* const ds_opts[] = {"FORMAT=ZARR_V2", nullptr};
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(root_dir.c_str(), nullptr, ds_opts));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  auto dim_y = grp->CreateDimension("y", "", "", 4, nullptr);
  auto dim_x = grp->CreateDimension("x", "", "", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  // Single-tile array: CacheTilePresence returns true immediately
  {
    const char* const b4_opts[] = {"BLOCKSIZE=4,4", nullptr};
    auto arr_single =
        grp->CreateMDArray("arr_single", {dim_y, dim_x},
                           GDALExtendedDataType::Create(GDT_Byte), b4_opts);
    ASSERT_NE(nullptr, arr_single);
    auto zarr_single = std::dynamic_pointer_cast<ZarrArray>(arr_single);
    ASSERT_NE(nullptr, zarr_single);
    EXPECT_TRUE(zarr_single->CacheTilePresence());
  }

  // Multi-tile array (4x4 with BLOCKSIZE=2,2 -> 4 tiles)
  const char* const b2_opts[] = {"BLOCKSIZE=2,2", nullptr};
  auto arr_multi =
      grp->CreateMDArray("arr_multi", {dim_y, dim_x},
                         GDALExtendedDataType::Create(GDT_Byte), b2_opts);
  ASSERT_NE(nullptr, arr_multi);
  auto zarr_multi = std::dynamic_pointer_cast<ZarrArray>(arr_multi);
  ASSERT_NE(nullptr, zarr_multi);

  // Write 1 tile [0, 0]
  const uint8_t wdata[4] = {1, 2, 3, 4};
  const GUInt64 start[2] = {0, 0};
  const size_t count[2] = {2, 2};
  const GInt64 step[2] = {1, 1};
  EXPECT_TRUE(arr_multi->Write(start, count, step, nullptr,
                               GDALExtendedDataType::Create(GDT_Byte), wdata));

  // Flush to disk so arr_multi directory and tile exist on disk
  ds.reset();

  // Pre-create the .gmac cache file as a Zarr dataset so GetCacheRootGroup
  // opens it without requiring the netCDF driver.
  const std::string gmac_file =
      test_dir.File("pres_store/arr_multi/.zarray.gmac");
  {
    GDALDatasetUniquePtr gmac_ds(
        drv->CreateMultiDimensional(gmac_file.c_str(), nullptr, ds_opts));
    ASSERT_NE(nullptr, gmac_ds);
  }

  // Re-open pres_store in update mode
  ds.reset(GDALDataset::Open(root_dir.c_str(),
                             GDAL_OF_MULTIDIM_RASTER | GDAL_OF_UPDATE));
  ASSERT_NE(nullptr, ds);
  grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);
  auto arr_multi_reopened = grp->OpenMDArray("arr_multi", nullptr);
  ASSERT_NE(nullptr, arr_multi_reopened);
  auto zarr_multi_reopened =
      std::dynamic_pointer_cast<ZarrArray>(arr_multi_reopened);
  ASSERT_NE(nullptr, zarr_multi_reopened);

  // Add a non-tile file and an out-of-range tile in the directory to exercise
  // error branches
  WriteVsiFile(test_dir.File("pres_store/arr_multi/not_a_tile"), "dummy");
  WriteVsiFile(test_dir.File("pres_store/arr_multi/99.99"), "dummy");

  // Call CacheTilePresence() -> some_tiles_missing
  EXPECT_TRUE(zarr_multi_reopened->CacheTilePresence());

  // Call again -> already filled branch
  EXPECT_TRUE(zarr_multi_reopened->CacheTilePresence());

  // Open with CACHE_TILE_PRESENCE=YES open option
  ds.reset();
  const char* const open_opts[] = {"CACHE_TILE_PRESENCE=YES", nullptr};
  ds.reset(GDALDataset::Open(root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER, nullptr,
                             open_opts));
  ASSERT_NE(nullptr, ds);
  grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);
  auto arr_reopened = grp->OpenMDArray("arr_multi", nullptr);
  ASSERT_NE(nullptr, arr_reopened);
  uint8_t rdata[4] = {0};
  EXPECT_TRUE(arr_reopened->Read(start, count, step, nullptr,
                                 GDALExtendedDataType::Create(GDT_Byte),
                                 rdata));
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(wdata[i], rdata[i]);
  }
}

// 8. Attributes, SpatialReference, Unit, Offset, Scale, Coordinates, Resize,
// Rename, Statistics
TEST_F(ZarrArrayTest, MetadataAttributesSRSAndStructure) {
  const ScopedTestDir test_dir("zarr_array_meta");
  const std::string root_dir = test_dir.File("meta_store");
  auto drv = GetGDALDriverManager()->GetDriverByName("Zarr");
  ASSERT_NE(nullptr, drv);

  const char* const ds_opts[] = {"FORMAT=ZARR_V2", nullptr};
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(root_dir.c_str(), nullptr, ds_opts));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  auto dim_y = grp->CreateDimension("y", "", "", 4, nullptr);
  auto dim_x = grp->CreateDimension("x", "", "", 4, nullptr);
  ASSERT_NE(nullptr, dim_y);
  ASSERT_NE(nullptr, dim_x);

  auto arr =
      grp->CreateMDArray("arr", {dim_y, dim_x},
                         GDALExtendedDataType::Create(GDT_Float32), nullptr);
  ASSERT_NE(nullptr, arr);

  // Unit
  EXPECT_TRUE(arr->SetUnit("meters"));
  EXPECT_EQ("meters", arr->GetUnit());
  EXPECT_TRUE(arr->SetUnit(""));
  EXPECT_TRUE(arr->GetUnit().empty());

  // Offset & Scale
  bool has_offset = false;
  EXPECT_TRUE(arr->SetOffset(10.5));
  EXPECT_DOUBLE_EQ(10.5, arr->GetOffset(&has_offset));
  EXPECT_TRUE(has_offset);

  bool has_scale = false;
  EXPECT_TRUE(arr->SetScale(2.5));
  EXPECT_DOUBLE_EQ(2.5, arr->GetScale(&has_scale));
  EXPECT_TRUE(has_scale);

  // SpatialReference
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));
  EXPECT_TRUE(arr->SetSpatialRef(&srs));
  auto cur_srs = arr->GetSpatialRef();
  ASSERT_NE(nullptr, cur_srs);
  EXPECT_THAT(*cur_srs, IsSameAs(srs));
  EXPECT_TRUE(arr->SetSpatialRef(nullptr));
  EXPECT_EQ(nullptr, arr->GetSpatialRef());
  // Reset SRS so it is active and serialized on Close()
  EXPECT_TRUE(arr->SetSpatialRef(&srs));
  EXPECT_TRUE(arr->SetUnit("m"));

  auto coord_attr = arr->CreateAttribute(
      "coordinates", {}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, coord_attr);
  const char* coord_val = "x";
  EXPECT_TRUE(coord_attr->Write(coord_val));

  // Attributes: Create and Delete
  auto attr = arr->CreateAttribute(
      "foo", {}, GDALExtendedDataType::CreateString(), nullptr);
  ASSERT_NE(nullptr, attr);
  EXPECT_TRUE(arr->DeleteAttribute("foo"));

  // Attribute >=2D error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, arr->CreateAttribute(
                           "bad_attr", {2, 2},
                           GDALExtendedDataType::Create(GDT_Int32), nullptr));
  }

  // Statistics with UPDATE_METADATA=YES
  const GUInt64 st_start[] = {0, 0};
  const size_t st_count[] = {4, 4};
  std::vector<float> st_vals(16, 5.0f);
  st_vals[0] = 1.0f;
  st_vals[15] = 9.0f;
  EXPECT_TRUE(arr->Write(st_start, st_count, nullptr, nullptr,
                         GDALExtendedDataType::Create(GDT_Float32),
                         st_vals.data()));
  const char* const stat_opts[] = {"UPDATE_METADATA=YES", nullptr};
  double stat_min = 0.0;
  double stat_max = 0.0;
  double stat_mean = 0.0;
  double stat_stddev = 0.0;
  GUInt64 valid_count = 0;
  EXPECT_TRUE(arr->ComputeStatistics(false, &stat_min, &stat_max, &stat_mean,
                                     &stat_stddev, &valid_count, nullptr,
                                     nullptr, stat_opts));
  EXPECT_DOUBLE_EQ(1.0, stat_min);
  EXPECT_DOUBLE_EQ(9.0, stat_max);
  auto actual_range = arr->GetAttribute("actual_range");
  ASSERT_NE(nullptr, actual_range);

  // Subgroup rename and deletion
  auto sub = grp->CreateGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub);
  auto sub_arr = sub->CreateMDArray(
      "a", {dim_y}, GDALExtendedDataType::Create(GDT_Byte), nullptr);
  ASSERT_NE(nullptr, sub_arr);
  EXPECT_TRUE(sub->Rename("sub2"));
  EXPECT_TRUE(sub->DeleteMDArray("a", nullptr));

  // Resize
  // Shrinking array error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->Resize({2, 2}, nullptr));
  }
  // Wrong number of dimensions error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->Resize({6}, nullptr));
  }
  // Duplicate dimension resized to conflicting sizes error
  {
    auto dim_dup = grp->CreateDimension("dup", "", "", 4, nullptr);
    auto arr_dup =
        grp->CreateMDArray("arr_dup", {dim_dup, dim_dup},
                           GDALExtendedDataType::Create(GDT_Byte), nullptr);
    ASSERT_NE(nullptr, arr_dup);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr_dup->Resize({6, 8}, nullptr));
  }
  // Valid grow
  EXPECT_TRUE(arr->Resize({6, 8}, nullptr));
  EXPECT_EQ(6, dim_y->GetSize());
  EXPECT_EQ(8, dim_x->GetSize());

  // Rename
  // Invalid name error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(arr->Rename("."));
  }
  // Valid rename
  EXPECT_TRUE(arr->Rename("arr_renamed"));
  EXPECT_EQ("arr_renamed", arr->GetName());

  // Read-only operations fail when opened read-only
  ds.reset();
  ds.reset(GDALDataset::Open(root_dir.c_str(), GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, ds);
  grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);
  auto ro_arr = grp->OpenMDArray("arr_renamed", nullptr);
  ASSERT_NE(nullptr, ro_arr);
  auto re_srs = ro_arr->GetSpatialRef();
  ASSERT_NE(nullptr, re_srs);
  EXPECT_STREQ("4326", re_srs->GetAuthorityCode(nullptr));
  EXPECT_EQ("m", ro_arr->GetUnit());

  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(ro_arr->SetUnit("km"));
    EXPECT_FALSE(ro_arr->Resize({10, 10}, nullptr));
    EXPECT_FALSE(ro_arr->Rename("new_name"));
    EXPECT_EQ(nullptr,
              ro_arr->CreateAttribute(
                  "bar", {}, GDALExtendedDataType::CreateString(), nullptr));
    EXPECT_FALSE(ro_arr->DeleteAttribute("actual_range"));
    const float raw_nd = 0.0f;
    EXPECT_FALSE(ro_arr->SetRawNoDataValue(&raw_nd));
  }

  // GetCoordinateVariables when group has gone out of scope
  grp.reset();
  ds.reset();
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto coords = ro_arr->GetCoordinateVariables();
    EXPECT_TRUE(coords.empty());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("gone out of scope"));
  }
}

// 9. ParseSpecialAttributes: CF-1 grid_mapping and STAC proj:epsg / proj:wkt2
TEST_F(ZarrArrayTest, ParseSpecialAttributes) {
  const ScopedTestDir test_dir("zarr_array_spec_attrs");

  // CF-1 grid_mapping
  const std::string zarr_root = R"json({"zarr_format": 2})json";
  WriteVsiFile(test_dir.File("root/.zgroup"), zarr_root);

  // Coordinate variables and crs_var
  const std::string zarr_crs = R"json({
    "zarr_format": 2, "shape": [], "chunks": [], "dtype": "<i4", "order": "C",
    "fill_value": 0, "compressor": null, "filters": null
  })json";
  const std::string zattrs_crs = R"json({
    "grid_mapping_name": "latitude_longitude",
    "semi_major_axis": 6378137.0,
    "inverse_flattening": 298.257223563,
    "str_attr": "hello",
    "double_arr_attr": [1.5, 2.5]
  })json";
  WriteVsiFile(test_dir.File("root/crs_var/.zarray"), zarr_crs);
  WriteVsiFile(test_dir.File("root/crs_var/.zattrs"), zattrs_crs);

  // Data array referencing crs_var via grid_mapping and coordinates
  const std::string zarr_data = R"json({
    "zarr_format": 2, "shape": [4, 4], "chunks": [2, 2], "dtype": "<f4", "order": "C",
    "fill_value": 0, "compressor": null, "filters": null
  })json";
  const std::string zattrs_data = R"json({
    "_ARRAY_DIMENSIONS": ["lat", "lon"],
    "grid_mapping": "crs_var",
    "coordinates": "crs_var nonexistent_var",
    "units": "kelvin",
    "add_offset": 273.15,
    "scale_factor": 0.1
  })json";
  WriteVsiFile(test_dir.File("root/data/.zarray"), zarr_data);
  WriteVsiFile(test_dir.File("root/data/.zattrs"), zattrs_data);

  // STAC proj:epsg
  const std::string zattrs_stac_epsg = R"json({
    "_ARRAY_DIMENSIONS": ["lat", "lon"],
    "proj:epsg": 32632
  })json";
  WriteVsiFile(test_dir.File("root/stac_epsg/.zarray"), zarr_data);
  WriteVsiFile(test_dir.File("root/stac_epsg/.zattrs"), zattrs_stac_epsg);

  // STAC proj:wkt2
  OGRSpatialReference srs3857;
  ASSERT_EQ(OGRERR_NONE, srs3857.importFromEPSG(3857));
  char* wkt = nullptr;
  srs3857.exportToWkt(&wkt);
  std::unique_ptr<char, CplFreeDeleter> wkt_guard(wkt);
  CPLJSONObject attrs_stac_wkt_obj;
  CPLJSONArray dims_arr;
  dims_arr.Add("lat");
  dims_arr.Add("lon");
  attrs_stac_wkt_obj.Add("_ARRAY_DIMENSIONS", dims_arr);
  attrs_stac_wkt_obj.Add("proj:wkt2", wkt);
  WriteVsiFile(test_dir.File("root/stac_wkt/.zarray"), zarr_data);
  WriteVsiFile(test_dir.File("root/stac_wkt/.zattrs"),
               attrs_stac_wkt_obj.Format(CPLJSONObject::PrettyFormat::Pretty));

  // _CRS with invalid url and valid wkt fallback
  const std::string zattrs_crs_fallback = R"json({
    "_ARRAY_DIMENSIONS": ["lat", "lon"],
    "_CRS": {
      "url": "http://invalid",
      "wkt": "GEOGCRS[\"WGS 84\",DATUM[\"World Geodetic System 1984\",ELLIPSOID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],CS[ellipsoidal,2],AXIS[\"latitude\",north],AXIS[\"longitude\",east],UNIT[\"degree\",0.0174532925199433]]"
    }
  })json";
  WriteVsiFile(test_dir.File("root/crs_fallback/.zarray"), zarr_data);
  WriteVsiFile(test_dir.File("root/crs_fallback/.zattrs"), zattrs_crs_fallback);

  GDALDatasetUniquePtr ds(GDALDataset::Open(test_dir.File("root").c_str(),
                                            GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, ds);
  auto grp = ds->GetRootGroup();
  ASSERT_NE(nullptr, grp);

  {
    auto arr_data = grp->OpenMDArray("data", nullptr);
    ASSERT_NE(nullptr, arr_data);
    EXPECT_EQ("kelvin", arr_data->GetUnit());
    bool has_val = false;
    EXPECT_DOUBLE_EQ(273.15, arr_data->GetOffset(&has_val));
    EXPECT_DOUBLE_EQ(0.1, arr_data->GetScale(&has_val));
    EXPECT_NE(nullptr, arr_data->GetSpatialRef());

    // GetCoordinateVariables
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto coord_vars = arr_data->GetCoordinateVariables();
    ASSERT_EQ(1, coord_vars.size());
    EXPECT_EQ("crs_var", coord_vars[0]->GetName());
  }

  {
    auto arr_epsg = grp->OpenMDArray("stac_epsg", nullptr);
    ASSERT_NE(nullptr, arr_epsg);
    auto srs = arr_epsg->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_STREQ("32632", srs->GetAuthorityCode(nullptr));
  }

  {
    auto arr_wkt = grp->OpenMDArray("stac_wkt", nullptr);
    ASSERT_NE(nullptr, arr_wkt);
    auto srs = arr_wkt->GetSpatialRef();
    ASSERT_NE(nullptr, srs);
    EXPECT_STREQ("3857", srs->GetAuthorityCode(nullptr));
  }

  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto arr_fb = grp->OpenMDArray("crs_fallback", nullptr);
    ASSERT_NE(nullptr, arr_fb);
    EXPECT_NE(nullptr, arr_fb->GetSpatialRef());
  }
}

// TODO: b/335317901 - Cover remaining ZarrArray edge error and cache paths.

}  // namespace
}  // namespace autotest2
