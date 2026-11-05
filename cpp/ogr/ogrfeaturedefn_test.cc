// Tests ogrfeaturedefn.cpp.
//
// Copyright 2017 Google Inc. All Rights Reserved.
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

// Test OGRFeatureDefn.
//
// unique_ptr is not the standard way that OGRFeatureDefn is used, but it is
// convenient for testing.  The Release test demonstrates the normal usage.

#include <memory>  // NOLINT(build/include_order)
#include <string>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/gdal/autotest2/cpp/util/error_handler.h"
#include "third_party/gdal/gcore/gdal_fwd.h"
#include "third_party/gdal/ogr/ogr_api.h"
#include "third_party/gdal/ogr/ogr_core.h"
#include "third_party/gdal/ogr/ogr_feature.h"
#include "third_party/gdal/port/cpl_port.h"

namespace autotest2 {
namespace {

TEST(OgrfeaturedefnTest, Empty) {
  const auto fd = std::make_unique<OGRFeatureDefn>("");
  ASSERT_NE(nullptr, fd);
  // Intentionally not incrementing the reference count.

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
  auto fd = std::make_unique<OGRFeatureDefn>("ref count");
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
  auto fd = std::make_unique<OGRFeatureDefn>("a");
  EXPECT_EQ(0, fd->GetFieldCount());
  auto a = std::make_unique<OGRFieldDefn>("b", OFTInteger);
  fd->AddFieldDefn(a.get());
  auto b = std::make_unique<OGRFieldDefn>("c", OFTReal);
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
  auto fd = std::make_unique<OGRFeatureDefn>("a");

  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(1, fd->GetGeomFieldCount());

  fd->SetGeomType(wkbPoint);
  EXPECT_EQ(wkbPoint, fd->GetGeomType());
  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(1, fd->GetGeomFieldCount());

  auto b = std::make_unique<OGRGeomFieldDefn>("b", wkbLineString);
  fd->AddGeomFieldDefn(b.get());
  EXPECT_EQ(wkbPoint, fd->GetGeomType());
  EXPECT_EQ(0, fd->GetFieldCount());
  EXPECT_EQ(2, fd->GetGeomFieldCount());

  auto c = std::make_unique<OGRGeomFieldDefn>("c", wkbPolygon);
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
  auto fd = std::make_unique<OGRFeatureDefn>("a");
  EXPECT_FALSE(fd->IsStyleIgnored());
  fd->SetStyleIgnored(true);
  EXPECT_TRUE(fd->IsStyleIgnored());
  fd->SetStyleIgnored(false);
  EXPECT_FALSE(fd->IsStyleIgnored());
}

TEST(OgrfeaturedefnTest, CreateDestroy) {
  // https://stackoverflow.com/questions/443147/c-mix-new-delete-between-libs
  auto fd = OGRFeatureDefn::CreateFeatureDefn("a");
  ASSERT_NE(nullptr, fd);
  OGRFeatureDefn::DestroyFeatureDefn(fd);
}

TEST(OgrfeaturedefnTest, IsSame) {
  auto fd = std::make_unique<OGRFeatureDefn>("a");
  EXPECT_TRUE(fd->IsSame(fd.get()));
  {
    auto other = std::make_unique<OGRFeatureDefn>("a");
    EXPECT_TRUE(fd->IsSame(other.get()));
  }
  {
    auto other = std::make_unique<OGRFeatureDefn>("b");
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    auto other = std::make_unique<OGRFeatureDefn>("a");
    auto a = std::make_unique<OGRFieldDefn>("b", OFTInteger);
    other->AddFieldDefn(a.get());
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  {
    auto other = std::make_unique<OGRFeatureDefn>("a");
    auto b = std::make_unique<OGRGeomFieldDefn>("b", wkbLineString);
    other->AddGeomFieldDefn(b.get());
    EXPECT_FALSE(fd->IsSame(other.get()));
  }
  // Style is ignored.
  {
    auto other = std::make_unique<OGRFeatureDefn>("a");
    other->SetStyleIgnored(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
    other->SetStyleIgnored(false);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }
  // Geometry ignored is ignored.
  {
    auto other = std::make_unique<OGRFeatureDefn>("a");
    other->SetGeometryIgnored(true);
    EXPECT_TRUE(fd->IsSame(other.get()));
    other->SetGeometryIgnored(false);
    EXPECT_TRUE(fd->IsSame(other.get()));
  }

  auto a = std::make_unique<OGRFieldDefn>("b", OFTInteger);
  fd->AddFieldDefn(a.get());
  auto c = std::make_unique<OGRGeomFieldDefn>("c", wkbPolygon);
  fd->AddGeomFieldDefn(c.get());
  EXPECT_TRUE(fd->IsSame(fd.get()));
}

TEST(OgrfeaturedefnTest, Clone) {
  auto fd = std::make_unique<OGRFeatureDefn>("a");
  auto a = std::make_unique<OGRFieldDefn>("b", OFTInteger);
  fd->AddFieldDefn(a.get());
  auto c = std::make_unique<OGRGeomFieldDefn>("c", wkbPolygon);
  fd->AddGeomFieldDefn(c.get());
  fd->SetStyleIgnored(true);
  fd->SetGeometryIgnored(true);

  auto clone = fd->Clone();
  ASSERT_NE(nullptr, clone);
  EXPECT_TRUE(fd->IsSame(clone));
  delete clone;
}

TEST(OgrfeaturedefnTest, SetNameAndFieldsAndUniquePtr) {
  auto fd = std::make_unique<OGRFeatureDefn>("InitialName");
  EXPECT_STREQ("InitialName", fd->GetName());
  fd->SetName("NewLayerName");
  EXPECT_STREQ("NewLayerName", fd->GetName());

  fd->ReserveSpaceForFields(10);
  fd->AddFieldDefn(std::make_unique<OGRFieldDefn>("CaseField", OFTInteger));
  EXPECT_EQ(0, fd->GetFieldIndexCaseSensitive("CaseField"));
  EXPECT_EQ(-1, fd->GetFieldIndexCaseSensitive("casefield"));
  EXPECT_EQ(0, fd->GetFieldIndex("casefield"));

  fd->AddGeomFieldDefn(
      std::make_unique<OGRGeomFieldDefn>("GeomUnique", wkbPoint));
  EXPECT_EQ(2, fd->GetGeomFieldCount());

  // Const methods
  const OGRFeatureDefn* constFd = fd.get();
  EXPECT_NE(nullptr, constFd->GetFieldDefn(0));
  EXPECT_EQ(nullptr, constFd->GetFieldDefn(-1));
  EXPECT_EQ(nullptr, constFd->GetFieldDefn(100));
  EXPECT_NE(nullptr, constFd->GetGeomFieldDefn(0));
  EXPECT_EQ(nullptr, constFd->GetGeomFieldDefn(-1));
  EXPECT_EQ(nullptr, constFd->GetGeomFieldDefn(100));

  // Iterators: GetFields() and GetGeomFields()
  int count = 0;
  for (const auto* f : fd->GetFields()) {
    EXPECT_NE(nullptr, f);
    count++;
  }
  EXPECT_EQ(1, count);

  count = 0;
  for (const auto* f : constFd->GetFields()) {
    EXPECT_NE(nullptr, f);
    count++;
  }
  EXPECT_EQ(1, count);

  count = 0;
  for (const auto* gf : fd->GetGeomFields()) {
    EXPECT_NE(nullptr, gf);
    count++;
  }
  EXPECT_EQ(2, count);

  count = 0;
  for (const auto* gf : constFd->GetGeomFields()) {
    EXPECT_NE(nullptr, gf);
    count++;
  }
  EXPECT_EQ(2, count);
}

TEST(OgrfeaturedefnTest, StealFieldAndGeomFieldDefn) {
  auto fd = std::make_unique<OGRFeatureDefn>("steal_test");
  fd->AddFieldDefn(std::make_unique<OGRFieldDefn>("f1", OFTInteger));
  fd->AddFieldDefn(std::make_unique<OGRFieldDefn>("f2", OFTReal));

  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, fd->StealFieldDefn(-1));
    EXPECT_EQ(nullptr, fd->StealFieldDefn(10));
  }

  auto stolen1 = fd->StealFieldDefn(0);
  ASSERT_NE(nullptr, stolen1);
  EXPECT_STREQ("f1", stolen1->GetNameRef());
  EXPECT_EQ(1, fd->GetFieldCount());
  EXPECT_EQ(0, fd->GetFieldIndex("f2"));

  {
    WithQuietHandler quiet;
    EXPECT_EQ(nullptr, fd->StealGeomFieldDefn(-1));
    EXPECT_EQ(nullptr, fd->StealGeomFieldDefn(10));
  }

  EXPECT_EQ(1, fd->GetGeomFieldCount());
  auto stolenGeom = fd->StealGeomFieldDefn(0);
  ASSERT_NE(nullptr, stolenGeom);
  EXPECT_EQ(0, fd->GetGeomFieldCount());

  // SetGeomType when geom field count == 0 creates default geom field
  fd->SetGeomType(wkbPolygon);
  EXPECT_EQ(1, fd->GetGeomFieldCount());
  EXPECT_EQ(wkbPolygon, fd->GetGeomType());

  // DeleteGeomFieldDefn to 0 again
  EXPECT_EQ(OGRERR_NONE, fd->DeleteGeomFieldDefn(0));
  EXPECT_EQ(0, fd->GetGeomFieldCount());
  EXPECT_EQ(wkbNone, fd->GetGeomType());
  EXPECT_FALSE(fd->IsGeometryIgnored());
  fd->SetGeometryIgnored(1);
  EXPECT_FALSE(fd->IsGeometryIgnored());
}

TEST(OgrfeaturedefnTest, SealingAndTemporaryUnsealer) {
  auto fd = std::make_unique<OGRFeatureDefn>("sealed_test");
  fd->AddFieldDefn(std::make_unique<OGRFieldDefn>("fld1", OFTString));
  fd->AddGeomFieldDefn(std::make_unique<OGRGeomFieldDefn>("geom1", wkbPoint));

  fd->Seal(/*bSealFields=*/true);

  {
    WithQuietHandler quiet;
    fd->Seal(true);  // already sealed
    fd->SetName("NewName");
    EXPECT_STREQ("sealed_test", fd->GetName());

    fd->ReserveSpaceForFields(20);

    OGRFieldDefn fExtra("f_extra", OFTInteger);
    fd->AddFieldDefn(&fExtra);
    EXPECT_EQ(1, fd->GetFieldCount());

    EXPECT_EQ(OGRERR_FAILURE, fd->DeleteFieldDefn(0));

    int map[] = {0};
    EXPECT_EQ(OGRERR_FAILURE, fd->ReorderFieldDefns(map));

    OGRGeomFieldDefn gfExtra("g_extra", wkbPoint);
    fd->AddGeomFieldDefn(&gfExtra);
    EXPECT_EQ(2, fd->GetGeomFieldCount());

    EXPECT_EQ(OGRERR_FAILURE, fd->DeleteGeomFieldDefn(0));

    fd->SetGeomType(wkbLineString);
  }

  // TemporaryUnsealer
  {
    auto unsealer = fd->GetTemporaryUnsealer(true);
    {
      auto nested = fd->GetTemporaryUnsealer(true);
      unsealer->SetName("UnsealedName");
      EXPECT_STREQ("UnsealedName", fd->GetName());
    }
  }

  // After exiting unsealer scope, object is resealed
  EXPECT_STREQ("UnsealedName", fd->GetName());
  {
    WithQuietHandler quiet;
    fd->SetName("BlockedAgain");
    EXPECT_STREQ("UnsealedName", fd->GetName());
  }

  // Unseal
  fd->Unseal(/*bUnsealFields=*/true);
  fd->SetName("FinallyUnsealed");
  EXPECT_STREQ("FinallyUnsealed", fd->GetName());

  // Error paths on unsealed
  {
    WithQuietHandler quiet;
    fd->Unseal(true);  // already unsealed

    // TemporaryUnsealer on already unsealed object (warning and counter < 0)
    {
      auto u1 = fd->GetTemporaryUnsealer(true);
      auto u2 = fd->GetTemporaryUnsealer(true);
    }
  }
}

TEST(OgrfeaturedefnTest, ComputeMapForSetFrom) {
  auto target = std::make_unique<OGRFeatureDefn>("target");
  target->AddFieldDefn(std::make_unique<OGRFieldDefn>("exact", OFTString));
  target->AddFieldDefn(
      std::make_unique<OGRFieldDefn>("CaseInsensitive", OFTInteger));
  target->AddFieldDefn(std::make_unique<OGRFieldDefn>("dup", OFTReal));
  target->AddFieldDefn(std::make_unique<OGRFieldDefn>("dup", OFTInteger));

  auto src = std::make_unique<OGRFeatureDefn>("src");
  src->AddFieldDefn(std::make_unique<OGRFieldDefn>("exact", OFTString));
  src->AddFieldDefn(
      std::make_unique<OGRFieldDefn>("caseinsensitive", OFTInteger));
  src->AddFieldDefn(std::make_unique<OGRFieldDefn>("dup", OFTReal));
  src->AddFieldDefn(std::make_unique<OGRFieldDefn>("missing", OFTInteger));

  // Forgiving: missing field maps to -1
  auto mapForgiving =
      target->ComputeMapForSetFrom(src.get(), /*bForgiving=*/true);
  ASSERT_EQ(4, mapForgiving.size());
  EXPECT_EQ(0, mapForgiving[0]);
  EXPECT_EQ(1, mapForgiving[1]);
  EXPECT_EQ(2, mapForgiving[2]);
  EXPECT_EQ(-1, mapForgiving[3]);

  // Non-forgiving: missing field causes empty vector return
  auto mapStrict =
      target->ComputeMapForSetFrom(src.get(), /*bForgiving=*/false);
  EXPECT_TRUE(mapStrict.empty());
}

TEST(OgrfeaturedefnTest, CApi) {
  OGRFeatureDefnH hFD = OGR_FD_Create("c_api_layer");
  ASSERT_NE(nullptr, hFD);
  EXPECT_STREQ("c_api_layer", OGR_FD_GetName(hFD));

  EXPECT_EQ(0, OGR_FD_GetReferenceCount(hFD));
  EXPECT_EQ(1, OGR_FD_Reference(hFD));
  EXPECT_EQ(1, OGR_FD_GetReferenceCount(hFD));
  EXPECT_EQ(0, OGR_FD_Dereference(hFD));
  EXPECT_EQ(0, OGR_FD_GetReferenceCount(hFD));

  OGRFieldDefnH hFld = OGR_Fld_Create("c_fld", OFTString);
  OGR_FD_AddFieldDefn(hFD, hFld);
  OGR_Fld_Destroy(hFld);
  EXPECT_EQ(1, OGR_FD_GetFieldCount(hFD));
  EXPECT_NE(nullptr, OGR_FD_GetFieldDefn(hFD, 0));
  EXPECT_EQ(0, OGR_FD_GetFieldIndex(hFD, "c_fld"));

  int panMap[] = {0};
  EXPECT_EQ(OGRERR_NONE, OGR_FD_ReorderFieldDefns(hFD, panMap));

  OGRGeomFieldDefnH hGFld = OGR_GFld_Create("c_geom", wkbPoint);
  OGR_FD_AddGeomFieldDefn(hFD, hGFld);
  OGR_GFld_Destroy(hGFld);
  EXPECT_EQ(2, OGR_FD_GetGeomFieldCount(hFD));
  EXPECT_NE(nullptr, OGR_FD_GetGeomFieldDefn(hFD, 1));
  EXPECT_EQ(1, OGR_FD_GetGeomFieldIndex(hFD, "c_geom"));

  OGR_FD_SetGeomType(hFD, wkbLineString);
  EXPECT_EQ(wkbLineString, OGR_FD_GetGeomType(hFD));

  EXPECT_FALSE(OGR_FD_IsGeometryIgnored(hFD));
  OGR_FD_SetGeometryIgnored(hFD, TRUE);
  EXPECT_TRUE(OGR_FD_IsGeometryIgnored(hFD));

  EXPECT_FALSE(OGR_FD_IsStyleIgnored(hFD));
  OGR_FD_SetStyleIgnored(hFD, TRUE);
  EXPECT_TRUE(OGR_FD_IsStyleIgnored(hFD));

  OGRFeatureDefnH hFD2 = OGR_FD_Create("c_api_layer");
  EXPECT_FALSE(OGR_FD_IsSame(hFD, hFD2));
  {
    WithQuietHandler quiet;
    EXPECT_FALSE(OGR_FD_IsSame(nullptr, hFD2));
    EXPECT_FALSE(OGR_FD_IsSame(hFD, nullptr));
  }
  OGR_FD_Destroy(hFD2);

  EXPECT_EQ(OGRERR_NONE, OGR_FD_DeleteFieldDefn(hFD, 0));
  EXPECT_EQ(OGRERR_NONE, OGR_FD_DeleteGeomFieldDefn(hFD, 1));

  OGR_FD_Release(hFD);

  // Destructor with ref count > 0 hits debug log
  auto fdRef = new OGRFeatureDefn("ref_debug");
  fdRef->Reference();
  delete fdRef;
}

}  // namespace
}  // namespace autotest2
