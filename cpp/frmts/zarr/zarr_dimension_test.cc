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
// Tests for ZarrDimension (zarr_dimension.cpp).
//
// See also:
//   https://github.com/OSGeo/gdal/blob/master/frmts/zarr/zarr_dimension.cpp

#include <memory>
#include <string>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/frmts/zarr/zarr_test_util.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/frmts/zarr/zarr.h"
#include "third_party/gdal/gcore/gdal.h"
#include "third_party/gdal/gcore/gdal_frmts.h"
#include "third_party/gdal/port/cpl_error.h"

namespace autotest2 {
namespace {

class ZarrDimensionTest : public ::testing::Test {
 protected:
  void SetUp() override { GDALRegister_Zarr(); }
};

TEST_F(ZarrDimensionTest, RenameFailsWhenNotUpdatable) {
  ScopedTestDir test_dir("zarr_dim");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/false);
  auto group = ZarrV2Group::Create(shared_res, "", "/");
  auto dim = std::make_shared<ZarrDimension>(shared_res, group, "/", "dim0", "",
                                             "", 10);
  dim->SetXArrayDimension();

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(dim->Rename("new_dim"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrDimensionTest, RenameFailsWhenNotXArrayDimension) {
  ScopedTestDir test_dir("zarr_dim");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);
  auto group = ZarrV2Group::Create(shared_res, "", "/");
  group->SetUpdatable(true);
  auto dim = std::make_shared<ZarrDimension>(shared_res, group, "/", "dim0", "",
                                             "", 10);
  // dim is NOT marked as XArray dimension

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(dim->Rename("new_dim"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrDimensionTest, RenameFailsWithInvalidObjectName) {
  ScopedTestDir test_dir("zarr_dim");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);
  auto group = ZarrV2Group::Create(shared_res, "", "/");
  group->SetUpdatable(true);
  auto dim = std::make_shared<ZarrDimension>(shared_res, group, "/", "dim0", "",
                                             "", 10);
  dim->SetXArrayDimension();

  const char* const invalid_names[] = {"", ".", "..", "a/b", "a\\b"};
  const ::WithQuietHandler quiet;
  for (const char* invalid_name : invalid_names) {
    CPLErrorReset();
    EXPECT_FALSE(dim->Rename(invalid_name));
    EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
  }
}

TEST_F(ZarrDimensionTest, RenameFailsWhenParentRenameDimensionFails) {
  ScopedTestDir test_dir("zarr_dim");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);
  auto group =
      ZarrV2Group::CreateOnDisk(shared_res, "", "grp", test_dir.File("grp"));
  ASSERT_NE(nullptr, group);
  group->SetUpdatable(true);

  auto dim1 = group->CreateDimension("dim1", "", "", 10);
  ASSERT_NE(nullptr, dim1);
  auto dim2 = group->CreateDimension("dim2", "", "", 20);
  ASSERT_NE(nullptr, dim2);

  // Trying to rename dim1 to dim2 will fail in group->RenameDimension
  // because a dimension named dim2 already exists
  auto zarr_dim1 = std::dynamic_pointer_cast<ZarrDimension>(dim1);
  ASSERT_NE(nullptr, zarr_dim1);
  zarr_dim1->SetXArrayDimension();

  const ::WithQuietHandler quiet;
  CPLErrorReset();
  EXPECT_FALSE(zarr_dim1->Rename("dim2"));
  EXPECT_EQ(CE_Failure, CPLGetLastErrorType());
}

TEST_F(ZarrDimensionTest, RenameSucceedsWithParentGroup) {
  ScopedTestDir test_dir("zarr_dim");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);
  auto group =
      ZarrV2Group::CreateOnDisk(shared_res, "", "grp", test_dir.File("grp"));
  ASSERT_NE(nullptr, group);
  group->SetUpdatable(true);

  auto dim = group->CreateDimension("dim0", "", "", 10);
  ASSERT_NE(nullptr, dim);
  auto zarr_dim = std::dynamic_pointer_cast<ZarrDimension>(dim);
  ASSERT_NE(nullptr, zarr_dim);
  zarr_dim->SetXArrayDimension();
  EXPECT_FALSE(zarr_dim->IsModified());

  EXPECT_TRUE(zarr_dim->Rename("renamed_dim"));
  EXPECT_EQ("renamed_dim", zarr_dim->GetName());
  EXPECT_TRUE(zarr_dim->IsModified());
}

TEST_F(ZarrDimensionTest, RenameSucceedsWithoutParentGroup) {
  ScopedTestDir test_dir("zarr_dim");
  auto shared_res =
      ZarrSharedResource::Create(test_dir.path(), /*bUpdatable=*/true);
  auto dim = std::make_shared<ZarrDimension>(shared_res,
                                             std::weak_ptr<ZarrGroupBase>(),
                                             "/", "dim_standalone", "", "", 10);
  dim->SetXArrayDimension();

  EXPECT_TRUE(dim->Rename("renamed_standalone"));
  EXPECT_EQ("renamed_standalone", dim->GetName());
  EXPECT_TRUE(dim->IsModified());
}

}  // namespace
}  // namespace autotest2
