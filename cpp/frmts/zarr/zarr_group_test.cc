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
// Tests for Zarr group base operations (zarr_group.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_group.cpp

#include <memory>
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_multidim.h"
#include "third_party/gdal/gcore/gdal_priv.h"
#include "third_party/gdal/port/cpl_conv.h"
#include "third_party/gdal/port/cpl_error.h"
#include "third_party/gdal/port/cpl_vsi.h"

namespace autotest2 {
namespace {

TEST(ZarrGroupTest, IsValidObjectName) {
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName(""));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName("."));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName(".."));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName("a/b"));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName("a\\b"));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName("a:b"));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName(".zgroup"));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName(".zarray"));
  EXPECT_FALSE(ZarrGroupBase::IsValidObjectName(".zattrs"));

  EXPECT_TRUE(ZarrGroupBase::IsValidObjectName("group"));
  EXPECT_TRUE(ZarrGroupBase::IsValidObjectName("var_1"));
  EXPECT_TRUE(ZarrGroupBase::IsValidObjectName("dataset.tif"));
}

TEST(ZarrGroupTest, CreateAndDeleteAttributes) {
  ScopedTestDir test_dir("zarr_grp_attr");
  const std::string root_dir = test_dir.File("root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // 1. Read-only error on CreateAttribute
  root->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto attr = root->CreateAttribute(
        "ro_attr", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    EXPECT_EQ(nullptr, attr);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  root->SetUpdatable(true);

  // 2. 2D attribute error on CreateAttribute
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto attr = root->CreateAttribute(
        "2d_attr", {2, 2}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
    EXPECT_EQ(nullptr, attr);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. Valid CreateAttribute (0D scalar and 1D vector)
  auto attr_scalar = root->CreateAttribute(
      "scalar_int", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, attr_scalar);
  int nVal = 42;
  EXPECT_TRUE(attr_scalar->Write(&nVal, 1));

  auto attr_vec = root->CreateAttribute(
      "vec_double", {2}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  ASSERT_NE(nullptr, attr_vec);
  const double adfVals[2] = {1.5, 2.5};
  EXPECT_TRUE(attr_vec->Write(adfVals, 2));

  // 4. GetAttribute and GetAttributes
  auto got_scalar = root->GetAttribute("scalar_int");
  ASSERT_NE(nullptr, got_scalar);
  EXPECT_EQ("scalar_int", got_scalar->GetName());

  auto all_attrs = root->GetAttributes(nullptr);
  EXPECT_EQ(2, all_attrs.size());

  // 5. DeleteAttribute: read-only error
  root->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteAttribute("scalar_int"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  root->SetUpdatable(true);

  // 6. DeleteAttribute: non-existent attribute
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteAttribute("missing_attr"));
  }

  // 7. DeleteAttribute: valid delete
  EXPECT_TRUE(root->DeleteAttribute("scalar_int"));
  EXPECT_EQ(nullptr, root->GetAttribute("scalar_int"));
  EXPECT_EQ(1, root->GetAttributes(nullptr).size());
}

TEST(ZarrGroupTest, CreateRenameAndUpdateDimensions) {
  ScopedTestDir test_dir("zarr_grp_dim");
  const std::string root_dir = test_dir.File("root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // 1. CreateDimension: empty name error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto dim = root->CreateDimension("", "X", "EAST", 10, nullptr);
    EXPECT_EQ(nullptr, dim);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 2. CreateDimension: valid dimensions
  auto dim_x = root->CreateDimension("x", "X", "EAST", 10, nullptr);
  ASSERT_NE(nullptr, dim_x);
  EXPECT_EQ("x", dim_x->GetName());
  EXPECT_EQ(10, dim_x->GetSize());

  auto dim_y = root->CreateDimension("y", "Y", "NORTH", 20, nullptr);
  ASSERT_NE(nullptr, dim_y);

  // 3. CreateDimension: duplicate name error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    auto dup = root->CreateDimension("x", "X", "EAST", 10, nullptr);
    EXPECT_EQ(nullptr, dup);
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. RenameDimension: collision with existing name fails
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(dim_x->Rename("y"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. RenameDimension: valid rename
  EXPECT_TRUE(dim_x->Rename("x_renamed"));
  EXPECT_EQ("x_renamed", dim_x->GetName());

  // 6. GetDimensions
  auto dims = root->GetDimensions(nullptr);
  EXPECT_EQ(2, dims.size());

  // 7. Lazy GetDimensions discovery from arrays
  {
    const std::string disc_dir = test_dir.File("disc_root");
    VSIMkdirRecursive(disc_dir.c_str(), 0755);
    WriteVsiFile(CPLFormFilenameSafe(disc_dir.c_str(), ".zgroup", nullptr),
                 "{\"zarr_format\": 2}");
    const std::string arr_dir =
        CPLFormFilenameSafe(disc_dir.c_str(), "arr", nullptr);
    VSIMkdirRecursive(arr_dir.c_str(), 0755);
    WriteVsiFile(CPLFormFilenameSafe(arr_dir.c_str(), ".zarray", nullptr),
                 R"json({
                   "zarr_format": 2,
                   "shape": [4],
                   "chunks": [4],
                   "dtype": "<i4",
                   "compressor": null,
                   "fill_value": null,
                   "order": "C",
                   "filters": null
                 })json");
    WriteVsiFile(CPLFormFilenameSafe(arr_dir.c_str(), ".zattrs", nullptr),
                 R"json({"_ARRAY_DIMENSIONS": ["dim0"]})json");

    auto disc_res = ZarrSharedResource::Create(disc_dir, /*bUpdatable=*/false);
    auto disc_root = ZarrV2Group::Create(disc_res, "", "/");
    disc_root->SetDirectoryName(disc_dir);
    auto disc_dims = disc_root->GetDimensions(nullptr);
    EXPECT_FALSE(disc_dims.empty());
  }

  // 8. UpdateDimensionSize: recursive update across subgroup and array
  auto sub = root->CreateGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub);
  auto sub_zarr = std::dynamic_pointer_cast<ZarrGroupBase>(sub);
  ASSERT_NE(nullptr, sub_zarr);

  auto arr = sub_zarr->CreateMDArray(
      "arr_in_sub", {dim_y}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, arr);

  // Resize dim_y and notify shared resource
  shared_res->SetRootGroup(root);
  auto updated_dim = std::make_shared<ZarrDimension>(
      shared_res, root, "/", "y", "HORIZONTAL_Y", "NORTH", 35);
  shared_res->UpdateDimensionSize(updated_dim);
  EXPECT_EQ(35, dim_y->GetSize());
  EXPECT_EQ(35, arr->GetDimensions()[0]->GetSize());
}

TEST(ZarrGroupTest, CreateAndDeleteGroupsAndNotifications) {
  ScopedTestDir test_dir("zarr_grp_del");
  const std::string root_dir = test_dir.File("root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // 1. DeleteGroup: read-only error
  root->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteGroup("any"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  root->SetUpdatable(true);

  // 2. DeleteGroup: path traversal error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteGroup("a/../b"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. DeleteGroup: non-existent group
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteGroup("missing_sub"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Create hierarchy: root -> sub -> nested child group, child array, attr,
  // dim
  auto sub = root->CreateGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub);
  auto sub_zarr = std::dynamic_pointer_cast<ZarrGroupBase>(sub);
  ASSERT_NE(nullptr, sub_zarr);

  auto nested = sub->CreateGroup("nested", nullptr);
  ASSERT_NE(nullptr, nested);

  auto dim = sub->CreateDimension("sub_dim", "X", "EAST", 5, nullptr);
  ASSERT_NE(nullptr, dim);

  auto arr = sub_zarr->CreateMDArray(
      "sub_arr", {dim}, GDALExtendedDataType::Create(GDT_Byte), nullptr);
  ASSERT_NE(nullptr, arr);

  auto attr = sub->CreateAttribute(
      "sub_attr", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, attr);

  // Verify group names
  auto grp_names = root->GetGroupNames(nullptr);
  EXPECT_EQ(1, grp_names.size());
  EXPECT_EQ("sub", grp_names[0]);

  // 5. DeleteGroup("sub") exercises NotifyChildrenOfDeletion() on all 4 types
  EXPECT_TRUE(root->DeleteGroup("sub"));
  EXPECT_TRUE(root->GetGroupNames(nullptr).empty());

  // 6. Verify deleted sub handle fails subsequent calls via
  // CheckValidAndErrorOutIfNot
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_TRUE(sub->GetGroupNames(nullptr).empty());
    EXPECT_TRUE(sub->GetMDArrayNames(nullptr).empty());
    EXPECT_EQ(nullptr, sub->CreateGroup("x", nullptr));
    EXPECT_FALSE(sub->DeleteGroup("x", nullptr));
    EXPECT_EQ(nullptr,
              sub->CreateAttribute(
                  "x", {}, GDALExtendedDataType::Create(GDT_Int32), nullptr));
    EXPECT_FALSE(sub->DeleteAttribute("x", nullptr));
    EXPECT_TRUE(sub->GetDimensions(nullptr).empty());
    EXPECT_EQ(nullptr, sub->CreateDimension("x", "X", "EAST", 1, nullptr));
    EXPECT_FALSE(sub_zarr->DeleteMDArray("x", nullptr));
    EXPECT_FALSE(sub_zarr->Rename("new_name"));
  }
}

TEST(ZarrGroupTest, DeleteAndRenameMDArray) {
  ScopedTestDir test_dir("zarr_grp_arr");
  const std::string root_dir = test_dir.File("root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  auto dim = root->CreateDimension("d", "X", "EAST", 8, nullptr);
  ASSERT_NE(nullptr, dim);

  auto arr = root->CreateMDArray(
      "arr1", {dim}, GDALExtendedDataType::Create(GDT_Int16), nullptr);
  ASSERT_NE(nullptr, arr);

  EXPECT_EQ(1, root->GetMDArrayNames(nullptr).size());
  EXPECT_EQ("arr1", root->GetMDArrayNames(nullptr)[0]);

  // 1. DeleteMDArray: read-only error
  root->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteMDArray("arr1"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  root->SetUpdatable(true);

  // 2. DeleteMDArray: path traversal error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteMDArray("../bad"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 3. DeleteMDArray: non-existent array error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->DeleteMDArray("non_existent"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Rename array (triggers NotifyArrayRenamed)
  EXPECT_TRUE(arr->Rename("arr2"));
  auto arr_names = root->GetMDArrayNames(nullptr);
  EXPECT_EQ(1, arr_names.size());
  EXPECT_EQ("arr2", arr_names[0]);

  // 5. Delete renamed array
  EXPECT_TRUE(root->DeleteMDArray("arr2"));
  EXPECT_TRUE(root->GetMDArrayNames(nullptr).empty());
}

TEST(ZarrGroupTest, RenameGroupAndParentRenamed) {
  ScopedTestDir test_dir("zarr_grp_ren");
  const std::string root_dir = test_dir.File("root");
  auto shared_res = ZarrSharedResource::Create(root_dir, /*bUpdatable=*/true);
  auto root = ZarrV2Group::CreateOnDisk(shared_res, "", "/", root_dir);
  ASSERT_NE(nullptr, root);

  // 1. Renaming root group fails
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(root->Rename("new_root"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  auto sub = root->CreateGroup("sub", nullptr);
  ASSERT_NE(nullptr, sub);
  auto sub_zarr = std::dynamic_pointer_cast<ZarrGroupBase>(sub);
  ASSERT_NE(nullptr, sub_zarr);

  // 2. Subgroup rename: read-only error
  sub_zarr->SetUpdatable(false);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub_zarr->Rename("sub2"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
  sub_zarr->SetUpdatable(true);

  // 3. Subgroup rename: invalid name error
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub_zarr->Rename(".zbad"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 4. Subgroup rename: collision with sibling group
  auto sibling_grp = root->CreateGroup("sibling_grp", nullptr);
  ASSERT_NE(nullptr, sibling_grp);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub_zarr->Rename("sibling_grp"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 5. Subgroup rename: collision with sibling array
  auto dim = root->CreateDimension("d", "X", "EAST", 4, nullptr);
  ASSERT_NE(nullptr, dim);
  auto sibling_arr = root->CreateMDArray(
      "sibling_arr", {dim}, GDALExtendedDataType::Create(GDT_Byte), nullptr);
  ASSERT_NE(nullptr, sibling_arr);
  {
    const ::WithQuietHandler quiet;
    CPLErrorReset();
    EXPECT_FALSE(sub_zarr->Rename("sibling_arr"));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }

  // 6. Valid subgroup rename with children
  auto nested = sub->CreateGroup("nested", nullptr);
  ASSERT_NE(nullptr, nested);
  auto sub_dim = sub->CreateDimension("sub_d", "Y", "NORTH", 6, nullptr);
  ASSERT_NE(nullptr, sub_dim);
  auto sub_arr = sub_zarr->CreateMDArray(
      "sub_arr", {sub_dim}, GDALExtendedDataType::Create(GDT_Int32), nullptr);
  ASSERT_NE(nullptr, sub_arr);
  auto sub_attr = sub->CreateAttribute(
      "sub_attr", {}, GDALExtendedDataType::Create(GDT_Float64), nullptr);
  ASSERT_NE(nullptr, sub_attr);

  EXPECT_TRUE(sub_zarr->Rename("sub_renamed"));
  EXPECT_EQ("sub_renamed", sub_zarr->GetName());
  EXPECT_EQ("/sub_renamed", sub_zarr->GetFullName());
  EXPECT_EQ("/sub_renamed/nested", nested->GetFullName());
  EXPECT_EQ("/sub_renamed/sub_arr", sub_arr->GetFullName());

  // 7. Flush cache / Close on group hierarchy
  EXPECT_TRUE(std::static_pointer_cast<ZarrGroupBase>(root)->Close());
}

// TODO: b/335317901 - Cover remaining ZarrGroupBase dimension and metadata edge
// cases in zarr_group.cpp.

}  // namespace
}  // namespace autotest2
