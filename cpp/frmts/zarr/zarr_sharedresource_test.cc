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
// Tests for ZarrSharedResource (zarr_sharedresource.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_sharedresource.cpp

#include <format>  // NOLINT
#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

class ZarrSharedResourceTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_Zarr(); }
};

TEST_F(ZarrSharedResourceTest, BasicAccessors) {
  ScopedTestDir test_dir("zarr_sr");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path() + "/", /*bUpdatable=*/true);
  ASSERT_NE(nullptr, shared_res);
  EXPECT_TRUE(shared_res->IsUpdatable());
  EXPECT_NE(nullptr, shared_res->GetPAM());
}

TEST_F(ZarrSharedResourceTest, ZMetadataManipulationAndSaveOnClose) {
  ScopedTestDir test_dir("zarr_sr");
  const std::string zmetadata_path = test_dir.File(".zmetadata");

  {
    auto shared_res =
        ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);
    shared_res->EnableZMetadata();

    CPLJSONObject item1;
    item1.Add("shape", 10);
    shared_res->SetZMetadataItem(test_dir.File("arr/.zarray"), item1);

    CPLJSONObject item2;
    item2.Add("zarr_format", 2);
    shared_res->SetZMetadataItem(test_dir.File("grp/.zgroup"), item2);

    // Rename
    shared_res->RenameZMetadataRecursive(test_dir.File("arr"),
                                         test_dir.File("renamed_arr"));

    // Delete
    shared_res->DeleteZMetadataItemRecursive(test_dir.File("grp"));

    // shared_res goes out of scope, destructor should save .zmetadata
  }

  // Verify saved .zmetadata file content
  const std::string saved_json = ReadVsiFile(zmetadata_path);
  EXPECT_FALSE(saved_json.empty());
  CPLJSONDocument doc;
  ASSERT_TRUE(doc.LoadMemory(saved_json));
  auto meta = doc.GetRoot()["metadata"];
  EXPECT_TRUE(meta["renamed_arr/.zarray"].IsValid());
  EXPECT_FALSE(meta["arr/.zarray"].IsValid());
  EXPECT_FALSE(meta["grp/.zgroup"].IsValid());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupEmptyDir) {
  ScopedTestDir test_dir("zarr_sr");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  EXPECT_EQ(nullptr, shared_res->GetRootGroup());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZGroupV2) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File(".zgroup"), R"json({"zarr_format": 2})json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
  EXPECT_EQ("/", rg->GetFullName());

  // Cached root group
  EXPECT_EQ(rg, shared_res->GetRootGroup());
  shared_res->SetRootGroup(nullptr);
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZGroupV2InvalidJson) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File(".zgroup"), "not valid json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, shared_res->GetRootGroup());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZMetadataV2) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File(".zmetadata"), R"json({
    "zarr_consolidated_format": 1,
    "metadata": {
      ".zgroup": {"zarr_format": 2}
    }
  })json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZMetadataV2DisabledOption) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File(".zmetadata"), R"json({
    "zarr_consolidated_format": 1,
    "metadata": {
      ".zgroup": {"zarr_format": 2}
    }
  })json");
  WriteVsiFile(test_dir.File(".zgroup"), R"json({"zarr_format": 2})json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  char* options[] = {const_cast<char*>("USE_ZMETADATA=NO"), nullptr};
  shared_res->SetOpenOptions(options);

  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZMetadataV2InvalidJson) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File(".zmetadata"), "invalid json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, shared_res->GetRootGroup());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupV2ZArray) {
  ScopedTestDir test_dir("zarr_sr");
  const std::string arr_dir = test_dir.File("my_array");
  VSIMkdir(arr_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(arr_dir.c_str(), ".zarray", nullptr),
               R"json({
                 "zarr_format": 2,
                 "shape": [4, 4],
                 "chunks": [2, 2],
                 "dtype": "<u1",
                 "compressor": null,
                 "fill_value": 0,
                 "order": "C",
                 "filters": null
               })json");

  auto shared_res = ZarrSharedResource::Create(arr_dir, /*bUpdatable=*/false);
  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
  auto arr = rg->OpenMDArray("my_array");
  ASSERT_NE(nullptr, arr);
  EXPECT_EQ(2, arr->GetDimensionCount());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupV2ZArrayNCZarr) {
  ScopedTestDir test_dir("zarr_sr");
  const std::string arr_dir = test_dir.File("nc_arr");
  VSIMkdir(arr_dir.c_str(), 0755);
  WriteVsiFile(test_dir.File(".zgroup"),
               R"json({
                 "zarr_format": 2,
                 "_NCZARR_GROUP": {
                   "dims": {"d0": 4, "d1": 4}
                 }
               })json");
  WriteVsiFile(CPLFormFilenameSafe(arr_dir.c_str(), ".zarray", nullptr),
               R"json({
                 "zarr_format": 2,
                 "shape": [4, 4],
                 "chunks": [2, 2],
                 "dtype": "<u1",
                 "compressor": null,
                 "fill_value": 0,
                 "order": "C",
                 "filters": null,
                 "_NCZARR_ARRAY": {
                   "dimrefs": ["/d0", "/d1"]
                 }
               })json");

  auto shared_res = ZarrSharedResource::Create(arr_dir, /*bUpdatable=*/false);
  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupV2ZArrayInvalidJson) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File(".zarray"), "not json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, shared_res->GetRootGroup());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZarrJsonV3Group) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File("zarr.json"),
               R"json({"zarr_format": 3, "node_type": "group"})json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
  EXPECT_EQ("/", rg->GetFullName());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZarrJsonV3Array) {
  ScopedTestDir test_dir("zarr_sr");
  const std::string arr_dir = test_dir.File("v3_array");
  VSIMkdir(arr_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(arr_dir.c_str(), "zarr.json", nullptr),
               R"json({
                 "zarr_format": 3,
                 "node_type": "array",
                 "shape": [4, 4],
                 "data_type": "uint8",
                 "chunk_grid": {
                   "name": "regular",
                   "configuration": {"chunk_shape": [2, 2]}
                 },
                 "chunk_key_encoding": {"name": "default"},
                 "fill_value": 0,
                 "codecs": [{"name": "bytes", "configuration": {"endian": "little"}}]
               })json");

  auto shared_res = ZarrSharedResource::Create(arr_dir, /*bUpdatable=*/false);
  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
  auto arr = rg->OpenMDArray("v3_array");
  ASSERT_NE(nullptr, arr);
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZarrJsonV3InvalidFormat) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File("zarr.json"),
               R"json({"zarr_format": 2, "node_type": "group"})json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, shared_res->GetRootGroup());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZarrJsonV3InvalidNodeType) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File("zarr.json"),
               R"json({"zarr_format": 3, "node_type": "unknown_type"})json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, shared_res->GetRootGroup());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupZarrJsonV3InvalidJson) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File("zarr.json"), "invalid json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_EQ(nullptr, shared_res->GetRootGroup());
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrSharedResourceTest, OpenRootGroupImplicitV3Subdirectory) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File("sub/zarr.json"),
               R"json({"zarr_format": 3, "node_type": "group"})json");

  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  auto rg = shared_res->GetRootGroup();
  ASSERT_NE(nullptr, rg);
}

TEST_F(ZarrSharedResourceTest, AddAndRemoveArrayInLoading) {
  ScopedTestDir test_dir("zarr_sr");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);

  const std::string array_path = test_dir.File("arr/.zarray");
  EXPECT_TRUE(shared_res->AddArrayInLoading(array_path));

  // Adding the same array path recursively fails
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(shared_res->AddArrayInLoading(array_path));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  shared_res->RemoveArrayInLoading(array_path);
  EXPECT_TRUE(shared_res->AddArrayInLoading(array_path));
  shared_res->RemoveArrayInLoading(array_path);

  // Exceeding 32 nested arrays fails
  std::vector<std::string> paths;
  for (int i = 0; i < 32; ++i) {
    paths.push_back(test_dir.File(std::format("arr_{}/.zarray", i)));
    EXPECT_TRUE(shared_res->AddArrayInLoading(paths.back()));
  }
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(
        shared_res->AddArrayInLoading(test_dir.File("arr_33/.zarray")));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  for (const auto& p : paths) {
    shared_res->RemoveArrayInLoading(p);
  }
}

TEST_F(ZarrSharedResourceTest, SetFilenameAdderHelper) {
  ScopedTestDir test_dir("zarr_sr");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);
  const std::string array_path = test_dir.File("adder/.zarray");

  {
    ZarrSharedResource::SetFilenameAdder adder(shared_res, array_path);
    EXPECT_TRUE(adder.ok());

    // Nested same array fails
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    ZarrSharedResource::SetFilenameAdder nested(shared_res, array_path);
    EXPECT_FALSE(nested.ok());
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Once adder destructs, it can be added again
  ZarrSharedResource::SetFilenameAdder adder2(shared_res, array_path);
  EXPECT_TRUE(adder2.ok());
}

TEST_F(ZarrSharedResourceTest, UpdateDimensionSize) {
  ScopedTestDir test_dir("zarr_sr");
  WriteVsiFile(test_dir.File(".zgroup"), R"json({"zarr_format": 2})json");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);

  auto dim = std::make_shared<ZarrDimension>(
      shared_res, std::weak_ptr<ZarrGroupBase>(), "/", "dim", "", "", 10);
  shared_res->UpdateDimensionSize(dim);

  // UpdateDimensionSize on unopenable root group logs error
  ScopedTestDir empty_dir("empty_sr");
  auto shared_res_empty =
      ZarrSharedResource::Create(empty_dir.path(), /*bUpdatable=*/true);
  const ::WithQuietHandler quiet;
  CPLErrorReset();
  shared_res_empty->UpdateDimensionSize(dim);
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

// TODO: b/335317901 - Cover NCZarr root-array parent .zgroup loading and
// Kerchunk Parquet cache error propagation in
// ZarrSharedResource::OpenRootGroup.

}  // namespace
}  // namespace autotest2
