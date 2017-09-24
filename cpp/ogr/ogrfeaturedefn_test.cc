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

// Test OGRFeatureDefn.
//
// unique_ptr is not the standard way that OGRFeatureDefn is used, but it is
// convenient for testing.  The Release test demonstrates the normal usage.

#include "port/cpl_port.h"
#include "ogr/ogr_feature.h"

#include <memory>  // NOLINT(build/include_order)

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "ogr/ogr_core.h"

namespace autotest2 {
namespace {

TEST(OgrfeaturedefnTest, Empty) {
  const auto fd = gtl::MakeUnique<OGRFeatureDefn>("");
  ASSERT_NE(nullptr, fd);
  // Intensionally not incrementing the reference count.

  EXPECT_STREQ("", fd->GetName());
  EXPECT_EQ(0, fd->GetFieldCount());

  // Invalid requests.
  EXPECT_EQ(nullptr, fd->GetFieldDefn(-1));
  EXPECT_EQ(nullptr, fd->GetFieldDefn(0));
  EXPECT_EQ(nullptr, fd->GetFieldDefn(1));

  // Find by name should always fail with -1.
  EXPECT_EQ(-1, fd->GetFieldIndex(nullptr));
  EXPECT_EQ(-1, fd->GetFieldIndex(""));
  EXPECT_EQ(-1, fd->GetFieldIndex("DoesNotExist"));
  EXPECT_EQ(-1, fd->GetFieldIndex("!@#$ing crazy junk[],./;'\xFF"));

  EXPECT_EQ(OGRERR_FAILURE, fd->DeleteFieldDefn(-1));
  EXPECT_EQ(OGRERR_FAILURE, fd->DeleteFieldDefn(0));
  EXPECT_EQ(OGRERR_FAILURE, fd->DeleteFieldDefn(1));

  // A nullptr is okay for an empty definition,
  // but it will explode if there are fields.
  EXPECT_EQ(OGRERR_NONE, fd->ReorderFieldDefns(nullptr));

  // Yes, the geom count starts at 1 when there is nothing.
  EXPECT_EQ(1, fd->GetGeomFieldCount());

  EXPECT_EQ(nullptr, fd->GetGeomFieldDefn(-1));
  EXPECT_NE(nullptr, fd->GetGeomFieldDefn(0));
  EXPECT_EQ(nullptr, fd->GetGeomFieldDefn(1));

  // Do not pass nullptr to GetGeomFieldIndex.
  EXPECT_EQ(0, fd->GetGeomFieldIndex(""));
  EXPECT_EQ(-1, fd->GetGeomFieldIndex("DoesNotExist"));
  EXPECT_EQ(-1, fd->GetGeomFieldIndex(")(*&\xEB"));

  EXPECT_EQ(OGRERR_FAILURE, fd->DeleteGeomFieldDefn(-1));
  EXPECT_EQ(OGRERR_NONE, fd->DeleteGeomFieldDefn(0));
  EXPECT_EQ(OGRERR_FAILURE, fd->DeleteGeomFieldDefn(1));

  EXPECT_EQ(wkbNone, fd->GetGeomType());

  EXPECT_EQ(0, fd->GetReferenceCount());
  EXPECT_FALSE(fd->IsGeometryIgnored());
  EXPECT_FALSE(fd->IsStyleIgnored());

  EXPECT_EQ(0, fd->GetFieldCount());
}

TEST(OgrfeaturedefnTest, ReferenceCount) {
  auto fd = gtl::MakeUnique<OGRFeatureDefn>("ref count");
  EXPECT_EQ(0, fd->GetReferenceCount());
  EXPECT_EQ(1, fd->Reference());
  EXPECT_EQ(2, fd->Reference());
  EXPECT_EQ(2, fd->GetReferenceCount());
  EXPECT_EQ(1, fd->Dereference());
  EXPECT_EQ(0, fd->Dereference());
  EXPECT_EQ(0, fd->GetReferenceCount());

  // What?
  EXPECT_EQ(-1, fd->Dereference());
}

TEST(OgrfeaturedefnTest, Release) {
  auto fd = new OGRFeatureDefn("release");
  EXPECT_EQ(0, fd->GetReferenceCount());
  EXPECT_EQ(1, fd->Reference());
  EXPECT_EQ(2, fd->Reference());
  fd->Release();
  // Object should still exist.
  fd->Release();
  // No need to delete as fd deleted itself.
}

TEST(OgrfeaturedefnTest, Fields) {
  auto fd = gtl::MakeUnique<OGRFeatureDefn>("a");
  EXPECT_EQ(0, fd->GetFieldCount());
  auto a = gtl::MakeUnique<OGRFieldDefn>("b", OFTInteger);
  fd->AddFieldDefn(a.get());
  auto b = gtl::MakeUnique<OGRFieldDefn>("c", OFTReal);
  fd->AddFieldDefn(b.get());
  EXPECT_EQ(0, fd->GetFieldIndex("b"));
  EXPECT_EQ(1, fd->GetFieldIndex("c"));
  EXPECT_EQ(2, fd->GetFieldCount());

  EXPECT_TRUE(b->IsSame(fd->GetFieldDefn(1)));

  int reorder[] = {1, 0};
  EXPECT_EQ(OGRERR_NONE, fd->ReorderFieldDefns(reorder));
  EXPECT_EQ(1, fd->GetFieldIndex("b"));
  EXPECT_EQ(0, fd->GetFieldIndex("c"));

  EXPECT_EQ(OGRERR_NONE, fd->DeleteFieldDefn(1));
  EXPECT_EQ(1, fd->GetFieldCount());
}

TEST(OgrfeaturedefnTest, GeomField) {
  auto fd = gtl::MakeUnique<OGRFeatureDefn>("a");

  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(1, fd->GetGeomFieldCount());

  fd->SetGeomType(wkbPoint);
  EXPECT_EQ(wkbPoint, fd->GetGeomType());
  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(1, fd->GetGeomFieldCount());

  auto b = gtl::MakeUnique<OGRGeomFieldDefn>("b", wkbLineString);
  fd->AddGeomFieldDefn(b.get());
  EXPECT_EQ(wkbPoint, fd->GetGeomType());
  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(2, fd->GetGeomFieldCount());

  auto c = gtl::MakeUnique<OGRGeomFieldDefn>("c", wkbPolygon);
  fd->AddGeomFieldDefn(c.get());
  EXPECT_EQ(wkbPoint, fd->GetGeomType());
  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(3, fd->GetGeomFieldCount());

  // Still the same.  This is the value taken from the first/initial geometry
  // field (#0). It is not altered by adding the additional "b" and "c" geometry
  // fields.
  EXPECT_EQ(wkbPoint, fd->GetGeomType());

  EXPECT_EQ(1, fd->GetGeomFieldIndex("b"));
  EXPECT_EQ(2, fd->GetGeomFieldIndex("c"));
  EXPECT_EQ(-1, fd->GetGeomFieldIndex("does not exist"));

  auto c2 = fd->GetGeomFieldDefn(2);
  ASSERT_NE(nullptr, c2);
  EXPECT_TRUE(c->IsSame(c2));

  fd->SetGeometryIgnored(true);
  EXPECT_TRUE(fd->IsGeometryIgnored());
  fd->SetGeometryIgnored(false);
  EXPECT_FALSE(fd->IsGeometryIgnored());
}

TEST(OgrfeaturedefnTest, Style) {
  auto fd = gtl::MakeUnique<OGRFeatureDefn>("a");
  EXPECT_FALSE(fd->IsStyleIgnored());
  fd->SetStyleIgnored(true);
  EXPECT_TRUE(fd->IsStyleIgnored());
  fd->SetStyleIgnored(false);
  EXPECT_FALSE(fd->IsStyleIgnored());
}

TEST(OgrfeaturedefnTest, CreateDestroy) {
  // http://stackoverflow.com/questions/443147/c-mix-new-delete-between-libs
  auto fd = OGRFeatureDefn::CreateFeatureDefn("a");
  ASSERT_NE(nullptr, fd);
  OGRFeatureDefn::DestroyFeatureDefn(fd);
}

TEST(OgrfeaturedefnTest, IsSame) {
  auto fd = gtl::MakeUnique<OGRFeatureDefn>("a");
  EXPECT_TRUE(fd->IsSame(fd.get()));
  {
    auto other = gtl::MakeUnique<OGRFeatureDefn>("a");
    EXPECT_TRUE(fd->IsSame(other.get()));
  }
  {
    auto other = gtl::MakeUnique<OGRFeatureDefn>("b");
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    auto other = gtl::MakeUnique<OGRFeatureDefn>("a");
    auto a = gtl::MakeUnique<OGRFieldDefn>("b", OFTInteger);
    other->AddFieldDefn(a.get());
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    auto other = gtl::MakeUnique<OGRFeatureDefn>("a");
    auto b = gtl::MakeUnique<OGRGeomFieldDefn>("b", wkbLineString);
    other->AddGeomFieldDefn(b.get());
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  // Style is ignored.
  {
    auto other = gtl::MakeUnique<OGRFeatureDefn>("a");
    other->SetStyleIgnored(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
    other->SetStyleIgnored(false);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }
  // Geometry ignored is ignored.
  {
    auto other = gtl::MakeUnique<OGRFeatureDefn>("a");
    other->SetGeometryIgnored(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
    other->SetGeometryIgnored(false);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  auto a = gtl::MakeUnique<OGRFieldDefn>("b", OFTInteger);
  fd->AddFieldDefn(a.get());
  auto c = gtl::MakeUnique<OGRGeomFieldDefn>("c", wkbPolygon);
  fd->AddGeomFieldDefn(c.get());
  EXPECT_TRUE(fd->IsSame(fd.get()));
}

TEST(OgrfeaturedefnTest, Clone) {
  auto fd = gtl::MakeUnique<OGRFeatureDefn>("a");
  auto a = gtl::MakeUnique<OGRFieldDefn>("b", OFTInteger);
  fd->AddFieldDefn(a.get());
  auto c = gtl::MakeUnique<OGRGeomFieldDefn>("c", wkbPolygon);
  fd->AddGeomFieldDefn(c.get());
  fd->SetStyleIgnored(true);
  fd->SetGeometryIgnored(true);

  auto clone = fd->Clone();
  ASSERT_NE(nullptr, clone);
  EXPECT_TRUE(fd->IsSame(clone));
  delete clone;
}

}  // namespace
}  // namespace autotest2
