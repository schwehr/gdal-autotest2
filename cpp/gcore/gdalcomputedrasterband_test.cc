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
// Tests GDALComputedRasterBand and GDALComputedDataset
// (gdalcomputedrasterband.cpp).
//
// Build caveat in google3: //third_party/gdal/BUILD compiles without muparser
// (HAVE_MUPARSER undefined), so GDALComputedRasterBand operators/functions
// implemented via muparser throw std::runtime_error as pinned below.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalcomputedrasterband.cpp
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdal_computedrasterband.h

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_computedrasterband.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

class GdalComputedRasterBandTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_MEM(); }

  static GDALDatasetUniquePtr CreateMemDS(int width, int height, int bands,
                                          GDALDataType dt) {
    GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
    if (drv == nullptr) {
      return nullptr;
    }
    return GDALDatasetUniquePtr(
        drv->Create("", width, height, bands, dt, nullptr));
  }
};

TEST_F(GdalComputedRasterBandTest, AllOperationsAndDataTypes) {
  GDALDatasetUniquePtr ds_byte = CreateMemDS(4, 4, 3, GDT_Byte);
  ASSERT_NE(nullptr, ds_byte);
  GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  EXPECT_EQ(CE_None, ds_byte->SetGeoTransform(gt));
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.SetWellKnownGeogCS("WGS84"));
  EXPECT_EQ(CE_None, ds_byte->SetSpatialRef(&srs));

  GDALRasterBand* b1 = ds_byte->GetRasterBand(1);
  GDALRasterBand* b2 = ds_byte->GetRasterBand(2);
  GDALRasterBand* b3 = ds_byte->GetRasterBand(3);
  ASSERT_NE(nullptr, b1);
  ASSERT_NE(nullptr, b2);
  ASSERT_NE(nullptr, b3);
  EXPECT_EQ(CE_None, b1->Fill(10));
  EXPECT_EQ(CE_None, b2->Fill(4));
  EXPECT_EQ(CE_None, b3->Fill(1));

  // Chained arithmetic: (b1 + b2) + b3 -> invokes GDALComputedDataset copy
  // ctor.
  auto sum3 = (*b1 + *b2) + *b3;
  EXPECT_EQ(4, sum3.GetXSize());
  EXPECT_EQ(4, sum3.GetYSize());

  // Dataset forwarding methods on GDALComputedDataset:
  GDALDataset* comp_ds = sum3.GetDataset();
  ASSERT_NE(nullptr, comp_ds);
  GDALGeoTransform out_gt;
  EXPECT_EQ(CE_None, comp_ds->GetGeoTransform(out_gt));
  EXPECT_DOUBLE_EQ(10.0, out_gt.xorig);
  EXPECT_NE(nullptr, comp_ds->GetSpatialRef());
  EXPECT_EQ(nullptr, comp_ds->GetMetadata("SUBDATASETS"));
  EXPECT_EQ(nullptr, comp_ds->GetMetadataItem("FOO", nullptr));
  EXPECT_NE(nullptr, comp_ds->GetInternalHandle("VRT_DATASET"));
  EXPECT_EQ(nullptr, comp_ds->GetInternalHandle("OTHER"));

  // Verify ReadBlock and RasterIO on sum3:
  double val = 0.0;
  EXPECT_EQ(CE_None, sum3.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64,
                                   0, 0, nullptr));
  EXPECT_DOUBLE_EQ(15.0, val);

  int block_x = 0;
  int block_y = 0;
  sum3.GetBlockSize(&block_x, &block_y);
  const size_t block_bytes = static_cast<size_t>(block_x) * block_y *
                             GDALGetDataTypeSizeBytes(sum3.GetRasterDataType());
  std::vector<GByte> block_buf(block_bytes, 0);
  EXPECT_EQ(CE_None, sum3.ReadBlock(0, 0, block_buf.data()));

  // Arithmetic operators:
  auto add_cst1 = *b1 + 5.0;  // GDT_Byte + [-128,127] int -> GDT_Byte
  EXPECT_EQ(GDT_Byte, add_cst1.GetRasterDataType());
  auto add_cst2 = *b1 + 500.0;  // GDT_Byte + 500 -> GDT_Float64
  EXPECT_EQ(GDT_Float64, add_cst2.GetRasterDataType());
  auto add_cst3 = 5.0 + *b1;
  EXPECT_EQ(GDT_Byte, add_cst3.GetRasterDataType());

  auto sub_bands = *b1 - *b2;
  auto sub_cst1 = *b1 - 2.0;
  auto sub_cst2 = 20.0 - *b1;
  auto neg_band = -(*b1);
  EXPECT_EQ(CE_None, neg_band.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                       GDT_Float64, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(-10.0, val);

  auto mul_bands = *b1 * *b2;
  auto mul_cst1 = *b1 * 3.0;
  auto mul_cst2 = 3.0 * *b1;

  auto div_bands = *b1 / *b2;
  auto div_cst1 = *b1 / 2.0;
  auto div_cst2 = 20.0 / *b1;
  EXPECT_EQ(CE_None, div_cst2.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                       GDT_Float64, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(2.0, val);

  // Comparison & logical operators (band op band, band op cst, cst op band).
  // Note: when GDAL is built without muparser,
  // comparison/logical/pow/IfThenElse throw std::runtime_error; EXPECT_THROW
  // exercises all those code paths.
  EXPECT_THROW((void)(*b1 > *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 > 5.0), std::runtime_error);
  EXPECT_THROW((void)(5.0 > *b1), std::runtime_error);
  EXPECT_THROW((void)(*b1 >= *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 >= 5.0), std::runtime_error);
  EXPECT_THROW((void)(5.0 >= *b1), std::runtime_error);
  EXPECT_THROW((void)(*b1 < *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 < 5.0), std::runtime_error);
  EXPECT_THROW((void)(5.0 < *b1), std::runtime_error);
  EXPECT_THROW((void)(*b1 <= *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 <= 5.0), std::runtime_error);
  EXPECT_THROW((void)(5.0 <= *b1), std::runtime_error);
  EXPECT_THROW((void)(*b1 == *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 == 10.0), std::runtime_error);
  EXPECT_THROW((void)(10.0 == *b1), std::runtime_error);
  EXPECT_THROW((void)(*b1 != *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 != 10.0), std::runtime_error);
  EXPECT_THROW((void)(10.0 != *b1), std::runtime_error);
  EXPECT_THROW((void)(*b1 && *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 && true), std::runtime_error);
  EXPECT_THROW((void)(true && *b1), std::runtime_error);
  EXPECT_THROW((void)(*b1 || *b2), std::runtime_error);
  EXPECT_THROW((void)(*b1 || false), std::runtime_error);
  EXPECT_THROW((void)(false || *b1), std::runtime_error);
  auto not_b = !(*b1);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(CE_Failure, not_b.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                         GDT_Float64, 0, 0, nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    CPLErrorReset();
  }

  // Also directly construct GDALComputedRasterBand with comparison/logical ops
  // so the switch cases in GDALComputedRasterBand constructors are covered:
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    for (auto op : {
             GDALComputedRasterBand::Operation::OP_GT,
             GDALComputedRasterBand::Operation::OP_GE,
             GDALComputedRasterBand::Operation::OP_LT,
             GDALComputedRasterBand::Operation::OP_LE,
             GDALComputedRasterBand::Operation::OP_EQ,
             GDALComputedRasterBand::Operation::OP_NE,
             GDALComputedRasterBand::Operation::OP_LOGICAL_AND,
             GDALComputedRasterBand::Operation::OP_LOGICAL_OR,
         }) {
      GDALComputedRasterBand cmp_bb(op, *b1, *b2);
      GDALComputedRasterBand cmp_bc(op, *b1, 5.0);
      GDALComputedRasterBand cmp_cb(op, 5.0, *b1);
    }
    for (auto op : {
             GDALComputedRasterBand::Operation::OP_ABS,
             GDALComputedRasterBand::Operation::OP_SQRT,
             GDALComputedRasterBand::Operation::OP_LOG,
             GDALComputedRasterBand::Operation::OP_LOG10,
         }) {
      GDALComputedRasterBand unary_b(op, *b1);
    }
    GDALComputedRasterBand pow_bb_direct(
        GDALComputedRasterBand::Operation::OP_POW, *b1, *b2);
    GDALComputedRasterBand pow_bc_direct(
        GDALComputedRasterBand::Operation::OP_POW, *b2, 3.0);
    GDALComputedRasterBand pow_cb_direct(
        GDALComputedRasterBand::Operation::OP_POW, 2.0, *b2);
    GDALComputedRasterBand ite_direct(
        GDALComputedRasterBand::Operation::OP_TERNARY, {b3, b1, b2});
    CPLErrorReset();
  }

  // AsType (OP_CAST):
  auto casted = b1->AsType(GDT_Float32);
  EXPECT_EQ(GDT_Float32, casted.GetRasterDataType());

  // Unary math functions:
  auto abs_b = gdal::abs(*b1);
  auto fabs_b = gdal::fabs(*b1);
  auto sqrt_b = gdal::sqrt(*b2);
  EXPECT_THROW((void)gdal::log(*b1), std::runtime_error);
  auto log10_b = gdal::log10(*b1);
  EXPECT_EQ(CE_None, sqrt_b.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                     GDT_Float64, 0, 0, nullptr));
  EXPECT_DOUBLE_EQ(2.0, val);
  EXPECT_EQ(CE_None, log10_b.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                      GDT_Float64, 0, 0, nullptr));
  EXPECT_NEAR(1.0, val, 1e-6);

  // pow functions:
  EXPECT_THROW((void)gdal::pow(*b1, *b2), std::runtime_error);
  auto pow_bc = gdal::pow(*b2, 3.0);
  auto pow_cb = gdal::pow(2.0, *b2);
  EXPECT_EQ(CE_None, pow_bc.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                     GDT_Float64, 0, 0, nullptr));
  EXPECT_NEAR(64.0, val, 1e-9);
  EXPECT_EQ(CE_None, pow_cb.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1,
                                     GDT_Float64, 0, 0, nullptr));
  EXPECT_NEAR(16.0, val, 1e-9);

  // min, max, mean, IfThenElse:
  auto min2 = gdal::min(*b1, *b2);
  auto max2 = gdal::max(*b1, *b2);
  auto min_multi = gdal::min(*b1, *b2, 2.0);
  auto max_multi = gdal::max(*b1, *b2, 20.0);
  auto mean_multi = gdal::mean(*b1, *b2, *b3);
  EXPECT_THROW((void)gdal::IfThenElse(*b3, *b1, *b2), std::runtime_error);
  EXPECT_THROW((void)gdal::IfThenElse(*b3, 99.0, *b2), std::runtime_error);
  EXPECT_THROW((void)gdal::IfThenElse(*b3, *b1, 88.0), std::runtime_error);
  EXPECT_THROW((void)gdal::IfThenElse(*b3, 99.0, 88.0), std::runtime_error);

  // Multi-band OP_ADD with constant (both float32-exact and non-float32-exact):
  GDALComputedRasterBand vec_add_f32(GDALComputedRasterBand::Operation::OP_ADD,
                                     {b1, b2}, 1.5);
  EXPECT_EQ(GDT_Float32, vec_add_f32.GetRasterDataType());
  GDALComputedRasterBand vec_add_f64(GDALComputedRasterBand::Operation::OP_ADD,
                                     {b1, b2}, 1.123456789012345);
  EXPECT_EQ(GDT_Float64, vec_add_f64.GetRasterDataType());

  // Float32 bands to cover GDT_Float32 branches in constructors:
  GDALDatasetUniquePtr ds_f32 = CreateMemDS(4, 4, 2, GDT_Float32);
  ASSERT_NE(nullptr, ds_f32);
  GDALRasterBand* fb1 = ds_f32->GetRasterBand(1);
  GDALRasterBand* fb2 = ds_f32->GetRasterBand(2);
  ASSERT_NE(nullptr, fb1);
  ASSERT_NE(nullptr, fb2);
  EXPECT_EQ(CE_None, fb1->Fill(4.0));
  EXPECT_EQ(CE_None, fb2->Fill(2.0));

  auto f32_add_bb = *fb1 + *fb2;
  EXPECT_EQ(GDT_Float32, f32_add_bb.GetRasterDataType());
  auto f32_add_bc = *fb1 + 1.5;
  EXPECT_EQ(GDT_Float32, f32_add_bc.GetRasterDataType());
  auto f32_div_cb = 8.0 / *fb1;
  EXPECT_EQ(GDT_Float32, f32_div_cb.GetRasterDataType());
  auto f32_sqrt = gdal::sqrt(*fb1);
  EXPECT_EQ(GDT_Float32, f32_sqrt.GetRasterDataType());

  // NoData value combinations:
  // 1. Both bands have same NoData value:
  EXPECT_EQ(CE_None, fb1->SetNoDataValue(-9999.0));
  EXPECT_EQ(CE_None, fb2->SetNoDataValue(-9999.0));
  auto same_ndv = *fb1 + *fb2;
  int has_ndv = 0;
  EXPECT_DOUBLE_EQ(-9999.0, same_ndv.GetNoDataValue(&has_ndv));
  EXPECT_EQ(1, has_ndv);

  // 2. Different NoData values:
  EXPECT_EQ(CE_None, fb2->SetNoDataValue(-1111.0));
  auto diff_ndv = *fb1 + *fb2;
  EXPECT_TRUE(std::isnan(diff_ndv.GetNoDataValue(&has_ndv)));
  EXPECT_EQ(1, has_ndv);

  // 3. Vector constructor with different NoData values:
  auto vec_diff_ndv = gdal::mean(*fb1, *fb2);
  EXPECT_TRUE(std::isnan(vec_diff_ndv.GetNoDataValue(&has_ndv)));
  EXPECT_EQ(1, has_ndv);

  // 4. One band with NoData, one without:
  EXPECT_EQ(CE_None, fb2->DeleteNoDataValue());
  auto one_ndv = *fb1 + *fb2;
  EXPECT_TRUE(std::isnan(one_ndv.GetNoDataValue(&has_ndv)));
  EXPECT_EQ(1, has_ndv);

  // Handle conversion and GDALComputedRasterBandRelease:
  auto* heap_band = new GDALComputedRasterBand(
      GDALComputedRasterBand::Operation::OP_ABS, *b1);
  GDALComputedRasterBandH handle = GDALComputedRasterBand::ToHandle(heap_band);
  EXPECT_EQ(heap_band, GDALComputedRasterBand::FromHandle(handle));
  GDALComputedRasterBandRelease(handle);
}

}  // namespace
}  // namespace autotest2
