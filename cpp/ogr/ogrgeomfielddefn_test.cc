// Copyright 2017 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/port/cpl_port.h"

#include <memory>   // NOLINT(build/include_order)
#include <utility>  // NOLINT(build/include_order)
#include <vector>   // NOLINT(build/include_order)

#include "testing/base/public/gunit.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/absl/memory/memory.h"

namespace autotest2 {
namespace {

TEST(OgrGeomFieldDefnTest, NullptrAndDefaults) {
  const auto fd = gtl::MakeUnique<OGRGeomFieldDefn>(nullptr, wkbUnknown);
  ASSERT_NE(nullptr, fd);
  EXPECT_STREQ("", fd->GetNameRef());
  EXPECT_EQ(wkbUnknown, fd->GetType());
  EXPECT_EQ(nullptr, fd->GetSpatialRef());
  EXPECT_FALSE(fd->IsIgnored());
  EXPECT_TRUE(fd->IsNullable());
}

TEST(OgrGeomFieldDefnTest, Name) {
  const auto fd = gtl::MakeUnique<OGRGeomFieldDefn>(nullptr, wkbUnknown);
  ASSERT_NE(nullptr, fd);
  fd->SetName("a\xFA");
  EXPECT_STREQ("a\xFA", fd->GetNameRef());
  fd->SetName(nullptr);
  EXPECT_STREQ("", fd->GetNameRef());
}

TEST(OgrGeomFieldDefnTest, Type) {
  const auto fd = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbUnknown);
  ASSERT_NE(nullptr, fd);
  fd->SetType(wkbTriangleZM);
  EXPECT_EQ(wkbTriangleZM, fd->GetType());
}

TEST(OgrGeomFieldDefnTest, Srs) {
  const auto fd = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbUnknown);
  ASSERT_NE(nullptr, fd);
  OGRSpatialReference srs;
  ASSERT_EQ(OGRERR_NONE, srs.importFromEPSG(4326));

  fd->SetSpatialRef(&srs);
  EXPECT_EQ(&srs, fd->GetSpatialRef());

  fd->SetSpatialRef(nullptr);
  EXPECT_EQ(nullptr, fd->GetSpatialRef());
}

TEST(OgrGeomFieldDefnTest, Ignored) {
  const auto fd = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbUnknown);
  EXPECT_FALSE(fd->IsIgnored());
  fd->SetIgnored(true);
  EXPECT_TRUE(fd->IsIgnored());
  fd->SetIgnored(false);
  EXPECT_FALSE(fd->IsIgnored());
}

TEST(OgrGeomFieldDefnTest, Nullable) {
  const auto fd = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbUnknown);
  EXPECT_TRUE(fd->IsNullable());
  fd->SetNullable(false);
  EXPECT_FALSE(fd->IsNullable());
  fd->SetNullable(true);
  EXPECT_TRUE(fd->IsNullable());
}

TEST(OgrGeomFieldDefnTest, IsSame) {
  auto fd = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbPoint);
  EXPECT_TRUE(fd->IsSame(fd.get()));
  {
    const auto other = gtl::MakeUnique<OGRGeomFieldDefn>("b", wkbPoint);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    const auto other = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbMultiPoint);
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    const auto other = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbPoint);

    OGRSpatialReference srs4326;
    ASSERT_EQ(OGRERR_NONE, srs4326.importFromEPSG(4326));
    other->SetSpatialRef(&srs4326);
    EXPECT_FALSE(fd->IsSame(other.get()));

    const auto another = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbPoint);
    another->SetSpatialRef(&srs4326);
    EXPECT_TRUE(other->IsSame(another.get()));

    OGRSpatialReference srs32611;
    ASSERT_EQ(OGRERR_NONE, srs32611.importFromEPSG(32611));
    another->SetSpatialRef(&srs32611);
    EXPECT_FALSE(other->IsSame(another.get()));
  }

  // Ignored is ignored.
  {
    const auto other = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbPoint);
    other->SetIgnored(false);
    EXPECT_TRUE(fd->IsSame(other.get()));
    other->SetIgnored(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  {
    const auto other = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbPoint);
    other->SetNullable(false);
    EXPECT_FALSE(fd->IsSame(other.get()));
    other->SetNullable(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }
}

TEST(OgrGeomFieldDefnTest, CopyConstructor) {
  auto fd = gtl::MakeUnique<OGRGeomFieldDefn>("a", wkbPoint);
  auto copy = gtl::MakeUnique<OGRGeomFieldDefn>(fd.get());
  EXPECT_TRUE(fd->IsSame(copy.get()));
}

}  // namespace
}  // namespace autotest2
