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
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace {

class UnownedSelfMDArray : public GDALMDArray {
 public:
  UnownedSelfMDArray()
      : GDALAbstractMDArray("", "unowned"),
        GDALMDArray("", "unowned"),
        dt_(GDALExtendedDataType::Create(GDT_Float64)) {}

  bool IsWritable() const override { return false; }
  const std::string& GetFilename() const override { return filename_; }
  const std::vector<std::shared_ptr<GDALDimension>>& GetDimensions()
      const override {
    return dims_;
  }
  const GDALExtendedDataType& GetDataType() const override { return dt_; }

 protected:
  bool IRead(const GUInt64*, const size_t*, const GInt64*, const GPtrDiff_t*,
             const GDALExtendedDataType&, void*) const override {
    return false;
  }

 private:
  std::string filename_ = "/vsimem/unowned.bin";
  std::vector<std::shared_ptr<GDALDimension>> dims_;
  GDALExtendedDataType dt_;
};

class CustomMockMDArray : public GDALMDArray {
 public:
  static std::shared_ptr<CustomMockMDArray> Create(
      const std::string& name,
      const std::vector<std::shared_ptr<GDALDimension>>& dims,
      const GDALExtendedDataType& dt, bool fail_read = false) {
    auto arr = std::shared_ptr<CustomMockMDArray>(
        new CustomMockMDArray(name, dims, dt, fail_read));
    arr->SetSelf(arr);
    return arr;
  }

  bool IsWritable() const override { return false; }
  const std::string& GetFilename() const override { return filename_; }
  const std::vector<std::shared_ptr<GDALDimension>>& GetDimensions()
      const override {
    return dims_;
  }
  const GDALExtendedDataType& GetDataType() const override { return dt_; }
  std::vector<GUInt64> GetBlockSize() const override {
    return std::vector<GUInt64>(dims_.size(), 1);
  }

 protected:
  CustomMockMDArray(const std::string& name,
                    const std::vector<std::shared_ptr<GDALDimension>>& dims,
                    const GDALExtendedDataType& dt, bool fail_read)
      : GDALAbstractMDArray("", name),
        GDALMDArray("", name),
        dims_(dims),
        dt_(dt),
        fail_read_(fail_read) {}

  bool IRead(const GUInt64*, const size_t*, const GInt64*, const GPtrDiff_t*,
             const GDALExtendedDataType&, void*) const override {
    return !fail_read_;
  }

 private:
  std::string filename_ = "/vsimem/mock.bin";
  std::vector<std::shared_ptr<GDALDimension>> dims_;
  GDALExtendedDataType dt_;
  bool fail_read_ = false;
};

TEST(GdalMultidimGriddedTest, ErrorValidationCases) {
  GDALAllRegister();
  CPLPushErrorHandler(CPLQuietErrorHandler);

  // 1. m_pSelf not set.
  UnownedSelfMDArray unowned;
  EXPECT_EQ(unowned.GetGridded("invdist"), nullptr);

  auto drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("/vsimem/gridded_err.mem", nullptr, nullptr));
  ASSERT_NE(ds, nullptr);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(rg, nullptr);

  auto dim_pts = rg->CreateDimension("pts", "", "", 5);
  auto dim_2 = rg->CreateDimension("dim2", "", "", 2);
  auto dim_other = rg->CreateDimension("other", "", "", 5);

  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_str = GDALExtendedDataType::CreateString();

  auto arr_main = rg->CreateMDArray("main", {dim_pts}, dt_f64, nullptr);
  auto arr_str = rg->CreateMDArray("str_arr", {dim_pts}, dt_str, nullptr);
  auto arr_0d = rg->CreateMDArray("arr_0d", {}, dt_f64, nullptr);
  auto arr_x = rg->CreateMDArray("x", {dim_pts}, dt_f64, nullptr);
  auto arr_y = rg->CreateMDArray("y", {dim_pts}, dt_f64, nullptr);
  auto arr_2d = rg->CreateMDArray("arr_2d", {dim_pts, dim_2}, dt_f64, nullptr);
  auto arr_other = rg->CreateMDArray("arr_other", {dim_other}, dt_f64, nullptr);
  auto arr_small = rg->CreateMDArray("arr_small", {dim_2}, dt_f64, nullptr);

  // 2. Invalid grid options.
  EXPECT_EQ(arr_main->GetGridded("invalid_grid_alg", arr_x, arr_y), nullptr);

  // 3. Non-numeric data type on main array.
  EXPECT_EQ(arr_str->GetGridded("invdist", arr_x, arr_y), nullptr);

  // 4. 0-D array.
  EXPECT_EQ(arr_0d->GetGridded("invdist", arr_x, arr_y), nullptr);

  // 5. Only X or only Y specified.
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_x, nullptr), nullptr);
  EXPECT_EQ(arr_main->GetGridded("invdist", nullptr, arr_y), nullptr);

  // 6. Coordinate variables auto-discovery failures:
  // < 2 coordinate variables:
  EXPECT_EQ(arr_main->GetGridded("invdist"), nullptr);
  // != GetDimensionCount() + 1 coordinate variables (3 coords for 1D array):
  auto attr_coords =
      arr_main->CreateAttribute("coordinates", {}, dt_str, nullptr);
  ASSERT_NE(attr_coords, nullptr);
  attr_coords->Write("x y main");
  EXPECT_EQ(arr_main->GetGridded("invdist"), nullptr);

  // 7. Invalid Y array (2D or non-numeric) & invalid X array (2D or
  // non-numeric).
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_x, arr_2d), nullptr);
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_x, arr_str), nullptr);
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_2d, arr_y), nullptr);
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_str, arr_y), nullptr);

  // 8. Mismatched dimensions between X and Y, or between X and main.
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_x, arr_other), nullptr);
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_other, arr_other), nullptr);

  // 9. <= 2 elements.
  EXPECT_EQ(arr_small->GetGridded("invdist", arr_small, arr_small), nullptr);

  // 10. > 10 * 1024 * 1024 elements without
  // ACCEPT_BIG_SPATIAL_INDEXING_VARIABLE.
  auto dim_big =
      std::make_shared<GDALDimension>("", "big", "", "", 10 * 1024 * 1024 + 10);
  auto mock_big = CustomMockMDArray::Create("big", {dim_big}, dt_f64);
  EXPECT_EQ(mock_big->GetGridded("invdist", mock_big, mock_big), nullptr);

  // 11. Read failure on X or Y array.
  auto mock_fail = CustomMockMDArray::Create("fail", {dim_pts}, dt_f64, true);
  EXPECT_EQ(arr_main->GetGridded("invdist", mock_fail, arr_y), nullptr);
  EXPECT_EQ(arr_main->GetGridded("invdist", arr_x, mock_fail), nullptr);

  CPLPopErrorHandler();
}

TEST(GdalMultidimGriddedTest, GriddingAndReadOperations) {
  GDALAllRegister();
  auto drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(drv, nullptr);
  std::unique_ptr<GDALDataset> ds(
      drv->CreateMultiDimensional("/vsimem/gridded_ok.mem", nullptr, nullptr));
  ASSERT_NE(ds, nullptr);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(rg, nullptr);

  auto dim_time = rg->CreateDimension("time", "", "", 2);
  auto dim_pts = rg->CreateDimension("pts", "", "", 5);
  auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto dt_str = GDALExtendedDataType::CreateString();

  auto arr_x = rg->CreateMDArray("lon", {dim_pts}, dt_f64, nullptr);
  auto arr_y = rg->CreateMDArray("lat", {dim_pts}, dt_f64, nullptr);
  auto arr_z = rg->CreateMDArray("z", {dim_time, dim_pts}, dt_f64, nullptr);

  const double x_vals[5] = {10.0, 11.0, 12.0, 10.0, 12.0};
  const double y_vals[5] = {20.0, 21.0, 22.0, 22.0, 20.0};
  const double z_vals[10] = {1.0,  2.0,  3.0,  4.0,  5.0,
                             10.0, 20.0, 30.0, 40.0, 50.0};
  const GUInt64 start1[1] = {0};
  const size_t count1[1] = {5};
  ASSERT_TRUE(arr_x->Write(start1, count1, nullptr, nullptr, dt_f64, x_vals));
  ASSERT_TRUE(arr_y->Write(start1, count1, nullptr, nullptr, dt_f64, y_vals));

  const GUInt64 start2[2] = {0, 0};
  const size_t count2[2] = {2, 5};
  ASSERT_TRUE(arr_z->Write(start2, count2, nullptr, nullptr, dt_f64, z_vals));

  arr_z->SetUnit("meters");
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  arr_z->SetSpatialRef(&srs);
  auto attr = arr_z->CreateAttribute("test_attr", {}, dt_str, nullptr);
  attr->Write("attr_val");

  // Also set "coordinates" attribute on a 1D array to test coordinate
  // auto-discovery.
  auto arr_z1d = rg->CreateMDArray("z1d", {dim_pts}, dt_f64, nullptr);
  ASSERT_TRUE(arr_z1d->Write(start1, count1, nullptr, nullptr, dt_f64, z_vals));
  auto coords_attr =
      arr_z1d->CreateAttribute("coordinates", {}, dt_str, nullptr);
  coords_attr->Write("lat lon");

  // Auto-discovered X/Y and auto-computed resolution, plus
  // radius1/radius2/nodata.
  auto gridded_1d = arr_z1d->GetGridded(
      "invdist:nodata=-999:radius1=2.0:radius2=3.0", nullptr, nullptr, nullptr);
  ASSERT_NE(gridded_1d, nullptr);
  EXPECT_FALSE(gridded_1d->IsWritable());
  EXPECT_EQ(gridded_1d->GetDataType().GetNumericDataType(), GDT_Float64);
  EXPECT_DOUBLE_EQ(gridded_1d->GetNoDataValueAsDouble(), -999.0);

  // Explicit X/Y, explicit RESOLUTION, and explicit radius.
  CPLStringList opts;
  opts.SetNameValue("RESOLUTION", "1.0");
  auto gridded = arr_z->GetGridded("nearest:nodata=-1:radius=5.0", arr_x, arr_y,
                                   opts.List());
  ASSERT_NE(gridded, nullptr);
  EXPECT_EQ(gridded->GetDimensionCount(), 3U);
  EXPECT_EQ(gridded->GetFilename(), arr_z->GetFilename());
  EXPECT_EQ(gridded->GetUnit(), "meters");
  EXPECT_NE(gridded->GetSpatialRef(), nullptr);
  EXPECT_NE(gridded->GetAttribute("test_attr"), nullptr);
  EXPECT_FALSE(gridded->GetAttributes().empty());
  auto block_size = gridded->GetBlockSize();
  ASSERT_EQ(block_size.size(), 3U);
  EXPECT_EQ(block_size[1], 256U);
  EXPECT_EQ(block_size[2], 256U);

  const auto& dims = gridded->GetDimensions();
  ASSERT_EQ(dims.size(), 3U);
  const size_t ny = static_cast<size_t>(dims[1]->GetSize());
  const size_t nx = static_cast<size_t>(dims[2]->GetSize());
  EXPECT_EQ(ny, 3U);
  EXPECT_EQ(nx, 3U);

  // Full slice read for time=0.
  std::vector<double> buf(ny * nx, 0.0);
  const GUInt64 r_start[3] = {0, 0, 0};
  const size_t r_count[3] = {1, ny, nx};
  EXPECT_TRUE(
      gridded->Read(r_start, r_count, nullptr, nullptr, dt_f64, buf.data()));

  // Sub-window read for time=0 (reuses cached m_adfZ and uses spatial filter).
  const GUInt64 sub_start[3] = {0, 0, 0};
  const size_t sub_count[3] = {1, 2, 2};
  std::vector<double> sub_buf(4, 0.0);
  EXPECT_TRUE(gridded->Read(sub_start, sub_count, nullptr, nullptr, dt_f64,
                            sub_buf.data()));

  // Read for time=1 (reloads m_adfZ).
  const GUInt64 r_start_t1[3] = {1, 0, 0};
  EXPECT_TRUE(
      gridded->Read(r_start_t1, r_count, nullptr, nullptr, dt_f64, buf.data()));

  // Error cases in IRead:
  CPLPushErrorHandler(CPLQuietErrorHandler);
  // 1. Non-numeric bufferDataType:
  char* str_buf[9] = {nullptr};
  EXPECT_FALSE(
      gridded->Read(r_start, r_count, nullptr, nullptr, dt_str, str_buf));

  // 2. count[0] != 1 on extra dimension:
  const size_t bad_count[3] = {2, ny, nx};
  std::vector<double> big_buf(2 * ny * nx, 0.0);
  EXPECT_FALSE(gridded->Read(r_start, bad_count, nullptr, nullptr, dt_f64,
                             big_buf.data()));

  // 3. Negative arrayStep on X or Y:
  const GUInt64 rev_start_x[3] = {0, 0, nx - 1};
  const GInt64 rev_step_x[3] = {1, 1, -1};
  EXPECT_FALSE(gridded->Read(rev_start_x, r_count, rev_step_x, nullptr, dt_f64,
                             buf.data()));

  const GUInt64 rev_start_y[3] = {0, ny - 1, 0};
  const GInt64 rev_step_y[3] = {1, -1, 1};
  EXPECT_FALSE(gridded->Read(rev_start_y, r_count, rev_step_y, nullptr, dt_f64,
                             buf.data()));

  // 4. Invalid RESOLUTION options in GetGridded:
  CPLStringList bad_res;
  bad_res.SetNameValue("RESOLUTION", "0");
  EXPECT_EQ(arr_z->GetGridded("invdist", arr_x, arr_y, bad_res.List()),
            nullptr);

  bad_res.SetNameValue("RESOLUTION", "1e-12");
  EXPECT_EQ(arr_z->GetGridded("invdist", arr_x, arr_y, bad_res.List()),
            nullptr);

  // 5. Parent Read failure during Gridded::IRead:
  auto fail_parent =
      CustomMockMDArray::Create("fail_parent", {dim_pts}, dt_f64, true);
  auto gridded_fail =
      fail_parent->GetGridded("invdist", arr_x, arr_y, opts.List());
  ASSERT_NE(gridded_fail, nullptr);
  const GUInt64 start_2d[2] = {0, 0};
  const size_t count_2d[2] = {ny, nx};
  EXPECT_FALSE(gridded_fail->Read(start_2d, count_2d, nullptr, nullptr, dt_f64,
                                  buf.data()));

  CPLPopErrorHandler();
}

}  // namespace
