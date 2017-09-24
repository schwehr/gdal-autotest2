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

#include "autotest2/cpp/util/ogr/ogrfeaturedefn.h"

#include "gunit.h"
#include "third_party/absl/memory/memory.h"
#include "ogr/ogr_api.h"
#include "ogr/ogr_core.h"
#include "ogr/ogr_feature.h"
#include "port/cpl_port.h"
#include "util/gtl/cleanup.h"

namespace autotest2 {
namespace {

TEST(BuilderTest, SkipBuild) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("int32", OFTInteger, OFSTNone);
  // Do nothing to make sure there is nothing left on the heap.
}

TEST(BuilderTest, BuildTwiceFails) {
  OGRFeatureDefnBuilder builder("schema");
  auto fd = builder.Build();
  ASSERT_NE(nullptr, fd);
  fd->Release();

  fd = builder.Build();
  EXPECT_EQ(nullptr, fd);
}

TEST(BuilderTest, OneGeo) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddGeom("geo", wkbPoint);
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });

  EXPECT_EQ(1, fd->GetGeomFieldCount());
  EXPECT_EQ(wkbPoint, fd->GetGeomType());
  auto geom = fd->GetGeomFieldDefn(0);
  EXPECT_STREQ("geo", geom->GetNameRef());

  EXPECT_EQ(0, fd->GetFieldCount());
}

TEST(BuilderTest, Empty) {
  // Create a definition without a geometry.
  OGRFeatureDefnBuilder builder("schema");
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });

  EXPECT_EQ(0, fd->GetGeomFieldCount());
  EXPECT_EQ(0, fd->GetFieldCount());
}

TEST(BuilderTest, OneField) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("i", OFTInteger, OFSTNone);
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });

  EXPECT_EQ(0, fd->GetGeomFieldCount());

  EXPECT_EQ(1, fd->GetFieldCount());
  auto field = fd->GetFieldDefn(0);
  EXPECT_STREQ("i", field->GetNameRef());
  EXPECT_EQ(OFTInteger, field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());
}

TEST(BuilderTest, OneGeom) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddGeom("a", wkbUnknown);
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });

  EXPECT_EQ(1, fd->GetGeomFieldCount());
  auto geom = fd->GetGeomFieldDefn(0);
  EXPECT_STREQ("a", geom->GetNameRef());
}

TEST(BuilderTest, BuildFields) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("int32", OFTInteger, OFSTNone);
  builder.AddField("int64", OFTInteger64, OFSTNone);
  builder.AddField("string", OFTString, OFSTNone);
  builder.AddField("float", OFTReal, OFSTFloat32);
  builder.AddField("double", OFTReal, OFSTNone);
  builder.AddField("bytes", OFTBinary, OFSTNone);
  builder.AddField("bool", OFTInteger, OFSTBoolean);
  builder.AddGeom("geo2", wkbUnknown);
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });

  EXPECT_EQ(1, fd->GetGeomFieldCount());
  EXPECT_EQ(wkbUnknown, fd->GetGeomType());
  auto geom = fd->GetGeomFieldDefn(0);
  EXPECT_STREQ("geo2", geom->GetNameRef());

  EXPECT_STREQ("schema", fd->GetName());
  EXPECT_EQ(7, fd->GetFieldCount());
  EXPECT_EQ(1, fd->GetGeomFieldCount());
  EXPECT_EQ(wkbUnknown, fd->GetGeomType());

  auto field = fd->GetFieldDefn(0);
  EXPECT_STREQ("int32", field->GetNameRef());
  EXPECT_EQ(OFTInteger, field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(1);
  EXPECT_STREQ("int64", field->GetNameRef());
  EXPECT_EQ(OFTInteger64  , field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(2);
  EXPECT_STREQ("string", field->GetNameRef());
  EXPECT_EQ(OFTString, field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(3);
  EXPECT_STREQ("float", field->GetNameRef());
  EXPECT_EQ(OFTReal, field->GetType());
  EXPECT_EQ(OFSTFloat32, field->GetSubType());

  field = fd->GetFieldDefn(4);
  EXPECT_STREQ("double", field->GetNameRef());
  EXPECT_EQ(OFTReal, field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(5);
  EXPECT_STREQ("bytes", field->GetNameRef());
  EXPECT_EQ(OFTBinary, field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(6);
  EXPECT_STREQ("bool", field->GetNameRef());
  EXPECT_EQ(OFTInteger, field->GetType());
  EXPECT_EQ(OFSTBoolean, field->GetSubType());
}

TEST(BuilderTest, MultipleGeomFields) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddGeom("a", wkbPolygon);
  builder.AddGeom("b", wkbMultiPolygon);
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });

  EXPECT_EQ(2, fd->GetGeomFieldCount());
  EXPECT_EQ(wkbPolygon, fd->GetGeomType());

  auto a = fd->GetGeomFieldDefn(0);
  EXPECT_STREQ("a", a->GetNameRef());
  EXPECT_EQ(wkbPolygon, a->GetType());

  auto b = fd->GetGeomFieldDefn(1);
  EXPECT_STREQ("b", b->GetNameRef());
  EXPECT_EQ(wkbMultiPolygon, b->GetType());
}

TEST(BuilderTest, Lists) {
  OGRFeatureDefnBuilder builder("schema");
  builder.AddField("intlist", OFTIntegerList, OFSTNone);
  builder.AddField("doublelist", OFTRealList, OFSTNone);
  builder.AddField("stringlist", OFTStringList, OFSTNone);
  builder.AddField("int64list", OFTInteger64List, OFSTNone);
  auto fd = builder.Build();
  auto fd_cleaner = gtl::MakeCleanup([fd] { fd->Release(); });

  EXPECT_EQ(4, fd->GetFieldCount());

  auto field = fd->GetFieldDefn(0);
  EXPECT_STREQ("intlist", field->GetNameRef());
  EXPECT_EQ(OFTIntegerList, field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(1);
  EXPECT_STREQ("doublelist", field->GetNameRef());
  EXPECT_EQ(OFTRealList  , field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(2);
  EXPECT_STREQ("stringlist", field->GetNameRef());
  EXPECT_EQ(OFTStringList  , field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());

  field = fd->GetFieldDefn(3);
  EXPECT_STREQ("int64list", field->GetNameRef());
  EXPECT_EQ(OFTInteger64List, field->GetType());
  EXPECT_EQ(OFSTNone, field->GetSubType());
}

}  // namespace
}  // namespace autotest2
