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
// Tests for Zarr V3 group operations (zarr_v3_group.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_v3_group.cpp

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_string.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

TEST(ZarrV3GroupTest, CreateOnDiskAndAttributesRoundTrip) {
  ScopedTestDir test_dir("zarr_v3_grp_disk");
  const std::string root_dir = test_dir.File("v3root");

  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);

  // 1. CreateOnDisk: fails if directory already exists
  VSIMkdir(root_dir.c_str(), 0755);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto fail_root = ZarrV3Group::CreateOnDisk(shared_res, "", "/", root_dir);
    EXPECT_EQ(nullptr, fail_root);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  VSIRmdir(root_dir.c_str());

  // 2. CreateOnDisk: succeeds on clean path
  auto root = ZarrV3Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // 3. Add attribute, modify, Close() saves to zarr.json
  auto attr = root->CreateAttribute(
      "title", {}, GDALExtendedDataType::CreateString(0), nullptr);
  ASSERT_NE(nullptr, attr);
  const char* pszTitle = "My Dataset";
  EXPECT_TRUE(attr->Write(pszTitle));

  EXPECT_TRUE(std::static_pointer_cast<ZarrGroupBase>(root)->Close());

  // 4. Re-open and verify LoadAttributes reads zarr.json
  auto reopen_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
  auto reopen_root = ZarrV3Group::Create(reopen_res, "", "/", root_dir);
  ASSERT_NE(nullptr, reopen_root);
  auto loaded_attr = reopen_root->GetAttribute("title");
  ASSERT_NE(nullptr, loaded_attr);
  EXPECT_EQ("title", loaded_attr->GetName());
  EXPECT_STREQ("My Dataset", loaded_attr->ReadAsString());

  // 5. Corrupt zarr.json in LoadAttributes
  const std::string corrupt_dir = test_dir.File("corrupt_v3");
  VSIMkdirRecursive(corrupt_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(corrupt_dir.c_str(), "zarr.json", nullptr),
               "{corrupt json");
  auto corrupt_res =
      ZarrSharedResource::Create(corrupt_dir, /*bUpdatable=*/false);
  auto corrupt_root = ZarrV3Group::Create(corrupt_res, "", "/", corrupt_dir);
  ASSERT_NE(nullptr, corrupt_root);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, corrupt_root->GetAttribute("any"));
  }
}

TEST(ZarrV3GroupTest, ExploreDirectoryAndOpenGroupAndArray) {
  ScopedTestDir test_dir("zarr_v3_explore");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);

  // Write root zarr.json
  WriteVsiFile(CPLFormFilenameSafe(root_dir.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 3,
                 "node_type": "group",
                 "attributes": {}
               })json");

  // 1. Array subgroup
  const std::string arr_ok =
      CPLFormFilenameSafe(root_dir.c_str(), "arr_ok", nullptr);
  VSIMkdirRecursive(arr_ok.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(arr_ok.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 3,
                 "node_type": "array",
                 "shape": [4],
                 "data_type": "int32",
                 "chunk_grid": {
                   "name": "regular",
                   "configuration": {"chunk_shape": [4]}
                 },
                 "chunk_key_encoding": {"name": "default"},
                 "fill_value": 0,
                 "codecs": [
                   {"name": "bytes", "configuration": {"endian": "little"}}
                 ]
               })json");

  // 2. Explicit group
  const std::string grp_ok =
      CPLFormFilenameSafe(root_dir.c_str(), "grp_ok", nullptr);
  VSIMkdirRecursive(grp_ok.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(grp_ok.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 3,
                 "node_type": "group",
                 "attributes": {"k": "v"}
               })json");

  // 3. Bad format group (zarr_format = 2)
  const std::string bad_fmt =
      CPLFormFilenameSafe(root_dir.c_str(), "bad_fmt", nullptr);
  VSIMkdirRecursive(bad_fmt.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(bad_fmt.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 2,
                 "node_type": "group"
               })json");

  // 4. Bad node_type group
  const std::string bad_node =
      CPLFormFilenameSafe(root_dir.c_str(), "bad_node", nullptr);
  VSIMkdirRecursive(bad_node.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(bad_node.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 3,
                 "node_type": "unknown_type"
               })json");

  // 5. Implicit group (no zarr.json)
  const std::string implicit_grp =
      CPLFormFilenameSafe(root_dir.c_str(), "implicit_grp", nullptr);
  VSIMkdirRecursive(implicit_grp.c_str(), 0755);

  // 6. Corrupt zarr.json group
  const std::string corrupt_grp =
      CPLFormFilenameSafe(root_dir.c_str(), "corrupt_grp", nullptr);
  VSIMkdirRecursive(corrupt_grp.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(corrupt_grp.c_str(), "zarr.json", nullptr),
               "{corrupt json");

  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
  auto root = ZarrV3Group::Create(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // ExploreDirectory triggered by GetGroupNames / GetMDArrayNames
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto grp_names = root->GetGroupNames(nullptr);
    auto arr_names = root->GetMDArrayNames(nullptr);

    EXPECT_THAT(grp_names, UnorderedElementsAre("grp_ok", "implicit_grp"));
    EXPECT_THAT(arr_names, ElementsAre("arr_ok"));
  }

  // OpenZarrGroup: explicit valid group (and cached)
  auto sub_grp = root->OpenZarrGroup("grp_ok", nullptr);
  ASSERT_NE(nullptr, sub_grp);
  EXPECT_EQ(sub_grp.get(), root->OpenZarrGroup("grp_ok", nullptr).get());

  // OpenZarrGroup: implicit group (deprecated warning)
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto imp = root->OpenZarrGroup("implicit_grp", nullptr);
    EXPECT_NE(nullptr, imp);
  }

  // OpenZarrGroup: bad format (zarr_format != 3)
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->OpenZarrGroup("bad_fmt", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unhandled zarr_format"));
  }

  // OpenZarrGroup: opened array instead of group
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->OpenZarrGroup("arr_ok", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("not a group"));
  }

  // OpenZarrGroup: corrupt and missing
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->OpenZarrGroup("corrupt_grp", nullptr));
    EXPECT_EQ(nullptr, root->OpenZarrGroup("missing_sub", nullptr));
  }

  // OpenZarrArray: valid array (and cached)
  auto sub_arr = root->OpenZarrArray("arr_ok", nullptr);
  ASSERT_NE(nullptr, sub_arr);
  EXPECT_EQ(sub_arr.get(), root->OpenZarrArray("arr_ok", nullptr).get());

  // OpenZarrArray: corrupt and missing
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->OpenZarrArray("corrupt_grp", nullptr));
    EXPECT_EQ(nullptr, root->OpenZarrArray("missing_arr", nullptr));
  }
}

TEST(ZarrV3GroupTest, CreateGroup) {
  ScopedTestDir test_dir("zarr_v3_grp_create");
  const std::string root_dir = test_dir.File("v3root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV3Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // 1. Read-only error
  root->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateGroup("sub", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Dataset not open in update mode"));
  }
  root->SetUpdatable(true);

  // 2. Invalid name error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateGroup(".zbad", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid group name"));
  }

  // 3. Valid CreateGroup
  auto sub = root->CreateGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub);
  EXPECT_EQ("sub", sub->GetName());

  // 4. Duplicate name error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateGroup("sub", nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));
  }
}

TEST(ZarrV3GroupTest, CreateMDArrayAllDataTypesAndCodecs) {
  ScopedTestDir test_dir("zarr_v3_all_types");
  const std::string root_dir = test_dir.File("v3root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV3Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  auto dim = root->CreateDimension("dim0", "X", "EAST", 16, nullptr);
  ASSERT_NE(nullptr, dim);
  const std::vector<std::shared_ptr<GDALDimension>> dims = {dim};

  // 1. All 14 supported numeric types
  for (GDALDataType eType : kZarrSupportedDataTypes) {
    const std::string name = std::string("arr_") + GDALGetDataTypeName(eType);
    auto arr = root->CreateMDArray(
        name, dims, GDALExtendedDataType::Create(eType), nullptr);
    ASSERT_NE(nullptr, arr)
        << "Failed for type: " << GDALGetDataTypeName(eType);
    EXPECT_EQ(name, arr->GetName());
    EXPECT_EQ(eType, arr->GetDataType().GetNumericDataType());
  }

  // 2. Unsupported data types
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "bad_cint16", dims,
                           GDALExtendedDataType::Create(GDT_CInt16), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unsupported data type"));

    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "bad_cint32", dims,
                           GDALExtendedDataType::Create(GDT_CInt32), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unsupported data type"));

    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "bad_str", dims,
                           GDALExtendedDataType::CreateString(8), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Unsupported data type"));
  }

  // 3. Error conditions
  root->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "ro_arr", dims,
                           GDALExtendedDataType::Create(GDT_Int32), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("Dataset not open in update mode"));
  }
  root->SetUpdatable(true);

  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    // Invalid name
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "a/b", dims, GDALExtendedDataType::Create(GDT_Int32),
                           nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("Invalid array name"));

    // Duplicate array name
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "arr_Byte", dims,
                           GDALExtendedDataType::Create(GDT_Int32), nullptr));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("already exists"));

    // FILTER option rejected in V3
    const char* const filter_opts[] = {"FILTER=DELTA", nullptr};
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              root->CreateMDArray("arr_filter", dims,
                                  GDALExtendedDataType::Create(GDT_Int32),
                                  filter_opts));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(), HasSubstr("FILTER option not supported"));

    // Unsupported compressor
    const char* const bad_comp_opts[] = {"COMPRESS=LZ4", nullptr};
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              root->CreateMDArray("arr_bad_comp", dims,
                                  GDALExtendedDataType::Create(GDT_Int32),
                                  bad_comp_opts));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
    EXPECT_THAT(CPLGetLastErrorMsg(),
                HasSubstr("COMPRESS = LZ4 not implemented with Zarr V3"));
  }

  // 4. Codec and layout options
  // CHUNK_MEMORY_LAYOUT=F, @ENDIAN=big, DIM_SEPARATOR=.
  {
    const char* const opts[] = {"CHUNK_MEMORY_LAYOUT=F", "@ENDIAN=big",
                                "DIM_SEPARATOR=.", nullptr};
    auto arr = root->CreateMDArray(
        "arr_f_big", dims, GDALExtendedDataType::Create(GDT_Int32), opts);
    ASSERT_NE(nullptr, arr);
  }

  // COMPRESS=GZIP
  {
    const char* const opts[] = {"COMPRESS=GZIP", "GZIP_LEVEL=1", nullptr};
    auto arr = root->CreateMDArray(
        "arr_gzip", dims, GDALExtendedDataType::Create(GDT_Float32), opts);
    ASSERT_NE(nullptr, arr);
  }

  // COMPRESS=ZSTD
  {
    const char* const opts[] = {"COMPRESS=ZSTD", "ZSTD_LEVEL=3",
                                "ZSTD_CHECKSUM=YES", nullptr};
    auto arr = root->CreateMDArray(
        "arr_zstd", dims, GDALExtendedDataType::Create(GDT_Int64), opts);
    ASSERT_NE(nullptr, arr);
  }

  // COMPRESS=BLOSC (shuffle = NONE, BYTE, BIT)
  {
    const char* const opts_none[] = {"COMPRESS=BLOSC", "BLOSC_SHUFFLE=NONE",
                                     nullptr};
    auto arr_none =
        root->CreateMDArray("arr_blosc_none", dims,
                            GDALExtendedDataType::Create(GDT_Int32), opts_none);
    ASSERT_NE(nullptr, arr_none);

    const char* const opts_byte[] = {"COMPRESS=BLOSC", "BLOSC_SHUFFLE=BYTE",
                                     nullptr};
    auto arr_byte =
        root->CreateMDArray("arr_blosc_byte", dims,
                            GDALExtendedDataType::Create(GDT_Int32), opts_byte);
    ASSERT_NE(nullptr, arr_byte);

    const char* const opts_bit[] = {"COMPRESS=BLOSC", "BLOSC_SHUFFLE=BIT",
                                    nullptr};
    auto arr_bit =
        root->CreateMDArray("arr_blosc_bit", dims,
                            GDALExtendedDataType::Create(GDT_Int32), opts_bit);
    ASSERT_NE(nullptr, arr_bit);

    // Invalid BLOSC_SHUFFLE
    const char* const opts_bad[] = {"COMPRESS=BLOSC", "BLOSC_SHUFFLE=BAD",
                                    nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "arr_blosc_bad", dims,
                           GDALExtendedDataType::Create(GDT_Int32), opts_bad));
  }

  dim.reset();
  root.reset();
  shared_res.reset();
  EXPECT_THAT(ReadVsiFile(test_dir.File("v3root/arr_zstd/zarr.json")),
              HasSubstr("zstd"));
}

// TODO: b/335317901 - Cover remaining ZarrV3Group directory and file I/O
// failure branches in zarr_v3_group.cpp.

}  // namespace
}  // namespace autotest2
