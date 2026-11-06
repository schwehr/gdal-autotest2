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
// Tests for GDALMultiDimTranslate (apps/gdalmdimtranslate_lib.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/apps/gdalmdimtranslate_lib.cpp
//   https://gdal.org/programs/gdalmdimtranslate.html

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/gdal/apps/gdal_utils.h"
#include "third_party/gdal/apps/gdal_utils_priv.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/autotest2/cpp/util/vsimem.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_dataset.h"
#include "third_party/gdal/gcore/gdal_driver.h"
#include "third_party/gdal/gcore/gdal_drivermanager.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_port.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

struct MultiDimTranslateOptionsDeleter {
  void operator()(GDALMultiDimTranslateOptions* opts) const {
    if (opts != nullptr) {
      GDALMultiDimTranslateOptionsFree(opts);
    }
  }
};
using MultiDimTranslateOptionsUniquePtr =
    std::unique_ptr<GDALMultiDimTranslateOptions,
                    MultiDimTranslateOptionsDeleter>;

class GdalMultiDimTranslateTest : public ::testing::Test {
 protected:
  void SetUp() override {
    GDALRegister_MEM();
    GDALRegister_VRT();
    GDALRegister_GTiff();
  }
};

// Test parser usage string retrieval.
TEST_F(GdalMultiDimTranslateTest, ParserUsage) {
  const std::string usage = GDALMultiDimTranslateAppGetParserUsage();
  EXPECT_FALSE(usage.empty());
  EXPECT_THAT(usage, HasSubstr("Usage: gdalmdimtranslate"));
  EXPECT_THAT(usage, HasSubstr("-of"));
  EXPECT_THAT(usage, HasSubstr("-array"));
  EXPECT_THAT(usage, HasSubstr("-group"));
  EXPECT_THAT(usage, HasSubstr("-subset"));
  EXPECT_THAT(usage, HasSubstr("-scaleaxes"));
}

// Test options creation, progress setting, and argument parsing.
TEST_F(GdalMultiDimTranslateTest, OptionsManagement) {
  // 1. Default options.
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(nullptr, nullptr));
  ASSERT_NE(nullptr, options);

  // Set progress callback.
  bool progress_called = false;
  auto progress_cb = [](double dfComplete, const char* pszMessage,
                        void* pProgressArg) -> int {
    *static_cast<bool*>(pProgressArg) = true;
    return 1;
  };
  GDALMultiDimTranslateOptionsSetProgress(options.get(), progress_cb,
                                          &progress_called);

  // 2. Comprehensive valid flags with -array.
  std::vector<std::string> args = {
      "-of",         "VRT",
      "-co",         "KEY=VALUE",
      "-array",      "name=temperature,dstname=temp,transpose=[1,0],ot=Float64",
      "-subset",     "time(10,20)",
      "-scaleaxes",  "x(2),y(0.5)",
      "-strict",     "-arrayoption",
      "OPT=1",       "-co",
      "ANOTHER=VAL", "--overwrite",
  };
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  options.reset(GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  // 2b. Valid flags with -group.
  std::vector<std::string> grp_args = {
      "-of",
      "VRT",
      "-group",
      "name=sub1,dstname=new_sub,recursive=no",
      "--no-overwrite",
  };
  std::vector<char*> grp_argv = {grp_args[0].data(), grp_args[1].data(),
                                 grp_args[2].data(), grp_args[3].data(),
                                 grp_args[4].data(), nullptr};
  options.reset(GDALMultiDimTranslateOptionsNew(grp_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  // 3. Options for binary.
  GDALMultiDimTranslateOptionsForBinary options_for_binary;
  std::vector<std::string> bin_args = {
      "-if", "MEM", "-oo", "KEY=VAL", "src.nc", "dst.vrt",
  };
  std::vector<char*> bin_argv;
  bin_argv.reserve(bin_args.size() + 1);
  for (auto& arg : bin_args) {
    bin_argv.push_back(arg.data());
  }
  bin_argv.push_back(nullptr);

  options.reset(
      GDALMultiDimTranslateOptionsNew(bin_argv.data(), &options_for_binary));
  ASSERT_NE(nullptr, options);
  EXPECT_EQ("src.nc", options_for_binary.osSource);
  EXPECT_EQ("dst.vrt", options_for_binary.osDest);
  EXPECT_EQ(1, options_for_binary.aosAllowInputDrivers.size());
  EXPECT_EQ(1, options_for_binary.aosOpenOptions.size());

  // 4. Invalid arguments return nullptr.
  std::vector<std::string> invalid_args = {"-unsupported_option"};
  std::vector<char*> invalid_argv = {invalid_args[0].data(), nullptr};
  WithQuietHandler quiet;
  CPLErrorReset();
  options.reset(GDALMultiDimTranslateOptionsNew(invalid_argv.data(), nullptr));
  EXPECT_EQ(nullptr, options);
  EXPECT_NE(CE_None, CPLGetLastErrorType());
}

// Test validation of inputs to GDALMultiDimTranslate.
TEST_F(GdalMultiDimTranslateTest, InvalidInputs) {
  constexpr char kDest[] = "/vsimem/mdim_translate/invalid.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  int bUsageError = false;

  // 1. nSrcCount != 1.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 0, nullptr,
                                             nullptr, &bUsageError));
    EXPECT_TRUE(bUsageError);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Only one source dataset is supported"));
  }

  // 2. pahSrcDS[0] is nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetH null_src[1] = {nullptr};
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, null_src,
                                             nullptr, &bUsageError));
    EXPECT_TRUE(bUsageError);
  }

  // 3. Existing destination dataset update (not supported yet).
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    GDALDatasetUniquePtr dst_ds(
        mem_drv->CreateMultiDimensional("dst", nullptr, nullptr));
    GDALDatasetH hDst = GDALDataset::ToHandle(dst_ds.release());
    EXPECT_EQ(nullptr,
              GDALMultiDimTranslate(nullptr, hDst, 1, &hSrc, nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Update of existing file not supported yet"));
  }

  // 4. Both pszDest and hDstDS are nullptr.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(nullptr, nullptr, 1, &hSrc,
                                             nullptr, &bUsageError));
    EXPECT_TRUE(bUsageError);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Both pszDest and hDstDS are NULL"));
  }

  // 5. Output driver not recognized.
  {
    std::vector<std::string> args = {"-of", "NONEXISTENT_DRIVER"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);

    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             options.get(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Output driver `NONEXISTENT_DRIVER' not recognised"));
  }

  // 6. Source dataset has no arrays for non-multidimensional output driver.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALMultiDimTranslate("/vsimem/mdim_translate/no_array.tif",
                                    nullptr, 1, &hSrc, nullptr, nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find source array"));
  }
}

// Test basic translation of a multidimensional dataset to VRT and MEM.
TEST_F(GdalMultiDimTranslateTest, BasicTranslateToVRT) {
  constexpr char kSrcVrt[] = "/vsimem/mdim_translate/src.vrt";
  constexpr char kDest[] = "/vsimem/mdim_translate/basic.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"y\" size=\"2\"/>\n"
      "    <Dimension name=\"x\" size=\"3\"/>\n"
      "    <Array name=\"data\">\n"
      "      <DataType>Float32</DataType>\n"
      "      <DimensionRef ref=\"/y\"/>\n"
      "      <DimensionRef ref=\"/x\"/>\n"
      "      <InlineValues>1 2 3 4 5 6</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";
  VsiMemTempWrapper src_vrt_file(kSrcVrt, vrt_xml);

  GDALDatasetUniquePtr src_ds(
      GDALDataset::Open(kSrcVrt, GDAL_OF_MULTIDIM_RASTER));
  if (!src_ds) {
    FAIL() << "Failed to open vrt: " << CPLGetLastErrorMsg();
  }
  ASSERT_NE(nullptr, src_ds);

  std::vector<std::string> args = {"-of", "VRT"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  // Verify resulting VRT dataset.
  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_root = out_ds->GetRootGroup();
  ASSERT_NE(nullptr, out_root);

  auto out_arr = out_root->OpenMDArray("data");
  ASSERT_NE(nullptr, out_arr);
  EXPECT_EQ(2, out_arr->GetDimensionCount());
  const auto dt_f32 = GDALExtendedDataType::Create(GDT_Float32);
  const std::vector<GUInt64> start = {0, 0};
  const std::vector<size_t> count = {2, 3};
  std::vector<float> read_vals(6, 0.0f);
  EXPECT_TRUE(out_arr->Read(start.data(), count.data(), nullptr, nullptr,
                            dt_f32, read_vals.data()));
  EXPECT_EQ(1.0f, read_vals[0]);
  EXPECT_EQ(6.0f, read_vals[5]);

  // Also test translate to MEM driver directly.
  {
    std::vector<std::string> mem_args = {"-of", "MEM"};
    std::vector<char*> mem_argv = {mem_args[0].data(), mem_args[1].data(),
                                   nullptr};
    MultiDimTranslateOptionsUniquePtr mem_opt(
        GDALMultiDimTranslateOptionsNew(mem_argv.data(), nullptr));
    ASSERT_NE(nullptr, mem_opt);
    GDALDatasetUniquePtr mem_dst(GDALDataset::FromHandle(
        GDALMultiDimTranslate("", nullptr, 1, &hSrc, mem_opt.get(), nullptr)));
    ASSERT_NE(nullptr, mem_dst);
    auto mem_arr = mem_dst->GetRootGroup()->OpenMDArray("data");
    ASSERT_NE(nullptr, mem_arr);
    EXPECT_EQ(2, mem_arr->GetDimensionCount());
  }
}

// Test array selection, renaming, and error cases in -array.
TEST_F(GdalMultiDimTranslateTest, ArraySelectionAndRename) {
  constexpr char kDest[] = "/vsimem/mdim_translate/array_sel.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim = src_root->CreateDimension("d", "", "", 2);
  const auto dt = GDALExtendedDataType::Create(GDT_Int32);
  auto arr1 = src_root->CreateMDArray("arr1", {dim}, dt);
  ASSERT_NE(nullptr, arr1);
  auto arr2 = src_root->CreateMDArray("arr2", {dim}, dt);
  ASSERT_NE(nullptr, arr2);

  // 1. Select arr1 and rename to renamed_arr.
  std::vector<std::string> args = {
      "-of",
      "VRT",
      "-array",
      "name=arr1,dstname=renamed_arr",
  };
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_root = out_ds->GetRootGroup();
  ASSERT_NE(nullptr, out_root);
  EXPECT_NE(nullptr, out_root->OpenMDArray("renamed_arr"));
  EXPECT_EQ(nullptr, out_root->OpenMDArray("arr1"));
  EXPECT_EQ(nullptr, out_root->OpenMDArray("arr2"));

  // 2. Select array using simple name syntax (no "name=" prefix).
  std::vector<std::string> simple_args = {"-of", "VRT", "-array", "arr2"};
  std::vector<char*> simple_argv = {
      simple_args[0].data(), simple_args[1].data(), simple_args[2].data(),
      simple_args[3].data(), nullptr};
  options.reset(GDALMultiDimTranslateOptionsNew(simple_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  constexpr char kDest2[] = "/vsimem/mdim_translate/array_sel2.vrt";
  hDst_ds.reset(GDALDataset::FromHandle(GDALMultiDimTranslate(
      kDest2, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds2(
      GDALDataset::Open(kDest2, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds2);
  EXPECT_NE(nullptr, out_ds2->GetRootGroup()->OpenMDArray("arr2"));

  // 3. Error case: array does not exist.
  std::vector<std::string> bad_args = {"-of", "VRT", "-array",
                                       "name=nonexistent"};
  std::vector<char*> bad_argv = {bad_args[0].data(), bad_args[1].data(),
                                 bad_args[2].data(), bad_args[3].data(),
                                 nullptr};
  options.reset(GDALMultiDimTranslateOptionsNew(bad_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             options.get(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Cannot find array"));
  }
}

// Test array transpose with valid and invalid permutations.
TEST_F(GdalMultiDimTranslateTest, ArrayTranspose) {
  constexpr char kDest[] = "/vsimem/mdim_translate/transpose.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_y = src_root->CreateDimension("y", "", "", 2);
  auto dim_x = src_root->CreateDimension("x", "", "", 3);
  const auto dt = GDALExtendedDataType::Create(GDT_Int32);
  auto arr = src_root->CreateMDArray("matrix", {dim_y, dim_x}, dt);
  ASSERT_NE(nullptr, arr);
  const std::vector<int> vals = {10, 20, 30, 40, 50, 60};
  const std::vector<GUInt64> start = {0, 0};
  const std::vector<size_t> count = {2, 3};
  EXPECT_TRUE(arr->Write(start.data(), count.data(), nullptr, nullptr, dt,
                         vals.data()));

  // 1. Transpose [1, 0].
  std::vector<std::string> args = {
      "-of",
      "VRT",
      "-array",
      "name=matrix,transpose=[1,0]",
  };
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_arr = out_ds->GetRootGroup()->OpenMDArray("matrix");
  ASSERT_NE(nullptr, out_arr);
  const auto& out_dims = out_arr->GetDimensions();
  ASSERT_EQ(2, out_dims.size());
  EXPECT_EQ(3, out_dims[0]->GetSize());
  EXPECT_EQ(2, out_dims[1]->GetSize());

  // 2. Invalid transpose syntax errors.
  const std::vector<std::string> invalid_transposes = {
      "name=matrix,transpose=invalid",
      "name=matrix,transpose=[bad]",
      "name=matrix,transpose=[0,2]",
  };
  for (const auto& inv_trans : invalid_transposes) {
    std::vector<std::string> inv_args = {"-of", "VRT", "-array", inv_trans};
    std::vector<char*> inv_argv = {inv_args[0].data(), inv_args[1].data(),
                                   inv_args[2].data(), inv_args[3].data(),
                                   nullptr};
    WithQuietHandler quiet;
    CPLErrorReset();
    MultiDimTranslateOptionsUniquePtr inv_opt(
        GDALMultiDimTranslateOptionsNew(inv_argv.data(), nullptr));
    if (inv_opt) {
      EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                               inv_opt.get(), nullptr));
    }
  }
}

// Test array view/slicing via view option in -array.
TEST_F(GdalMultiDimTranslateTest, ArrayViewAndSlicing) {
  constexpr char kDest[] = "/vsimem/mdim_translate/view.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_y = src_root->CreateDimension("y", "", "", 4);
  auto dim_x = src_root->CreateDimension("x", "", "", 4);
  const auto dt = GDALExtendedDataType::Create(GDT_Int32);
  auto arr = src_root->CreateMDArray("grid", {dim_y, dim_x}, dt);
  ASSERT_NE(nullptr, arr);

  // View: slice [1:3, 1:3] (2x2 subgrid).
  std::vector<std::string> args = {
      "-of",
      "VRT",
      "-array",
      "name=grid,view=[1:3,1:3]",
  };
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_arr = out_ds->GetRootGroup()->OpenMDArray("grid");
  ASSERT_NE(nullptr, out_arr);
  const auto& dims = out_arr->GetDimensions();
  ASSERT_EQ(2, dims.size());
  EXPECT_EQ(2, dims[0]->GetSize());
  EXPECT_EQ(2, dims[1]->GetSize());
}

// Test output data type conversion via ot= and resample option in -array.
TEST_F(GdalMultiDimTranslateTest, ArrayTypeConversionAndResample) {
  constexpr char kDest[] = "/vsimem/mdim_translate/type_conv.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_y = src_root->CreateDimension("y", "", "", 2);
  auto dim_x = src_root->CreateDimension("x", "", "", 3);
  auto arr = src_root->CreateMDArray("data", {dim_y, dim_x},
                                     GDALExtendedDataType::Create(GDT_Float32));
  ASSERT_NE(nullptr, arr);

  // 1. Convert to Float64.
  std::vector<std::string> args = {
      "-of",
      "VRT",
      "-array",
      "name=data,ot=Float64",
  };
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_arr = out_ds->GetRootGroup()->OpenMDArray("data");
  ASSERT_NE(nullptr, out_arr);
  EXPECT_EQ(GDT_Float64, out_arr->GetDataType().GetNumericDataType());

  // 1b. resample=true fails without georeferencing.
  {
    std::vector<std::string> resamp_args = {
        "-of",
        "VRT",
        "-array",
        "name=data,resample=true",
    };
    std::vector<char*> resamp_argv = {
        resamp_args[0].data(), resamp_args[1].data(), resamp_args[2].data(),
        resamp_args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr resamp_opt(
        GDALMultiDimTranslateOptionsNew(resamp_argv.data(), nullptr));
    ASSERT_NE(nullptr, resamp_opt);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             resamp_opt.get(), nullptr));
  }

  // 2. Convert to String type.
  std::vector<std::string> str_args = {
      "-of",
      "VRT",
      "-array",
      "name=data,ot=String",
  };
  std::vector<char*> str_argv = {str_args[0].data(), str_args[1].data(),
                                 str_args[2].data(), str_args[3].data(),
                                 nullptr};
  options.reset(GDALMultiDimTranslateOptionsNew(str_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  constexpr char kDestStr[] = "/vsimem/mdim_translate/type_conv_str.vrt";
  hDst_ds.reset(GDALDataset::FromHandle(GDALMultiDimTranslate(
      kDestStr, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_str_ds(
      GDALDataset::Open(kDestStr, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_str_ds);
  EXPECT_EQ(GEDTC_STRING, out_str_ds->GetRootGroup()
                              ->OpenMDArray("data")
                              ->GetDataType()
                              .GetClass());
}

// Test group copying, group renaming, and recursive vs non-recursive copying.
TEST_F(GdalMultiDimTranslateTest, GroupSelectionAndRecursion) {
  constexpr char kDest[] = "/vsimem/mdim_translate/groups.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto sub1 = src_root->CreateGroup("sub1");
  ASSERT_NE(nullptr, sub1);
  auto sub2 = sub1->CreateGroup("sub2");
  ASSERT_NE(nullptr, sub2);
  auto dim = sub1->CreateDimension("d", "", "", 2);
  auto arr1 = sub1->CreateMDArray("arr1", {dim},
                                  GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, arr1);
  auto other_grp = src_root->CreateGroup("other_grp");
  ASSERT_NE(nullptr, other_grp);

  // 1. Copy single group sub1 with recursive=no (copied directly into root).
  std::vector<std::string> args = {
      "-of",
      "VRT",
      "-group",
      "name=sub1,recursive=no",
  };
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_root = out_ds->GetRootGroup();
  ASSERT_NE(nullptr, out_root);
  EXPECT_NE(nullptr, out_root->OpenMDArray("arr1"));
  EXPECT_EQ(nullptr,
            out_root->OpenGroup("sub2"));  // Not copied when recursive=no.

  // 2. Multiple groups: creates distinct subgroups in destination root group.
  std::vector<std::string> multi_grp_args = {
      "-of", "VRT", "-group", "sub1", "-group", "other_grp",
  };
  std::vector<char*> multi_grp_argv = {multi_grp_args[0].data(),
                                       multi_grp_args[1].data(),
                                       multi_grp_args[2].data(),
                                       multi_grp_args[3].data(),
                                       multi_grp_args[4].data(),
                                       multi_grp_args[5].data(),
                                       nullptr};
  options.reset(
      GDALMultiDimTranslateOptionsNew(multi_grp_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  constexpr char kDestMulti[] = "/vsimem/mdim_translate/multi_group.vrt";
  hDst_ds.reset(GDALDataset::FromHandle(GDALMultiDimTranslate(
      kDestMulti, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds_multi(
      GDALDataset::Open(kDestMulti, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds_multi);
  EXPECT_NE(nullptr, out_ds_multi->GetRootGroup()->OpenGroup("sub1"));
  EXPECT_NE(nullptr, out_ds_multi->GetRootGroup()->OpenGroup("other_grp"));

  // 3. Error case: invalid group spec part.
  std::vector<std::string> bad_grp_args = {"-of", "VRT", "-group",
                                           "name=sub1,invalid_flag=true"};
  std::vector<char*> bad_grp_argv = {
      bad_grp_args[0].data(), bad_grp_args[1].data(), bad_grp_args[2].data(),
      bad_grp_args[3].data(), nullptr};
  WithQuietHandler quiet;
  CPLErrorReset();
  options.reset(GDALMultiDimTranslateOptionsNew(bad_grp_argv.data(), nullptr));
  if (options) {
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             options.get(), nullptr));
  }
}

// Test dimension subsetting with numeric indexing variables (range and slice).
TEST_F(GdalMultiDimTranslateTest, DimensionSubsettingNumeric) {
  constexpr char kDest[] = "/vsimem/mdim_translate/subset_num.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_time = src_root->CreateDimension("time", "", "", 5);
  auto dim_x = src_root->CreateDimension("x", "", "", 2);
  const auto dt_f64 = GDALExtendedDataType::Create(GDT_Float64);
  auto arr_time = src_root->CreateMDArray("time", {dim_time}, dt_f64);
  ASSERT_NE(nullptr, arr_time);
  const std::vector<double> time_vals = {10.0, 20.0, 30.0, 40.0, 50.0};
  const std::vector<GUInt64> start1 = {0};
  const std::vector<size_t> count1 = {5};
  EXPECT_TRUE(arr_time->Write(start1.data(), count1.data(), nullptr, nullptr,
                              dt_f64, time_vals.data()));
  EXPECT_TRUE(dim_time->SetIndexingVariable(arr_time));

  auto arr_var = src_root->CreateMDArray("var", {dim_time, dim_x}, dt_f64);
  ASSERT_NE(nullptr, arr_var);

  // 1. Range subsetting: time(20, 40) -> 3 elements (indices 1, 2, 3).
  std::vector<std::string> args = {
      "-of",
      "VRT",
      "-subset",
      "time(20,40)",
  };
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_arr = out_ds->GetRootGroup()->OpenMDArray("var");
  ASSERT_NE(nullptr, out_arr);
  EXPECT_EQ(3, out_arr->GetDimensions()[0]->GetSize());

  // 2. Single-value slice subsetting: time(30) -> dimensionality reduced by 1.
  std::vector<std::string> slice_args = {
      "-of",
      "VRT",
      "-subset",
      "time(30)",
  };
  std::vector<char*> slice_argv = {slice_args[0].data(), slice_args[1].data(),
                                   slice_args[2].data(), slice_args[3].data(),
                                   nullptr};
  options.reset(GDALMultiDimTranslateOptionsNew(slice_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  constexpr char kDestSlice[] = "/vsimem/mdim_translate/subset_slice.vrt";
  hDst_ds.reset(GDALDataset::FromHandle(GDALMultiDimTranslate(
      kDestSlice, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_slice_ds(
      GDALDataset::Open(kDestSlice, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_slice_ds);
  auto out_slice_arr = out_slice_ds->GetRootGroup()->OpenMDArray("var");
  ASSERT_NE(nullptr, out_slice_arr);
  // Sliced out time dimension: only x dimension remains.
  EXPECT_EQ(1, out_slice_arr->GetDimensionCount());
  EXPECT_EQ("x", out_slice_arr->GetDimensions()[0]->GetName());

  // 3. Error cases for subset.
  const std::vector<std::string> invalid_subsets = {
      "time(20",         // Missing closing paren
      "time(10,20,30)",  // Invalid number of tokens
      "time(100,200)",   // Out of bounds empty range
      "x(10,20)",        // Dimension without indexing variable
  };
  for (const auto& inv_sub : invalid_subsets) {
    std::vector<std::string> inv_args = {"-of", "VRT", "-subset", inv_sub};
    std::vector<char*> inv_argv = {inv_args[0].data(), inv_args[1].data(),
                                   inv_args[2].data(), inv_args[3].data(),
                                   nullptr};
    WithQuietHandler quiet;
    CPLErrorReset();
    MultiDimTranslateOptionsUniquePtr inv_opt(
        GDALMultiDimTranslateOptionsNew(inv_argv.data(), nullptr));
    if (inv_opt) {
      EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                               inv_opt.get(), nullptr));
    }
  }
}

// Test dimension subsetting with string indexing variables.
TEST_F(GdalMultiDimTranslateTest, DimensionSubsettingString) {
  constexpr char kDest[] = "/vsimem/mdim_translate/subset_str.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_cat = src_root->CreateDimension("cat", "", "", 4);
  const auto dt_str = GDALExtendedDataType::CreateString();
  auto arr_cat = src_root->CreateMDArray("cat", {dim_cat}, dt_str);
  ASSERT_NE(nullptr, arr_cat);
  const char* cat_vals[4] = {"alpha", "beta", "delta", "gamma"};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {4};
  EXPECT_TRUE(arr_cat->Write(start.data(), count.data(), nullptr, nullptr,
                             dt_str, cat_vals));
  EXPECT_TRUE(dim_cat->SetIndexingVariable(arr_cat));

  auto arr_val = src_root->CreateMDArray(
      "val", {dim_cat}, GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, arr_val);

  // Range subset on string dimension.
  std::vector<std::string> args = {
      "-of",
      "VRT",
      "-subset",
      "cat(beta,delta)",
  };
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_arr = out_ds->GetRootGroup()->OpenMDArray("val");
  ASSERT_NE(nullptr, out_arr);
  EXPECT_EQ(2, out_arr->GetDimensions()[0]->GetSize());
}

// Test axis scaling via -scaleaxes option.
TEST_F(GdalMultiDimTranslateTest, ScaleAxes) {
  constexpr char kDest[] = "/vsimem/mdim_translate/scaleaxes.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_x = src_root->CreateDimension("x", "", "", 10);
  auto arr = src_root->CreateMDArray("var", {dim_x},
                                     GDALExtendedDataType::Create(GDT_Float32));
  ASSERT_NE(nullptr, arr);

  // Scale dimension x by 2 (downsampling size to 5).
  std::vector<std::string> args = {"-of", "VRT", "-scaleaxes", "x(2)"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_arr = out_ds->GetRootGroup()->OpenMDArray("var");
  ASSERT_NE(nullptr, out_arr);
  EXPECT_EQ(5, out_arr->GetDimensions()[0]->GetSize());

  // Error case: invalid scale factor.
  std::vector<std::string> bad_scale_args = {"-of", "VRT", "-scaleaxes",
                                             "x(bad)"};
  std::vector<char*> bad_scale_argv = {
      bad_scale_args[0].data(), bad_scale_args[1].data(),
      bad_scale_args[2].data(), bad_scale_args[3].data(), nullptr};
  WithQuietHandler quiet;
  CPLErrorReset();
  MultiDimTranslateOptionsUniquePtr bad_opt(
      GDALMultiDimTranslateOptionsNew(bad_scale_argv.data(), nullptr));
  if (bad_opt) {
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             bad_opt.get(), nullptr));
  }
}

// Test output to non-multidimensional raster driver (GTiff).
TEST_F(GdalMultiDimTranslateTest, CopyToNonMultiDimensionalDriver) {
  constexpr char kDest[] = "/vsimem/mdim_translate/out.tif";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_y = src_root->CreateDimension("y", "", "", 2);
  auto dim_x = src_root->CreateDimension("x", "", "", 3);
  const auto dt = GDALExtendedDataType::Create(GDT_Byte);
  auto arr = src_root->CreateMDArray("raster", {dim_y, dim_x}, dt);
  ASSERT_NE(nullptr, arr);
  const std::vector<GByte> vals = {1, 2, 3, 4, 5, 6};
  const std::vector<GUInt64> start = {0, 0};
  const std::vector<size_t> count = {2, 3};
  EXPECT_TRUE(arr->Write(start.data(), count.data(), nullptr, nullptr, dt,
                         vals.data()));

  // 1. Successful translation to GTiff.
  std::vector<std::string> args = {"-of", "GTiff"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr tif_ds(GDALDataset::Open(kDest, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, tif_ds);
  EXPECT_EQ(3, tif_ds->GetRasterXSize());
  EXPECT_EQ(2, tif_ds->GetRasterYSize());
  EXPECT_EQ(1, tif_ds->GetRasterCount());

  // 2. Error case: multiple arrays in source without -array specification.
  auto arr2 = src_root->CreateMDArray("another_array", {dim_y, dim_x}, dt);
  ASSERT_NE(nullptr, arr2);

  options.reset(GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             options.get(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Several arrays exist. Select one"));
  }

  // 3. Error case: multiple -array options for non-multidim driver.
  std::vector<std::string> multi_arr_args = {
      "-of", "GTiff", "-array", "raster", "-array", "another_array",
  };
  std::vector<char*> multi_arr_argv = {multi_arr_args[0].data(),
                                       multi_arr_args[1].data(),
                                       multi_arr_args[2].data(),
                                       multi_arr_args[3].data(),
                                       multi_arr_args[4].data(),
                                       multi_arr_args[5].data(),
                                       nullptr};
  options.reset(
      GDALMultiDimTranslateOptionsNew(multi_arr_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             options.get(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("only one array should be specified"));
  }
}

// Test overwrite and no-overwrite flags.
TEST_F(GdalMultiDimTranslateTest, OverwriteOptions) {
  constexpr char kDest[] = "/vsimem/mdim_translate/overwrite.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);
  auto dim = src_root->CreateDimension("d", "", "", 1);
  src_root->CreateMDArray("a", {dim}, GDALExtendedDataType::Create(GDT_Int32));

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());

  // Create initial file.
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, nullptr, nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  // 1. With --no-overwrite, should fail because file exists.
  std::vector<std::string> no_ow_args = {"--no-overwrite"};
  std::vector<char*> no_ow_argv = {no_ow_args[0].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(no_ow_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             options.get(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
  }

  // 2. With --overwrite, should succeed.
  std::vector<std::string> ow_args = {"--overwrite"};
  std::vector<char*> ow_argv = {ow_args[0].data(), nullptr};
  options.reset(GDALMultiDimTranslateOptionsNew(ow_argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetUniquePtr ow_dst(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  EXPECT_NE(nullptr, ow_dst);
}

// Test translating a classic 2D raster dataset to multidimensional VRT.
TEST_F(GdalMultiDimTranslateTest, RasterToMultiDimVRT) {
  constexpr char kSrcTif[] = "/vsimem/mdim_translate/raster.tif";
  constexpr char kDest[] = "/vsimem/mdim_translate/raster_to_vrt.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr src_ds(
      gtiff_drv->Create(kSrcTif, 4, 3, 1, GDT_Float32, nullptr));
  ASSERT_NE(nullptr, src_ds);
  double gt[6] = {100.0, 10.0, 0.0, 200.0, 0.0, -10.0};
  EXPECT_EQ(CE_None, src_ds->SetGeoTransform(gt));
  src_ds->FlushCache(true);

  std::vector<std::string> args = {"-of", "VRT", "-array", "band=1"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto root = out_ds->GetRootGroup();
  ASSERT_NE(nullptr, root);
  auto arr = root->OpenMDArray("Band1");
  ASSERT_NE(nullptr, arr);
  EXPECT_EQ(2, arr->GetDimensionCount());
}

// Test array views on dimensions with indexing variables (positive and negative
// increments).
TEST_F(GdalMultiDimTranslateTest, ViewWithIndexingVariables) {
  constexpr char kDest[] = "/vsimem/mdim_translate/view_idx.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto dim_x = src_root->CreateDimension("x", "", "", 6);
  auto arr_x = src_root->CreateMDArray(
      "x", {dim_x}, GDALExtendedDataType::Create(GDT_Float64));
  ASSERT_NE(nullptr, arr_x);
  const std::vector<double> x_vals = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {6};
  EXPECT_TRUE(arr_x->Write(start.data(), count.data(), nullptr, nullptr,
                           arr_x->GetDataType(), x_vals.data()));
  EXPECT_TRUE(dim_x->SetIndexingVariable(arr_x));

  auto arr_data = src_root->CreateMDArray(
      "data", {dim_x}, GDALExtendedDataType::Create(GDT_Float32));
  ASSERT_NE(nullptr, arr_data);

  // 1. Positive step view: view=[1:5:2]
  {
    std::vector<std::string> args = {"-of", "VRT", "-array",
                                     "name=data,view=[1:5:2]"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();

    GDALDatasetUniquePtr out_ds(
        GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(nullptr, out_ds);
    auto out_arr = out_ds->GetRootGroup()->OpenMDArray("data");
    ASSERT_NE(nullptr, out_arr);
    EXPECT_EQ(2, out_arr->GetDimensions()[0]->GetSize());
  }

  // 2. Negative step view (reverse): view=[::-1]
  {
    constexpr char kDestRev[] = "/vsimem/mdim_translate/view_rev.vrt";
    std::vector<std::string> args = {"-of", "VRT", "-array",
                                     "name=data,view=[::-1]"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDestRev, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();

    GDALDatasetUniquePtr out_ds(
        GDALDataset::Open(kDestRev, GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(nullptr, out_ds);
    auto out_arr = out_ds->GetRootGroup()->OpenMDArray("data");
    ASSERT_NE(nullptr, out_arr);
    EXPECT_EQ(6, out_arr->GetDimensions()[0]->GetSize());
  }
}

// Test dimension subsetting on descending dimensions and non-monotonic
// dimensions.
TEST_F(GdalMultiDimTranslateTest, DescendingAndNonMonotonicSubsetting) {
  constexpr char kDest[] = "/vsimem/mdim_translate/subset_desc.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  // Descending time dimension: [50, 40, 30, 20, 10]
  auto dim_time = src_root->CreateDimension("time", "", "", 5);
  auto arr_time = src_root->CreateMDArray(
      "time", {dim_time}, GDALExtendedDataType::Create(GDT_Float64));
  ASSERT_NE(nullptr, arr_time);
  arr_time->SetUnit("seconds");
  const std::vector<double> time_vals = {50.0, 40.0, 30.0, 20.0, 10.0};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {5};
  EXPECT_TRUE(arr_time->Write(start.data(), count.data(), nullptr, nullptr,
                              arr_time->GetDataType(), time_vals.data()));
  EXPECT_TRUE(dim_time->SetIndexingVariable(arr_time));

  auto arr_data = src_root->CreateMDArray(
      "data", {dim_time}, GDALExtendedDataType::Create(GDT_Float32));
  ASSERT_NE(nullptr, arr_data);

  // 1. Range subset on descending dimension: time(20,40)
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset", "time(20,40)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();

    GDALDatasetUniquePtr out_ds(
        GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(nullptr, out_ds);
    auto out_arr = out_ds->GetRootGroup()->OpenMDArray("data");
    ASSERT_NE(nullptr, out_arr);
    EXPECT_EQ(3, out_arr->GetDimensions()[0]->GetSize());
  }

  // 2. Slice on descending dimension: time(30)
  {
    constexpr char kDestSlice[] = "/vsimem/mdim_translate/desc_slice.vrt";
    std::vector<std::string> args = {"-of", "VRT", "-subset", "time(30)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDestSlice, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();

    GDALDatasetUniquePtr out_ds(
        GDALDataset::Open(kDestSlice, GDAL_OF_MULTIDIM_RASTER));
    ASSERT_NE(nullptr, out_ds);
    auto out_arr = out_ds->GetRootGroup()->OpenMDArray("data");
    ASSERT_NE(nullptr, out_arr);
    EXPECT_EQ(0, out_arr->GetDimensionCount());
    auto unit_attr = out_arr->GetAttribute("DIM_time_UNIT");
    ASSERT_NE(nullptr, unit_attr);
  }
}

// Test root attributes copying and Conventions filtering.
TEST_F(GdalMultiDimTranslateTest, RootAttributesAndConventions) {
  constexpr char kDest[] = "/vsimem/mdim_translate/root_attrs.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto attr_conv = src_root->CreateAttribute(
      "Conventions", {}, GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, attr_conv);
  attr_conv->Write("CF-1.8");

  auto attr_title = src_root->CreateAttribute(
      "title", {}, GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, attr_title);
  attr_title->Write("Test MultiDim Dataset");

  auto attr_num = src_root->CreateAttribute(
      "num_attr", {}, GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, attr_num);
  attr_num->Write(99);

  std::vector<std::string> args = {"-of", "VRT", "-arrayoption", "OPT=1"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_root = out_ds->GetRootGroup();
  ASSERT_NE(nullptr, out_root);
  // Conventions is explicitly skipped during attribute copy.
  EXPECT_EQ(nullptr, out_root->GetAttribute("Conventions"));
  EXPECT_NE(nullptr, out_root->GetAttribute("title"));
  EXPECT_NE(nullptr, out_root->GetAttribute("num_attr"));
}

// Test dimension name collision resolution when sizes differ.
TEST_F(GdalMultiDimTranslateTest, DimensionNameCollision) {
  constexpr char kDest[] = "/vsimem/mdim_translate/dim_collision.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto grp1 = src_root->CreateGroup("grp1");
  auto grp2 = src_root->CreateGroup("grp2");
  ASSERT_NE(nullptr, grp1);
  ASSERT_NE(nullptr, grp2);

  auto dim1 = grp1->CreateDimension("d", "", "", 2);
  auto dim2 = grp2->CreateDimension("d", "", "", 5);
  grp1->CreateMDArray("arr1", {dim1}, GDALExtendedDataType::Create(GDT_Int32));
  grp2->CreateMDArray("arr2", {dim2}, GDALExtendedDataType::Create(GDT_Int32));

  std::vector<std::string> args = {"-of",        "VRT",    "-array",
                                   "/grp1/arr1", "-array", "/grp2/arr2"};
  std::vector<char*> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  EXPECT_EQ(2, out_ds->GetRootGroup()->GetDimensions().size());
}

// Test copying array with explicit -array spec to GTiff.
TEST_F(GdalMultiDimTranslateTest, CopyToNonMultiDimVariations) {
  constexpr char kSrcVrt[] = "/vsimem/mdim_translate/src2.vrt";
  constexpr char kDest[] = "/vsimem/mdim_translate/non_mdim_2d.tif";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"y\" size=\"2\"/>\n"
      "    <Dimension name=\"x\" size=\"5\"/>\n"
      "    <Array name=\"a2d\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"/y\"/>\n"
      "      <DimensionRef ref=\"/x\"/>\n"
      "      <InlineValues>1 2 3 4 5 6 7 8 9 10</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";
  VsiMemTempWrapper src_vrt_file(kSrcVrt, vrt_xml);

  GDALDatasetUniquePtr src_ds(
      GDALDataset::Open(kSrcVrt, GDAL_OF_MULTIDIM_RASTER));
  if (!src_ds) {
    FAIL() << "Failed to open vrt: " << CPLGetLastErrorMsg();
  }
  ASSERT_NE(nullptr, src_ds);

  std::vector<std::string> args = {"-of", "GTiff", "-array", "a2d"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr tif_ds(GDALDataset::Open(kDest, GDAL_OF_RASTER));
  ASSERT_NE(nullptr, tif_ds);
  EXPECT_EQ(5, tif_ds->GetRasterXSize());
  EXPECT_EQ(2, tif_ds->GetRasterYSize());
}

// Test incompatible options with -array "view=...".
TEST_F(GdalMultiDimTranslateTest, IncompatibleOptionsWithView) {
  // 1. -scaleaxes with view
  {
    std::vector<std::string> args = {
        "-of", "VRT", "-scaleaxes", "x(2)", "-array", "view",
    };
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    WithQuietHandler quiet;
    CPLErrorReset();
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    EXPECT_EQ(nullptr, options);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not compatible with the -array \"view\" option"));
  }

  // 2. -subset with view
  {
    std::vector<std::string> args = {
        "-of", "VRT", "-subset", "x(10,20)", "-array", "view",
    };
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    WithQuietHandler quiet;
    CPLErrorReset();
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    EXPECT_EQ(nullptr, options);
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("not compatible with the -array \"view\" option"));
  }

  // 3. Unexpected array spec part fails at translate time
  {
    std::vector<std::string> args = {
        "-of",
        "VRT",
        "-array",
        "name=data,unknown_part=val",
    };
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);

    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr src_ds(
        mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALMultiDimTranslate("/vsimem/mdim_translate/err.vrt", nullptr,
                                    1, &hSrc, options.get(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Unexpected array specification part"));
  }
}

// Test classic raster dataset translated as whole multidimensional array and
// without geotransform.
TEST_F(GdalMultiDimTranslateTest, ClassicRasterAsMDArrayAndNoGeoTransform) {
  constexpr char kDest[] = "/vsimem/mdim_translate/classic_cube.vrt";
  constexpr char kDestNoGt[] = "/vsimem/mdim_translate/no_gt.vrt";
  constexpr char kSrcTif[] = "/vsimem/mdim_translate/src_classic.tif";
  constexpr char kSrcNoGtTif[] = "/vsimem/mdim_translate/src_nogt.tif";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr src_ds(
      gtiff_drv->Create(kSrcTif, 4, 3, 2, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_ds);
  double gt[6] = {0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  src_ds->SetGeoTransform(gt);
  src_ds->FlushCache(true);

  // 1. Classic dataset AsMDArray() when band < 0 (no band specified).
  std::vector<std::string> args = {"-of", "VRT", "-array", "dstname=cube"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto names = out_ds->GetRootGroup()->GetMDArrayNames();
  ASSERT_FALSE(names.empty());
  auto arr = out_ds->GetRootGroup()->OpenMDArray(names[0]);
  ASSERT_NE(nullptr, arr);
  EXPECT_EQ(1, arr->GetDimensionCount());

  // 2. Classic dataset without geotransform produces inlined indexing
  // variables.
  GDALDatasetUniquePtr src_nogt(
      gtiff_drv->Create(kSrcNoGtTif, 3, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, src_nogt);
  src_nogt->FlushCache(true);

  std::vector<std::string> args2 = {"-of", "VRT", "-array", "band=1"};
  std::vector<char*> argv2 = {args2[0].data(), args2[1].data(), args2[2].data(),
                              args2[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr opt2(
      GDALMultiDimTranslateOptionsNew(argv2.data(), nullptr));
  ASSERT_NE(nullptr, opt2);
  GDALDatasetH hSrc2 = GDALDataset::ToHandle(src_nogt.get());
  GDALDatasetUniquePtr hDst2_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
      kDestNoGt, nullptr, 1, &hSrc2, opt2.get(), nullptr)));
  ASSERT_NE(nullptr, hDst2_ds);
  hDst2_ds.reset();

  GDALDatasetUniquePtr out_ds2(
      GDALDataset::Open(kDestNoGt, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds2);
  EXPECT_NE(nullptr, out_ds2->GetRootGroup()->OpenMDArray("Band1"));
}

// Test error handling for -group option.
TEST_F(GdalMultiDimTranslateTest, GroupErrorHandling) {
  constexpr char kDest[] = "/vsimem/mdim_translate/group_err.vrt";
  constexpr char kSrcTif[] = "/vsimem/mdim_translate/src_for_grp.tif";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* gtiff_drv = GetGDALDriverManager()->GetDriverByName("GTiff");
  ASSERT_NE(nullptr, gtiff_drv);
  GDALDatasetUniquePtr classic_ds(
      gtiff_drv->Create(kSrcTif, 2, 2, 1, GDT_Byte, nullptr));
  ASSERT_NE(nullptr, classic_ds);
  classic_ds->FlushCache(true);

  GDALDatasetH hClassic = GDALDataset::ToHandle(classic_ds.get());

  // 1. -group with classic raster fails.
  {
    std::vector<std::string> args = {"-of", "VRT", "-group", "sub"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hClassic,
                                             options.get(), nullptr));
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("No multidimensional source dataset"));
  }

  // 2. -group with nonexistent group in multidim dataset fails.
  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr mdim_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, mdim_ds);
  auto root = mdim_ds->GetRootGroup();
  root->CreateGroup("existing_group");
  GDALDatasetH hMdim = GDALDataset::ToHandle(mdim_ds.get());

  {
    std::vector<std::string> args = {"-of", "VRT", "-group", "missing_group"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hMdim,
                                             options.get(), nullptr));
  }

  // 3. Multiple -group where one is missing fails.
  {
    std::vector<std::string> args = {
        "-of", "VRT", "-group", "existing_group", "-group", "missing_group"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), args[4].data(), args[5].data(),
                               nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hMdim,
                                             options.get(), nullptr));
  }
}

// Test copying subgroup attributes and linking dimension indexing variables.
TEST_F(GdalMultiDimTranslateTest, SubgroupAttributesAndIndexingVars) {
  constexpr char kDest[] = "/vsimem/mdim_translate/subgrp_attrs.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  auto sub = src_root->CreateGroup("sub");
  ASSERT_NE(nullptr, sub);
  auto attr = sub->CreateAttribute("sub_int", {},
                                   GDALExtendedDataType::Create(GDT_Int32));
  ASSERT_NE(nullptr, attr);
  EXPECT_TRUE(attr->Write(42));

  auto dim = sub->CreateDimension("d", "", "", 3);
  auto arr_d =
      sub->CreateMDArray("d", {dim}, GDALExtendedDataType::Create(GDT_Float32));
  ASSERT_NE(nullptr, arr_d);
  const std::vector<float> d_vals = {1.0f, 2.0f, 3.0f};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {3};
  EXPECT_TRUE(arr_d->Write(start.data(), count.data(), nullptr, nullptr,
                           arr_d->GetDataType(), d_vals.data()));
  EXPECT_TRUE(dim->SetIndexingVariable(arr_d));

  sub->CreateMDArray("sub_data", {dim}, GDALExtendedDataType::Create(GDT_Byte));

  std::vector<std::string> args = {"-of", "VRT", "-group", "sub"};
  std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                             args[3].data(), nullptr};
  MultiDimTranslateOptionsUniquePtr options(
      GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
  ASSERT_NE(nullptr, options);

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
      GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
  ASSERT_NE(nullptr, hDst_ds);
  hDst_ds.reset();

  GDALDatasetUniquePtr out_ds(
      GDALDataset::Open(kDest, GDAL_OF_MULTIDIM_RASTER));
  ASSERT_NE(nullptr, out_ds);
  auto out_root = out_ds->GetRootGroup();
  ASSERT_NE(nullptr, out_root);
  EXPECT_NE(nullptr, out_root->GetAttribute("sub_int"));
}

// Test boundary cases of numeric and string subsetting.
TEST_F(GdalMultiDimTranslateTest, MinMaxBoundarySubsetting) {
  constexpr char kDest[] = "/vsimem/mdim_translate/boundary.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  // Numeric ascending [10, 20, 30, 40, 50]
  auto dim_time = src_root->CreateDimension("time", "", "", 5);
  auto arr_time = src_root->CreateMDArray(
      "time", {dim_time}, GDALExtendedDataType::Create(GDT_Float64));
  ASSERT_NE(nullptr, arr_time);
  const std::vector<double> time_vals = {10.0, 20.0, 30.0, 40.0, 50.0};
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {5};
  EXPECT_TRUE(arr_time->Write(start.data(), count.data(), nullptr, nullptr,
                              arr_time->GetDataType(), time_vals.data()));
  EXPECT_TRUE(dim_time->SetIndexingVariable(arr_time));

  // String ascending ["alpha", "beta", "delta", "gamma"]
  auto dim_str = src_root->CreateDimension("cat", "", "", 4);
  auto arr_str = src_root->CreateMDArray("cat", {dim_str},
                                         GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, arr_str);
  const char* str_vals[4] = {"alpha", "beta", "delta", "gamma"};
  const std::vector<size_t> str_count = {4};
  EXPECT_TRUE(arr_str->Write(start.data(), str_count.data(), nullptr, nullptr,
                             arr_str->GetDataType(), str_vals));
  EXPECT_TRUE(dim_str->SetIndexingVariable(arr_str));

  src_root->CreateMDArray("var_num", {dim_time},
                          GDALExtendedDataType::Create(GDT_Int32));
  src_root->CreateMDArray("var_str", {dim_str},
                          GDALExtendedDataType::Create(GDT_Int32));

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());

  // 1. time(5, 30) (dfMin < min)
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset", "time(5,30)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // 2. time(20, 100) (dfMax > max)
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset", "time(20,100)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // 3. cat(aaa, delta) (osMin < min)
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset", "cat(aaa,delta)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // 4. cat(beta, zzz) (osMax > max)
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset", "cat(beta,zzz)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // 5. cat(alpha, alpha) (single slice on string dimension)
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset",
                                     "cat(alpha,alpha)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // 6. cat(aaa, aab) (empty string set error)
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset", "cat(aaa,aab)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                             options.get(), nullptr));
  }
}

// Test size-1 dimensions, descending string dimensions, and netCDF extension.
TEST_F(GdalMultiDimTranslateTest, ExhaustiveSubsettingAndOptions) {
  constexpr char kDest[] = "/vsimem/mdim_translate/size1.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("src", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  // 1. Size-1 numeric dimension.
  auto dim_num1 = src_root->CreateDimension("n1", "", "", 1);
  auto arr_num1 = src_root->CreateMDArray(
      "n1", {dim_num1}, GDALExtendedDataType::Create(GDT_Float64));
  ASSERT_NE(nullptr, arr_num1);
  double n1_val = 10.0;
  const std::vector<GUInt64> start = {0};
  const std::vector<size_t> count = {1};
  EXPECT_TRUE(arr_num1->Write(start.data(), count.data(), nullptr, nullptr,
                              arr_num1->GetDataType(), &n1_val));
  EXPECT_TRUE(dim_num1->SetIndexingVariable(arr_num1));
  src_root->CreateMDArray("var_n1", {dim_num1},
                          GDALExtendedDataType::Create(GDT_Int32));

  // 2. Size-1 string dimension.
  auto dim_str1 = src_root->CreateDimension("s1", "", "", 1);
  auto arr_str1 = src_root->CreateMDArray("s1", {dim_str1},
                                          GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, arr_str1);
  const char* s1_val = "hello";
  EXPECT_TRUE(arr_str1->Write(start.data(), count.data(), nullptr, nullptr,
                              arr_str1->GetDataType(), &s1_val));
  EXPECT_TRUE(dim_str1->SetIndexingVariable(arr_str1));
  src_root->CreateMDArray("var_s1", {dim_str1},
                          GDALExtendedDataType::Create(GDT_Int32));

  // 3. Descending string dimension.
  auto dim_sdesc = src_root->CreateDimension("sdesc", "", "", 4);
  auto arr_sdesc = src_root->CreateMDArray(
      "sdesc", {dim_sdesc}, GDALExtendedDataType::CreateString());
  ASSERT_NE(nullptr, arr_sdesc);
  const char* sdesc_vals[4] = {"gamma", "delta", "beta", "alpha"};
  const std::vector<size_t> count4 = {4};
  EXPECT_TRUE(arr_sdesc->Write(start.data(), count4.data(), nullptr, nullptr,
                               arr_sdesc->GetDataType(), sdesc_vals));
  EXPECT_TRUE(dim_sdesc->SetIndexingVariable(arr_sdesc));
  src_root->CreateMDArray("var_sdesc", {dim_sdesc},
                          GDALExtendedDataType::Create(GDT_Int32));

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());

  // Test size-1 numeric subsetting.
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset", "n1(5,15)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // Test size-1 string subsetting.
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset",
                                     "s1(hello,hello)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // Test descending string subsetting.
  {
    std::vector<std::string> args = {"-of", "VRT", "-subset",
                                     "sdesc(beta,delta)"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(GDALMultiDimTranslate(
        kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    ASSERT_NE(nullptr, hDst_ds);
    hDst_ds.reset();
  }

  // Test netCDF extension auto driver selection.
  {
    WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              GDALMultiDimTranslate("/vsimem/mdim_translate/out.nc", nullptr, 1,
                                    &hSrc, nullptr, nullptr));
  }
}

TEST_F(GdalMultiDimTranslateTest, DriverSelectionErrors) {
  constexpr char kSrcVrt[] = "/vsimem/mdim_translate/drv_err.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"x\" size=\"2\"/>\n"
      "    <Array name=\"ar\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"x\"/>\n"
      "      <InlineValues>1 2</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VsiMemTempWrapper src_vrt_file(kSrcVrt, vrt_xml);

  GDALDatasetUniquePtr src_ds(
      GDALDataset::Open(kSrcVrt, GDAL_OF_MULTIDIM_RASTER));
  if (!src_ds) {
    FAIL() << "Failed to open vrt: " << CPLGetLastErrorMsg();
  }
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());

  WithQuietHandler quiet;

  // 1. Destination has no extension and no format -> "Cannot determine output
  // driver"
  {
    CPLErrorReset();
    GDALDatasetH hDst =
        GDALMultiDimTranslate("/vsimem/mdim_translate/noextfile", nullptr, 1,
                              &hSrc, nullptr, nullptr);
    EXPECT_EQ(nullptr, hDst);
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  }

  // 2. Output driver unknown or does not support creation -> "Output driver
  // '...' not recognised"
  {
    std::vector<std::string> args = {"-of", "NONEXISTENT_DRIVER"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    CPLErrorReset();
    GDALDatasetH hDst =
        GDALMultiDimTranslate("/vsimem/mdim_translate/out.tif", nullptr, 1,
                              &hSrc, options.get(), nullptr);
    EXPECT_EQ(nullptr, hDst);
    EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
  }
}

TEST_F(GdalMultiDimTranslateTest, ClassicDataset1DAndHorizontalTypes) {
  constexpr char kSrcVrt[] = "/vsimem/mdim_translate/classic_types.vrt";
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  const std::string vrt_xml =
      "<VRTDataset>\n"
      "  <Group name=\"/\">\n"
      "    <Dimension name=\"x\" size=\"2\" type=\"HORIZONTAL_X\"/>\n"
      "    <Dimension name=\"y\" size=\"2\" type=\"HORIZONTAL_Y\"/>\n"
      "    <Array name=\"ar1d\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"x\"/>\n"
      "      <InlineValues>1 2</InlineValues>\n"
      "    </Array>\n"
      "    <Array name=\"ar2d\">\n"
      "      <DataType>Byte</DataType>\n"
      "      <DimensionRef ref=\"y\"/>\n"
      "      <DimensionRef ref=\"x\"/>\n"
      "      <InlineValues>1 2 3 4</InlineValues>\n"
      "    </Array>\n"
      "  </Group>\n"
      "</VRTDataset>\n";

  VsiMemTempWrapper src_vrt_file(kSrcVrt, vrt_xml);

  GDALDatasetUniquePtr src_ds(
      GDALDataset::Open(kSrcVrt, GDAL_OF_MULTIDIM_RASTER));
  if (!src_ds) {
    FAIL() << "Failed to open vrt: " << CPLGetLastErrorMsg();
  }
  ASSERT_NE(nullptr, src_ds);
  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());

  // Copy 1D array to GTiff (hits dims.size() == 1, iXDim = 0)
  {
    std::vector<std::string> args = {"-of", "GTiff", "-array", "name=ar1d"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
        GDALMultiDimTranslate("/vsimem/mdim_translate/out_1d.tif", nullptr, 1,
                              &hSrc, options.get(), nullptr)));
  }

  // Copy 2D array with HORIZONTAL_X and HORIZONTAL_Y dimension types (hits
  // lines 1778, 1782)
  {
    std::vector<std::string> args = {"-of", "GTiff", "-array", "name=ar2d"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    GDALDatasetUniquePtr hDst_ds(GDALDataset::FromHandle(
        GDALMultiDimTranslate("/vsimem/mdim_translate/out_2d.tif", nullptr, 1,
                              &hSrc, options.get(), nullptr)));
  }
}

TEST_F(GdalMultiDimTranslateTest, MonotonicityAndSliceBoundaries) {
  absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

  GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
  ASSERT_NE(nullptr, mem_drv);
  GDALDatasetUniquePtr src_ds(
      mem_drv->CreateMultiDimensional("", nullptr, nullptr));
  ASSERT_NE(nullptr, src_ds);
  auto src_root = src_ds->GetRootGroup();
  ASSERT_NE(nullptr, src_root);

  const GUInt64 start[1] = {0};
  const size_t count3[1] = {3};

  // non_mono
  auto dim_nm = src_root->CreateDimension("non_mono", "", "", 3);
  auto var_nm = src_root->CreateMDArray(
      "non_mono_var", {dim_nm}, GDALExtendedDataType::Create(GDT_Float64));
  std::vector<double> vals_nm = {10.0, 30.0, 20.0};
  var_nm->Write(start, count3, nullptr, nullptr, var_nm->GetDataType(),
                vals_nm.data());
  dim_nm->SetIndexingVariable(var_nm);
  src_root->CreateMDArray("data_non_mono", {dim_nm},
                          GDALExtendedDataType::Create(GDT_Byte));

  // rev_num
  auto dim_rn = src_root->CreateDimension("rev_num", "", "", 3);
  auto var_rn = src_root->CreateMDArray(
      "rev_num_var", {dim_rn}, GDALExtendedDataType::Create(GDT_Float64));
  std::vector<double> vals_rn = {30.0, 20.0, 10.0};
  var_rn->Write(start, count3, nullptr, nullptr, var_rn->GetDataType(),
                vals_rn.data());
  dim_rn->SetIndexingVariable(var_rn);
  src_root->CreateMDArray("data_rev_num", {dim_rn},
                          GDALExtendedDataType::Create(GDT_Byte));

  // fwd_num
  auto dim_fn = src_root->CreateDimension("fwd_num", "", "", 3);
  auto var_fn = src_root->CreateMDArray(
      "fwd_num_var", {dim_fn}, GDALExtendedDataType::Create(GDT_Float64));
  std::vector<double> vals_fn = {10.0, 20.0, 30.0};
  var_fn->Write(start, count3, nullptr, nullptr, var_fn->GetDataType(),
                vals_fn.data());
  dim_fn->SetIndexingVariable(var_fn);
  src_root->CreateMDArray("data_fwd_num", {dim_fn},
                          GDALExtendedDataType::Create(GDT_Byte));

  // non_mono_str
  auto dim_nms = src_root->CreateDimension("non_mono_str", "", "", 3);
  auto var_nms = src_root->CreateMDArray("non_mono_str_var", {dim_nms},
                                         GDALExtendedDataType::CreateString());
  const char* vals_nms[3] = {"b", "c", "a"};
  var_nms->Write(start, count3, nullptr, nullptr, var_nms->GetDataType(),
                 vals_nms);
  dim_nms->SetIndexingVariable(var_nms);
  src_root->CreateMDArray("data_non_mono_str", {dim_nms},
                          GDALExtendedDataType::Create(GDT_Byte));

  // rev_str
  auto dim_rs = src_root->CreateDimension("rev_str", "", "", 3);
  auto var_rs = src_root->CreateMDArray("rev_str_var", {dim_rs},
                                        GDALExtendedDataType::CreateString());
  const char* vals_rs[3] = {"c", "b", "a"};
  var_rs->Write(start, count3, nullptr, nullptr, var_rs->GetDataType(),
                vals_rs);
  dim_rs->SetIndexingVariable(var_rs);
  src_root->CreateMDArray("data_rev_str", {dim_rs},
                          GDALExtendedDataType::Create(GDT_Byte));

  // fwd_str
  auto dim_fs = src_root->CreateDimension("fwd_str", "", "", 3);
  auto var_fs = src_root->CreateMDArray("fwd_str_var", {dim_fs},
                                        GDALExtendedDataType::CreateString());
  const char* vals_fs[3] = {"a", "b", "c"};
  var_fs->Write(start, count3, nullptr, nullptr, var_fs->GetDataType(),
                vals_fs);
  dim_fs->SetIndexingVariable(var_fs);
  src_root->CreateMDArray("data_fwd_str", {dim_fs},
                          GDALExtendedDataType::Create(GDT_Byte));

  // big dim
  auto dim_big = src_root->CreateDimension("big", "", "", 4000000000ULL);
  src_root->CreateMDArray("data_big", {dim_big},
                          GDALExtendedDataType::Create(GDT_Byte));

  GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());
  WithQuietHandler quiet;

  auto run_translate = [&](const std::string& subset_arg) {
    std::vector<std::string> args = {"-of", "VRT", "-subset", subset_arg};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
    ASSERT_NE(nullptr, options);
    CPLErrorReset();
    GDALDatasetUniquePtr dst_ds(GDALDataset::FromHandle(
        GDALMultiDimTranslate("/vsimem/mdim_translate/out_mono.vrt", nullptr, 1,
                              &hSrc, options.get(), nullptr)));
    EXPECT_EQ(nullptr, dst_ds) << subset_arg;
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType()) << subset_arg;
  };

  // Numeric:
  // Non-monotonic error (lines 201-204)
  run_translate("non_mono(15,25)");
  // Reversed array: dfMin > max (lines 212-213)
  run_translate("rev_num(35,40)");
  // Forward array: dfMin < min with slice (lines 219-220)
  run_translate("fwd_num(5)");
  // Reversed array: dfMax > max with slice (lines 246-247)
  run_translate("rev_num(35)");
  // Forward array: dfMax < min (lines 256-257)
  run_translate("fwd_num(0,5)");

  // String:
  // Non-monotonic string error (lines 352-355)
  run_translate("non_mono_str(b,c)");
  // Reversed string: osMin > max (lines 364-365)
  run_translate("rev_str(z,zz)");
  // Forward string: osMin < min with slice (lines 371-372)
  run_translate("fwd_str(0)");

  // Negative step range indexing (lines 1158-1163)
  run_translate("fwd_num[2:-1:0]");

  // Slice on dimension with index > INT_MAX: (lines 1338-1343)
  run_translate("big[3000000000]");
}

TEST_F(GdalMultiDimTranslateTest, FinalBranchCoverage) {
  // 1. Binary options format propagation (line 2082)
  {
    GDALMultiDimTranslateOptionsForBinary options_for_binary;
    std::vector<std::string> args = {"-of", "VRT", "src.nc", "dst.vrt"};
    std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                               args[3].data(), nullptr};
    MultiDimTranslateOptionsUniquePtr options(
        GDALMultiDimTranslateOptionsNew(argv.data(), &options_for_binary));
    ASSERT_NE(nullptr, options);
    EXPECT_EQ("VRT", options_for_binary.osFormat);
  }

  // 2. Incompatible -subset and -scaleaxes with -array "view" (lines 2053-2068)
  {
    WithQuietHandler quiet;
    // -scaleaxes with -array "view"
    {
      std::vector<std::string> args = {"-scaleaxes", "d=2", "-array", "view"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      CPLErrorReset();
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      EXPECT_EQ(nullptr, options);
      EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    }
    // -subset with -array "view"
    {
      std::vector<std::string> args = {"-subset", "dim(0)", "-array", "view"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      CPLErrorReset();
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      EXPECT_EQ(nullptr, options);
      EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    }
  }

  // 3. No multidimensional source dataset (lines 1696-1698) and invalid band
  // (lines 906-908)
  {
    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr src_ds(
        mem_drv->Create("", 10, 10, 1, GDT_Byte, nullptr));
    ASSERT_NE(nullptr, src_ds);
    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());

    WithQuietHandler quiet;

    // No multidimensional source dataset
    {
      std::vector<std::string> args = {"-of", "VRT", "-arrayoption", "OPT=1"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      CPLErrorReset();
      GDALDatasetH hDst =
          GDALMultiDimTranslate("/vsimem/mdim_translate/nomdim.vrt", nullptr, 1,
                                &hSrc, options.get(), nullptr);
      EXPECT_EQ(nullptr, hDst);
      EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    }

    // No multidimensional source dataset: -group cannot be used (lines
    // 1630-1636)
    {
      std::vector<std::string> args = {"-of", "VRT", "-group", "sub"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      CPLErrorReset();
      GDALDatasetH hDst =
          GDALMultiDimTranslate("/vsimem/mdim_translate/nomdim_grp.vrt",
                                nullptr, 1, &hSrc, options.get(), nullptr);
      EXPECT_EQ(nullptr, hDst);
      EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
    }

    // Invalid band number (lines 906-908)
    {
      std::vector<std::string> args = {"-of", "VRT", "-array", "band=99"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      CPLErrorReset();
      GDALDatasetH hDst =
          GDALMultiDimTranslate("/vsimem/mdim_translate/badband.vrt", nullptr,
                                1, &hSrc, options.get(), nullptr);
      EXPECT_EQ(nullptr, hDst);
    }
  }

  // 4. Recursive subgroup copying and multiple slices in view (lines 1025-1026,
  // 1540-1546)
  {
    constexpr char kDest[] = "/vsimem/mdim_translate/subgroups.vrt";
    absl::Cleanup cleanup = [] { VSIRmdirRecursive("/vsimem/mdim_translate"); };

    GDALDriver* mem_drv = GetGDALDriverManager()->GetDriverByName("MEM");
    ASSERT_NE(nullptr, mem_drv);
    GDALDatasetUniquePtr src_ds(
        mem_drv->CreateMultiDimensional("", nullptr, nullptr));
    ASSERT_NE(nullptr, src_ds);
    auto root = src_ds->GetRootGroup();
    ASSERT_NE(nullptr, root);
    auto sub = root->CreateGroup("sub");
    ASSERT_NE(nullptr, sub);
    auto sub2 = root->CreateGroup("sub2");
    ASSERT_NE(nullptr, sub2);
    auto subsub = sub->CreateGroup("subsub");
    ASSERT_NE(nullptr, subsub);
    auto dim = subsub->CreateDimension("d", "", "", 2);
    ASSERT_NE(nullptr, dim);
    subsub->CreateMDArray("ar", {dim}, GDALExtendedDataType::Create(GDT_Byte));
    auto dim2 = sub2->CreateDimension("d2", "", "", 2);
    ASSERT_NE(nullptr, dim2);
    sub2->CreateMDArray("ar2", {dim2}, GDALExtendedDataType::Create(GDT_Byte));

    GDALDatasetH hSrc = GDALDataset::ToHandle(src_ds.get());

    // Copy multiple groups (lines 1653-1676)
    {
      std::vector<std::string> args = {"-of", "VRT",    "-group",
                                       "sub", "-group", "sub2"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), args[4].data(), args[5].data(),
                                 nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      GDALDatasetUniquePtr hDst_ds(
          GDALDataset::FromHandle(GDALMultiDimTranslate(
              kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    }

    // Nonexistent single group (lines 1646-1647)
    {
      std::vector<std::string> args = {"-of", "VRT", "-group", "nonexistent"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetH hDst = GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                                options.get(), nullptr);
      EXPECT_EQ(nullptr, hDst);
    }

    // Copy group recursively
    {
      std::vector<std::string> args = {"-of", "VRT", "-group", "sub"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      GDALDatasetUniquePtr hDst_ds(
          GDALDataset::FromHandle(GDALMultiDimTranslate(
              kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    }

    // View with scalefactor conflict (lines 941-944)
    {
      std::vector<std::string> args = {
          "-of", "VRT",    "-scaleaxes",
          "d=2", "-array", "name=/sub/subsub/ar,view=[0:1]"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), args[4].data(), args[5].data(),
                                 nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetH hDst = GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                                options.get(), nullptr);
      EXPECT_EQ(nullptr, hDst);
      EXPECT_EQ(CPLE_NotSupported, CPLGetLastErrorNo());
    }

    // Invalid view expression (line 948)
    {
      std::vector<std::string> args = {"-of", "VRT", "-array",
                                       "name=/sub/subsub/ar,view=[invalid]"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetH hDst = GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                                options.get(), nullptr);
      EXPECT_EQ(nullptr, hDst);
    }

    // Invalid group spec in multi-group (line 1661)
    {
      std::vector<std::string> args = {"-of", "VRT",    "-group",
                                       "sub", "-group", "invalid[spec"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), args[4].data(), args[5].data(),
                                 nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      WithQuietHandler quiet;
      CPLErrorReset();
      GDALDatasetH hDst = GDALMultiDimTranslate(kDest, nullptr, 1, &hSrc,
                                                options.get(), nullptr);
      EXPECT_EQ(nullptr, hDst);
    }

    // View with multiple slice specs (lines 1025-1026)
    {
      std::vector<std::string> args = {"-of", "VRT", "-array",
                                       "name=/sub/subsub/ar,view=[0:1][0:1]"};
      std::vector<char*> argv = {args[0].data(), args[1].data(), args[2].data(),
                                 args[3].data(), nullptr};
      MultiDimTranslateOptionsUniquePtr options(
          GDALMultiDimTranslateOptionsNew(argv.data(), nullptr));
      ASSERT_NE(nullptr, options);
      GDALDatasetUniquePtr hDst_ds(
          GDALDataset::FromHandle(GDALMultiDimTranslate(
              kDest, nullptr, 1, &hSrc, options.get(), nullptr)));
    }
  }
}

}  // namespace
}  // namespace autotest2

// TODO: b/335317901 - Cover remaining unreachable/defensive branches in
// apps/gdalmdimtranslate_lib.cpp.
