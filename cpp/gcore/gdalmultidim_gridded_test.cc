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
// Unit tests for gcore/gdalmultidim_gridded.cpp.
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/gcore/gdalmultidim_gridded.cpp

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/ogr/ogr_spatialref.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

constexpr char kTestDir[] = "/vsimem/gdalmultidim_gridded_test";

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
  std::string filename_ = std::string(kTestDir) + "/unowned.bin";
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
  std::string filename_ = std::string(kTestDir) + "/mock.bin";
  std::vector<std::shared_ptr<GDALDimension>> dims_;
  GDALExtendedDataType dt_;
  bool fail_read_ = false;
};

TEST(GdalMultidimGriddedTest, ErrorValidationCases) {
  absl::Cleanup cleanup_dir = []() { VSIRmdirRecursive(kTestDir); };
  GDALRegister_MEM();

  GDALDriver* const drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  const std::string ds_path = std::string(kTestDir) + "/gridded_err.mem";
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(ds_path.c_str(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(nullptr, rg);

  auto dim_pts = rg->CreateDimension("pts", "", "", 5);
  auto dim_2 = rg->CreateDimension("dim2", "", "", 2);
  auto dim_other = rg->CreateDimension("other", "", "", 5);

  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_str = GDALExtendedDataType::CreateString();

  auto arr_main = rg->CreateMDArray("main", {dim_pts}, dt_f64, nullptr);
  auto arr_str = rg->CreateMDArray("str_arr", {dim_pts}, dt_str, nullptr);
  auto arr_0d = rg->CreateMDArray("arr_0d", {}, dt_f64, nullptr);
  auto arr_x = rg->CreateMDArray("x", {dim_pts}, dt_f64, nullptr);
  auto arr_y = rg->CreateMDArray("y", {dim_pts}, dt_f64, nullptr);
  auto arr_2d = rg->CreateMDArray("arr_2d", {dim_pts, dim_2}, dt_f64, nullptr);
  auto arr_other = rg->CreateMDArray("arr_other", {dim_other}, dt_f64, nullptr);
  auto arr_small = rg->CreateMDArray("arr_small", {dim_2}, dt_f64, nullptr);

  auto attr_coords =
      arr_main->CreateAttribute("coordinates", {}, dt_str, nullptr);
  ASSERT_NE(nullptr, attr_coords);
  EXPECT_TRUE(attr_coords->Write("x y main"));

  auto dim_big =
      std::make_shared<GDALDimension>("", "big", "", "", 10 * 1024 * 1024 + 10);
  auto mock_big = CustomMockMDArray::Create("big", {dim_big}, dt_f64);
  auto mock_fail = CustomMockMDArray::Create("fail", {dim_pts}, dt_f64, true);

  {
    WithQuietHandler quiet;
    CPLErrorReset();

    // 1. m_pSelf not set.
    UnownedSelfMDArray unowned;
    EXPECT_EQ(nullptr, unowned.GetGridded("invdist"));

    // 2. Invalid grid options.
    EXPECT_EQ(nullptr, arr_main->GetGridded("invalid_grid_alg", arr_x, arr_y));

    // 3. Non-numeric data type on main array.
    EXPECT_EQ(nullptr, arr_str->GetGridded("invdist", arr_x, arr_y));

    // 4. 0-D array.
    EXPECT_EQ(nullptr, arr_0d->GetGridded("invdist", arr_x, arr_y));

    // 5. Only X or only Y specified.
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_x, nullptr));
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", nullptr, arr_y));

    // 6. Coordinate variables auto-discovery failures:
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist"));
    EXPECT_EQ(nullptr, arr_x->GetGridded("invdist"));

    // 7. Invalid Y or X array (2D or non-numeric).
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_x, arr_2d));
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_x, arr_str));
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_2d, arr_y));
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_str, arr_y));

    // 8. Mismatched dimensions between X and Y, or between X and main.
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_x, arr_other));
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_other, arr_other));

    // 9. <= 2 elements.
    EXPECT_EQ(nullptr, arr_small->GetGridded("invdist", arr_small, arr_small));

    // 10. > 10 * 1024 * 1024 elements without
    // ACCEPT_BIG_SPATIAL_INDEXING_VARIABLE.
    EXPECT_EQ(nullptr, mock_big->GetGridded("invdist", mock_big, mock_big));

    // 11. Read failure on X or Y array.
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", mock_fail, arr_y));
    EXPECT_EQ(nullptr, arr_main->GetGridded("invdist", arr_x, mock_fail));

    CPLErrorReset();
  }
}

TEST(GdalMultidimGriddedTest, GriddingAndReadOperations) {
  absl::Cleanup cleanup_dir = []() { VSIRmdirRecursive(kTestDir); };
  GDALRegister_MEM();

  GDALDriver* const drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, drv);
  const std::string ds_path = std::string(kTestDir) + "/gridded_ok.mem";
  GDALDatasetUniquePtr ds(
      drv->CreateMultiDimensional(ds_path.c_str(), nullptr, nullptr));
  ASSERT_NE(nullptr, ds);
  auto rg = ds->GetRootGroup();
  ASSERT_NE(nullptr, rg);

  auto dim_time = rg->CreateDimension("time", "", "", 2);
  auto dim_pts = rg->CreateDimension("pts", "", "", 5);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  const auto dt_str = GDALExtendedDataType::CreateString();

  auto arr_x = rg->CreateMDArray("lon", {dim_pts}, dt_f64, nullptr);
  auto arr_y = rg->CreateMDArray("lat", {dim_pts}, dt_f64, nullptr);
  auto arr_z = rg->CreateMDArray("z", {dim_time, dim_pts}, dt_f64, nullptr);

  constexpr auto x_vals = std::to_array<double>({10.0, 11.0, 12.0, 10.0, 12.0});
  constexpr auto y_vals = std::to_array<double>({20.0, 21.0, 22.0, 22.0, 20.0});
  constexpr auto z_vals = std::to_array<double>(
      {1.0, 2.0, 3.0, 4.0, 5.0, 10.0, 20.0, 30.0, 40.0, 50.0});
  constexpr auto start1 = std::to_array<GUInt64>({0});
  constexpr auto count1 = std::to_array<size_t>({5});
  ASSERT_TRUE(arr_x->Write(start1.data(), count1.data(), nullptr, nullptr,
                           dt_f64, x_vals.data()));
  ASSERT_TRUE(arr_y->Write(start1.data(), count1.data(), nullptr, nullptr,
                           dt_f64, y_vals.data()));

  constexpr auto start2 = std::to_array<GUInt64>({0, 0});
  constexpr auto count2 = std::to_array<size_t>({2, 5});
  ASSERT_TRUE(arr_z->Write(start2.data(), count2.data(), nullptr, nullptr,
                           dt_f64, z_vals.data()));

  arr_z->SetUnit("meters");
  OGRSpatialReference srs;
  srs.SetWellKnownGeogCS("WGS84");
  arr_z->SetSpatialRef(&srs);
  auto attr = arr_z->CreateAttribute("test_attr", {}, dt_str, nullptr);
  ASSERT_NE(nullptr, attr);
  EXPECT_TRUE(attr->Write("attr_val"));

  // Set "coordinates" attribute on a 1D array to test coordinate
  // auto-discovery.
  auto arr_z1d = rg->CreateMDArray("z1d", {dim_pts}, dt_f64, nullptr);
  ASSERT_TRUE(arr_z1d->Write(start1.data(), count1.data(), nullptr, nullptr,
                             dt_f64, z_vals.data()));
  auto coords_attr =
      arr_z1d->CreateAttribute("coordinates", {}, dt_str, nullptr);
  ASSERT_NE(nullptr, coords_attr);
  EXPECT_TRUE(coords_attr->Write("lat lon"));

  // Auto-discovered X/Y and auto-computed resolution, plus
  // radius1/radius2/nodata.
  auto gridded_1d = arr_z1d->GetGridded(
      "invdist:nodata=-999:radius1=2.0:radius2=3.0", nullptr, nullptr, nullptr);
  ASSERT_NE(nullptr, gridded_1d);
  EXPECT_FALSE(gridded_1d->IsWritable());
  EXPECT_EQ(GDT_Float64, gridded_1d->GetDataType().GetNumericDataType());
  EXPECT_DOUBLE_EQ(-999.0, gridded_1d->GetNoDataValueAsDouble());

  // Explicit X/Y, explicit RESOLUTION, and explicit radius.
  CPLStringList opts;
  opts.SetNameValue("RESOLUTION", "1.0");
  auto gridded = arr_z->GetGridded("nearest:nodata=-1:radius=5.0", arr_x, arr_y,
                                   opts.List());
  ASSERT_NE(nullptr, gridded);
  EXPECT_EQ(3U, gridded->GetDimensionCount());
  EXPECT_EQ(arr_z->GetFilename(), gridded->GetFilename());
  EXPECT_EQ("meters", gridded->GetUnit());
  EXPECT_NE(nullptr, gridded->GetSpatialRef());
  EXPECT_NE(nullptr, gridded->GetAttribute("test_attr"));
  EXPECT_FALSE(gridded->GetAttributes().empty());
  const auto block_size = gridded->GetBlockSize();
  ASSERT_EQ(3U, block_size.size());
  EXPECT_EQ(256U, block_size[1]);
  EXPECT_EQ(256U, block_size[2]);

  const auto& dims = gridded->GetDimensions();
  ASSERT_EQ(3U, dims.size());
  const size_t ny = static_cast<size_t>(dims[1]->GetSize());
  const size_t nx = static_cast<size_t>(dims[2]->GetSize());
  EXPECT_EQ(3U, ny);
  EXPECT_EQ(3U, nx);

  // Full slice read for time=0.
  std::vector<double> buf(ny * nx, 0.0);
  constexpr auto r_start = std::to_array<GUInt64>({0, 0, 0});
  const std::array<size_t, 3> r_count = {1, ny, nx};
  EXPECT_TRUE(gridded->Read(r_start.data(), r_count.data(), nullptr, nullptr,
                            dt_f64, buf.data()));

  // Sub-window read for time=0 (reuses cached m_adfZ and uses spatial filter).
  constexpr auto sub_start = std::to_array<GUInt64>({0, 0, 0});
  constexpr auto sub_count = std::to_array<size_t>({1, 2, 2});
  std::vector<double> sub_buf(4, 0.0);
  EXPECT_TRUE(gridded->Read(sub_start.data(), sub_count.data(), nullptr,
                            nullptr, dt_f64, sub_buf.data()));

  // Read for time=1 (reloads m_adfZ).
  constexpr auto r_start_t1 = std::to_array<GUInt64>({1, 0, 0});
  EXPECT_TRUE(gridded->Read(r_start_t1.data(), r_count.data(), nullptr, nullptr,
                            dt_f64, buf.data()));

  // Parent Read failure during Gridded::IRead.
  auto fail_parent =
      CustomMockMDArray::Create("fail_parent", {dim_pts}, dt_f64, true);
  auto gridded_fail =
      fail_parent->GetGridded("invdist", arr_x, arr_y, opts.List());
  ASSERT_NE(nullptr, gridded_fail);

  // Error cases in IRead and GetGridded options:
  {
    WithQuietHandler quiet;
    CPLErrorReset();

    // 1. Non-numeric bufferDataType:
    std::array<char*, 9> str_buf = {};
    EXPECT_FALSE(gridded->Read(r_start.data(), r_count.data(), nullptr, nullptr,
                               dt_str, str_buf.data()));

    // 2. count[0] != 1 on extra dimension:
    const std::array<size_t, 3> bad_count = {2, ny, nx};
    std::vector<double> big_buf(2 * ny * nx, 0.0);
    EXPECT_FALSE(gridded->Read(r_start.data(), bad_count.data(), nullptr,
                               nullptr, dt_f64, big_buf.data()));

    // 3. Negative arrayStep on X or Y:
    const std::array<GUInt64, 3> rev_start_x = {0, 0, nx - 1};
    constexpr auto rev_step_x = std::to_array<GInt64>({1, 1, -1});
    EXPECT_FALSE(gridded->Read(rev_start_x.data(), r_count.data(),
                               rev_step_x.data(), nullptr, dt_f64, buf.data()));

    const std::array<GUInt64, 3> rev_start_y = {0, ny - 1, 0};
    constexpr auto rev_step_y = std::to_array<GInt64>({1, -1, 1});
    EXPECT_FALSE(gridded->Read(rev_start_y.data(), r_count.data(),
                               rev_step_y.data(), nullptr, dt_f64, buf.data()));

    // 4. Invalid RESOLUTION options in GetGridded:
    CPLStringList bad_res;
    bad_res.SetNameValue("RESOLUTION", "0");
    EXPECT_EQ(nullptr,
              arr_z->GetGridded("invdist", arr_x, arr_y, bad_res.List()));

    bad_res.SetNameValue("RESOLUTION", "1e-12");
    EXPECT_EQ(nullptr,
              arr_z->GetGridded("invdist", arr_x, arr_y, bad_res.List()));

    // 5. Parent Read failure during Gridded::IRead:
    constexpr auto start_2d = std::to_array<GUInt64>({0, 0});
    const std::array<size_t, 2> count_2d = {ny, nx};
    EXPECT_FALSE(gridded_fail->Read(start_2d.data(), count_2d.data(), nullptr,
                                    nullptr, dt_f64, buf.data()));

    CPLErrorReset();
  }
}

}  // namespace
}  // namespace autotest2
