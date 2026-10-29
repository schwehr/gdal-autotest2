// Copyright 2026 Google Inc. All Rights Reserved.
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

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_computedrasterband.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"

namespace {

std::unique_ptr<GDALDataset> CreateMemDS(int width, int height, int bands,
                                         GDALDataType dt) {
  GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("MEM");
  if (!drv) return nullptr;
  return std::unique_ptr<GDALDataset>(
      drv->Create("", width, height, bands, dt, nullptr));
}

TEST(GdalComputedRasterBandTest, AllOperationsAndDataTypes) {
  GDALAllRegister();

  auto ds_byte = CreateMemDS(4, 4, 3, GDT_Byte);
  ASSERT_NE(ds_byte, nullptr);
  GDALGeoTransform gt(10.0, 1.0, 0.0, 20.0, 0.0, -1.0);
  ds_byte->SetGeoTransform(gt);
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  ds_byte->SetSpatialRef(&srs);

  GDALRasterBand* b1 = ds_byte->GetRasterBand(1);
  GDALRasterBand* b2 = ds_byte->GetRasterBand(2);
  GDALRasterBand* b3 = ds_byte->GetRasterBand(3);
  b1->Fill(10);
  b2->Fill(4);
  b3->Fill(1);

  // Chained arithmetic: (b1 + b2) + b3 -> invokes GDALComputedDataset copy
  // ctor.
  auto sum3 = (*b1 + *b2) + *b3;
  EXPECT_EQ(sum3.GetXSize(), 4);
  EXPECT_EQ(sum3.GetYSize(), 4);

  // Dataset forwarding methods on GDALComputedDataset:
  GDALDataset* comp_ds = sum3.GetDataset();
  ASSERT_NE(comp_ds, nullptr);
  GDALGeoTransform out_gt;
  EXPECT_EQ(comp_ds->GetGeoTransform(out_gt), CE_None);
  EXPECT_DOUBLE_EQ(out_gt.xorig, 10.0);
  EXPECT_NE(comp_ds->GetSpatialRef(), nullptr);
  EXPECT_EQ(comp_ds->GetMetadata("SUBDATASETS"), nullptr);
  EXPECT_EQ(comp_ds->GetMetadataItem("FOO", nullptr), nullptr);
  EXPECT_NE(comp_ds->GetInternalHandle("VRT_DATASET"), nullptr);
  EXPECT_EQ(comp_ds->GetInternalHandle("OTHER"), nullptr);

  // Verify ReadBlock and RasterIO on sum3:
  double val = 0.0;
  EXPECT_EQ(sum3.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0,
                          nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(val, 15.0);

  std::vector<GByte> block_buf(16 * sum3.GetRasterDataType(), 0);
  EXPECT_EQ(sum3.ReadBlock(0, 0, block_buf.data()), CE_None);

  // Arithmetic operators:
  auto add_cst1 = *b1 + 5.0;  // GDT_Byte + [-128,127] int -> GDT_Byte
  EXPECT_EQ(add_cst1.GetRasterDataType(), GDT_Byte);
  auto add_cst2 = *b1 + 500.0;  // GDT_Byte + 500 -> GDT_Float64
  EXPECT_EQ(add_cst2.GetRasterDataType(), GDT_Float64);
  auto add_cst3 = 5.0 + *b1;
  EXPECT_EQ(add_cst3.GetRasterDataType(), GDT_Byte);

  auto sub_bands = *b1 - *b2;
  auto sub_cst1 = *b1 - 2.0;
  auto sub_cst2 = 20.0 - *b1;
  auto neg_band = -(*b1);
  EXPECT_EQ(neg_band.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0,
                              0, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(val, -10.0);

  auto mul_bands = *b1 * *b2;
  auto mul_cst1 = *b1 * 3.0;
  auto mul_cst2 = 3.0 * *b1;

  auto div_bands = *b1 / *b2;
  auto div_cst1 = *b1 / 2.0;
  auto div_cst2 = 20.0 / *b1;
  EXPECT_EQ(div_cst2.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0,
                              0, nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(val, 2.0);

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
  CPLPushErrorHandler(CPLQuietErrorHandler);
  EXPECT_EQ(not_b.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0,
                           nullptr),
            CE_Failure);
  CPLPopErrorHandler();

  // Also directly construct GDALComputedRasterBand with comparison/logical ops
  // so the switch cases in GDALComputedRasterBand constructors are covered:
  {
    CPLPushErrorHandler(CPLQuietErrorHandler);
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
    CPLPopErrorHandler();
  }

  // AsType (OP_CAST):
  auto casted = b1->AsType(GDT_Float32);
  EXPECT_EQ(casted.GetRasterDataType(), GDT_Float32);

  // Unary math functions:
  auto abs_b = gdal::abs(*b1);
  auto fabs_b = gdal::fabs(*b1);
  auto sqrt_b = gdal::sqrt(*b2);
  EXPECT_THROW((void)gdal::log(*b1), std::runtime_error);
  auto log10_b = gdal::log10(*b1);
  EXPECT_EQ(sqrt_b.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0,
                            nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(val, 2.0);
  EXPECT_EQ(log10_b.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0,
                             nullptr),
            CE_None);
  EXPECT_NEAR(val, 1.0, 1e-6);

  // pow functions:
  EXPECT_THROW((void)gdal::pow(*b1, *b2), std::runtime_error);
  auto pow_bc = gdal::pow(*b2, 3.0);
  auto pow_cb = gdal::pow(2.0, *b2);
  EXPECT_EQ(pow_bc.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0,
                            nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(val, 64.0);
  EXPECT_EQ(pow_cb.RasterIO(GF_Read, 0, 0, 1, 1, &val, 1, 1, GDT_Float64, 0, 0,
                            nullptr),
            CE_None);
  EXPECT_DOUBLE_EQ(val, 16.0);

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
  EXPECT_EQ(vec_add_f32.GetRasterDataType(), GDT_Float32);
  GDALComputedRasterBand vec_add_f64(GDALComputedRasterBand::Operation::OP_ADD,
                                     {b1, b2}, 1.123456789012345);
  EXPECT_EQ(vec_add_f64.GetRasterDataType(), GDT_Float64);

  // Float32 bands to cover GDT_Float32 branches in constructors:
  auto ds_f32 = CreateMemDS(4, 4, 2, GDT_Float32);
  ASSERT_NE(ds_f32, nullptr);
  GDALRasterBand* fb1 = ds_f32->GetRasterBand(1);
  GDALRasterBand* fb2 = ds_f32->GetRasterBand(2);
  fb1->Fill(4.0);
  fb2->Fill(2.0);

  auto f32_add_bb = *fb1 + *fb2;
  EXPECT_EQ(f32_add_bb.GetRasterDataType(), GDT_Float32);
  auto f32_add_bc = *fb1 + 1.5;
  EXPECT_EQ(f32_add_bc.GetRasterDataType(), GDT_Float32);
  auto f32_div_cb = 8.0 / *fb1;
  EXPECT_EQ(f32_div_cb.GetRasterDataType(), GDT_Float32);
  auto f32_sqrt = gdal::sqrt(*fb1);
  EXPECT_EQ(f32_sqrt.GetRasterDataType(), GDT_Float32);

  // NoData value combinations:
  // 1. Both bands have same NoData value:
  fb1->SetNoDataValue(-9999.0);
  fb2->SetNoDataValue(-9999.0);
  auto same_ndv = *fb1 + *fb2;
  int has_ndv = 0;
  EXPECT_DOUBLE_EQ(same_ndv.GetNoDataValue(&has_ndv), -9999.0);
  EXPECT_EQ(has_ndv, 1);

  // 2. Different NoData values:
  fb2->SetNoDataValue(-1111.0);
  auto diff_ndv = *fb1 + *fb2;
  EXPECT_TRUE(std::isnan(diff_ndv.GetNoDataValue(&has_ndv)));
  EXPECT_EQ(has_ndv, 1);

  // 3. Vector constructor with different NoData values:
  auto vec_diff_ndv = gdal::mean(*fb1, *fb2);
  EXPECT_TRUE(std::isnan(vec_diff_ndv.GetNoDataValue(&has_ndv)));
  EXPECT_EQ(has_ndv, 1);

  // 4. One band with NoData, one without:
  fb2->DeleteNoDataValue();
  auto one_ndv = *fb1 + *fb2;
  EXPECT_TRUE(std::isnan(one_ndv.GetNoDataValue(&has_ndv)));
  EXPECT_EQ(has_ndv, 1);

  // Handle conversion and GDALComputedRasterBandRelease:
  auto* heap_band = new GDALComputedRasterBand(
      GDALComputedRasterBand::Operation::OP_ABS, *b1);
  GDALComputedRasterBandH handle = GDALComputedRasterBand::ToHandle(heap_band);
  EXPECT_EQ(GDALComputedRasterBand::FromHandle(handle), heap_band);
  GDALComputedRasterBandRelease(handle);
}

}  // namespace
