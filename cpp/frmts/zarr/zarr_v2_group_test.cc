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
// Tests for Zarr V2 group operations (zarr_v2_group.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_v2_group.cpp

#include <format>  // NOLINT
#include <memory>
#include <string>
#include <utility>
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
#include "third_party/gdal/port/cpl_json.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

using ::testing::HasSubstr;

TEST(ZarrV2GroupTest, CreateOnDiskExploreAndOpen) {
  ScopedTestDir test_dir("zarr_v2_disk");
  const std::string root_dir = test_dir.File("root");

  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);

  // 1. CreateOnDisk: fails if directory already exists
  VSIMkdir(root_dir.c_str(), 0755);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto fail_root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
    EXPECT_EQ(nullptr, fail_root);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  VSIRmdir(root_dir.c_str());

  // 2. CreateOnDisk: succeeds on clean path
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // 3. Add attribute, modify, Close() writes .zattrs & updates .zmetadata
  auto attr = root->CreateAttribute(
      "title", {}, GDALExtendedDataType::CreateString(0), nullptr);
  ASSERT_NE(nullptr, attr);
  const char* pszTitle = "V2 Dataset";
  EXPECT_TRUE(attr->Write(pszTitle));

  EXPECT_TRUE(std::static_pointer_cast<ZarrGroupBase>(root)->Close());
  EXPECT_THAT(
      ReadVsiFile(CPLFormFilenameSafe(root_dir.c_str(), ".zattrs", nullptr)),
      HasSubstr("V2 Dataset"));

  // 4. ExploreDirectory early return if dir contains .zarray directly
  const std::string leaf_dir = test_dir.File("leaf_arr");
  VSIMkdirRecursive(leaf_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(leaf_dir.c_str(), ".zarray", nullptr),
               R"json({
                 "zarr_format": 2,
                 "shape": [2],
                 "chunks": [2],
                 "dtype": "<i4",
                 "compressor": null,
                 "fill_value": null,
                 "order": "C",
                 "filters": null
               })json");
  auto leaf_res = ZarrSharedResource::Create(leaf_dir, /*bUpdatable=*/false);
  auto leaf_root = ZarrV2Group::Create(leaf_res, "", "/");
  leaf_root->SetDirectoryName(leaf_dir);
  EXPECT_TRUE(leaf_root->GetGroupNames(nullptr).empty());
  EXPECT_TRUE(leaf_root->GetMDArrayNames(nullptr).empty());

  // 5. ExploreDirectory on multi-content dir
  const std::string multi_dir = test_dir.File("multi");
  VSIMkdirRecursive(multi_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(multi_dir.c_str(), ".zgroup", nullptr),
               "{\"zarr_format\": 2}");
  WriteVsiFile(CPLFormFilenameSafe(multi_dir.c_str(), ".zattrs", nullptr),
               "{}");
  WriteVsiFile(CPLFormFilenameSafe(multi_dir.c_str(), "notes.txt", nullptr),
               "text");

  // Child array
  const std::string sub_arr_dir =
      CPLFormFilenameSafe(multi_dir.c_str(), "sub_arr", nullptr);
  VSIMkdirRecursive(sub_arr_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(sub_arr_dir.c_str(), ".zarray", nullptr),
               R"json({
                 "zarr_format": 2,
                 "shape": [4],
                 "chunks": [4],
                 "dtype": "<u1",
                 "compressor": null,
                 "fill_value": null,
                 "order": "C",
                 "filters": null
               })json");

  // Child group
  const std::string sub_grp_dir =
      CPLFormFilenameSafe(multi_dir.c_str(), "sub_grp", nullptr);
  VSIMkdirRecursive(sub_grp_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(sub_grp_dir.c_str(), ".zgroup", nullptr),
               "{\"zarr_format\": 2}");

  auto multi_res = ZarrSharedResource::Create(multi_dir, /*bUpdatable=*/false);
  auto multi_root = ZarrV2Group::Create(multi_res, "", "/");
  multi_root->SetDirectoryName(multi_dir);

  auto grp_names = multi_root->GetGroupNames(nullptr);
  auto arr_names = multi_root->GetMDArrayNames(nullptr);
  EXPECT_EQ(1, grp_names.size());
  EXPECT_EQ("sub_grp", grp_names[0]);
  EXPECT_EQ(1, arr_names.size());
  EXPECT_EQ("sub_arr", arr_names[0]);

  // OpenZarrArray and OpenZarrGroup (valid, cached, and missing)
  auto opened_arr = multi_root->OpenZarrArray("sub_arr", nullptr);
  ASSERT_NE(nullptr, opened_arr);
  EXPECT_EQ(opened_arr.get(),
            multi_root->OpenZarrArray("sub_arr", nullptr).get());
  EXPECT_EQ(nullptr, multi_root->OpenZarrArray("missing", nullptr));

  auto opened_grp = multi_root->OpenZarrGroup("sub_grp", nullptr);
  ASSERT_NE(nullptr, opened_grp);
  EXPECT_EQ(opened_grp.get(),
            multi_root->OpenZarrGroup("sub_grp", nullptr).get());
  EXPECT_EQ(nullptr, multi_root->OpenZarrGroup("missing", nullptr));
}

TEST(ZarrV2GroupTest, InitFromZMetadata) {
  ScopedTestDir test_dir("zarr_v2_zmeta");
  const std::string root_dir = test_dir.File("root");

  // 1. Non-object metadata early return
  {
    auto res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
    auto grp = ZarrV2Group::Create(res, "", "/");
    CPLJSONDocument doc;
    doc.LoadMemory(R"json({"metadata": "not_an_object"})json");
    grp->InitFromZMetadata(doc.GetRoot());
    EXPECT_TRUE(grp->GetGroupNames(nullptr).empty());
  }

  // 2. Rich .zmetadata
  const std::string zmeta_json = R"json({
    "metadata": {
      ".zattrs": {
        "dataset_name": "test_dataset"
      },
      "x/.zarray": {
        "zarr_format": 2,
        "shape": [10],
        "chunks": [10],
        "dtype": "<f8",
        "compressor": null,
        "fill_value": null,
        "order": "C",
        "filters": null
      },
      "x/.zattrs": {
        "_ARRAY_DIMENSIONS": ["x"]
      },
      "grid/.zarray": {
        "zarr_format": 2,
        "shape": [5, 10],
        "chunks": [5, 10],
        "dtype": "<i4",
        "compressor": null,
        "fill_value": null,
        "order": "C",
        "filters": null
      },
      "grid/.zattrs": {
        "_ARRAY_DIMENSIONS": ["y", "x"],
        "unit": "meters"
      },
      "sub/.zgroup": {
        "zarr_format": 2
      },
      "sub/.zattrs": {
        "group_attr": 123
      },
      "sub/raw/.zarray": {
        "zarr_format": 2,
        "shape": [3],
        "chunks": [3],
        "dtype": "|u1",
        "compressor": null,
        "fill_value": null,
        "order": "C",
        "filters": null
      }
    }
  })json";

  auto res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
  auto grp = ZarrV2Group::Create(res, "", "/");
  grp->SetDirectoryName(root_dir);
  CPLJSONDocument doc;
  ASSERT_TRUE(doc.LoadMemory(zmeta_json.c_str()));
  grp->InitFromZMetadata(doc.GetRoot());

  // Check root attributes
  auto title_attr = grp->GetAttribute("dataset_name");
  ASSERT_NE(nullptr, title_attr);
  EXPECT_STREQ("test_dataset", title_attr->ReadAsString());

  // Check 1D coordinate indexing variable dimension discovery
  auto dim_x = grp->GetDimensions(nullptr);
  EXPECT_FALSE(dim_x.empty());

  // Check arrays
  auto arr_grid = grp->OpenMDArray("grid", nullptr);
  ASSERT_NE(nullptr, arr_grid);
  EXPECT_EQ(2, arr_grid->GetDimensionCount());

  // Check subgroup and nested array
  auto grp_sub = grp->OpenGroup("sub", nullptr);
  ASSERT_NE(nullptr, grp_sub);
  auto sub_attr = grp_sub->GetAttribute("group_attr");
  ASSERT_NE(nullptr, sub_attr);
  EXPECT_EQ(123, sub_attr->ReadAsInt());

  auto arr_raw = grp_sub->OpenMDArray("raw", nullptr);
  ASSERT_NE(nullptr, arr_raw);
}

TEST(ZarrV2GroupTest, InitFromZGroupNCZarr) {
  ScopedTestDir test_dir("zarr_v2_nczarr");
  const std::string root_dir = test_dir.File("root");
  VSIMkdirRecursive(root_dir.c_str(), 0755);

  // 1. Updatable group fails with NCZarr
  {
    auto res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
    auto grp = ZarrV2Group::Create(res, "", "/");
    grp->SetUpdatable(true);
    CPLJSONDocument doc;
    doc.LoadMemory(R"json({"_NCZARR_GROUP": {}})json");
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(grp->InitFromZGroup(doc.GetRoot()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. Read-only NCZarr root group with superblock, dims, vars, groups
  const std::string x_arr_dir =
      CPLFormFilenameSafe(root_dir.c_str(), "x", nullptr);
  VSIMkdirRecursive(x_arr_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(x_arr_dir.c_str(), ".zarray", nullptr),
               R"json({
                 "zarr_format": 2,
                 "shape": [4],
                 "chunks": [4],
                 "dtype": "<f4",
                 "compressor": null,
                 "fill_value": null,
                 "order": "C",
                 "filters": null
               })json");

  const std::string sub_dir =
      CPLFormFilenameSafe(root_dir.c_str(), "sub", nullptr);
  VSIMkdirRecursive(sub_dir.c_str(), 0755);
  WriteVsiFile(CPLFormFilenameSafe(sub_dir.c_str(), ".zgroup", nullptr),
               R"json({
                 "zarr_format": 2,
                 "_NCZARR_GROUP": {
                   "dims": {},
                   "vars": [],
                   "groups": []
                 }
               })json");

  const std::string root_zgroup_json = R"json({
    "zarr_format": 2,
    "_NCZARR_SUPERBLOCK": {"version": 1},
    "_NCZARR_GROUP": {
      "dims": {
        "x": 4,
        "bad/dim": 2,
        "zero_dim": 0
      },
      "vars": ["x", "v"],
      "groups": ["sub"]
    }
  })json";
  WriteVsiFile(CPLFormFilenameSafe(root_dir.c_str(), ".zgroup", nullptr),
               root_zgroup_json);

  auto res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
  auto grp = ZarrV2Group::Create(res, "", "/");
  grp->SetDirectoryName(root_dir);
  CPLJSONDocument doc;
  ASSERT_TRUE(doc.LoadMemory(root_zgroup_json.c_str()));
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(grp->InitFromZGroup(doc.GetRoot()));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // Dimensions
  auto dims = grp->GetDimensions(nullptr);
  EXPECT_FALSE(dims.empty());

  // Groups and Arrays
  auto grp_names = grp->GetGroupNames(nullptr);
  EXPECT_EQ(1, grp_names.size());
  EXPECT_EQ("sub", grp_names[0]);

  auto arr_names = grp->GetMDArrayNames(nullptr);
  EXPECT_FALSE(arr_names.empty());

  // 3. Opening child group directly walks up to parent group
  auto sub_grp_direct = grp->OpenZarrGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub_grp_direct);

  // 4. Standalone child group opened with no parent walks up to parent group on
  // disk
  {
    auto standalone_sub = ZarrV2Group::Create(res, "", "/");
    standalone_sub->SetDirectoryName(sub_dir);
    CPLJSONDocument sub_doc;
    ASSERT_TRUE(
        sub_doc.Load(CPLFormFilenameSafe(sub_dir.c_str(), ".zgroup", nullptr)));
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(standalone_sub->InitFromZGroup(sub_doc.GetRoot()));
    EXPECT_EQ("sub", standalone_sub->GetName());
  }
}

TEST(ZarrV2GroupTest, CreateGroup) {
  ScopedTestDir test_dir("zarr_v2_grp_create");
  const std::string root_dir = test_dir.File("root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // Read-only error
  root->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateGroup("sub", nullptr));
  }
  root->SetUpdatable(true);

  // Invalid name
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateGroup(".zbad", nullptr));
  }

  // Valid create
  auto sub = root->CreateGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub);

  // Duplicate error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateGroup("sub", nullptr));
  }
}

TEST(ZarrV2GroupTest, CreateMDArrayTypesCompressorsAndFilters) {
  ScopedTestDir test_dir("zarr_v2_all_features");
  const std::string root_dir = test_dir.File("root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  auto dim = root->CreateDimension("dim0", "X", "EAST", 8, nullptr);
  ASSERT_NE(nullptr, dim);
  const std::vector<std::shared_ptr<GDALDimension>> dims = {dim};

  // 1. All 14 supported numeric types
  for (GDALDataType eType : kZarrSupportedDataTypes) {
    const std::string name = std::string("arr_") + GDALGetDataTypeName(eType);
    auto arr = root->CreateMDArray(
        name, dims, GDALExtendedDataType::Create(eType), nullptr);
    ASSERT_NE(nullptr, arr) << "Failed for: " << GDALGetDataTypeName(eType);
  }

  // 2. Unsupported numeric types
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "bad_cint16", dims,
                           GDALExtendedDataType::Create(GDT_CInt16), nullptr));
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "bad_cint32", dims,
                           GDALExtendedDataType::Create(GDT_CInt32), nullptr));
  }

  // 3. String types
  // Unlimited string (error)
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "str_unlimited", dims,
                           GDALExtendedDataType::CreateString(0), nullptr));
  }
  // ASCII fixed string
  {
    const char* const opts[] = {"STRING_FORMAT=ASCII", nullptr};
    auto arr = root->CreateMDArray(
        "str_ascii", dims, GDALExtendedDataType::CreateString(10), opts);
    ASSERT_NE(nullptr, arr);
  }
  // UNICODE fixed string
  {
    const char* const opts[] = {"STRING_FORMAT=UNICODE", nullptr};
    auto arr = root->CreateMDArray(
        "str_unicode", dims, GDALExtendedDataType::CreateString(10), opts);
    ASSERT_NE(nullptr, arr);
  }

  // 4. Compound types
  {
    std::vector<std::unique_ptr<GDALEDTComponent>> comps;
    comps.emplace_back(std::make_unique<GDALEDTComponent>(
        "val", 0, GDALExtendedDataType::Create(GDT_Float32)));
    comps.emplace_back(std::make_unique<GDALEDTComponent>(
        "tag", sizeof(float), GDALExtendedDataType::CreateString(8)));
    auto comp_type = GDALExtendedDataType::Create(
        "my_compound", sizeof(float) + sizeof(char*), std::move(comps));

    auto arr = root->CreateMDArray("arr_compound", dims, comp_type, nullptr);
    ASSERT_NE(nullptr, arr);

    // Invalid compound containing unsupported GDT_CInt16
    std::vector<std::unique_ptr<GDALEDTComponent>> bad_comps;
    bad_comps.emplace_back(std::make_unique<GDALEDTComponent>(
        "bad", 0, GDALExtendedDataType::Create(GDT_CInt16)));
    auto bad_comp_type = GDALExtendedDataType::Create("bad_comp", sizeof(int),
                                                      std::move(bad_comps));
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray("arr_bad_compound", dims,
                                           bad_comp_type, nullptr));
  }

  // 5. Compressors
  {
    const char* const opts_gzip[] = {"COMPRESS=GZIP", nullptr};
    auto arr_gzip = root->CreateMDArray(
        "arr_gzip", dims, GDALExtendedDataType::Create(GDT_Int32), opts_gzip);
    ASSERT_NE(nullptr, arr_gzip);

    const char* const opts_blosc[] = {"COMPRESS=BLOSC", "BLOSC_SHUFFLE=BYTE",
                                      nullptr};
    auto arr_blosc = root->CreateMDArray(
        "arr_blosc", dims, GDALExtendedDataType::Create(GDT_Int32), opts_blosc);
    ASSERT_NE(nullptr, arr_blosc);

    const char* const opts_zstd[] = {"COMPRESS=ZSTD", nullptr};
    auto arr_zstd = root->CreateMDArray(
        "arr_zstd", dims, GDALExtendedDataType::Create(GDT_Int32), opts_zstd);
    ASSERT_NE(nullptr, arr_zstd);

    const char* const opts_bad[] = {"COMPRESS=UNKNOWN_COMP", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "arr_bad_comp", dims,
                           GDALExtendedDataType::Create(GDT_Int32), opts_bad));
  }

  // 6. Filters
  // Loop FILTER=DELTA across all 14 numeric types to cover switch cases
  for (GDALDataType eType : kZarrSupportedDataTypes) {
    const std::string name = std::string("delta_") + GDALGetDataTypeName(eType);
    const char* const opts[] = {"FILTER=DELTA", nullptr};
    auto arr = root->CreateMDArray(name, dims,
                                   GDALExtendedDataType::Create(eType), opts);
    ASSERT_NE(nullptr, arr)
        << "DELTA filter failed for: " << GDALGetDataTypeName(eType);
  }

  // FILTER=DELTA on string type without DELTA_DTYPE fails
  {
    const char* const opts[] = {"FILTER=DELTA", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr,
              root->CreateMDArray("delta_str_fail", dims,
                                  GDALExtendedDataType::CreateString(8), opts));
  }

  // FILTER=DELTA with explicit DELTA_DTYPE option
  {
    const char* const opts[] = {"FILTER=DELTA", "DELTA_DTYPE=<i4", nullptr};
    auto arr =
        root->CreateMDArray("delta_explicit_dtype", dims,
                            GDALExtendedDataType::Create(GDT_Int32), opts);
    ASSERT_NE(nullptr, arr);
  }

  // Unknown filter error
  {
    const char* const opts[] = {"FILTER=NON_EXISTENT", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "bad_filter", dims,
                           GDALExtendedDataType::Create(GDT_Int32), opts));
  }

  // 7. General options: CHUNK_MEMORY_LAYOUT=F, DIM_SEPARATOR=/
  {
    const char* const opts[] = {"CHUNK_MEMORY_LAYOUT=F", "DIM_SEPARATOR=/",
                                nullptr};
    auto arr = root->CreateMDArray(
        "arr_f_slash", dims, GDALExtendedDataType::Create(GDT_Int32), opts);
    ASSERT_NE(nullptr, arr);
  }

  // Existing directory collision error
  {
    const std::string existing_arr_dir =
        CPLFormFilenameSafe(root_dir.c_str(), "existing_collision", nullptr);
    VSIMkdir(existing_arr_dir.c_str(), 0755);
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "existing_collision", dims,
                           GDALExtendedDataType::Create(GDT_Int32), nullptr));
  }

  // Invalid blocksize error
  {
    const char* const opts[] = {"BLOCKSIZE=1,2,3", nullptr};
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_EQ(nullptr, root->CreateMDArray(
                           "bad_blocksize", dims,
                           GDALExtendedDataType::Create(GDT_Int32), opts));
  }

  // Flush and verify written .zarray files contain expected metadata
  EXPECT_TRUE(std::static_pointer_cast<ZarrGroupBase>(root)->Close());
  EXPECT_THAT(ReadVsiFile(test_dir.File("root/arr_gzip/.zarray")),
              HasSubstr("gzip"));
  EXPECT_THAT(ReadVsiFile(test_dir.File("root/arr_CFloat16/.zarray")),
              HasSubstr("\"<c4\""));
  EXPECT_THAT(ReadVsiFile(test_dir.File("root/arr_CFloat32/.zarray")),
              HasSubstr("\"<c8\""));

  // Verify round-trip reopening across numeric types. GDT_CFloat16 ("<c4")
  // fails in the file-static ParseDtype() in zarr_v2_array.cpp (called from
  // ZarrV2Group::LoadArray), which omits chType == 'c' && nBytes == 4.
  auto ro_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/false);
  auto ro_root = ZarrV2Group::Create(ro_res, "", "/");
  ro_root->SetDirectoryName(root_dir);
  for (GDALDataType eType : kZarrSupportedDataTypes) {
    const std::string name = std::format("arr_{}", GDALGetDataTypeName(eType));
    if (eType == GDT_CFloat16) {
      const ::WithQuietHandler quiet;
      CPLErrorReset();
      EXPECT_EQ(nullptr, ro_root->OpenZarrArray(name, nullptr));
      EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
      EXPECT_EQ(CPLE_AppDefined, CPLGetLastErrorNo());
      EXPECT_THAT(CPLGetLastErrorMsg(),
                  HasSubstr("Invalid or unsupported format for dtype: <c4"));
      CPLErrorReset();
    } else {
      auto reopened = ro_root->OpenZarrArray(name, nullptr);
      ASSERT_NE(nullptr, reopened)
          << "Reopen failed for: " << GDALGetDataTypeName(eType);
      EXPECT_EQ(eType, reopened->GetDataType().GetNumericDataType());
    }
  }
}

// TODO: b/335317901 - Pins upstream quirk: zarr_v2_group.cpp writes "<c4" for
// GDT_CFloat16 (following NumPy's total-byte-width convention: 4 bytes = two
// 2-byte float16 components, whereas "<c8" is GDT_CFloat32), but NumPy and
// zarr-python define no 'c4' dtype and file-static ParseDtype() in
// third_party/gdal/frmts/zarr/zarr_v2_array.cpp only handles chType == 'c' for
// nBytes == 8 and 16, so round-trip reopening fails. An upstream fix could
// either add nBytes == 4 support to ParseDtype() and GetAlignment() in
// zarr_v2_array.cpp (plus COMPLEX_IEEEFP byte-swapping in EncodeElt()/
// DecodeSourceElt() in zarr_array.cpp) or reject/promote GDT_CFloat16 in
// zarr_v2_group.cpp.
// TODO: b/335317901 - Test additional V2 group features.

}  // namespace
}  // namespace autotest2
