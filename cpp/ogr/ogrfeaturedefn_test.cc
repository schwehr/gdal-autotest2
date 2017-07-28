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
  // auto fd = gtl::MakeUnique<OGRFeatureDefn>("release");
  auto fd = new OGRFeatureDefn("release");
  EXPECT_EQ(0, fd->GetReferenceCount());
  EXPECT_EQ(1, fd->Reference());
  EXPECT_EQ(2, fd->Reference());
  fd->Release();
  // Object should still exist.
  fd->Release();
  // No need to delete as fd deleted itself.
}

TEST(OgrfeaturedefnTest, SetGeomTypeOnEmpty) {
  auto fd = gtl::MakeUnique<OGRFeatureDefn>("a");

  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(1, fd->GetGeomFieldCount());

  fd->SetGeomType(wkbPoint);
  EXPECT_EQ(wkbPoint, fd->GetGeomType());
  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(1, fd->GetGeomFieldCount());
}

}  // namespace
}  // namespace autotest2
